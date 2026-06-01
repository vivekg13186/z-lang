/*
 * zide.c — zide: an enhanced REPL for the z language.
 *
 * Built on top of the z interpreter core (z.c), which it #includes after
 * defining Z_NO_MAIN to suppress z's own main(). Everything the interpreter
 * exposes — eval, the environment, builtins, persistent history, and the
 * raw-mode terminal helpers — is reused directly.
 *
 * UI is patterned after z-console (the raylib GUI), brought to the terminal:
 *
 *   • Jupyter-style numbered cells:  In [N]>   and   Out [N]=
 *   • thin divider line between cells
 *   • live syntax highlighting as you type
 *   • bracket-match highlight when the cursor sits next to ( or )
 *   • inline signature hint as ghost-text after typing `(funcname `
 *   • dropdown autocomplete popup (arrow keys + Tab/Enter to accept, Esc to cancel)
 *   • bracket-aware multi-line input with continuation prompt
 *   • arrow-key history (shared ~/.z_history), `help` / `?`, `:q` to quit
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
#define C_OUTPROMPT "\x1b[1;35m" /* bold magenta — Out [N]= */
#define C_HINT    "\x1b[90m"
#define C_MATCH   "\x1b[7;33m" /* inverse yellow — bracket match */
#define C_DIVIDER "\x1b[38;5;238m"   /* very dark gray — cell divider */
#define C_SELECT  "\x1b[7m"    /* inverse — popup selection */

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

/* ============================================================
 * Signature table — pulled from g_help_topics at startup so we
 * can show ghost-text hints after `(funcname ` is typed.
 * ============================================================ */

typedef struct { char* name; char* signature; char* desc; } SigEntry;
static SigEntry* g_sigs   = NULL;
static int       g_sigs_n = 0;
static int       g_sigs_c = 0;

static void zide_sig_push(const char* name, const char* sig, const char* desc) {
    for (int i = 0; i < g_sigs_n; i++)
        if (strcmp(g_sigs[i].name, name) == 0) return;   /* keep first */
    if (g_sigs_n + 1 > g_sigs_c) {
        g_sigs_c = g_sigs_c ? g_sigs_c * 2 : 64;
        g_sigs = (SigEntry*)realloc(g_sigs, g_sigs_c * sizeof(SigEntry));
    }
    g_sigs[g_sigs_n].name      = str_dup(name);
    g_sigs[g_sigs_n].signature = str_dup(sig);
    g_sigs[g_sigs_n].desc      = str_dup(desc ? desc : "");
    g_sigs_n++;
}

/* Parse one body line of the form:  "  (name args)   description"
 * Whitespace-only or non-paren lines are ignored. */
static void zide_sig_parse_line(const char* line, size_t len) {
    size_t i = 0;
    while (i < len && (line[i] == ' ' || line[i] == '\t')) i++;
    if (i >= len || line[i] != '(') return;
    size_t open = i;
    int depth = 0;
    size_t close = open;
    for (; close < len; close++) {
        if (line[close] == '(') depth++;
        else if (line[close] == ')') { depth--; if (depth == 0) { close++; break; } }
    }
    if (depth != 0) return;
    /* Extract function name = first symbol after '(' */
    size_t ns = open + 1;
    while (ns < close && (line[ns] == ' ')) ns++;
    size_t ne = ns;
    while (ne < close && line[ne] != ' ' && line[ne] != ')') ne++;
    if (ne == ns) return;
    char name[128];
    size_t nlen = ne - ns; if (nlen >= sizeof(name)) nlen = sizeof(name)-1;
    memcpy(name, line + ns, nlen); name[nlen] = 0;
    /* Signature = the whole "(...)" text */
    char sig[256];
    size_t slen = close - open; if (slen >= sizeof(sig)) slen = sizeof(sig)-1;
    memcpy(sig, line + open, slen); sig[slen] = 0;
    /* Description = remainder of the line, trimmed */
    size_t d = close;
    while (d < len && (line[d] == ' ' || line[d] == '\t')) d++;
    char desc[256];
    size_t dlen = len - d; if (dlen >= sizeof(desc)) dlen = sizeof(desc)-1;
    memcpy(desc, line + d, dlen); desc[dlen] = 0;
    /* Trim trailing whitespace from desc */
    while (dlen && (desc[dlen-1] == ' ' || desc[dlen-1] == '\t' || desc[dlen-1] == '\r')) desc[--dlen] = 0;
    zide_sig_push(name, sig, desc);
}

static void zide_build_signatures(void) {
    for (size_t t = 0; t < HELP_TOPIC_COUNT; t++) {
        const char* body = g_help_topics[t].body;
        if (!body) continue;
        size_t p = 0, total = strlen(body);
        while (p < total) {
            size_t q = p;
            while (q < total && body[q] != '\n') q++;
            zide_sig_parse_line(body + p, q - p);
            p = (q < total) ? q + 1 : q;
        }
    }
}

static const SigEntry* zide_sig_find(const char* name) {
    for (int i = 0; i < g_sigs_n; i++)
        if (strcmp(g_sigs[i].name, name) == 0) return &g_sigs[i];
    return NULL;
}

/* ============================================================
 * Bracket-match scan — returns the index of the matching bracket
 * for the bracket at position `idx`, or -1 if not balanced or
 * `idx` doesn't sit on a bracket. Skips brackets inside strings
 * and line comments.
 * ============================================================ */

static int zide_in_string_or_comment(const char* buf, size_t len, size_t at) {
    int in_str = 0;
    for (size_t i = 0; i < at && i < len; i++) {
        char c = buf[i];
        if (in_str) {
            if (c == '\\' && i + 1 < len) { i++; continue; }
            if (c == '"') in_str = 0;
        } else {
            if (c == '"') in_str = 1;
            else if (c == ';') { while (i < at && i < len && buf[i] != '\n') i++; }
        }
    }
    return in_str;
}

static int zide_match_bracket_at(const char* buf, size_t len, size_t idx) {
    if (idx >= len) return -1;
    char c = buf[idx];
    int open_c, close_c, dir;
    if      (c == '(' || c == '[') { open_c = c; close_c = (c == '(' ? ')' : ']'); dir = +1; }
    else if (c == ')' || c == ']') { close_c = c; open_c  = (c == ')' ? '(' : '['); dir = -1; }
    else return -1;
    if (zide_in_string_or_comment(buf, len, idx)) return -1;
    int depth = 0;
    for (size_t i = idx; ; i += (dir > 0 ? 1 : (size_t)-1)) {
        if (dir > 0 && i >= len) return -1;
        if (dir < 0 && i == (size_t)-1) return -1;
        char ch = buf[i];
        if (!zide_in_string_or_comment(buf, len, i)) {
            if (ch == (dir > 0 ? open_c  : close_c)) depth++;
            else if (ch == (dir > 0 ? close_c : open_c )) {
                depth--;
                if (depth == 0) return (int)i;
            }
        }
        if (dir < 0 && i == 0) return -1;
    }
}

/* Given cursor `pos`, choose which bracket (if any) to highlight as
 * the "current" one. Mimics editor behaviour: prefers the bracket
 * just before the cursor, then the one under it. Returns the pair
 * via out args, or both -1 if nothing to highlight. */
static void zide_bracket_pair(const char* buf, size_t len, size_t pos,
                              int* a, int* b) {
    *a = *b = -1;
    if (pos > 0) {
        char c = buf[pos-1];
        if (c == '(' || c == ')' || c == '[' || c == ']') {
            int m = zide_match_bracket_at(buf, len, pos-1);
            if (m >= 0) { *a = (int)(pos-1); *b = m; return; }
        }
    }
    if (pos < len) {
        char c = buf[pos];
        if (c == '(' || c == ')' || c == '[' || c == ']') {
            int m = zide_match_bracket_at(buf, len, pos);
            if (m >= 0) { *a = (int)pos; *b = m; return; }
        }
    }
}

/* ============================================================
 * Coloured emit with optional bracket-pair highlight
 * ============================================================ */

static void zide_emit_colored(const char* buf, size_t len, Env* env, int mb1, int mb2) {
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
            int is_match = ((int)i == mb1 || (int)i == mb2);
            fputs(is_match ? C_MATCH : C_PAREN, stdout);
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

/* ============================================================
 * Inline signature hint detection — when the user has just typed
 * `(name<space>...` and the caret sits inside the arg area but no
 * complete sub-expression yet, surface the signature as ghost text.
 * Returns the SigEntry pointer or NULL.
 * ============================================================ */

static const SigEntry* zide_active_signature(const char* buf, size_t len, size_t pos) {
    /* Walk backwards counting paren depth, look for the open paren of
     * the innermost incomplete call surrounding pos. */
    int depth = 0;
    size_t i = pos;
    while (i > 0) {
        i--;
        if (zide_in_string_or_comment(buf, len, i)) continue;
        char c = buf[i];
        if (c == ')' || c == ']') depth++;
        else if (c == '(' || c == '[') {
            if (depth == 0) {
                /* found enclosing open. Extract name token after it. */
                if (c != '(') return NULL;
                size_t ns = i + 1;
                while (ns < len && buf[ns] == ' ') ns++;
                size_t ne = ns;
                while (ne < len && is_sym_cont((unsigned char)buf[ne])) ne++;
                if (ne == ns) return NULL;
                if (ne >= len || (buf[ne] != ' ' && buf[ne] != '\n' && buf[ne] != ')')) return NULL;
                char name[128];
                size_t nlen = ne - ns; if (nlen >= sizeof(name)) nlen = sizeof(name)-1;
                memcpy(name, buf + ns, nlen); name[nlen] = 0;
                return zide_sig_find(name);
            }
            depth--;
        }
    }
    return NULL;
}

/* ============================================================
 * Renderer — repaints the prompt line with colours, bracket-match
 * highlight, and an optional inline signature hint. Also clears
 * any popup lines that may have been drawn below.
 * ============================================================ */

static int g_popup_lines_drawn = 0;   /* tracks rows printed below prompt */

/* Visible-column width of a UTF-8 string. Counts lead bytes only — UTF-8
 * continuation bytes (10xxxxxx) take zero columns. The terminal advances
 * the cursor by COLUMNS per code point, but snprintf / strlen return
 * BYTES, so without this the cursor move-back math over-counts and the
 * caret ends up too far left whenever the hint contains chars like →. */
static size_t zide_visible_width(const char* s, size_t n) {
    size_t w = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if ((c & 0xC0) != 0x80) w++;
    }
    return w;
}

/* Clear popup rows that were drawn below the prompt line.
 * Uses CSI cursor-down (\x1b[B) instead of '\n' so we never scroll the
 * terminal — scrolling would shift the prompt off its row and desync
 * every subsequent cursor calculation. */
static void zide_clear_popup(void) {
    if (g_popup_lines_drawn <= 0) return;
    for (int i = 0; i < g_popup_lines_drawn; i++) {
        fputs("\x1b[B\r\x1b[2K", stdout);
    }
    printf("\x1b[%dA\r", g_popup_lines_drawn);
    g_popup_lines_drawn = 0;
}

static void zide_render(const char* prompt, const char* buf, size_t len, size_t pos, Env* env) {
    int mb1 = -1, mb2 = -1;
    zide_bracket_pair(buf, len, pos, &mb1, &mb2);

    /* Always go to col 0 first; clear any popup below. */
    fputc('\r', stdout);
    zide_clear_popup();
    fputs(prompt, stdout);
    zide_emit_colored(buf, len, env, mb1, mb2);
    fputs("\x1b[K", stdout);                  /* clear to EOL */

    /* Inline signature hint after the input. */
    size_t hint_visible_len = 0;
    if (ZIDE_COLOR) {
        const SigEntry* s = zide_active_signature(buf, len, pos);
        if (s) {
            char hint[256];
            int n;
            if (s->desc && *s->desc)
                n = snprintf(hint, sizeof(hint), "   %s  %s", s->signature, s->desc);
            else
                n = snprintf(hint, sizeof(hint), "   %s", s->signature);
            if (n > 0) {
                size_t hlen = (size_t)n;
                if (hlen >= sizeof(hint)) hlen = sizeof(hint) - 1;
                fputs(C_HINT, stdout);
                fputs(hint, stdout);
                fputs(C_RESET, stdout);
                hint_visible_len = zide_visible_width(hint, hlen);
            }
        }
    }

    /* Move cursor back from end-of-hint to the caret. Use visible widths
     * (not byte counts) so multi-byte UTF-8 in the input or hint doesn't
     * push the cursor too far left. */
    size_t tail_width = zide_visible_width(buf + pos, len - pos);
    size_t after = tail_width + hint_visible_len;
    if (after > 0) printf("\x1b[%zuD", after);
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
 * Autocomplete popup — opens beneath the prompt, scroll with
 * Up/Down, accept with Tab/Enter, cancel with Esc or any other
 * key (the key is re-injected so backspace, characters, etc.
 * keep flowing). Returns the chosen string (caller owns it via
 * the StrList) or NULL on cancel. `*reinject` is set to a key
 * to feed back into the outer loop, or -1.
 * ============================================================ */

#define POPUP_ROWS 7

/* Draw the popup beneath the current prompt line.
 *
 * Uses CSI cursor-down (\x1b[B) for every vertical move so the terminal
 * never scrolls — scrolling at the screen bottom would shift the prompt
 * off its row and leave the cursor math permanently wrong (which is what
 * was happening when Tab was pressed near the bottom).
 *
 * If the prompt sits at the very bottom of the visible screen, \x1b[B
 * is a no-op and the popup is simply invisible — much better than a
 * scrolled, drifting prompt. */
static void popup_draw(StrList* cands, int sel, int top) {
    int rows = cands->len < POPUP_ROWS ? cands->len : POPUP_ROWS;
    for (int i = 0; i < rows; i++) {
        fputs("\x1b[B\r\x1b[2K", stdout);
        int idx = top + i;
        if (idx >= cands->len) continue;
        const char* name = cands->items[idx];
        const SigEntry* s = zide_sig_find(name);
        int is_sel = (idx == sel);
        if (is_sel) fputs(ZC(C_SELECT), stdout);
        printf("  %-18s ", name);
        if (s) {
            fputs(is_sel ? "" : ZC(C_HINT), stdout);
            if (s->desc && *s->desc) printf("%s  %s", s->signature, s->desc);
            else                     fputs(s->signature, stdout);
            if (!is_sel) fputs(ZC(C_RESET), stdout);
        }
        if (is_sel) fputs(ZC(C_RESET), stdout);
    }
    /* Return to the prompt line at col 0. The outer caller (zide_render)
     * will reposition the cursor at the caret afterwards. */
    printf("\x1b[%dA\r", rows);
    fflush(stdout);
    g_popup_lines_drawn = rows;
}

/* Returns selected item index, or -1 on cancel.
 * If a key cancelled by being a normal input (not Esc/arrows), it's
 * placed in *reinject so the outer loop can act on it. */
static int popup_run(StrList* cands, const char* prompt, const char* buf,
                     size_t len, size_t pos, Env* env, int* reinject) {
    int sel = 0, top = 0;
    *reinject = -1;
    popup_draw(cands, sel, top);
    while (1) {
        int c = read_key();
        if (c < 0) { zide_clear_popup(); return -1; }
        if (c == 27) {
            int s1 = read_key();
            if (s1 != '[' && s1 != 'O') { zide_clear_popup(); return -1; }
            int s2 = read_key();
            if (s2 < 0) { zide_clear_popup(); return -1; }
            if (s2 == 'A') { /* up */
                if (sel > 0) sel--;
                if (sel < top) top = sel;
                popup_draw(cands, sel, top);
                continue;
            }
            if (s2 == 'B') { /* down */
                if (sel < cands->len - 1) sel++;
                if (sel >= top + POPUP_ROWS) top = sel - POPUP_ROWS + 1;
                popup_draw(cands, sel, top);
                continue;
            }
            /* other arrow keys cancel */
            zide_clear_popup();
            zide_render(prompt, buf, len, pos, env);
            return -1;
        }
        if (c == 9 || c == '\r' || c == '\n') {   /* accept */
            zide_clear_popup();
            return sel;
        }
        /* anything else: cancel, re-inject character for the outer editor. */
        zide_clear_popup();
        *reinject = c;
        return -1;
    }
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

/* Pump one key value `c` through the editor state. Mostly the body of the
 * old main key loop, factored out so popup_run can feed a cancelled key
 * back in without recursion. */
static int zide_dispatch_key(int c, const char* prompt, char* out, size_t outsz,
                             size_t* plen, size_t* ppos, Env* env,
                             int* phist_pos, char* saved);

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

        int rc = zide_dispatch_key(c, prompt, out, outsz, &len, &pos, env, &hist_pos, saved);
        if (rc == 0) continue;             /* keep editing */
        if (rc == 10) {                    /* got line */
            out[len] = 0;
            fputs("\r\n", stdout); fflush(stdout);
            raw_disable();
            return 0;
        }
        if (rc == 11) {                    /* Ctrl-C */
            fputs("^C\r\n", stdout); fflush(stdout);
            out[0] = 0; raw_disable();
            return 1;
        }
        if (rc == -1) { raw_disable(); return -1; }
    }
}

/* Returns:
 *    0 = handled, keep editing
 *   10 = newline (commit line)
 *   11 = Ctrl-C
 *   -1 = EOF
 */
static int zide_dispatch_key(int c, const char* prompt, char* out, size_t outsz,
                             size_t* plen, size_t* ppos, Env* env,
                             int* phist_pos, char* saved) {
    size_t len = *plen, pos = *ppos;
    int hist_pos = *phist_pos;

    if (c == '\r' || c == '\n') { *plen = len; *ppos = pos; return 10; }
    if (c == 3)  { return 11; }            /* Ctrl-C */
    if (c == 4) {                          /* Ctrl-D */
        if (len == 0) { fputs("\r\n", stdout); fflush(stdout); return -1; }
        if (pos < len) { memmove(out+pos, out+pos+1, len-pos); len--; }
        goto render;
    }
    if (c == 127 || c == 8) {              /* Backspace */
        if (pos > 0) {
            memmove(out+pos-1, out+pos, len-pos+1);
            pos--; len--;
        }
        goto render;
    }
    if (c == 9) {                          /* Tab — popup completion */
        /* Find the bounds of the symbol the caret is sitting in. We
         * REPLACE the whole word, not just the part before the cursor —
         * otherwise pressing Tab in the middle of "prnt" with the caret
         * after "pr" inserts the completion BEFORE the trailing "nt"
         * and leaves the cursor in the middle of the joined word. */
        size_t ws = pos, we = pos;
        while (ws > 0 && is_sym_cont((unsigned char)out[ws-1])) ws--;
        while (we < len && is_sym_cont((unsigned char)out[we]))   we++;
        if (ws == we) goto render;       /* not on a symbol */

        char prefix[256];
        size_t pl = pos - ws;            /* chars to the LEFT of caret */
        if (pl >= sizeof(prefix)) pl = sizeof(prefix)-1;
        memcpy(prefix, out+ws, pl); prefix[pl] = 0;

        StrList cands = {0};
        zide_candidates(env, prefix, &cands);
        if (cands.len == 0) { sl_free(&cands); goto render; }

        /* Helper: replace out[ws..we) with `full`, leave caret right after it. */
        #define ZIDE_REPLACE_WORD(FULL) do {                                  \
            const char* _full = (FULL);                                       \
            size_t _flen = strlen(_full);                                     \
            size_t _word_len = we - ws;                                       \
            if (len - _word_len + _flen + 1 < outsz) {                        \
                /* shift the tail to make room for the new word */            \
                memmove(out + ws + _flen, out + we, len - we + 1);            \
                memcpy(out + ws, _full, _flen);                               \
                len = len - _word_len + _flen;                                \
                pos = ws + _flen;                                             \
                we  = ws + _flen;                                             \
            }                                                                 \
        } while (0)

        /* If exactly one candidate, just complete and we're done. */
        if (cands.len == 1) {
            ZIDE_REPLACE_WORD(cands.items[0]);
            sl_free(&cands);
            goto render;
        }
        /* Otherwise, first extend to the common prefix then open popup. */
        size_t common = zide_common_prefix_len(&cands);
        if (common > pl) {
            char common_buf[256];
            if (common >= sizeof(common_buf)) common = sizeof(common_buf)-1;
            memcpy(common_buf, cands.items[0], common); common_buf[common] = 0;
            ZIDE_REPLACE_WORD(common_buf);
            *plen = len; *ppos = pos;
            zide_render(prompt, out, len, pos, env);
        }
        /* Open the popup. */
        int reinject = -1;
        int sel = popup_run(&cands, prompt, out, len, pos, env, &reinject);
        if (sel >= 0) {
            ZIDE_REPLACE_WORD(cands.items[sel]);
        }
        sl_free(&cands);
        #undef ZIDE_REPLACE_WORD
        *plen = len; *ppos = pos;
        zide_render(prompt, out, len, pos, env);
        if (reinject >= 0) {
            return zide_dispatch_key(reinject, prompt, out, outsz, plen, ppos, env, phist_pos, saved);
        }
        return 0;
    }
    if (c == 1)  { pos = 0;   goto render; } /* Ctrl-A */
    if (c == 5)  { pos = len; goto render; } /* Ctrl-E */
    if (c == 11) { len = pos; out[len] = 0; goto render; } /* Ctrl-K */
    if (c == 12) { fputs("\x1b[H\x1b[2J", stdout); goto render; } /* Ctrl-L */

    if (c == 27) {                         /* ESC — arrow keys */
        int s1 = read_key();
        if (s1 != '[' && s1 != 'O') goto render;
        int s2 = read_key();
        if (s2 < 0) goto render;
        switch (s2) {
            case 'A':  /* up — older history */
                if (hist_pos > 0) {
                    if (hist_pos == g_history.count) { memcpy(saved, out, len); saved[len] = 0; }
                    hist_pos--;
                    const char* l = hist_get(hist_pos);
                    if (l) { strncpy(out, l, outsz-1); out[outsz-1]=0; len = pos = strlen(out); }
                }
                break;
            case 'B':  /* down — newer history */
                if (hist_pos < g_history.count) {
                    hist_pos++;
                    if (hist_pos == g_history.count) { strncpy(out, saved, outsz-1); out[outsz-1]=0; }
                    else { const char* l = hist_get(hist_pos); if (l) { strncpy(out, l, outsz-1); out[outsz-1]=0; } }
                    len = pos = strlen(out);
                }
                break;
            case 'C': if (pos < len) pos++; break;
            case 'D': if (pos > 0)   pos--; break;
            case 'H': pos = 0;   break;
            case 'F': pos = len; break;
        }
        goto render;
    }

    if ((c >= 32 && c < 127) || (unsigned char)c >= 128) {
        if (len + 1 < outsz) {
            if (pos < len) memmove(out+pos+1, out+pos, len-pos);
            out[pos++] = (char)c;
            len++;
            out[len] = 0;
        }
    }

render:
    *plen = len; *ppos = pos; *phist_pos = hist_pos;
    zide_render(prompt, out, len, pos, env);
    return 0;
}

/* ============================================================
 * REPL loop — Jupyter-style numbered cells with dividers.
 * ============================================================ */

/* ============================================================
 * Session cells — keep the source of each committed cell so the
 * user can :save them all to a .z file (same idea as Ctrl+S in
 * z-console).
 * ============================================================ */

static char** g_cells   = NULL;
static int    g_cells_n = 0;
static int    g_cells_c = 0;

static void zide_cells_push(const char* src) {
    if (g_cells_n + 1 > g_cells_c) {
        g_cells_c = g_cells_c ? g_cells_c * 2 : 32;
        g_cells = (char**)realloc(g_cells, g_cells_c * sizeof(char*));
    }
    g_cells[g_cells_n++] = str_dup(src ? src : "");
}

/* Write all collected cells to `path` as a .z file. `path` may be NULL
 * or empty — in that case a timestamped default in $HOME is used and the
 * resolved path is copied into `out_path` for display. Returns 0 on
 * success, non-zero on error (with an errno-style message in errbuf). */
static int zide_save_cells(const char* path, char* out_path, size_t out_sz,
                           char* errbuf, size_t errsz) {
    char buf[1024];
    if (!path || !*path) {
        time_t t = time(NULL); struct tm tmv; localtime_r(&t, &tmv);
        const char* home = getenv("HOME");
        snprintf(buf, sizeof(buf),
                 "%s/.zide_session_%04d%02d%02d_%02d%02d%02d.z",
                 home ? home : ".",
                 tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                 tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
        path = buf;
    }
    FILE* f = fopen(path, "w");
    if (!f) {
        int e = errno;
        snprintf(errbuf, errsz, "%.300s: %s", path, strerror(e));
        return -1;
    }
    time_t now = time(NULL);
    fprintf(f, "; zide session — %d cell%s, saved %s",
            g_cells_n, g_cells_n == 1 ? "" : "s", ctime(&now));
    for (int i = 0; i < g_cells_n; i++) {
        const char* src = g_cells[i];
        fputs(src, f);
        /* Ensure a blank line separates cells. */
        size_t L = strlen(src);
        if (L == 0 || src[L-1] != '\n') fputc('\n', f);
        fputc('\n', f);
    }
    fclose(f);
    if (out_path && out_sz) { strncpy(out_path, path, out_sz - 1); out_path[out_sz - 1] = 0; }
    return 0;
}

/* Print a thin horizontal rule that spans some sensible width. */
static void zide_divider(void) {
    if (!ZIDE_COLOR) { fputs("\n", stdout); return; }
    int width = 64;
    const char* env_cols = getenv("COLUMNS");
    if (env_cols && *env_cols) { int v = atoi(env_cols); if (v > 20 && v < 200) width = v; }
    fputs(C_DIVIDER, stdout);
    for (int i = 0; i < width; i++) fputc('-', stdout);
    fputs(C_RESET, stdout);
    fputc('\n', stdout);
}

static void zide_repl(Env* env) {
    char line[ZIDE_LINE_MAX];
    static char acc[65536]; acc[0] = '\0';
    volatile int balance = 0;
    volatile int cell = 1;

    ZIDE_COLOR = z_stdout_is_tty();
    hist_load();
    zide_build_signatures();

    if (ZIDE_COLOR) {
        printf("%szide %s%s — enhanced REPL for z\n", C_PROMPT, Z_VERSION, C_RESET);
    } else {
        printf("zide %s — enhanced REPL for z\n", Z_VERSION);
    }
    printf("%snumbered cells · bracket match · Tab popup · :save [path] writes cells to .z · :q to quit%s\n",
           ZC(C_HINT), ZC(C_RESET));

    while (1) {
        const char* prompt;
        char pbuf[64];
        if (balance > 0) {
            /* Continuation prompt — visually aligned with the cell prompt. */
            snprintf(pbuf, sizeof(pbuf), "%s   ...   %s  ", ZC(C_PROMPT), ZC(C_RESET));
        } else {
            zide_divider();
            snprintf(pbuf, sizeof(pbuf), "%sIn [%d]>%s ", ZC(C_PROMPT), cell, ZC(C_RESET));
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
            /* :save  or  :save path/to/file.z — dump every committed cell. */
            if (tn >= 5 && strncmp(p, ":save", 5) == 0
                && (tn == 5 || p[5] == ' ' || p[5] == '\t')) {
                char path_arg[1024]; path_arg[0] = 0;
                if (tn > 5) {
                    const char* a = p + 5;
                    while (a < end && (*a == ' ' || *a == '\t')) a++;
                    size_t alen = end - a;
                    if (alen >= sizeof(path_arg)) alen = sizeof(path_arg) - 1;
                    memcpy(path_arg, a, alen); path_arg[alen] = 0;
                }
                char resolved[1024], errbuf[512];
                if (zide_save_cells(path_arg, resolved, sizeof(resolved),
                                    errbuf, sizeof(errbuf)) == 0) {
                    const char* plural = g_cells_n == 1 ? "" : "s";
                    if (ZIDE_COLOR) printf("%s; saved %d cell%s → %s%s\n",
                                           C_HINT, g_cells_n, plural, resolved, C_RESET);
                    else            printf("; saved %d cell%s -> %s\n",
                                           g_cells_n, plural, resolved);
                } else {
                    fprintf(stderr, "%ssave failed:%s %s\n",
                            ZC("\x1b[31m"), ZC(C_RESET), errbuf);
                }
                acc[0] = '\0'; balance = 0;
                continue;
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
          hist_add(acc); zide_cells_push(acc); if (sv) acc[n-1]=sv; }

        ErrFrame f;
        f.prev = g_err_top; f.value = NULL; f.msg[0] = 0;
        g_err_top = &f;
        if (setjmp(f.buf) == 0) {
            Value* r = run_source(acc, env);
            g_err_top = f.prev;
            if (r && r->type != V_NULL) {
                if (ZIDE_COLOR) printf("%sOut [%d]=%s ", C_OUTPROMPT, cell, C_RESET);
                else            printf("Out [%d]= ", cell);
                print_value(stdout, r, 1);
                fputc('\n', stdout);
            }
        } else {
            g_err_top = f.prev;
            fprintf(stderr, "%serror:%s %s\n", ZC("\x1b[31m"), ZC(C_RESET), f.msg);
        }
        acc[0] = '\0';
        cell++;
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
