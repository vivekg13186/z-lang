/*
 * z_http.h — optional libcurl-backed HTTP module for z.
 *
 * Compiled into z only when -DZ_WITH_LIBCURL is set:
 *
 *   make LIBCURL=1
 *
 * Replaces the default shell-out path (which fork/execs `curl`) with a
 * direct link against libcurl. Benefits:
 *   - no `curl` binary required on PATH
 *   - much faster (no process spawn per call)
 *   - clean error messages from CURLE_* codes instead of curl's stderr
 *
 * Install dev headers:
 *   apt-get install libcurl4-openssl-dev
 *   dnf install libcurl-devel
 *   brew install curl                (Homebrew links pkg-config for `libcurl`)
 *
 * Both http:get and http:post accept an `opts` object as the LAST
 * argument (after headers). Recognised keys:
 *
 *   verify-ssl       (bool, default true)   set false to ignore certificate
 *                                           and hostname verification
 *   follow-redirects (bool, default false)  CURLOPT_FOLLOWLOCATION
 *   max-redirects    (int,  default 10)     CURLOPT_MAXREDIRS
 *   timeout          (number of seconds; 0 = no timeout)
 *   user-agent       (string, default "z/${Z_VERSION}")
 *
 *   (http:get  url [headers] [opts])
 *   (http:post url body [headers] [opts])
 */

#ifndef Z_HTTP_H_INCLUDED
#define Z_HTTP_H_INCLUDED

#ifdef Z_WITH_LIBCURL

#include <curl/curl.h>

/* libcurl needs one global init / cleanup pair per process. We do it
 * lazily on first request so callers don't pay the cost when they don't
 * use HTTP. */
static int   g_zh_inited = 0;
static void  zh_global_init(void) {
    if (!g_zh_inited) {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        g_zh_inited = 1;
    }
}

/* Growing buffer for the response body. */
typedef struct { char* data; size_t len, cap; } ZhBuf;
static size_t zh_write_cb(void* ptr, size_t size, size_t nmemb, void* user) {
    ZhBuf* b = (ZhBuf*)user;
    size_t add = size * nmemb;
    if (b->len + add + 1 > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 4096;
        while (nc < b->len + add + 1) nc *= 2;
        b->data = (char*)realloc(b->data, nc);
        b->cap = nc;
    }
    memcpy(b->data + b->len, ptr, add);
    b->len += add;
    b->data[b->len] = 0;
    return add;
}

/* Convert a z object of headers into a curl_slist. */
static struct curl_slist* zh_build_headers(Value* headers, const char* fn) {
    if (!headers || headers->type == V_NULL) return NULL;
    if (headers->type != V_OBJECT) z_raise("%s: headers must be an object", fn);
    struct curl_slist* list = NULL;
    char line[2048], num[64];
    for (size_t i = 0; i < headers->as.obj.len; i++) {
        Value* v = headers->as.obj.vals[i];
        const char* val;
        if      (v->type == V_STR)  val = v->as.s;
        else if (v->type == V_NUM)  { snprintf(num, sizeof(num), "%g", v->as.n); val = num; }
        else if (v->type == V_BOOL) { val = v->as.b ? "true" : "false"; }
        else { curl_slist_free_all(list); z_raise("%s: header values must be string/number/boolean", fn); }
        snprintf(line, sizeof(line), "%s: %s", headers->as.obj.keys[i], val);
        list = curl_slist_append(list, line);
    }
    return list;
}

/* Read a bool/number/string option from the opts object with a default. */
static int    zh_opt_bool(Value* opts, const char* k, int    def);
static double zh_opt_num (Value* opts, const char* k, double def);
static const char* zh_opt_str(Value* opts, const char* k, const char* def);

/* zh_env_insecure() lives in z.c, declared before this header is included.
 * Forward-declare it here so this header is self-contained. */
static int zh_env_insecure(void);

/* Apply common options + perform; returns the body as a z string. */
static Value* zh_perform(CURL* curl, Value* opts, const char* fn) {
    long verify = zh_opt_bool(opts, "verify-ssl", 1);
    if (zh_env_insecure()) verify = 0;   /* Z_HTTP_INSECURE=1 forces off */
    if (!verify) {
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    }
    if (zh_opt_bool(opts, "follow-redirects", 0)) {
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        long max_red = (long)zh_opt_num(opts, "max-redirects", 10);
        curl_easy_setopt(curl, CURLOPT_MAXREDIRS, max_red);
    }
    double timeout = zh_opt_num(opts, "timeout", 0);
    if (timeout > 0) curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long)timeout);

    char ua_buf[128];
    snprintf(ua_buf, sizeof(ua_buf), "z/%s", Z_VERSION);
    curl_easy_setopt(curl, CURLOPT_USERAGENT,
                     zh_opt_str(opts, "user-agent", ua_buf));

    ZhBuf body = {0};
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, zh_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,     &body);

    CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) {
        char msg[256];
        snprintf(msg, sizeof(msg), "%s: %s", fn, curl_easy_strerror(rc));
        free(body.data);
        z_raise("%s", msg);
    }
    if (!body.data) body.data = (char*)calloc(1, 1);
    return v_str_take(body.data);
}

static Value* b_http_get_libcurl(int argc, Value** argv, Env* e) {
    (void)e;
    if (argc < 1 || argc > 3)
        z_raise("http:get: expected (http:get url [headers] [opts])");
    const char* url = str_arg(argv[0], "http:get");
    Value* headers = argc >= 2 ? argv[1] : NULL;
    Value* opts    = argc >= 3 ? argv[2] : NULL;

    zh_global_init();
    CURL* curl = curl_easy_init();
    if (!curl) z_raise("http:get: curl_easy_init failed");
    curl_easy_setopt(curl, CURLOPT_URL, url);
    struct curl_slist* hdrs = zh_build_headers(headers, "http:get");
    if (hdrs) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    Value* r = zh_perform(curl, opts, "http:get");
    if (hdrs) curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);
    return r;
}

static Value* b_http_post_libcurl(int argc, Value** argv, Env* e) {
    if (argc < 2 || argc > 4)
        z_raise("http:post: expected (http:post url body [headers] [opts])");
    const char* url = str_arg(argv[0], "http:post");
    Value* body    = argv[1];
    Value* headers = argc >= 3 ? argv[2] : NULL;
    Value* opts    = argc >= 4 ? argv[3] : NULL;

    /* Stringify body — same convention as the shell-out version. */
    const char* body_str;
    Value* owned = NULL;
    if (body->type == V_STR) body_str = body->as.s;
    else if (body->type == V_BYTES) body_str = (const char*)body->as.bytes.data;
    else {
        Value* json_args[1] = { body };
        owned = b_json_stringify(1, json_args, e);
        body_str = owned->as.s;
    }
    size_t body_len = (body->type == V_BYTES) ? body->as.bytes.len : strlen(body_str);

    zh_global_init();
    CURL* curl = curl_easy_init();
    if (!curl) z_raise("http:post: curl_easy_init failed");
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS,     body_str);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body_len);

    /* Default Content-Type only if the caller didn't supply one. */
    int have_ct = 0;
    if (headers && headers->type == V_OBJECT) {
        for (size_t i = 0; i < headers->as.obj.len; i++)
            if (z_strcaseeq(headers->as.obj.keys[i], "Content-Type")) { have_ct = 1; break; }
    }
    struct curl_slist* hdrs = zh_build_headers(headers, "http:post");
    if (!have_ct) {
        const char* def = (body->type == V_STR)   ? "Content-Type: text/plain"
                         : (body->type == V_BYTES) ? "Content-Type: application/octet-stream"
                         :                           "Content-Type: application/json";
        hdrs = curl_slist_append(hdrs, def);
    }
    if (hdrs) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);

    Value* r = zh_perform(curl, opts, "http:post");
    if (hdrs) curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);
    return r;
}

/* DELETE — like GET but with CUSTOMREQUEST=DELETE. */
static Value* b_http_delete_libcurl(int argc, Value** argv, Env* e) {
    (void)e;
    if (argc < 1 || argc > 3)
        z_raise("http:delete: expected (http:delete url [headers] [opts])");
    const char* url = str_arg(argv[0], "http:delete");
    Value* headers = argc >= 2 ? argv[1] : NULL;
    Value* opts    = argc >= 3 ? argv[2] : NULL;

    zh_global_init();
    CURL* curl = curl_easy_init();
    if (!curl) z_raise("http:delete: curl_easy_init failed");
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
    struct curl_slist* hdrs = zh_build_headers(headers, "http:delete");
    if (hdrs) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    Value* r = zh_perform(curl, opts, "http:delete");
    if (hdrs) curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);
    return r;
}

/* PUT — like POST but with CUSTOMREQUEST=PUT. */
static Value* b_http_put_libcurl(int argc, Value** argv, Env* e) {
    if (argc < 2 || argc > 4)
        z_raise("http:put: expected (http:put url body [headers] [opts])");
    const char* url = str_arg(argv[0], "http:put");
    Value* body    = argv[1];
    Value* headers = argc >= 3 ? argv[2] : NULL;
    Value* opts    = argc >= 4 ? argv[3] : NULL;

    const char* body_str;
    Value* owned = NULL;
    if (body->type == V_STR) body_str = body->as.s;
    else if (body->type == V_BYTES) body_str = (const char*)body->as.bytes.data;
    else {
        Value* json_args[1] = { body };
        owned = b_json_stringify(1, json_args, e);
        body_str = owned->as.s;
    }
    size_t body_len = (body->type == V_BYTES) ? body->as.bytes.len : strlen(body_str);

    zh_global_init();
    CURL* curl = curl_easy_init();
    if (!curl) z_raise("http:put: curl_easy_init failed");
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS,     body_str);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body_len);

    int have_ct = 0;
    if (headers && headers->type == V_OBJECT) {
        for (size_t i = 0; i < headers->as.obj.len; i++)
            if (z_strcaseeq(headers->as.obj.keys[i], "Content-Type")) { have_ct = 1; break; }
    }
    struct curl_slist* hdrs = zh_build_headers(headers, "http:put");
    if (!have_ct) {
        const char* def = (body->type == V_STR)   ? "Content-Type: text/plain"
                         : (body->type == V_BYTES) ? "Content-Type: application/octet-stream"
                         :                           "Content-Type: application/json";
        hdrs = curl_slist_append(hdrs, def);
    }
    if (hdrs) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);

    Value* r = zh_perform(curl, opts, "http:put");
    if (hdrs) curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);
    return r;
}

/* HEAD — fetches headers only. Body callback is unused; instead a header
 * callback accumulates the raw response headers as the returned string,
 * so callers can grep them just like `curl -I` output. */
static size_t zh_hdr_cb(char* ptr, size_t size, size_t nmemb, void* user) {
    return zh_write_cb(ptr, size, nmemb, user);
}
static Value* b_http_head_libcurl(int argc, Value** argv, Env* e) {
    (void)e;
    if (argc < 1 || argc > 3)
        z_raise("http:head: expected (http:head url [headers] [opts])");
    const char* url = str_arg(argv[0], "http:head");
    Value* headers = argc >= 2 ? argv[1] : NULL;
    Value* opts    = argc >= 3 ? argv[2] : NULL;

    zh_global_init();
    CURL* curl = curl_easy_init();
    if (!curl) z_raise("http:head: curl_easy_init failed");
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);

    /* Apply our shared opts (verify-ssl, follow-redirects, timeout, UA). */
    long verify = zh_opt_bool(opts, "verify-ssl", 1);
    if (zh_env_insecure()) verify = 0;
    if (!verify) {
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    }
    if (zh_opt_bool(opts, "follow-redirects", 0)) {
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        long max_red = (long)zh_opt_num(opts, "max-redirects", 10);
        curl_easy_setopt(curl, CURLOPT_MAXREDIRS, max_red);
    }
    double timeout = zh_opt_num(opts, "timeout", 0);
    if (timeout > 0) curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long)timeout);
    char ua_buf[128];
    snprintf(ua_buf, sizeof(ua_buf), "z/%s", Z_VERSION);
    curl_easy_setopt(curl, CURLOPT_USERAGENT,
                     zh_opt_str(opts, "user-agent", ua_buf));

    struct curl_slist* hdrs = zh_build_headers(headers, "http:head");
    if (hdrs) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);

    /* Capture response headers (not the body) into the returned string. */
    ZhBuf out = {0};
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, zh_hdr_cb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA,     &out);

    CURLcode rc = curl_easy_perform(curl);
    if (hdrs) curl_slist_free_all(hdrs);
    if (rc != CURLE_OK) {
        char msg[256];
        snprintf(msg, sizeof(msg), "http:head: %s", curl_easy_strerror(rc));
        free(out.data);
        curl_easy_cleanup(curl);
        z_raise("%s", msg);
    }
    curl_easy_cleanup(curl);
    if (!out.data) out.data = (char*)calloc(1, 1);
    return v_str_take(out.data);
}

#endif /* Z_WITH_LIBCURL */
#endif /* Z_HTTP_H_INCLUDED */
