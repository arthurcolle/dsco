/* context_fabric.c — unified content-addressable + semantic context store.
 *
 * Fronts the existing stores behind one addressing scheme (ctxkey):
 *   - VFS KV buckets  ctx_blob / ctx_meta / ctx_pin / ctx_scope  (storage)
 *   - vecstore "ctx_index"                                        (semantic search)
 *   - tools_embed_text()                                          (in-process embeddings)
 *   - sha256_hex()                                                (content addressing)
 *
 * See include/context_fabric.h for the model. This is the foundation slice:
 * put / get / search / scope / pin. The windowing invariant and the
 * meta-orchestrator compose over this layer.
 */

#include "context_fabric.h"

#include "crypto.h"
#include "json_util.h"
#include "tools.h"
#include "vecstore.h"
#include "vfs.h"

#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* Buckets */
#define CTX_BUCKET_BLOB  "ctx_blob"
#define CTX_BUCKET_META  "ctx_meta"
#define CTX_BUCKET_PIN   "ctx_pin"
#define CTX_BUCKET_SCOPE "ctx_scope"
#define CTX_VEC_COLL     "ctx_index"

#define CTX_EMBED_MAX_BYTES 8192  /* cap text sent to the embedder */
#define CTX_PREVIEW_BYTES   140
#define CTX_LEXICAL_SCAN_MAX 1024 /* bounded fallback scan when no embeddings */

struct ctx_broker {
    vfs_db_t       *vfs;
    vecstore_t     *vec;
    pthread_mutex_t mu;
    bool            owns_vfs;
};

/* ── ctxkey formatting / parsing ──────────────────────────────────────────*/

static const char *const k_kind_names[CTX_KIND_COUNT] = {
    "blob", "file", "msg", "tool", "mem", "plan", "run", "scope"};

const char *ctx_kind_name(ctx_kind_t kind) {
    if (kind < 0 || kind >= CTX_KIND_COUNT)
        return "blob";
    return k_kind_names[kind];
}

static bool ctx_kind_from_name(const char *s, size_t n, ctx_kind_t *out) {
    for (int i = 0; i < CTX_KIND_COUNT; i++) {
        if (strlen(k_kind_names[i]) == n && strncmp(s, k_kind_names[i], n) == 0) {
            *out = (ctx_kind_t)i;
            return true;
        }
    }
    return false;
}

int ctxkey_format(const ctxkey_t *key, char *out, size_t cap) {
    if (!key || !out || cap == 0)
        return -1;
    int n;
    if (key->slice_kind == CTX_SLICE_BYTES)
        n = snprintf(out, cap, "ck:%s:%s#b:%ld-%ld", ctx_kind_name(key->kind), key->id,
                     key->slice_start, key->slice_end);
    else if (key->slice_kind == CTX_SLICE_LINES)
        n = snprintf(out, cap, "ck:%s:%s#l:%ld-%ld", ctx_kind_name(key->kind), key->id,
                     key->slice_start, key->slice_end);
    else
        n = snprintf(out, cap, "ck:%s:%s", ctx_kind_name(key->kind), key->id);
    if (n < 0 || (size_t)n >= cap)
        return -1;
    return n;
}

bool ctxkey_parse(const char *s, ctxkey_t *out) {
    if (!s || !out)
        return false;
    memset(out, 0, sizeof(*out));
    out->slice_kind = CTX_SLICE_NONE;
    if (strncmp(s, "ck:", 3) != 0)
        return false;
    s += 3;
    const char *colon = strchr(s, ':');
    if (!colon)
        return false;
    if (!ctx_kind_from_name(s, (size_t)(colon - s), &out->kind))
        return false;
    const char *id = colon + 1;
    const char *hash = strchr(id, '#');
    size_t id_len = hash ? (size_t)(hash - id) : strlen(id);
    if (id_len == 0 || id_len >= CTX_ID_LEN)
        return false;
    memcpy(out->id, id, id_len);
    out->id[id_len] = '\0';
    if (hash) {
        char kind_c = 0;
        long a = 0, b = -1;
        if (sscanf(hash + 1, "%c:%ld-%ld", &kind_c, &a, &b) >= 2) {
            out->slice_kind = (kind_c == 'l') ? CTX_SLICE_LINES : CTX_SLICE_BYTES;
            out->slice_start = a;
            out->slice_end = b;
        }
    }
    return true;
}

/* ── path helpers ─────────────────────────────────────────────────────────*/

static void ctx_mkdir_p(const char *path) {
    char tmp[1024];
    size_t len = strlen(path);
    if (len == 0 || len >= sizeof(tmp))
        return;
    memcpy(tmp, path, len + 1);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0700);
            *p = '/';
        }
    }
}

static void ctx_default_db_path(char *out, size_t cap) {
    const char *override = getenv("DSCO_CONTEXT_DB");
    if (override && *override) {
        snprintf(out, cap, "%s", override);
        return;
    }
    const char *home = getenv("HOME");
    if (!home || !*home)
        home = "/tmp";
    snprintf(out, cap, "%s/.dsco/context/fabric.db", home);
}

/* ── content classification / preview ─────────────────────────────────────*/

static bool ctx_is_textual(const void *bytes, size_t n) {
    const unsigned char *p = bytes;
    size_t sample = n < 512 ? n : 512;
    if (sample == 0)
        return true;
    size_t printable = 0;
    for (size_t i = 0; i < sample; i++) {
        if (p[i] == 0)
            return false; /* NUL → binary */
        if (p[i] == '\n' || p[i] == '\t' || p[i] == '\r' || (p[i] >= 0x20 && p[i] < 0x7f) ||
            p[i] >= 0x80)
            printable++;
    }
    return printable * 100 >= sample * 85; /* ≥85% printable */
}

/* Copy a sanitized single-line preview (control chars → space) into `out`. */
static void ctx_make_preview(const void *bytes, size_t n, char *out, size_t cap) {
    const unsigned char *p = bytes;
    size_t lim = n < CTX_PREVIEW_BYTES ? n : CTX_PREVIEW_BYTES;
    if (lim >= cap)
        lim = cap - 1;
    size_t j = 0;
    for (size_t i = 0; i < lim; i++) {
        unsigned char c = p[i];
        out[j++] = (c < 0x20 || c == 0x7f) ? ' ' : (char)c;
    }
    out[j] = '\0';
}

/* ── metadata (JSON in ctx_meta + vecstore metadata) ──────────────────────*/

static char *ctx_build_meta_json(ctx_kind_t kind, const char *source, const char *tags,
                                 double importance, size_t size, const char *preview) {
    jbuf_t b;
    jbuf_init(&b, 256);
    jbuf_appendf(&b, "{\"kind\":%d,\"size\":%zu,\"importance\":%.3f,\"source\":", (int)kind, size,
                 importance);
    jbuf_append_json_str(&b, source ? source : "");
    jbuf_append(&b, ",\"tags\":");
    jbuf_append_json_str(&b, tags ? tags : "");
    jbuf_append(&b, ",\"preview\":");
    jbuf_append_json_str(&b, preview ? preview : "");
    jbuf_append(&b, "}");
    return b.data; /* caller frees */
}

/* ── broker lifecycle ─────────────────────────────────────────────────────*/

ctx_broker_t *ctx_broker_open(const char *db_path) {
    char pathbuf[1024];
    if (!db_path) {
        ctx_default_db_path(pathbuf, sizeof(pathbuf));
        db_path = pathbuf;
    }
    /* ensure parent dir exists */
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s", db_path);
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        ctx_mkdir_p(dir);
    }
    vfs_db_t *vfs = vfs_open(db_path);
    if (!vfs)
        return NULL;
    ctx_broker_t *b = calloc(1, sizeof(*b));
    if (!b) {
        vfs_close(vfs);
        return NULL;
    }
    b->vfs = vfs;
    b->owns_vfs = true;
    b->vec = vecstore_open(vfs, CTX_VEC_COLL); /* may be NULL → search degrades to lexical */
    pthread_mutex_init(&b->mu, NULL);
    return b;
}

void ctx_broker_close(ctx_broker_t *b) {
    if (!b)
        return;
    if (b->vec)
        vecstore_close(b->vec);
    if (b->owns_vfs && b->vfs)
        vfs_close(b->vfs);
    pthread_mutex_destroy(&b->mu);
    free(b);
}

ctx_broker_t *ctx_broker_open_vfs(vfs_db_t *vfs) {
    if (!vfs)
        return NULL;
    ctx_broker_t *b = calloc(1, sizeof(*b));
    if (!b)
        return NULL;
    b->vfs = vfs;
    b->owns_vfs = false; /* shared handle — do not close on broker close */
    b->vec = vecstore_open(vfs, CTX_VEC_COLL);
    pthread_mutex_init(&b->mu, NULL);
    return b;
}

static pthread_mutex_t g_default_mu = PTHREAD_MUTEX_INITIALIZER;
static ctx_broker_t   *g_default_broker = NULL;
static vfs_db_t       *g_shared_vfs = NULL;

void ctx_broker_set_shared_vfs(vfs_db_t *vfs) {
    pthread_mutex_lock(&g_default_mu);
    g_shared_vfs = vfs;
    pthread_mutex_unlock(&g_default_mu);
}

ctx_broker_t *ctx_broker_default(void) {
    pthread_mutex_lock(&g_default_mu);
    if (!g_default_broker) {
        /* Prefer the shared process-wide vfs so the fabric lives in the one
         * db (~/.dsco/vfs.db) beside every other store; fall back to a private
         * file only when no shared handle was injected (tests, tools run before
         * main wiring). */
        g_default_broker =
            g_shared_vfs ? ctx_broker_open_vfs(g_shared_vfs) : ctx_broker_open(NULL);
    }
    ctx_broker_t *b = g_default_broker;
    pthread_mutex_unlock(&g_default_mu);
    return b;
}

/* ── put ──────────────────────────────────────────────────────────────────*/

bool ctx_put(ctx_broker_t *b, const void *bytes, size_t n, const ctx_put_opts_t *opts,
             ctxkey_t *out_key, bool *out_deduped) {
    if (!b || !bytes || !out_key)
        return false;
    ctx_kind_t kind = opts ? opts->kind : CTX_KIND_BLOB;
    const char *source = opts ? opts->source : NULL;
    const char *tags = opts ? opts->tags : NULL;
    double importance = (opts && opts->importance > 0.0) ? opts->importance : 0.5;
    int embed_mode = opts ? opts->embed : -1;

    char cid[65];
    sha256_hex(bytes, n, cid);

    memset(out_key, 0, sizeof(*out_key));
    out_key->kind = kind;
    snprintf(out_key->id, sizeof(out_key->id), "%s", cid);
    out_key->slice_kind = CTX_SLICE_NONE;

    pthread_mutex_lock(&b->mu);

    /* dedup: content already present? */
    char *existing = vfs_kv_get_str(b->vfs, CTX_BUCKET_META, cid);
    if (existing) {
        free(existing);
        if (out_deduped)
            *out_deduped = true;
        pthread_mutex_unlock(&b->mu);
        return true;
    }
    if (out_deduped)
        *out_deduped = false;

    bool ok = vfs_kv_put(b->vfs, CTX_BUCKET_BLOB, cid, bytes, n);
    if (ok) {
        char preview[CTX_PREVIEW_BYTES + 8];
        ctx_make_preview(bytes, n, preview, sizeof(preview));
        char *meta = ctx_build_meta_json(kind, source, tags, importance, n, preview);
        if (meta) {
            vfs_kv_put_str(b->vfs, CTX_BUCKET_META, cid, meta);
            /* semantic index: embed textual content unless disabled */
            bool want_embed = (embed_mode == 1) || (embed_mode != 0 && ctx_is_textual(bytes, n));
            if (want_embed && b->vec) {
                size_t elen = n < CTX_EMBED_MAX_BYTES ? n : CTX_EMBED_MAX_BYTES;
                char *etext = malloc(elen + 1);
                if (etext) {
                    memcpy(etext, bytes, elen);
                    etext[elen] = '\0';
                    int dim = 0;
                    float *vec = tools_embed_text(etext, &dim);
                    if (vec && dim > 0)
                        vecstore_insert(b->vec, cid, vec, dim, meta);
                    free(vec);
                    free(etext);
                }
            }
            free(meta);
        }
    }
    pthread_mutex_unlock(&b->mu);
    return ok;
}

/* ── get (with slice) ─────────────────────────────────────────────────────*/

static char *ctx_apply_slice(char *buf, size_t len, const ctxkey_t *key, size_t *out_len) {
    if (key->slice_kind == CTX_SLICE_NONE) {
        *out_len = len;
        return buf;
    }
    if (key->slice_kind == CTX_SLICE_BYTES) {
        long start = key->slice_start < 0 ? 0 : key->slice_start;
        long end = (key->slice_end < 0 || (size_t)key->slice_end > len) ? (long)len : key->slice_end;
        if (start > (long)len)
            start = (long)len;
        if (end < start)
            end = start;
        size_t span = (size_t)(end - start);
        char *slice = malloc(span + 1);
        if (!slice) {
            *out_len = 0;
            return buf; /* fall back to whole on OOM */
        }
        memcpy(slice, buf + start, span);
        slice[span] = '\0';
        free(buf);
        *out_len = span;
        return slice;
    }
    /* CTX_SLICE_LINES — 1-based inclusive */
    long want_start = key->slice_start < 1 ? 1 : key->slice_start;
    long want_end = key->slice_end; /* <0 means to end */
    long line = 1;
    size_t i = 0, span_start = 0, span_end = len;
    bool found_start = (want_start == 1);
    if (found_start)
        span_start = 0;
    for (i = 0; i < len; i++) {
        if (buf[i] == '\n') {
            line++;
            if (!found_start && line == want_start) {
                span_start = i + 1;
                found_start = true;
            }
            if (want_end >= 0 && line == want_end + 1) {
                span_end = i + 1;
                break;
            }
        }
    }
    if (!found_start) {
        char *empty = calloc(1, 1);
        free(buf);
        *out_len = 0;
        return empty;
    }
    size_t span = span_end - span_start;
    char *slice = malloc(span + 1);
    if (!slice) {
        *out_len = len;
        return buf;
    }
    memcpy(slice, buf + span_start, span);
    slice[span] = '\0';
    free(buf);
    *out_len = span;
    return slice;
}

char *ctx_get(ctx_broker_t *b, const ctxkey_t *key, size_t *out_len) {
    if (!b || !key)
        return NULL;
    size_t len = 0;
    pthread_mutex_lock(&b->mu);
    char *buf = vfs_kv_get(b->vfs, CTX_BUCKET_BLOB, key->id, &len);
    pthread_mutex_unlock(&b->mu);
    if (!buf)
        return NULL;
    /* ensure NUL-terminated for text callers (vfs returns raw bytes) */
    char *nt = realloc(buf, len + 1);
    if (!nt) {
        free(buf);
        return NULL;
    }
    nt[len] = '\0';
    size_t final = 0;
    char *result = ctx_apply_slice(nt, len, key, &final);
    if (out_len)
        *out_len = final;
    return result;
}

/* ── search ───────────────────────────────────────────────────────────────*/

static void ctx_hit_from_meta(ctx_search_hit_t *hit, const char *cid, const char *meta, float score) {
    memset(hit, 0, sizeof(*hit));
    hit->key.kind = CTX_KIND_BLOB;
    snprintf(hit->key.id, sizeof(hit->key.id), "%s", cid);
    hit->key.slice_kind = CTX_SLICE_NONE;
    hit->score = score;
    if (meta) {
        hit->key.kind = (ctx_kind_t)json_get_int(meta, "kind", CTX_KIND_BLOB);
        hit->size = (size_t)json_get_i64(meta, "size", 0);
        char *src = json_get_str(meta, "source");
        char *prev = json_get_str(meta, "preview");
        if (src) {
            snprintf(hit->source, sizeof(hit->source), "%s", src);
            free(src);
        }
        if (prev) {
            snprintf(hit->preview, sizeof(hit->preview), "%s", prev);
            free(prev);
        }
    }
}

/* Case-insensitive substring test with no intermediate buffer, so neither the
 * needle nor the haystack can be silently truncated by a fixed-size copy. */
static bool ctx_ci_contains(const char *hay, const char *needle) {
    if (!needle || !*needle)
        return true;
    if (!hay)
        return false;
    for (const char *h = hay; *h; h++) {
        const char *a = h, *n = needle;
        while (*a && *n && tolower((unsigned char)*a) == tolower((unsigned char)*n)) {
            a++;
            n++;
        }
        if (!*n)
            return true;
    }
    return false;
}

static int ctx_search_lexical(ctx_broker_t *b, const char *query, int kind_filter, int max,
                              ctx_search_hit_t *out) {
    int kcount = 0;
    char **keys = vfs_kv_keys(b->vfs, CTX_BUCKET_META, &kcount);
    if (!keys)
        return 0;

    int found = 0;
    int scanned = 0;
    for (int i = 0; i < kcount && found < max && scanned < CTX_LEXICAL_SCAN_MAX; i++) {
        scanned++;
        char *meta = vfs_kv_get_str(b->vfs, CTX_BUCKET_META, keys[i]);
        if (!meta)
            continue;
        if (kind_filter >= 0 && json_get_int(meta, "kind", 0) != kind_filter) {
            free(meta);
            continue;
        }
        /* Match the full query against each metadata field independently — no
         * fixed-size concatenation, so neither query nor field is truncated. */
        char *src = json_get_str(meta, "source");
        char *tags = json_get_str(meta, "tags");
        char *prev = json_get_str(meta, "preview");
        bool hit = ctx_ci_contains(src, query) || ctx_ci_contains(tags, query) ||
                   ctx_ci_contains(prev, query);
        free(src);
        free(tags);
        free(prev);
        if (hit) {
            ctx_hit_from_meta(&out[found], keys[i], meta, 0.5f);
            found++;
        }
        free(meta);
    }
    for (int i = 0; i < kcount; i++)
        free(keys[i]);
    free(keys);
    return found;
}

int ctx_search(ctx_broker_t *b, const char *query, int kind_filter, int max, ctx_search_hit_t *out) {
    if (!b || !out || max <= 0)
        return 0;
    if (max > 64)
        max = 64;

    pthread_mutex_lock(&b->mu);

    /* Try semantic search first. */
    int dim = 0;
    float *qvec = (b->vec && query && *query) ? tools_embed_text(query, &dim) : NULL;
    if (qvec && dim > 0 && b->vec) {
        int cap = max * 3; /* over-fetch to allow kind filtering + dedup */
        if (cap > 192)
            cap = 192;
        vecstore_result_t *res = calloc((size_t)cap, sizeof(*res));
        int n = res ? vecstore_query(b->vec, qvec, dim, res, cap) : 0;
        free(qvec);
        int found = 0;
        for (int i = 0; i < n && found < max; i++) {
            if (!res[i].id)
                continue;
            char *meta = vfs_kv_get_str(b->vfs, CTX_BUCKET_META, res[i].id);
            if (kind_filter >= 0 && meta && json_get_int(meta, "kind", 0) != kind_filter) {
                free(meta);
                continue;
            }
            ctx_hit_from_meta(&out[found], res[i].id, meta, res[i].score);
            found++;
            free(meta);
        }
        if (res)
            vecstore_result_free(res, n);
        free(res);
        pthread_mutex_unlock(&b->mu);
        if (found > 0)
            return found;
        /* semantic returned nothing — fall through to lexical */
    } else {
        free(qvec);
    }

    int found = ctx_search_lexical(b, query, kind_filter, max, out);
    pthread_mutex_unlock(&b->mu);
    return found;
}

/* ── pin ──────────────────────────────────────────────────────────────────*/

bool ctx_pin(ctx_broker_t *b, const ctxkey_t *key) {
    if (!b || !key)
        return false;
    char ks[CTX_KEY_STR_MAX];
    if (ctxkey_format(key, ks, sizeof(ks)) < 0)
        return false;
    pthread_mutex_lock(&b->mu);
    bool ok = vfs_kv_put_str(b->vfs, CTX_BUCKET_PIN, ks, "1");
    pthread_mutex_unlock(&b->mu);
    return ok;
}

bool ctx_unpin(ctx_broker_t *b, const ctxkey_t *key) {
    if (!b || !key)
        return false;
    char ks[CTX_KEY_STR_MAX];
    if (ctxkey_format(key, ks, sizeof(ks)) < 0)
        return false;
    pthread_mutex_lock(&b->mu);
    bool ok = vfs_kv_delete(b->vfs, CTX_BUCKET_PIN, ks);
    pthread_mutex_unlock(&b->mu);
    return ok;
}

int ctx_pinned_list(ctx_broker_t *b, ctxkey_t *out, int max) {
    if (!b || !out || max <= 0)
        return 0;
    pthread_mutex_lock(&b->mu);
    int kcount = 0;
    char **keys = vfs_kv_keys(b->vfs, CTX_BUCKET_PIN, &kcount);
    int found = 0;
    for (int i = 0; i < kcount && found < max; i++) {
        if (keys[i] && ctxkey_parse(keys[i], &out[found]))
            found++;
    }
    for (int i = 0; i < kcount; i++)
        free(keys[i]);
    free(keys);
    pthread_mutex_unlock(&b->mu);
    return found;
}

/* ── scope ──────────────────────────────────────────────────────────────── */

bool ctx_scope_create(ctx_broker_t *b, const ctxkey_t *keys, int n, ctxkey_t *out_scope) {
    if (!b || !keys || n <= 0 || !out_scope)
        return false;
    /* Build a JSON array of member key strings; scope id = sha256 of the array. */
    jbuf_t arr;
    jbuf_init(&arr, 256);
    jbuf_append(&arr, "[");
    for (int i = 0; i < n; i++) {
        char ks[CTX_KEY_STR_MAX];
        if (ctxkey_format(&keys[i], ks, sizeof(ks)) < 0) {
            jbuf_free(&arr);
            return false;
        }
        if (i)
            jbuf_append(&arr, ",");
        jbuf_append_json_str(&arr, ks);
    }
    jbuf_append(&arr, "]");
    if (!arr.data)
        return false;

    char sid[65];
    sha256_hex((const uint8_t *)arr.data, strlen(arr.data), sid);

    memset(out_scope, 0, sizeof(*out_scope));
    out_scope->kind = CTX_KIND_SCOPE;
    snprintf(out_scope->id, sizeof(out_scope->id), "%s", sid);
    out_scope->slice_kind = CTX_SLICE_NONE;

    pthread_mutex_lock(&b->mu);
    bool ok = vfs_kv_put_str(b->vfs, CTX_BUCKET_SCOPE, sid, arr.data);
    pthread_mutex_unlock(&b->mu);
    jbuf_free(&arr);
    return ok;
}

int ctx_scope_resolve(ctx_broker_t *b, const ctxkey_t *scope, ctxkey_t *out, int max) {
    if (!b || !scope || !out || max <= 0)
        return 0;
    pthread_mutex_lock(&b->mu);
    char *arr = vfs_kv_get_str(b->vfs, CTX_BUCKET_SCOPE, scope->id);
    pthread_mutex_unlock(&b->mu);
    if (!arr)
        return 0;
    /* Parse the JSON array of key strings. Members are simple quoted strings
     * with no embedded quotes (ctxkeys are [a-z0-9:#-]), so a linear scan is
     * sufficient and avoids a full JSON parse. */
    int found = 0;
    const char *p = arr;
    while (found < max && (p = strchr(p, '"')) != NULL) {
        p++;
        const char *end = strchr(p, '"');
        if (!end)
            break;
        char ks[CTX_KEY_STR_MAX];
        size_t klen = (size_t)(end - p);
        if (klen < sizeof(ks)) {
            memcpy(ks, p, klen);
            ks[klen] = '\0';
            if (ctxkey_parse(ks, &out[found]))
                found++;
        }
        p = end + 1;
    }
    free(arr);
    return found;
}

/* ── stats ──────────────────────────────────────────────────────────────── */

ctx_stat_t ctx_broker_stat(ctx_broker_t *b) {
    ctx_stat_t st;
    memset(&st, 0, sizeof(st));
    if (!b)
        return st;
    pthread_mutex_lock(&b->mu);
    int kcount = 0;
    char **keys = vfs_kv_keys(b->vfs, CTX_BUCKET_META, &kcount);
    st.blobs = kcount;
    for (int i = 0; i < kcount; i++) {
        char *meta = vfs_kv_get_str(b->vfs, CTX_BUCKET_META, keys[i]);
        if (meta) {
            st.bytes += json_get_i64(meta, "size", 0);
            free(meta);
        }
        free(keys[i]);
    }
    free(keys);
    int pcount = 0;
    char **pins = vfs_kv_keys(b->vfs, CTX_BUCKET_PIN, &pcount);
    st.pinned = pcount;
    for (int i = 0; i < pcount; i++)
        free(pins[i]);
    free(pins);
    int scount = 0;
    char **scopes = vfs_kv_keys(b->vfs, CTX_BUCKET_SCOPE, &scount);
    st.scopes = scount;
    for (int i = 0; i < scount; i++)
        free(scopes[i]);
    free(scopes);
    st.vectors = b->vec ? vecstore_count(b->vec) : 0;
    st.embeddings_available = (b->vec != NULL);
    pthread_mutex_unlock(&b->mu);
    return st;
}
