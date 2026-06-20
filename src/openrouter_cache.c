/* Background OpenRouter model catalog — see include/openrouter_cache.h.
 *
 * Loads ~/.dsco/openrouter_models.json into memory at startup, then refreshes
 * from https://openrouter.ai/api/v1/models on a detached thread when stale.
 * The parsed catalog is published via an atomic pointer; model_lookup() (in
 * config.h) falls through to openrouter_cache_lookup() so any real slug
 * resolves with live context/pricing/reasoning metadata. The /models endpoint
 * is public, so this works before the user has configured any API key. */

#define _POSIX_C_SOURCE 200809L
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <curl/curl.h>

#include "config.h"          /* model_info_t */
#include "openrouter_cache.h"
#include "json_util.h"

#define OR_MODELS_URL   "https://openrouter.ai/api/v1/models"
#define OR_CACHE_TTL    (12 * 60 * 60)   /* refresh over network when older */

typedef struct {
    model_info_t *models;   /* owned; strings strdup'd, alias==model_id */
    int           count;
} or_catalog_t;

/* Published catalog. Refresh swaps in a new pointer and intentionally leaks
 * the old one: readers (model_lookup) hold raw pointers into it with no
 * refcount, and refreshes happen at most ~once per TTL per process, so the
 * leak is tiny and bounded. */
static _Atomic(or_catalog_t *) g_catalog = NULL;
static pthread_once_t          g_once    = PTHREAD_ONCE_INIT;

/* ── lookup (declared in config.h) ─────────────────────────────────────── */
const model_info_t *openrouter_cache_lookup(const char *slug) {
    if (!slug || !*slug) return NULL;
    or_catalog_t *cat = atomic_load_explicit(&g_catalog, memory_order_acquire);
    if (!cat) return NULL;
    for (int i = 0; i < cat->count; i++) {
        if (strcmp(slug, cat->models[i].model_id) == 0)
            return &cat->models[i];
    }
    return NULL;
}

int openrouter_cache_count(void) {
    or_catalog_t *cat = atomic_load_explicit(&g_catalog, memory_order_acquire);
    return cat ? cat->count : 0;
}

/* ── JSON → catalog ────────────────────────────────────────────────────── */

typedef struct {
    model_info_t *models;
    int           count;
    int           cap;
} build_ctx_t;

static void on_model(const char *elem, void *ctx) {
    build_ctx_t *b = ctx;

    char *id = json_get_str(elem, "id");
    if (!id || !id[0]) { free(id); return; }
    /* OpenRouter prefixes deprecated/hidden variants with '~' — skip them. */
    if (id[0] == '~') { free(id); return; }

    int ctx_len = json_get_int(elem, "context_length", 0);

    int max_out = 0;
    char *tp = json_get_raw(elem, "top_provider");
    if (tp) { max_out = json_get_int(tp, "max_completion_tokens", 0); free(tp); }

    double in_p = 0, out_p = 0, cache_r = 0;
    char *pr = json_get_raw(elem, "pricing");
    if (pr) {
        char *s;
        s = json_get_str(pr, "prompt");           if (s) { in_p    = strtod(s, NULL) * 1e6; free(s); }
        s = json_get_str(pr, "completion");        if (s) { out_p   = strtod(s, NULL) * 1e6; free(s); }
        s = json_get_str(pr, "input_cache_read");  if (s) { cache_r = strtod(s, NULL) * 1e6; free(s); }
        free(pr);
    }

    int thinking = 0;
    char *sp = json_get_raw(elem, "supported_parameters");
    if (sp) { if (strstr(sp, "\"reasoning\"")) thinking = 1; free(sp); }

    if (b->count == b->cap) {
        int ncap = b->cap ? b->cap * 2 : 256;
        model_info_t *nm = realloc(b->models, (size_t)ncap * sizeof(*nm));
        if (!nm) { free(id); return; }
        b->models = nm;
        b->cap = ncap;
    }

    model_info_t *m = &b->models[b->count];
    m->alias            = id;   /* alias == model_id: the raw slug is the name */
    m->model_id         = id;
    m->context_window   = ctx_len > 0 ? ctx_len : 131072;
    m->max_output       = max_out > 0 ? max_out : 16384;
    m->input_price      = in_p;
    m->output_price     = out_p;
    m->cache_read_price  = cache_r;
    m->cache_write_price = 0;
    m->supports_thinking = thinking;
    b->count++;
}

/* Parse a full /models response body into a catalog. NULL on failure. */
static or_catalog_t *catalog_from_json(const char *json) {
    if (!json) return NULL;
    build_ctx_t b = {0};
    int n = json_array_foreach(json, "data", on_model, &b);
    if (n <= 0 || b.count == 0) { free(b.models); return NULL; }

    or_catalog_t *cat = malloc(sizeof(*cat));
    if (!cat) { free(b.models); return NULL; }
    cat->models = b.models;
    cat->count  = b.count;
    return cat;
}

static void publish(or_catalog_t *cat) {
    if (cat) atomic_store_explicit(&g_catalog, cat, memory_order_release);
}

/* ── disk cache ────────────────────────────────────────────────────────── */

static int cache_path(char *out, size_t n) {
    const char *home = getenv("HOME");
    if (!home || !*home) return -1;
    snprintf(out, n, "%s/.dsco", home);
    mkdir(out, 0700);   /* ignore EEXIST */
    snprintf(out, n, "%s/.dsco/openrouter_models.json", home);
    return 0;
}

static char *read_file(const char *path, size_t *len_out) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz <= 0) { fclose(f); return NULL; }
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = '\0';
    if (len_out) *len_out = rd;
    return buf;
}

static void write_file(const char *path, const char *data, size_t len) {
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return;
    fwrite(data, 1, len, f);
    fclose(f);
    rename(tmp, path);   /* atomic replace */
}

/* Returns true when the on-disk cache is missing or older than the TTL. */
static bool cache_is_stale(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return true;
    time_t now = time(NULL);
    return (now - st.st_mtime) > OR_CACHE_TTL;
}

/* ── network fetch ─────────────────────────────────────────────────────── */

typedef struct { char *data; size_t len; } http_buf_t;

static size_t http_write_cb(char *ptr, size_t sz, size_t nm, void *ud) {
    size_t n = sz * nm;
    http_buf_t *b = ud;
    char *p = realloc(b->data, b->len + n + 1);
    if (!p) return 0;
    b->data = p;
    memcpy(b->data + b->len, ptr, n);
    b->len += n;
    b->data[b->len] = '\0';
    return n;
}

static char *http_get(const char *url, size_t *len_out) {
    CURL *c = curl_easy_init();
    if (!c) return NULL;
    http_buf_t b = {0};
    struct curl_slist *h = NULL;
    h = curl_slist_append(h, "Accept: application/json");
    h = curl_slist_append(h, "User-Agent: dsco/1.0 (openrouter-catalog)");
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, http_write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &b);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2TLS);
    CURLcode r = curl_easy_perform(c);
    long code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    curl_slist_free_all(h);
    curl_easy_cleanup(c);
    if (r != CURLE_OK || code < 200 || code >= 300) { free(b.data); return NULL; }
    if (len_out) *len_out = b.len;
    return b.data;
}

/* ── background worker ─────────────────────────────────────────────────── */

static void *worker(void *arg) {
    (void)arg;
    char path[1024];
    bool have_path = (cache_path(path, sizeof(path)) == 0);

    /* 1. Publish whatever is on disk immediately, so lookups work offline. */
    if (have_path) {
        size_t len = 0;
        char *disk = read_file(path, &len);
        if (disk) {
            publish(catalog_from_json(disk));
            free(disk);
        }
    }

    /* 2. Refresh over the network when stale (or when there was no disk copy). */
    if (!have_path || cache_is_stale(path)) {
        size_t len = 0;
        char *body = http_get(OR_MODELS_URL, &len);
        if (body) {
            or_catalog_t *cat = catalog_from_json(body);
            if (cat) {
                publish(cat);
                if (have_path) write_file(path, body, len);
            }
            free(body);
        }
    }
    return NULL;
}

static void start_once(void) {
    pthread_t tid;
    if (pthread_create(&tid, NULL, worker, NULL) == 0)
        pthread_detach(tid);
}

void openrouter_cache_init(void) {
    pthread_once(&g_once, start_once);
}
