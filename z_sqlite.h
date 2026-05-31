/*
 * z_sqlite.h — optional SQLite + key-value module for z.
 *
 * Compiled into z only when -DZ_WITH_SQLITE is set:
 *
 *   make SQLITE=1
 *
 * Unlike z_img.h / z_vision.h (which shell out to external CLI tools),
 * this module *links* against libsqlite3 directly — sqlite is small,
 * widely available, and the wire-level marshalling pain of a CLI bridge
 * isn't worth it. Pass `make SQLITE=1` and the Makefile adds -lsqlite3.
 *
 * Install:
 *   macOS:    brew install sqlite       (headers in /opt/homebrew or /usr/local)
 *   Debian:   sudo apt-get install libsqlite3-dev
 *   Fedora:   sudo dnf install sqlite-devel
 *   Windows:  pacman -S mingw-w64-x86_64-sqlite3   (MSYS2)
 *
 * Builtins:
 *
 *   (sqlite:open  "path-or-:memory:")    → handle
 *   (sqlite:exec  db sql [params])       run statements; returns affected-row count
 *   (sqlite:query db sql [params])       → array of row-objects (column-name keyed)
 *   (sqlite:close db)                    → true; releases the underlying handle
 *   (sqlite:last-insert-id db)           → integer rowid
 *
 * Parameters:
 *   - Array  → positional ? placeholders
 *   - Object → named :foo / @foo placeholders
 *   - null   → no params
 *
 * Cell type mapping (both directions):
 *     SQLite          ↔    z
 *     NULL                 null
 *     INTEGER              number   (always written as int64 when ts is whole)
 *     REAL                 number
 *     TEXT                 string
 *     BLOB                 bytes    (z's first-class binary type — no NUL hazard)
 *
 * KV wrapper — convenient key/value store on top of a single sqlite db:
 *
 *   (kv:open  "kv.db")        → handle (creates `kv (k TEXT PRIMARY KEY, v BLOB)`)
 *   (kv:set   store key val)
 *   (kv:get   store key)      → value or null
 *   (kv:del   store key)      → boolean (was anything deleted)
 *   (kv:keys  store [prefix]) → array of strings, sorted
 *
 * KV values can be any z value sqlite accepts (number/string/bytes/null).
 * For arrays/objects, json:stringify them first.
 */

#ifdef Z_WITH_SQLITE

#include <sqlite3.h>

/* Handle table — each open db gets a small integer id. Handles fit in 256
 * which is well over any reasonable z-script workload. */
#define ZSQ_MAX_HANDLES 256
static sqlite3* g_zsq_handles[ZSQ_MAX_HANDLES];
static int      g_zsq_count = 0;

static int zsq_register(sqlite3* db) {
    /* Reuse a freed slot if one's available. */
    for (int i = 0; i < g_zsq_count; i++)
        if (g_zsq_handles[i] == NULL) { g_zsq_handles[i] = db; return i; }
    if (g_zsq_count >= ZSQ_MAX_HANDLES) return -1;
    int id = g_zsq_count++;
    g_zsq_handles[id] = db;
    return id;
}

static sqlite3* zsq_lookup(Value* h, const char* fn, const char* expect_tag) {
    if (h->type != V_OBJECT)
        z_raise("%s: expected a %s handle, got %s", fn, expect_tag, type_name(h));
    Value* tag = obj_get(&h->as.obj, "__type");
    if (!tag || tag->type != V_STR
        || (strcmp(tag->as.s, expect_tag) != 0
            && !(strcmp(expect_tag, "sqlite") == 0
                 && strcmp(tag->as.s, "sqlite-kv") == 0))) {
        z_raise("%s: not a %s handle", fn, expect_tag);
    }
    Value* id = obj_get(&h->as.obj, "id");
    if (!id || id->type != V_NUM) z_raise("%s: malformed handle", fn);
    int i = (int)id->as.n;
    if (i < 0 || i >= g_zsq_count || !g_zsq_handles[i])
        z_raise("%s: stale or closed handle", fn);
    return g_zsq_handles[i];
}

static void zsq_bind(sqlite3_stmt* stmt, int idx, Value* v, const char* fn) {
    switch (v->type) {
        case V_NULL:  sqlite3_bind_null(stmt, idx); break;
        case V_BOOL:  sqlite3_bind_int(stmt, idx, v->as.b); break;
        case V_NUM: {
            double n = v->as.n;
            if (n == (double)(long long)n && n > -9.0e18 && n < 9.0e18)
                sqlite3_bind_int64(stmt, idx, (long long)n);
            else
                sqlite3_bind_double(stmt, idx, n);
            break;
        }
        case V_STR:
            sqlite3_bind_text(stmt, idx, v->as.s, -1, SQLITE_TRANSIENT);
            break;
        case V_BYTES:
            sqlite3_bind_blob(stmt, idx, v->as.bytes.data,
                              (int)v->as.bytes.len, SQLITE_TRANSIENT);
            break;
        default:
            z_raise("%s: cannot bind a value of type %s", fn, type_name(v));
    }
}

static void zsq_bind_params(sqlite3_stmt* stmt, Value* params, const char* fn) {
    if (!params || params->type == V_NULL) return;
    if (params->type == V_ARRAY || params->type == V_LIST) {
        int n = sqlite3_bind_parameter_count(stmt);
        if ((int)params->as.list.len > n)
            z_raise("%s: %zu parameters supplied but SQL has only %d placeholders",
                    fn, params->as.list.len, n);
        for (size_t i = 0; i < params->as.list.len; i++)
            zsq_bind(stmt, (int)i + 1, params->as.list.items[i], fn);
        return;
    }
    if (params->type == V_OBJECT) {
        for (size_t i = 0; i < params->as.obj.len; i++) {
            char tag[128];
            snprintf(tag, sizeof(tag), ":%s", params->as.obj.keys[i]);
            int idx = sqlite3_bind_parameter_index(stmt, tag);
            if (idx == 0) {
                snprintf(tag, sizeof(tag), "@%s", params->as.obj.keys[i]);
                idx = sqlite3_bind_parameter_index(stmt, tag);
            }
            if (idx > 0) zsq_bind(stmt, idx, params->as.obj.vals[i], fn);
        }
        return;
    }
    z_raise("%s: parameters must be an array (positional) or object (named)", fn);
}

static Value* zsq_col_value(sqlite3_stmt* stmt, int i) {
    switch (sqlite3_column_type(stmt, i)) {
        case SQLITE_NULL:    return v_null();
        case SQLITE_INTEGER: return v_num((double)sqlite3_column_int64(stmt, i));
        case SQLITE_FLOAT:   return v_num(sqlite3_column_double(stmt, i));
        case SQLITE_TEXT: {
            const unsigned char* s = sqlite3_column_text(stmt, i);
            return v_str(s ? (const char*)s : "");
        }
        case SQLITE_BLOB: {
            const void* b = sqlite3_column_blob(stmt, i);
            int n = sqlite3_column_bytes(stmt, i);
            return v_bytes((const unsigned char*)b, (size_t)n);
        }
        default: return v_null();
    }
}

/* ---- sqlite:* ---- */

static Value* b_sqlite_open(int argc, Value** argv, Env* e) {
    (void)e; EXPECT_ARGC("sqlite:open", 1);
    const char* path = str_arg(argv[0], "sqlite:open");
    sqlite3* db = NULL;
    if (sqlite3_open(path, &db) != SQLITE_OK) {
        const char* msg = db ? sqlite3_errmsg(db) : "(could not open)";
        char tmp[256]; snprintf(tmp, sizeof(tmp), "sqlite:open: %s", msg);
        if (db) sqlite3_close(db);
        z_raise("%s", tmp);
    }
    int id = zsq_register(db);
    if (id < 0) { sqlite3_close(db); z_raise("sqlite:open: too many open handles"); }
    Value* h = v_object();
    obj_set(&h->as.obj, "__type", v_str("sqlite"));
    obj_set(&h->as.obj, "id",     v_num(id));
    obj_set(&h->as.obj, "path",   v_str(path));
    return h;
}

static Value* b_sqlite_exec(int argc, Value** argv, Env* e) {
    (void)e;
    if (argc < 2 || argc > 3) z_raise("sqlite:exec: (sqlite:exec db sql [params])");
    sqlite3* db = zsq_lookup(argv[0], "sqlite:exec", "sqlite");
    const char* sql = str_arg(argv[1], "sqlite:exec");
    /* No params → use sqlite3_exec so multi-statement scripts work
     * (CREATE TABLE foo; CREATE INDEX bar ...). */
    if (argc < 3 || argv[2]->type == V_NULL) {
        char* err = NULL;
        if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK) {
            char msg[512]; snprintf(msg, sizeof(msg), "sqlite:exec: %s", err ? err : "(unknown)");
            sqlite3_free(err);
            z_raise("%s", msg);
        }
        return v_num((double)sqlite3_changes(db));
    }
    sqlite3_stmt* stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        z_raise("sqlite:exec: %s", sqlite3_errmsg(db));
    zsq_bind_params(stmt, argv[2], "sqlite:exec");
    int rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
        char msg[512]; snprintf(msg, sizeof(msg), "sqlite:exec: %s", sqlite3_errmsg(db));
        sqlite3_finalize(stmt);
        z_raise("%s", msg);
    }
    int n = sqlite3_changes(db);
    sqlite3_finalize(stmt);
    return v_num((double)n);
}

static Value* b_sqlite_query(int argc, Value** argv, Env* e) {
    (void)e;
    if (argc < 2 || argc > 3) z_raise("sqlite:query: (sqlite:query db sql [params])");
    sqlite3* db = zsq_lookup(argv[0], "sqlite:query", "sqlite");
    const char* sql = str_arg(argv[1], "sqlite:query");
    sqlite3_stmt* stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        z_raise("sqlite:query: %s", sqlite3_errmsg(db));
    if (argc == 3) zsq_bind_params(stmt, argv[2], "sqlite:query");
    int ncols = sqlite3_column_count(stmt);
    Value* rows = v_array();
    while (1) {
        int rc = sqlite3_step(stmt);
        if (rc == SQLITE_DONE) break;
        if (rc != SQLITE_ROW) {
            char msg[512]; snprintf(msg, sizeof(msg), "sqlite:query: %s", sqlite3_errmsg(db));
            sqlite3_finalize(stmt);
            z_raise("%s", msg);
        }
        Value* row = v_object();
        for (int i = 0; i < ncols; i++) {
            const char* name = sqlite3_column_name(stmt, i);
            obj_set(&row->as.obj, name ? name : "?", zsq_col_value(stmt, i));
        }
        vlist_push(&rows->as.list, row);
    }
    sqlite3_finalize(stmt);
    return rows;
}

static Value* b_sqlite_close(int argc, Value** argv, Env* e) {
    (void)e; EXPECT_ARGC("sqlite:close", 1);
    Value* h = argv[0];
    if (h->type != V_OBJECT) z_raise("sqlite:close: not a handle");
    Value* id = obj_get(&h->as.obj, "id");
    if (!id || id->type != V_NUM) z_raise("sqlite:close: malformed handle");
    int i = (int)id->as.n;
    if (i >= 0 && i < g_zsq_count && g_zsq_handles[i]) {
        sqlite3_close(g_zsq_handles[i]);
        g_zsq_handles[i] = NULL;
    }
    return v_true();
}

static Value* b_sqlite_last_id(int argc, Value** argv, Env* e) {
    (void)e; EXPECT_ARGC("sqlite:last-insert-id", 1);
    sqlite3* db = zsq_lookup(argv[0], "sqlite:last-insert-id", "sqlite");
    return v_num((double)sqlite3_last_insert_rowid(db));
}

/* ---- kv:* convenience wrapper ---- */

static Value* b_kv_open(int argc, Value** argv, Env* e) {
    (void)e; EXPECT_ARGC("kv:open", 1);
    Value* db_h = b_sqlite_open(1, argv, e);
    sqlite3* db = zsq_lookup(db_h, "kv:open", "sqlite");
    char* err = NULL;
    const char* schema =
        "CREATE TABLE IF NOT EXISTS kv ("
        "  k TEXT PRIMARY KEY,"
        "  v BLOB"
        ")";
    if (sqlite3_exec(db, schema, NULL, NULL, &err) != SQLITE_OK) {
        char msg[256]; snprintf(msg, sizeof(msg), "kv:open: %s", err ? err : "?");
        sqlite3_free(err);
        z_raise("%s", msg);
    }
    /* Retag as kv so kv:* probes accept it without coupling. */
    obj_set(&db_h->as.obj, "__type", v_str("sqlite-kv"));
    return db_h;
}

static Value* b_kv_set(int argc, Value** argv, Env* e) {
    (void)e; EXPECT_ARGC("kv:set", 3);
    sqlite3* db = zsq_lookup(argv[0], "kv:set", "sqlite-kv");
    const char* k = str_arg(argv[1], "kv:set");
    sqlite3_stmt* stmt = NULL;
    sqlite3_prepare_v2(db,
        "INSERT INTO kv (k,v) VALUES (?,?)"
        " ON CONFLICT(k) DO UPDATE SET v=excluded.v",
        -1, &stmt, NULL);
    sqlite3_bind_text(stmt, 1, k, -1, SQLITE_TRANSIENT);
    zsq_bind(stmt, 2, argv[2], "kv:set");
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) z_raise("kv:set: %s", sqlite3_errmsg(db));
    return v_true();
}

static Value* b_kv_get(int argc, Value** argv, Env* e) {
    (void)e; EXPECT_ARGC("kv:get", 2);
    sqlite3* db = zsq_lookup(argv[0], "kv:get", "sqlite-kv");
    const char* k = str_arg(argv[1], "kv:get");
    sqlite3_stmt* stmt = NULL;
    sqlite3_prepare_v2(db, "SELECT v FROM kv WHERE k = ?", -1, &stmt, NULL);
    sqlite3_bind_text(stmt, 1, k, -1, SQLITE_TRANSIENT);
    Value* r = v_null();
    if (sqlite3_step(stmt) == SQLITE_ROW) r = zsq_col_value(stmt, 0);
    sqlite3_finalize(stmt);
    return r;
}

static Value* b_kv_del(int argc, Value** argv, Env* e) {
    (void)e; EXPECT_ARGC("kv:del", 2);
    sqlite3* db = zsq_lookup(argv[0], "kv:del", "sqlite-kv");
    const char* k = str_arg(argv[1], "kv:del");
    sqlite3_stmt* stmt = NULL;
    sqlite3_prepare_v2(db, "DELETE FROM kv WHERE k = ?", -1, &stmt, NULL);
    sqlite3_bind_text(stmt, 1, k, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    int n = sqlite3_changes(db);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) z_raise("kv:del: %s", sqlite3_errmsg(db));
    return v_bool(n > 0);
}

static Value* b_kv_keys(int argc, Value** argv, Env* e) {
    (void)e;
    if (argc < 1 || argc > 2) z_raise("kv:keys: (kv:keys db [prefix])");
    sqlite3* db = zsq_lookup(argv[0], "kv:keys", "sqlite-kv");
    sqlite3_stmt* stmt = NULL;
    if (argc == 2) {
        const char* prefix = str_arg(argv[1], "kv:keys");
        sqlite3_prepare_v2(db, "SELECT k FROM kv WHERE k LIKE ? ORDER BY k",
                           -1, &stmt, NULL);
        char pat[512]; snprintf(pat, sizeof(pat), "%s%%", prefix);
        sqlite3_bind_text(stmt, 1, pat, -1, SQLITE_TRANSIENT);
    } else {
        sqlite3_prepare_v2(db, "SELECT k FROM kv ORDER BY k", -1, &stmt, NULL);
    }
    Value* arr = v_array();
    while (sqlite3_step(stmt) == SQLITE_ROW)
        vlist_push(&arr->as.list, v_str((const char*)sqlite3_column_text(stmt, 0)));
    sqlite3_finalize(stmt);
    return arr;
}

static void install_sqlite_builtins(Env* env) {
    env_define(env, "sqlite:open",           v_native(b_sqlite_open));
    env_define(env, "sqlite:exec",           v_native(b_sqlite_exec));
    env_define(env, "sqlite:query",          v_native(b_sqlite_query));
    env_define(env, "sqlite:close",          v_native(b_sqlite_close));
    env_define(env, "sqlite:last-insert-id", v_native(b_sqlite_last_id));
    env_define(env, "kv:open",  v_native(b_kv_open));
    env_define(env, "kv:set",   v_native(b_kv_set));
    env_define(env, "kv:get",   v_native(b_kv_get));
    env_define(env, "kv:del",   v_native(b_kv_del));
    env_define(env, "kv:keys",  v_native(b_kv_keys));
}

#endif /* Z_WITH_SQLITE */
