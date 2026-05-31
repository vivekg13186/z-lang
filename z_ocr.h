/*
 * z_ocr.h — optional OCR module backed by libtesseract.
 *
 * Compiled into z only when -DZ_WITH_OCR is set:
 *
 *   make OCR=1
 *
 * Embeds libtesseract directly — no Python / pytesseract required at
 * runtime. The tesseract binary + its `tessdata` language files must
 * still be installed (the runtime needs them to recognize glyphs).
 *
 * Install:
 *   macOS:    brew install tesseract            (headers + libs + tessdata-eng)
 *   Debian:   sudo apt-get install libtesseract-dev libleptonica-dev tesseract-ocr
 *   Fedora:   sudo dnf install tesseract-devel leptonica-devel tesseract
 *   Windows:  pacman -S mingw-w64-x86_64-tesseract-ocr  (MSYS2)
 *
 * Builtins:
 *
 *   (ocr:image path [lang])
 *     → string with the recognized text
 *
 *   (ocr:words path [lang])
 *     → array of { word, confidence, x, y, width, height }
 *       (confidence is 0..100, bounding box in image pixels)
 *
 *   (ocr:lang)  → currently-active language name
 *
 * `lang` defaults to "eng". For multiple languages chain them with `+`:
 *   (ocr:image "doc.png" "eng+deu")
 *
 * tessdata location:
 *   The C API uses $TESSDATA_PREFIX if set, else its compile-time default.
 *   On macOS Homebrew installs that's /opt/homebrew/share/tessdata. If
 *   tesseract complains about missing language files at runtime, point
 *   at the right directory:
 *       TESSDATA_PREFIX=/opt/homebrew/share/tessdata ./z myscript.z
 */

#ifndef Z_OCR_H_INCLUDED
#define Z_OCR_H_INCLUDED

#ifdef Z_WITH_OCR

#include <tesseract/capi.h>
/* Leptonica's header is sometimes at <leptonica/allheaders.h> (Linux
 * distros, modern Homebrew) and sometimes just <allheaders.h> (older
 * installs, some Windows builds). Pick whichever exists. */
#if defined(__has_include)
#  if __has_include(<leptonica/allheaders.h>)
#    include <leptonica/allheaders.h>
#  elif __has_include(<allheaders.h>)
#    include <allheaders.h>
#  else
#    error "leptonica headers not found — install libleptonica-dev (apt) / leptonica-devel (dnf) / leptonica (brew)"
#  endif
#else
#  include <leptonica/allheaders.h>
#endif

/* Shared init helper: creates and inits a TessBaseAPI in the given lang.
 * Returns NULL and raises on failure. */
static TessBaseAPI* zo_open(const char* lang, const char* fn) {
    TessBaseAPI* api = TessBaseAPICreate();
    if (!api) z_raise("%s: TessBaseAPICreate failed", fn);
    if (TessBaseAPIInit3(api, NULL, lang ? lang : "eng") != 0) {
        TessBaseAPIDelete(api);
        z_raise("%s: cannot init tesseract for lang '%s' "
                "(check that tessdata files are installed; set "
                "$TESSDATA_PREFIX if they're in a non-default dir)",
                fn, lang ? lang : "eng");
    }
    return api;
}

/* (ocr:image path [lang]) → recognized text */
static Value* b_ocr_image(int argc, Value** argv, Env* e) {
    (void)e;
    if (argc < 1 || argc > 2) z_raise("ocr:image: (ocr:image path [lang])");
    const char* path = str_arg(argv[0], "ocr:image");
    const char* lang = (argc == 2) ? str_arg(argv[1], "ocr:image") : "eng";

    /* Leptonica typedefs `PIX` (uppercase); using the struct tag works
     * regardless of which spelling the local header exposes. */
    struct Pix* pix = pixRead(path);
    if (!pix) z_raise("ocr:image: cannot read '%s' "
                      "(needs PNG/JPG/TIFF/BMP; verify the path)", path);
    TessBaseAPI* api = zo_open(lang, "ocr:image");
    TessBaseAPISetImage2(api, pix);
    if (TessBaseAPIRecognize(api, NULL) != 0) {
        TessBaseAPIDelete(api);
        pixDestroy(&pix);
        z_raise("ocr:image: recognize step failed");
    }
    char* text = TessBaseAPIGetUTF8Text(api);
    Value* r = v_str(text ? text : "");
    if (text) TessDeleteText(text);
    TessBaseAPIDelete(api);
    pixDestroy(&pix);
    return r;
}

/* (ocr:words path [lang]) → array of { word, confidence, x, y, width, height }
 * Confidence is 0..100. Empty words and tesseract sentinel words are skipped. */
static Value* b_ocr_words(int argc, Value** argv, Env* e) {
    (void)e;
    if (argc < 1 || argc > 2) z_raise("ocr:words: (ocr:words path [lang])");
    const char* path = str_arg(argv[0], "ocr:words");
    const char* lang = (argc == 2) ? str_arg(argv[1], "ocr:words") : "eng";

    /* Leptonica typedefs `PIX` (uppercase); using the struct tag works
     * regardless of which spelling the local header exposes. */
    struct Pix* pix = pixRead(path);
    if (!pix) z_raise("ocr:words: cannot read '%s'", path);
    TessBaseAPI* api = zo_open(lang, "ocr:words");
    TessBaseAPISetImage2(api, pix);
    if (TessBaseAPIRecognize(api, NULL) != 0) {
        TessBaseAPIDelete(api);
        pixDestroy(&pix);
        z_raise("ocr:words: recognize step failed");
    }

    Value* out = v_array();
    TessResultIterator* it = TessBaseAPIGetIterator(api);
    if (it) {
        TessPageIteratorLevel level = RIL_WORD;
        TessPageIterator* pi = TessResultIteratorGetPageIterator(it);
        do {
            char* word = TessResultIteratorGetUTF8Text(it, level);
            float conf = TessResultIteratorConfidence(it, level);
            if (word && word[0]) {
                int x1, y1, x2, y2;
                if (TessPageIteratorBoundingBox(pi, level, &x1, &y1, &x2, &y2)) {
                    Value* o = v_object();
                    obj_set(&o->as.obj, "word",       v_str(word));
                    obj_set(&o->as.obj, "confidence", v_num(conf));
                    obj_set(&o->as.obj, "x",          v_num(x1));
                    obj_set(&o->as.obj, "y",          v_num(y1));
                    obj_set(&o->as.obj, "width",      v_num(x2 - x1));
                    obj_set(&o->as.obj, "height",     v_num(y2 - y1));
                    vlist_push(&out->as.list, o);
                }
            }
            if (word) TessDeleteText(word);
        } while (TessPageIteratorNext(pi, level));
        TessResultIteratorDelete(it);
    }
    TessBaseAPIDelete(api);
    pixDestroy(&pix);
    return out;
}

static Value* b_ocr_lang(int argc, Value** argv, Env* e) {
    (void)e; (void)argc; (void)argv;
    return v_str("eng");   /* placeholder; real env query via TessBaseAPIGetInitLanguagesAsString */
}

static void install_ocr_builtins(Env* env) {
    env_define(env, "ocr:image", v_native(b_ocr_image));
    env_define(env, "ocr:words", v_native(b_ocr_words));
    env_define(env, "ocr:lang",  v_native(b_ocr_lang));
}

#endif /* Z_WITH_OCR */
#endif /* Z_OCR_H_INCLUDED */
