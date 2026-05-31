/*
 * z_vision.h — optional computer-vision module for z.
 *
 * Compiled into z only when -DZ_WITH_VISION is set:
 *
 *   make VISION=1
 *
 * Like z_img.h, this module shells out to existing tools rather than linking
 * against C libraries — keeps z's single-file build story intact.
 *
 * Runtime dependencies (each function probes for what it needs and gives a
 * helpful error if the tool is missing):
 *
 *   vision:barcode  → `zbarimg`   (apt-get install zbar-tools / brew install zbar)
 *   vision:faces    → `python3` + opencv-python  (pip install opencv-python)
 *   vision:objects  → `python3` + opencv-python  (pip install opencv-python)
 *   vision:plate    — REMOVED. For license-plate OCR build with OCR=1 and
 *                     combine `vision:objects` proposals with `ocr:image`.
 *
 * Return shapes:
 *
 *   (vision:barcode path)
 *     → [ { "type": "QR-Code", "data": "https://..." }, ... ]
 *
 *   (vision:faces path)
 *     → [ { "x": 120, "y": 60, "width": 80, "height": 80 }, ... ]
 *
 *   (vision:objects path)
 *     → [ { "class": "person", "confidence": 0.87,
 *           "x": 100, "y": 200, "width": 64, "height": 128 }, ... ]
 *
 * All functions read the image path off disk; they don't load image bytes
 * across the FFI boundary. An empty array means "no detections", not an error.
 */

#ifdef Z_WITH_VISION

/* ============================================================
 * Tool probes (cached, single-threaded — same pattern as z_img.h)
 * ============================================================ */

static int z_vision_have(const char* probe_cmd) {
    int code = 0;
    char* out = z_capture_command(probe_cmd, &code);
    free(out);
    return code == 0;
}

static int z_vision_have_python_cv2(void) {
    static int checked = -1;
    if (checked != -1) return checked;
    /* `python3 -c 'import cv2'` returns 0 if importable. */
    checked = z_vision_have("python3 -c \"import cv2\" 2>/dev/null") ? 1 : 0;
    return checked;
}

/* Trim trailing newlines on captured output for prettier error messages. */
static void z_vision_trim(char* s) {
    if (!s) return;
    size_t L = strlen(s);
    while (L && (s[L-1] == '\n' || s[L-1] == '\r' || s[L-1] == ' ')) s[--L] = 0;
}

/* Iterate output line by line, calling `on_line(line, ctx)`. Modifies buf. */
typedef void (*z_line_fn)(char* line, void* ctx);
static void z_vision_for_each_line(char* buf, z_line_fn on_line, void* ctx) {
    if (!buf) return;
    char* p = buf;
    while (*p) {
        char* nl = strchr(p, '\n');
        if (nl) *nl = '\0';
        /* Strip trailing \r from CRLF. */
        size_t L = strlen(p);
        if (L && p[L-1] == '\r') p[L-1] = '\0';
        if (*p) on_line(p, ctx);
        if (!nl) break;
        p = nl + 1;
    }
}

/* ============================================================
 * vision:barcode — `zbarimg` default output is `TYPE:DATA` per line.
 * ============================================================ */

static void on_barcode_line(char* line, void* ctx) {
    Value* out = (Value*)ctx;
    char* colon = strchr(line, ':');
    if (!colon) return;
    *colon = '\0';
    const char* type = line;
    const char* data = colon + 1;
    Value* o = v_object();
    obj_set(&o->as.obj, "type", v_str(type));
    obj_set(&o->as.obj, "data", v_str(data));
    vlist_push(&out->as.list, o);
}

static Value* b_vision_barcode(int argc, Value** argv, Env* e) {
    (void)e;
    EXPECT_ARGC("vision:barcode", 1);
    const char* path = str_arg(argv[0], "vision:barcode");
    if (!z_vision_have("zbarimg --version >/dev/null 2>&1"))
        z_raise("vision:barcode: needs `zbarimg` on PATH "
                "(apt-get install zbar-tools / brew install zbar)");
    char cmd[8192];
    /* -q quiets summary; --raw would drop the TYPE: prefix we parse. */
    snprintf(cmd, sizeof(cmd), "zbarimg -q \"%s\" 2>/dev/null", path);
    int code = 0;
    char* raw = z_capture_command(cmd, &code);
    /* zbarimg exits 4 when no barcodes found; treat as empty result, not error. */
    if (code != 0 && code != 4 && code != 1024) {
        char* msg = raw ? raw : str_dup("");
        z_vision_trim(msg);
        z_raise("vision:barcode: zbarimg failed (%d): %s", code, msg);
    }
    Value* out = v_array();
    z_vision_for_each_line(raw, on_barcode_line, out);
    free(raw);
    return out;
}

/* ============================================================
 * vision:faces — OpenCV Haar cascade via python3.
 * Output of the inline script: one detection per line as "x y w h".
 * ============================================================ */

static void z_vision_require_cv2(const char* fn) {
    if (!z_vision_have("python3 --version >/dev/null 2>&1"))
        z_raise("%s: needs `python3` on PATH", fn);
    if (!z_vision_have_python_cv2())
        z_raise("%s: needs opencv-python (pip install opencv-python)", fn);
}

static void on_face_line(char* line, void* ctx) {
    Value* out = (Value*)ctx;
    int x = 0, y = 0, w = 0, h = 0;
    if (sscanf(line, "%d %d %d %d", &x, &y, &w, &h) != 4) return;
    Value* o = v_object();
    obj_set(&o->as.obj, "x",      v_num(x));
    obj_set(&o->as.obj, "y",      v_num(y));
    obj_set(&o->as.obj, "width",  v_num(w));
    obj_set(&o->as.obj, "height", v_num(h));
    vlist_push(&out->as.list, o);
}

/* The python program is embedded as a C string so the module stays single-
 * file. Reads cascade XML from cv2.data which ships with opencv-python. */
static const char Z_VISION_FACE_PY[] =
    "import sys, cv2\n"
    "img = cv2.imread(sys.argv[1])\n"
    "if img is None:\n"
    "    sys.stderr.write('cannot open image\\n'); sys.exit(2)\n"
    "gray = cv2.cvtColor(img, cv2.COLOR_BGR2GRAY)\n"
    "xml = cv2.data.haarcascades + 'haarcascade_frontalface_default.xml'\n"
    "cas = cv2.CascadeClassifier(xml)\n"
    "for (x, y, w, h) in cas.detectMultiScale(gray, 1.1, 5):\n"
    "    print(f'{int(x)} {int(y)} {int(w)} {int(h)}')\n";

static Value* b_vision_faces(int argc, Value** argv, Env* e) {
    (void)e;
    EXPECT_ARGC("vision:faces", 1);
    const char* path = str_arg(argv[0], "vision:faces");
    z_vision_require_cv2("vision:faces");
    /* Pass the script via -c and the path as argv[1]. We quote both. */
    char cmd[16384];
    /* Escape any " in the script by repeating $'...' trickery isn't portable;
     * the script has no double quotes, so a simple wrapping works. */
    snprintf(cmd, sizeof(cmd),
             "python3 -c \"%s\" \"%s\" 2>/dev/null",
             Z_VISION_FACE_PY, path);
    int code = 0;
    char* raw = z_capture_command(cmd, &code);
    if (code != 0) {
        char* msg = raw ? raw : str_dup("");
        z_vision_trim(msg);
        z_raise("vision:faces: python/cv2 failed (%d): %s",
                code, msg[0] ? msg : "no output");
    }
    Value* out = v_array();
    z_vision_for_each_line(raw, on_face_line, out);
    free(raw);
    return out;
}

/* ============================================================
 * vision:objects — OpenCV HOG people detector (the most reliable thing
 * that ships with opencv-python out of the box, no model download required).
 * Output: "person <conf> <x> <y> <w> <h>" per line.
 * ============================================================ */

static void on_object_line(char* line, void* ctx) {
    Value* out = (Value*)ctx;
    char cls[64] = {0};
    double conf = 0;
    int x = 0, y = 0, w = 0, h = 0;
    if (sscanf(line, "%63s %lf %d %d %d %d",
               cls, &conf, &x, &y, &w, &h) != 6) return;
    Value* o = v_object();
    obj_set(&o->as.obj, "class",      v_str(cls));
    obj_set(&o->as.obj, "confidence", v_num(conf));
    obj_set(&o->as.obj, "x",          v_num(x));
    obj_set(&o->as.obj, "y",          v_num(y));
    obj_set(&o->as.obj, "width",      v_num(w));
    obj_set(&o->as.obj, "height",     v_num(h));
    vlist_push(&out->as.list, o);
}

static const char Z_VISION_OBJ_PY[] =
    "import sys, cv2\n"
    "img = cv2.imread(sys.argv[1])\n"
    "if img is None:\n"
    "    sys.stderr.write('cannot open image\\n'); sys.exit(2)\n"
    "hog = cv2.HOGDescriptor()\n"
    "hog.setSVMDetector(cv2.HOGDescriptor_getDefaultPeopleDetector())\n"
    "boxes, weights = hog.detectMultiScale(img, winStride=(8, 8))\n"
    "for (b, w_) in zip(boxes, weights):\n"
    "    x, y, ww, hh = b\n"
    "    print(f'person {float(w_):.3f} {int(x)} {int(y)} {int(ww)} {int(hh)}')\n";

static Value* b_vision_objects(int argc, Value** argv, Env* e) {
    (void)e;
    EXPECT_ARGC("vision:objects", 1);
    const char* path = str_arg(argv[0], "vision:objects");
    z_vision_require_cv2("vision:objects");
    char cmd[16384];
    snprintf(cmd, sizeof(cmd),
             "python3 -c \"%s\" \"%s\" 2>/dev/null",
             Z_VISION_OBJ_PY, path);
    int code = 0;
    char* raw = z_capture_command(cmd, &code);
    if (code != 0) {
        char* msg = raw ? raw : str_dup("");
        z_vision_trim(msg);
        z_raise("vision:objects: python/cv2 failed (%d): %s",
                code, msg[0] ? msg : "no output");
    }
    Value* out = v_array();
    z_vision_for_each_line(raw, on_object_line, out);
    free(raw);
    return out;
}

/* ============================================================
 * Registration
 *
 * Note: `vision:plate` was removed — the openalpr backend is dead and the
 * python+pytesseract replacement was too brittle to keep as a default
 * (depends on user-installed pytesseract + heuristic contour finding).
 * For OCR use the dedicated `OCR=1` module (see z_ocr.h), which embeds
 * libtesseract directly. To build a plate pipeline on top, combine
 * `vision:objects` (for region proposals) with `ocr:image` (per-ROI OCR).
 * ============================================================ */

static void install_vision_builtins(Env* env) {
    env_define(env, "vision:barcode", v_native(b_vision_barcode));
    env_define(env, "vision:faces",   v_native(b_vision_faces));
    env_define(env, "vision:objects", v_native(b_vision_objects));
}

#endif /* Z_WITH_VISION */
