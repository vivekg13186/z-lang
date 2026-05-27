/*
 * z_img.h — optional image manipulation module for z.
 *
 * Compiled into z only when -DZ_WITH_IMAGE is set:
 *
 *   make IMAGE=1
 *   cmake -DZ_WITH_IMAGE=ON ...
 *
 * Runtime requirement: ImageMagick (`magick` or `convert` on PATH).
 *   macOS:    brew install imagemagick
 *   Linux:    apt-get install imagemagick   /   dnf install ImageMagick
 *   Windows:  scoop install imagemagick     /   the official MSI installer
 *
 * Cross-platform: the module shells out to ImageMagick, so it works
 * anywhere ImageMagick does. Paths and text are wrapped in double quotes;
 * avoid embedding " in inputs.
 *
 * Builtins:
 *   (img:create  dst w h [color])               — make a blank canvas
 *   (img:resize  src dst w h)                   — write a resized copy
 *   (img:crop    src dst x y w h)               — write a rectangular crop
 *   (img:rotate  src dst degrees)               — rotate clockwise (any angle)
 *   (img:circle  src dst cx cy r fill [stroke] [stroke-width])
 *                                               — draw a circle
 *   (img:rect    src dst x y w h fill [stroke] [stroke-width])
 *                                               — draw a rectangle
 *   (img:add-text src dst text [x y size color])
 *                                               — burn text onto an image
 *   (img:bw         src dst [threshold])        — 1-bit black & white
 *   (img:grayscale  src dst)                    — 8-bit grayscale
 *   (img:to-pdf     images dst)                 — combine images into one PDF
 *   (img:qr         text dst [scale])           — QR code  (needs qrencode/zint)
 *   (img:barcode    data dst [type])            — 1D/2D barcode (needs zint)
 *   (img:info    path)                          — { width, height, format }
 *
 * img:qr and img:barcode use extra tools rather than ImageMagick:
 *   qrencode — macOS: brew install qrencode · Linux: apt-get install qrencode
 *   zint     — macOS: brew install zint     · Linux: apt-get install zint
 *
 * Colors are anything ImageMagick accepts: "red", "#ff8800", "rgb(0,128,255)",
 * "none" for transparent.
 *
 * Note on PDF output: some Linux distros ship ImageMagick with PDF write
 * disabled in /etc/ImageMagick-{6,7}/policy.xml. If img:to-pdf fails with a
 * "not authorized" error, edit that file and change the policy line
 *     <policy domain="coder" rights="none" pattern="PDF" />
 * to
 *     <policy domain="coder" rights="read|write" pattern="PDF" />
 */

#ifdef Z_WITH_IMAGE

/* Cached lookup for the ImageMagick command — `magick` (v7+) or `convert`
 * (older installs, also a compat alias on v7). Decided once on first use. */
static const char* z_img_tool(void) {
    static char tool[16];
    static int  checked = 0;
    if (checked) return tool[0] ? tool : NULL;
    checked = 1;

    int code = 0;
    char* out;

    /* Prefer the modern `magick` command. */
    out = z_capture_command("magick -version", &code);
    free(out);
    if (code == 0) { strcpy(tool, "magick"); return tool; }

    /* Fall back to the legacy `convert`. */
    out = z_capture_command("convert -version", &code);
    free(out);
    if (code == 0) { strcpy(tool, "convert"); return tool; }

    /* Try Windows `magick.exe` explicitly (rare PATH quirk). */
    out = z_capture_command("magick.exe -version", &code);
    free(out);
    if (code == 0) { strcpy(tool, "magick.exe"); return tool; }

    tool[0] = 0;
    return NULL;
}

static void z_img_require_tool(const char* fn) {
    if (!z_img_tool()) {
        z_raise("%s: ImageMagick not found. Install it and ensure `magick` "
                "or `convert` is on your PATH.", fn);
    }
}

/* Compose: `<tool> convert-args...` for v7, or just `convert convert-args...`
 * for older installs. v7's `magick` accepts the same convert args directly. */
static char* z_img_run(const char* fn, const char* tail) {
    z_img_require_tool(fn);
    const char* tool = z_img_tool();
    size_t n = strlen(tool) + 1 + strlen(tail) + 1;
    char* cmd = (char*)malloc(n);
    snprintf(cmd, n, "%s %s", tool, tail);
    int code = 0;
    char* out = z_capture_command(cmd, &code);
    free(cmd);
    if (code != 0) {
        char* msg = out ? out : str_dup("");
        /* Trim trailing newline for prettier messages */
        size_t L = strlen(msg);
        while (L && (msg[L-1] == '\n' || msg[L-1] == '\r')) msg[--L] = 0;
        z_raise("%s: ImageMagick failed (code %d): %s", fn, code, msg);
    }
    return out;
}

/* ---------- builtins ---------- */

/* (img:create dst width height [color])
 * Default color "white". Use "none" for transparency. */
static Value* b_img_create(int argc, Value** argv, Env* e) {
    (void)e;
    if (argc < 3 || argc > 4)
        z_raise("img:create: expected (img:create dst w h [color])");
    const char* dst = str_arg(argv[0], "img:create");
    long w = (long)num_arg(argv[1], "img:create");
    long h = (long)num_arg(argv[2], "img:create");
    const char* color = argc >= 4 ? str_arg(argv[3], "img:create") : "white";
    char tail[2048];
    snprintf(tail, sizeof(tail),
             "-size \"%ldx%ld\" \"xc:%s\" \"%s\"", w, h, color, dst);
    free(z_img_run("img:create", tail));
    return v_str(dst);
}

/* (img:circle src dst cx cy radius fill [stroke] [stroke-width])
 * Defaults: stroke="none", stroke-width=1.
 * Pass fill="none" for an outline-only shape (and supply a stroke). */
static Value* b_img_circle(int argc, Value** argv, Env* e) {
    (void)e;
    if (argc < 6 || argc > 8)
        z_raise("img:circle: expected (img:circle src dst cx cy r fill [stroke] [width])");
    const char* src = str_arg(argv[0], "img:circle");
    const char* dst = str_arg(argv[1], "img:circle");
    long cx = (long)num_arg(argv[2], "img:circle");
    long cy = (long)num_arg(argv[3], "img:circle");
    long r  = (long)num_arg(argv[4], "img:circle");
    const char* fill   = str_arg(argv[5], "img:circle");
    const char* stroke = argc >= 7 ? str_arg(argv[6], "img:circle") : "none";
    long sw            = argc >= 8 ? (long)num_arg(argv[7], "img:circle") : 1;
    /* ImageMagick wants a center + any point on the perimeter — use (cx+r,cy). */
    char tail[2048];
    snprintf(tail, sizeof(tail),
             "\"%s\" -fill \"%s\" -stroke \"%s\" -strokewidth %ld "
             "-draw \"circle %ld,%ld %ld,%ld\" \"%s\"",
             src, fill, stroke, sw, cx, cy, cx + r, cy, dst);
    free(z_img_run("img:circle", tail));
    return v_str(dst);
}

/* (img:rect src dst x y w h fill [stroke] [stroke-width]) */
static Value* b_img_rect(int argc, Value** argv, Env* e) {
    (void)e;
    if (argc < 7 || argc > 9)
        z_raise("img:rect: expected (img:rect src dst x y w h fill [stroke] [width])");
    const char* src = str_arg(argv[0], "img:rect");
    const char* dst = str_arg(argv[1], "img:rect");
    long x = (long)num_arg(argv[2], "img:rect");
    long y = (long)num_arg(argv[3], "img:rect");
    long w = (long)num_arg(argv[4], "img:rect");
    long h = (long)num_arg(argv[5], "img:rect");
    const char* fill   = str_arg(argv[6], "img:rect");
    const char* stroke = argc >= 8 ? str_arg(argv[7], "img:rect") : "none";
    long sw            = argc >= 9 ? (long)num_arg(argv[8], "img:rect") : 1;
    char tail[2048];
    snprintf(tail, sizeof(tail),
             "\"%s\" -fill \"%s\" -stroke \"%s\" -strokewidth %ld "
             "-draw \"rectangle %ld,%ld %ld,%ld\" \"%s\"",
             src, fill, stroke, sw, x, y, x + w - 1, y + h - 1, dst);
    free(z_img_run("img:rect", tail));
    return v_str(dst);
}

static Value* b_img_resize(int argc, Value** argv, Env* e) {
    (void)e;
    EXPECT_ARGC("img:resize", 4);
    const char* src = str_arg(argv[0], "img:resize");
    const char* dst = str_arg(argv[1], "img:resize");
    long w = (long)num_arg(argv[2], "img:resize");
    long h = (long)num_arg(argv[3], "img:resize");
    char tail[2048];
    snprintf(tail, sizeof(tail),
             "\"%s\" -resize \"%ldx%ld!\" \"%s\"", src, w, h, dst);
    free(z_img_run("img:resize", tail));
    return v_str(dst);
}

static Value* b_img_crop(int argc, Value** argv, Env* e) {
    (void)e;
    EXPECT_ARGC("img:crop", 6);
    const char* src = str_arg(argv[0], "img:crop");
    const char* dst = str_arg(argv[1], "img:crop");
    long x = (long)num_arg(argv[2], "img:crop");
    long y = (long)num_arg(argv[3], "img:crop");
    long w = (long)num_arg(argv[4], "img:crop");
    long h = (long)num_arg(argv[5], "img:crop");
    char tail[2048];
    snprintf(tail, sizeof(tail),
             "\"%s\" -crop \"%ldx%ld+%ld+%ld\" +repage \"%s\"",
             src, w, h, x, y, dst);
    free(z_img_run("img:crop", tail));
    return v_str(dst);
}

static Value* b_img_rotate(int argc, Value** argv, Env* e) {
    (void)e;
    EXPECT_ARGC("img:rotate", 3);
    const char* src = str_arg(argv[0], "img:rotate");
    const char* dst = str_arg(argv[1], "img:rotate");
    double deg = num_arg(argv[2], "img:rotate");
    char tail[2048];
    snprintf(tail, sizeof(tail),
             "\"%s\" -background none -rotate \"%g\" \"%s\"", src, deg, dst);
    free(z_img_run("img:rotate", tail));
    return v_str(dst);
}

/* (img:add-text src dst text [x] [y] [size] [color])
 * Defaults: x=10, y=30, size=24, color="white" */
static Value* b_img_add_text(int argc, Value** argv, Env* e) {
    (void)e;
    if (argc < 3 || argc > 7)
        z_raise("img:add-text: expected (img:add-text src dst text [x y size color])");
    const char* src  = str_arg(argv[0], "img:add-text");
    const char* dst  = str_arg(argv[1], "img:add-text");
    const char* text = str_arg(argv[2], "img:add-text");
    long x    = argc >= 4 ? (long)num_arg(argv[3], "img:add-text") : 10;
    long y    = argc >= 5 ? (long)num_arg(argv[4], "img:add-text") : 30;
    long size = argc >= 6 ? (long)num_arg(argv[5], "img:add-text") : 24;
    const char* color = argc >= 7 ? str_arg(argv[6], "img:add-text") : "white";

    char tail[4096];
    snprintf(tail, sizeof(tail),
             "\"%s\" -gravity NorthWest -pointsize %ld -fill \"%s\" "
             "-annotate \"+%ld+%ld\" \"%s\" \"%s\"",
             src, size, color, x, y, text, dst);
    free(z_img_run("img:add-text", tail));
    return v_str(dst);
}

/* (img:bw src dst [threshold])
 * Threshold defaults to 50 (percent). Lower → more white, higher → more black. */
static Value* b_img_bw(int argc, Value** argv, Env* e) {
    (void)e;
    if (argc < 2 || argc > 3)
        z_raise("img:bw: expected (img:bw src dst [threshold])");
    const char* src = str_arg(argv[0], "img:bw");
    const char* dst = str_arg(argv[1], "img:bw");
    double thr = argc >= 3 ? num_arg(argv[2], "img:bw") : 50.0;
    if (thr < 0) thr = 0;
    if (thr > 100) thr = 100;
    char tail[2048];
    /* -colorspace gray first so threshold operates on luminance, then collapse
     * to a 1-bit bilevel image. */
    snprintf(tail, sizeof(tail),
             "\"%s\" -colorspace gray -threshold \"%g%%\" -monochrome \"%s\"",
             src, thr, dst);
    free(z_img_run("img:bw", tail));
    return v_str(dst);
}

/* (img:grayscale src dst) — converts to an 8-bit grayscale image. */
static Value* b_img_grayscale(int argc, Value** argv, Env* e) {
    (void)e;
    EXPECT_ARGC("img:grayscale", 2);
    const char* src = str_arg(argv[0], "img:grayscale");
    const char* dst = str_arg(argv[1], "img:grayscale");
    char tail[2048];
    snprintf(tail, sizeof(tail),
             "\"%s\" -colorspace gray -type Grayscale \"%s\"", src, dst);
    free(z_img_run("img:grayscale", tail));
    return v_str(dst);
}

/* (img:to-pdf images dst)
 * `images` is an array of image paths (or a single string for convenience).
 * Each image becomes one page of the output PDF in array order. */
static Value* b_img_to_pdf(int argc, Value** argv, Env* e) {
    (void)e;
    EXPECT_ARGC("img:to-pdf", 2);
    Value* imgs = argv[0];
    const char* dst = str_arg(argv[1], "img:to-pdf");

    StrBuf sb;
    sb_init(&sb);

    if (imgs->type == V_STR) {
        sb_putc(&sb, '"');
        sb_puts(&sb, imgs->as.s);
        sb_putc(&sb, '"');
    } else if (imgs->type == V_ARRAY || imgs->type == V_LIST) {
        if (imgs->as.list.len == 0)
            z_raise("img:to-pdf: image list is empty");
        for (size_t i = 0; i < imgs->as.list.len; i++) {
            Value* p = imgs->as.list.items[i];
            if (p->type != V_STR)
                z_raise("img:to-pdf: image paths must be strings");
            if (i) sb_putc(&sb, ' ');
            sb_putc(&sb, '"');
            sb_puts(&sb, p->as.s);
            sb_putc(&sb, '"');
        }
    } else {
        z_raise("img:to-pdf: first argument must be a string or array of strings");
    }

    sb_putc(&sb, ' ');
    sb_putc(&sb, '"');
    sb_puts(&sb, dst);
    sb_putc(&sb, '"');

    free(z_img_run("img:to-pdf", sb.data));
    free(sb.data);
    return v_str(dst);
}

/* Is a CLI tool available? Probes with the given `<tool> --version` command. */
static int z_tool_available(const char* probe) {
    int code = 0;
    char* out = z_capture_command(probe, &code);
    free(out);
    return code == 0;
}

/* Trim trailing newlines and raise a formatted error from captured output. */
static void z_img_fail(const char* fn, int code, char* out) {
    char* msg = out ? out : str_dup("");
    size_t L = strlen(msg);
    while (L && (msg[L-1] == '\n' || msg[L-1] == '\r')) msg[--L] = 0;
    z_raise("%s: failed (code %d): %s", fn, code, msg);
}

/* (img:qr text dst [scale])
 * Generates a QR code PNG. Prefers `qrencode`, falls back to `zint`.
 * scale = pixel size of each module (default 4). */
static Value* b_img_qr(int argc, Value** argv, Env* e) {
    (void)e;
    if (argc < 2 || argc > 3)
        z_raise("img:qr: expected (img:qr text dst [scale])");
    const char* text = str_arg(argv[0], "img:qr");
    const char* dst  = str_arg(argv[1], "img:qr");
    long scale = argc >= 3 ? (long)num_arg(argv[2], "img:qr") : 4;
    if (scale < 1) scale = 1;

    char cmd[8192];
    int code = 0;
    char* out;

    if (z_tool_available("qrencode --version")) {
        snprintf(cmd, sizeof(cmd),
                 "qrencode -o \"%s\" -s %ld \"%s\"", dst, scale, text);
        out = z_capture_command(cmd, &code);
    } else if (z_tool_available("zint --version")) {
        snprintf(cmd, sizeof(cmd),
                 "zint -o \"%s\" -b 58 --scale=%ld -d \"%s\"", dst, scale, text);
        out = z_capture_command(cmd, &code);
    } else {
        z_raise("img:qr: needs `qrencode` or `zint` on PATH "
                "(brew install qrencode / apt-get install qrencode)");
        return v_null();
    }
    if (code != 0) z_img_fail("img:qr", code, out);
    free(out);
    return v_str(dst);
}

/* (img:barcode data dst [type])
 * Generates a barcode PNG via `zint`. Default type "code128".
 * Supported names: code128, code39, ean13, ean8, upca, upce, qr,
 *                  datamatrix, pdf417. */
static Value* b_img_barcode(int argc, Value** argv, Env* e) {
    (void)e;
    if (argc < 2 || argc > 3)
        z_raise("img:barcode: expected (img:barcode data dst [type])");
    const char* data = str_arg(argv[0], "img:barcode");
    const char* dst  = str_arg(argv[1], "img:barcode");
    const char* type = argc >= 3 ? str_arg(argv[2], "img:barcode") : "code128";

    int sym;
    if      (z_strcaseeq(type, "code128")) sym = 20;
    else if (z_strcaseeq(type, "code39"))  sym = 8;
    else if (z_strcaseeq(type, "ean13") || z_strcaseeq(type, "ean")) sym = 13;
    else if (z_strcaseeq(type, "ean8"))    sym = 14;
    else if (z_strcaseeq(type, "upca") || z_strcaseeq(type, "upc")) sym = 34;
    else if (z_strcaseeq(type, "upce"))    sym = 37;
    else if (z_strcaseeq(type, "qr"))      sym = 58;
    else if (z_strcaseeq(type, "datamatrix") || z_strcaseeq(type, "dm")) sym = 71;
    else if (z_strcaseeq(type, "pdf417"))  sym = 55;
    else {
        z_raise("img:barcode: unknown type '%s' "
                "(code128, code39, ean13, ean8, upca, upce, qr, datamatrix, pdf417)", type);
        return v_null();
    }

    if (!z_tool_available("zint --version"))
        z_raise("img:barcode: needs `zint` on PATH "
                "(brew install zint / apt-get install zint)");

    char cmd[8192];
    snprintf(cmd, sizeof(cmd), "zint -o \"%s\" -b %d -d \"%s\"", dst, sym, data);
    int code = 0;
    char* out = z_capture_command(cmd, &code);
    if (code != 0) z_img_fail("img:barcode", code, out);
    free(out);
    return v_str(dst);
}

/* (img:info path) → { "width": N, "height": N, "format": "PNG" } */
static Value* b_img_info(int argc, Value** argv, Env* e) {
    (void)e;
    EXPECT_ARGC("img:info", 1);
    const char* path = str_arg(argv[0], "img:info");
    z_img_require_tool("img:info");

    /* `identify` is the ImageMagick query tool; under v7 use `magick identify`. */
    const char* tool = z_img_tool();
    char cmd[2048];
    if (strcmp(tool, "convert") == 0) {
        snprintf(cmd, sizeof(cmd),
                 "identify -format \"%%w %%h %%m\" \"%s\"", path);
    } else {
        snprintf(cmd, sizeof(cmd),
                 "%s identify -format \"%%w %%h %%m\" \"%s\"", tool, path);
    }
    int code = 0;
    char* out = z_capture_command(cmd, &code);
    if (code != 0) {
        char* msg = out ? out : str_dup("");
        size_t L = strlen(msg);
        while (L && (msg[L-1] == '\n' || msg[L-1] == '\r')) msg[--L] = 0;
        z_raise("img:info: identify failed: %s", msg);
    }
    int w = 0, h = 0;
    char fmt[64] = "";
    sscanf(out, "%d %d %63s", &w, &h, fmt);
    free(out);

    Value* o = v_object();
    obj_set(&o->as.obj, "width",  v_num((double)w));
    obj_set(&o->as.obj, "height", v_num((double)h));
    obj_set(&o->as.obj, "format", v_str(fmt));
    return o;
}

static void install_image_builtins(Env* env) {
    env_define(env, "img:create",    v_native(b_img_create));
    env_define(env, "img:resize",    v_native(b_img_resize));
    env_define(env, "img:crop",      v_native(b_img_crop));
    env_define(env, "img:rotate",    v_native(b_img_rotate));
    env_define(env, "img:circle",    v_native(b_img_circle));
    env_define(env, "img:rect",      v_native(b_img_rect));
    env_define(env, "img:add-text",  v_native(b_img_add_text));
    env_define(env, "img:bw",        v_native(b_img_bw));
    env_define(env, "img:grayscale", v_native(b_img_grayscale));
    env_define(env, "img:to-pdf",    v_native(b_img_to_pdf));
    env_define(env, "img:qr",        v_native(b_img_qr));
    env_define(env, "img:barcode",   v_native(b_img_barcode));
    env_define(env, "img:info",      v_native(b_img_info));
}

#endif /* Z_WITH_IMAGE */
