/*
 * z — a tiny Lisp-flavoured   language.
 *
 * Single-file tree-walking interpreter written in C99.
 *
 *   build:   make
 *   run:     ./z              # REPL
 *            ./z program.z    # run a file
 *
 * Spec: see Concept.md in this folder.
 *
 * Memory model: arena/leak. The interpreter allocates and never frees;
 * intended for short-running scripts. Plenty of headroom for typical
 *   programs and trivially safe.
 */

#if !defined(_WIN32)
#  define _POSIX_C_SOURCE 200809L
#  define _DEFAULT_SOURCE
#endif

#if defined(_WIN32) && defined(_MSC_VER)
#  define _CRT_SECURE_NO_WARNINGS 1
#  define _CRT_NONSTDC_NO_DEPRECATE 1
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <time.h>
#include <stdarg.h>
#include <setjmp.h>
#include <errno.h>
#include <stdint.h>

#if defined(_WIN32)
#  include <windows.h>
#  include <io.h>
#  include <conio.h>
#  include <sys/stat.h>
#  ifndef popen
#    define popen  _popen
#    define pclose _pclose
#  endif
#  ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#    define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#  endif
#else
#  include <unistd.h>
#  include <sys/time.h>
#  include <sys/wait.h>
#  include <sys/stat.h>
#  include <termios.h>
#  include <dirent.h>
#endif

/* MSVC's <sys/stat.h> doesn't define S_ISDIR / S_ISREG. */
#ifndef S_ISDIR
#  define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#endif
#ifndef S_ISREG
#  define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#endif

/* ============================================================
 * Portable system helpers
 * ============================================================ */

static void z_sleep_seconds(double sec) {
#if defined(_WIN32)
    if (sec < 0) sec = 0;
    Sleep((DWORD)(sec * 1000.0));
#else
    struct timespec ts;
    if (sec < 0) sec = 0;
    ts.tv_sec  = (time_t)sec;
    ts.tv_nsec = (long)((sec - (double)ts.tv_sec) * 1e9);
    nanosleep(&ts, NULL);
#endif
}

static double z_now_seconds(void) {
#if defined(_WIN32)
    /* GetSystemTimeAsFileTime gives 100ns ticks since 1601-01-01. */
    FILETIME ft;
    ULARGE_INTEGER u;
    GetSystemTimeAsFileTime(&ft);
    u.LowPart  = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    /* offset from 1601-01-01 to 1970-01-01 in 100ns ticks */
    const unsigned long long EPOCH_DIFF = 116444736000000000ULL;
    unsigned long long ticks = u.QuadPart - EPOCH_DIFF;
    return (double)ticks / 1.0e7;
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
#endif
}

static void z_localtime(time_t t, struct tm* out) {
#if defined(_WIN32)
    /* MSVC and modern MinGW have localtime_s with reversed arg order. */
    localtime_s(out, &t);
#else
    localtime_r(&t, out);
#endif
}

/* ============================================================
 * Forward declarations and core types
 * ============================================================ */

typedef struct Value Value;
typedef struct Env Env;

typedef enum {
    V_NULL,
    V_BOOL,
    V_NUM,
    V_STR,
    V_SYM,
    V_LIST,    /* code list / s-expression  */
    V_ARRAY,   /* runtime array             */
    V_OBJECT,  /* string-keyed object       */
    V_FN,      /* user-defined function     */
    V_NATIVE   /* native C builtin          */
} ValueType;

typedef Value* (*NativeFn)(int argc, Value** argv, Env* env);

typedef struct {
    Value** items;
    size_t  len;
    size_t  cap;
} VList;

typedef struct {
    char**  keys;
    Value** vals;
    size_t  len;
    size_t  cap;
} VObject;

typedef struct {
    Value* params;    /* V_LIST of V_SYM */
    Value* body;      /* V_LIST of expressions, executed as do-block */
    Env*   closure;
    char*  name;      /* may be NULL for lambdas */
} VUserFn;

struct Value {
    ValueType type;
    union {
        int      b;
        double   n;
        char*    s;
        VList    list;     /* used by V_LIST and V_ARRAY */
        VObject  obj;
        VUserFn  fn;
        NativeFn native;
    } as;
};

/* environment is a chain of frames */
struct Env {
    VObject vars;
    Env*    parent;
};

/* ============================================================
 * Error handling — setjmp / longjmp based exceptions
 * ============================================================ */

typedef struct ErrFrame {
    jmp_buf          buf;
    struct ErrFrame* prev;
    char             msg[512];
    Value*           value; /* optional payload */
} ErrFrame;

static ErrFrame* g_err_top = NULL;

/* Program-level CLI args — populated in main() and exposed via (argv). */
static int    g_prog_argc = 0;
static char** g_prog_argv = NULL;

static void z_raise(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (g_err_top) {
        strncpy(g_err_top->msg, buf, sizeof(g_err_top->msg) - 1);
        g_err_top->msg[sizeof(g_err_top->msg) - 1] = '\0';
        g_err_top->value = NULL;
        longjmp(g_err_top->buf, 1);
    } else {
        fprintf(stderr, "z: error: %s\n", buf);
        exit(1);
    }
}

/* ============================================================
 * Value constructors
 * ============================================================ */

static Value* val_new(ValueType t) {
    Value* v = (Value*)calloc(1, sizeof(Value));
    v->type = t;
    return v;
}

static Value* v_null(void) {
    static Value singleton = { V_NULL, {0} };
    return &singleton;
}
static Value* v_true(void) {
    static Value singleton;
    static int   inited = 0;
    if (!inited) { singleton.type = V_BOOL; singleton.as.b = 1; inited = 1; }
    return &singleton;
}
static Value* v_false(void) {
    static Value singleton;
    static int   inited = 0;
    if (!inited) { singleton.type = V_BOOL; singleton.as.b = 0; inited = 1; }
    return &singleton;
}
static Value* v_bool(int b) { return b ? v_true() : v_false(); }

static Value* v_num(double n) {
    Value* v = val_new(V_NUM);
    v->as.n = n;
    return v;
}

static char* str_dup(const char* s) {
    size_t n = strlen(s);
    char* r = (char*)malloc(n + 1);
    memcpy(r, s, n + 1);
    return r;
}
static char* str_dup_n(const char* s, size_t n) {
    char* r = (char*)malloc(n + 1);
    memcpy(r, s, n);
    r[n] = '\0';
    return r;
}

static Value* v_str(const char* s) {
    Value* v = val_new(V_STR);
    v->as.s = str_dup(s);
    return v;
}
static Value* v_str_take(char* s) {
    Value* v = val_new(V_STR);
    v->as.s = s;
    return v;
}
static Value* v_sym(const char* s) {
    Value* v = val_new(V_SYM);
    v->as.s = str_dup(s);
    return v;
}

static void vlist_init(VList* l) {
    l->items = NULL; l->len = 0; l->cap = 0;
}
static void vlist_push(VList* l, Value* v) {
    if (l->len + 1 > l->cap) {
        l->cap = l->cap ? l->cap * 2 : 4;
        l->items = (Value**)realloc(l->items, l->cap * sizeof(Value*));
    }
    l->items[l->len++] = v;
}
static Value* v_list(void) {
    Value* v = val_new(V_LIST);
    vlist_init(&v->as.list);
    return v;
}
static Value* v_array(void) {
    Value* v = val_new(V_ARRAY);
    vlist_init(&v->as.list);
    return v;
}

static Value* v_object(void) {
    Value* v = val_new(V_OBJECT);
    v->as.obj.keys = NULL;
    v->as.obj.vals = NULL;
    v->as.obj.len = 0;
    v->as.obj.cap = 0;
    return v;
}
static int obj_index(VObject* o, const char* key) {
    for (size_t i = 0; i < o->len; i++) {
        if (strcmp(o->keys[i], key) == 0) return (int)i;
    }
    return -1;
}
static void obj_set(VObject* o, const char* key, Value* v) {
    int i = obj_index(o, key);
    if (i >= 0) { o->vals[i] = v; return; }
    if (o->len + 1 > o->cap) {
        o->cap = o->cap ? o->cap * 2 : 4;
        o->keys = (char**)realloc(o->keys, o->cap * sizeof(char*));
        o->vals = (Value**)realloc(o->vals, o->cap * sizeof(Value*));
    }
    o->keys[o->len] = str_dup(key);
    o->vals[o->len] = v;
    o->len++;
}
static Value* obj_get(VObject* o, const char* key) {
    int i = obj_index(o, key);
    return (i >= 0) ? o->vals[i] : NULL;
}

static Value* v_native(NativeFn f) {
    Value* v = val_new(V_NATIVE);
    v->as.native = f;
    return v;
}

static Value* v_userfn(const char* name, Value* params, Value* body, Env* closure) {
    Value* v = val_new(V_FN);
    v->as.fn.name    = name ? str_dup(name) : NULL;
    v->as.fn.params  = params;
    v->as.fn.body    = body;
    v->as.fn.closure = closure;
    return v;
}

/* ============================================================
 * Truthiness, type names, equality
 * ============================================================ */

static int is_truthy(Value* v) {
    if (!v) return 0;
    switch (v->type) {
        case V_NULL: return 0;
        case V_BOOL: return v->as.b;
        case V_NUM:  return v->as.n != 0.0;
        case V_STR:  return v->as.s[0] != '\0';
        case V_ARRAY:
        case V_LIST: return v->as.list.len > 0;
        case V_OBJECT: return v->as.obj.len > 0;
        default: return 1;
    }
}

static const char* type_name(Value* v) {
    if (!v) return "null";
    switch (v->type) {
        case V_NULL:   return "null";
        case V_BOOL:   return "boolean";
        case V_NUM:    return "number";
        case V_STR:    return "string";
        case V_SYM:    return "symbol";
        case V_LIST:   return "list";
        case V_ARRAY:  return "array";
        case V_OBJECT: return "object";
        case V_FN:     return "function";
        case V_NATIVE: return "native";
    }
    return "?";
}

static int value_equals(Value* a, Value* b) {
    if (a == b) return 1;
    if (!a || !b) return 0;
    if (a->type != b->type) {
        /* compare numbers loosely if mixed null/number */
        return 0;
    }
    switch (a->type) {
        case V_NULL: return 1;
        case V_BOOL: return a->as.b == b->as.b;
        case V_NUM:  return a->as.n == b->as.n;
        case V_STR:
        case V_SYM:  return strcmp(a->as.s, b->as.s) == 0;
        case V_ARRAY:
        case V_LIST: {
            if (a->as.list.len != b->as.list.len) return 0;
            for (size_t i = 0; i < a->as.list.len; i++)
                if (!value_equals(a->as.list.items[i], b->as.list.items[i])) return 0;
            return 1;
        }
        case V_OBJECT: {
            if (a->as.obj.len != b->as.obj.len) return 0;
            for (size_t i = 0; i < a->as.obj.len; i++) {
                Value* bv = obj_get(&b->as.obj, a->as.obj.keys[i]);
                if (!bv || !value_equals(a->as.obj.vals[i], bv)) return 0;
            }
            return 1;
        }
        default: return 0;
    }
}

/* ============================================================
 * Printing — both human-readable and JSON-ish forms
 * ============================================================ */

static void print_value(FILE* out, Value* v, int as_repr);

static void print_string_escaped(FILE* out, const char* s) {
    fputc('"', out);
    for (const char* p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        switch (c) {
            case '"':  fputs("\\\"", out); break;
            case '\\': fputs("\\\\", out); break;
            case '\n': fputs("\\n", out); break;
            case '\t': fputs("\\t", out); break;
            case '\r': fputs("\\r", out); break;
            default:
                if (c < 0x20) fprintf(out, "\\u%04x", c);
                else fputc(c, out);
        }
    }
    fputc('"', out);
}

static void print_value(FILE* out, Value* v, int as_repr) {
    if (!v) { fputs("null", out); return; }
    switch (v->type) {
        case V_NULL: fputs("null", out); break;
        case V_BOOL: fputs(v->as.b ? "true" : "false", out); break;
        case V_NUM: {
            double n = v->as.n;
            if (n == (long long)n && n > -1e15 && n < 1e15) {
                fprintf(out, "%lld", (long long)n);
            } else {
                fprintf(out, "%g", n);
            }
            break;
        }
        case V_STR:
            if (as_repr) print_string_escaped(out, v->as.s);
            else fputs(v->as.s, out);
            break;
        case V_SYM:
            fputs(v->as.s, out);
            break;
        case V_LIST: {
            fputc('(', out);
            for (size_t i = 0; i < v->as.list.len; i++) {
                if (i) fputc(' ', out);
                print_value(out, v->as.list.items[i], 1);
            }
            fputc(')', out);
            break;
        }
        case V_ARRAY: {
            fputc('[', out);
            for (size_t i = 0; i < v->as.list.len; i++) {
                if (i) fputs(", ", out);
                print_value(out, v->as.list.items[i], 1);
            }
            fputc(']', out);
            break;
        }
        case V_OBJECT: {
            fputc('{', out);
            for (size_t i = 0; i < v->as.obj.len; i++) {
                if (i) fputs(", ", out);
                print_string_escaped(out, v->as.obj.keys[i]);
                fputs(": ", out);
                print_value(out, v->as.obj.vals[i], 1);
            }
            fputc('}', out);
            break;
        }
        case V_FN: {
            fprintf(out, "<fn %s>", v->as.fn.name ? v->as.fn.name : "anonymous");
            break;
        }
        case V_NATIVE: fputs("<native>", out); break;
    }
}

/* Portable growing string buffer (used by JSON stringify; works on all OSes). */
typedef struct {
    char*  data;
    size_t len;
    size_t cap;
} StrBuf;

static void sb_init(StrBuf* sb) { sb->data = NULL; sb->len = 0; sb->cap = 0; }
static void sb_reserve(StrBuf* sb, size_t extra) {
    if (sb->len + extra + 1 > sb->cap) {
        size_t nc = sb->cap ? sb->cap * 2 : 64;
        while (nc < sb->len + extra + 1) nc *= 2;
        sb->data = (char*)realloc(sb->data, nc);
        sb->cap = nc;
    }
}
static void sb_putc(StrBuf* sb, char c) {
    sb_reserve(sb, 1);
    sb->data[sb->len++] = c;
    sb->data[sb->len] = '\0';
}
static void sb_puts(StrBuf* sb, const char* s) {
    size_t n = strlen(s);
    sb_reserve(sb, n);
    memcpy(sb->data + sb->len, s, n);
    sb->len += n;
    sb->data[sb->len] = '\0';
}
static void sb_printf(StrBuf* sb, const char* fmt, ...) {
    char tmp[128];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n < sizeof(tmp)) { sb_puts(sb, tmp); return; }
    /* Need bigger buffer. */
    char* big = (char*)malloc((size_t)n + 1);
    va_start(ap, fmt);
    vsnprintf(big, (size_t)n + 1, fmt, ap);
    va_end(ap);
    sb_puts(sb, big);
    free(big);
}

static void sb_escape_string(StrBuf* sb, const char* s) {
    sb_putc(sb, '"');
    for (const char* p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        switch (c) {
            case '"':  sb_puts(sb, "\\\""); break;
            case '\\': sb_puts(sb, "\\\\"); break;
            case '\n': sb_puts(sb, "\\n");  break;
            case '\t': sb_puts(sb, "\\t");  break;
            case '\r': sb_puts(sb, "\\r");  break;
            default:
                if (c < 0x20) sb_printf(sb, "\\u%04x", c);
                else sb_putc(sb, (char)c);
        }
    }
    sb_putc(sb, '"');
}

/* JSON output — only valid for JSON-safe values. Writes into a StrBuf. */
static void json_encode(StrBuf* sb, Value* v) {
    if (!v) { sb_puts(sb, "null"); return; }
    switch (v->type) {
        case V_NULL: sb_puts(sb, "null"); break;
        case V_BOOL: sb_puts(sb, v->as.b ? "true" : "false"); break;
        case V_NUM: {
            double n = v->as.n;
            if (n == (long long)n && n > -1e15 && n < 1e15)
                sb_printf(sb, "%lld", (long long)n);
            else
                sb_printf(sb, "%g", n);
            break;
        }
        case V_STR: sb_escape_string(sb, v->as.s); break;
        case V_ARRAY:
        case V_LIST: {
            sb_putc(sb, '[');
            for (size_t i = 0; i < v->as.list.len; i++) {
                if (i) sb_putc(sb, ',');
                json_encode(sb, v->as.list.items[i]);
            }
            sb_putc(sb, ']');
            break;
        }
        case V_OBJECT: {
            sb_putc(sb, '{');
            for (size_t i = 0; i < v->as.obj.len; i++) {
                if (i) sb_putc(sb, ',');
                sb_escape_string(sb, v->as.obj.keys[i]);
                sb_putc(sb, ':');
                json_encode(sb, v->as.obj.vals[i]);
            }
            sb_putc(sb, '}');
            break;
        }
        default:
            z_raise("cannot JSON-encode %s", type_name(v));
    }
}

/* ============================================================
 * Lexer
 * ============================================================ */

typedef enum {
    TK_EOF,
    TK_LPAREN,
    TK_RPAREN,
    TK_LBRACK,
    TK_RBRACK,
    TK_NUM,
    TK_STR,
    TK_SYM
} TokKind;

typedef struct {
    TokKind kind;
    char*   text;   /* for STR/SYM (NUM stored as text too for simplicity) */
    double  num;
    int     line;
} Token;

typedef struct {
    const char* src;
    size_t      pos;
    size_t      len;
    int         line;
    Token*      tokens;
    size_t      tcount;
    size_t      tcap;
} Lexer;

static void tok_push(Lexer* L, Token t) {
    if (L->tcount + 1 > L->tcap) {
        L->tcap = L->tcap ? L->tcap * 2 : 64;
        L->tokens = (Token*)realloc(L->tokens, L->tcap * sizeof(Token));
    }
    L->tokens[L->tcount++] = t;
}

static int is_sym_start(int c) {
    if (c <= 0) return 0;
    if (isalpha(c)) return 1;
    /* common symbol punctuation, plus operator chars */
    return strchr("+-*/%<>=!&|?_.:", c) != NULL;
}
static int is_sym_cont(int c) {
    if (c <= 0) return 0;
    if (isalnum(c)) return 1;
    return strchr("+-*/%<>=!&|?_.:", c) != NULL;
}

static void lex_error(Lexer* L, const char* msg) {
    z_raise("lex error at line %d: %s", L->line, msg);
}

static void tokenize(Lexer* L) {
    while (L->pos < L->len) {
        char c = L->src[L->pos];
        if (c == '\n') { L->line++; L->pos++; continue; }
        if (isspace((unsigned char)c)) { L->pos++; continue; }
        if (c == ';') {
            /* line comment */
            while (L->pos < L->len && L->src[L->pos] != '\n') L->pos++;
            continue;
        }
        if (c == '(') { Token t = {TK_LPAREN, NULL, 0, L->line}; tok_push(L, t); L->pos++; continue; }
        if (c == ')') { Token t = {TK_RPAREN, NULL, 0, L->line}; tok_push(L, t); L->pos++; continue; }
        if (c == '[') { Token t = {TK_LBRACK, NULL, 0, L->line}; tok_push(L, t); L->pos++; continue; }
        if (c == ']') { Token t = {TK_RBRACK, NULL, 0, L->line}; tok_push(L, t); L->pos++; continue; }
        if (c == '"') {
            /* string */
            L->pos++;
            size_t start = L->pos;
            /* compute escaped length */
            char buf[8192]; size_t bi = 0;
            while (L->pos < L->len && L->src[L->pos] != '"') {
                char ch = L->src[L->pos];
                if (ch == '\\' && L->pos + 1 < L->len) {
                    char esc = L->src[L->pos + 1];
                    switch (esc) {
                        case 'n': ch = '\n'; break;
                        case 't': ch = '\t'; break;
                        case 'r': ch = '\r'; break;
                        case '"': ch = '"';  break;
                        case '\\': ch = '\\'; break;
                        case '0': ch = '\0'; break;
                        default:
                            /* Preserve unknown escapes (\d, \$, etc.) so they
                             * stay intact for regex strings and template-string
                             * escaping. The parser strips the backslash from
                             * \$ when building the AST. */
                            if (bi + 1 >= sizeof(buf)) lex_error(L, "string too long");
                            buf[bi++] = '\\';
                            ch = esc;
                            break;
                    }
                    L->pos += 2;
                } else {
                    if (ch == '\n') L->line++;
                    L->pos++;
                }
                if (bi + 1 >= sizeof(buf)) lex_error(L, "string too long");
                buf[bi++] = ch;
            }
            if (L->pos >= L->len) lex_error(L, "unterminated string");
            L->pos++; /* skip closing quote */
            buf[bi] = '\0';
            Token t = {TK_STR, str_dup_n(buf, bi), 0, L->line};
            tok_push(L, t);
            (void)start;
            continue;
        }
        /* number? leading digit, or -/+ tightly followed by a digit (no whitespace).
           A '-' followed by space stays a symbol, so `(- 5)` still parses as subtract. */
        if (isdigit((unsigned char)c) ||
            ((c == '-' || c == '+') && L->pos + 1 < L->len && isdigit((unsigned char)L->src[L->pos + 1]))) {
            size_t s = L->pos;
            if (c == '+' || c == '-') L->pos++;
            while (L->pos < L->len && isdigit((unsigned char)L->src[L->pos])) L->pos++;
            if (L->pos < L->len && L->src[L->pos] == '.') {
                L->pos++;
                while (L->pos < L->len && isdigit((unsigned char)L->src[L->pos])) L->pos++;
            }
            if (L->pos < L->len && (L->src[L->pos] == 'e' || L->src[L->pos] == 'E')) {
                L->pos++;
                if (L->pos < L->len && (L->src[L->pos] == '+' || L->src[L->pos] == '-')) L->pos++;
                while (L->pos < L->len && isdigit((unsigned char)L->src[L->pos])) L->pos++;
            }
            char buf[64];
            size_t n = L->pos - s;
            if (n >= sizeof(buf)) lex_error(L, "number too long");
            memcpy(buf, L->src + s, n);
            buf[n] = '\0';
            Token t = {TK_NUM, NULL, strtod(buf, NULL), L->line};
            tok_push(L, t);
            continue;
        }
        if (is_sym_start((unsigned char)c)) {
            size_t s = L->pos;
            while (L->pos < L->len && is_sym_cont((unsigned char)L->src[L->pos])) L->pos++;
            size_t n = L->pos - s;
            Token t = {TK_SYM, str_dup_n(L->src + s, n), 0, L->line};
            tok_push(L, t);
            continue;
        }
        {
            char m[64];
            snprintf(m, sizeof(m), "unexpected character '%c' (0x%02x)", c, (unsigned char)c);
            lex_error(L, m);
        }
    }
    Token t = {TK_EOF, NULL, 0, L->line};
    tok_push(L, t);
}

/* ============================================================
 * Parser — produces Values directly (V_LIST for s-expressions)
 * ============================================================ */

typedef struct {
    Token* toks;
    size_t pos;
} Parser;

static Value* parse_expr(Parser* p);
static Value* parse_all(const char* src);  /* forward decl for template strings */

/* Turn a string literal into either a plain V_STR or, if it contains
 * unescaped ${...} interpolations, a (concat <part> <expr> <part>...) call.
 * \$ is the escape for a literal $.  Other \X sequences (e.g. regex \d)
 * are preserved verbatim with the backslash intact. */
static Value* parse_string_or_template(const char* s) {
    /* Decide first whether this is a template at all. */
    int has_template = 0;
    for (const char* p = s; *p; ) {
        if (*p == '\\' && p[1]) { p += 2; continue; }
        if (*p == '$' && p[1] == '{') { has_template = 1; break; }
        p++;
    }

    if (!has_template) {
        /* Plain string — but still process \$ → $ so users can write literal
         * "$" without enabling interpolation. Everything else passes through. */
        size_t n = strlen(s);
        char* buf = (char*)malloc(n + 1);
        size_t bi = 0;
        for (const char* p = s; *p; ) {
            if (*p == '\\' && p[1] == '$') { buf[bi++] = '$'; p += 2; }
            else buf[bi++] = *p++;
        }
        buf[bi] = 0;
        return v_str_take(buf);
    }

    /* Build (concat <literal> <expr> <literal> ...). */
    Value* call = v_list();
    vlist_push(&call->as.list, v_sym("concat"));

    size_t cap = strlen(s) + 1;
    char* lit = (char*)malloc(cap);
    size_t li = 0;

    for (const char* p = s; *p; ) {
        /* \$ → literal $.  Other backslash escapes pass through unchanged. */
        if (*p == '\\' && p[1] == '$') {
            lit[li++] = '$';
            p += 2;
            continue;
        }
        if (*p == '$' && p[1] == '{') {
            if (li > 0) { lit[li] = 0; vlist_push(&call->as.list, v_str(lit)); li = 0; }
            p += 2;
            const char* expr_start = p;
            int depth = 1;
            int in_str = 0;
            while (*p && depth > 0) {
                if (in_str) {
                    if (*p == '\\' && p[1]) { p += 2; continue; }
                    if (*p == '"') in_str = 0;
                    p++;
                } else if (*p == '"') { in_str = 1; p++; }
                else if (*p == '{')   { depth++; p++; }
                else if (*p == '}')   { depth--; if (depth) p++; }
                else p++;
            }
            if (*p != '}') {
                free(lit);
                z_raise("template string: unterminated ${...}");
            }
            /* Recursively parse the embedded expression. parse_all wraps in
             * (do <expr>) — we unwrap that if it's exactly one form. */
            char* expr_src = str_dup_n(expr_start, p - expr_start);
            Value* prog = parse_all(expr_src);
            free(expr_src);
            if (prog && prog->type == V_LIST && prog->as.list.len == 2) {
                vlist_push(&call->as.list, prog->as.list.items[1]);
            } else if (prog) {
                vlist_push(&call->as.list, prog);
            }
            p++; /* skip closing } */
            continue;
        }
        lit[li++] = *p++;
    }
    if (li > 0) { lit[li] = 0; vlist_push(&call->as.list, v_str(lit)); }
    free(lit);
    return call;
}

static Value* parse_list(Parser* p, TokKind close) {
    Value* node;
    if (close == TK_RBRACK) {
        /* [ ... ] → treat as a call list too; (array ...) form */
        node = v_list();
    } else {
        node = v_list();
    }
    /* For bracket form [a b c] we treat it as (a b c). Spec uses [array 1 2 3]
       which yields (array 1 2 3) — works out. */
    while (p->toks[p->pos].kind != close && p->toks[p->pos].kind != TK_EOF) {
        vlist_push(&node->as.list, parse_expr(p));
    }
    if (p->toks[p->pos].kind != close) z_raise("parse error: unexpected EOF, missing closing %s",
                                                close == TK_RPAREN ? ")" : "]");
    p->pos++;
    return node;
}

static Value* parse_expr(Parser* p) {
    Token t = p->toks[p->pos];
    switch (t.kind) {
        case TK_LPAREN: p->pos++; return parse_list(p, TK_RPAREN);
        case TK_LBRACK: p->pos++; return parse_list(p, TK_RBRACK);
        case TK_NUM:    p->pos++; return v_num(t.num);
        case TK_STR:    p->pos++; return parse_string_or_template(t.text);
        case TK_SYM: {
            p->pos++;
            if (strcmp(t.text, "true") == 0)  return v_true();
            if (strcmp(t.text, "false") == 0) return v_false();
            if (strcmp(t.text, "null") == 0)  return v_null();
            return v_sym(t.text);
        }
        case TK_RPAREN: z_raise("parse error: unexpected ')' at line %d", t.line); return NULL;
        case TK_RBRACK: z_raise("parse error: unexpected ']' at line %d", t.line); return NULL;
        case TK_EOF:    z_raise("parse error: unexpected EOF"); return NULL;
    }
    return NULL;
}

/* Parse entire source into a list of top-level expressions (a synthetic do-list) */
static Value* parse_all(const char* src) {
    Lexer L = { src, 0, strlen(src), 1, NULL, 0, 0 };
    tokenize(&L);
    Parser p = { L.tokens, 0 };
    Value* program = v_list();
    /* Wrap as (do ...) */
    vlist_push(&program->as.list, v_sym("do"));
    while (p.toks[p.pos].kind != TK_EOF) {
        vlist_push(&program->as.list, parse_expr(&p));
    }
    return program;
}

/* ============================================================
 * Environment
 * ============================================================ */

static Env* env_new(Env* parent) {
    Env* e = (Env*)calloc(1, sizeof(Env));
    e->parent = parent;
    e->vars.keys = NULL;
    e->vars.vals = NULL;
    e->vars.len = 0;
    e->vars.cap = 0;
    return e;
}
static void env_define(Env* e, const char* name, Value* v) {
    obj_set(&e->vars, name, v);
}
static Value* env_lookup(Env* e, const char* name) {
    for (Env* c = e; c; c = c->parent) {
        Value* v = obj_get(&c->vars, name);
        if (v) return v;
    }
    return NULL;
}
static int env_assign(Env* e, const char* name, Value* v) {
    for (Env* c = e; c; c = c->parent) {
        if (obj_index(&c->vars, name) >= 0) {
            obj_set(&c->vars, name, v);
            return 1;
        }
    }
    /* Not found anywhere → define in current scope */
    obj_set(&e->vars, name, v);
    return 1;
}

/* ============================================================
 * Evaluator
 * ============================================================ */

static Value* eval(Value* expr, Env* env);
static Value* apply(Value* callee, int argc, Value** argv, Env* env);

/* Helpers used by special forms */
static const char* sym_name(Value* v) {
    if (!v || v->type != V_SYM) z_raise("expected symbol");
    return v->as.s;
}

static Value* eval_do(VList* exprs, size_t start, Env* env) {
    Value* last = v_null();
    for (size_t i = start; i < exprs->len; i++) {
        last = eval(exprs->items[i], env);
    }
    return last;
}

static Value* eval_if(VList* args, Env* env) {
    if (args->len < 2 || args->len > 3)
        z_raise("if: expected (if cond then [else])");
    Value* cond = eval(args->items[0], env);
    if (is_truthy(cond)) return eval(args->items[1], env);
    if (args->len == 3) return eval(args->items[2], env);
    return v_null();
}

static Value* eval_while(VList* args, Env* env) {
    if (args->len < 2) z_raise("while: expected (while cond body...)");
    Value* last = v_null();
    while (1) {
        Value* c = eval(args->items[0], env);
        if (!is_truthy(c)) break;
        for (size_t i = 1; i < args->len; i++) last = eval(args->items[i], env);
    }
    return last;
}

static Value* eval_for(VList* args, Env* env) {
    if (args->len < 3) z_raise("for: expected (for var coll body...)");
    const char* var = sym_name(args->items[0]);
    Value* coll = eval(args->items[1], env);
    Value* last = v_null();
    if (coll->type == V_ARRAY || coll->type == V_LIST) {
        for (size_t i = 0; i < coll->as.list.len; i++) {
            env_define(env, var, coll->as.list.items[i]);
            for (size_t j = 2; j < args->len; j++) last = eval(args->items[j], env);
        }
    } else if (coll->type == V_OBJECT) {
        for (size_t i = 0; i < coll->as.obj.len; i++) {
            env_define(env, var, v_str(coll->as.obj.keys[i]));
            for (size_t j = 2; j < args->len; j++) last = eval(args->items[j], env);
        }
    } else {
        z_raise("for: cannot iterate over %s", type_name(coll));
    }
    return last;
}

static Value* eval_fn(VList* args, Env* env, int anonymous) {
    /* (fn name (params...) body...) or (lambda (params...) body...) */
    size_t i = 0;
    const char* name = NULL;
    if (!anonymous) {
        if (args->len < 3) z_raise("fn: expected (fn name (params) body...)");
        name = sym_name(args->items[0]);
        i = 1;
    } else {
        if (args->len < 2) z_raise("lambda: expected (lambda (params) body...)");
    }
    Value* params = args->items[i++];
    if (params->type != V_LIST) z_raise("%s: param list must be a list", anonymous ? "lambda" : "fn");
    /* Build a synthetic do-body */
    Value* body = v_list();
    vlist_push(&body->as.list, v_sym("do"));
    for (; i < args->len; i++) vlist_push(&body->as.list, args->items[i]);

    Value* f = v_userfn(name, params, body, env);
    if (!anonymous) env_define(env, name, f);
    return f;
}

static Value* eval_set(VList* args, Env* env) {
    if (args->len != 2) z_raise("set: expected (set name value)");
    Value* target = args->items[0];
    if (target->type != V_SYM) z_raise("set: first argument must be a symbol");
    Value* val = eval(args->items[1], env);
    /* Support dotted path: foo.bar */
    if (strchr(target->as.s, '.')) {
        char* path = str_dup(target->as.s);
        char* dot = strchr(path, '.');
        *dot = '\0';
        const char* root = path;
        const char* field = dot + 1;
        Value* obj = env_lookup(env, root);
        if (!obj || obj->type != V_OBJECT) z_raise("set: %s is not an object", root);
        obj_set(&obj->as.obj, field, val);
        free(path);
        return val;
    }
    env_assign(env, target->as.s, val);
    return val;
}

static Value* eval_try(VList* args, Env* env) {
    /* (try body (catch errname handler-body...)) */
    if (args->len < 2) z_raise("try: expected (try body (catch err handler))");
    Value* tryExpr   = args->items[0];
    Value* catchForm = args->items[1];
    if (catchForm->type != V_LIST || catchForm->as.list.len < 2
        || catchForm->as.list.items[0]->type != V_SYM
        || strcmp(catchForm->as.list.items[0]->as.s, "catch") != 0) {
        z_raise("try: second form must be (catch name body...)");
    }
    const char* errname = sym_name(catchForm->as.list.items[1]);

    ErrFrame frame;
    frame.prev   = g_err_top;
    frame.value  = NULL;
    frame.msg[0] = '\0';
    g_err_top    = &frame;

    if (setjmp(frame.buf) == 0) {
        Value* result = eval(tryExpr, env);
        g_err_top = frame.prev;
        return result;
    } else {
        g_err_top = frame.prev;
        Env* sub = env_new(env);
        env_define(sub, errname, v_str(frame.msg));
        Value* last = v_null();
        for (size_t i = 2; i < catchForm->as.list.len; i++)
            last = eval(catchForm->as.list.items[i], sub);
        return last;
    }
}

static Value* eval(Value* expr, Env* env) {
    if (!expr) return v_null();
    switch (expr->type) {
        case V_NULL:
        case V_BOOL:
        case V_NUM:
        case V_STR:
        case V_ARRAY:
        case V_OBJECT:
        case V_FN:
        case V_NATIVE:
            return expr;
        case V_SYM: {
            const char* name = expr->as.s;
            /* Support dotted access: obj.field */
            const char* dot = strchr(name, '.');
            if (dot && dot != name) {
                char* root = str_dup_n(name, dot - name);
                Value* base = env_lookup(env, root);
                free(root);
                if (!base) z_raise("undefined variable '%s'", name);
                const char* rest = dot + 1;
                while (rest) {
                    const char* next = strchr(rest, '.');
                    char* part = next ? str_dup_n(rest, next - rest) : str_dup(rest);
                    if (base->type == V_OBJECT) {
                        Value* nv = obj_get(&base->as.obj, part);
                        if (!nv) { free(part); return v_null(); }
                        base = nv;
                    } else {
                        free(part);
                        z_raise("cannot access field of %s", type_name(base));
                    }
                    free(part);
                    rest = next ? next + 1 : NULL;
                }
                return base;
            }
            Value* v = env_lookup(env, name);
            if (!v) z_raise("undefined variable '%s'", name);
            return v;
        }
        case V_LIST: {
            VList* l = &expr->as.list;
            if (l->len == 0) return v_null();
            Value* head = l->items[0];
            /* Special forms (head must be a literal symbol) */
            if (head->type == V_SYM) {
                const char* op = head->as.s;
                VList rest = { l->items + 1, l->len - 1, 0 };
                if (strcmp(op, "do") == 0)     return eval_do(&rest, 0, env);
                if (strcmp(op, "if") == 0)     return eval_if(&rest, env);
                if (strcmp(op, "while") == 0)  return eval_while(&rest, env);
                if (strcmp(op, "for") == 0)    return eval_for(&rest, env);
                if (strcmp(op, "fn") == 0)     return eval_fn(&rest, env, 0);
                if (strcmp(op, "lambda") == 0) return eval_fn(&rest, env, 1);
                if (strcmp(op, "set") == 0)    return eval_set(&rest, env);
                if (strcmp(op, "try") == 0)    return eval_try(&rest, env);
                if (strcmp(op, "quote") == 0) {
                    if (rest.len != 1) z_raise("quote: expected one argument");
                    return rest.items[0];
                }
                if (strcmp(op, "and") == 0 || strcmp(op, "&&") == 0) {
                    Value* last = v_true();
                    for (size_t i = 0; i < rest.len; i++) {
                        last = eval(rest.items[i], env);
                        if (!is_truthy(last)) return v_false();
                    }
                    return rest.len ? last : v_true();
                }
                if (strcmp(op, "or") == 0 || strcmp(op, "||") == 0) {
                    Value* last = v_false();
                    for (size_t i = 0; i < rest.len; i++) {
                        last = eval(rest.items[i], env);
                        if (is_truthy(last)) return last;
                    }
                    return last;
                }
            }
            /* Regular call: evaluate head and args */
            Value* callee = eval(head, env);
            Value** argv = (Value**)malloc(sizeof(Value*) * (l->len - 1 + 1));
            int argc = 0;
            for (size_t i = 1; i < l->len; i++) {
                argv[argc++] = eval(l->items[i], env);
            }
            Value* r = apply(callee, argc, argv, env);
            free(argv);
            return r;
        }
        default:
            return expr;
    }
}

static Value* apply(Value* callee, int argc, Value** argv, Env* env) {
    if (!callee) z_raise("cannot call null");
    if (callee->type == V_NATIVE) {
        return callee->as.native(argc, argv, env);
    }
    if (callee->type == V_FN) {
        Env* call_env = env_new(callee->as.fn.closure);
        VList* params = &callee->as.fn.params->as.list;
        if ((size_t)argc != params->len) {
            z_raise("function %s expected %zu args, got %d",
                    callee->as.fn.name ? callee->as.fn.name : "<anon>",
                    params->len, argc);
        }
        for (size_t i = 0; i < params->len; i++) {
            env_define(call_env, sym_name(params->items[i]), argv[i]);
        }
        return eval(callee->as.fn.body, call_env);
    }
    z_raise("cannot call value of type %s", type_name(callee));
    return v_null();
}

/* ============================================================
 * Built-ins (the standard library)
 * ============================================================ */

#define ARG_AT(i) argv[i]
#define EXPECT_ARGC(name, n) do { if (argc != (n)) z_raise("%s: expected %d args, got %d", name, n, argc); } while(0)
#define EXPECT_MIN(name, n)  do { if (argc < (n))  z_raise("%s: expected at least %d args, got %d", name, n, argc); } while(0)
#define AS_NUM(v, name) ((v)->type == V_NUM ? (v)->as.n : (z_raise("%s: expected number, got %s", name, type_name(v)), 0.0))

static double num_arg(Value* v, const char* fn) {
    if (v->type != V_NUM) z_raise("%s: expected number, got %s", fn, type_name(v));
    return v->as.n;
}
static const char* str_arg(Value* v, const char* fn) {
    if (v->type != V_STR) z_raise("%s: expected string, got %s", fn, type_name(v));
    return v->as.s;
}

/* ---------- arithmetic ---------- */
static Value* b_add(int argc, Value** argv, Env* e) {
    (void)e;
    if (argc == 0) return v_num(0);
    /* string concatenation if first is string */
    if (argv[0]->type == V_STR) {
        size_t total = 0;
        for (int i = 0; i < argc; i++) total += strlen(str_arg(argv[i], "+"));
        char* out = (char*)malloc(total + 1);
        out[0] = '\0';
        for (int i = 0; i < argc; i++) strcat(out, argv[i]->as.s);
        return v_str_take(out);
    }
    double s = 0;
    for (int i = 0; i < argc; i++) s += num_arg(argv[i], "+");
    return v_num(s);
}
static Value* b_sub(int argc, Value** argv, Env* e) {
    (void)e;
    EXPECT_MIN("-", 1);
    if (argc == 1) return v_num(-num_arg(argv[0], "-"));
    double s = num_arg(argv[0], "-");
    for (int i = 1; i < argc; i++) s -= num_arg(argv[i], "-");
    return v_num(s);
}
static Value* b_mul(int argc, Value** argv, Env* e) {
    (void)e;
    double s = 1;
    for (int i = 0; i < argc; i++) s *= num_arg(argv[i], "*");
    return v_num(s);
}
static Value* b_div(int argc, Value** argv, Env* e) {
    (void)e;
    EXPECT_MIN("/", 1);
    if (argc == 1) return v_num(1.0 / num_arg(argv[0], "/"));
    double s = num_arg(argv[0], "/");
    for (int i = 1; i < argc; i++) {
        double d = num_arg(argv[i], "/");
        if (d == 0) z_raise("/: division by zero");
        s /= d;
    }
    return v_num(s);
}
static Value* b_mod(int argc, Value** argv, Env* e) {
    (void)e;
    EXPECT_ARGC("%", 2);
    double a = num_arg(argv[0], "%"), b = num_arg(argv[1], "%");
    if (b == 0) z_raise("%%: division by zero");
    return v_num(fmod(a, b));
}

/* ---------- comparison ---------- */
static int cmp_values(Value* a, Value* b) {
    if (a->type == V_NUM && b->type == V_NUM) {
        if (a->as.n < b->as.n) return -1;
        if (a->as.n > b->as.n) return 1;
        return 0;
    }
    if (a->type == V_STR && b->type == V_STR) return strcmp(a->as.s, b->as.s);
    z_raise("cannot compare %s and %s", type_name(a), type_name(b));
    return 0;
}
static Value* b_lt(int argc, Value** v, Env* e){(void)e;EXPECT_ARGC("<",2);return v_bool(cmp_values(v[0],v[1])<0);}
static Value* b_gt(int argc, Value** v, Env* e){(void)e;EXPECT_ARGC(">",2);return v_bool(cmp_values(v[0],v[1])>0);}
static Value* b_le(int argc, Value** v, Env* e){(void)e;EXPECT_ARGC("<=",2);return v_bool(cmp_values(v[0],v[1])<=0);}
static Value* b_ge(int argc, Value** v, Env* e){(void)e;EXPECT_ARGC(">=",2);return v_bool(cmp_values(v[0],v[1])>=0);}
static Value* b_eq(int argc, Value** v, Env* e){(void)e;EXPECT_ARGC("==",2);return v_bool(value_equals(v[0],v[1]));}
static Value* b_ne(int argc, Value** v, Env* e){(void)e;EXPECT_ARGC("!=",2);return v_bool(!value_equals(v[0],v[1]));}
static Value* b_not(int argc, Value** v, Env* e){(void)e;EXPECT_ARGC("!",1);return v_bool(!is_truthy(v[0]));}

/* ---------- get / put / array / object ---------- */
static Value* b_array(int argc, Value** argv, Env* e) {
    (void)e;
    Value* a = v_array();
    for (int i = 0; i < argc; i++) vlist_push(&a->as.list, argv[i]);
    return a;
}
static Value* b_object(int argc, Value** argv, Env* e) {
    (void)e;
    if (argc % 2 != 0) z_raise("object: expected even number of args (key value pairs)");
    Value* o = v_object();
    for (int i = 0; i < argc; i += 2) {
        if (argv[i]->type != V_STR) z_raise("object: key must be string");
        obj_set(&o->as.obj, argv[i]->as.s, argv[i+1]);
    }
    return o;
}
static Value* b_get(int argc, Value** argv, Env* e) {
    (void)e;
    EXPECT_MIN("get", 1);
    if (argc == 1) return argv[0];  /* (get x) — just returns x (x already evaluated) */
    Value* container = argv[0];
    for (int i = 1; i < argc; i++) {
        Value* key = argv[i];
        if (container->type == V_ARRAY || container->type == V_LIST) {
            if (key->type != V_NUM) z_raise("get: array index must be number");
            long idx = (long)key->as.n;
            if (idx < 0) idx += (long)container->as.list.len;
            if (idx < 0 || (size_t)idx >= container->as.list.len) return v_null();
            container = container->as.list.items[idx];
        } else if (container->type == V_OBJECT) {
            if (key->type != V_STR) z_raise("get: object key must be string");
            Value* nv = obj_get(&container->as.obj, key->as.s);
            if (!nv) return v_null();
            container = nv;
        } else {
            z_raise("get: cannot index %s", type_name(container));
        }
    }
    return container;
}
static Value* b_put(int argc, Value** argv, Env* e) {
    (void)e;
    EXPECT_ARGC("put", 3);
    Value* container = argv[0];
    Value* key       = argv[1];
    Value* val       = argv[2];
    if (container->type == V_OBJECT) {
        if (key->type != V_STR) z_raise("put: object key must be string");
        obj_set(&container->as.obj, key->as.s, val);
    } else if (container->type == V_ARRAY) {
        if (key->type != V_NUM) z_raise("put: array index must be number");
        long idx = (long)key->as.n;
        if (idx < 0 || (size_t)idx >= container->as.list.len)
            z_raise("put: index out of bounds");
        container->as.list.items[idx] = val;
    } else {
        z_raise("put: cannot index %s", type_name(container));
    }
    return val;
}
static Value* b_push(int argc, Value** argv, Env* e) {
    (void)e;
    EXPECT_ARGC("push", 2);
    if (argv[0]->type != V_ARRAY) z_raise("push: expected array");
    vlist_push(&argv[0]->as.list, argv[1]);
    return argv[0];
}
static Value* b_pop(int argc, Value** argv, Env* e) {
    (void)e;
    EXPECT_ARGC("pop", 1);
    if (argv[0]->type != V_ARRAY) z_raise("pop: expected array");
    if (argv[0]->as.list.len == 0) return v_null();
    Value* last = argv[0]->as.list.items[--argv[0]->as.list.len];
    return last;
}
static Value* b_length(int argc, Value** argv, Env* e) {
    (void)e;
    EXPECT_ARGC("length", 1);
    Value* v = argv[0];
    switch (v->type) {
        case V_STR:    return v_num((double)strlen(v->as.s));
        case V_ARRAY:
        case V_LIST:   return v_num((double)v->as.list.len);
        case V_OBJECT: return v_num((double)v->as.obj.len);
        case V_NULL:   return v_num(0);
        default: z_raise("length: unsupported type %s", type_name(v));
    }
    return v_num(0);
}

static Value* b_keys(int argc, Value** argv, Env* e) {
    (void)e;
    EXPECT_ARGC("keys", 1);
    if (argv[0]->type != V_OBJECT) z_raise("keys: expected object");
    Value* a = v_array();
    for (size_t i = 0; i < argv[0]->as.obj.len; i++)
        vlist_push(&a->as.list, v_str(argv[0]->as.obj.keys[i]));
    return a;
}
static Value* b_values(int argc, Value** argv, Env* e) {
    (void)e;
    EXPECT_ARGC("values", 1);
    if (argv[0]->type != V_OBJECT) z_raise("values: expected object");
    Value* a = v_array();
    for (size_t i = 0; i < argv[0]->as.obj.len; i++)
        vlist_push(&a->as.list, argv[0]->as.obj.vals[i]);
    return a;
}
static Value* b_entries(int argc, Value** argv, Env* e) {
    (void)e;
    EXPECT_ARGC("entries", 1);
    if (argv[0]->type != V_OBJECT) z_raise("entries: expected object");
    Value* a = v_array();
    for (size_t i = 0; i < argv[0]->as.obj.len; i++) {
        Value* pair = v_array();
        vlist_push(&pair->as.list, v_str(argv[0]->as.obj.keys[i]));
        vlist_push(&pair->as.list, argv[0]->as.obj.vals[i]);
        vlist_push(&a->as.list, pair);
    }
    return a;
}

/* ---------- higher-order ---------- */
static Value* b_map(int argc, Value** argv, Env* e) {
    EXPECT_ARGC("map", 2);
    Value* fn = argv[0];
    Value* coll = argv[1];
    if (coll->type != V_ARRAY && coll->type != V_LIST) z_raise("map: expected array");
    Value* out = v_array();
    for (size_t i = 0; i < coll->as.list.len; i++) {
        Value* a[1] = { coll->as.list.items[i] };
        vlist_push(&out->as.list, apply(fn, 1, a, e));
    }
    return out;
}
static Value* b_filter(int argc, Value** argv, Env* e) {
    EXPECT_ARGC("filter", 2);
    Value* fn = argv[0];
    Value* coll = argv[1];
    if (coll->type != V_ARRAY && coll->type != V_LIST) z_raise("filter: expected array");
    Value* out = v_array();
    for (size_t i = 0; i < coll->as.list.len; i++) {
        Value* a[1] = { coll->as.list.items[i] };
        Value* keep = apply(fn, 1, a, e);
        if (is_truthy(keep)) vlist_push(&out->as.list, coll->as.list.items[i]);
    }
    return out;
}
static Value* b_reduce(int argc, Value** argv, Env* e) {
    if (argc != 2 && argc != 3) z_raise("reduce: expected (reduce fn coll [init])");
    Value* fn = argv[0];
    Value* coll = argv[1];
    if (coll->type != V_ARRAY && coll->type != V_LIST) z_raise("reduce: expected array");
    size_t i = 0;
    Value* acc;
    if (argc == 3) acc = argv[2];
    else {
        if (coll->as.list.len == 0) z_raise("reduce: empty collection without initial value");
        acc = coll->as.list.items[0];
        i = 1;
    }
    for (; i < coll->as.list.len; i++) {
        Value* a[2] = { acc, coll->as.list.items[i] };
        acc = apply(fn, 2, a, e);
    }
    return acc;
}

/* Render any z value as a plain-text string. Used by concat (and therefore by
 * template-string interpolation). Compound types come out in JSON form so
 * embedded arrays/objects render meaningfully instead of as "?". */
static char* value_to_cstr(Value* v) {
    StrBuf sb; sb_init(&sb);
    if (!v) { sb_puts(&sb, "null"); return sb.data; }
    char tmp[64];
    switch (v->type) {
        case V_NULL: sb_puts(&sb, "null"); break;
        case V_BOOL: sb_puts(&sb, v->as.b ? "true" : "false"); break;
        case V_NUM: {
            double n = v->as.n;
            if (n == (long long)n && n > -1e15 && n < 1e15)
                snprintf(tmp, sizeof(tmp), "%lld", (long long)n);
            else
                snprintf(tmp, sizeof(tmp), "%g", n);
            sb_puts(&sb, tmp);
            break;
        }
        case V_STR: sb_puts(&sb, v->as.s); break;
        case V_SYM: sb_puts(&sb, v->as.s); break;
        case V_ARRAY:
        case V_LIST:
        case V_OBJECT:
            json_encode(&sb, v);
            break;
        case V_FN:
            sb_puts(&sb, v->as.fn.name ? v->as.fn.name : "<fn>");
            break;
        case V_NATIVE:
            sb_puts(&sb, "<native>");
            break;
    }
    if (!sb.data) { sb.data = (char*)calloc(1, 1); sb.len = 0; }
    return sb.data;
}

/* ---------- string ---------- */
static Value* b_concat(int argc, Value** argv, Env* e) {
    (void)e;
    StrBuf sb; sb_init(&sb);
    for (int i = 0; i < argc; i++) {
        char* part = value_to_cstr(argv[i]);
        sb_puts(&sb, part);
        free(part);
    }
    if (!sb.data) { sb.data = (char*)calloc(1, 1); sb.len = 0; }
    return v_str_take(sb.data);
}
static Value* b_split(int argc, Value** argv, Env* e) {
    (void)e;
    EXPECT_ARGC("split", 2);
    const char* sep = str_arg(argv[0], "split");
    const char* s   = str_arg(argv[1], "split");
    Value* out = v_array();
    size_t sep_len = strlen(sep);
    if (sep_len == 0) {
        for (const char* p = s; *p; p++) {
            char c[2] = { *p, 0 };
            vlist_push(&out->as.list, v_str(c));
        }
        return out;
    }
    const char* start = s;
    const char* hit;
    while ((hit = strstr(start, sep))) {
        vlist_push(&out->as.list, v_str_take(str_dup_n(start, hit - start)));
        start = hit + sep_len;
    }
    vlist_push(&out->as.list, v_str(start));
    return out;
}
static Value* b_trim(int argc, Value** argv, Env* e) {
    (void)e;
    EXPECT_ARGC("trim", 1);
    const char* s = str_arg(argv[0], "trim");
    const char* a = s;
    while (*a && isspace((unsigned char)*a)) a++;
    const char* b = s + strlen(s);
    while (b > a && isspace((unsigned char)*(b - 1))) b--;
    return v_str_take(str_dup_n(a, b - a));
}
static Value* b_lower(int argc, Value** argv, Env* e) {
    (void)e;
    EXPECT_ARGC("lower", 1);
    const char* s = str_arg(argv[0], "lower");
    char* out = str_dup(s);
    for (char* p = out; *p; p++) *p = (char)tolower((unsigned char)*p);
    return v_str_take(out);
}
static Value* b_upper(int argc, Value** argv, Env* e) {
    (void)e;
    EXPECT_ARGC("upper", 1);
    const char* s = str_arg(argv[0], "upper");
    char* out = str_dup(s);
    for (char* p = out; *p; p++) *p = (char)toupper((unsigned char)*p);
    return v_str_take(out);
}
static Value* b_replace(int argc, Value** argv, Env* e) {
    (void)e;
    EXPECT_ARGC("replace", 3);
    const char* hay = str_arg(argv[0], "replace");
    const char* need = str_arg(argv[1], "replace");
    const char* rep = str_arg(argv[2], "replace");
    size_t need_len = strlen(need), rep_len = strlen(rep);
    if (need_len == 0) return v_str(hay);
    /* count occurrences */
    size_t count = 0;
    for (const char* p = hay; (p = strstr(p, need)); p += need_len) count++;
    size_t hay_len = strlen(hay);
    size_t out_len = hay_len + count * (rep_len > need_len ? rep_len - need_len : 0);
    /* Allocate worst case */
    char* out = (char*)malloc(hay_len + count * rep_len + 1);
    char* o = out;
    const char* p = hay;
    while (1) {
        const char* hit = strstr(p, need);
        if (!hit) { strcpy(o, p); break; }
        memcpy(o, p, hit - p); o += (hit - p);
        memcpy(o, rep, rep_len); o += rep_len;
        p = hit + need_len;
    }
    (void)out_len;
    return v_str_take(out);
}
static Value* b_starts_with(int argc, Value** argv, Env* e) {
    (void)e;
    EXPECT_ARGC("starts_with", 2);
    const char* s = str_arg(argv[0], "starts_with");
    const char* p = str_arg(argv[1], "starts_with");
    size_t sl = strlen(s), pl = strlen(p);
    if (pl > sl) return v_false();
    return v_bool(memcmp(s, p, pl) == 0);
}

static Value* b_ends_with(int argc, Value** argv, Env* e) {
    (void)e;
    EXPECT_ARGC("ends_with", 2);
    const char* s = str_arg(argv[0], "ends_with");
    const char* p = str_arg(argv[1], "ends_with");
    size_t sl = strlen(s), pl = strlen(p);
    if (pl > sl) return v_false();
    return v_bool(memcmp(s + sl - pl, p, pl) == 0);
}

/* (contains container needle)
 *   string  → does it contain the substring?
 *   array   → does any element equal needle?
 *   object  → does it have that key? (needle must be a string)
 */
static Value* b_contains(int argc, Value** argv, Env* e) {
    (void)e;
    EXPECT_ARGC("contains", 2);
    Value* container = argv[0];
    Value* needle    = argv[1];
    if (container->type == V_STR) {
        if (needle->type != V_STR)
            z_raise("contains: needle must be a string when container is a string");
        return v_bool(strstr(container->as.s, needle->as.s) != NULL);
    }
    if (container->type == V_ARRAY || container->type == V_LIST) {
        for (size_t i = 0; i < container->as.list.len; i++)
            if (value_equals(container->as.list.items[i], needle)) return v_true();
        return v_false();
    }
    if (container->type == V_OBJECT) {
        if (needle->type != V_STR)
            z_raise("contains: object key must be a string");
        return v_bool(obj_index(&container->as.obj, needle->as.s) >= 0);
    }
    z_raise("contains: unsupported type %s", type_name(container));
    return v_false();
}

/* (index_of container needle)
 *   string  → byte index of the first match, or -1
 *   array   → index of the first equal element, or -1
 */
static Value* b_index_of(int argc, Value** argv, Env* e) {
    (void)e;
    EXPECT_ARGC("index_of", 2);
    Value* container = argv[0];
    Value* needle    = argv[1];
    if (container->type == V_STR) {
        if (needle->type != V_STR)
            z_raise("index_of: needle must be a string when container is a string");
        const char* hit = strstr(container->as.s, needle->as.s);
        return v_num(hit ? (double)(hit - container->as.s) : -1.0);
    }
    if (container->type == V_ARRAY || container->type == V_LIST) {
        for (size_t i = 0; i < container->as.list.len; i++)
            if (value_equals(container->as.list.items[i], needle))
                return v_num((double)i);
        return v_num(-1);
    }
    z_raise("index_of: unsupported type %s", type_name(container));
    return v_num(-1);
}

static Value* b_substring(int argc, Value** argv, Env* e) {
    (void)e;
    if (argc != 2 && argc != 3) z_raise("substring: expected (substring s start [end])");
    const char* s = str_arg(argv[0], "substring");
    long len = (long)strlen(s);
    long start = (long)num_arg(argv[1], "substring");
    long end   = argc == 3 ? (long)num_arg(argv[2], "substring") : len;
    if (start < 0) start = 0;
    if (end > len) end = len;
    if (end < start) end = start;
    return v_str_take(str_dup_n(s + start, end - start));
}

/* ============================================================
 * Mini regex engine — supports:
 *   . * + ? ^ $   [class] [^class]   \d \w \s \D \W \S
 *   (no groups, no alternation, no back-references)
 * Backtracking implementation, ~200 lines.
 * ============================================================ */

static int rx_match_class(const char* cls, int cls_len, char c) {
    int i = 0, neg = 0;
    if (cls_len > 0 && cls[0] == '^') { neg = 1; i = 1; }
    int matched = 0;
    while (i < cls_len) {
        if (cls[i] == '\\' && i + 1 < cls_len) {
            char e = cls[i + 1];
            if      (e == 'd' &&  isdigit((unsigned char)c)) matched = 1;
            else if (e == 'D' && !isdigit((unsigned char)c)) matched = 1;
            else if (e == 'w' && (isalnum((unsigned char)c) || c == '_')) matched = 1;
            else if (e == 'W' && !(isalnum((unsigned char)c) || c == '_')) matched = 1;
            else if (e == 's' &&  isspace((unsigned char)c)) matched = 1;
            else if (e == 'S' && !isspace((unsigned char)c)) matched = 1;
            else if (e == 'n' && c == '\n') matched = 1;
            else if (e == 't' && c == '\t') matched = 1;
            else if (e == c) matched = 1;
            i += 2;
        } else if (i + 2 < cls_len && cls[i + 1] == '-' && cls[i + 2] != ']') {
            if ((unsigned char)c >= (unsigned char)cls[i]
             && (unsigned char)c <= (unsigned char)cls[i + 2]) matched = 1;
            i += 3;
        } else {
            if (c == cls[i]) matched = 1;
            i++;
        }
    }
    return neg ? !matched : matched;
}

/* Length of a single regex atom (one char, escaped, or [class]). */
static int rx_atom_len(const char* re) {
    if (*re == '\\' && re[1]) return 2;
    if (*re == '[') {
        const char* end = strchr(re + 1, ']');
        if (!end) return 1;  /* malformed */
        return (int)(end - re + 1);
    }
    return 1;
}

static int rx_match_atom(const char* re, char c) {
    if (*re == '\\' && re[1]) {
        char e = re[1];
        if (e == 'd') return isdigit((unsigned char)c) ? 1 : 0;
        if (e == 'D') return !isdigit((unsigned char)c) ? 1 : 0;
        if (e == 'w') return (isalnum((unsigned char)c) || c == '_') ? 1 : 0;
        if (e == 'W') return !(isalnum((unsigned char)c) || c == '_') ? 1 : 0;
        if (e == 's') return isspace((unsigned char)c) ? 1 : 0;
        if (e == 'S') return !isspace((unsigned char)c) ? 1 : 0;
        if (e == 'n') return c == '\n';
        if (e == 't') return c == '\t';
        return c == e;
    }
    if (*re == '.') return c != 0 && c != '\n';
    if (*re == '[') {
        const char* end = strchr(re + 1, ']');
        if (!end) return 0;
        return rx_match_class(re + 1, (int)(end - re - 1), c);
    }
    return c == *re;
}

static int rx_match_here(const char* re, const char* text, const char** out_end);

static int rx_match_star(const char* atom, int atom_len, const char* rest,
                         const char* text, const char** out_end) {
    const char* t = text;
    while (*t && rx_match_atom(atom, *t)) t++;
    /* Greedy match, then backtrack down to zero. */
    while (t >= text) {
        if (rx_match_here(rest, t, out_end)) return 1;
        if (t == text) break;
        t--;
    }
    return 0;
}

static int rx_match_plus(const char* atom, int atom_len, const char* rest,
                         const char* text, const char** out_end) {
    if (!*text || !rx_match_atom(atom, *text)) return 0;
    const char* t = text + 1;
    while (*t && rx_match_atom(atom, *t)) t++;
    while (t > text) {
        if (rx_match_here(rest, t, out_end)) return 1;
        t--;
    }
    return 0;
}

static int rx_match_question(const char* atom, int atom_len, const char* rest,
                             const char* text, const char** out_end) {
    if (*text && rx_match_atom(atom, *text)
        && rx_match_here(rest, text + 1, out_end)) return 1;
    return rx_match_here(rest, text, out_end);
}

static int rx_match_here(const char* re, const char* text, const char** out_end) {
    while (*re) {
        if (*re == '$' && re[1] == 0) {
            if (*text == 0) { if (out_end) *out_end = text; return 1; }
            return 0;
        }
        int al = rx_atom_len(re);
        char next = re[al];
        if (next == '*') return rx_match_star(re, al, re + al + 1, text, out_end);
        if (next == '+') return rx_match_plus(re, al, re + al + 1, text, out_end);
        if (next == '?') return rx_match_question(re, al, re + al + 1, text, out_end);

        if (!*text) return 0;
        if (!rx_match_atom(re, *text)) return 0;
        re   += al;
        text += 1;
    }
    if (out_end) *out_end = text;
    return 1;
}

/* Find the first match in `text`. Returns 1 if found and fills out_start/out_end
 * with byte offsets into `text`. */
static int rx_search(const char* re, const char* text, int* out_start, int* out_end) {
    if (*re == '^') {
        const char* end_p = NULL;
        if (rx_match_here(re + 1, text, &end_p)) {
            if (out_start) *out_start = 0;
            if (out_end)   *out_end = (int)(end_p - text);
            return 1;
        }
        return 0;
    }
    const char* p = text;
    while (1) {
        const char* end_p = NULL;
        if (rx_match_here(re, p, &end_p)) {
            if (out_start) *out_start = (int)(p - text);
            if (out_end)   *out_end = (int)(end_p - text);
            return 1;
        }
        if (!*p) return 0;
        p++;
    }
}

/* ---------- regex builtins ---------- */

static Value* b_rx_test(int argc, Value** argv, Env* e) {
    (void)e; EXPECT_ARGC("regex:test", 2);
    const char* re = str_arg(argv[0], "regex:test");
    const char* s  = str_arg(argv[1], "regex:test");
    return v_bool(rx_search(re, s, NULL, NULL));
}

static Value* b_rx_match(int argc, Value** argv, Env* e) {
    (void)e; EXPECT_ARGC("regex:match", 2);
    const char* re = str_arg(argv[0], "regex:match");
    const char* s  = str_arg(argv[1], "regex:match");
    int a, b;
    if (!rx_search(re, s, &a, &b)) return v_null();
    return v_str_take(str_dup_n(s + a, b - a));
}

static Value* b_rx_find_all(int argc, Value** argv, Env* e) {
    (void)e; EXPECT_ARGC("regex:find-all", 2);
    const char* re = str_arg(argv[0], "regex:find-all");
    const char* s  = str_arg(argv[1], "regex:find-all");
    Value* out = v_array();
    const char* p = s;
    while (*p) {
        int a, b;
        if (!rx_search(re, p, &a, &b)) break;
        if (b == a) {  /* zero-length match → advance one char to avoid loop */
            if (!p[a]) break;
            vlist_push(&out->as.list, v_str(""));
            p += a + 1;
        } else {
            vlist_push(&out->as.list, v_str_take(str_dup_n(p + a, b - a)));
            p += b;
        }
    }
    return out;
}

static Value* b_rx_replace(int argc, Value** argv, Env* e) {
    (void)e; EXPECT_ARGC("regex:replace", 3);
    const char* re  = str_arg(argv[0], "regex:replace");
    const char* s   = str_arg(argv[1], "regex:replace");
    const char* rep = str_arg(argv[2], "regex:replace");
    StrBuf sb; sb_init(&sb);
    const char* p = s;
    while (*p) {
        int a, b;
        if (!rx_search(re, p, &a, &b)) break;
        for (int i = 0; i < a; i++) sb_putc(&sb, p[i]);
        sb_puts(&sb, rep);
        if (b == a) {
            if (p[a]) sb_putc(&sb, p[a]);
            p += a + 1;
            if (!*p) { sb_puts(&sb, ""); }
        } else {
            p += b;
        }
    }
    sb_puts(&sb, p);
    if (!sb.data) sb_putc(&sb, '\0');
    return v_str_take(sb.data);
}

static Value* b_rx_split(int argc, Value** argv, Env* e) {
    (void)e; EXPECT_ARGC("regex:split", 2);
    const char* re = str_arg(argv[0], "regex:split");
    const char* s  = str_arg(argv[1], "regex:split");
    Value* out = v_array();
    const char* p = s;
    while (*p) {
        int a, b;
        if (!rx_search(re, p, &a, &b)) break;
        if (b == a) { p++; continue; }
        vlist_push(&out->as.list, v_str_take(str_dup_n(p, a)));
        p += b;
    }
    vlist_push(&out->as.list, v_str(p));
    return out;
}

/* ============================================================
 * Base64, lightweight encryption, and UUIDs
 * ============================================================ */

static const char B64_ALPHABET[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static char* base64_encode_bytes(const unsigned char* data, size_t len) {
    size_t olen = 4 * ((len + 2) / 3);
    char* out = (char*)malloc(olen + 1);
    size_t i, j;
    for (i = 0, j = 0; i < len; ) {
        unsigned a = i < len ? data[i++] : 0;
        unsigned b = i < len ? data[i++] : 0;
        unsigned c = i < len ? data[i++] : 0;
        unsigned triple = (a << 16) | (b << 8) | c;
        out[j++] = B64_ALPHABET[(triple >> 18) & 0x3F];
        out[j++] = B64_ALPHABET[(triple >> 12) & 0x3F];
        out[j++] = B64_ALPHABET[(triple >> 6)  & 0x3F];
        out[j++] = B64_ALPHABET[ triple        & 0x3F];
    }
    int mod = (int)(len % 3);
    if (mod == 1) { out[olen-1] = '='; out[olen-2] = '='; }
    else if (mod == 2) { out[olen-1] = '='; }
    out[olen] = 0;
    return out;
}

static int b64_value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

/* Decodes base64, ignoring whitespace and stopping at padding. Returns a
 * malloc'd byte buffer; *out_len gets the decoded length. */
static unsigned char* base64_decode_str(const char* in, size_t* out_len) {
    size_t len = strlen(in);
    unsigned char* out = (unsigned char*)malloc(len / 4 * 3 + 4);
    size_t o = 0;
    int quad[4], qn = 0;
    for (size_t i = 0; i < len; i++) {
        char c = in[i];
        if (c == '=') break;
        if (isspace((unsigned char)c)) continue;
        int v = b64_value(c);
        if (v < 0) continue;            /* skip stray characters */
        quad[qn++] = v;
        if (qn == 4) {
            out[o++] = (unsigned char)((quad[0] << 2) | (quad[1] >> 4));
            out[o++] = (unsigned char)(((quad[1] & 0xF) << 4) | (quad[2] >> 2));
            out[o++] = (unsigned char)(((quad[2] & 0x3) << 6) | quad[3]);
            qn = 0;
        }
    }
    if (qn == 2) {
        out[o++] = (unsigned char)((quad[0] << 2) | (quad[1] >> 4));
    } else if (qn == 3) {
        out[o++] = (unsigned char)((quad[0] << 2) | (quad[1] >> 4));
        out[o++] = (unsigned char)(((quad[1] & 0xF) << 4) | (quad[2] >> 2));
    }
    out[o] = 0;
    if (out_len) *out_len = o;
    return out;
}

static Value* b_base64_encode(int argc, Value** argv, Env* e) {
    (void)e; EXPECT_ARGC("base64:encode", 1);
    const char* s = str_arg(argv[0], "base64:encode");
    return v_str_take(base64_encode_bytes((const unsigned char*)s, strlen(s)));
}
static Value* b_base64_decode(int argc, Value** argv, Env* e) {
    (void)e; EXPECT_ARGC("base64:decode", 1);
    const char* s = str_arg(argv[0], "base64:decode");
    size_t n = 0;
    unsigned char* out = base64_decode_str(s, &n);
    return v_str_take((char*)out);
}

/* --- XTEA block cipher (64-bit block, 128-bit key) --- */
static void xtea_encrypt_block(const uint32_t key[4], uint32_t v[2]) {
    uint32_t v0 = v[0], v1 = v[1], sum = 0;
    const uint32_t delta = 0x9E3779B9u;
    for (int i = 0; i < 32; i++) {
        v0 += (((v1 << 4) ^ (v1 >> 5)) + v1) ^ (sum + key[sum & 3]);
        sum += delta;
        v1 += (((v0 << 4) ^ (v0 >> 5)) + v0) ^ (sum + key[(sum >> 11) & 3]);
    }
    v[0] = v0; v[1] = v1;
}

/* Derive a 128-bit key from an arbitrary string (FNV-1a based; not a strong
 * KDF — this is lightweight obfuscation, not high-security crypto). */
static void crypt_derive_key(const char* keystr, uint32_t key[4]) {
    uint32_t seeds[4] = { 0x811c9dc5u, 0x01000193u, 0xdeadbeefu, 0xcafebabeu };
    for (int k = 0; k < 4; k++) {
        uint32_t h = seeds[k];
        for (const char* p = keystr; *p; p++) {
            h ^= (unsigned char)*p;
            h *= 16777619u;
            h ^= (uint32_t)(k + 1);
        }
        key[k] = h;
    }
}

/* XTEA in CTR mode — symmetric, no padding, endianness-independent. */
static void crypt_ctr(const uint32_t key[4], const unsigned char nonce[8],
                      const unsigned char* in, size_t len, unsigned char* out) {
    uint64_t base = 0;
    for (int i = 0; i < 8; i++) base |= (uint64_t)nonce[i] << (8 * i);
    size_t blocks = (len + 7) / 8;
    for (size_t b = 0; b < blocks; b++) {
        uint64_t counter = base + b;
        unsigned char cb[8];
        for (int i = 0; i < 8; i++) cb[i] = (unsigned char)(counter >> (8 * i));
        uint32_t v[2];
        v[0] = (uint32_t)cb[0] | ((uint32_t)cb[1]<<8) | ((uint32_t)cb[2]<<16) | ((uint32_t)cb[3]<<24);
        v[1] = (uint32_t)cb[4] | ((uint32_t)cb[5]<<8) | ((uint32_t)cb[6]<<16) | ((uint32_t)cb[7]<<24);
        xtea_encrypt_block(key, v);
        unsigned char ks[8];
        ks[0]=(unsigned char)v[0];      ks[1]=(unsigned char)(v[0]>>8);
        ks[2]=(unsigned char)(v[0]>>16);ks[3]=(unsigned char)(v[0]>>24);
        ks[4]=(unsigned char)v[1];      ks[5]=(unsigned char)(v[1]>>8);
        ks[6]=(unsigned char)(v[1]>>16);ks[7]=(unsigned char)(v[1]>>24);
        size_t off = b * 8;
        for (size_t j = 0; j < 8 && off + j < len; j++)
            out[off + j] = in[off + j] ^ ks[j];
    }
}

/* (encrypt key text) → base64( nonce[8] || ciphertext ). Text only (no NUL). */
static Value* b_encrypt(int argc, Value** argv, Env* e) {
    (void)e; EXPECT_ARGC("encrypt", 2);
    const char* keystr = str_arg(argv[0], "encrypt");
    const char* text   = str_arg(argv[1], "encrypt");
    size_t len = strlen(text);

    uint32_t key[4];
    crypt_derive_key(keystr, key);

    unsigned char nonce[8];
    for (int i = 0; i < 8; i++) nonce[i] = (unsigned char)(rand() & 0xFF);

    unsigned char* buf = (unsigned char*)malloc(8 + len);
    memcpy(buf, nonce, 8);
    crypt_ctr(key, nonce, (const unsigned char*)text, len, buf + 8);

    char* b64 = base64_encode_bytes(buf, 8 + len);
    free(buf);
    return v_str_take(b64);
}

/* (decrypt key cipher) → original text. */
static Value* b_decrypt(int argc, Value** argv, Env* e) {
    (void)e; EXPECT_ARGC("decrypt", 2);
    const char* keystr = str_arg(argv[0], "decrypt");
    const char* b64    = str_arg(argv[1], "decrypt");

    size_t total = 0;
    unsigned char* buf = base64_decode_str(b64, &total);
    if (total < 8) { free(buf); z_raise("decrypt: ciphertext too short / malformed"); }

    uint32_t key[4];
    crypt_derive_key(keystr, key);

    size_t len = total - 8;
    unsigned char* out = (unsigned char*)malloc(len + 1);
    crypt_ctr(key, buf, buf + 8, len, out);
    out[len] = 0;
    free(buf);
    return v_str_take((char*)out);
}

/* (uuid) → random RFC-4122 version-4 UUID string. */
static Value* b_uuid(int argc, Value** argv, Env* e) {
    (void)e; (void)argc; (void)argv;
    unsigned char b[16];
    for (int i = 0; i < 16; i++) b[i] = (unsigned char)(rand() & 0xFF);
    b[6] = (unsigned char)((b[6] & 0x0F) | 0x40);   /* version 4 */
    b[8] = (unsigned char)((b[8] & 0x3F) | 0x80);   /* variant 10xx */
    char out[37];
    snprintf(out, sizeof(out),
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        b[0],b[1],b[2],b[3], b[4],b[5], b[6],b[7], b[8],b[9],
        b[10],b[11],b[12],b[13],b[14],b[15]);
    return v_str(out);
}

/* ============================================================
 * URL handling (percent-encoding, query-string builder)
 * ============================================================ */

static int url_is_safe(char c) {
    return isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '~';
}

static void url_percent_encode(StrBuf* sb, const char* s) {
    char hex[8];
    for (const char* p = s; *p; p++) {
        if (url_is_safe(*p)) sb_putc(sb, *p);
        else {
            snprintf(hex, sizeof(hex), "%%%02X", (unsigned char)*p);
            sb_puts(sb, hex);
        }
    }
}

static int url_hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static Value* b_url_encode(int argc, Value** argv, Env* e) {
    (void)e; EXPECT_ARGC("url:encode", 1);
    const char* s = str_arg(argv[0], "url:encode");
    StrBuf sb; sb_init(&sb);
    url_percent_encode(&sb, s);
    if (!sb.data) sb.data = (char*)calloc(1, 1);
    return v_str_take(sb.data);
}

static Value* b_url_decode(int argc, Value** argv, Env* e) {
    (void)e; EXPECT_ARGC("url:decode", 1);
    const char* s = str_arg(argv[0], "url:decode");
    StrBuf sb; sb_init(&sb);
    for (const char* p = s; *p; p++) {
        if (*p == '+') { sb_putc(&sb, ' '); continue; }
        if (*p == '%' && p[1] && p[2]) {
            int hi = url_hex_val(p[1]), lo = url_hex_val(p[2]);
            if (hi >= 0 && lo >= 0) {
                sb_putc(&sb, (char)((hi << 4) | lo));
                p += 2;
                continue;
            }
        }
        sb_putc(&sb, *p);
    }
    if (!sb.data) sb.data = (char*)calloc(1, 1);
    return v_str_take(sb.data);
}

/* (url:build base params) — appends ?k=v&k=v to base, encoding both sides.
 * If base already contains a '?', the new params join with '&'. */
static Value* b_url_build(int argc, Value** argv, Env* e) {
    (void)e; EXPECT_ARGC("url:build", 2);
    const char* base = str_arg(argv[0], "url:build");
    Value* params = argv[1];
    if (params->type != V_OBJECT)
        z_raise("url:build: params must be an object");

    StrBuf sb; sb_init(&sb);
    sb_puts(&sb, base);
    int first = strchr(base, '?') == NULL;
    for (size_t i = 0; i < params->as.obj.len; i++) {
        char* val = value_to_cstr(params->as.obj.vals[i]);
        sb_putc(&sb, first ? '?' : '&');
        first = 0;
        url_percent_encode(&sb, params->as.obj.keys[i]);
        sb_putc(&sb, '=');
        url_percent_encode(&sb, val);
        free(val);
    }
    if (!sb.data) sb.data = (char*)calloc(1, 1);
    return v_str_take(sb.data);
}

/* ============================================================
 * zip / tar — shell-out wrappers
 * ============================================================ */

/* These are defined further down the file (in HTTP / b_exec). Forward them. */
static char* z_capture_command(const char* cmd, int* out_code);
static int   z_strcaseeq(const char* a, const char* b);

static void archive_append_quoted(StrBuf* sb, const char* s, const char* fn) {
    if (strchr(s, '"')) z_raise("%s: path may not contain a double quote", fn);
    sb_putc(sb, '"');
    sb_puts(sb, s);
    sb_putc(sb, '"');
}

static Value* archive_run(const char* fn, char* cmd) {
    int code = 0;
    char* out = z_capture_command(cmd, &code);
    if (code != 0) {
        char* msg = out ? out : str_dup("");
        size_t L = strlen(msg);
        while (L && (msg[L-1] == '\n' || msg[L-1] == '\r')) msg[--L] = 0;
        z_raise("%s: failed (code %d): %s", fn, code, msg);
    }
    free(out);
    return v_true();
}

/* (zip:create archive files) */
static Value* b_zip_create(int argc, Value** argv, Env* e) {
    (void)e; EXPECT_ARGC("zip:create", 2);
    const char* archive = str_arg(argv[0], "zip:create");
    Value* files = argv[1];
    if (files->type != V_ARRAY && files->type != V_LIST)
        z_raise("zip:create: files must be an array");

    StrBuf sb; sb_init(&sb);
    sb_puts(&sb, "zip -q -r ");
    archive_append_quoted(&sb, archive, "zip:create");
    for (size_t i = 0; i < files->as.list.len; i++) {
        Value* f = files->as.list.items[i];
        if (f->type != V_STR) z_raise("zip:create: file paths must be strings");
        sb_putc(&sb, ' ');
        archive_append_quoted(&sb, f->as.s, "zip:create");
    }
    archive_run("zip:create", sb.data);
    free(sb.data);
    return v_str(archive);
}

/* (zip:extract archive [dest-dir]) */
static Value* b_zip_extract(int argc, Value** argv, Env* e) {
    (void)e;
    if (argc < 1 || argc > 2)
        z_raise("zip:extract: expected (zip:extract archive [dest])");
    const char* archive = str_arg(argv[0], "zip:extract");
    const char* dest    = argc >= 2 ? str_arg(argv[1], "zip:extract") : ".";
    StrBuf sb; sb_init(&sb);
    sb_puts(&sb, "unzip -q -o ");
    archive_append_quoted(&sb, archive, "zip:extract");
    sb_puts(&sb, " -d ");
    archive_append_quoted(&sb, dest, "zip:extract");
    archive_run("zip:extract", sb.data);
    free(sb.data);
    return v_str(dest);
}

/* (tar:create archive files [compression])
 * compression: "none" (default), "gz", "bz2", "xz" */
static Value* b_tar_create(int argc, Value** argv, Env* e) {
    (void)e;
    if (argc < 2 || argc > 3)
        z_raise("tar:create: expected (tar:create archive files [compression])");
    const char* archive = str_arg(argv[0], "tar:create");
    Value* files = argv[1];
    if (files->type != V_ARRAY && files->type != V_LIST)
        z_raise("tar:create: files must be an array");
    const char* comp = argc >= 3 ? str_arg(argv[2], "tar:create") : "none";

    const char* flag;
    if      (z_strcaseeq(comp, "none") || !*comp) flag = "-cf";
    else if (z_strcaseeq(comp, "gz")   || z_strcaseeq(comp, "gzip"))  flag = "-czf";
    else if (z_strcaseeq(comp, "bz2")  || z_strcaseeq(comp, "bzip2")) flag = "-cjf";
    else if (z_strcaseeq(comp, "xz")) flag = "-cJf";
    else {
        z_raise("tar:create: unknown compression '%s' (none, gz, bz2, xz)", comp);
        return v_null();
    }

    StrBuf sb; sb_init(&sb);
    sb_puts(&sb, "tar ");
    sb_puts(&sb, flag);
    sb_putc(&sb, ' ');
    archive_append_quoted(&sb, archive, "tar:create");
    for (size_t i = 0; i < files->as.list.len; i++) {
        Value* f = files->as.list.items[i];
        if (f->type != V_STR) z_raise("tar:create: file paths must be strings");
        sb_putc(&sb, ' ');
        archive_append_quoted(&sb, f->as.s, "tar:create");
    }
    archive_run("tar:create", sb.data);
    free(sb.data);
    return v_str(archive);
}

/* (tar:extract archive [dest]) — auto-detects compression by extension. */
static Value* b_tar_extract(int argc, Value** argv, Env* e) {
    (void)e;
    if (argc < 1 || argc > 2)
        z_raise("tar:extract: expected (tar:extract archive [dest])");
    const char* archive = str_arg(argv[0], "tar:extract");
    const char* dest    = argc >= 2 ? str_arg(argv[1], "tar:extract") : ".";
    StrBuf sb; sb_init(&sb);
    sb_puts(&sb, "tar -xf ");
    archive_append_quoted(&sb, archive, "tar:extract");
    sb_puts(&sb, " -C ");
    archive_append_quoted(&sb, dest, "tar:extract");
    archive_run("tar:extract", sb.data);
    free(sb.data);
    return v_str(dest);
}

/* ============================================================
 * scanf — parse formatted input into an array of values
 * ============================================================ */

/* Read one line from stdin, stripped of trailing newline. Returns a
 * heap-allocated string the caller owns. */
static char* z_read_stdin_line(void) {
    size_t cap = 128, len = 0;
    char* buf = (char*)malloc(cap);
    int c;
    while ((c = fgetc(stdin)) != EOF && c != '\n') {
        if (len + 2 > cap) { cap *= 2; buf = (char*)realloc(buf, cap); }
        buf[len++] = (char)c;
    }
    if (len > 0 && buf[len-1] == '\r') len--;
    buf[len] = 0;
    return buf;
}

/* (input)         → next line of stdin
 * (input "msg")   → prints msg, then reads next line of stdin */
static Value* b_input(int argc, Value** argv, Env* e) {
    (void)e;
    if (argc > 1) z_raise("input: expected (input) or (input prompt)");
    if (argc == 1) {
        const char* p = str_arg(argv[0], "input");
        fputs(p, stdout);
        fflush(stdout);
    }
    return v_str_take(z_read_stdin_line());
}

/* Supports: %d/%i (integer), %f/%g/%e (float), %s (until whitespace),
 * %c (single character), %% (literal %).  Whitespace in the format matches
 * any run of whitespace in the input; other literal characters must match
 * exactly. Returns an array of parsed values.
 *
 *   (scanf fmt)         — reads one line from stdin and parses it
 *   (scanf fmt string)  — parses the given string instead
 */
static Value* b_scanf(int argc, Value** argv, Env* e) {
    (void)e;
    if (argc < 1 || argc > 2) z_raise("scanf: expected (scanf fmt [input])");
    const char* fmt = str_arg(argv[0], "scanf");
    char* line_owned = NULL;
    const char* in;
    if (argc == 2) {
        in = str_arg(argv[1], "scanf");
    } else {
        line_owned = z_read_stdin_line();
        in = line_owned;
    }

    Value* out = v_array();
    const char* f = fmt;
    while (*f && *in) {
        if (*f == '%') {
            f++;
            char spec = *f++;
            if (spec == '%') {
                if (*in != '%') break;
                in++;
                continue;
            }
            while (*in && isspace((unsigned char)*in)) in++;
            if (!*in && spec != 's') break;

            if (spec == 'd' || spec == 'i') {
                char* end;
                long n = strtol(in, &end, 10);
                if (end == in) break;
                vlist_push(&out->as.list, v_num((double)n));
                in = end;
            } else if (spec == 'f' || spec == 'g' || spec == 'e') {
                char* end;
                double n = strtod(in, &end);
                if (end == in) break;
                vlist_push(&out->as.list, v_num(n));
                in = end;
            } else if (spec == 's') {
                const char* start = in;
                while (*in && !isspace((unsigned char)*in)) in++;
                if (in == start) break;
                vlist_push(&out->as.list, v_str_take(str_dup_n(start, in - start)));
            } else if (spec == 'c') {
                char one[2] = { *in, 0 };
                vlist_push(&out->as.list, v_str(one));
                in++;
            } else {
                /* unsupported spec — bail out gracefully */
                break;
            }
        } else if (isspace((unsigned char)*f)) {
            while (*in && isspace((unsigned char)*in)) in++;
            f++;
        } else {
            if (*in != *f) break;
            in++;
            f++;
        }
    }
    free(line_owned);
    return out;
}

/* ---------- core ---------- */
static Value* b_print(int argc, Value** argv, Env* e) {
    (void)e;
    for (int i = 0; i < argc; i++) {
        if (i) fputc(' ', stdout);
        print_value(stdout, argv[i], 0);
    }
    fputc('\n', stdout);
    fflush(stdout);
    return v_null();
}
static Value* b_type(int argc, Value** argv, Env* e) {
    (void)e;
    EXPECT_ARGC("type", 1);
    return v_str(type_name(argv[0]));
}
static Value* b_assert(int argc, Value** argv, Env* e) {
    (void)e;
    EXPECT_MIN("assert", 1);
    if (!is_truthy(argv[0])) {
        const char* msg = (argc >= 2 && argv[1]->type == V_STR) ? argv[1]->as.s : "assertion failed";
        z_raise("assert: %s", msg);
    }
    return v_true();
}
static Value* b_sleep(int argc, Value** argv, Env* e) {
    (void)e;
    EXPECT_ARGC("sleep", 1);
    double sec = num_arg(argv[0], "sleep");
    z_sleep_seconds(sec);
    return v_null();
}

/* ---------- math ---------- */
static Value* b_min(int argc, Value** argv, Env* e) {
    (void)e; EXPECT_MIN("min", 1);
    double m = num_arg(argv[0], "min");
    for (int i = 1; i < argc; i++) { double x = num_arg(argv[i], "min"); if (x < m) m = x; }
    return v_num(m);
}
static Value* b_max(int argc, Value** argv, Env* e) {
    (void)e; EXPECT_MIN("max", 1);
    double m = num_arg(argv[0], "max");
    for (int i = 1; i < argc; i++) { double x = num_arg(argv[i], "max"); if (x > m) m = x; }
    return v_num(m);
}
static Value* b_floor(int argc, Value** argv, Env* e) {
    (void)e; EXPECT_ARGC("floor", 1);
    return v_num(floor(num_arg(argv[0], "floor")));
}
static Value* b_ceil(int argc, Value** argv, Env* e) {
    (void)e; EXPECT_ARGC("ceil", 1);
    return v_num(ceil(num_arg(argv[0], "ceil")));
}
static Value* b_random(int argc, Value** argv, Env* e) {
    (void)e; (void)argc; (void)argv;
    return v_num((double)rand() / (double)RAND_MAX);
}
static Value* b_abs(int argc, Value** argv, Env* e) {
    (void)e; EXPECT_ARGC("abs", 1);
    return v_num(fabs(num_arg(argv[0], "abs")));
}

/* ---------- file I/O ---------- */
static Value* b_read(int argc, Value** argv, Env* e) {
    (void)e; EXPECT_ARGC("read", 1);
    const char* path = str_arg(argv[0], "read");
    FILE* f = fopen(path, "rb");
    if (!f) z_raise("read: cannot open '%s': %s", path, strerror(errno));
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* buf = (char*)malloc(n + 1);
    size_t r = fread(buf, 1, n, f);
    buf[r] = '\0';
    fclose(f);
    return v_str_take(buf);
}
static Value* b_write(int argc, Value** argv, Env* e) {
    (void)e; EXPECT_ARGC("write", 2);
    const char* path = str_arg(argv[0], "write");
    const char* data = str_arg(argv[1], "write");
    FILE* f = fopen(path, "wb");
    if (!f) z_raise("write: cannot open '%s': %s", path, strerror(errno));
    fwrite(data, 1, strlen(data), f);
    fclose(f);
    return v_true();
}
static Value* b_append(int argc, Value** argv, Env* e) {
    (void)e; EXPECT_ARGC("append", 2);
    const char* path = str_arg(argv[0], "append");
    const char* data = str_arg(argv[1], "append");
    FILE* f = fopen(path, "ab");
    if (!f) z_raise("append: cannot open '%s': %s", path, strerror(errno));
    fwrite(data, 1, strlen(data), f);
    fclose(f);
    return v_true();
}
static Value* b_delete(int argc, Value** argv, Env* e) {
    (void)e; EXPECT_ARGC("delete", 1);
    const char* path = str_arg(argv[0], "delete");
    if (remove(path) != 0) z_raise("delete: cannot remove '%s': %s", path, strerror(errno));
    return v_true();
}

/* (list-dir path) → array of entry names (without "." and "..").
 * Cross-platform: uses opendir/readdir on POSIX, _findfirst/_findnext on Windows. */
static Value* b_list_dir(int argc, Value** argv, Env* e) {
    (void)e;
    EXPECT_ARGC("list-dir", 1);
    const char* path = str_arg(argv[0], "list-dir");
    Value* out = v_array();

#if defined(_WIN32)
    char pattern[2048];
    snprintf(pattern, sizeof(pattern), "%s\\*", path);
    struct _finddata_t fd;
    intptr_t h = _findfirst(pattern, &fd);
    if (h == -1)
        z_raise("list-dir: cannot open '%s': %s", path, strerror(errno));
    do {
        if (strcmp(fd.name, ".") == 0 || strcmp(fd.name, "..") == 0) continue;
        vlist_push(&out->as.list, v_str(fd.name));
    } while (_findnext(h, &fd) == 0);
    _findclose(h);
#else
    DIR* d = opendir(path);
    if (!d) z_raise("list-dir: cannot open '%s': %s", path, strerror(errno));
    struct dirent* ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        vlist_push(&out->as.list, v_str(ent->d_name));
    }
    closedir(d);
#endif
    return out;
}

/* (file-info path) → { path, exists, is-dir, is-file, size, modified }.
 * `exists` is false (and the other fields absent) if the path doesn't resolve. */
static Value* b_file_info(int argc, Value** argv, Env* e) {
    (void)e;
    EXPECT_ARGC("file-info", 1);
    const char* path = str_arg(argv[0], "file-info");

    Value* o = v_object();
    obj_set(&o->as.obj, "path", v_str(path));

    struct stat st;
    if (stat(path, &st) != 0) {
        obj_set(&o->as.obj, "exists", v_false());
        return o;
    }
    obj_set(&o->as.obj, "exists",   v_true());
    obj_set(&o->as.obj, "is-dir",   v_bool(S_ISDIR(st.st_mode) ? 1 : 0));
    obj_set(&o->as.obj, "is-file",  v_bool(S_ISREG(st.st_mode) ? 1 : 0));
    obj_set(&o->as.obj, "size",     v_num((double)st.st_size));
    obj_set(&o->as.obj, "modified", v_num((double)st.st_mtime));
    return o;
}

/* Binary-safe file copy. Returns 0 on success, or sets *err to a message. */
static int z_copy_bytes(const char* src, const char* dst, const char** err) {
    FILE* in = fopen(src, "rb");
    if (!in) { *err = strerror(errno); return -1; }
    FILE* out = fopen(dst, "wb");
    if (!out) { *err = strerror(errno); fclose(in); return -1; }
    char buf[65536];
    size_t n;
    int rc = 0;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) { *err = "write failed"; rc = -1; break; }
    }
    if (rc == 0 && ferror(in)) { *err = "read failed"; rc = -1; }
    fclose(in);
    if (fclose(out) != 0 && rc == 0) { *err = strerror(errno); rc = -1; }
    return rc;
}

/* (copy-file src dst) — copies file contents, overwriting dst. */
static Value* b_copy_file(int argc, Value** argv, Env* e) {
    (void)e; EXPECT_ARGC("copy-file", 2);
    const char* src = str_arg(argv[0], "copy-file");
    const char* dst = str_arg(argv[1], "copy-file");
    const char* err = NULL;
    if (z_copy_bytes(src, dst, &err) != 0)
        z_raise("copy-file: '%s' -> '%s': %s", src, dst, err ? err : "unknown error");
    return v_str(dst);
}

/* (move-file src dst) — rename, falling back to copy+delete across devices. */
static Value* b_move_file(int argc, Value** argv, Env* e) {
    (void)e; EXPECT_ARGC("move-file", 2);
    const char* src = str_arg(argv[0], "move-file");
    const char* dst = str_arg(argv[1], "move-file");
    /* Remove an existing destination first so rename succeeds on Windows. */
    remove(dst);
    if (rename(src, dst) == 0) return v_str(dst);
    /* rename can fail across filesystems (EXDEV) — fall back to copy + unlink. */
    const char* err = NULL;
    if (z_copy_bytes(src, dst, &err) != 0)
        z_raise("move-file: '%s' -> '%s': %s", src, dst, err ? err : "unknown error");
    if (remove(src) != 0)
        z_raise("move-file: copied to '%s' but could not remove '%s': %s",
                dst, src, strerror(errno));
    return v_str(dst);
}

/* ---------- JSON ---------- */
typedef struct {
    const char* s;
    size_t pos;
    size_t len;
} JsonP;
static void json_skip_ws(JsonP* p) {
    while (p->pos < p->len && isspace((unsigned char)p->s[p->pos])) p->pos++;
}
static Value* json_parse_value(JsonP* p);
static Value* json_parse_string(JsonP* p) {
    p->pos++; /* opening " */
    char buf[8192]; size_t bi = 0;
    while (p->pos < p->len && p->s[p->pos] != '"') {
        char c = p->s[p->pos];
        if (c == '\\' && p->pos + 1 < p->len) {
            char esc = p->s[p->pos + 1];
            switch (esc) {
                case 'n': c = '\n'; break;
                case 't': c = '\t'; break;
                case 'r': c = '\r'; break;
                case '"': c = '"'; break;
                case '\\': c = '\\'; break;
                case '/': c = '/'; break;
                case 'b': c = '\b'; break;
                case 'f': c = '\f'; break;
                default: c = esc;
            }
            p->pos += 2;
        } else {
            p->pos++;
        }
        if (bi + 1 >= sizeof(buf)) z_raise("json:parse: string too long");
        buf[bi++] = c;
    }
    if (p->pos >= p->len) z_raise("json:parse: unterminated string");
    p->pos++;
    buf[bi] = '\0';
    return v_str_take(str_dup_n(buf, bi));
}
static Value* json_parse_value(JsonP* p) {
    json_skip_ws(p);
    if (p->pos >= p->len) z_raise("json:parse: unexpected end");
    char c = p->s[p->pos];
    if (c == '{') {
        p->pos++;
        Value* o = v_object();
        json_skip_ws(p);
        if (p->pos < p->len && p->s[p->pos] == '}') { p->pos++; return o; }
        while (1) {
            json_skip_ws(p);
            if (p->s[p->pos] != '"') z_raise("json:parse: object key must be string");
            Value* key = json_parse_string(p);
            json_skip_ws(p);
            if (p->s[p->pos] != ':') z_raise("json:parse: expected ':'");
            p->pos++;
            Value* val = json_parse_value(p);
            obj_set(&o->as.obj, key->as.s, val);
            json_skip_ws(p);
            if (p->s[p->pos] == ',') { p->pos++; continue; }
            if (p->s[p->pos] == '}') { p->pos++; return o; }
            z_raise("json:parse: expected ',' or '}'");
        }
    }
    if (c == '[') {
        p->pos++;
        Value* a = v_array();
        json_skip_ws(p);
        if (p->pos < p->len && p->s[p->pos] == ']') { p->pos++; return a; }
        while (1) {
            vlist_push(&a->as.list, json_parse_value(p));
            json_skip_ws(p);
            if (p->s[p->pos] == ',') { p->pos++; continue; }
            if (p->s[p->pos] == ']') { p->pos++; return a; }
            z_raise("json:parse: expected ',' or ']'");
        }
    }
    if (c == '"') return json_parse_string(p);
    if (c == 't' && p->pos + 3 < p->len && strncmp(p->s + p->pos, "true", 4) == 0)  { p->pos += 4; return v_true(); }
    if (c == 'f' && p->pos + 4 < p->len && strncmp(p->s + p->pos, "false", 5) == 0) { p->pos += 5; return v_false(); }
    if (c == 'n' && p->pos + 3 < p->len && strncmp(p->s + p->pos, "null", 4) == 0)  { p->pos += 4; return v_null(); }
    if (c == '-' || isdigit((unsigned char)c)) {
        size_t s = p->pos;
        if (c == '-') p->pos++;
        while (p->pos < p->len && isdigit((unsigned char)p->s[p->pos])) p->pos++;
        if (p->pos < p->len && p->s[p->pos] == '.') {
            p->pos++;
            while (p->pos < p->len && isdigit((unsigned char)p->s[p->pos])) p->pos++;
        }
        if (p->pos < p->len && (p->s[p->pos] == 'e' || p->s[p->pos] == 'E')) {
            p->pos++;
            if (p->pos < p->len && (p->s[p->pos] == '+' || p->s[p->pos] == '-')) p->pos++;
            while (p->pos < p->len && isdigit((unsigned char)p->s[p->pos])) p->pos++;
        }
        char buf[64];
        size_t n = p->pos - s;
        if (n >= sizeof(buf)) z_raise("json:parse: number too long");
        memcpy(buf, p->s + s, n); buf[n] = '\0';
        return v_num(strtod(buf, NULL));
    }
    z_raise("json:parse: unexpected character '%c'", c);
    return v_null();
}
static Value* b_json_parse(int argc, Value** argv, Env* e) {
    (void)e; EXPECT_ARGC("json:parse", 1);
    const char* s = str_arg(argv[0], "json:parse");
    JsonP p = { s, 0, strlen(s) };
    return json_parse_value(&p);
}
static Value* b_json_stringify(int argc, Value** argv, Env* e) {
    (void)e; EXPECT_ARGC("json:stringify", 1);
    StrBuf sb;
    sb_init(&sb);
    json_encode(&sb, argv[0]);
    if (!sb.data) { sb_putc(&sb, '\0'); sb.len = 0; }
    return v_str_take(sb.data);
}

/* ---------- time / system ---------- */
static Value* b_now(int argc, Value** argv, Env* e) {
    (void)e; (void)argc; (void)argv;
    return v_num(z_now_seconds());
}
static Value* b_timestamp(int argc, Value** argv, Env* e) {
    (void)e; (void)argc; (void)argv;
    return v_num((double)time(NULL));
}
static Value* b_format_date(int argc, Value** argv, Env* e) {
    (void)e;
    if (argc < 1 || argc > 2) z_raise("format-date: expected (format-date ts [fmt])");
    time_t t = (time_t)num_arg(argv[0], "format-date");
    const char* fmt = argc == 2 ? str_arg(argv[1], "format-date") : "%Y-%m-%d %H:%M:%S";
    char buf[128];
    struct tm tmv;
    z_localtime(t, &tmv);
    strftime(buf, sizeof(buf), fmt, &tmv);
    return v_str(buf);
}
static Value* b_env(int argc, Value** argv, Env* e) {
    (void)e; EXPECT_ARGC("env", 1);
    const char* name = str_arg(argv[0], "env");
    const char* v = getenv(name);
    return v ? v_str(v) : v_null();
}
/* Run a shell command via popen and capture combined stdout+stderr.
 * `out_code` (if non-NULL) gets the process exit status, decoded portably. */
static char* z_capture_command(const char* cmd, int* out_code) {
    /* Merge stderr into stdout for a single capture stream — portable on both
       POSIX and Windows cmd.exe. */
    size_t merged_len = strlen(cmd) + 6;
    char* merged = (char*)malloc(merged_len);
    snprintf(merged, merged_len, "%s 2>&1", cmd);

    FILE* p = popen(merged, "r");
    free(merged);
    if (!p) z_raise("run: failed to launch command");

    char buf[4096];
    size_t total_cap = 4096;
    size_t total_len = 0;
    char* out = (char*)malloc(total_cap);
    out[0] = '\0';
    size_t r;
    while ((r = fread(buf, 1, sizeof(buf), p)) > 0) {
        if (total_len + r + 1 > total_cap) {
            total_cap = (total_len + r) * 2;
            out = (char*)realloc(out, total_cap);
        }
        memcpy(out + total_len, buf, r);
        total_len += r;
        out[total_len] = '\0';
    }
    int status = pclose(p);
    int code;
#if defined(_WIN32)
    code = status;
#else
    if (WIFEXITED(status))       code = WEXITSTATUS(status);
    else if (WIFSIGNALED(status)) code = 128 + WTERMSIG(status);
    else                          code = -1;
#endif
    if (out_code) *out_code = code;
    return out;
}

static Value* b_exec(int argc, Value** argv, Env* e) {
    (void)e; EXPECT_ARGC("exec", 1);
    const char* cmd = str_arg(argv[0], "exec");
    return v_str_take(z_capture_command(cmd, NULL));
}

/* (run "cmd")  →  { "stdout": "...", "code": N }
 * stdout includes stderr (merged). Code is the process exit status. */
static Value* b_run(int argc, Value** argv, Env* e) {
    (void)e; EXPECT_ARGC("run", 1);
    const char* cmd = str_arg(argv[0], "run");
    int code = 0;
    char* out = z_capture_command(cmd, &code);
    Value* result = v_object();
    obj_set(&result->as.obj, "stdout", v_str_take(out));
    obj_set(&result->as.obj, "code",   v_num((double)code));
    return result;
}

/* (argv)  →  array of strings passed after the script name on the command line.
 *
 *   z myscript.z foo bar  →  (argv) is ["foo", "bar"]
 */
static Value* b_argv(int argc, Value** argv, Env* e) {
    (void)e; (void)argc; (void)argv;
    Value* a = v_array();
    /* g_prog_argv[0] = z binary, g_prog_argv[1] = script path; the rest are
       user-visible. */
    int start = g_prog_argc > 1 ? 2 : 1;
    for (int i = start; i < g_prog_argc; i++) {
        vlist_push(&a->as.list, v_str(g_prog_argv[i]));
    }
    return a;
}
static Value* b_exit(int argc, Value** argv, Env* e) {
    (void)e;
    int code = argc >= 1 ? (int)num_arg(argv[0], "exit") : 0;
    exit(code);
    return v_null();
}

/* ---------- HTTP — shells out to curl to keep core deps at zero ---------- */

/* case-insensitive ASCII strcmp — used to detect a user-provided Content-Type */
static int z_strcaseeq(const char* a, const char* b) {
    while (*a && *b) {
        char ca = (char)tolower((unsigned char)*a);
        char cb = (char)tolower((unsigned char)*b);
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

/* Append a -H "key: value" argument to the command being built.
 * Values are wrapped in double quotes; embedded " is rejected as a
 * safety measure (the workflow language doesn't try to be a shell). */
static void http_append_header(StrBuf* sb, const char* key, const char* value, const char* fn) {
    if (strchr(key, '"') || strchr(value, '"'))
        z_raise("%s: header key/value may not contain a double quote", fn);
    sb_puts(sb, " -H \"");
    sb_puts(sb, key);
    sb_puts(sb, ": ");
    sb_puts(sb, value);
    sb_putc(sb, '"');
}

/* Did the caller supply a Content-Type header? Lets us suppress the default. */
static int http_has_content_type(Value* headers) {
    if (!headers || headers->type != V_OBJECT) return 0;
    for (size_t i = 0; i < headers->as.obj.len; i++) {
        if (z_strcaseeq(headers->as.obj.keys[i], "Content-Type")) return 1;
    }
    return 0;
}

/* Walk a headers object and append each pair as a -H "..." flag. */
static void http_emit_headers(StrBuf* sb, Value* headers, const char* fn) {
    if (!headers || headers->type == V_NULL) return;
    if (headers->type != V_OBJECT)
        z_raise("%s: headers must be an object", fn);
    for (size_t i = 0; i < headers->as.obj.len; i++) {
        Value* v = headers->as.obj.vals[i];
        const char* val_str;
        char numbuf[64];
        if (v->type == V_STR) val_str = v->as.s;
        else if (v->type == V_NUM) {
            double n = v->as.n;
            if (n == (long long)n && n > -1e15 && n < 1e15)
                snprintf(numbuf, sizeof(numbuf), "%lld", (long long)n);
            else
                snprintf(numbuf, sizeof(numbuf), "%g", n);
            val_str = numbuf;
        } else if (v->type == V_BOOL) {
            val_str = v->as.b ? "true" : "false";
        } else {
            z_raise("%s: header values must be string/number/boolean", fn);
            val_str = "";
        }
        http_append_header(sb, headers->as.obj.keys[i], val_str, fn);
    }
}

/* (http:get url [headers]) */
static Value* b_http_get(int argc, Value** argv, Env* e) {
    if (argc < 1 || argc > 2)
        z_raise("http:get: expected (http:get url [headers])");
    const char* url = str_arg(argv[0], "http:get");
    Value* headers = argc >= 2 ? argv[1] : NULL;

    StrBuf sb; sb_init(&sb);
    sb_puts(&sb, "curl -sS");
    http_emit_headers(&sb, headers, "http:get");
    sb_puts(&sb, " \"");
    sb_puts(&sb, url);
    sb_putc(&sb, '"');

    Value* cmd_arg[1] = { v_str_take(sb.data) };
    return b_exec(1, cmd_arg, e);
}

/* Pick a directory for short-lived temp files. Honours $TMPDIR / %TEMP%. */
static const char* z_tmp_dir(void) {
#ifdef _WIN32
    const char* d = getenv("TEMP"); if (d && *d) return d;
    d = getenv("TMP");              if (d && *d) return d;
    return ".";
#else
    const char* d = getenv("TMPDIR"); if (d && *d) return d;
    return "/tmp";
#endif
}

/* (http:post url body [headers])
 *   body string  → sent verbatim; default Content-Type: text/plain
 *   body object  → JSON-stringified; default Content-Type: application/json
 * User-provided Content-Type in headers wins.
 *
 * Body is written to a temp file and passed to curl via --data-binary @<file>,
 * so arbitrary content (JSON with quotes, binary data) works on every shell. */
static Value* b_http_post(int argc, Value** argv, Env* e) {
    if (argc < 2 || argc > 3)
        z_raise("http:post: expected (http:post url body [headers])");
    const char* url = str_arg(argv[0], "http:post");
    Value* body    = argv[1];
    Value* headers = argc >= 3 ? argv[2] : NULL;

    /* Stringify body. */
    const char* body_str;
    Value* owned = NULL;
    if (body->type == V_STR) {
        body_str = body->as.s;
    } else {
        Value* json_args[1] = { body };
        owned = b_json_stringify(1, json_args, e);
        body_str = owned->as.s;
    }

    /* Write body to a temp file. */
    char tmp_path[1024];
    snprintf(tmp_path, sizeof(tmp_path), "%s/z_http_body_%d_%d.tmp",
             z_tmp_dir(), (int)time(NULL), rand());
    FILE* tf = fopen(tmp_path, "wb");
    if (!tf) z_raise("http:post: cannot create temp file '%s'", tmp_path);
    if (*body_str) fwrite(body_str, 1, strlen(body_str), tf);
    fclose(tf);

    StrBuf sb; sb_init(&sb);
    sb_puts(&sb, "curl -sS -X POST");

    /* Default Content-Type only if the user didn't supply one. */
    if (!http_has_content_type(headers)) {
        if (body->type == V_STR)
            sb_puts(&sb, " -H \"Content-Type: text/plain\"");
        else
            sb_puts(&sb, " -H \"Content-Type: application/json\"");
    }
    http_emit_headers(&sb, headers, "http:post");

    sb_puts(&sb, " --data-binary @\"");
    sb_puts(&sb, tmp_path);
    sb_puts(&sb, "\" \"");
    sb_puts(&sb, url);
    sb_putc(&sb, '"');

    Value* cmd_arg[1] = { v_str_take(sb.data) };
    Value* result = b_exec(1, cmd_arg, e);
    remove(tmp_path);
    return result;
}

/* ---------- import ---------- */
/* Forward declaration */
static Value* run_source(const char* src, Env* env);

static Value* b_import(int argc, Value** argv, Env* e) {
    EXPECT_ARGC("import", 1);
    const char* path = str_arg(argv[0], "import");
    FILE* f = fopen(path, "rb");
    if (!f) z_raise("import: cannot open '%s': %s", path, strerror(errno));
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* buf = (char*)malloc(n + 1);
    fread(buf, 1, n, f);
    buf[n] = '\0';
    fclose(f);
    Value* r = run_source(buf, e);
    free(buf);
    return r;
}

/* ============================================================
 * Optional modules (compiled in via -DZ_WITH_*)
 * ============================================================ */

#ifdef Z_WITH_IMAGE
#include "z_img.h"
#endif

/* ============================================================
 * (help) — cheat sheet printer
 * ============================================================ */

static int z_stdout_is_tty(void) {
#ifdef _WIN32
    return _isatty(_fileno(stdout));
#else
    return isatty(fileno(stdout));
#endif
}

/* Chosen at the start of each help call so colour disappears under pipes. */
static const char* HBOLD = "";
static const char* HDIM  = "";
static const char* HRST  = "";

static void help_set_colors(void) {
    if (z_stdout_is_tty()) {
        HBOLD = "\x1b[1m";
        HDIM  = "\x1b[2m";
        HRST  = "\x1b[0m";
    } else {
        HBOLD = HDIM = HRST = "";
    }
}

/* Each topic is one section. Printing a topic prints its block. */
typedef struct { const char* key; const char* heading; const char* body; } HelpTopic;

static const HelpTopic g_help_topics[] = {
    { "forms", "special forms",
      "  (do expr...)              sequential block; returns last value\n"
      "  (if cond then [else])     conditional\n"
      "  (while cond body...)      loop while cond is truthy\n"
      "  (for var coll body...)    iterate over array/object\n"
      "  (fn name (args) body...)  define named function\n"
      "  (lambda (args) body...)   anonymous function\n"
      "  (set name value)          define/assign variable\n"
      "  (try body (catch e ...))  catch runtime errors\n"
      "  (and ...) (or ...)        short-circuit logic, also && / ||\n"
      "  (quote x)                 return x unevaluated\n"
    },
    { "arith", "arithmetic",
      "  +  -  *  /  %             usual numeric ops; + also concatenates strings\n"
    },
    { "cmp", "comparison",
      "  <  >  <=  >=  ==  !=      compare numbers or strings\n"
    },
    { "logic", "logic",
      "  !  &&  ||                 also: (and ...) (or ...)\n"
    },
    { "arrays", "arrays & objects",
      "  (array ...)               make an array\n"
      "  (object \"k\" v ...)        make an object\n"
      "  (get c k...)              index by key/position; chains: (get o \"a\" 0)\n"
      "  (put c k v)               set element/field\n"
      "  (push a x)  (pop a)       append / remove last\n"
      "  (length c)                size of string, array, or object\n"
      "  (keys o)  (values o)  (entries o)\n"
      "  (map f a)  (filter pred a)  (reduce f a [init])\n"
    },
    { "strings", "strings",
      "  (concat ...)              join values into a string\n"
      "  (split sep s)             → array\n"
      "  (trim s)  (lower s)  (upper s)\n"
      "  (replace s old new)\n"
      "  (substring s start [end])\n"
      "  (starts-with s prefix)    (ends-with s suffix)\n"
      "  (contains c x)            string / array / object key\n"
      "  (index-of c x)            -1 if not found\n"
      "  \"hello ${expr}\"           template strings — ${...} interpolates\n"
      "                            any z expression; use \\$ for literal $\n"
    },
    { "regex", "regex",
      "  (regex:test re s)         does the pattern match anywhere?\n"
      "  (regex:match re s)        first match as string, or null\n"
      "  (regex:find-all re s)     all matches → array\n"
      "  (regex:replace re s rep)  replace all matches\n"
      "  (regex:split re s)        split on pattern → array\n"
      "  pattern syntax: . * + ? ^ $ [abc] [^abc] [a-z]\n"
      "                  \\d \\w \\s  \\D \\W \\S\n"
    },
    { "math", "math",
      "  (min ...)  (max ...)      variadic\n"
      "  (floor n)  (ceil n)  (abs n)\n"
      "  (random)                  0.0–1.0\n"
    },
    { "core", "core",
      "  (print ...)               write to stdout\n"
      "  (type v)                  → \"number\" / \"string\" / ...\n"
      "  (assert cond [msg])\n"
      "  (sleep seconds)\n"
    },
    { "file", "file I/O",
      "  (read path)               → string\n"
      "  (write path s)\n"
      "  (append path s)\n"
      "  (delete path)\n"
      "  (list-dir path)           → array of entry names\n"
      "  (file-info path)          → { exists, is-dir, is-file, size, modified }\n"
      "  (copy-file src dst)\n"
      "  (move-file src dst)       rename, with cross-device fallback\n"
    },
    { "json", "JSON",
      "  (json:parse s)            → value\n"
      "  (json:stringify v)        → string\n"
    },
    { "crypto", "encoding & crypto",
      "  (base64:encode s)         → base64 string\n"
      "  (base64:decode s)         → original string\n"
      "  (encrypt key text)        → base64 ciphertext (lightweight XTEA-CTR)\n"
      "  (decrypt key cipher)      → original text\n"
      "  (uuid)                    → random v4 UUID\n"
    },
    { "url", "URLs",
      "  (url:encode s)            → percent-encoded string\n"
      "  (url:decode s)            → decoded string\n"
      "  (url:build base params)   → base + ?k=v&k=v from an object\n"
    },
    { "archive", "archives & parsing",
      "  (zip:create archive files)         needs `zip`\n"
      "  (zip:extract archive [dest])       needs `unzip`\n"
      "  (tar:create archive files [comp])  comp: none|gz|bz2|xz\n"
      "  (tar:extract archive [dest])\n"
      "  (scanf fmt [input])       → array; reads stdin if input omitted\n"
      "  (input [prompt])          → line of stdin (optional prompt)\n"
    },
    { "http", "HTTP (via curl)",
      "  (http:get url [headers])      → response body\n"
      "  (http:post url body [headers])  body: object → JSON, string → raw\n"
      "  headers: optional object, e.g. (object \"Authorization\" \"Bearer ...\")\n"
    },
    { "system", "system",
      "  (now)                     unix seconds, float\n"
      "  (timestamp)               unix seconds, int\n"
      "  (format-date ts [fmt])    strftime — %Y %m %d %H %M %S %A %B ...\n"
      "  (env name)                getenv\n"
      "  (exec cmd)                → stdout+stderr as string\n"
      "  (run cmd)                 → { stdout, code }\n"
      "  (argv)                    args passed to the z program\n"
      "  (exit [code])\n"
      "  (import \"file.z\")         load and evaluate another file\n"
    },
    { "image", "images (optional — build with IMAGE=1)",
      "  (img:create dst w h [color])\n"
      "  (img:info path)           → { width, height, format }\n"
      "  (img:resize src dst w h)\n"
      "  (img:crop src dst x y w h)\n"
      "  (img:rotate src dst deg)\n"
      "  (img:circle src dst cx cy r fill [stroke] [width])\n"
      "  (img:rect src dst x y w h fill [stroke] [width])\n"
      "  (img:add-text src dst text [x y size color])\n"
      "  (img:grayscale src dst)\n"
      "  (img:bw src dst [threshold])\n"
      "  (img:to-pdf images dst)\n"
      "  (img:qr text dst [scale])           needs qrencode or zint\n"
      "  (img:barcode data dst [type])       needs zint\n"
    },
};

#define HELP_TOPIC_COUNT (sizeof(g_help_topics) / sizeof(g_help_topics[0]))

static void help_print_topic(const HelpTopic* t) {
    printf("%s%s%s\n%s\n", HBOLD, t->heading, HRST, t->body);
}

static Value* b_help(int argc, Value** argv, Env* env) {
    help_set_colors();
    /* Only show the image section if it was actually compiled in. */
    int has_image = env_lookup(env, "img:create") != NULL;

    if (argc == 0) {
        printf("%sz language cheat sheet%s\n", HBOLD, HRST);
        printf("%s(help \"topic\") for one section · "
               "topics: forms arith cmp logic arrays strings regex math core "
               "file json crypto url archive http system%s%s\n\n",
               HDIM, has_image ? " image" : "", HRST);
        for (size_t i = 0; i < HELP_TOPIC_COUNT; i++) {
            if (strcmp(g_help_topics[i].key, "image") == 0 && !has_image) continue;
            help_print_topic(&g_help_topics[i]);
        }
        return v_null();
    }

    if (argc == 1 && argv[0]->type == V_STR) {
        const char* want = argv[0]->as.s;
        for (size_t i = 0; i < HELP_TOPIC_COUNT; i++) {
            if (strcmp(g_help_topics[i].key, want) == 0) {
                if (strcmp(want, "image") == 0 && !has_image) {
                    printf("image module not compiled in — rebuild with IMAGE=1\n");
                    return v_null();
                }
                help_print_topic(&g_help_topics[i]);
                return v_null();
            }
        }
        printf("no such help topic: %s\n", want);
        return v_null();
    }

    z_raise("help: expected (help) or (help \"topic\")");
    return v_null();
}

/* ============================================================
 * Wiring built-ins into the global env
 * ============================================================ */

static void install_builtins(Env* env) {
    /* arithmetic */
    env_define(env, "+", v_native(b_add));
    env_define(env, "-", v_native(b_sub));
    env_define(env, "*", v_native(b_mul));
    env_define(env, "/", v_native(b_div));
    env_define(env, "%", v_native(b_mod));
    /* comparison */
    env_define(env, "<",  v_native(b_lt));
    env_define(env, ">",  v_native(b_gt));
    env_define(env, "<=", v_native(b_le));
    env_define(env, ">=", v_native(b_ge));
    env_define(env, "==", v_native(b_eq));
    env_define(env, "!=", v_native(b_ne));
    env_define(env, "!",  v_native(b_not));
    /* collections */
    env_define(env, "array",   v_native(b_array));
    env_define(env, "object",  v_native(b_object));
    env_define(env, "get",     v_native(b_get));
    env_define(env, "put",     v_native(b_put));
    env_define(env, "push",    v_native(b_push));
    env_define(env, "pop",     v_native(b_pop));
    env_define(env, "length",  v_native(b_length));
    env_define(env, "keys",    v_native(b_keys));
    env_define(env, "values",  v_native(b_values));
    env_define(env, "entries", v_native(b_entries));
    env_define(env, "map",     v_native(b_map));
    env_define(env, "filter",  v_native(b_filter));
    env_define(env, "reduce",  v_native(b_reduce));
    /* string */
    env_define(env, "concat",    v_native(b_concat));
    env_define(env, "split",     v_native(b_split));
    env_define(env, "trim",      v_native(b_trim));
    env_define(env, "lower",     v_native(b_lower));
    env_define(env, "upper",     v_native(b_upper));
    env_define(env, "replace",     v_native(b_replace));
    env_define(env, "substring",   v_native(b_substring));
    env_define(env, "starts-with", v_native(b_starts_with));
    env_define(env, "ends-with",   v_native(b_ends_with));
    env_define(env, "contains",    v_native(b_contains));
    env_define(env, "index-of",    v_native(b_index_of));
    /* regex */
    env_define(env, "regex:test",     v_native(b_rx_test));
    env_define(env, "regex:match",    v_native(b_rx_match));
    env_define(env, "regex:find-all", v_native(b_rx_find_all));
    env_define(env, "regex:replace",  v_native(b_rx_replace));
    env_define(env, "regex:split",    v_native(b_rx_split));
    /* core */
    env_define(env, "print",  v_native(b_print));
    env_define(env, "type",   v_native(b_type));
    env_define(env, "assert", v_native(b_assert));
    env_define(env, "sleep",  v_native(b_sleep));
    /* math */
    env_define(env, "min",    v_native(b_min));
    env_define(env, "max",    v_native(b_max));
    env_define(env, "floor",  v_native(b_floor));
    env_define(env, "ceil",   v_native(b_ceil));
    env_define(env, "random", v_native(b_random));
    env_define(env, "abs",    v_native(b_abs));
    /* file */
    env_define(env, "read",      v_native(b_read));
    env_define(env, "write",     v_native(b_write));
    env_define(env, "append",    v_native(b_append));
    env_define(env, "delete",    v_native(b_delete));
    env_define(env, "list-dir",  v_native(b_list_dir));
    env_define(env, "file-info", v_native(b_file_info));
    env_define(env, "copy-file", v_native(b_copy_file));
    env_define(env, "move-file", v_native(b_move_file));
    /* json */
    env_define(env, "json:parse",     v_native(b_json_parse));
    env_define(env, "json:stringify", v_native(b_json_stringify));
    /* base64 / crypto / ids */
    env_define(env, "base64:encode", v_native(b_base64_encode));
    env_define(env, "base64:decode", v_native(b_base64_decode));
    env_define(env, "encrypt",       v_native(b_encrypt));
    env_define(env, "decrypt",       v_native(b_decrypt));
    env_define(env, "uuid",          v_native(b_uuid));
    /* URL handling */
    env_define(env, "url:encode",    v_native(b_url_encode));
    env_define(env, "url:decode",    v_native(b_url_decode));
    env_define(env, "url:build",     v_native(b_url_build));
    /* archives */
    env_define(env, "zip:create",    v_native(b_zip_create));
    env_define(env, "zip:extract",   v_native(b_zip_extract));
    env_define(env, "tar:create",    v_native(b_tar_create));
    env_define(env, "tar:extract",   v_native(b_tar_extract));
    /* parsing / input */
    env_define(env, "scanf",         v_native(b_scanf));
    env_define(env, "input",         v_native(b_input));
    /* http */
    env_define(env, "http:get",  v_native(b_http_get));
    env_define(env, "http:post", v_native(b_http_post));
    /* system / time */
    env_define(env, "now",         v_native(b_now));
    env_define(env, "timestamp",   v_native(b_timestamp));
    env_define(env, "format-date", v_native(b_format_date));
    env_define(env, "env",         v_native(b_env));
    env_define(env, "exec",        v_native(b_exec));
    env_define(env, "run",         v_native(b_run));
    env_define(env, "argv",        v_native(b_argv));
    env_define(env, "exit",        v_native(b_exit));
    /* modules */
    env_define(env, "import", v_native(b_import));

    /* help */
    env_define(env, "help", v_native(b_help));

    /* Optional modules — only present if compiled in. */
#ifdef Z_WITH_IMAGE
    install_image_builtins(env);
#endif
}

/* ============================================================
 * Driver: run source, REPL, main
 * ============================================================ */

static Value* run_source(const char* src, Env* env) {
    Value* program = parse_all(src);
    return eval(program, env);
}

static char* read_file_all(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "z: cannot open %s: %s\n", path, strerror(errno)); exit(1); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* buf = (char*)malloc(n + 1);
    fread(buf, 1, n, f);
    buf[n] = '\0';
    fclose(f);
    return buf;
}

/* ============================================================
 * REPL line editor with arrow-key history (cross-platform)
 * ============================================================ */

#define Z_HIST_MAX 1000
#define Z_LINE_MAX 8192

typedef struct {
    char* lines[Z_HIST_MAX];
    int   count;       /* current entries (max Z_HIST_MAX) */
    int   first;       /* index of oldest entry — ring buffer */
} History;

static History g_history;

static const char* hist_get(int idx) {
    if (idx < 0 || idx >= g_history.count) return NULL;
    return g_history.lines[(g_history.first + idx) % Z_HIST_MAX];
}

static void hist_add(const char* line) {
    if (!line || !*line) return;
    /* dedup against most recent */
    if (g_history.count > 0) {
        const char* last = hist_get(g_history.count - 1);
        if (last && strcmp(last, line) == 0) return;
    }
    if (g_history.count < Z_HIST_MAX) {
        int idx = (g_history.first + g_history.count) % Z_HIST_MAX;
        g_history.lines[idx] = str_dup(line);
        g_history.count++;
    } else {
        free(g_history.lines[g_history.first]);
        g_history.lines[g_history.first] = str_dup(line);
        g_history.first = (g_history.first + 1) % Z_HIST_MAX;
    }
}

static const char* hist_path(char* buf, size_t bufsz) {
    const char* home;
#ifdef _WIN32
    home = getenv("USERPROFILE");
    if (!home) home = getenv("HOMEPATH");
#else
    home = getenv("HOME");
#endif
    if (!home || !*home) return NULL;
    snprintf(buf, bufsz, "%s/.z_history", home);
    return buf;
}

static void hist_load(void) {
    char path[1024];
    if (!hist_path(path, sizeof(path))) return;
    FILE* f = fopen(path, "r");
    if (!f) return;
    char line[Z_LINE_MAX];
    while (fgets(line, sizeof(line), f)) {
        size_t n = strlen(line);
        while (n && (line[n-1] == '\n' || line[n-1] == '\r')) line[--n] = 0;
        if (n) hist_add(line);
    }
    fclose(f);
}

static void hist_save(void) {
    char path[1024];
    if (!hist_path(path, sizeof(path))) return;
    FILE* f = fopen(path, "w");
    if (!f) return;
    for (int i = 0; i < g_history.count; i++) {
        const char* l = hist_get(i);
        if (l) fprintf(f, "%s\n", l);
    }
    fclose(f);
}

/* --- platform raw-mode + key reading --- */

#ifdef _WIN32
static int z_isatty(void)  { return _isatty(_fileno(stdin)); }
static int raw_enable(void) {
    /* Enable ANSI escape sequence interpretation on Windows 10+. */
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode;
    if (GetConsoleMode(hOut, &mode))
        SetConsoleMode(hOut, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    return 0;
}
static void raw_disable(void) { }
static int read_key(void) { return _getch(); }
#else
static struct termios g_orig_termios;
static int            g_raw_enabled = 0;

static int z_isatty(void) { return isatty(STDIN_FILENO); }
static int raw_enable(void) {
    if (!isatty(STDIN_FILENO)) return -1;
    if (tcgetattr(STDIN_FILENO, &g_orig_termios) != 0) return -1;
    struct termios raw = g_orig_termios;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN]  = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) return -1;
    g_raw_enabled = 1;
    return 0;
}
static void raw_disable(void) {
    if (g_raw_enabled) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_orig_termios);
        g_raw_enabled = 0;
    }
}
static int read_key(void) {
    unsigned char c;
    if (read(STDIN_FILENO, &c, 1) != 1) return -1;
    return (int)c;
}
#endif

/* Refresh the displayed line so it matches buf+pos. Uses ANSI escapes. */
static void redraw_line(const char* prompt, const char* buf, size_t len, size_t pos) {
    fputc('\r', stdout);
    fputs(prompt, stdout);
    fwrite(buf, 1, len, stdout);
    fputs("\x1b[K", stdout);                 /* clear to end of line */
    if (pos < len) printf("\x1b[%dD", (int)(len - pos));  /* cursor left */
    fflush(stdout);
}

/* read_line return codes:
 *   0 = got a line in `out`
 *   1 = Ctrl-C pressed (current input discarded)
 *  -1 = EOF (Ctrl-D on empty input, or stream closed)
 */
static int z_read_line(const char* prompt, char* out, size_t outsz) {
    if (!z_isatty() || raw_enable() != 0) {
        /* Non-interactive fallback: behave like fgets. */
        fputs(prompt, stdout);
        fflush(stdout);
        if (!fgets(out, (int)outsz, stdin)) return -1;
        size_t n = strlen(out);
        while (n && (out[n-1] == '\n' || out[n-1] == '\r')) out[--n] = 0;
        return 0;
    }

    size_t len = 0, pos = 0;
    int    hist_pos = g_history.count;
    char   saved[Z_LINE_MAX]; saved[0] = 0;
    out[0] = 0;

    fputs(prompt, stdout);
    fflush(stdout);

    while (1) {
        int c = read_key();
        if (c < 0) { raw_disable(); return -1; }

        /* Enter — emit CRLF because POSIX raw mode has OPOST off, so a bare
           '\n' won't return the cursor to column 0. */
        if (c == '\r' || c == '\n') {
            out[len] = 0;
            fputs("\r\n", stdout);
            fflush(stdout);
            raw_disable();
            return 0;
        }
        /* Ctrl-C */
        if (c == 3) {
            fputs("^C\r\n", stdout);
            fflush(stdout);
            out[0] = 0;
            raw_disable();
            return 1;
        }
        /* Ctrl-D */
        if (c == 4) {
            if (len == 0) {
                fputs("\r\n", stdout);
                fflush(stdout);
                raw_disable();
                return -1;
            }
            if (pos < len) {
                memmove(out + pos, out + pos + 1, len - pos);
                len--;
                redraw_line(prompt, out, len, pos);
            }
            continue;
        }
        /* Backspace (127 on POSIX, 8 on Windows) */
        if (c == 127 || c == 8) {
            if (pos > 0) {
                memmove(out + pos - 1, out + pos, len - pos + 1);
                pos--; len--;
                redraw_line(prompt, out, len, pos);
            }
            continue;
        }
        /* Ctrl-A / Ctrl-E — line start / end */
        if (c == 1)  { pos = 0;   redraw_line(prompt, out, len, pos); continue; }
        if (c == 5)  { pos = len; redraw_line(prompt, out, len, pos); continue; }
        /* Ctrl-K — kill to end of line */
        if (c == 11) { len = pos; out[len] = 0; redraw_line(prompt, out, len, pos); continue; }
        /* Ctrl-L — clear screen */
        if (c == 12) { fputs("\x1b[H\x1b[2J", stdout); redraw_line(prompt, out, len, pos); continue; }

#ifdef _WIN32
        /* Windows arrow keys come as a 0 or 224 prefix then a scan code. */
        if (c == 0 || c == 224) {
            int s = read_key();
            int arrow = 0;
            if      (s == 72) arrow = 'A';   /* up    */
            else if (s == 80) arrow = 'B';   /* down  */
            else if (s == 77) arrow = 'C';   /* right */
            else if (s == 75) arrow = 'D';   /* left  */
            else if (s == 71) { pos = 0;   redraw_line(prompt, out, len, pos); continue; }
            else if (s == 79) { pos = len; redraw_line(prompt, out, len, pos); continue; }
            if (!arrow) continue;
            c = 27;
            /* fall through to ESC[arrow handling below by emulating sequence */
            goto handle_arrow;
        }
#endif

        /* ESC sequence (POSIX arrows) */
        if (c == 27) {
            int s1, s2;
#ifndef _WIN32
            s1 = read_key();
            if (s1 != '[' && s1 != 'O') continue;
            s2 = read_key();
            if (s2 < 0) continue;
#else
            (void)s1; (void)s2;
handle_arrow:
            s2 = arrow;
#endif
            switch (s2) {
                case 'A':  /* up */
                    if (hist_pos > 0) {
                        if (hist_pos == g_history.count) {
                            /* save current buffer before walking back */
                            memcpy(saved, out, len);
                            saved[len] = 0;
                        }
                        hist_pos--;
                        const char* l = hist_get(hist_pos);
                        if (l) {
                            strncpy(out, l, outsz - 1);
                            out[outsz - 1] = 0;
                            len = pos = strlen(out);
                            redraw_line(prompt, out, len, pos);
                        }
                    }
                    break;
                case 'B':  /* down */
                    if (hist_pos < g_history.count) {
                        hist_pos++;
                        if (hist_pos == g_history.count) {
                            strncpy(out, saved, outsz - 1);
                            out[outsz - 1] = 0;
                        } else {
                            const char* l = hist_get(hist_pos);
                            if (l) {
                                strncpy(out, l, outsz - 1);
                                out[outsz - 1] = 0;
                            }
                        }
                        len = pos = strlen(out);
                        redraw_line(prompt, out, len, pos);
                    }
                    break;
                case 'C':  /* right */
                    if (pos < len) { pos++; redraw_line(prompt, out, len, pos); }
                    break;
                case 'D':  /* left */
                    if (pos > 0)  { pos--; redraw_line(prompt, out, len, pos); }
                    break;
                case 'H':  pos = 0;   redraw_line(prompt, out, len, pos); break;
                case 'F':  pos = len; redraw_line(prompt, out, len, pos); break;
            }
            continue;
        }

        /* Printable */
        if (c >= 32 && c < 127) {
            if (len + 1 < outsz) {
                if (pos < len) memmove(out + pos + 1, out + pos, len - pos);
                out[pos++] = (char)c;
                len++;
                out[len] = 0;
                redraw_line(prompt, out, len, pos);
            }
        }
        /* UTF-8 continuation / high bytes — insert as-is for simplicity */
        else if ((unsigned char)c >= 128) {
            if (len + 1 < outsz) {
                if (pos < len) memmove(out + pos + 1, out + pos, len - pos);
                out[pos++] = (char)c;
                len++;
                out[len] = 0;
                redraw_line(prompt, out, len, pos);
            }
        }
    }
}

static void repl(Env* env) {
    char line[Z_LINE_MAX];
    volatile int balance = 0;
    static char acc[65536]; acc[0] = '\0';

    hist_load();

    fputs("z — REPL.\n", stdout);
    fputs("  type `help` for a cheat sheet · arrows: history / cursor · Ctrl-D: exit · Ctrl-C: cancel\n", stdout);

    while (1) {
        int rc = z_read_line(balance ? "... " : "z> ", line, sizeof(line));
        if (rc == -1) { fputs("bye!\n", stdout); break; }
        if (rc == 1)  { acc[0] = '\0'; balance = 0; continue; }

        /* REPL shortcuts: bare `help` or `?` (no parens) → call (help). */
        if (balance == 0) {
            char* p = line;
            while (*p == ' ' || *p == '\t') p++;
            char* end = p + strlen(p);
            while (end > p && (end[-1] == ' ' || end[-1] == '\t'
                            || end[-1] == '\n' || end[-1] == '\r')) end--;
            size_t n = end - p;
            if ((n == 4 && strncmp(p, "help", 4) == 0)
                || (n == 1 && *p == '?')) {
                strcpy(line, "(help)\n");
            }
        }

        /* Track paren balance for multi-line input. */
        for (char* p = line; *p; p++) {
            if (*p == '(') balance++;
            else if (*p == ')') balance--;
        }
        if (strlen(acc) + strlen(line) + 2 >= sizeof(acc)) {
            acc[0] = '\0'; balance = 0;
            fputs("input too long\n", stderr);
            continue;
        }
        strcat(acc, line);
        strcat(acc, "\n");
        if (balance > 0) continue;
        if (balance < 0) {
            fprintf(stderr, "unbalanced parens\n");
            acc[0] = '\0'; balance = 0;
            continue;
        }

        /* Trim trailing newline before adding to history */
        {
            size_t n = strlen(acc);
            char saved = 0;
            if (n && acc[n-1] == '\n') { saved = acc[n-1]; acc[n-1] = 0; }
            hist_add(acc);
            if (saved) acc[n-1] = saved;
        }

        ErrFrame f;
        f.prev   = g_err_top;
        f.value  = NULL;
        f.msg[0] = '\0';
        g_err_top = &f;
        if (setjmp(f.buf) == 0) {
            Value* r = run_source(acc, env);
            g_err_top = f.prev;
            if (r && r->type != V_NULL) {
                print_value(stdout, r, 1);
                fputc('\n', stdout);
            }
        } else {
            g_err_top = f.prev;
            fprintf(stderr, "error: %s\n", f.msg);
        }
        acc[0] = '\0';
    }

    hist_save();
}

/* Define Z_NO_MAIN before #including z.c to embed the interpreter in another
 * front end (see zide.c). */
#ifndef Z_NO_MAIN
int main(int argc, char** argv) {
    srand((unsigned)time(NULL));
    g_prog_argc = argc;
    g_prog_argv = argv;
    atexit(raw_disable);  /* always restore terminal cleanly */
    Env* env = env_new(NULL);
    install_builtins(env);

    if (argc < 2) {
        repl(env);
        return 0;
    }
    /* Run file */
    char* src = read_file_all(argv[1]);
    ErrFrame f;
    f.prev   = g_err_top;
    f.value  = NULL;
    f.msg[0] = '\0';
    g_err_top = &f;
    if (setjmp(f.buf) == 0) {
        run_source(src, env);
        g_err_top = f.prev;
    } else {
        g_err_top = f.prev;
        fprintf(stderr, "z: error: %s\n", f.msg);
        free(src);
        return 1;
    }
    free(src);
    return 0;
}
#endif /* Z_NO_MAIN */
