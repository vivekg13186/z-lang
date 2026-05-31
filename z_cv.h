/*
 * z_cv.h — embedded computer-vision module for z.
 *
 * Compiled into z only when -DZ_WITH_CV is set:
 *
 *   make CV=1
 *
 * Doesn't link OpenCV or shell out to Python. Reads PGM (Netpbm grayscale)
 * images directly and runs a Haar cascade detector in-process.
 *
 * Workflow (combine with IMAGE=1):
 *
 *   1. Convert your input to grayscale PGM once:
 *        (img:grayscale "photo.jpg" "/tmp/photo.pgm")    ; needs IMAGE=1
 *
 *   2. Convert an OpenCV Haar cascade XML to z's compact .zhc format once:
 *        python3 tools/cascade_to_bin.py \
 *            haarcascade_frontalface_default.xml face.zhc
 *      (Cascade XMLs ship with OpenCV under data/haarcascades/, or can be
 *       downloaded from https://github.com/opencv/opencv/tree/4.x/data/haarcascades)
 *
 *   3. Detect at runtime:
 *        (cv:faces "/tmp/photo.pgm" "face.zhc")
 *          → [ { "x": 120, "y": 60, "width": 80, "height": 80,
 *                "score": 1.2 }, ... ]
 *
 * The module accepts options as an object:
 *   (cv:faces image cascade
 *             (object "scale-factor" 1.1
 *                     "min-size"     24
 *                     "max-size"     0
 *                     "merge"        true))
 *
 *   scale-factor (default 1.1) — how much the window grows per pyramid step
 *   min-size     (default 0)   — skip windows smaller than this (px)
 *   max-size     (default 0)   — skip windows larger than this (0 = no cap)
 *   merge        (default true)— non-max suppression of overlapping hits
 *
 * Builtins:
 *   (cv:faces image-pgm cascade-zhc [opts])  → array of detections
 *   (cv:read-pgm path)                       → { width, height, bytes }
 *   (cv:save-pgm path width height bytes)    → true
 *   (cv:image-info pgm-path)                 → { width, height }
 */

#ifndef Z_CV_H_INCLUDED
#define Z_CV_H_INCLUDED

#ifdef Z_WITH_CV

#include <stdint.h>

/* ============================================================
 * PGM (Netpbm P5 / P2) reader.
 *
 * Format:
 *   P5\n              (P2 = ASCII; we handle both)
 *   # optional comment line(s)\n
 *   W H\n
 *   max\n            (typically 255; we clamp to 255)
 *   <W*H bytes of pixel data>
 *
 * Returns malloced grayscale buffer with `*out_w`, `*out_h`. Caller frees.
 * ============================================================ */

static unsigned char* zcv_read_pgm(const char* path, int* out_w, int* out_h) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    char tag[3] = {0};
    if (fread(tag, 1, 2, f) != 2) { fclose(f); return NULL; }
    int ascii;
    if      (tag[0] == 'P' && tag[1] == '5') ascii = 0;
    else if (tag[0] == 'P' && tag[1] == '2') ascii = 1;
    else { fclose(f); return NULL; }

    /* Skip whitespace and `#` comment lines. */
    int c;
    int header[3] = { -1, -1, -1 };   /* w, h, max */
    int idx = 0;
    while (idx < 3) {
        c = fgetc(f);
        if (c == EOF) { fclose(f); return NULL; }
        if (c == '#') { while (c != '\n' && c != EOF) c = fgetc(f); continue; }
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') continue;
        ungetc(c, f);
        int v;
        if (fscanf(f, "%d", &v) != 1) { fclose(f); return NULL; }
        header[idx++] = v;
    }
    int w = header[0], h = header[1], maxv = header[2];
    if (w <= 0 || h <= 0 || maxv <= 0) { fclose(f); return NULL; }
    /* One whitespace separator between header and pixel data. */
    c = fgetc(f);
    if (c != ' ' && c != '\t' && c != '\n' && c != '\r') ungetc(c, f);

    unsigned char* buf = (unsigned char*)malloc((size_t)w * (size_t)h);
    if (!buf) { fclose(f); return NULL; }
    if (ascii) {
        for (long i = 0; i < (long)w * h; i++) {
            int v;
            if (fscanf(f, "%d", &v) != 1) { free(buf); fclose(f); return NULL; }
            if (maxv != 255) v = v * 255 / maxv;
            buf[i] = (unsigned char)(v < 0 ? 0 : v > 255 ? 255 : v);
        }
    } else {
        if (fread(buf, 1, (size_t)w * h, f) != (size_t)w * h) {
            free(buf); fclose(f); return NULL;
        }
        if (maxv != 255) {
            for (long i = 0; i < (long)w * h; i++)
                buf[i] = (unsigned char)((int)buf[i] * 255 / maxv);
        }
    }
    fclose(f);
    *out_w = w; *out_h = h;
    return buf;
}

static int zcv_write_pgm(const char* path, int w, int h, const unsigned char* buf) {
    FILE* f = fopen(path, "wb");
    if (!f) return 0;
    fprintf(f, "P5\n%d %d\n255\n", w, h);
    fwrite(buf, 1, (size_t)w * (size_t)h, f);
    fclose(f);
    return 1;
}

/* ============================================================
 * Integral image — sum[x,y] = sum of all pixels with col<=x, row<=y.
 * Allows constant-time rectangular sums for Haar features.
 *
 * Width of integral is W+1 / H+1 so we have a zero-padded left+top edge.
 * Stored as uint32 (fits 255 * 4096 * 4096 = 4.27e9 — well within range).
 * ============================================================ */

static uint32_t* zcv_integral(const unsigned char* img, int w, int h) {
    int IW = w + 1;
    uint32_t* I = (uint32_t*)calloc((size_t)IW * (size_t)(h + 1), sizeof(uint32_t));
    if (!I) return NULL;
    for (int y = 0; y < h; y++) {
        uint32_t row = 0;
        for (int x = 0; x < w; x++) {
            row += img[y * w + x];
            I[(y + 1) * IW + (x + 1)] = I[y * IW + (x + 1)] + row;
        }
    }
    return I;
}

/* Integral image of squared pixel values — used for window variance
 * normalization (standard Viola-Jones trick to be lighting-invariant). */
static uint64_t* zcv_integral_sq(const unsigned char* img, int w, int h) {
    int IW = w + 1;
    uint64_t* I = (uint64_t*)calloc((size_t)IW * (size_t)(h + 1), sizeof(uint64_t));
    if (!I) return NULL;
    for (int y = 0; y < h; y++) {
        uint64_t row = 0;
        for (int x = 0; x < w; x++) {
            uint32_t v = img[y * w + x];
            row += (uint64_t)v * v;
            I[(y + 1) * IW + (x + 1)] = I[y * IW + (x + 1)] + row;
        }
    }
    return I;
}

static inline uint32_t zcv_rect_sum(const uint32_t* I, int IW,
                                     int x, int y, int w, int h) {
    return I[(y + h) * IW + (x + w)]
         + I[y * IW + x]
         - I[(y + h) * IW + x]
         - I[y * IW + (x + w)];
}
static inline uint64_t zcv_rect_sum_sq(const uint64_t* I, int IW,
                                        int x, int y, int w, int h) {
    return I[(y + h) * IW + (x + w)]
         + I[y * IW + x]
         - I[(y + h) * IW + x]
         - I[y * IW + (x + w)];
}

/* ============================================================
 * Cascade — loaded once via cv:faces, kept in a small cache so the same
 * cascade can be reused across calls without re-parsing the .zhc file.
 * ============================================================ */

typedef struct {
    int   x, y, w, h;
    float weight;
} ZcvRect;

typedef struct {
    float    threshold, left, right;
    int      n_rects;
    ZcvRect  rects[3];      /* standard Haar features have 2 or 3 rects */
} ZcvFeature;

typedef struct {
    int          n_features;
    ZcvFeature*  features;
} ZcvTree;

typedef struct {
    float       threshold;
    int         n_trees;
    ZcvTree*    trees;
} ZcvStage;

typedef struct {
    char*       path;        /* key — cache by absolute path */
    int         window_w, window_h;
    int         n_stages;
    ZcvStage*   stages;
} ZcvCascade;

#define ZCV_CACHE_CAP 8
static ZcvCascade g_zcv_cache[ZCV_CACHE_CAP];
static int        g_zcv_cache_n = 0;

static ZcvCascade* zcv_load_cascade(const char* path) {
    /* Cache hit? */
    for (int i = 0; i < g_zcv_cache_n; i++)
        if (g_zcv_cache[i].path && strcmp(g_zcv_cache[i].path, path) == 0)
            return &g_zcv_cache[i];

    FILE* f = fopen(path, "rb");
    if (!f) z_raise("cv:faces: cannot open cascade '%s': %s",
                    path, strerror(errno));
    char magic[4];
    if (fread(magic, 1, 4, f) != 4
        || magic[0] != 'Z' || magic[1] != 'H' || magic[2] != 'C' || magic[3] != '1') {
        fclose(f);
        z_raise("cv:faces: '%s' is not a ZHC1 file "
                "(run `python3 tools/cascade_to_bin.py cascade.xml out.zhc` to create one)", path);
    }
    uint32_t hdr[3];
    if (fread(hdr, sizeof(uint32_t), 3, f) != 3) { fclose(f); z_raise("cv:faces: short header"); }

    if (g_zcv_cache_n >= ZCV_CACHE_CAP) {
        /* Evict the first slot; primitive but bounded. */
        free(g_zcv_cache[0].path);
        for (int s = 0; s < g_zcv_cache[0].n_stages; s++) {
            for (int t = 0; t < g_zcv_cache[0].stages[s].n_trees; t++)
                free(g_zcv_cache[0].stages[s].trees[t].features);
            free(g_zcv_cache[0].stages[s].trees);
        }
        free(g_zcv_cache[0].stages);
        memmove(&g_zcv_cache[0], &g_zcv_cache[1],
                (ZCV_CACHE_CAP - 1) * sizeof(ZcvCascade));
        g_zcv_cache_n--;
    }
    ZcvCascade* cc = &g_zcv_cache[g_zcv_cache_n++];
    cc->path     = str_dup(path);
    cc->window_w = (int)hdr[0];
    cc->window_h = (int)hdr[1];
    cc->n_stages = (int)hdr[2];
    cc->stages   = (ZcvStage*)calloc((size_t)cc->n_stages, sizeof(ZcvStage));

    for (int s = 0; s < cc->n_stages; s++) {
        float st_thr; uint32_t n_trees;
        if (fread(&st_thr,  sizeof(float),    1, f) != 1
         || fread(&n_trees, sizeof(uint32_t), 1, f) != 1) {
            fclose(f); z_raise("cv:faces: short stage header");
        }
        cc->stages[s].threshold = st_thr;
        cc->stages[s].n_trees   = (int)n_trees;
        cc->stages[s].trees     = (ZcvTree*)calloc(n_trees, sizeof(ZcvTree));
        for (int t = 0; t < (int)n_trees; t++) {
            uint32_t nf;
            if (fread(&nf, sizeof(uint32_t), 1, f) != 1) { fclose(f); z_raise("cv:faces: short tree header"); }
            cc->stages[s].trees[t].n_features = (int)nf;
            cc->stages[s].trees[t].features = (ZcvFeature*)calloc(nf, sizeof(ZcvFeature));
            for (int fi = 0; fi < (int)nf; fi++) {
                ZcvFeature* F = &cc->stages[s].trees[t].features[fi];
                uint32_t nr;
                if (fread(&F->threshold, sizeof(float), 1, f) != 1
                 || fread(&F->left,      sizeof(float), 1, f) != 1
                 || fread(&F->right,     sizeof(float), 1, f) != 1
                 || fread(&nr,           sizeof(uint32_t), 1, f) != 1) {
                    fclose(f); z_raise("cv:faces: short feature header");
                }
                if (nr > 3) { fclose(f); z_raise("cv:faces: feature has %u rects (max 3)", nr); }
                F->n_rects = (int)nr;
                for (int r = 0; r < (int)nr; r++) {
                    int32_t coords[4]; float weight;
                    if (fread(coords, sizeof(int32_t), 4, f) != 4
                     || fread(&weight, sizeof(float), 1, f) != 1) {
                        fclose(f); z_raise("cv:faces: short rect");
                    }
                    F->rects[r].x = coords[0];
                    F->rects[r].y = coords[1];
                    F->rects[r].w = coords[2];
                    F->rects[r].h = coords[3];
                    F->rects[r].weight = weight;
                }
            }
        }
    }
    fclose(f);
    return cc;
}

/* ============================================================
 * Multi-scale detection.
 *
 * Standard Viola-Jones approach: rather than resize the image, we keep
 * the image and scale the cascade window (which means scaling each
 * feature's rect coordinates and weights). Faster + retains pixel
 * precision.
 * ============================================================ */

typedef struct { int x, y, w, h; float score; } ZcvDetection;

static int zcv_eval_window(const ZcvCascade* cc,
                            const uint32_t* I,  int IW,
                            const uint64_t* IS, int ISW,
                            int win_x, int win_y,
                            int win_w, int win_h,
                            float* out_score) {
    /* Per-window standard deviation for feature-threshold normalization. */
    uint32_t sum  = zcv_rect_sum   (I,  IW,  win_x, win_y, win_w, win_h);
    uint64_t sum2 = zcv_rect_sum_sq(IS, ISW, win_x, win_y, win_w, win_h);
    double area  = (double)win_w * win_h;
    double mean  = sum / area;
    double var   = (double)sum2 / area - mean * mean;
    double stdev = var > 1.0 ? sqrt(var) : 1.0;

    double scale_x = (double)win_w / cc->window_w;
    double scale_y = (double)win_h / cc->window_h;
    /* The feature thresholds were trained on a normalized window — scale
     * back to current window area for compatibility. */
    double inv_area_scale = 1.0 / (scale_x * scale_y);

    float total = 0;
    for (int s = 0; s < cc->n_stages; s++) {
        const ZcvStage* st = &cc->stages[s];
        double stage_sum = 0;
        for (int t = 0; t < st->n_trees; t++) {
            const ZcvTree* tr = &st->trees[t];
            /* Standard cascades have one-feature stumps; we still loop. */
            for (int fi = 0; fi < tr->n_features; fi++) {
                const ZcvFeature* F = &tr->features[fi];
                double feat_sum = 0;
                for (int r = 0; r < F->n_rects; r++) {
                    int rx = win_x + (int)(F->rects[r].x * scale_x);
                    int ry = win_y + (int)(F->rects[r].y * scale_y);
                    int rw = (int)(F->rects[r].w * scale_x);
                    int rh = (int)(F->rects[r].h * scale_y);
                    if (rw <= 0 || rh <= 0) continue;
                    feat_sum += zcv_rect_sum(I, IW, rx, ry, rw, rh)
                                * F->rects[r].weight;
                }
                feat_sum *= inv_area_scale;
                double thr = F->threshold * stdev * area;
                stage_sum += (feat_sum < thr) ? F->left : F->right;
            }
        }
        if (stage_sum < st->threshold) return 0;     /* rejected here */
        total += (float)stage_sum;
    }
    if (out_score) *out_score = total;
    return 1;
}

/* Non-max suppression by IoU; keep highest-score detection in each cluster. */
static float zcv_iou(ZcvDetection a, ZcvDetection b) {
    int ix1 = a.x > b.x ? a.x : b.x;
    int iy1 = a.y > b.y ? a.y : b.y;
    int ix2 = (a.x + a.w) < (b.x + b.w) ? (a.x + a.w) : (b.x + b.w);
    int iy2 = (a.y + a.h) < (b.y + b.h) ? (a.y + a.h) : (b.y + b.h);
    int iw = ix2 - ix1; int ih = iy2 - iy1;
    if (iw <= 0 || ih <= 0) return 0;
    float inter = (float)iw * ih;
    float uni   = (float)(a.w * a.h + b.w * b.h) - inter;
    return uni > 0 ? inter / uni : 0;
}

static int zcv_nms_cmp(const void* a, const void* b) {
    float fa = ((ZcvDetection*)a)->score;
    float fb = ((ZcvDetection*)b)->score;
    if (fa < fb) return  1;
    if (fa > fb) return -1;
    return 0;
}

static int zcv_nms(ZcvDetection* in, int n, float thr) {
    qsort(in, n, sizeof(ZcvDetection), zcv_nms_cmp);
    int kept = 0;
    char* drop = (char*)calloc(n, 1);
    for (int i = 0; i < n; i++) {
        if (drop[i]) continue;
        in[kept++] = in[i];
        for (int j = i + 1; j < n; j++) {
            if (drop[j]) continue;
            if (zcv_iou(in[i], in[j]) > thr) drop[j] = 1;
        }
    }
    free(drop);
    return kept;
}

/* ============================================================
 * Z builtins.
 * ============================================================ */

static Value* b_cv_faces(int argc, Value** argv, Env* e) {
    (void)e;
    if (argc < 2 || argc > 3) z_raise("cv:faces: (cv:faces image-pgm cascade-zhc [opts])");
    const char* img_path = str_arg(argv[0], "cv:faces");
    const char* cas_path = str_arg(argv[1], "cv:faces");

    float scale_factor = 1.1f;
    int   min_size = 0, max_size = 0;
    int   merge = 1;
    if (argc == 3) {
        Value* o = argv[2];
        if (o->type != V_OBJECT) z_raise("cv:faces: opts must be an object");
        Value* sf = obj_get(&o->as.obj, "scale-factor");
        Value* mn = obj_get(&o->as.obj, "min-size");
        Value* mx = obj_get(&o->as.obj, "max-size");
        Value* mg = obj_get(&o->as.obj, "merge");
        if (sf && sf->type == V_NUM)  scale_factor = (float)sf->as.n;
        if (mn && mn->type == V_NUM)  min_size = (int)mn->as.n;
        if (mx && mx->type == V_NUM)  max_size = (int)mx->as.n;
        if (mg)                       merge    = is_truthy(mg);
    }
    if (scale_factor <= 1.0f) scale_factor = 1.05f;

    int w, h;
    unsigned char* img = zcv_read_pgm(img_path, &w, &h);
    if (!img) z_raise("cv:faces: cannot read PGM '%s' "
                      "(convert with `(img:grayscale src dst)` first)", img_path);

    ZcvCascade* cc = zcv_load_cascade(cas_path);
    uint32_t* I  = zcv_integral   (img, w, h);
    uint64_t* IS = zcv_integral_sq(img, w, h);
    int IW = w + 1, ISW = w + 1;

    if (min_size <= 0) min_size = cc->window_w;
    if (max_size <= 0) max_size = w < h ? w : h;

    int cap = 64, n_det = 0;
    ZcvDetection* dets = (ZcvDetection*)malloc(cap * sizeof(ZcvDetection));

    for (float scale = (float)min_size / cc->window_w;
         scale * cc->window_w <= max_size && scale * cc->window_h <= max_size;
         scale *= scale_factor) {
        int win_w = (int)(cc->window_w * scale);
        int win_h = (int)(cc->window_h * scale);
        if (win_w >= w || win_h >= h) break;
        /* Slide with a 5% step of window size — coarse but fast. */
        int step = win_w / 20; if (step < 1) step = 1;
        for (int y = 0; y + win_h < h; y += step) {
            for (int x = 0; x + win_w < w; x += step) {
                float score;
                if (!zcv_eval_window(cc, I, IW, IS, ISW,
                                     x, y, win_w, win_h, &score)) continue;
                if (n_det >= cap) {
                    cap *= 2;
                    dets = (ZcvDetection*)realloc(dets, cap * sizeof(ZcvDetection));
                }
                dets[n_det++] = (ZcvDetection){ x, y, win_w, win_h, score };
            }
        }
    }
    if (merge && n_det > 1) n_det = zcv_nms(dets, n_det, 0.3f);

    Value* out = v_array();
    for (int i = 0; i < n_det; i++) {
        Value* o = v_object();
        obj_set(&o->as.obj, "x",      v_num(dets[i].x));
        obj_set(&o->as.obj, "y",      v_num(dets[i].y));
        obj_set(&o->as.obj, "width",  v_num(dets[i].w));
        obj_set(&o->as.obj, "height", v_num(dets[i].h));
        obj_set(&o->as.obj, "score",  v_num(dets[i].score));
        vlist_push(&out->as.list, o);
    }
    free(dets);
    free(I);
    free(IS);
    free(img);
    return out;
}

/* (cv:read-pgm path) → { width, height, bytes } */
static Value* b_cv_read_pgm(int argc, Value** argv, Env* e) {
    (void)e; EXPECT_ARGC("cv:read-pgm", 1);
    const char* path = str_arg(argv[0], "cv:read-pgm");
    int w, h;
    unsigned char* img = zcv_read_pgm(path, &w, &h);
    if (!img) z_raise("cv:read-pgm: cannot read '%s'", path);
    Value* o = v_object();
    obj_set(&o->as.obj, "width",  v_num(w));
    obj_set(&o->as.obj, "height", v_num(h));
    obj_set(&o->as.obj, "bytes",  v_bytes_take(img, (size_t)w * (size_t)h));
    return o;
}

static Value* b_cv_save_pgm(int argc, Value** argv, Env* e) {
    (void)e; EXPECT_ARGC("cv:save-pgm", 4);
    const char* path = str_arg(argv[0], "cv:save-pgm");
    int w = (int)num_arg(argv[1], "cv:save-pgm");
    int h = (int)num_arg(argv[2], "cv:save-pgm");
    if (argv[3]->type != V_BYTES) z_raise("cv:save-pgm: data must be bytes");
    if ((int)argv[3]->as.bytes.len < w * h)
        z_raise("cv:save-pgm: bytes too short (%zu < %d * %d)",
                argv[3]->as.bytes.len, w, h);
    if (!zcv_write_pgm(path, w, h, argv[3]->as.bytes.data))
        z_raise("cv:save-pgm: cannot write '%s'", path);
    return v_true();
}

static Value* b_cv_image_info(int argc, Value** argv, Env* e) {
    (void)e; EXPECT_ARGC("cv:image-info", 1);
    const char* path = str_arg(argv[0], "cv:image-info");
    int w, h;
    unsigned char* img = zcv_read_pgm(path, &w, &h);
    if (!img) z_raise("cv:image-info: cannot read '%s'", path);
    free(img);
    Value* o = v_object();
    obj_set(&o->as.obj, "width",  v_num(w));
    obj_set(&o->as.obj, "height", v_num(h));
    obj_set(&o->as.obj, "format", v_str("pgm"));
    return o;
}

static void install_cv_builtins(Env* env) {
    env_define(env, "cv:faces",      v_native(b_cv_faces));
    env_define(env, "cv:read-pgm",   v_native(b_cv_read_pgm));
    env_define(env, "cv:save-pgm",   v_native(b_cv_save_pgm));
    env_define(env, "cv:image-info", v_native(b_cv_image_info));
}

#endif /* Z_WITH_CV */
#endif /* Z_CV_H_INCLUDED */
