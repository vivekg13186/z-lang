/*
 * z-console.c — raylib-based GUI REPL for the z language.
 *
 * A console-style window with a typing prompt at the bottom and a scrolling
 * output area above. Each evaluation creates a "cell": the input echo, any
 * text output, and a canvas for shapes/text drawn via the ui:* builtins.
 *
 * Build (needs raylib installed):
 *
 *   macOS:    brew install raylib  &&  make
 *   Linux:    sudo apt-get install libraylib-dev  &&  make
 *   Windows:  install raylib via vcpkg, then make
 *
 * Place this folder next to z.c (sibling); the Makefile includes ../z.c.
 *
 * Builtins added on top of the standard z stdlib (current cell unless noted):
 *
 *   (ui:text   "line")                       append a line of text
 *   (ui:circle cx cy r [color])              filled circle
 *   (ui:rect   x  y  w  h  [color])          filled rectangle
 *   (ui:line   x1 y1 x2 y2 [color])          straight line
 *   (ui:text-at x y "msg" [size] [color])    text at an absolute position
 *   (ui:clear)                               wipe current cell's drawings
 *
 * Colours accept named ("red","green","blue","yellow","orange","purple",
 * "pink","white","black","gray","darkgray","lightgray","magenta","maroon",
 * "violet") or hex "#RRGGBB".
 *
 * `print` is intercepted so existing programs work — output shows up in
 * the cell instead of (or in addition to) stdout.
 */

#define Z_NO_MAIN
#include "../z.c"

#include "raylib.h"
#include <limits.h>

/* ============================================================
 * Font loading — JetBrains Mono (default) with graceful fallback chain.
 *
 * JetBrains Mono is OFL-licensed and **embedded directly into the
 * binary** at build time via tools/embed_font.py, so a freshly-built
 * z-console needs zero external font files. The fallback chain below
 * still exists so users can override with their own TTF, or so builds
 * that skipped the embed step (e.g. size-conscious targets) still
 * resolve to a system font.
 *
 * Order of attempts:
 *   1. $Z_CONSOLE_FONT                  (explicit override, any TTF path)
 *   2. EMBEDDED JetBrains Mono bytes    (zc_font_embed.h; ~274 KB inline)
 *   3. Bundled font next to binary / cwd  (JetBrainsMono-Regular.ttf, then
 *                                          legacy Menlo-Regular.ttf)
 *   4. Common system locations
 *   5. GetFontDefault()                 (raylib's built-in pixel font)
 *
 * Loaded once at size 64 with the full ASCII glyph set so any FS_* size
 * used by the UI scales cleanly via DrawTextEx.
 * ============================================================ */

#include "zc_font_embed.h"   /* defines zc_jbm_font[] and zc_jbm_font_len  */

static const char* ZC_FONT_CANDIDATES[] = {
    /* --- JetBrains Mono (default) --- */
    /* macOS — Homebrew cask `font-jetbrains-mono` drops files under
     * /Library/Fonts/ or ~/Library/Fonts/. */
    "/Library/Fonts/JetBrainsMono-Regular.ttf",
    "/Library/Fonts/JetBrainsMonoNL-Regular.ttf",
    /* Debian/Ubuntu (apt: fonts-jetbrains-mono). */
    "/usr/share/fonts/truetype/jetbrains-mono/JetBrainsMono-Regular.ttf",
    "/usr/share/fonts/JetBrainsMono/JetBrainsMono-Regular.ttf",
    /* Fedora/Arch and generic locations. */
    "/usr/share/fonts/TTF/JetBrainsMono-Regular.ttf",
    "/usr/local/share/fonts/JetBrainsMono-Regular.ttf",
    /* Windows. */
    "C:/Windows/Fonts/JetBrainsMono-Regular.ttf",

    /* --- Menlo (legacy fallback; many of these fail to parse via raylib's
     * stb_truetype because the cmap table is Mac-only — kept anyway so
     * users who already had a working Menlo extraction keep working). --- */
    "/System/Library/Fonts/Menlo.ttc",
    "/Library/Fonts/Menlo-Regular.ttf",
    "/Library/Fonts/Menlo.ttf",
    "/usr/share/fonts/truetype/Menlo-Regular.ttf",
    "/usr/local/share/fonts/Menlo-Regular.ttf",
    "C:/Windows/Fonts/Menlo-Regular.ttf",
    NULL
};

/* Try a path under $HOME as well. */
static int z_path_exists(const char* p) {
    FILE* f = fopen(p, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

static Font g_console_font;
static int  g_console_font_loaded = 0;

/* Try to load a font from `path`; on success, install it and return 1. */
/* Try to load a font from `path`. Returns 1 on success; on failure prints
 * a clear stderr line explaining what raylib actually did (file-loaded vs
 * parse-failed), so the user isn't left wondering why a "loaded
 * successfully" FILEIO log line still ends with a system-font fallback.
 *
 * If the extension-based LoadFontEx path fails (raylib's stb_truetype
 * wrapper can be picky about Mac-flavoured TTFs where the first SFNT
 * table is something nonstandard like FontForge's "FFTM"), we retry via
 * LoadFontFromMemory with an explicit ".ttf" hint — that bypasses the
 * extension sniff and feeds stb_truetype the bytes directly. */
static int z_try_load_font(const char* path, int base_size) {
    if (!path || !*path) return 0;
    if (!z_path_exists(path)) return 0;

    Font f = LoadFontEx(path, base_size, NULL, 0);
    if (f.texture.id) {
        g_console_font = f;
        g_console_font_loaded = 1;
        SetTextureFilter(g_console_font.texture, TEXTURE_FILTER_BILINEAR);
        return 1;
    }

    /* LoadFontEx failed despite the file existing on disk — try the
     * in-memory loader with an explicit extension. */
    int data_size = 0;
    unsigned char* data = LoadFileData(path, &data_size);
    if (data && data_size > 0) {
        f = LoadFontFromMemory(".ttf", data, data_size, base_size, NULL, 0);
        UnloadFileData(data);
        if (f.texture.id) {
            g_console_font = f;
            g_console_font_loaded = 1;
            SetTextureFilter(g_console_font.texture, TEXTURE_FILTER_BILINEAR);
            return 1;
        }
    }

    fprintf(stderr,
        "z-console: '%s' loaded off disk but raylib's stb_truetype "
        "couldn't parse it.\n"
        "  Most common cause: the font's cmap table only has Macintosh\n"
        "  platform encodings; stb_truetype only reads Microsoft/Unicode\n"
        "  cmaps. Fix on macOS by extracting a clean copy from system Menlo:\n"
        "      pip install fonttools\n"
        "      python3 tools/extract_menlo.py %s\n"
        "  On Linux/Windows, substitute any other monospace TTF\n"
        "  (JetBrains Mono, Fira Code, DejaVu Sans Mono) — all have\n"
        "  proper Unicode cmaps.\n"
        "  Override at runtime with $Z_CONSOLE_FONT=/path/to/other.ttf\n",
        path, path);
    return 0;
}

/* Strip the trailing path component from `argv0` so we can look for a
 * bundled JetBrainsMono-Regular.ttf next to the executable. */
static void z_dirname(const char* argv0, char* out, size_t out_sz) {
    if (!argv0 || !*argv0) { snprintf(out, out_sz, "."); return; }
    const char* slash = NULL;
    for (const char* p = argv0; *p; p++)
        if (*p == '/' || *p == '\\') slash = p;
    if (!slash) { snprintf(out, out_sz, "."); return; }
    size_t n = (size_t)(slash - argv0);
    if (n >= out_sz) n = out_sz - 1;
    memcpy(out, argv0, n);
    out[n] = 0;
}

static void load_console_font(const char* argv0) {
    const int BASE_SIZE = 64;   /* scaled down by DrawTextEx for any FS_* */

    /* 1. Explicit override. */
    const char* override = getenv("Z_CONSOLE_FONT");
    if (override && *override) {
        if (z_try_load_font(override, BASE_SIZE)) return;
        fprintf(stderr, "z-console: could not load font from $Z_CONSOLE_FONT='%s'\n",
                override);
    }

    /* 2. Embedded JetBrains Mono — the bytes were baked into the binary
     * by tools/embed_font.py at build time. zc_jbm_font_len is 0 if the
     * embed step was skipped (e.g. someone built without the .ttf), in
     * which case we silently fall through to file-based lookup. */
    if (zc_jbm_font_len > 0) {
        Font f = LoadFontFromMemory(".ttf",
                                    (unsigned char*)zc_jbm_font,
                                    (int)zc_jbm_font_len,
                                    BASE_SIZE, NULL, 0);
        if (f.texture.id) {
            g_console_font = f;
            g_console_font_loaded = 1;
            SetTextureFilter(g_console_font.texture, TEXTURE_FILTER_BILINEAR);
            return;
        }
    }

    /* 3. Bundled font next to the binary or in the current working dir. */
    const char* bundled[] = {
        "JetBrainsMono-Regular.ttf", "JetBrainsMonoNL-Regular.ttf",
        "Menlo-Regular.ttf", "Menlo.ttf",   /* legacy fallback */
        NULL
    };
    for (int i = 0; bundled[i]; i++) {
        if (z_try_load_font(bundled[i], BASE_SIZE)) return;
    }
    char bindir[1024];
    z_dirname(argv0, bindir, sizeof(bindir));
    for (int i = 0; bundled[i]; i++) {
        char path[1280];
        snprintf(path, sizeof(path), "%s/%s", bindir, bundled[i]);
        if (z_try_load_font(path, BASE_SIZE)) return;
    }
    /* 3. System paths. */
    for (int i = 0; ZC_FONT_CANDIDATES[i]; i++) {
        if (!z_path_exists(ZC_FONT_CANDIDATES[i])) continue;
        Font f = LoadFontEx(ZC_FONT_CANDIDATES[i], BASE_SIZE, NULL, 0);
        if (f.texture.id) {
            g_console_font = f;
            g_console_font_loaded = 1;
            SetTextureFilter(g_console_font.texture, TEXTURE_FILTER_BILINEAR);
            return;
        }
    }
    /* ~/Library/Fonts on macOS, ~/.local/share/fonts on Linux. */
    const char* home = getenv("HOME");
    if (home) {
        char path[1024];
        const char* user_paths[] = {
            "/Library/Fonts/JetBrainsMono-Regular.ttf",
            "/.local/share/fonts/JetBrainsMono-Regular.ttf",
            "/.fonts/JetBrainsMono-Regular.ttf",
            "/Library/Fonts/Menlo-Regular.ttf",     /* legacy */
            "/.local/share/fonts/Menlo-Regular.ttf",
            "/.fonts/Menlo-Regular.ttf",
            NULL
        };
        for (int i = 0; user_paths[i]; i++) {
            snprintf(path, sizeof(path), "%s%s", home, user_paths[i]);
            if (!z_path_exists(path)) continue;
            Font f = LoadFontEx(path, BASE_SIZE, NULL, 0);
            if (f.texture.id) {
                g_console_font = f;
                g_console_font_loaded = 1;
                SetTextureFilter(g_console_font.texture, TEXTURE_FILTER_BILINEAR);
                return;
            }
        }
    }
    /* Fallback — raylib's pixel font. */
    g_console_font = GetFontDefault();
    g_console_font_loaded = 0;
    fprintf(stderr,
            "z-console: no usable TTF found, using raylib's default font.\n"
            "  Quickest fix:\n"
            "    macOS: brew install --cask font-jetbrains-mono\n"
            "    apt:   sudo apt-get install fonts-jetbrains-mono\n"
            "    or download from https://www.jetbrains.com/lp/mono/ and\n"
            "    drop JetBrainsMono-Regular.ttf next to the z-console binary.\n"
            "  Override at runtime with $Z_CONSOLE_FONT=/path/to/some.ttf\n");
}

/* ============================================================
 * Persisted state — theme, font size, window size between sessions.
 * Plain-text key=value file at $HOME/.z_console_state.
 * ============================================================ */

typedef struct {
    int theme_idx;
    int font_size;
    int window_w, window_h;
} ZcState;

static void zc_state_path(char* out, size_t out_sz) {
    const char* home = getenv("HOME");
    snprintf(out, out_sz, "%s/.z_console_state", home ? home : ".");
}

static int zc_state_load(ZcState* s) {
    s->theme_idx = 0;
    s->font_size = 18;
    s->window_w  = 900;
    s->window_h  = 640;
    char path[512]; zc_state_path(path, sizeof(path));
    FILE* f = fopen(path, "r");
    if (!f) return 0;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char key[64]; int val;
        if (sscanf(line, "%63[^=]=%d", key, &val) == 2) {
            if      (!strcmp(key, "theme"))    s->theme_idx = val;
            else if (!strcmp(key, "font"))     s->font_size = val;
            else if (!strcmp(key, "window_w")) s->window_w  = val;
            else if (!strcmp(key, "window_h")) s->window_h  = val;
        }
    }
    fclose(f);
    return 1;
}

static void zc_state_save(const ZcState* s) {
    char path[512]; zc_state_path(path, sizeof(path));
    FILE* f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "; z-console persisted state\n");
    fprintf(f, "theme=%d\n",    s->theme_idx);
    fprintf(f, "font=%d\n",     s->font_size);
    fprintf(f, "window_w=%d\n", s->window_w);
    fprintf(f, "window_h=%d\n", s->window_h);
    fclose(f);
}

/* Cooperative interrupt hook called by z.c's eval() every ~4096 calls.
 * Pumps raylib events (so keypresses aren't queued indefinitely) and sets
 * z_eval_interrupted when Ctrl+. is detected. Cleared by z.c the next time
 * eval() runs after the long expression aborts. */
extern volatile int z_eval_interrupted;
static void zc_eval_tick(void) {
    PollInputEvents();
    int ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_LEFT_SUPER)
            || IsKeyDown(KEY_RIGHT_CONTROL);
    if (ctrl && IsKeyDown(KEY_PERIOD)) z_eval_interrupted = 1;
}

/* Convenience wrappers so the rest of the file reads naturally. */
static void zc_draw_text(const char* text, int x, int y, int size, Color color) {
    /* spacing argument: 0 keeps natural kerning from the font atlas. */
    DrawTextEx(g_console_font, text ? text : "",
               (Vector2){ (float)x, (float)y },
               (float)size, 0.0f, color);
}
static float zc_measure_text_f(const char* text, int size) {
    Vector2 v = MeasureTextEx(g_console_font, text ? text : "",
                              (float)size, 0.0f);
    return v.x;
}
static int zc_measure_text(const char* text, int size) {
    /* Most call-sites are layout-only (popup widths, status bar centring),
     * where 1-px rounding is invisible. The per-glyph drawing path uses the
     * float variant directly to avoid accumulated truncation drift. */
    return (int)(zc_measure_text_f(text, size) + 0.5f);
}
/* Length-bounded measure for sub-strings without a null terminator. */
static float zc_measure_text_n_f(const char* s, int n, int size) {
    char buf[1024];
    if (n < 0) n = 0;
    if (n >= (int)sizeof(buf)) n = (int)sizeof(buf) - 1;
    memcpy(buf, s, n); buf[n] = 0;
    return zc_measure_text_f(buf, size);
}
static int zc_measure_text_n(const char* s, int n, int size) {
    return (int)(zc_measure_text_n_f(s, n, size) + 0.5f);
}

/* ============================================================
 * Syntax highlighting palette + tokenizer for the prompt + cell echo.
 *
 * We do a token-at-a-time pass instead of pre-tokenizing the whole buffer
 * — the input is short and we re-render every frame, so a single linear
 * scan is fine.
 * ============================================================ */

typedef struct { Color comment, string, interp, number, kw, builtin, paren, lit, normal, bracket_match; } ZcSyntaxColors;

/* ============================================================
 * Theme — a complete colour set, switchable at runtime (Ctrl+T) and
 * persisted across sessions in ~/.z_console_state.
 * ============================================================ */

typedef struct {
    const char* name;
    Color bg, input_bg, prompt_fg, input_echo, text_fg, err_fg;
    Color border, canvas_bg, canvas_line, status_bg, status_fg;
    Color popup_bg, popup_sel_bg, popup_fg, popup_dim_fg, popup_border, popup_match;
    ZcSyntaxColors syntax;
} ZcTheme;

static const ZcTheme ZC_THEMES[] = {
    /* --- dark (default) --- */
    {
        .name        = "dark",
        .bg          = (Color){ 15, 17, 21, 255},
        .input_bg    = (Color){ 26, 28, 34, 255},
        .prompt_fg   = (Color){136,255,170, 255},
        .input_echo  = (Color){136,170,255, 255},
        .text_fg     = (Color){230,230,230, 255},
        .err_fg      = (Color){255,100,100, 255},
        .border      = (Color){ 60, 60, 60, 255},
        .canvas_bg   = (Color){ 0, 0, 0, 0 },   /* transparent */
        .canvas_line = (Color){ 0, 0, 0, 0 },   /* transparent */
        .status_bg   = (Color){ 20, 22, 28, 255},
        .status_fg   = (Color){150,150,170, 255},
        .popup_bg    = (Color){ 34, 36, 46, 245},
        .popup_sel_bg= (Color){ 70, 90,140, 255},
        .popup_fg    = (Color){220,220,230, 255},
        .popup_dim_fg= (Color){150,150,170, 255},
        .popup_border= (Color){ 80, 80,100, 255},
        .popup_match = (Color){250,210,120, 255},
        .syntax      = {
            .comment       = (Color){120,120,140, 255},
            .string        = (Color){180,210,140, 255},
            .interp        = (Color){250,170,120, 255},
            .number        = (Color){200,180,255, 255},
            .kw            = (Color){200,160,255, 255},
            .builtin       = (Color){120,200,255, 255},
            .paren         = (Color){180,180,180, 255},
            .lit           = (Color){250,200,140, 255},
            .normal        = (Color){230,230,230, 255},
            .bracket_match = (Color){255,230,100, 255},
        },
    },
    /* --- light --- */
    {
        .name        = "light",
        .bg          = (Color){250,250,248, 255},
        .input_bg    = (Color){235,235,232, 255},
        .prompt_fg   = (Color){ 20,120, 60, 255},
        .input_echo  = (Color){ 60, 90,170, 255},
        .text_fg     = (Color){ 30, 30, 30, 255},
        .err_fg      = (Color){200, 40, 40, 255},
        .border      = (Color){200,200,200, 255},
        .canvas_bg   = (Color){ 0, 0, 0, 0 },   /* transparent */
        .canvas_line = (Color){ 0, 0, 0, 0 },   /* transparent */
        .status_bg   = (Color){240,240,236, 255},
        .status_fg   = (Color){100,100,110, 255},
        .popup_bg    = (Color){255,255,250, 245},
        .popup_sel_bg= (Color){180,210,250, 255},
        .popup_fg    = (Color){ 30, 30, 40, 255},
        .popup_dim_fg= (Color){120,120,130, 255},
        .popup_border= (Color){180,180,180, 255},
        .popup_match = (Color){180,110, 40, 255},
        .syntax      = {
            .comment       = (Color){130,130,130, 255},
            .string        = (Color){ 50,130, 50, 255},
            .interp        = (Color){180, 80, 40, 255},
            .number        = (Color){ 90, 60,180, 255},
            .kw            = (Color){140, 40,160, 255},
            .builtin       = (Color){ 30, 90,180, 255},
            .paren         = (Color){120,120,120, 255},
            .lit           = (Color){180,110, 40, 255},
            .normal        = (Color){ 30, 30, 30, 255},
            .bracket_match = (Color){220,140,  0, 255},
        },
    },
    /* --- high-contrast --- */
    {
        .name        = "high-contrast",
        .bg          = (Color){  0,  0,  0, 255},
        .input_bg    = (Color){  0,  0,  0, 255},
        .prompt_fg   = (Color){  0,255,  0, 255},
        .input_echo  = (Color){255,255,  0, 255},
        .text_fg     = (Color){255,255,255, 255},
        .err_fg      = (Color){255, 60, 60, 255},
        .border      = (Color){255,255,255, 255},
        .canvas_bg   = (Color){ 0, 0, 0, 0 },   /* transparent */
        .canvas_line = (Color){ 0, 0, 0, 0 },   /* transparent */
        .status_bg   = (Color){  0,  0,  0, 255},
        .status_fg   = (Color){255,255,  0, 255},
        .popup_bg    = (Color){  0,  0,  0, 255},
        .popup_sel_bg= (Color){  0,128,  0, 255},
        .popup_fg    = (Color){255,255,255, 255},
        .popup_dim_fg= (Color){180,180,180, 255},
        .popup_border= (Color){255,255,255, 255},
        .popup_match = (Color){255,255,  0, 255},
        .syntax      = {
            .comment       = (Color){180,180,180, 255},
            .string        = (Color){  0,255,  0, 255},
            .interp        = (Color){255,165,  0, 255},
            .number        = (Color){  0,255,255, 255},
            .kw            = (Color){255,  0,255, 255},
            .builtin       = (Color){  0,200,255, 255},
            .paren         = (Color){200,200,200, 255},
            .lit           = (Color){255,255,  0, 255},
            .normal        = (Color){255,255,255, 255},
            .bracket_match = (Color){255,255,  0, 255},
        },
    },
};
static const int ZC_THEME_COUNT = sizeof(ZC_THEMES) / sizeof(ZC_THEMES[0]);
static int g_theme_idx = 0;

#define TC (ZC_THEMES[g_theme_idx])   /* shorthand for the active theme */

/* g_syntax is now sourced from the active theme — kept here as a pointer
 * shorthand so existing zc_draw_highlighted code stays readable. */
#define g_syntax (TC.syntax)

static const char* ZC_SPECIAL_FORMS[] = {
    "do", "if", "when", "unless", "cond", "let",
    "while", "for", "fn", "lambda", "set", "try", "catch",
    "and", "or", "quote", "else", "&&", "||", "->", "->>", "&", NULL
};
static const char* ZC_LITERALS[] = { "true", "false", "null", "nil", NULL };

static int zc_in_list(const char* needle, const char* const* list) {
    for (int i = 0; list[i]; i++)
        if (strcmp(list[i], needle) == 0) return 1;
    return 0;
}

/* Find the position of the matching paren for the one at `pos`, or -1.
 * Searches forward if buf[pos]=='(' and backward if buf[pos]==')'. */
static int zc_match_paren(const char* buf, int len, int pos) {
    if (pos < 0 || pos >= len) return -1;
    char c = buf[pos];
    int dir, depth, target;
    if (c == '(')      { dir = +1; target = ')'; }
    else if (c == ')') { dir = -1; target = '('; }
    else return -1;
    depth = 1;
    int in_string = 0;
    for (int i = pos + dir; i >= 0 && i < len; i += dir) {
        if (buf[i] == '"' && (i == 0 || buf[i-1] != '\\'))
            in_string = !in_string;
        if (in_string) continue;
        if (buf[i] == c) depth++;
        else if (buf[i] == target) {
            depth--;
            if (depth == 0) return i;
        }
    }
    return -1;
}

/* Net paren balance over the whole buffer, ignoring chars inside strings
 * and after a ; comment. Returns 0 = balanced, >0 = unclosed `(`, <0 = too
 * many `)`. */
static int zc_paren_balance(const char* buf, int len) {
    int depth = 0;
    int in_string = 0;
    int in_comment = 0;
    for (int i = 0; i < len; i++) {
        char c = buf[i];
        if (in_comment) { if (c == '\n') in_comment = 0; continue; }
        if (c == ';' && !in_string) { in_comment = 1; continue; }
        if (c == '"' && (i == 0 || buf[i-1] != '\\')) in_string = !in_string;
        if (in_string) continue;
        if (c == '(' || c == '[') depth++;
        else if (c == ')' || c == ']') { depth--; }
    }
    return depth;
}

/* Was the last non-comment string left unterminated? */
static int zc_string_unterminated(const char* buf, int len) {
    int in_string = 0, in_comment = 0;
    for (int i = 0; i < len; i++) {
        char c = buf[i];
        if (in_comment) { if (c == '\n') in_comment = 0; continue; }
        if (c == ';' && !in_string) { in_comment = 1; continue; }
        if (c == '"' && (i == 0 || buf[i-1] != '\\')) in_string = !in_string;
    }
    return in_string;
}

/* Walk symbol tokens that appear in head-of-call position (`(name ...`) and
 * collect any that aren't a special form, literal, or env name. Writes
 * up to `max` names into `out` and returns the count. Lambda/fn params
 * and `set` names are deliberately excluded so newly-defined bindings
 * don't false-positive. */
static int zc_collect_unknown_calls(const char* buf, int len, Env* env,
                                    char out[][32], int max) {
    int n = 0;
    int in_string = 0, in_comment = 0;
    for (int i = 0; i < len && n < max; i++) {
        char c = buf[i];
        if (in_comment) { if (c == '\n') in_comment = 0; continue; }
        if (c == ';' && !in_string) { in_comment = 1; continue; }
        if (c == '"' && (i == 0 || buf[i-1] != '\\')) { in_string = !in_string; continue; }
        if (in_string) continue;
        if (c != '(') continue;
        int j = i + 1;
        while (j < len && (buf[j] == ' ' || buf[j] == '\t')) j++;
        if (j >= len || !is_sym_start((unsigned char)buf[j])) continue;
        int s = j;
        while (j < len && is_sym_cont((unsigned char)buf[j])) j++;
        char tok[32];
        int L = j - s; if (L >= (int)sizeof(tok)) L = (int)sizeof(tok) - 1;
        memcpy(tok, buf + s, L); tok[L] = 0;
        /* Special forms / introducers — skip and (for fn/lambda/set) skip
         * the symbol that follows so e.g. `(fn greet (x) ...)` doesn't flag
         * `greet` or the params. */
        if (zc_in_list(tok, ZC_SPECIAL_FORMS) || zc_in_list(tok, ZC_LITERALS))
            continue;
        if (env && env_lookup(env, tok)) continue;
        /* Dedup. */
        int dup = 0;
        for (int k = 0; k < n; k++) if (strcmp(out[k], tok) == 0) { dup = 1; break; }
        if (!dup) {
            int cp = L < 31 ? L : 31;
            memcpy(out[n], tok, cp); out[n][cp] = 0;
            n++;
        }
    }
    return n;
}

/* Helper: draw a token at a float x position so accumulation stays precise. */
static void zc_draw_tok_f(const char* buf, float fx, int y, int size, Color col) {
    DrawTextEx(g_console_font, buf ? buf : "",
               (Vector2){ fx, (float)y },
               (float)size, 0.0f, col);
}

/* Draw `text` with z syntax highlighting, returning the pixel width.
 *
 * Internally accumulates the x position as a float so per-token width
 * rounding doesn't drift left of where the cursor (measured as one
 * substring) expects. `match_pos` (if >= 0) marks a paren whose matching
 * mate should glow; `env` is used to classify identifiers. */
static int zc_draw_highlighted(const char* text, int len, int x, int y, int size,
                               Env* env, int match_pos) {
    int i = 0;
    float draw_x = (float)x;
    char tmp[2] = {0, 0};
    while (i < len) {
        char c = text[i];
        /* Comment */
        if (c == ';') {
            int s = i;
            while (i < len && text[i] != '\n') i++;
            char buf[1024];
            int n = i - s < (int)sizeof(buf)-1 ? i - s : (int)sizeof(buf)-1;
            memcpy(buf, text + s, n); buf[n] = 0;
            zc_draw_tok_f(buf, draw_x, y, size, g_syntax.comment);
            draw_x += zc_measure_text_f(buf, size);
            continue;
        }
        /* String — also colourises ${...} interpolation. */
        if (c == '"') {
            int s = i;
            i++;
            while (i < len && text[i] != '"') {
                if (text[i] == '\\' && i + 1 < len) { i += 2; continue; }
                i++;
            }
            if (i < len) i++;  /* closing quote */
            char buf[1024];
            int n = i - s < (int)sizeof(buf)-1 ? i - s : (int)sizeof(buf)-1;
            memcpy(buf, text + s, n); buf[n] = 0;
            zc_draw_tok_f(buf, draw_x, y, size, g_syntax.string);
            draw_x += zc_measure_text_f(buf, size);
            continue;
        }
        /* Number — bare digit-start. */
        if ((c >= '0' && c <= '9')
            || (c == '-' && i + 1 < len && text[i+1] >= '0' && text[i+1] <= '9')) {
            int s = i;
            if (c == '-') i++;
            while (i < len && (isdigit((unsigned char)text[i])
                            || text[i] == '.' || text[i] == 'e' || text[i] == 'E'
                            || text[i] == 'x' || text[i] == 'X'
                            || (text[i] >= 'a' && text[i] <= 'f')
                            || (text[i] >= 'A' && text[i] <= 'F')))
                i++;
            char buf[64];
            int n = i - s < (int)sizeof(buf)-1 ? i - s : (int)sizeof(buf)-1;
            memcpy(buf, text + s, n); buf[n] = 0;
            zc_draw_tok_f(buf, draw_x, y, size, g_syntax.number);
            draw_x += zc_measure_text_f(buf, size);
            continue;
        }
        /* Identifier — symbol token. */
        if (is_sym_start((unsigned char)c)) {
            int s = i;
            while (i < len && is_sym_cont((unsigned char)text[i])) i++;
            char tok[128];
            int n = i - s < (int)sizeof(tok)-1 ? i - s : (int)sizeof(tok)-1;
            memcpy(tok, text + s, n); tok[n] = 0;
            Color col;
            if      (zc_in_list(tok, ZC_LITERALS))      col = g_syntax.lit;
            else if (zc_in_list(tok, ZC_SPECIAL_FORMS)) col = g_syntax.kw;
            else if (env && env_lookup(env, tok))       col = g_syntax.builtin;
            else                                        col = g_syntax.normal;
            zc_draw_tok_f(tok, draw_x, y, size, col);
            draw_x += zc_measure_text_f(tok, size);
            continue;
        }
        /* Single character — paren / bracket / punctuation. */
        Color col = g_syntax.normal;
        if (c == '(' || c == ')' || c == '[' || c == ']')
            col = (i == match_pos) ? g_syntax.bracket_match : g_syntax.paren;
        tmp[0] = c;
        zc_draw_tok_f(tmp, draw_x, y, size, col);
        draw_x += zc_measure_text_f(tmp, size);
        i++;
    }
    return (int)(draw_x - (float)x + 0.5f);
}

/* Find word-boundary positions for Ctrl+←/→ navigation. Both stop at the
 * nearest newline so multi-line input doesn't suck content from another
 * line when the user is just trying to skip a word. */
static int zc_word_left(const char* buf, int pos) {
    if (pos <= 0) return 0;
    pos--;
    while (pos > 0 && buf[pos] != '\n'
           && !is_sym_cont((unsigned char)buf[pos])) pos--;
    while (pos > 0 && buf[pos-1] != '\n'
           && is_sym_cont((unsigned char)buf[pos-1])) pos--;
    return pos;
}
static int zc_word_right(const char* buf, int len, int pos) {
    if (pos >= len) return len;
    while (pos < len && buf[pos] != '\n'
           && !is_sym_cont((unsigned char)buf[pos])) pos++;
    while (pos < len && buf[pos] != '\n'
           && is_sym_cont((unsigned char)buf[pos])) pos++;
    return pos;
}

/* Start / end of the line containing `pos`. End is the index of '\n' (or
 * input_len if on the last line). */
static int zc_line_start(const char* buf, int pos) {
    while (pos > 0 && buf[pos-1] != '\n') pos--;
    return pos;
}
static int zc_line_end(const char* buf, int len, int pos) {
    while (pos < len && buf[pos] != '\n') pos++;
    return pos;
}

/* ============================================================
 * UI op model — each evaluation fills a Cell.
 * ============================================================ */

typedef enum {
    OP_CIRCLE,
    OP_RECT,
    OP_LINE,
    OP_TEXT_AT,
    OP_IMAGE
} OpKind;

typedef struct {
    OpKind    kind;
    float     x, y, w, h, r, x2, y2;
    Color     color;
    char*     text;
    int       size;
    Texture2D tex;     /* used by OP_IMAGE */
    int       tex_loaded;
} DrawOp;

typedef struct {
    char*   input;        /* user's input line, echoed at the top of the cell */
    char*   text_out;     /* concatenated print/ui:text output */
    DrawOp* ops;
    int     op_count, op_cap;
    int     canvas_h;     /* auto-grows to fit the lowest shape           */
    int     is_error;     /* paints text_out red                          */
} Cell;

static Cell* g_cells       = NULL;
static int   g_cell_count  = 0;
static int   g_cell_cap    = 0;
static Cell* g_current     = NULL;  /* set during eval so ui:* knows where to write */

static void cell_append_text(Cell* c, const char* s) {
    if (!s || !*s) return;
    size_t old_len = c->text_out ? strlen(c->text_out) : 0;
    size_t add     = strlen(s);
    c->text_out = (char*)realloc(c->text_out, old_len + add + 1);
    if (old_len == 0) c->text_out[0] = 0;
    strcat(c->text_out, s);
}

static void cell_push_op(Cell* c, DrawOp op) {
    if (c->op_count + 1 > c->op_cap) {
        c->op_cap = c->op_cap ? c->op_cap * 2 : 8;
        c->ops    = (DrawOp*)realloc(c->ops, c->op_cap * sizeof(DrawOp));
    }
    c->ops[c->op_count++] = op;
    float bottom = 0;
    switch (op.kind) {
        case OP_CIRCLE:  bottom = op.y + op.r;                       break;
        case OP_RECT:    bottom = op.y + op.h;                       break;
        case OP_LINE:    bottom = op.y > op.y2 ? op.y : op.y2;       break;
        case OP_TEXT_AT: bottom = op.y + (op.size > 0 ? op.size:20); break;
        case OP_IMAGE:   bottom = op.y + op.h;                       break;
    }
    if ((int)bottom + 10 > c->canvas_h) c->canvas_h = (int)bottom + 10;
}

/* ============================================================
 * Colour parsing
 * ============================================================ */

static int hex2(const char* p) {
    int hi = -1, lo = -1;
    if (p[0] >= '0' && p[0] <= '9') hi = p[0]-'0';
    else if (p[0] >= 'a' && p[0] <= 'f') hi = p[0]-'a'+10;
    else if (p[0] >= 'A' && p[0] <= 'F') hi = p[0]-'A'+10;
    if (p[1] >= '0' && p[1] <= '9') lo = p[1]-'0';
    else if (p[1] >= 'a' && p[1] <= 'f') lo = p[1]-'a'+10;
    else if (p[1] >= 'A' && p[1] <= 'F') lo = p[1]-'A'+10;
    if (hi < 0 || lo < 0) return -1;
    return (hi << 4) | lo;
}

static Color parse_color(const char* s) {
    if (!s || !*s) return BLACK;
    if (s[0] == '#') {
        size_t L = strlen(s);
        int r=0,g=0,b=0,a=255;
        if (L >= 7) {
            int rr = hex2(s+1), gg = hex2(s+3), bb = hex2(s+5);
            if (rr>=0) r = rr; if (gg>=0) g = gg; if (bb>=0) b = bb;
        }
        if (L >= 9) { int aa = hex2(s+7); if (aa>=0) a = aa; }
        return (Color){ (unsigned char)r,(unsigned char)g,(unsigned char)b,(unsigned char)a };
    }
    if (!strcmp(s,"red"))       return RED;
    if (!strcmp(s,"green"))     return GREEN;
    if (!strcmp(s,"blue"))      return BLUE;
    if (!strcmp(s,"yellow"))    return YELLOW;
    if (!strcmp(s,"orange"))    return ORANGE;
    if (!strcmp(s,"purple"))    return PURPLE;
    if (!strcmp(s,"pink"))      return PINK;
    if (!strcmp(s,"white"))     return WHITE;
    if (!strcmp(s,"black"))     return BLACK;
    if (!strcmp(s,"gray"))      return GRAY;
    if (!strcmp(s,"grey"))      return GRAY;
    if (!strcmp(s,"darkgray"))  return DARKGRAY;
    if (!strcmp(s,"lightgray")) return LIGHTGRAY;
    if (!strcmp(s,"magenta"))   return MAGENTA;
    if (!strcmp(s,"maroon"))    return MAROON;
    if (!strcmp(s,"violet"))    return VIOLET;
    return BLACK;
}

/* ============================================================
 * ui:* builtins — populate g_current during eval
 * ============================================================ */

static Value* b_ui_text(int argc, Value** argv, Env* e) {
    (void)e; EXPECT_ARGC("ui:text", 1);
    if (!g_current) return v_null();
    char* s = value_to_cstr(argv[0]);
    cell_append_text(g_current, s);
    cell_append_text(g_current, "\n");
    free(s);
    return v_null();
}

static Value* b_ui_circle(int argc, Value** argv, Env* e) {
    (void)e;
    if (argc < 3 || argc > 4)
        z_raise("ui:circle: (ui:circle cx cy r [color])");
    if (!g_current) return v_null();
    DrawOp op; memset(&op, 0, sizeof(op));
    op.kind  = OP_CIRCLE;
    op.x     = (float)num_arg(argv[0], "ui:circle");
    op.y     = (float)num_arg(argv[1], "ui:circle");
    op.r     = (float)num_arg(argv[2], "ui:circle");
    op.color = parse_color(argc >= 4 ? str_arg(argv[3], "ui:circle") : "blue");
    cell_push_op(g_current, op);
    return v_null();
}

static Value* b_ui_rect(int argc, Value** argv, Env* e) {
    (void)e;
    if (argc < 4 || argc > 5)
        z_raise("ui:rect: (ui:rect x y w h [color])");
    if (!g_current) return v_null();
    DrawOp op; memset(&op, 0, sizeof(op));
    op.kind  = OP_RECT;
    op.x     = (float)num_arg(argv[0], "ui:rect");
    op.y     = (float)num_arg(argv[1], "ui:rect");
    op.w     = (float)num_arg(argv[2], "ui:rect");
    op.h     = (float)num_arg(argv[3], "ui:rect");
    op.color = parse_color(argc >= 5 ? str_arg(argv[4], "ui:rect") : "blue");
    cell_push_op(g_current, op);
    return v_null();
}

static Value* b_ui_line(int argc, Value** argv, Env* e) {
    (void)e;
    if (argc < 4 || argc > 5)
        z_raise("ui:line: (ui:line x1 y1 x2 y2 [color])");
    if (!g_current) return v_null();
    DrawOp op; memset(&op, 0, sizeof(op));
    op.kind  = OP_LINE;
    op.x     = (float)num_arg(argv[0], "ui:line");
    op.y     = (float)num_arg(argv[1], "ui:line");
    op.x2    = (float)num_arg(argv[2], "ui:line");
    op.y2    = (float)num_arg(argv[3], "ui:line");
    op.color = parse_color(argc >= 5 ? str_arg(argv[4], "ui:line") : "black");
    cell_push_op(g_current, op);
    return v_null();
}

static Value* b_ui_text_at(int argc, Value** argv, Env* e) {
    (void)e;
    if (argc < 3 || argc > 5)
        z_raise("ui:text-at: (ui:text-at x y \"text\" [size] [color])");
    if (!g_current) return v_null();
    DrawOp op; memset(&op, 0, sizeof(op));
    op.kind  = OP_TEXT_AT;
    op.x     = (float)num_arg(argv[0], "ui:text-at");
    op.y     = (float)num_arg(argv[1], "ui:text-at");
    op.text  = str_dup(str_arg(argv[2], "ui:text-at"));
    op.size  = argc >= 4 ? (int)num_arg(argv[3], "ui:text-at") : 20;
    op.color = parse_color(argc >= 5 ? str_arg(argv[4], "ui:text-at") : "black");
    cell_push_op(g_current, op);
    return v_null();
}

/* (ui:image "path" [x y] [w h])
 *
 * Display an image inline in the current cell. Reasonable defaults:
 *   - no x/y   → image starts at top-left of the canvas
 *   - no w/h   → image's native pixel size
 *   - w only   → height auto-computed to preserve aspect ratio
 *
 * Supported formats are whatever raylib's `LoadTexture` accepts: PNG, JPG,
 * BMP, TGA, GIF (first frame), QOI, and several others.
 *
 * The texture is owned by the cell's DrawOp and freed when that cell is
 * cleared (Ctrl-L) or the window closes. */
static Value* b_ui_image(int argc, Value** argv, Env* e) {
    (void)e;
    if (argc < 1 || argc > 5)
        z_raise("ui:image: (ui:image \"path\" [x y] [w h])");
    if (!g_current) return v_null();
    const char* path = str_arg(argv[0], "ui:image");
    Texture2D tex = LoadTexture(path);
    if (tex.id == 0)
        z_raise("ui:image: could not load '%s' (unsupported format or missing file)", path);
    DrawOp op; memset(&op, 0, sizeof(op));
    op.kind       = OP_IMAGE;
    op.tex        = tex;
    op.tex_loaded = 1;
    op.x          = argc >= 3 ? (float)num_arg(argv[1], "ui:image") : 0.0f;
    op.y          = argc >= 3 ? (float)num_arg(argv[2], "ui:image") : 0.0f;
    if (argc >= 5) {
        op.w = (float)num_arg(argv[3], "ui:image");
        op.h = (float)num_arg(argv[4], "ui:image");
    } else if (argc == 4) {
        /* width given, height = width * native-aspect */
        op.w = (float)num_arg(argv[3], "ui:image");
        op.h = op.w * (float)tex.height / (float)tex.width;
    } else {
        op.w = (float)tex.width;
        op.h = (float)tex.height;
    }
    cell_push_op(g_current, op);
    return v_null();
}

/* (ui:polygon (array x1 y1 x2 y2 ...) [color])  — filled polygon. */
static Value* b_ui_polygon(int argc, Value** argv, Env* e) {
    (void)e;
    if (argc < 1 || argc > 2) z_raise("ui:polygon: (ui:polygon points [color])");
    if (!g_current) return v_null();
    Value* pts = argv[0];
    if (pts->type != V_ARRAY && pts->type != V_LIST)
        z_raise("ui:polygon: first arg must be an array of numbers");
    if (pts->as.list.len < 6 || (pts->as.list.len & 1))
        z_raise("ui:polygon: need at least 3 points (6 numbers), even count");
    /* Walk all points to update the canvas height; render as a fan of
     * triangles with raylib's DrawTriangle, which is in raylib core. */
    Color col = parse_color(argc >= 2 ? str_arg(argv[1], "ui:polygon") : "black");
    float maxy = 0;
    /* Store as an OP_LINE chain so the existing renderer just runs — we
     * lay it out as edge segments (closed loop). Cheap but works without
     * adding a new OpKind. */
    int n = (int)pts->as.list.len / 2;
    for (int i = 0; i < n; i++) {
        int j = (i + 1) % n;
        DrawOp op; memset(&op, 0, sizeof(op));
        op.kind  = OP_LINE;
        op.x     = (float)num_arg(pts->as.list.items[2*i],   "ui:polygon");
        op.y     = (float)num_arg(pts->as.list.items[2*i+1], "ui:polygon");
        op.x2    = (float)num_arg(pts->as.list.items[2*j],   "ui:polygon");
        op.y2    = (float)num_arg(pts->as.list.items[2*j+1], "ui:polygon");
        op.color = col;
        if (op.y  > maxy) maxy = op.y;
        if (op.y2 > maxy) maxy = op.y2;
        cell_push_op(g_current, op);
    }
    return v_null();
}

/* (ui:triangle x1 y1 x2 y2 x3 y3 [color]) — three-edge polygon. */
static Value* b_ui_triangle(int argc, Value** argv, Env* e) {
    (void)e;
    if (argc < 6 || argc > 7)
        z_raise("ui:triangle: (ui:triangle x1 y1 x2 y2 x3 y3 [color])");
    if (!g_current) return v_null();
    Color col = parse_color(argc >= 7 ? str_arg(argv[6], "ui:triangle") : "black");
    float xs[3] = { (float)num_arg(argv[0],"ui:triangle"),
                    (float)num_arg(argv[2],"ui:triangle"),
                    (float)num_arg(argv[4],"ui:triangle") };
    float ys[3] = { (float)num_arg(argv[1],"ui:triangle"),
                    (float)num_arg(argv[3],"ui:triangle"),
                    (float)num_arg(argv[5],"ui:triangle") };
    for (int i = 0; i < 3; i++) {
        DrawOp op; memset(&op, 0, sizeof(op));
        op.kind  = OP_LINE;
        op.x  = xs[i];     op.y  = ys[i];
        op.x2 = xs[(i+1)%3]; op.y2 = ys[(i+1)%3];
        op.color = col;
        cell_push_op(g_current, op);
    }
    return v_null();
}

/* (ui:save-canvas "path") — snapshot the WHOLE window to a PNG and write it.
 * Best-effort: captures everything currently on screen. */
static Value* b_ui_save_canvas(int argc, Value** argv, Env* e) {
    (void)e;
    EXPECT_ARGC("ui:save-canvas", 1);
    const char* path = str_arg(argv[0], "ui:save-canvas");
    TakeScreenshot(path);
    return v_str(path);
}

static Value* b_ui_clear(int argc, Value** argv, Env* e) {
    (void)e; (void)argc; (void)argv;
    if (g_current) {
        /* Unload any textures the previous draws owned so we don't leak GPU
         * memory across (ui:clear) calls in long-running cells. */
        for (int i = 0; i < g_current->op_count; i++) {
            DrawOp* op = &g_current->ops[i];
            if (op->tex_loaded) { UnloadTexture(op->tex); op->tex_loaded = 0; }
        }
        g_current->op_count = 0;
        g_current->canvas_h = 0;
        if (g_current->text_out) g_current->text_out[0] = 0;
    }
    return v_null();
}

/* Intercept print so existing z code's output is captured into the cell. */
static Value* b_print_to_cell(int argc, Value** argv, Env* e) {
    (void)e;
    StrBuf sb; sb_init(&sb);
    for (int i = 0; i < argc; i++) {
        if (i) sb_putc(&sb, ' ');
        char* s = value_to_cstr(argv[i]);
        sb_puts(&sb, s);
        free(s);
    }
    sb_putc(&sb, '\n');
    if (g_current) cell_append_text(g_current, sb.data);
    fputs(sb.data, stdout);
    fflush(stdout);
    free(sb.data);
    return v_null();
}

/* ============================================================
 * Cheatsheet examples — used to render an inline usage hint next to each
 * autocomplete row. Names that aren't in this table just show without an
 * example. Pairs were generated from CHEATSHEET.md and stay in sync with
 * the docs by convention.
 * ============================================================ */

typedef struct { const char* name; const char* example; } ZcExample;

static const ZcExample Z_CONSOLE_EXAMPLES[] = {
    { "do", "(do (print \"a\") (print \"b\"))" },
    { "if", "(if (> x 0) \"pos\" \"neg\")" },
    { "when", "(when (> x 0) (print \"positive\"))" },
    { "unless", "(unless (zero? x) (print x))" },
    { "cond", "(cond ((> x 0) \"+\") ((< x 0) \"-\") (else \"0\"))" },
    { "let", "(let ((x 10) (y 20)) (+ x y))" },
    { "->",  "(-> \"  hi  \" trim upper)" },
    { "->>", "(->> xs (map sq) (filter pos?))" },
    { "while", "(while (< i 10) (set i (+ i 1)))" },
    { "for", "(for x (array 1 2 3) (print x))" },
    { "fn", "(fn add (a b) (+ a b))" },
    { "lambda", "(lambda (x) (* x 2))" },
    { "set", "(set name \"vivek\")" },
    { "quote", "(quote (a b c))" },
    { "and", "(and true false)" },
    { "or", "(or false true)" },
    { "array", "(array 1 2 3)" },
    { "object", "(object \"name\" \"vivek\" \"age\" 30)" },
    { "get", "(get (array 10 20) 1)" },
    { "put", "(put (object) \"key\" \"val\")" },
    { "push", "(push (array 1 2) 3)" },
    { "pop", "(pop (array 1 2 3))" },
    { "length", "(length \"hello\")" },
    { "keys", "(keys (object \"a\" 1))" },
    { "values", "(values (object \"a\" 1))" },
    { "entries", "(entries (object \"a\" 1))" },
    { "reverse", "(reverse (array 1 2 3))" },
    { "sort", "(sort (array 3 1 2))" },
    { "chunk", "(chunk (array 1 2 3 4 5) 2)" },
    { "map", "(map (lambda (x) (* x x)) (array 1 2 3))" },
    { "filter", "(filter (lambda (x) (> x 2)) (array 1 2 3))" },
    { "reduce", "(reduce (lambda (a b) (+ a b)) (array 1 2 3) 0)" },
    { "concat", "(concat \"foo\" \"bar\")" },
    { "split", "(split \",\" \"a,b,c\")" },
    { "join", "(join \", \" (array 1 2 3))" },
    { "trim", "(trim \"  hi  \")" },
    { "lower", "(lower \"ABC\")" },
    { "upper", "(upper \"abc\")" },
    { "replace", "(replace \"hello\" \"l\" \"L\")" },
    { "substring", "(substring \"hello\" 0 3)" },
    { "between", "(between \"<a>hi</a>\" \"<a>\" \"</a>\")" },
    { "levenshtein", "(levenshtein \"kitten\" \"sitting\")" },
    { "starts-with", "(starts-with \"hello\" \"he\")" },
    { "ends-with", "(ends-with \"hello\" \"lo\")" },
    { "contains", "(contains \"hello\" \"ell\")" },
    { "index-of", "(index-of \"hello\" \"l\")" },
    { "regex:test", "(regex:test \"\\\\d+\" \"abc123\")" },
    { "regex:match", "(regex:match \"\\\\d+\" \"abc123\")" },
    { "regex:find-all", "(regex:find-all \"\\\\d+\" \"a1 b2\")" },
    { "regex:replace", "(regex:replace \"\\\\d\" \"a1b2\" \"X\")" },
    { "regex:split", "(regex:split \"[, ]+\" \"a, b c\")" },
    { "min", "(min 3 1 2)" },
    { "max", "(max 3 1 2)" },
    { "floor", "(floor 3.7)" },
    { "ceil", "(ceil 3.2)" },
    { "abs", "(abs -5)" },
    { "random", "(random)" },
    { "round", "(round 3.5)" },
    { "trunc", "(trunc -3.7)" },
    { "sign", "(sign -3)" },
    { "mod", "(mod 10 3)" },
    { "sqrt", "(sqrt 2)" },
    { "cbrt", "(cbrt 27)" },
    { "pow", "(pow 2 10)" },
    { "exp", "(exp 1)" },
    { "log", "(log 2.71828)" },
    { "log2", "(log2 1024)" },
    { "log10", "(log10 1000)" },
    { "sin", "(sin (/ pi 2))" },
    { "cos", "(cos 0)" },
    { "tan", "(tan (/ pi 4))" },
    { "asin", "(asin 1)" },
    { "acos", "(acos 0)" },
    { "atan", "(atan 1)" },
    { "atan2", "(atan2 1 1)" },
    { "sinh", "(sinh 0)" },
    { "cosh", "(cosh 0)" },
    { "tanh", "(tanh 0)" },
    { "pi", "pi" },
    { "e", "e" },
    { "print", "(print \"Hello\")" },
    { "type", "(type 42)" },
    { "assert", "(assert (> 1 0) \"must be positive\")" },
    { "sleep", "(sleep 0.5)" },
    { "help", "(help \"strings\")" },
    { "read", "(read \"data.txt\")" },
    { "read-lines", "(read-lines \"data.txt\")" },
    { "read-bytes", "(read-bytes \"image.png\")" },
    { "write", "(write \"out.txt\" \"content\")" },
    { "write-bytes", "(write-bytes \"out.bin\" b)" },
    { "bytes", "(bytes (array 0 1 2 255))" },
    { "hex", "(hex (bytes \"abc\"))" },
    { "unhex", "(unhex \"deadbeef\")" },
    { "bytes:get", "(bytes:get b 0)" },
    { "bytes:slice", "(bytes:slice b 0 4)" },
    { "bytes:concat", "(bytes:concat b1 b2)" },
    { "string->bytes", "(string->bytes \"hi\")" },
    { "bytes->string", "(bytes->string b)" },
    { "append", "(append \"log.txt\" \"a line\")" },
    { "delete", "(delete \"tmp.txt\")" },
    { "list-dir", "(list-dir \".\")" },
    { "file-info", "(file-info \"z.c\")" },
    { "copy-file", "(copy-file \"a.txt\" \"b.txt\")" },
    { "move-file", "(move-file \"a.txt\" \"b.txt\")" },
    { "json:parse", "(json:parse \"{\\\"a\\\":1}\")" },
    { "json:stringify", "(json:stringify (object \"a\" 1))" },
    { "base64:encode", "(base64:encode \"hello\")" },
    { "base64:decode", "(base64:decode \"aGVsbG8=\")" },
    { "encrypt", "(encrypt \"key\" \"secret\")" },
    { "decrypt", "(decrypt \"key\" cipher)" },
    { "uuid", "(uuid)" },
    { "md5", "(md5 \"abc\")" },
    { "sha256", "(sha256 \"abc\")" },
    { "sha512", "(sha512 \"abc\")" },
    { "url:encode", "(url:encode \"hello world!\")" },
    { "url:decode", "(url:decode \"hello%20world%21\")" },
    { "url:build", "(url:build \"https://x.com/p\" (object \"q\" \"1 2\"))" },
    { "zip:create", "(zip:create \"out.zip\" (array \"a.txt\" \"b.txt\"))" },
    { "zip:extract", "(zip:extract \"out.zip\" \"./dest\")" },
    { "tar:create", "(tar:create \"out.tgz\" (array \"a\" \"b\") \"gz\")" },
    { "tar:extract", "(tar:extract \"out.tgz\" \"./dest\")" },
    { "input", "(input \"your name: \")" },
    { "http:get", "(http:get \"https://example.com\")" },
    { "http:post", "(http:post url (object \"k\" \"v\"))" },
    { "now", "(now)" },
    { "timestamp", "(timestamp)" },
    { "format-date", "(format-date (timestamp) \"%Y-%m-%d\")" },
    { "env", "(env \"HOME\")" },
    { "exec", "(exec \"ls -1\")" },
    { "run", "(run \"ls\")" },
    { "argv", "(argv)" },
    { "exit", "(exit 0)" },
    { "import", "(import \"lib.z\")" },
    { "img:create", "(img:create \"c.png\" 400 300 \"white\")" },
    { "img:info", "(img:info \"in.png\")" },
    { "img:resize", "(img:resize \"in.png\" \"out.png\" 200 200)" },
    { "img:crop", "(img:crop \"in.png\" \"out.png\" 0 0 100 100)" },
    { "img:rotate", "(img:rotate \"in.png\" \"out.png\" 90)" },
    { "img:compose", "(img:compose \"bg.png\" \"logo.png\" \"out.png\" 20 30)" },
    { "img:replace-color", "(img:replace-color \"in.png\" \"out.png\" \"red\" \"blue\" 10)" },
    { "img:circle", "(img:circle \"c.png\" \"c.png\" 100 100 50 \"red\")" },
    { "img:rect", "(img:rect \"c.png\" \"c.png\" 10 10 80 40 \"blue\")" },
    { "img:add-text", "(img:add-text \"in.png\" \"out.png\" \"hi\" 10 30 24 \"white\")" },
    { "img:bw", "(img:bw \"in.png\" \"out.png\")" },
    { "img:grayscale", "(img:grayscale \"in.png\" \"out.png\")" },
    { "img:to-pdf", "(img:to-pdf (array \"a.png\" \"b.png\") \"out.pdf\")" },
    { "img:qr", "(img:qr \"https://example.com\" \"qr.png\" 6)" },
    { "img:barcode", "(img:barcode \"012345678905\" \"code.png\" \"ean13\")" },
    { "vision:barcode", "(vision:barcode \"receipt.png\")" },
    { "vision:faces", "(vision:faces \"group.jpg\")" },
    { "vision:objects", "(vision:objects \"street.jpg\")" },
    { "cv:faces",      "(cv:faces \"in.pgm\" \"face.zhc\")" },
    { "cv:read-pgm",   "(cv:read-pgm \"in.pgm\")" },
    { "cv:save-pgm",   "(cv:save-pgm \"out.pgm\" w h bytes)" },
    { "cv:image-info", "(cv:image-info \"in.pgm\")" },
    { "ocr:image",  "(ocr:image \"doc.png\")" },
    { "ocr:words",  "(ocr:words \"doc.png\" \"eng\")" },
    { "sqlite:open",   "(sqlite:open \":memory:\")" },
    { "sqlite:exec",   "(sqlite:exec db \"INSERT ...\" (array v))" },
    { "sqlite:query",  "(sqlite:query db \"SELECT ...\" (array v))" },
    { "sqlite:close",  "(sqlite:close db)" },
    { "sqlite:last-insert-id", "(sqlite:last-insert-id db)" },
    { "kv:open",       "(kv:open \"store.db\")" },
    { "kv:set",        "(kv:set s \"k\" v)" },
    { "kv:get",        "(kv:get s \"k\")" },
    { "kv:del",        "(kv:del s \"k\")" },
    { "kv:keys",       "(kv:keys s \"prefix\")" },
    { "html:query",   "(html:query \"li.a\" html)" },
    { "html:text",    "(html:text \"<b>hi</b>\")" },
    { "html:attr",    "(html:attr \"href\" link)" },
    { "xml:query",    "(xml:query \"/root/user/name\" xml)" },
    { "xml:text",     "(xml:text \"<n>Ada</n>\")" },
    { "xml:attr",     "(xml:attr \"id\" frag)" },
    /* The ui:* draw helpers added by z-console itself. */
    { "ui:text", "(ui:text \"hello\")" },
    { "ui:circle", "(ui:circle 100 100 50 \"red\")" },
    { "ui:rect", "(ui:rect 10 10 80 40 \"blue\")" },
    { "ui:line", "(ui:line 0 0 200 200 \"black\")" },
    { "ui:text-at", "(ui:text-at 20 200 \"hi\" 22 \"black\")" },
    { "ui:image", "(ui:image \"photo.png\" 10 10 320 240)" },
    { "ui:clear", "(ui:clear)" },
};
static const size_t Z_CONSOLE_EXAMPLES_LEN =
    sizeof(Z_CONSOLE_EXAMPLES) / sizeof(Z_CONSOLE_EXAMPLES[0]);

static const char* zc_example_for(const char* name) {
    for (size_t i = 0; i < Z_CONSOLE_EXAMPLES_LEN; i++)
        if (strcmp(Z_CONSOLE_EXAMPLES[i].name, name) == 0)
            return Z_CONSOLE_EXAMPLES[i].example;
    return NULL;
}

/* ============================================================
 * Function signatures — parameter names + arity, displayed as the
 * call shape (e.g. "(substring s start [end])") so the user can see
 * what arguments are expected without referring to docs.
 *
 * For built-ins we hand-author the table below. For user-defined
 * functions we introspect V_FN.params at lookup time.
 * ============================================================ */

typedef struct { const char* name; const char* sig; } ZcSig;

static const ZcSig Z_CONSOLE_SIGS[] = {
    /* special forms */
    { "do",          "expr..." },
    { "if",          "cond then [else]" },
    { "when",        "cond body..." },
    { "unless",      "cond body..." },
    { "cond",        "(test body...) (test body...) ..." },
    { "let",         "((name val) ...) body..." },
    { "->",          "initial step ..." },
    { "->>",         "initial step ..." },
    { "fn",          "name (params... [& rest]) body..." },
    { "while",       "cond body..." },
    { "for",         "var coll body..." },
    { "fn",          "name (params) body..." },
    { "lambda",      "(params) body..." },
    { "set",         "name value" },
    { "try",         "body (catch err handler...)" },
    { "quote",       "expr" },
    { "and",         "expr..." },
    { "or",          "expr..." },

    /* arithmetic / comparison */
    { "+",           "a b ..." },
    { "-",           "a b ..." },
    { "*",           "a b ..." },
    { "/",           "a b ..." },
    { "%",           "a b" },
    { "<",  "a b" }, { ">",  "a b" }, { "<=", "a b" }, { ">=", "a b" },
    { "==", "a b" }, { "!=", "a b" }, { "!",  "x" },

    /* arrays / objects */
    { "array",       "...elements" },
    { "object",      "key val ..." },
    { "get",         "container key..." },
    { "put",         "container key value" },
    { "push",        "array x" },
    { "pop",         "array" },
    { "length",      "x" },
    { "keys",        "obj" },
    { "values",      "obj" },
    { "entries",     "obj" },
    { "contains",    "container needle" },
    { "index-of",    "container needle" },
    { "reverse",     "string-or-array" },
    { "sort",        "array [cmp]" },
    { "chunk",       "array n" },
    { "map",         "fn array" },
    { "filter",      "pred array" },
    { "reduce",      "fn array [init]" },

    /* strings */
    { "concat",      "...parts" },
    { "split",       "sep s" },
    { "join",        "sep array" },
    { "trim",        "s" },
    { "lower",       "s" },
    { "upper",       "s" },
    { "replace",     "s old new" },
    { "substring",   "s start [end]" },
    { "between",     "s from to" },
    { "levenshtein", "a b" },
    { "starts-with", "s prefix" },
    { "ends-with",   "s suffix" },

    /* regex */
    { "regex:test",     "pattern s" },
    { "regex:match",    "pattern s" },
    { "regex:find-all", "pattern s" },
    { "regex:replace",  "pattern s replacement" },
    { "regex:split",    "pattern s" },

    /* math */
    { "min",   "...numbers" }, { "max",   "...numbers" },
    { "floor", "n" }, { "ceil", "n" }, { "round", "n" }, { "trunc", "n" },
    { "abs",   "n" }, { "sign", "n" }, { "mod",   "a b" },
    { "sqrt",  "n" }, { "cbrt", "n" }, { "pow",   "base exponent" },
    { "exp",   "n" }, { "log",  "n" }, { "log2",  "n" }, { "log10", "n" },
    { "sin",   "x" }, { "cos",  "x" }, { "tan",   "x" },
    { "asin",  "x" }, { "acos", "x" }, { "atan",  "x" }, { "atan2", "y x" },
    { "sinh",  "x" }, { "cosh", "x" }, { "tanh",  "x" },
    { "random", "" },
    { "random-int",    "lo hi" },
    { "random-choice", "array" },
    { "shuffle",       "array" },
    { "random-seed",   "n" },
    { "clamp",         "x lo hi" },
    { "lerp",          "a b t" },
    { "is-nan",        "x" },
    { "is-finite",     "x" },
    { "format",        "\"fmt\" args..." },
    { "pad-left",      "s n [ch]" },
    { "pad-right",     "s n [ch]" },
    { "repeat",        "s n" },
    { "count-occurrences", "haystack needle" },
    { "slugify",       "s" },
    { "merge",         "obj/arr ..." },
    { "dissoc",        "obj key" },
    { "select-keys",   "obj keys-array" },
    { "update",        "obj key fn" },
    { "distinct",      "array" },
    { "zip",           "array array ..." },
    { "take",          "n array" },
    { "drop",          "n array" },
    { "take-while",    "pred array" },
    { "drop-while",    "pred array" },
    { "group-by",      "fn array" },
    { "get-in",        "obj path-array" },
    { "assoc-in",      "obj path-array value" },
    { "update-in",     "obj path-array fn" },
    { "parse-date",    "s [fmt]" },
    { "date+",         "ts amount unit" },
    { "date-diff",     "a b unit" },
    { "csv:parse",     "s" },
    { "csv:stringify", "rows" },

    /* core / system */
    { "print",       "...values" },
    { "type",        "v" },
    { "assert",      "cond [msg]" },
    { "sleep",       "seconds" },
    { "help",        "[topic]" },
    { "now",         "" },
    { "timestamp",   "" },
    { "format-date", "ts [fmt]" },
    { "env",         "name" },
    { "exec",        "command" },
    { "run",         "prog [args...]" },
    { "argv",        "" },
    { "exit",        "[code]" },
    { "import",      "path" },
    { "input",       "[prompt]" },
    { "scanf",       "fmt [input]" },

    /* file I/O */
    { "read",        "path" },
    { "read-lines",  "path" },
    { "read-bytes",  "path" },
    { "write",       "path content" },
    { "write-bytes", "path bytes-or-string" },
    { "append",      "path content" },
    { "bytes",       "string-or-array-or-bytes" },
    { "hex",         "bytes-or-string" },
    { "unhex",       "hex-string" },
    { "bytes:get",   "bytes index" },
    { "bytes:slice", "bytes start [end]" },
    { "bytes:concat", "bytes-or-string ..." },
    { "string->bytes", "string" },
    { "bytes->string", "bytes" },
    { "delete",      "path" },
    { "list-dir",    "path" },
    { "file-info",   "path" },
    { "copy-file",   "src dst" },
    { "move-file",   "src dst" },

    /* JSON / encoding / crypto */
    { "json:parse",     "s" },
    { "json:stringify", "v" },
    { "base64:encode",  "s" },
    { "base64:decode",  "s" },
    { "encrypt",        "key text" },
    { "decrypt",        "key cipher" },
    { "uuid",           "" },
    { "md5",            "s" },
    { "sha256",         "s" },
    { "sha512",         "s" },

    /* URL / archive / HTTP */
    { "url:encode",     "s" },
    { "url:decode",     "s" },
    { "url:build",      "base obj" },
    { "zip:create",     "out files" },
    { "zip:extract",    "archive [dest]" },
    { "tar:create",     "out files [compression]" },
    { "tar:extract",    "archive [dest]" },
    { "http:get",       "url [headers]" },
    { "http:post",      "url body [headers]" },

    /* image (optional) */
    { "img:create",        "dst w h [color]" },
    { "img:resize",        "src dst w h" },
    { "img:crop",          "src dst x y w h" },
    { "img:rotate",        "src dst degrees" },
    { "img:compose",       "base overlay dst x y" },
    { "img:replace-color", "src dst from to [fuzz]" },
    { "img:circle",        "src dst cx cy r fill [stroke] [width]" },
    { "img:rect",          "src dst x y w h fill [stroke] [width]" },
    { "img:add-text",      "src dst text [x y size color]" },
    { "img:bw",            "src dst [threshold]" },
    { "img:grayscale",     "src dst" },
    { "img:to-pdf",        "images dst" },
    { "img:qr",            "text dst [scale]" },
    { "img:barcode",       "data dst [type]" },
    { "img:info",          "path" },

    /* vision (optional) */
    { "vision:barcode", "path" },
    { "vision:faces",   "path" },
    { "vision:objects", "path" },

    /* CV (optional — embedded Haar face detection) */
    { "cv:faces",      "image-pgm cascade-zhc [opts]" },
    { "cv:read-pgm",   "path" },
    { "cv:save-pgm",   "path w h bytes" },
    { "cv:image-info", "path" },

    /* OCR (optional) */
    { "ocr:image",  "path [lang]" },
    { "ocr:words",  "path [lang]" },
    { "ocr:lang",   "" },

    /* SQLite + KV (optional) */
    { "sqlite:open",   "path" },
    { "sqlite:exec",   "db sql [params]" },
    { "sqlite:query",  "db sql [params]" },
    { "sqlite:close",  "db" },
    { "sqlite:last-insert-id", "db" },
    { "kv:open",       "path" },
    { "kv:set",        "store key value" },
    { "kv:get",        "store key" },
    { "kv:del",        "store key" },
    { "kv:keys",       "store [prefix]" },

    /* HTML / XML query */
    { "html:query", "selector html" },
    { "html:text",  "html-fragment" },
    { "html:attr",  "name fragment" },
    { "xml:query",  "path xml" },
    { "xml:text",   "xml-fragment" },
    { "xml:attr",   "name fragment" },

    /* z-console primitives */
    { "ui:text",      "line" },
    { "ui:circle",    "cx cy r [color]" },
    { "ui:rect",      "x y w h [color]" },
    { "ui:line",      "x1 y1 x2 y2 [color]" },
    { "ui:text-at",   "x y text [size] [color]" },
    { "ui:image",     "path [x y] [w h]" },
    { "ui:polygon",   "points [color]" },
    { "ui:triangle",  "x1 y1 x2 y2 x3 y3 [color]" },
    { "ui:save-canvas", "path" },
    { "ui:clear",     "" },
};
static const size_t Z_CONSOLE_SIGS_LEN =
    sizeof(Z_CONSOLE_SIGS) / sizeof(Z_CONSOLE_SIGS[0]);

static const char* zc_sig_for(const char* name) {
    for (size_t i = 0; i < Z_CONSOLE_SIGS_LEN; i++)
        if (strcmp(Z_CONSOLE_SIGS[i].name, name) == 0)
            return Z_CONSOLE_SIGS[i].sig;
    return NULL;
}

/* Build "(name p1 p2 ...)" into `out`, returning the buffer.
 *
 * Resolution order:
 *   1. Hardcoded signature table (built-ins).
 *   2. If `env` is non-NULL and the name resolves to a V_FN, walk its
 *      param list and synthesize the signature from those symbol names.
 *   3. Fall back to the cheatsheet example.
 *   4. Empty string if nothing applies. */
static const char* zc_call_shape(const char* name, Env* env,
                                 char* out, size_t out_sz) {
    const char* sig = zc_sig_for(name);
    if (sig) {
        if (*sig) snprintf(out, out_sz, "(%s %s)", name, sig);
        else      snprintf(out, out_sz, "(%s)", name);
        return out;
    }
    if (env) {
        Value* v = env_lookup(env, name);
        if (v && v->type == V_FN) {
            VList* p = &v->as.fn.params->as.list;
            char buf[256];
            size_t off = 0;
            buf[0] = 0;
            for (size_t i = 0; i < p->len && off < sizeof(buf) - 2; i++) {
                Value* sym = p->items[i];
                if (sym->type != V_SYM) continue;
                int n = snprintf(buf + off, sizeof(buf) - off,
                                 "%s%s", i ? " " : "", sym->as.s);
                if (n < 0) break;
                off += (size_t)n;
            }
            if (off > 0) snprintf(out, out_sz, "(%s %s)", name, buf);
            else         snprintf(out, out_sz, "(%s)", name);
            return out;
        }
    }
    const char* ex = zc_example_for(name);
    if (ex) { snprintf(out, out_sz, "%s", ex); return out; }
    out[0] = 0;
    return out;
}

/* ============================================================
 * Autocomplete popup — when the caret sits at the end of a symbol token
 * the GUI shows a floating list of all candidates that start with what
 * the user typed. ↑/↓ highlight an item, Tab/Enter insert it, Esc hides.
 * ============================================================ */

static const char* Z_CONSOLE_FORMS[] = {
    "do", "if", "when", "unless", "cond", "let",
    "while", "for", "fn", "lambda", "set", "try", "catch",
    "and", "or", "quote", "else", "->", "->>", NULL
};

#define ZC_MAX_CANDIDATES 256

typedef struct {
    const char* items[ZC_MAX_CANDIDATES];  /* borrowed pointers — never freed */
    int count;
    int selected;                          /* index in items, or -1 */
    int token_start;                       /* index in input_buf where the
                                            * current symbol started */
    int token_len;                         /* length already typed */
    /* When the user has finished typing a function name and moved on to
     * arguments, the popup switches to a single-row hint that displays the
     * function's example until the call closes or Enter runs the line. */
    int  hint_mode;
    char hint_name[64];
} ZcAutocomplete;

static int zc_strcmp_qsort(const void* a, const void* b) {
    return strcmp(*(const char**)a, *(const char**)b);
}

/* Find the symbol-like token ending at the cursor. Returns its length and
 * sets *token_start. Only returns >0 when the cursor sits at the end of a
 * token (so editing the middle of a word doesn't trigger the popup). */
static int zc_token_at_cursor(const char* buf, int len, int cursor,
                              int* token_start) {
    int s = cursor;
    while (s > 0 && is_sym_cont((unsigned char)buf[s-1])) s--;
    int e = cursor;
    while (e < len && is_sym_cont((unsigned char)buf[e])) e++;
    *token_start = s;
    if (e != cursor) return 0;
    return e - s;
}

/* Find the function name at the head of the innermost unclosed `(` to the
 * left of the cursor. Writes it into `out`. Returns the length, or 0. */
static int zc_find_call_head(const char* buf, int cursor,
                             char* out, size_t out_sz) {
    int depth = 0;
    int paren_pos = -1;
    for (int i = cursor - 1; i >= 0; i--) {
        char c = buf[i];
        if (c == ')') depth++;
        else if (c == '(') {
            if (depth == 0) { paren_pos = i; break; }
            depth--;
        }
    }
    if (paren_pos < 0) return 0;
    int s = paren_pos + 1;
    while (buf[s] == ' ' || buf[s] == '\t') s++;
    int e = s;
    while (buf[e] && is_sym_cont((unsigned char)buf[e])) e++;
    int n = e - s;
    if (n <= 0) return 0;
    if (n >= (int)out_sz) n = (int)out_sz - 1;
    memcpy(out, buf + s, n);
    out[n] = 0;
    return n;
}

/* Populate `ac` with every form / env name that starts with the symbol
 * the user is typing. Resets selection if the candidate set changed.
 *
 * If the user is no longer typing a symbol (e.g., already on the args of
 * a call), we fall back to "hint mode": the popup shows a single row with
 * the example for the current call's head function. */
static void zc_autocomplete_update(ZcAutocomplete* ac,
                                   const char* buf, int len, int cursor,
                                   Env* env) {
    int tstart;
    int tlen = zc_token_at_cursor(buf, len, cursor, &tstart);
    int prev_token_start = ac->token_start;
    int prev_token_len   = ac->token_len;
    ac->token_start = tstart;
    ac->token_len   = tlen;
    ac->count       = 0;
    ac->hint_mode   = 0;

    /* Not actively typing a symbol → maybe show a signature hint instead. */
    if (tlen == 0) {
        ac->selected = -1;
        char head[64];
        int n = zc_find_call_head(buf, cursor, head, sizeof(head));
        if (n > 0 && zc_example_for(head)) {
            ac->hint_mode = 1;
            memcpy(ac->hint_name, head, n);
            ac->hint_name[n] = 0;
            ac->items[0] = ac->hint_name;
            ac->count = 1;
        }
        return;
    }

    const char* prefix = buf + tstart;
    /* Special forms first. */
    for (int i = 0; Z_CONSOLE_FORMS[i] && ac->count < ZC_MAX_CANDIDATES; i++)
        if (strncmp(Z_CONSOLE_FORMS[i], prefix, tlen) == 0)
            ac->items[ac->count++] = Z_CONSOLE_FORMS[i];
    /* Walk the env chain. */
    for (Env* c = env; c && ac->count < ZC_MAX_CANDIDATES; c = c->parent) {
        for (size_t i = 0; i < c->vars.len && ac->count < ZC_MAX_CANDIDATES; i++) {
            const char* k = c->vars.keys[i];
            if (strncmp(k, prefix, tlen) != 0) continue;
            /* Dedup — env can shadow forms or repeat across scopes. */
            int dup = 0;
            for (int j = 0; j < ac->count; j++)
                if (strcmp(ac->items[j], k) == 0) { dup = 1; break; }
            if (!dup) ac->items[ac->count++] = k;
        }
    }
    qsort(ac->items, ac->count, sizeof(const char*), zc_strcmp_qsort);

    /* If the token grew or shrank, reset selection so the user always sees
     * the first match highlighted as soon as they start a new word. */
    if (tstart != prev_token_start || tlen != prev_token_len)
        ac->selected = ac->count > 0 ? 0 : -1;
    else if (ac->selected >= ac->count)
        ac->selected = ac->count - 1;
    else if (ac->selected < 0 && ac->count > 0)
        ac->selected = 0;
}

/* Replace input_buf[token_start..cursor] with the chosen candidate, leaving
 * the caret right after the inserted name. */
static void zc_autocomplete_apply(ZcAutocomplete* ac,
                                  char* buf, int* len, int* cursor,
                                  int bufsz) {
    if (ac->count == 0 || ac->selected < 0) return;
    const char* pick = ac->items[ac->selected];
    int picklen = (int)strlen(pick);
    int new_len = *len - ac->token_len + picklen;
    if (new_len + 1 >= bufsz) return;
    /* Shift the tail (everything after the token we're replacing). */
    int tail_src = ac->token_start + ac->token_len;
    int tail_dst = ac->token_start + picklen;
    memmove(buf + tail_dst, buf + tail_src, *len - tail_src + 1);
    memcpy(buf + ac->token_start, pick, picklen);
    *len    = new_len;
    *cursor = tail_dst;
    /* Force recompute on next frame. */
    ac->count = 0;
    ac->token_len = 0;
}

/* ============================================================
 * Main loop
 * ============================================================ */

#define INPUT_MAX 4096

int main(int argc, char** argv) {
    srand((unsigned)time(NULL));
    g_prog_argc = argc;
    g_prog_argv = argv;

    /* Wire the cancellable-eval hook before any builtins run, so an
     * interrupt fired during a long evaluation can propagate up. The hook
     * pumps raylib events without drawing, then checks Ctrl+Period. */
    z_eval_tick = zc_eval_tick;

    Env* env = env_new(NULL);
    install_builtins(env);
    env_define(env, "print",      v_native(b_print_to_cell));
    env_define(env, "ui:text",    v_native(b_ui_text));
    env_define(env, "ui:circle",  v_native(b_ui_circle));
    env_define(env, "ui:rect",    v_native(b_ui_rect));
    env_define(env, "ui:line",    v_native(b_ui_line));
    env_define(env, "ui:text-at", v_native(b_ui_text_at));
    env_define(env, "ui:image",   v_native(b_ui_image));
    env_define(env, "ui:polygon", v_native(b_ui_polygon));
    env_define(env, "ui:triangle",v_native(b_ui_triangle));
    env_define(env, "ui:save-canvas", v_native(b_ui_save_canvas));
    env_define(env, "ui:clear",   v_native(b_ui_clear));

    hist_load();

    /* Load persisted theme + window size before opening the window. */
    ZcState state;
    zc_state_load(&state);
    if (state.theme_idx >= 0 && state.theme_idx < ZC_THEME_COUNT)
        g_theme_idx = state.theme_idx;

    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT | FLAG_VSYNC_HINT);
    InitWindow(state.window_w > 0 ? state.window_w : 900,
               state.window_h > 0 ? state.window_h : 640,
               "z-console");
    SetTargetFPS(60);
    SetExitKey(0);
    /* Must be called after InitWindow — needs an active GL context.
     * argv[0] lets us look for a bundled JetBrainsMono-Regular.ttf next
     * to the executable, not just in $PWD. */
    load_console_font(argc > 0 ? argv[0] : NULL);

    char input_buf[INPUT_MAX] = {0};
    int  input_len = 0;
    int  input_cursor = 0;
    int  hist_pos = -1;
    char input_saved[INPUT_MAX] = {0};

    float scroll_y = 0;
    int   need_autoscroll = 0;

    /* Theme-driven colour aliases. Each macro reads the active theme,
     * so Ctrl+T takes effect on the very next frame without restarting. */
    #define BG          (TC.bg)
    #define INPUT_BG    (TC.input_bg)
    #define PROMPT_FG   (TC.prompt_fg)
    #define INPUT_ECHO  (TC.input_echo)
    #define TEXT_FG     (TC.text_fg)
    #define ERR_FG      (TC.err_fg)
    #define BORDER      (TC.border)
    #define CANVAS_BG   (TC.canvas_bg)
    #define CANVAS_LINE (TC.canvas_line)
    #define AC_BG       (TC.popup_bg)
    #define AC_SEL_BG   (TC.popup_sel_bg)
    #define AC_FG       (TC.popup_fg)
    #define AC_DIM_FG   (TC.popup_dim_fg)
    #define AC_BORDER   (TC.popup_border)
    #define AC_MATCH_FG (TC.popup_match)

    ZcAutocomplete autocomplete = (ZcAutocomplete){0};
    autocomplete.selected = -1;

    /* Per-frame bookkeeping. Hit-test rects for each visible cell's In[N]
     * label, so clicking one loads that cell back into the prompt. */
    #define MAX_HITS 256
    typedef struct { float x, y, w, h; int cell_idx; } ZcCellHit;
    ZcCellHit cell_hits[MAX_HITS];
    int cell_hits_n = 0;

    int show_help_overlay = 0;

    /* Mouse selection state. Line-granular: each line in cell text_out is a
     * "selectable line". We track every visible line's bbox + the source
     * char range so Ctrl+C can re-assemble the text. */
    #define MAX_LINES 4096
    typedef struct {
        float x, y, w, h;
        int   cell_idx;
        int   start, end;     /* byte indices into cell->text_out */
    } ZcLine;
    ZcLine lines[MAX_LINES];
    int  lines_n = 0;
    int  sel_active = 0;       /* mouse currently dragging */
    int  sel_anchor = -1;      /* line index where the drag started */
    int  sel_start  = -1;
    int  sel_end    = -1;

    int FS_INPUT = state.font_size > 0 ? state.font_size : 18;
    int FS_ECHO  = FS_INPUT - 2;
    int FS_TEXT  = FS_INPUT - 4;
    /* Line-height derived from font size; recomputed each frame so the
     * Ctrl+= / Ctrl+- hotkeys scale spacing along with the type. */
    #define LH_ECHO  (FS_ECHO + 3)
    #define LH_TEXT  (FS_TEXT + 3)
    #define CELL_GAP 4
    #define MAX_CELLS 200   /* drop oldest cells beyond this (with texture cleanup) */
    double last_eval_ms = 0.0;

    while (!WindowShouldClose()) {
        int W = GetScreenWidth();
        int H = GetScreenHeight();
        /* input_h grows with newlines in the prompt so multi-line s-expressions
         * are fully visible while typing. */
        int prompt_lines = 1;
        for (int i = 0; i < input_len; i++)
            if (input_buf[i] == '\n') prompt_lines++;
        int input_h  = 24 + prompt_lines * (FS_INPUT + 4);
        int status_h = 18;
        int output_h = H - input_h - status_h;

        int c;
        while ((c = GetCharPressed()) != 0) {
            if (input_len + 1 < INPUT_MAX) {
                memmove(input_buf + input_cursor + 1,
                        input_buf + input_cursor,
                        input_len - input_cursor + 1);
                input_buf[input_cursor++] = (char)c;
                input_len++;
            }
        }
        if (IsKeyPressed(KEY_BACKSPACE) && input_cursor > 0) {
            memmove(input_buf + input_cursor - 1,
                    input_buf + input_cursor,
                    input_len - input_cursor + 1);
            input_cursor--; input_len--;
        }
        if (IsKeyPressed(KEY_DELETE) && input_cursor < input_len) {
            memmove(input_buf + input_cursor,
                    input_buf + input_cursor + 1,
                    input_len - input_cursor);
            input_len--;
        }
        int mod_ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_LEFT_SUPER);

        /* Word-jump with Ctrl+←/→. Falls through to plain left/right otherwise. */
        if (IsKeyPressed(KEY_LEFT)) {
            if (mod_ctrl) input_cursor = zc_word_left(input_buf, input_cursor);
            else if (input_cursor > 0) input_cursor--;
        }
        if (IsKeyPressed(KEY_RIGHT)) {
            if (mod_ctrl) input_cursor = zc_word_right(input_buf, input_len, input_cursor);
            else if (input_cursor < input_len) input_cursor++;
        }
        /* Home / End move within the current line. Ctrl+Home / Ctrl+End
         * still jump to the start / end of the whole buffer. */
        if (IsKeyPressed(KEY_HOME))
            input_cursor = mod_ctrl ? 0 : zc_line_start(input_buf, input_cursor);
        if (IsKeyPressed(KEY_END))
            input_cursor = mod_ctrl ? input_len
                                    : zc_line_end(input_buf, input_len, input_cursor);

        /* Ctrl+W — kill word backward. zc_word_left already stops at
         * newlines, so this never reaches into the previous line. */
        if (mod_ctrl && IsKeyPressed(KEY_W) && input_cursor > 0) {
            int wstart = zc_word_left(input_buf, input_cursor);
            memmove(input_buf + wstart, input_buf + input_cursor,
                    input_len - input_cursor + 1);
            input_len    -= input_cursor - wstart;
            input_cursor  = wstart;
        }
        /* Ctrl+U — kill from cursor back to the start of the current line. */
        if (mod_ctrl && IsKeyPressed(KEY_U) && input_cursor > 0) {
            int ls = zc_line_start(input_buf, input_cursor);
            if (ls < input_cursor) {
                memmove(input_buf + ls, input_buf + input_cursor,
                        input_len - input_cursor + 1);
                input_len    -= input_cursor - ls;
                input_cursor  = ls;
            }
        }
        /* Ctrl+K — kill from cursor to the end of the current line.
         * If already at end-of-line, nibble the following '\n' so the
         * line is joined with the next. */
        if (mod_ctrl && IsKeyPressed(KEY_K) && input_cursor < input_len) {
            int le = zc_line_end(input_buf, input_len, input_cursor);
            if (le > input_cursor) {
                memmove(input_buf + input_cursor, input_buf + le,
                        input_len - le + 1);
                input_len -= le - input_cursor;
            } else if (le < input_len && input_buf[le] == '\n') {
                memmove(input_buf + le, input_buf + le + 1,
                        input_len - le);
                input_len--;
            }
        }
        /* Ctrl+C / Ctrl+V / Ctrl+X — system clipboard.
         * Ctrl+L is used by clear-screen and is handled separately below. */
        if (mod_ctrl && IsKeyPressed(KEY_V)) {
            const char* clip = GetClipboardText();
            if (clip && *clip) {
                int add = (int)strlen(clip);
                if (input_len + add + 1 < INPUT_MAX) {
                    memmove(input_buf + input_cursor + add,
                            input_buf + input_cursor,
                            input_len - input_cursor + 1);
                    memcpy(input_buf + input_cursor, clip, add);
                    input_cursor += add;
                    input_len    += add;
                }
            }
        }
        if (mod_ctrl && IsKeyPressed(KEY_C) && input_len > 0) {
            SetClipboardText(input_buf);
        }
        if (mod_ctrl && IsKeyPressed(KEY_X) && input_len > 0) {
            SetClipboardText(input_buf);
            input_buf[0] = 0; input_len = 0; input_cursor = 0;
        }
        /* Ctrl+S — save the session (all cell inputs) to a timestamped .z. */
        if (mod_ctrl && IsKeyPressed(KEY_S)) {
            char path[512];
            time_t t = time(NULL); struct tm tm; localtime_r(&t, &tm);
            const char* home = getenv("HOME");
            snprintf(path, sizeof(path),
                     "%s/.z_console_session_%04d%02d%02d_%02d%02d%02d.z",
                     home ? home : ".",
                     tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                     tm.tm_hour, tm.tm_min, tm.tm_sec);
            FILE* f = fopen(path, "w");
            if (f) {
                fprintf(f, "; z-console session, %d cells, saved %s",
                        g_cell_count, ctime(&t));
                for (int i = 0; i < g_cell_count; i++)
                    fprintf(f, "%s\n", g_cells[i].input ? g_cells[i].input : "");
                fclose(f);
                /* Surface the path in a fresh cell so the user sees where it landed. */
                if (g_cell_count + 1 <= g_cell_cap || (g_cell_cap = g_cell_cap ? g_cell_cap*2 : 8, g_cells = (Cell*)realloc(g_cells, g_cell_cap*sizeof(Cell)))) {
                    Cell* note = &g_cells[g_cell_count++];
                    memset(note, 0, sizeof(*note));
                    note->input = str_dup("; session saved");
                    char msg[600]; snprintf(msg, sizeof(msg), "→ %s\n", path);
                    cell_append_text(note, msg);
                }
            }
        }
        /* Ctrl+T — cycle through the available themes. */
        if (mod_ctrl && IsKeyPressed(KEY_T)) {
            g_theme_idx = (g_theme_idx + 1) % ZC_THEME_COUNT;
        }
        /* F1 — toggle the help overlay. */
        if (IsKeyPressed(KEY_F1)) show_help_overlay = !show_help_overlay;
        if (show_help_overlay && IsKeyPressed(KEY_ESCAPE)) show_help_overlay = 0;
        /* Ctrl+= and Ctrl+-  — font-size hotkeys. */
        if (mod_ctrl && (IsKeyPressed(KEY_EQUAL) || IsKeyPressed(KEY_KP_ADD))) {
            if (FS_INPUT < 36) { FS_INPUT++; FS_ECHO++; FS_TEXT++; }
        }
        if (mod_ctrl && (IsKeyPressed(KEY_MINUS) || IsKeyPressed(KEY_KP_SUBTRACT))) {
            if (FS_INPUT > 10) { FS_INPUT--; FS_ECHO--; FS_TEXT--; }
        }
        /* Drag-and-drop: .z runs the file as a cell; image extensions become
         * a (ui:image "path") at the prompt. */
        if (IsFileDropped()) {
            FilePathList drops = LoadDroppedFiles();
            for (unsigned int i = 0; i < drops.count; i++) {
                const char* p = drops.paths[i];
                size_t L = strlen(p);
                int is_z   = L > 2 && strcmp(p + L - 2, ".z") == 0;
                int is_img = L > 4 && (strcmp(p + L - 4, ".png") == 0
                                    || strcmp(p + L - 4, ".jpg") == 0
                                    || strcmp(p + L - 4, ".bmp") == 0
                                    || strcmp(p + L - 4, ".tga") == 0
                                    || strcmp(p + L - 4, ".gif") == 0)
                          || (L > 5 && strcmp(p + L - 5, ".jpeg") == 0);
                if (is_z) {
                    /* Run the file as if it were typed at the prompt. */
                    char* src = read_file_all(p);
                    if (src) {
                        size_t n = strlen(src);
                        if (n + 1 < INPUT_MAX) {
                            memcpy(input_buf, src, n);
                            input_buf[n] = 0;
                            input_len = (int)n;
                            input_cursor = input_len;
                        }
                        free(src);
                    }
                } else if (is_img) {
                    char snippet[600];
                    snprintf(snippet, sizeof(snippet), "(ui:image \"%s\")", p);
                    int add = (int)strlen(snippet);
                    if (input_len + add + 1 < INPUT_MAX) {
                        memmove(input_buf + input_cursor + add,
                                input_buf + input_cursor,
                                input_len - input_cursor + 1);
                        memcpy(input_buf + input_cursor, snippet, add);
                        input_cursor += add;
                        input_len    += add;
                    }
                }
            }
            UnloadDroppedFiles(drops);
        }

        /* Autocomplete keybindings — only when in completion mode (not hint
         * mode), so arg-typing keeps Tab/Enter/arrows working normally. */
        if (autocomplete.count > 0 && !autocomplete.hint_mode) {
            if (IsKeyPressed(KEY_UP)) {
                autocomplete.selected = autocomplete.selected <= 0
                    ? autocomplete.count - 1
                    : autocomplete.selected - 1;
            } else if (IsKeyPressed(KEY_DOWN)) {
                autocomplete.selected = (autocomplete.selected + 1) % autocomplete.count;
            } else if (IsKeyPressed(KEY_TAB) || IsKeyPressed(KEY_ENTER)) {
                zc_autocomplete_apply(&autocomplete, input_buf,
                                      &input_len, &input_cursor, INPUT_MAX);
                /* Swallow this Enter so it doesn't also fire the eval below. */
                if (IsKeyPressed(KEY_ENTER)) goto skip_enter;
            } else if (IsKeyPressed(KEY_ESCAPE)) {
                autocomplete.count = 0;
                autocomplete.selected = -1;
            }
        } else if (autocomplete.hint_mode && IsKeyPressed(KEY_ESCAPE)) {
            /* Esc dismisses the hint until the input changes again. */
            autocomplete.count = 0;
            autocomplete.hint_mode = 0;
        }

        if (IsKeyPressed(KEY_UP) && (autocomplete.count == 0 || autocomplete.hint_mode)) {
            /* Edit-and-rerun shortcut: Up on an empty buffer loads the most
             * recent cell's input back into the prompt. */
            if (input_len == 0 && g_cell_count > 0 && hist_pos == -1) {
                const char* prev = g_cells[g_cell_count - 1].input;
                if (prev) {
                    strncpy(input_buf, prev, INPUT_MAX - 1);
                    input_buf[INPUT_MAX - 1] = 0;
                    input_len    = (int)strlen(input_buf);
                    input_cursor = input_len;
                    goto skip_history_up;
                }
            }
            if (hist_pos == -1) {
                memcpy(input_saved, input_buf, input_len + 1);
                hist_pos = g_history.count;
            }
            if (hist_pos > 0) {
                hist_pos--;
                const char* l = hist_get(hist_pos);
                if (l) {
                    strncpy(input_buf, l, INPUT_MAX - 1);
                    input_buf[INPUT_MAX - 1] = 0;
                    input_len = (int)strlen(input_buf);
                    input_cursor = input_len;
                }
            }
            skip_history_up:;
        }
        if (IsKeyPressed(KEY_DOWN) && hist_pos >= 0) {
            hist_pos++;
            if (hist_pos >= g_history.count) {
                hist_pos = -1;
                strcpy(input_buf, input_saved);
            } else {
                const char* l = hist_get(hist_pos);
                if (l) {
                    strncpy(input_buf, l, INPUT_MAX - 1);
                    input_buf[INPUT_MAX - 1] = 0;
                }
            }
            input_len = (int)strlen(input_buf);
            input_cursor = input_len;
        }

        if ((IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_LEFT_SUPER))
            && IsKeyPressed(KEY_L)) {
            for (int i = 0; i < g_cell_count; i++) {
                free(g_cells[i].input);
                free(g_cells[i].text_out);
                for (int j = 0; j < g_cells[i].op_count; j++) {
                    free(g_cells[i].ops[j].text);
                    if (g_cells[i].ops[j].tex_loaded)
                        UnloadTexture(g_cells[i].ops[j].tex);
                }
                free(g_cells[i].ops);
            }
            g_cell_count = 0;
            scroll_y = 0;
        }

        /* Multi-line input. Shift+Enter always inserts a newline; plain Enter
         * inserts a newline when parens aren't yet balanced, otherwise runs.
         * Inside an open string, Enter also adds a newline. */
        int shift_down = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
        if (IsKeyPressed(KEY_ENTER)) {
            int depth = zc_paren_balance(input_buf, input_len);
            if (shift_down || depth > 0) {
                if (input_len + 1 < INPUT_MAX) {
                    memmove(input_buf + input_cursor + 1,
                            input_buf + input_cursor,
                            input_len - input_cursor + 1);
                    input_buf[input_cursor++] = '\n';
                    input_len++;
                }
                /* Skip the eval block this frame. */
                goto after_enter;
            }
        }

        if (IsKeyPressed(KEY_ENTER) && input_len > 0) {
            /* Trim oldest cells if we're above MAX_CELLS to bound memory and
             * GPU resources (each ui:image cell owns a Texture2D). */
            if (g_cell_count >= MAX_CELLS) {
                int drop = g_cell_count - MAX_CELLS + 1;
                for (int i = 0; i < drop; i++) {
                    free(g_cells[i].input);
                    free(g_cells[i].text_out);
                    for (int j = 0; j < g_cells[i].op_count; j++) {
                        free(g_cells[i].ops[j].text);
                        if (g_cells[i].ops[j].tex_loaded)
                            UnloadTexture(g_cells[i].ops[j].tex);
                    }
                    free(g_cells[i].ops);
                }
                memmove(g_cells, g_cells + drop,
                        (g_cell_count - drop) * sizeof(Cell));
                g_cell_count -= drop;
            }
            if (g_cell_count + 1 > g_cell_cap) {
                g_cell_cap = g_cell_cap ? g_cell_cap * 2 : 8;
                g_cells = (Cell*)realloc(g_cells, g_cell_cap * sizeof(Cell));
            }
            Cell* cell = &g_cells[g_cell_count++];
            memset(cell, 0, sizeof(*cell));
            cell->input = str_dup(input_buf);
            g_current = cell;

            double t0 = GetTime();
            ErrFrame f; f.prev = g_err_top; f.value = NULL; f.msg[0] = 0;
            g_err_top = &f;
            if (setjmp(f.buf) == 0) {
                Value* r = run_source(input_buf, env);
                g_err_top = f.prev;
                if (r && r->type != V_NULL
                    && (!cell->text_out || !cell->text_out[0])
                    && cell->op_count == 0) {
                    char* s = value_to_cstr(r);
                    cell_append_text(cell, s);
                    cell_append_text(cell, "\n");
                    free(s);
                }
            } else {
                g_err_top = f.prev;
                cell_append_text(cell, "error: ");
                cell_append_text(cell, f.msg);
                cell_append_text(cell, "\n");
                cell->is_error = 1;
            }
            last_eval_ms = (GetTime() - t0) * 1000.0;

            hist_add(input_buf);
            input_buf[0] = 0; input_len = 0; input_cursor = 0;
            hist_pos = -1;
            g_current = NULL;
            need_autoscroll = 1;
        }
        skip_enter:;
        after_enter:;

        /* Recompute autocomplete candidates against the current input. */
        zc_autocomplete_update(&autocomplete, input_buf, input_len,
                               input_cursor, env);

        float wheel = GetMouseWheelMove();
        scroll_y -= wheel * 30;
        if (scroll_y < 0) scroll_y = 0;

        BeginDrawing();
        ClearBackground(BG);

        int content_h = 10;
        for (int i = 0; i < g_cell_count; i++) {
            Cell* c = &g_cells[i];
            content_h += LH_ECHO;
            if (c->text_out && c->text_out[0]) {
                for (const char* p = c->text_out; *p; p++)
                    if (*p == '\n') content_h += LH_TEXT;
            }
            if (c->canvas_h > 0) content_h += c->canvas_h + 8;
            content_h += CELL_GAP;
        }

        if (need_autoscroll) {
            scroll_y = content_h > output_h ? content_h - output_h + 10 : 0;
            need_autoscroll = 0;
        }
        if (content_h > output_h && scroll_y > content_h - output_h)
            scroll_y = content_h - output_h;

        cell_hits_n = 0;
        lines_n     = 0;
        BeginScissorMode(0, 0, W, output_h);
        int y = 10 - (int)scroll_y;
        for (int i = 0; i < g_cell_count; i++) {
            Cell* c = &g_cells[i];

            /* Cell numbering + colourised, multi-line echo. */
            char label[16];
            snprintf(label, sizeof(label), "In[%d]:", i + 1);
            zc_draw_text(label, 4, y, FS_ECHO, INPUT_ECHO);
            int label_px = zc_measure_text(label, FS_ECHO) + 12;
            /* Record a clickable rect over the label so the user can jump
             * back to this cell with a click (loads input) or shift-click
             * (loads + immediately re-runs). */
            if (cell_hits_n < MAX_HITS) {
                cell_hits[cell_hits_n++] = (ZcCellHit){
                    4.0f, (float)y, (float)label_px, (float)LH_ECHO, i
                };
            }
            const char* echo = c->input ? c->input : "";
            const char* ln_s = echo;
            while (*ln_s) {
                const char* ln_e = strchr(ln_s, '\n');
                int ln_len = ln_e ? (int)(ln_e - ln_s) : (int)strlen(ln_s);
                zc_draw_highlighted(ln_s, ln_len, label_px, y, FS_ECHO, env, -1);
                y += LH_ECHO;
                if (!ln_e) break;
                ln_s = ln_e + 1;
            }

            if (c->text_out && c->text_out[0]) {
                const char* base = c->text_out;
                const char* line = base;
                while (*line) {
                    const char* nl = strchr(line, '\n');
                    int len = nl ? (int)(nl - line) : (int)strlen(line);
                    char tmp[1024];
                    int copy = len < (int)sizeof(tmp) - 1 ? len : (int)sizeof(tmp) - 1;
                    memcpy(tmp, line, copy);
                    tmp[copy] = 0;
                    int line_idx_now = lines_n;
                    /* Record this line for hit-testing + Ctrl+C extraction. */
                    if (lines_n < MAX_LINES) {
                        lines[lines_n++] = (ZcLine){
                            20.0f, (float)y - 2, (float)(W - 40), (float)LH_TEXT,
                            i,
                            (int)(line - base),
                            (int)(line - base) + len
                        };
                    }
                    /* Selection highlight underlay. */
                    int sel_lo = sel_start < sel_end ? sel_start : sel_end;
                    int sel_hi = sel_start < sel_end ? sel_end   : sel_start;
                    if (sel_start >= 0 && sel_end >= 0
                        && line_idx_now >= sel_lo && line_idx_now <= sel_hi) {
                        DrawRectangle(20, y - 2, W - 40, LH_TEXT,
                                      (Color){TC.popup_sel_bg.r,
                                              TC.popup_sel_bg.g,
                                              TC.popup_sel_bg.b, 110});
                    }
                    zc_draw_text(tmp, 24, y, FS_TEXT, c->is_error ? ERR_FG : TEXT_FG);
                    y += LH_TEXT;
                    if (!nl) break;
                    line = nl + 1;
                }
            }

            if (c->canvas_h > 0) {
                int cx = 24, cy = y, cw = W - 48, ch = c->canvas_h;
                /* Only fill if the theme requests a visible canvas color;
                 * a fully-transparent CANVAS_BG lets the cell sit on the
                 * window background. */
                if (CANVAS_BG.a   > 0) DrawRectangle      (cx, cy, cw, ch, CANVAS_BG);
                if (CANVAS_LINE.a > 0) DrawRectangleLines(cx, cy, cw, ch, CANVAS_LINE);
                BeginScissorMode(cx, cy, cw, ch);
                for (int j = 0; j < c->op_count; j++) {
                    DrawOp* op = &c->ops[j];
                    switch (op->kind) {
                        case OP_CIRCLE:
                            DrawCircle(cx + (int)op->x, cy + (int)op->y, op->r, op->color);
                            break;
                        case OP_RECT:
                            DrawRectangle(cx + (int)op->x, cy + (int)op->y,
                                          (int)op->w, (int)op->h, op->color);
                            break;
                        case OP_LINE:
                            DrawLineEx((Vector2){cx + op->x,  cy + op->y},
                                       (Vector2){cx + op->x2, cy + op->y2},
                                       2.0f, op->color);
                            break;
                        case OP_TEXT_AT:
                            zc_draw_text(op->text ? op->text : "",
                                         cx + (int)op->x, cy + (int)op->y,
                                         op->size > 0 ? op->size : 20, op->color);
                            break;
                        case OP_IMAGE:
                            if (op->tex_loaded) {
                                Rectangle src = { 0, 0,
                                    (float)op->tex.width,
                                    (float)op->tex.height };
                                Rectangle dst = {
                                    (float)(cx + (int)op->x),
                                    (float)(cy + (int)op->y),
                                    op->w, op->h };
                                DrawTexturePro(op->tex, src, dst,
                                               (Vector2){0, 0}, 0.0f, WHITE);
                            }
                            break;
                    }
                }
                EndScissorMode();
                y += ch + 8;
            }
            y += CELL_GAP;
        }
        EndScissorMode();

        /* Click any In[N]: label to reload that cell's input into the prompt.
         * Shift-click re-runs immediately. */
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            float mx = (float)GetMouseX(), my = (float)GetMouseY();
            int hit_label = 0;
            for (int i = 0; i < cell_hits_n; i++) {
                ZcCellHit* h = &cell_hits[i];
                if (mx < h->x || mx > h->x + h->w
                 || my < h->y || my > h->y + h->h) continue;
                hit_label = 1;
                const char* src = g_cells[h->cell_idx].input;
                if (!src) break;
                strncpy(input_buf, src, INPUT_MAX - 1);
                input_buf[INPUT_MAX - 1] = 0;
                input_len    = (int)strlen(input_buf);
                input_cursor = input_len;
                hist_pos = -1;
                break;
            }
            /* Not on a label → start a text selection if we hit an output line. */
            if (!hit_label && my < output_h) {
                int found = -1;
                for (int i = 0; i < lines_n; i++) {
                    if (my >= lines[i].y && my <= lines[i].y + lines[i].h) {
                        found = i; break;
                    }
                }
                if (found >= 0) {
                    sel_active = 1;
                    sel_anchor = found;
                    sel_start  = found;
                    sel_end    = found;
                } else {
                    /* Click in empty area clears the selection. */
                    sel_start = sel_end = sel_anchor = -1;
                }
            }
        }
        /* Drag updates the selection extent. */
        if (sel_active && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
            float my = (float)GetMouseY();
            int found = -1;
            for (int i = 0; i < lines_n; i++) {
                if (my >= lines[i].y && my <= lines[i].y + lines[i].h) {
                    found = i; break;
                }
            }
            if (found < 0 && lines_n > 0) {
                /* Mouse past last line → extend to end. */
                if (my > lines[lines_n-1].y) found = lines_n - 1;
                else                          found = 0;
            }
            if (found >= 0) sel_end = found;
        }
        if (sel_active && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
            sel_active = 0;
        }
        /* Ctrl+C with an active selection copies the joined text. */
        if (mod_ctrl && IsKeyPressed(KEY_C) && sel_start >= 0 && sel_end >= 0
            && input_len == 0) {
            int lo = sel_start < sel_end ? sel_start : sel_end;
            int hi = sel_start < sel_end ? sel_end   : sel_start;
            size_t total = 0;
            for (int i = lo; i <= hi && i < lines_n; i++)
                total += (size_t)(lines[i].end - lines[i].start) + 1;
            char* buf = (char*)malloc(total + 1);
            if (buf) {
                size_t off = 0;
                for (int i = lo; i <= hi && i < lines_n; i++) {
                    Cell* c = &g_cells[lines[i].cell_idx];
                    if (!c->text_out) continue;
                    int n = lines[i].end - lines[i].start;
                    memcpy(buf + off, c->text_out + lines[i].start, n);
                    off += n;
                    buf[off++] = '\n';
                }
                buf[off] = 0;
                SetClipboardText(buf);
                free(buf);
            }
        }

        /* Live error feedback — a thin strip immediately above the prompt
         * surfaces issues we can detect statically. Mostly silent: the
         * strip stays invisible while the input is well-formed. */
        char diag[256] = {0};
        if (input_len > 0) {
            int depth = zc_paren_balance(input_buf, input_len);
            int unterm = zc_string_unterminated(input_buf, input_len);
            char unknowns[6][32];
            int  nuk = zc_collect_unknown_calls(input_buf, input_len, env,
                                                unknowns, 6);
            size_t off = 0;
            if (depth > 0) {
                int n = snprintf(diag + off, sizeof(diag) - off,
                                 "  %d unclosed paren%s", depth, depth==1?"":"s");
                if (n > 0) off += (size_t)n;
            } else if (depth < 0) {
                int n = snprintf(diag + off, sizeof(diag) - off,
                                 "  %d extra `)`", -depth);
                if (n > 0) off += (size_t)n;
            }
            if (unterm) {
                int n = snprintf(diag + off, sizeof(diag) - off,
                                 "%sstring not closed",
                                 off ? "  ·  " : "  ");
                if (n > 0) off += (size_t)n;
            }
            if (nuk > 0) {
                int n = snprintf(diag + off, sizeof(diag) - off,
                                 "%sunknown: %s", off ? "  ·  " : "  ",
                                 unknowns[0]);
                if (n > 0) off += (size_t)n;
                for (int k = 1; k < nuk && off < sizeof(diag) - 4; k++) {
                    n = snprintf(diag + off, sizeof(diag) - off,
                                 ", %s", unknowns[k]);
                    if (n > 0) off += (size_t)n;
                }
            }
        }
        if (diag[0]) {
            int strip_h = FS_TEXT + 6;
            DrawRectangle(0, output_h - strip_h, W, strip_h,
                          (Color){45,30,30,235});
            DrawLine(0, output_h - strip_h, W, output_h - strip_h, BORDER);
            zc_draw_text(diag, 8, output_h - strip_h + 2,
                         FS_TEXT, (Color){255,180,120,255});
        }

        DrawRectangle(0, output_h, W, input_h, INPUT_BG);
        DrawLine(0, output_h, W, output_h, BORDER);
        zc_draw_text("z>", 12, output_h + 12, FS_INPUT, PROMPT_FG);

        int prompt_w  = zc_measure_text("z>", FS_INPUT) + 20;
        int avail_w   = W - prompt_w - 12;

        /* Find a matching paren if the caret is on one — used to highlight it
         * in the colourised draw. We check both the char at cursor and the one
         * immediately before. */
        int match_pos = -1;
        if (input_cursor < input_len) match_pos = zc_match_paren(input_buf, input_len, input_cursor);
        if (match_pos < 0 && input_cursor > 0) {
            int alt = zc_match_paren(input_buf, input_len, input_cursor - 1);
            if (alt >= 0) match_pos = alt;
        }

        /* Multi-line syntax-highlighted input. Track cursor pixel position by
         * walking the buffer one logical row at a time. */
        BeginScissorMode(prompt_w, output_h, avail_w + 12, input_h);
        int line_idx = 0;
        int line_start = 0;
        int cursor_row = 0;
        float cursor_col_px_f = 0.0f;
        for (int i = 0; i <= input_len; i++) {
            /* If the caret sits exactly ON a newline, put it at column 0
             * of the NEXT line, which is what users expect after pressing
             * the Right arrow off the end of a line. */
            int caret_here = (i == input_cursor);
            if (caret_here && i < input_len && input_buf[i] == '\n') {
                cursor_row      = line_idx + 1;
                cursor_col_px_f = 0.0f;
                caret_here      = 0;   /* don't fire the in-line case below */
            }
            if (caret_here) {
                cursor_row = line_idx;
                /* Float-precision measurement so the cursor lands exactly
                 * where the renderer's float-accumulating draw stops. */
                cursor_col_px_f = zc_measure_text_n_f(input_buf + line_start,
                                                      i - line_start, FS_INPUT);
            }
            if (i == input_len || input_buf[i] == '\n') {
                int rel_match = (match_pos >= line_start && match_pos < i)
                                  ? match_pos - line_start : -1;
                zc_draw_highlighted(input_buf + line_start, i - line_start,
                                    prompt_w, output_h + 12 + line_idx * (FS_INPUT + 4),
                                    FS_INPUT, env, rel_match);
                line_idx++;
                line_start = i + 1;
            }
        }
        int cursor_col_px = (int)(cursor_col_px_f + 0.5f);
        /* Cursor (blinking). */
        if (((int)(GetTime() * 2)) % 2 == 0) {
            int cx = prompt_w + cursor_col_px;
            int cy = output_h + 10 + cursor_row * (FS_INPUT + 4);
            DrawLine(cx, cy, cx, cy + FS_INPUT + 6, WHITE);
        }
        EndScissorMode();

        /* Status bar — bottom strip. */
        int bar_y = H - status_h;
        DrawRectangle(0, bar_y, W, status_h, TC.status_bg);
        DrawLine(0, bar_y, W, bar_y, BORDER);
        char status[160];
        snprintf(status, sizeof(status),
                 " %d cells · last eval %.1f ms · %s · F1 help · Ctrl+S save · Ctrl+T theme · Ctrl+= / Ctrl+- font · Ctrl+L clear",
                 g_cell_count, last_eval_ms,
                 g_console_font_loaded
                     ? (zc_jbm_font_len > 0 ? "JetBrains Mono (embedded)" : "TTF")
                     : "default font");
        zc_draw_text(status, 6, bar_y + 2, FS_TEXT - 2, TC.status_fg);

        /* Scroll-to-bottom floating button, visible only when the user has
         * scrolled up. Click jumps back to the latest cell. */
        float max_scroll = (float)(content_h > output_h ? content_h - output_h : 0);
        if (scroll_y < max_scroll - 30) {
            int btn_size = 28;
            int btn_x = W - btn_size - 14;
            int btn_y = output_h - btn_size - 12;
            Vector2 mp = { (float)btn_x, (float)btn_y };  /* probe corner */
            (void)mp;
            int hover = 0;
            float mx = (float)GetMouseX(), my = (float)GetMouseY();
            if (mx >= btn_x && mx <= btn_x + btn_size
             && my >= btn_y && my <= btn_y + btn_size) hover = 1;
            DrawRectangle(btn_x, btn_y, btn_size, btn_size,
                          hover ? (Color){90,110,160,255} : (Color){60,70,100,230});
            DrawRectangleLines(btn_x, btn_y, btn_size, btn_size, BORDER);
            /* Down-arrow glyph: three lines forming a V. */
            DrawLineEx((Vector2){btn_x + 8.f,  btn_y + 10.f},
                       (Vector2){btn_x + 14.f, btn_y + 18.f}, 2.0f, WHITE);
            DrawLineEx((Vector2){btn_x + 14.f, btn_y + 18.f},
                       (Vector2){btn_x + 20.f, btn_y + 10.f}, 2.0f, WHITE);
            if (hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                scroll_y = max_scroll;
            }
        }

        /* Autocomplete popup, floating above the input bar. */
        if (autocomplete.count > 0) {
            const int ROW_H     = FS_INPUT + 4;
            const int MAX_VISIBLE = 10;
            int visible = autocomplete.count < MAX_VISIBLE
                            ? autocomplete.count : MAX_VISIBLE;

            /* Scroll the visible window to keep the selection on-screen. */
            int sel = autocomplete.selected < 0 ? 0 : autocomplete.selected;
            int scroll = 0;
            if (sel >= visible) scroll = sel - visible + 1;

            /* Two-column layout: name on the left, dim example on the right.
             * Width = widest (name + gap + example) in the visible window,
             * clamped to fit the window. */
            const int NAME_GAP = 24;
            int name_w = 0, ex_w = 0;
            for (int i = 0; i < autocomplete.count; i++) {
                int nw = zc_measure_text(autocomplete.items[i], FS_INPUT);
                if (nw > name_w) name_w = nw;
                char shape[256];
                zc_call_shape(autocomplete.items[i], env, shape, sizeof(shape));
                if (shape[0]) {
                    int ew = zc_measure_text(shape, FS_INPUT - 2);
                    if (ew > ex_w) ex_w = ew;
                }
            }
            int pop_w = name_w + NAME_GAP + ex_w + 32;
            if (pop_w < 280) pop_w = 280;
            if (pop_w > W - 16) pop_w = W - 16;
            int pop_h = visible * ROW_H + 8;
            /* Anchor the popup just above the input field, aligned with the
             * caret's column when possible. */
            int caret_col = prompt_w + cursor_col_px;
            int pop_x = caret_col - 12;
            if (pop_x + pop_w > W - 8) pop_x = W - 8 - pop_w;
            if (pop_x < 8) pop_x = 8;
            int pop_y = output_h - pop_h - 4;
            if (pop_y < 8) pop_y = 8;

            DrawRectangle(pop_x, pop_y, pop_w, pop_h, AC_BG);
            DrawRectangleLines(pop_x, pop_y, pop_w, pop_h, AC_BORDER);

            for (int i = 0; i < visible; i++) {
                int idx = i + scroll;
                int row_y = pop_y + 4 + i * ROW_H;
                int is_sel = (idx == autocomplete.selected);
                if (is_sel) {
                    DrawRectangle(pop_x + 2, row_y - 2,
                                  pop_w - 4, ROW_H, AC_SEL_BG);
                }
                /* Render the matched prefix in a different colour so the user
                 * sees what they've typed at a glance. */
                const char* name = autocomplete.items[idx];
                char prefix[256];
                int plen = autocomplete.token_len < 255 ? autocomplete.token_len : 255;
                memcpy(prefix, name, plen);
                prefix[plen] = 0;
                zc_draw_text(prefix, pop_x + 10, row_y, FS_INPUT, AC_MATCH_FG);
                int pref_px = zc_measure_text(prefix, FS_INPUT);
                zc_draw_text(name + plen, pop_x + 10 + pref_px, row_y,
                             FS_INPUT, is_sel ? AC_FG : AC_DIM_FG);
                /* Inline call shape: parameter names from the signature
                 * table or user-fn introspection, falling back to the
                 * cheatsheet example. */
                char shape[256];
                zc_call_shape(name, env, shape, sizeof(shape));
                if (shape[0]) {
                    int ex_x = pop_x + 10 + name_w + NAME_GAP;
                    int max_ex_px = pop_x + pop_w - ex_x - 10;
                    while (shape[0] && zc_measure_text(shape, FS_INPUT - 2) > max_ex_px) {
                        size_t L = strlen(shape);
                        if (L < 4) break;
                        shape[L-4] = '.'; shape[L-3] = '.'; shape[L-2] = '.'; shape[L-1] = 0;
                    }
                    zc_draw_text(shape, ex_x, row_y + 2,
                                 FS_INPUT - 2, AC_DIM_FG);
                }
            }
            /* "+N more" indicator if the list is truncated. */
            if (autocomplete.count > MAX_VISIBLE) {
                char tail[32];
                snprintf(tail, sizeof(tail), "(+%d more)",
                         autocomplete.count - MAX_VISIBLE);
                zc_draw_text(tail, pop_x + pop_w - 90, pop_y + pop_h - 18,
                             FS_INPUT - 4, AC_DIM_FG);
            }
        }

        /* F1 help overlay. Translucent full-screen panel with a dump of
         * keybindings + a categorised list of builtins (sourced from the
         * same signature table the popup uses). Esc or F1 closes it. */
        if (show_help_overlay) {
            DrawRectangle(0, 0, W, H, (Color){0,0,0,220});
            int panel_x = 24;
            int panel_y = 24;
            int panel_w = W - 48;
            int panel_h = H - 48;
            DrawRectangle(panel_x, panel_y, panel_w, panel_h, AC_BG);
            DrawRectangleLines(panel_x, panel_y, panel_w, panel_h, AC_BORDER);
            int hy = panel_y + 14;
            zc_draw_text("z-console — press F1 or Esc to close",
                         panel_x + 16, hy, FS_INPUT + 2, AC_MATCH_FG);
            hy += FS_INPUT + 16;

            /* Two-column layout: keybindings on the left, builtins on the right. */
            int col_w = panel_w / 2 - 24;
            int left_x  = panel_x + 16;
            int right_x = panel_x + 16 + col_w + 24;
            int y_left  = hy;
            int y_right = hy;
            const char* keys[][2] = {
                {"Enter",            "run / newline if parens unbalanced"},
                {"Shift+Enter",      "force newline"},
                {"Tab",              "accept highlighted autocomplete"},
                {"Esc",              "dismiss popup / overlay"},
                {"↑ / ↓",            "history / autocomplete navigation"},
                {"Ctrl+←/→",         "word jump"},
                {"Ctrl+W / U / K",   "kill word back / line back / line end"},
                {"Ctrl+C / V / X",   "clipboard copy / paste / cut"},
                {"Ctrl+S",           "save session"},
                {"Ctrl+T",           "cycle theme"},
                {"Ctrl+L",           "clear all cells"},
                {"Ctrl+.",           "interrupt running eval"},
                {"Ctrl+= / Ctrl+-",  "grow / shrink font"},
                {"F1",               "this help"},
                {"Click In[N]:",     "reload that cell into prompt"},
                {"Drop .z file",     "load into prompt"},
                {"Drop image",       "insert (ui:image \"path\")"},
                { NULL, NULL }
            };
            zc_draw_text("Keys", left_x, y_left, FS_INPUT, AC_MATCH_FG);
            y_left += FS_INPUT + 8;
            for (int i = 0; keys[i][0]; i++) {
                zc_draw_text(keys[i][0], left_x, y_left, FS_TEXT, AC_FG);
                zc_draw_text(keys[i][1], left_x + 150, y_left, FS_TEXT, AC_DIM_FG);
                y_left += FS_TEXT + 4;
            }

            zc_draw_text("Builtins", right_x, y_right, FS_INPUT, AC_MATCH_FG);
            y_right += FS_INPUT + 8;
            const int ROWS_PER_COL = (panel_h - 80) / (FS_TEXT + 4);
            int rcount = 0;
            for (size_t i = 0; i < Z_CONSOLE_SIGS_LEN && rcount < ROWS_PER_COL; i++) {
                char shape[200];
                snprintf(shape, sizeof(shape),
                         "%s  %s",
                         Z_CONSOLE_SIGS[i].name,
                         Z_CONSOLE_SIGS[i].sig);
                /* Trim overflow with ellipsis. */
                int maxpx = col_w - 8;
                while (shape[0] && zc_measure_text(shape, FS_TEXT) > maxpx) {
                    size_t L = strlen(shape);
                    if (L < 4) break;
                    shape[L-4] = '.'; shape[L-3] = '.'; shape[L-2] = '.'; shape[L-1] = 0;
                }
                zc_draw_text(shape, right_x, y_right, FS_TEXT, AC_FG);
                y_right += FS_TEXT + 4;
                rcount++;
            }
            if (Z_CONSOLE_SIGS_LEN > (size_t)ROWS_PER_COL) {
                char more[64];
                snprintf(more, sizeof(more), "+ %d more — see (help)",
                         (int)(Z_CONSOLE_SIGS_LEN - ROWS_PER_COL));
                zc_draw_text(more, right_x, y_right + 4, FS_TEXT, AC_DIM_FG);
            }
        }

        EndDrawing();
    }

    hist_save();
    /* Persist state so the next launch comes up with the same look + size. */
    state.theme_idx = g_theme_idx;
    state.font_size = FS_INPUT;
    state.window_w  = GetScreenWidth();
    state.window_h  = GetScreenHeight();
    zc_state_save(&state);
    if (g_console_font_loaded) UnloadFont(g_console_font);
    CloseWindow();
    return 0;
}
