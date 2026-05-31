/*
 * z_html.h — minimal HTML/XML query module for z.
 *
 * No external dependencies. Compiled into z unconditionally (small enough
 * to justify being a core feature). The same parser handles both HTML and
 * XML; the difference is just which void-elements get auto-closed.
 *
 * Builtins:
 *   (html:query  selector html)   → array of outer-HTML strings
 *   (html:text   html-fragment)   → concatenated text content (tags stripped)
 *   (html:attr   name fragment)   → value of `name` on outermost tag, or null
 *   (xml:query   path  xml)       → array of outer-XML strings; path is
 *                                   "/a/b/c" — walks element names
 *   (xml:text    xml-fragment)    → text content of XML
 *   (xml:attr    name fragment)   → outermost-element attribute
 *
 * Supported CSS selector syntax (subset):
 *     tag           tag-name match (case-insensitive)
 *     *             any tag
 *     .class        class attribute contains token
 *     #id           id attribute equals
 *     [attr]        attribute exists
 *     [attr=value]  attribute equals (quoted or unquoted)
 *     [attr*=v]     attribute contains substring
 *     [attr^=v]     attribute starts-with
 *     [attr$=v]     attribute ends-with
 *     tag.cls#id    compound (and-of-simple-selectors)
 *     a b           descendant combinator (any depth)
 *     a > b         direct-child combinator
 *
 * What's NOT supported: pseudo-classes, sibling combinators (~, +),
 * attribute namespaces, negation, CSS escapes. Plenty for typical scraping.
 */

#ifndef Z_HTML_H_INCLUDED
#define Z_HTML_H_INCLUDED

/* ============================================================
 * Lightweight string helpers — case-insensitive compare, dup-n.
 * ============================================================ */

static int  zh_strcaseeq(const char* a, const char* b) {
    while (*a && *b) {
        int ca = (*a >= 'A' && *a <= 'Z') ? (*a | 0x20) : *a;
        int cb = (*b >= 'A' && *b <= 'Z') ? (*b | 0x20) : *b;
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}
static int zh_strncaseeq(const char* a, const char* b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        int ca = (a[i] >= 'A' && a[i] <= 'Z') ? (a[i] | 0x20) : a[i];
        int cb = (b[i] >= 'A' && b[i] <= 'Z') ? (b[i] | 0x20) : b[i];
        if (ca != cb) return 0;
        if (a[i] == 0) return 1;
    }
    return 1;
}

static char* zh_strndup(const char* s, size_t n) {
    char* r = (char*)malloc(n + 1);
    memcpy(r, s, n); r[n] = 0;
    return r;
}

static char* zh_tolower_dup(const char* s, size_t n) {
    char* r = (char*)malloc(n + 1);
    for (size_t i = 0; i < n; i++)
        r[i] = (s[i] >= 'A' && s[i] <= 'Z') ? (s[i] | 0x20) : s[i];
    r[n] = 0;
    return r;
}

/* ============================================================
 * Node tree.
 * ============================================================ */

typedef struct ZhNode {
    char* tag;                 /* lowercased tag name; NULL for text node */
    char* text;                /* for text nodes only */
    int   start, end;          /* byte indices into the original source —
                                  used to reconstruct outer HTML on demand */
    char** attr_keys;
    char** attr_vals;
    int    attr_count;
    struct ZhNode** children;
    int    child_count;
    int    child_cap;
} ZhNode;

static ZhNode* zh_new_tag(const char* name, size_t name_len) {
    ZhNode* n = (ZhNode*)calloc(1, sizeof(ZhNode));
    n->tag = zh_tolower_dup(name, name_len);
    return n;
}
static ZhNode* zh_new_text(const char* s, size_t len) {
    ZhNode* n = (ZhNode*)calloc(1, sizeof(ZhNode));
    n->text = zh_strndup(s, len);
    return n;
}

static void zh_add_child(ZhNode* parent, ZhNode* child) {
    if (parent->child_count + 1 > parent->child_cap) {
        parent->child_cap = parent->child_cap ? parent->child_cap * 2 : 4;
        parent->children = (ZhNode**)realloc(parent->children,
                                  parent->child_cap * sizeof(ZhNode*));
    }
    parent->children[parent->child_count++] = child;
}

static void zh_add_attr(ZhNode* n, const char* k, size_t klen,
                                  const char* v, size_t vlen) {
    n->attr_keys = (char**)realloc(n->attr_keys, (n->attr_count + 1) * sizeof(char*));
    n->attr_vals = (char**)realloc(n->attr_vals, (n->attr_count + 1) * sizeof(char*));
    n->attr_keys[n->attr_count] = zh_tolower_dup(k, klen);
    n->attr_vals[n->attr_count] = zh_strndup(v, vlen);
    n->attr_count++;
}

static void zh_free(ZhNode* n) {
    if (!n) return;
    free(n->tag);
    free(n->text);
    for (int i = 0; i < n->attr_count; i++) {
        free(n->attr_keys[i]);
        free(n->attr_vals[i]);
    }
    free(n->attr_keys);
    free(n->attr_vals);
    for (int i = 0; i < n->child_count; i++) zh_free(n->children[i]);
    free(n->children);
    free(n);
}

/* HTML void elements: self-close even without a slash. */
static int zh_is_void(const char* tag) {
    static const char* V[] = {
        "area","base","br","col","embed","hr","img","input","link","meta",
        "param","source","track","wbr", NULL
    };
    for (int i = 0; V[i]; i++) if (strcmp(tag, V[i]) == 0) return 1;
    return 0;
}

/* HTML raw-text elements: contents are not parsed (script, style). */
static int zh_is_rawtext(const char* tag) {
    return strcmp(tag, "script") == 0 || strcmp(tag, "style") == 0;
}

/* ============================================================
 * Parser. Builds a tree from a string.
 *   `xml_strict` = 1   → no void-element list; only `/>` self-closes;
 *                       case-sensitive tag names (preserved as-is)
 *   `xml_strict` = 0   → HTML mode (void elements, lowercased, raw text)
 * ============================================================ */

typedef struct {
    const char* src;
    int  len;
    int  pos;
    int  xml_strict;
} ZhParser;

static int zh_skip_ws(ZhParser* p) {
    int n = 0;
    while (p->pos < p->len
        && (p->src[p->pos] == ' ' || p->src[p->pos] == '\t'
         || p->src[p->pos] == '\n' || p->src[p->pos] == '\r')) {
        p->pos++; n++;
    }
    return n;
}

/* Parse one attribute starting at p->pos. Returns 1 on success. */
static int zh_parse_attr(ZhParser* p, ZhNode* into) {
    zh_skip_ws(p);
    if (p->pos >= p->len) return 0;
    char c = p->src[p->pos];
    if (c == '/' || c == '>') return 0;
    int ks = p->pos;
    while (p->pos < p->len
        && p->src[p->pos] != '=' && p->src[p->pos] != '>'
        && p->src[p->pos] != '/'
        && p->src[p->pos] != ' ' && p->src[p->pos] != '\t'
        && p->src[p->pos] != '\n' && p->src[p->pos] != '\r')
        p->pos++;
    int klen = p->pos - ks;
    if (klen == 0) return 0;

    zh_skip_ws(p);
    if (p->pos < p->len && p->src[p->pos] == '=') {
        p->pos++;
        zh_skip_ws(p);
        int vs, ve;
        if (p->pos < p->len && (p->src[p->pos] == '"' || p->src[p->pos] == '\'')) {
            char q = p->src[p->pos++];
            vs = p->pos;
            while (p->pos < p->len && p->src[p->pos] != q) p->pos++;
            ve = p->pos;
            if (p->pos < p->len) p->pos++;
        } else {
            vs = p->pos;
            while (p->pos < p->len
                && p->src[p->pos] != ' ' && p->src[p->pos] != '\t'
                && p->src[p->pos] != '\n' && p->src[p->pos] != '\r'
                && p->src[p->pos] != '>' && p->src[p->pos] != '/')
                p->pos++;
            ve = p->pos;
        }
        zh_add_attr(into, p->src + ks, (size_t)klen,
                    p->src + vs, (size_t)(ve - vs));
    } else {
        /* Bare attribute (no value) — store empty string. */
        zh_add_attr(into, p->src + ks, (size_t)klen, "", 0);
    }
    return 1;
}

/* Recursive parse. Returns when end-of-input or a closing tag for the
 * current parent. *closed_by is set to non-NULL if we hit a close that
 * doesn't match the current parent (so the caller can re-emit it). */
static void zh_parse_children(ZhParser* p, ZhNode* parent, const char* parent_tag) {
    while (p->pos < p->len) {
        /* Look for the next `<`. */
        int text_start = p->pos;
        while (p->pos < p->len && p->src[p->pos] != '<') p->pos++;
        if (p->pos > text_start) {
            ZhNode* tn = zh_new_text(p->src + text_start, (size_t)(p->pos - text_start));
            tn->start = text_start;
            tn->end   = p->pos;
            zh_add_child(parent, tn);
        }
        if (p->pos >= p->len) return;
        /* Comment / CDATA / DOCTYPE / processing-instruction — skip. */
        if (p->pos + 4 <= p->len && memcmp(p->src + p->pos, "<!--", 4) == 0) {
            p->pos += 4;
            while (p->pos + 3 <= p->len
                   && memcmp(p->src + p->pos, "-->", 3) != 0) p->pos++;
            if (p->pos + 3 <= p->len) p->pos += 3;
            continue;
        }
        if (p->pos + 9 <= p->len && memcmp(p->src + p->pos, "<![CDATA[", 9) == 0) {
            int cs = p->pos + 9;
            p->pos = cs;
            while (p->pos + 3 <= p->len
                   && memcmp(p->src + p->pos, "]]>", 3) != 0) p->pos++;
            int ce = p->pos;
            if (p->pos + 3 <= p->len) p->pos += 3;
            ZhNode* tn = zh_new_text(p->src + cs, (size_t)(ce - cs));
            tn->start = cs; tn->end = ce;
            zh_add_child(parent, tn);
            continue;
        }
        if (p->pos + 2 <= p->len && p->src[p->pos+1] == '!') {
            /* DOCTYPE or other declaration — skip to `>`. */
            while (p->pos < p->len && p->src[p->pos] != '>') p->pos++;
            if (p->pos < p->len) p->pos++;
            continue;
        }
        if (p->pos + 2 <= p->len && p->src[p->pos+1] == '?') {
            while (p->pos + 2 <= p->len
                   && !(p->src[p->pos] == '?' && p->src[p->pos+1] == '>')) p->pos++;
            if (p->pos + 2 <= p->len) p->pos += 2;
            continue;
        }
        /* Closing tag? */
        if (p->pos + 2 <= p->len && p->src[p->pos+1] == '/') {
            int save = p->pos;
            p->pos += 2;
            int ns = p->pos;
            while (p->pos < p->len && p->src[p->pos] != '>'
                && p->src[p->pos] != ' ' && p->src[p->pos] != '\t'
                && p->src[p->pos] != '\n' && p->src[p->pos] != '\r')
                p->pos++;
            int ne = p->pos;
            while (p->pos < p->len && p->src[p->pos] != '>') p->pos++;
            if (p->pos < p->len) p->pos++;
            if (parent_tag && zh_strncaseeq(p->src + ns, parent_tag,
                                            ne - ns) && strlen(parent_tag) == (size_t)(ne - ns)) {
                /* Properly closed. Note the parent's end position. */
                parent->end = p->pos;
                return;
            }
            /* Mismatched close — common in real HTML. Rewind so the caller
             * (an ancestor) can see it. */
            p->pos = save;
            return;
        }
        /* Opening tag. */
        int tag_start = p->pos;
        p->pos++; /* skip < */
        int ns = p->pos;
        while (p->pos < p->len
            && p->src[p->pos] != ' ' && p->src[p->pos] != '\t'
            && p->src[p->pos] != '\n' && p->src[p->pos] != '\r'
            && p->src[p->pos] != '>' && p->src[p->pos] != '/'
            && p->src[p->pos] != '<')
            p->pos++;
        int ne = p->pos;
        if (ne == ns) { p->pos++; continue; }  /* bare `<` */
        ZhNode* node = zh_new_tag(p->src + ns, (size_t)(ne - ns));
        node->start = tag_start;
        while (zh_parse_attr(p, node)) { /* loop */ }
        zh_skip_ws(p);
        int self_close = 0;
        if (p->pos < p->len && p->src[p->pos] == '/') {
            self_close = 1; p->pos++;
        }
        if (p->pos < p->len && p->src[p->pos] == '>') p->pos++;

        if (!self_close && !p->xml_strict && zh_is_void(node->tag))
            self_close = 1;

        if (self_close) {
            node->end = p->pos;
            zh_add_child(parent, node);
            continue;
        }

        if (!p->xml_strict && zh_is_rawtext(node->tag)) {
            /* Eat until the matching </tag> — don't parse internals. */
            int cs = p->pos;
            char closer[32];
            snprintf(closer, sizeof(closer), "</%s", node->tag);
            int clen = (int)strlen(closer);
            while (p->pos + clen <= p->len
                   && !zh_strncaseeq(p->src + p->pos, closer, (size_t)clen))
                p->pos++;
            int ce = p->pos;
            if (ce > cs) {
                ZhNode* tn = zh_new_text(p->src + cs, (size_t)(ce - cs));
                tn->start = cs; tn->end = ce;
                zh_add_child(node, tn);
            }
            while (p->pos < p->len && p->src[p->pos] != '>') p->pos++;
            if (p->pos < p->len) p->pos++;
            node->end = p->pos;
            zh_add_child(parent, node);
            continue;
        }

        /* Recurse for the contents. */
        zh_parse_children(p, node, node->tag);
        if (node->end == 0) node->end = p->pos;
        zh_add_child(parent, node);
    }
}

static ZhNode* zh_parse(const char* src, int xml_strict) {
    ZhParser p = { src, (int)strlen(src), 0, xml_strict };
    ZhNode* root = zh_new_tag("#document", 9);
    root->start = 0;
    zh_parse_children(&p, root, NULL);
    if (root->end == 0) root->end = p.len;
    return root;
}

/* ============================================================
 * CSS-selector subset parser + matcher.
 * ============================================================ */

typedef enum { ZH_AT_HAS, ZH_AT_EQ, ZH_AT_CONTAINS, ZH_AT_STARTS, ZH_AT_ENDS } ZhAttrOp;

typedef struct {
    char* tag;           /* NULL or "*" → any */
    char** cls;
    int    cls_n;
    char*  id;
    struct { char* key; ZhAttrOp op; char* val; } attrs[8];
    int    attr_n;
} ZhSimple;

typedef enum { ZH_C_DESC, ZH_C_CHILD } ZhCombinator;

typedef struct {
    ZhSimple* parts;
    ZhCombinator* combs;   /* combs[i] connects parts[i] to parts[i+1] */
    int n;
} ZhSelector;

static void zh_free_selector(ZhSelector* s) {
    if (!s) return;
    for (int i = 0; i < s->n; i++) {
        free(s->parts[i].tag);
        for (int j = 0; j < s->parts[i].cls_n; j++) free(s->parts[i].cls[j]);
        free(s->parts[i].cls);
        free(s->parts[i].id);
        for (int j = 0; j < s->parts[i].attr_n; j++) {
            free(s->parts[i].attrs[j].key);
            free(s->parts[i].attrs[j].val);
        }
    }
    free(s->parts);
    free(s->combs);
    free(s);
}

static int zh_is_ident_ch(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
        || (c >= '0' && c <= '9') || c == '-' || c == '_';
}

/* Parse one simple selector (tag.class#id[attr=val]…) starting at *pp. */
static int zh_parse_simple(const char** pp, ZhSimple* out) {
    memset(out, 0, sizeof(*out));
    const char* p = *pp;
    if (*p == '*') { out->tag = strdup("*"); p++; }
    else if (zh_is_ident_ch(*p)) {
        const char* s = p;
        while (zh_is_ident_ch(*p)) p++;
        out->tag = zh_tolower_dup(s, (size_t)(p - s));
    }
    while (*p) {
        if (*p == '.') {
            p++;
            const char* s = p;
            while (zh_is_ident_ch(*p)) p++;
            if (p > s) {
                out->cls = (char**)realloc(out->cls, (out->cls_n + 1) * sizeof(char*));
                out->cls[out->cls_n++] = zh_strndup(s, (size_t)(p - s));
            }
        } else if (*p == '#') {
            p++;
            const char* s = p;
            while (zh_is_ident_ch(*p)) p++;
            if (p > s) out->id = zh_strndup(s, (size_t)(p - s));
        } else if (*p == '[') {
            p++;
            const char* s = p;
            while (*p && *p != '=' && *p != ']' && *p != '*' && *p != '^' && *p != '$') p++;
            int klen = (int)(p - s);
            ZhAttrOp op = ZH_AT_HAS;
            char* val = NULL;
            if (*p == '*' && p[1] == '=') { op = ZH_AT_CONTAINS; p += 2; }
            else if (*p == '^' && p[1] == '=') { op = ZH_AT_STARTS; p += 2; }
            else if (*p == '$' && p[1] == '=') { op = ZH_AT_ENDS;   p += 2; }
            else if (*p == '=') { op = ZH_AT_EQ; p++; }
            if (op != ZH_AT_HAS) {
                char quote = 0;
                if (*p == '"' || *p == '\'') { quote = *p; p++; }
                const char* vs = p;
                while (*p && *p != ']' && (!quote || *p != quote)) p++;
                val = zh_strndup(vs, (size_t)(p - vs));
                if (quote && *p == quote) p++;
            }
            if (*p == ']') p++;
            if (klen > 0 && out->attr_n < (int)(sizeof(out->attrs)/sizeof(out->attrs[0]))) {
                out->attrs[out->attr_n].key = zh_tolower_dup(s, (size_t)klen);
                out->attrs[out->attr_n].op  = op;
                out->attrs[out->attr_n].val = val;
                out->attr_n++;
            } else {
                free(val);
            }
        } else {
            break;
        }
    }
    *pp = p;
    return out->tag || out->id || out->cls_n || out->attr_n;
}

/* Parse full selector: simple [ combinator simple ]*. */
static ZhSelector* zh_parse_selector(const char* s) {
    ZhSelector* sel = (ZhSelector*)calloc(1, sizeof(*sel));
    sel->parts = (ZhSimple*)calloc(16, sizeof(ZhSimple));
    sel->combs = (ZhCombinator*)calloc(16, sizeof(ZhCombinator));
    while (*s == ' ') s++;
    while (*s && sel->n < 16) {
        if (!zh_parse_simple(&s, &sel->parts[sel->n])) break;
        sel->n++;
        int had_ws = 0;
        while (*s == ' ' || *s == '\t') { s++; had_ws = 1; }
        if (*s == '>') {
            sel->combs[sel->n - 1] = ZH_C_CHILD; s++;
            while (*s == ' ' || *s == '\t') s++;
        } else if (had_ws && *s) {
            sel->combs[sel->n - 1] = ZH_C_DESC;
        } else if (*s == 0) {
            break;
        } else {
            break;
        }
    }
    if (sel->n == 0) { zh_free_selector(sel); return NULL; }
    return sel;
}

/* Class attribute is space-separated tokens. */
static int zh_class_has(const char* class_attr, const char* tok) {
    if (!class_attr) return 0;
    size_t tlen = strlen(tok);
    const char* p = class_attr;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        const char* s = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if ((size_t)(p - s) == tlen && memcmp(s, tok, tlen) == 0) return 1;
    }
    return 0;
}

static const char* zh_get_attr(ZhNode* n, const char* name) {
    for (int i = 0; i < n->attr_count; i++)
        if (zh_strcaseeq(n->attr_keys[i], name)) return n->attr_vals[i];
    return NULL;
}

static int zh_match_simple(const ZhSimple* s, ZhNode* n) {
    if (!n->tag || n->tag[0] == '#') return 0;
    if (s->tag && strcmp(s->tag, "*") != 0 && strcmp(s->tag, n->tag) != 0) return 0;
    if (s->id) {
        const char* id = zh_get_attr(n, "id");
        if (!id || strcmp(id, s->id) != 0) return 0;
    }
    if (s->cls_n) {
        const char* cls = zh_get_attr(n, "class");
        for (int i = 0; i < s->cls_n; i++)
            if (!zh_class_has(cls, s->cls[i])) return 0;
    }
    for (int i = 0; i < s->attr_n; i++) {
        const char* v = zh_get_attr(n, s->attrs[i].key);
        if (!v) return 0;
        const char* want = s->attrs[i].val;
        size_t vl = v ? strlen(v) : 0, wl = want ? strlen(want) : 0;
        switch (s->attrs[i].op) {
            case ZH_AT_HAS:      break;
            case ZH_AT_EQ:       if (!want || strcmp(v, want) != 0) return 0; break;
            case ZH_AT_CONTAINS: if (!want || !strstr(v, want)) return 0; break;
            case ZH_AT_STARTS:   if (!want || vl < wl || memcmp(v, want, wl) != 0) return 0; break;
            case ZH_AT_ENDS:     if (!want || vl < wl || memcmp(v + vl - wl, want, wl) != 0) return 0; break;
        }
    }
    return 1;
}

/* Does `n` (and its ancestor chain in `path`) satisfy the selector? Match
 * right-to-left starting at the last simple-selector matching `n`. */
static int zh_match_chain(const ZhSelector* sel, ZhNode** path, int depth) {
    int i = sel->n - 1;
    int d = depth - 1;
    if (!zh_match_simple(&sel->parts[i], path[d])) return 0;
    while (i > 0) {
        ZhCombinator c = sel->combs[i - 1];
        i--;
        d--;
        if (d < 0) return 0;
        if (c == ZH_C_CHILD) {
            if (!zh_match_simple(&sel->parts[i], path[d])) return 0;
        } else {  /* descendant */
            int found = 0;
            while (d >= 0) {
                if (zh_match_simple(&sel->parts[i], path[d])) { found = 1; break; }
                d--;
            }
            if (!found) return 0;
        }
    }
    return 1;
}

/* Walk the tree DFS, calling `cb(matched_node, ctx)` for each match. */
static void zh_walk(ZhNode* root, const ZhSelector* sel,
                    ZhNode** path, int depth,
                    void (*cb)(ZhNode*, void*), void* ctx) {
    for (int i = 0; i < root->child_count; i++) {
        ZhNode* c = root->children[i];
        if (!c->tag || c->tag[0] == '#') continue;
        path[depth] = c;
        if (zh_match_chain(sel, path, depth + 1)) cb(c, ctx);
        zh_walk(c, sel, path, depth + 1, cb, ctx);
    }
}

/* ============================================================
 * z-side glue.
 * ============================================================ */

typedef struct { Value* arr; const char* src; } ZhCollect;

static void zh_collect_outer(ZhNode* n, void* ctx) {
    ZhCollect* cc = (ZhCollect*)ctx;
    int s = n->start, e = n->end;
    if (e <= s) return;
    char* outer = zh_strndup(cc->src + s, (size_t)(e - s));
    vlist_push(&cc->arr->as.list, v_str_take(outer));
}

/* Recursive helper for text extraction. */
static void zh_text_into(ZhNode* n, StrBuf* sb) {
    if (n->text) { sb_puts(sb, n->text); return; }
    for (int i = 0; i < n->child_count; i++) zh_text_into(n->children[i], sb);
}

static Value* b_html_query_common(int argc, Value** argv,
                                   const char* fname, int xml_strict) {
    if (argc != 2) z_raise("%s: expected (selector input)", fname);
    const char* sel_src = str_arg(argv[0], fname);
    const char* input   = str_arg(argv[1], fname);
    ZhNode* root = zh_parse(input, xml_strict);
    ZhSelector* sel = zh_parse_selector(sel_src);
    if (!sel) { zh_free(root); z_raise("%s: invalid selector '%s'", fname, sel_src); }
    Value* arr = v_array();
    ZhCollect cc = { arr, input };
    ZhNode* path[64];
    zh_walk(root, sel, path, 0, zh_collect_outer, &cc);
    zh_free_selector(sel);
    zh_free(root);
    return arr;
}

static Value* b_html_query(int argc, Value** argv, Env* e) {
    (void)e;
    return b_html_query_common(argc, argv, "html:query", 0);
}

static Value* b_xml_path_query(int argc, Value** argv, Env* e) {
    (void)e;
    EXPECT_ARGC("xml:query", 2);
    const char* path_s = str_arg(argv[0], "xml:query");
    const char* input  = str_arg(argv[1], "xml:query");
    /* Convert "/a/b/c" → "a > b > c" so we can reuse the CSS engine. */
    char css[1024];
    size_t off = 0;
    int first = 1;
    const char* p = path_s;
    while (*p == '/') p++;
    while (*p) {
        const char* s = p;
        while (*p && *p != '/') p++;
        if (p > s) {
            int n = (int)(p - s);
            int wrote = snprintf(css + off, sizeof(css) - off,
                                 "%s%.*s", first ? "" : " > ", n, s);
            if (wrote < 0) break;
            off += (size_t)wrote;
            first = 0;
        }
        while (*p == '/') p++;
    }
    Value* args[2] = { v_str(css), v_str(input) };
    Value* r = b_html_query_common(2, args, "xml:query", 1);
    return r;
}

static Value* b_html_text(int argc, Value** argv, Env* e) {
    (void)e;
    EXPECT_ARGC("html:text", 1);
    const char* input = str_arg(argv[0], "html:text");
    ZhNode* root = zh_parse(input, 0);
    StrBuf sb; sb_init(&sb);
    zh_text_into(root, &sb);
    if (!sb.data) { sb.data = (char*)calloc(1, 1); }
    zh_free(root);
    return v_str_take(sb.data);
}
static Value* b_xml_text(int argc, Value** argv, Env* e) {
    (void)e;
    EXPECT_ARGC("xml:text", 1);
    const char* input = str_arg(argv[0], "xml:text");
    ZhNode* root = zh_parse(input, 1);
    StrBuf sb; sb_init(&sb);
    zh_text_into(root, &sb);
    if (!sb.data) { sb.data = (char*)calloc(1, 1); }
    zh_free(root);
    return v_str_take(sb.data);
}

/* (html:attr name fragment) — attribute on the outermost element. */
static Value* b_html_attr_common(int argc, Value** argv, const char* fname, int xml_strict) {
    if (argc != 2) z_raise("%s: expected (name fragment)", fname);
    const char* name  = str_arg(argv[0], fname);
    const char* input = str_arg(argv[1], fname);
    ZhNode* root = zh_parse(input, xml_strict);
    Value* result = v_null();
    /* The first non-text child of #document is the outer tag. */
    for (int i = 0; i < root->child_count; i++) {
        ZhNode* n = root->children[i];
        if (!n->tag || n->tag[0] == '#') continue;
        const char* v = zh_get_attr(n, name);
        if (v) result = v_str(v);
        break;
    }
    zh_free(root);
    return result;
}
static Value* b_html_attr(int argc, Value** argv, Env* e) {
    (void)e;
    return b_html_attr_common(argc, argv, "html:attr", 0);
}
static Value* b_xml_attr(int argc, Value** argv, Env* e) {
    (void)e;
    return b_html_attr_common(argc, argv, "xml:attr", 1);
}

static void install_html_builtins(Env* env) {
    env_define(env, "html:query", v_native(b_html_query));
    env_define(env, "html:text",  v_native(b_html_text));
    env_define(env, "html:attr",  v_native(b_html_attr));
    env_define(env, "xml:query",  v_native(b_xml_path_query));
    env_define(env, "xml:text",   v_native(b_xml_text));
    env_define(env, "xml:attr",   v_native(b_xml_attr));
}

#endif /* Z_HTML_H_INCLUDED */
