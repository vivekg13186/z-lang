/*
 * zide.c — zide: an enhanced REPL for the z language.
 *
 * Built on top of the z interpreter core (z.c), which it #includes after
 * defining Z_NO_MAIN to suppress z's own main(). Everything the interpreter
 * exposes — eval, the environment, builtins, persistent history, and the
 * raw-mode terminal helpers — is reused directly.
 *
 * Features over the plain `z` REPL:
 *   • live syntax highlighting as you type
 *   • Tab autocompletion of builtins, special forms, and your own variables
 *   • bracket-aware multi-line input with a continuation prompt
 *   • arrow-key history (shared ~/.z_history) and in-line editing
 *   • `help` / `?` cheat sheet, `:q` to quit
 *
 * Build:
 *   cc -O2 -std=c99 zide.c -o zide -lm
 *   make zide            # drops it in dist/<os>_<arch>/zide
 */

#define Z_NO_MAIN
#include "z.c"

/* ============================================================
 * Colour palette (only emitted when stdout is a TTY)
 * ============================================================ */

static int   ZIDE_COLOR = 0;
#define ZC(code) (ZIDE_COLOR ? (code) : "")

#define C_RESET   "\x1b[0m"
#define C_COMMENT "\x1b[90m"   /* gray    */
#define C_STRING  "\x1b[32m"   /* green   */
#define C_NUMBER  "\x1b[36m"   /* cyan    */
#define C_KEYWORD "\x1b[35m"   /* magenta — special forms */
#define C_BUILTIN "\x1b[34m"   /* blue    — known functions */
#define C_INTERP  "\x1b[33m"   /* yellow  — ${...} */
#define C_PAREN   "\x1b[2m"    /* dim     — brackets */
#define C_LITERAL "\x1b[33m"   /* yellow  — true/false/null */
#define C_PROMPT  "\x1b[1;36m" /* bold cyan */
#define C_HINT    "\x1b[90m"

static const char* ZIDE_FORMS[] = {
    "do", "if", "when", "unless", "cond", "let",
    "while", "for", "fn", "lambda", "set", "try", "catch",
    "and", "or", "quote", "else", "->", "->>", NULL
};

static int zide_is_form(const char* tok) {
    for (int i = 0; ZIDE_FORMS[i]; i++)
        if (strcmp(tok, ZIDE_FORMS[i]) == 0) return 1;
    return 0;
}

static const char* zide_classify(const char* tok, Env* env) {
    if (strcmp(tok, "true") == 0 || strcmp(tok, "false") == 0 || strcmp(tok, "null") == 0)
        return C_LITERAL;
    if (zide_is_form(tok)) return C_KEYWORD;
    if (env_lookup(env, tok)) return C_BUILTIN;
    return "";  /* unknown symbol — leave at terminal default */
}

/* Emit `buf` to stdout with syntax colours. Colour codes don't advance the
 * visible cursor, so downstream cursor math by visible columns still works. */
static void zide_emit_colored(const char* buf, size_t len, Env* env) {
    if (!ZIDE_COLOR) { fwrite(buf, 1, len, stdout); return; }
    size_t i = 0;
    while (i < len) {
        char c = buf[i];

        if (c == ';') {                       /* comment to EOL */
            fputs(C_COMMENT, stdout);
            while (i < len && buf[i] != '\n') fputc(buf[i++], stdout);
            fputs(C_RESET, stdout);
            continue;
        }
        if (c == '"') {                       /* string literal */
            fputs(C_STRING, stdout);
            fputc('"', stdout); i++;
            while (i < len && buf[i] != '"') {
                if (buf[i] == '\\' && i + 1 < len) {
                    fputc(buf[i++], stdout);
                    if (i < len) fputc(buf[i++], stdout);
                    continue;
                }
                if (buf[i] == '$' && i + 1 < len && buf[i + 1] == '{') {
                    fputs(C_INTERP, stdout);
                    fputc('$', stdout); fputc('{', stdout); i += 2;
                    int depth = 1;
                    while (i < len && depth > 0) {
                        if (buf[i] == '{') depth++;
                        else if (buf[i] == '}') { depth--; if (depth == 0) break; }
                        fputc(buf[i++], stdout);
                    }
                    if (i < len && buf[i] == '}') fputc(buf[i++], stdout);
                    fputs(C_STRING, stdout);
                    continue;
                }
                fputc(buf[i++], stdout);
            }
            if (i < len && buf[i] == '"') fputc(buf[i++], stdout);
            fputs(C_RESET, stdout);
            continue;
        }
        if (isdigit((unsigned char)c)
            || ((c == '-' || c == '+') && i + 1 < len && isdigit((unsigned char)buf[i + 1]))) {
            fputs(C_NUMBER, stdout);
            fputc(buf[i++], stdout);
            while (i < len && (isdigit((unsigned char)buf[i]) || buf[i] == '.'
                            || buf[i] == 'e' || buf[i] == 'E')) fputc(buf[i++], stdout);
            fputs(C_RESET, stdout);
            continue;
        }
        if (c == '(' || c == ')' || c == '[' || c == ']') {
            fputs(C_PAREN, stdout);
            fputc(c, stdout);
            fputs(C_RESET, stdout);
            i++;
            continue;
        }
        if (is_sym_start((unsigned char)c)) {
            size_t start = i;
            while (i < len && is_sym_cont((unsigned char)buf[i])) i++;
            char tok[256];
            size_t n = i - start;
            if (n >= sizeof(tok)) n = sizeof(tok) - 1;
            memcpy(tok, buf + start, n);
            tok[n] = 0;
            const char* color = zide_classify(tok, env);
            if (*color) fputs(color, stdout);
            fwrite(buf + start, 1, i - start, stdout);
            if (*color) fputs(C_RESET, stdout);
            continue;
        }
        fputc(c, stdout);
        i++;
    }
}

static void zide_render(const char* prompt, const char* buf, size_t len, size_t pos, Env* env) {
    fputc('\r', stdout);
    fputs(prompt, stdout);
    zide_emit_colored(buf, len, env);
    fputs("\x1b[K", stdout);                  /* clear to EOL */
    if (pos < len) printf("\x1b[%dD", (int)(len - pos));
    fflush(stdout);
}

/* ============================================================
 * Autocompletion
 * ============================================================ */

typedef struct { char** items; int len; int cap; } StrList;

static void sl_add(StrList* l, const char* s) {
    for (int i = 0; i < l->len; i++)
        if (strcmp(l->items[i], s) == 0) return;   /* dedup */
    if (l->len + 1 > l->cap) {
        l->cap = l->cap ? l->cap * 2 : 32;
        l->items = (char**)realloc(l->items, l->cap * sizeof(char*));
    }
    l->items[l->len++] = str_dup(s);
}
static void sl_free(StrList* l) {
    for (int i = 0; i < l->len; i++) free(l->items[i]);
    free(l->items);
    l->items = NULL; l->len = l->cap = 0;
}
static int sl_cmp(const void* a, const void* b) {
    return strcmp(*(const char**)a, *(const char**)b);
}

/* Collect every completion candidate that starts with `prefix`. */
static void zide_candidates(Env* env, const char* prefix, StrList* out) {
    size_t pl = strlen(prefix);
    for (int i = 0; ZIDE_FORMS[i]; i++)
        if (strncmp(ZIDE_FORMS[i], prefix, pl) == 0) sl_add(out, ZIDE_FORMS[i]);
    for (Env* c = env; c; c = c->parent)
        for (size_t i = 0; i < c->vars.len; i++)
            if (strncmp(c->vars.keys[i], prefix, pl) == 0) sl_add(out, c->vars.keys[i]);
    qsort(out->items, out->len, sizeof(char*), sl_cmp);
}

/* Longest common prefix across all candidates. */
static size_t zide_common_prefix_len(StrList* l) {
    if (l->len == 0) return 0;
    size_t n = strlen(l->items[0]);
    for (int i = 1; i < l->len; i++) {
        size_t k = 0;
        while (k < n && l->items[i][k] && l->items[0][k] == l->items[i][k]) k++;
        n = k;
    }
    return n;
}

/* ============================================================
 * Line editor
 * ============================================================ */

#define ZIDE_LINE_MAX 8192

static void zide_insert(char* buf, size_t* len, size_t* pos, const char* s, size_t bufsz) {
    size_t sl = strlen(s);
    if (*len + sl >= bufsz) return;
    memmove(buf + *pos + sl, buf + *pos, *len - *pos + 1);
    memcpy(buf + *pos, s, sl);
    *len += sl;
    *pos += sl;
}

/* Returns 0 = got line, 1 = Ctrl-C (discard), -1 = EOF. */
static int zide_read_line(const char* prompt, char* out, size_t outsz, Env* env) {
    if (!z_isatty() || raw_enable() != 0) {
        fputs(prompt, stdout); fflush(stdout);
        if (!fgets(out, (int)outsz, stdin)) return -1;
        size_t n = strlen(out);
        while (n && (out[n-1] == '\n' || out[n-1] == '\r')) out[--n] = 0;
        return 0;
    }

    size_t len = 0, pos = 0;
    int    hist_pos = g_history.count;
    char   saved[ZIDE_LINE_MAX]; saved[0] = 0;
    out[0] = 0;

    zide_render(prompt, out, len, pos, env);

    while (1) {
        int c = read_key();
        if (c < 0) { raw_disable(); return -1; }

        if (c == '\r' || c == '\n') {
            out[len] = 0;
            fputs("\r\n", stdout); fflush(stdout);
            raw_disable();
            return 0;
        }
        if (c == 3) {                          /* Ctrl-C */
            fputs("^C\r\n", stdout); fflush(stdout);
            out[0] = 0; raw_disable();
            return 1;
        }
        if (c == 4) {                          /* Ctrl-D */
            if (len == 0) { fputs("\r\n", stdout); fflush(stdout); raw_disable(); return -1; }
            if (pos < len) { memmove(out+pos, out+pos+1, len-pos); len--; zide_render(prompt, out, len, pos, env); }
            continue;
        }
        if (c == 127 || c == 8) {              /* Backspace */
            if (pos > 0) {
                memmove(out+pos-1, out+pos, len-pos+1);
                pos--; len--;
                zide_render(prompt, out, len, pos, env);
            }
            continue;
        }
        if (c == 9) {                          /* Tab — autocomplete */
            size_t ws = pos;
            while (ws > 0 && is_sym_cont((unsigned char)out[ws-1])) ws--;
            if (ws == pos) continue;           /* nothing to complete */
            char prefix[256];
            size_t plen = pos - ws;
            if (plen >= sizeof(prefix)) plen = sizeof(prefix)-1;
            memcpy(prefix, out+ws, plen); prefix[plen] = 0;

            StrList cands = {0};
            zide_candidates(env, prefix, &cands);
            if (cands.len == 0) { sl_free(&cands); continue; }

            size_t common = zide_common_prefix_len(&cands);
            if (common > plen) {
                char add[256];
                size_t addn = common - plen;
                if (addn >= sizeof(add)) addn = sizeof(add)-1;
                memcpy(add, cands.items[0] + plen, addn);
                add[addn] = 0;
                zide_insert(out, &len, &pos, add, outsz);
                zide_render(prompt, out, len, pos, env);
            } else if (cands.len > 1) {
                /* Show the options, then redraw. */
                fputs("\r\n", stdout);
                fputs(ZC(C_HINT), stdout);
                for (int i = 0; i < cands.len && i < 40; i++)
                    printf("%s  ", cands.items[i]);
                fputs(ZC(C_RESET), stdout);
                fputs("\r\n", stdout);
                zide_render(prompt, out, len, pos, env);
            }
            sl_free(&cands);
            continue;
        }
        if (c == 1)  { pos = 0;   zide_render(prompt, out, len, pos, env); continue; } /* Ctrl-A */
        if (c == 5)  { pos = len; zide_render(prompt, out, len, pos, env); continue; } /* Ctrl-E */
        if (c == 11) { len = pos; out[len] = 0; zide_render(prompt, out, len, pos, env); continue; } /* Ctrl-K */
        if (c == 12) { fputs("\x1b[H\x1b[2J", stdout); zide_render(prompt, out, len, pos, env); continue; } /* Ctrl-L */

        if (c == 27) {                         /* ESC — arrow keys */
            int s1 = read_key();
            if (s1 != '[' && s1 != 'O') continue;
            int s2 = read_key();
            if (s2 < 0) continue;
            switch (s2) {
                case 'A':  /* up — older history */
                    if (hist_pos > 0) {
                        if (hist_pos == g_history.count) { memcpy(saved, out, len); saved[len] = 0; }
                        hist_pos--;
                        const char* l = hist_get(hist_pos);
                        if (l) { strncpy(out, l, outsz-1); out[outsz-1]=0; len = pos = strlen(out);
                                 zide_render(prompt, out, len, pos, env); }
                    }
                    break;
                case 'B':  /* down — newer history */
                    if (hist_pos < g_history.count) {
                        hist_pos++;
                        if (hist_pos == g_history.count) { strncpy(out, saved, outsz-1); out[outsz-1]=0; }
                        else { const char* l = hist_get(hist_pos); if (l) { strncpy(out, l, outsz-1); out[outsz-1]=0; } }
                        len = pos = strlen(out);
                        zide_render(prompt, out, len, pos, env);
                    }
                    break;
                case 'C': if (pos < len) { pos++; zide_render(prompt, out, len, pos, env); } break;
                case 'D': if (pos > 0)  { pos--; zide_render(prompt, out, len, pos, env); } break;
                case 'H': pos = 0;   zide_render(prompt, out, len, pos, env); break;
                case 'F': pos = len; zide_render(prompt, out, len, pos, env); break;
            }
            continue;
        }

        if ((c >= 32 && c < 127) || (unsigned char)c >= 128) {
            if (len + 1 < outsz) {
                if (pos < len) memmove(out+pos+1, out+pos, len-pos);
                out[pos++] = (char)c;
                len++;
                out[len] = 0;
                zide_render(prompt, out, len, pos, env);
            }
        }
    }
}

/* ============================================================
 * REPL loop
 * ============================================================ */

static void zide_repl(Env* env) {
    char line[ZIDE_LINE_MAX];
    static char acc[65536]; acc[0] = '\0';
    volatile int balance = 0;

    ZIDE_COLOR = z_stdout_is_tty();
    hist_load();

    if (ZIDE_COLOR) {
        printf("%szide %s%s — enhanced REPL for z\n", C_PROMPT, Z_VERSION, C_RESET);
    } else {
        printf("zide %s — enhanced REPL for z\n", Z_VERSION);
    }
    printf("%ssyntax colouring · Tab completes · arrows for history · `help` for the cheat sheet · :q to quit%s\n",
           ZC(C_HINT), ZC(C_RESET));

    while (1) {
        const char* prompt;
        char pbuf[64];
        if (balance > 0) {
            snprintf(pbuf, sizeof(pbuf), "%s...%s ", ZC(C_PROMPT), ZC(C_RESET));
        } else {
            snprintf(pbuf, sizeof(pbuf), "%szide>%s ", ZC(C_PROMPT), ZC(C_RESET));
        }
        prompt = pbuf;

        int rc = zide_read_line(prompt, line, sizeof(line), env);
        if (rc == -1) { fputs("bye!\n", stdout); break; }
        if (rc == 1)  { acc[0] = '\0'; balance = 0; continue; }

        /* trim for command checks */
        char* p = line; while (*p == ' ' || *p == '\t') p++;
        char* end = p + strlen(p);
        while (end > p && (end[-1]==' '||end[-1]=='\t'||end[-1]=='\n'||end[-1]=='\r')) end--;
        size_t tn = end - p;

        if (balance == 0) {
            if ((tn == 2 && strncmp(p, ":q", 2) == 0)
             || (tn == 5 && strncmp(p, ":quit", 5) == 0)) { fputs("bye!\n", stdout); break; }
            if ((tn == 4 && strncmp(p, "help", 4) == 0) || (tn == 1 && *p == '?')) {
                strcpy(line, "(help)");
            }
        }

        for (char* q = line; *q; q++) {
            if (*q == '(') balance++;
            else if (*q == ')') balance--;
        }
        if (strlen(acc) + strlen(line) + 2 >= sizeof(acc)) { acc[0]=0; balance=0; fputs("input too long\n", stderr); continue; }
        strcat(acc, line);
        strcat(acc, "\n");
        if (balance > 0) continue;
        if (balance < 0) { fprintf(stderr, "unbalanced parens\n"); acc[0]=0; balance=0; continue; }

        { size_t n = strlen(acc); char sv = 0; if (n && acc[n-1]=='\n'){ sv=acc[n-1]; acc[n-1]=0; }
          hist_add(acc); if (sv) acc[n-1]=sv; }

        ErrFrame f;
        f.prev = g_err_top; f.value = NULL; f.msg[0] = 0;
        g_err_top = &f;
        if (setjmp(f.buf) == 0) {
            Value* r = run_source(acc, env);
            g_err_top = f.prev;
            if (r && r->type != V_NULL) {
                if (ZIDE_COLOR) fputs(C_HINT, stdout);
                fputs("=> ", stdout);
                if (ZIDE_COLOR) fputs(C_RESET, stdout);
                print_value(stdout, r, 1);
                fputc('\n', stdout);
            }
        } else {
            g_err_top = f.prev;
            fprintf(stderr, "%serror:%s %s\n", ZC("\x1b[31m"), ZC(C_RESET), f.msg);
        }
        acc[0] = '\0';
    }

    hist_save();
}

int main(int argc, char** argv) {
    srand((unsigned)time(NULL));
    g_prog_argc = argc;
    g_prog_argv = argv;
    atexit(raw_disable);
    Env* env = env_new(NULL);
    install_builtins(env);

    if (argc >= 2 && (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-v") == 0)) {
        printf("zide %s\n", Z_VERSION);
        return 0;
    }
    /* `zide file.z` still just runs the file, like z. */
    if (argc >= 2) {
        char* src = read_file_all(argv[1]);
        ErrFrame f;
        f.prev = g_err_top; f.value = NULL; f.msg[0] = 0;
        g_err_top = &f;
        if (setjmp(f.buf) == 0) {
            run_source(src, env);
            g_err_top = f.prev;
        } else {
            g_err_top = f.prev;
            fprintf(stderr, "zide: error: %s\n", f.msg);
            free(src);
            return 1;
        }
        free(src);
        return 0;
    }

    zide_repl(env);
    return 0;
}
