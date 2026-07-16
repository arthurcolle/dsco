/* Background OpenRouter model catalog — see include/openrouter_cache.h.
 *
 * Loads ~/.dsco/openrouter_models.json into memory at startup, then refreshes
 * from https://openrouter.ai/api/v1/models on a detached thread when stale.
 * The parsed catalog is published via an atomic pointer; model_lookup() (in
 * config.h) falls through to openrouter_cache_lookup() so any real slug
 * resolves with live context/pricing/reasoning metadata. The /models endpoint
 * is public, so this works before the user has configured any API key.
 *
 * Each published catalog carries a precomputed open-addressing hash index over
 * both the exact slug and a normalised (case/punctuation-insensitive) key, so
 * lookups are O(1) across the full ~340-model catalog instead of a linear scan,
 * and bare/aliased names ("gpt-4o", "Llama 4 Maverick") resolve too. */

#define _POSIX_C_SOURCE 200809L
#include <stdatomic.h>
#include "http_pool.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <curl/curl.h>

#include "config.h" /* model_info_t, model_normalize_key */
#include "openrouter_cache.h"
#include "json_util.h"

#define OR_MODELS_URL "https://openrouter.ai/api/v1/models"
#define OR_CACHE_TTL (60 * 60) /* refresh over network when older */

/* One indexed model. `info` is the registry-compatible view returned by
 * model_lookup(); the trailing fields carry the richer OpenRouter metadata. */
typedef struct {
    model_info_t info; /* info.alias == info.model_id == id (owned) */
    char *name;        /* display name (owned, may be NULL) */
    char *org;         /* provider prefix slice (owned) */
    char *canonical;   /* canonical_slug if distinct from id (owned/NULL) */
    char *norm;        /* normalised key for fuzzy match (owned) */
    long created;
    int multimodal;
    int tool_capable;
    double intelligence_index;
    double coding_index;
    double agentic_index;
} or_model_t;

/* Open-addressing hash index entry. kind: 0 = exact slug, 1 = normalised. */
typedef struct {
    const char *key;
    int idx;
    uint8_t kind;
} or_hentry_t;

typedef struct {
    or_model_t *models; /* owned; strings strdup'd */
    int count;
    or_hentry_t *buckets; /* owned; size nbuckets (power of two) */
    int nbuckets;
} or_catalog_t;

/* Published catalog. Refresh swaps in a new pointer and intentionally leaks
 * the old one: readers hold raw pointers into it with no refcount, and
 * refreshes happen at most ~once per TTL per process, so the leak is tiny. */
static _Atomic(or_catalog_t *) g_catalog = NULL;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;
/* Publish notification: wait_ready blocks here instead of sleep-polling. */
static pthread_mutex_t g_pub_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_pub_cv = PTHREAD_COND_INITIALIZER;

/* ── hash index ────────────────────────────────────────────────────────── */

static unsigned long fnv1a(const char *s) {
    unsigned long h = 1469598103934665603UL;
    for (; *s; s++) {
        h ^= (unsigned char)*s;
        h *= 1099511628211UL;
    }
    return h;
}

static int next_pow2(int n) {
    int p = 16;
    while (p < n)
        p <<= 1;
    return p;
}

static void index_insert(or_catalog_t *cat, const char *key, int idx, uint8_t kind) {
    if (!key || !*key)
        return;
    unsigned long mask = (unsigned long)cat->nbuckets - 1;
    unsigned long h = fnv1a(key) & mask;
    for (int probe = 0; probe < cat->nbuckets; probe++) {
        or_hentry_t *e = &cat->buckets[h];
        if (!e->key) {
            e->key = key;
            e->idx = idx;
            e->kind = kind;
            return;
        }
        /* keep first writer on collision (mirrors registry "first match wins") */
        if (e->kind == kind && strcmp(e->key, key) == 0)
            return;
        h = (h + 1) & mask;
    }
}

static int index_find(const or_catalog_t *cat, const char *key, uint8_t kind) {
    if (!cat->nbuckets || !key || !*key)
        return -1;
    unsigned long mask = (unsigned long)cat->nbuckets - 1;
    unsigned long h = fnv1a(key) & mask;
    for (int probe = 0; probe < cat->nbuckets; probe++) {
        const or_hentry_t *e = &cat->buckets[h];
        if (!e->key)
            return -1;
        if (e->kind == kind && strcmp(e->key, key) == 0)
            return e->idx;
        h = (h + 1) & mask;
    }
    return -1;
}

/* ── lookup (declared in config.h) ─────────────────────────────────────── */
const model_info_t *openrouter_cache_lookup(const char *slug) {
    if (!slug || !*slug)
        return NULL;
    or_catalog_t *cat = atomic_load_explicit(&g_catalog, memory_order_acquire);
    if (!cat)
        return NULL;

    /* Pass 1: exact slug (and canonical_slug) match. */
    int idx = index_find(cat, slug, 0);
    if (idx >= 0)
        return &cat->models[idx].info;

    /* Pass 2: normalised match — resolves bare/aliased names like "gpt-4o". */
    char want[256];
    model_normalize_key(slug, want, sizeof(want));
    if (want[0]) {
        idx = index_find(cat, want, 1);
        if (idx >= 0)
            return &cat->models[idx].info;
    }
    return NULL;
}

int openrouter_cache_count(void) {
    or_catalog_t *cat = atomic_load_explicit(&g_catalog, memory_order_acquire);
    return cat ? cat->count : 0;
}

int openrouter_cache_foreach(or_model_cb cb, void *ud) {
    or_catalog_t *cat = atomic_load_explicit(&g_catalog, memory_order_acquire);
    if (!cat || !cb)
        return 0;
    for (int i = 0; i < cat->count; i++) {
        const or_model_t *m = &cat->models[i];
        or_model_view_t v = {
            .id = m->info.model_id,
            .name = m->name,
            .org = m->org,
            .context_window = m->info.context_window,
            .max_output = m->info.max_output,
            .input_price = m->info.input_price,
            .output_price = m->info.output_price,
            .cache_read_price = m->info.cache_read_price,
            .cache_write_price = m->info.cache_write_price,
            .supports_thinking = m->info.supports_thinking,
            .multimodal = m->multimodal,
            .tool_capable = m->tool_capable,
            .intelligence_index = m->intelligence_index,
            .coding_index = m->coding_index,
            .agentic_index = m->agentic_index,
            .created = m->created,
        };
        cb(&v, ud);
    }
    return cat->count;
}

/* ── JSON → catalog ────────────────────────────────────────────────────── */

typedef struct {
    or_model_t *models;
    int count;
    int cap;
} build_ctx_t;

static char *org_of(const char *id) {
    const char *slash = strchr(id, '/');
    size_t n = slash ? (size_t)(slash - id) : strlen(id);
    char *o = malloc(n + 1);
    if (!o)
        return NULL;
    memcpy(o, id, n);
    o[n] = '\0';
    return o;
}

static void on_model(const char *elem, void *ctx) {
    build_ctx_t *b = ctx;

    char *id = json_get_str(elem, "id");
    if (!id || !id[0]) {
        free(id);
        return;
    }
    /* OpenRouter prefixes deprecated/hidden variants with '~' — skip them. */
    if (id[0] == '~') {
        free(id);
        return;
    }

    int ctx_len = json_get_int(elem, "context_length", 0);
    long created = (long)json_get_double(elem, "created", 0);

    int max_out = 0;
    char *tp = json_get_raw(elem, "top_provider");
    if (tp) {
        max_out = json_get_int(tp, "max_completion_tokens", 0);
        free(tp);
    }

    double in_p = 0, out_p = 0, cache_r = 0, cache_w = 0;
    char *pr = json_get_raw(elem, "pricing");
    if (pr) {
        char *s;
        s = json_get_str(pr, "prompt");
        if (s) {
            in_p = strtod(s, NULL) * 1e6;
            free(s);
        }
        s = json_get_str(pr, "completion");
        if (s) {
            out_p = strtod(s, NULL) * 1e6;
            free(s);
        }
        s = json_get_str(pr, "input_cache_read");
        if (s) {
            cache_r = strtod(s, NULL) * 1e6;
            free(s);
        }
        s = json_get_str(pr, "input_cache_write");
        if (s) {
            cache_w = strtod(s, NULL) * 1e6;
            free(s);
        }
        free(pr);
    }

    int thinking = 0;
    int tool_capable = 0;
    double intelligence_index = -1.0;
    double coding_index = -1.0;
    double agentic_index = -1.0;
    char *sp = json_get_raw(elem, "supported_parameters");
    if (sp) {
        if (strstr(sp, "\"reasoning\""))
            thinking = 1;
        if (strstr(sp, "\"tools\""))
            tool_capable = 1;
        free(sp);
    }

    /* Multimodal: any non-text input or output modality. */
    int multimodal = 0;
    char *arch = json_get_raw(elem, "architecture");
    if (arch) {
        char *mod = json_get_str(arch, "modality");
        if (mod) {
            /* e.g. "text+image->text" or "text->text+image" */
            const char *arrow = strstr(mod, "->");
            if (strstr(mod, "image") || strstr(mod, "audio") || strstr(mod, "video"))
                multimodal = 1;
            (void)arrow;
            free(mod);
        }
        free(arch);
    }

    char *benchmarks = json_get_raw(elem, "benchmarks");
    if (benchmarks) {
        char *aa = json_get_raw(benchmarks, "artificial_analysis");
        if (aa) {
            intelligence_index = json_get_double(aa, "intelligence_index", -1.0);
            coding_index = json_get_double(aa, "coding_index", -1.0);
            agentic_index = json_get_double(aa, "agentic_index", -1.0);
            free(aa);
        }
        free(benchmarks);
    }

    char *name = json_get_str(elem, "name");
    char *canonical = json_get_str(elem, "canonical_slug");
    if (canonical && strcmp(canonical, id) == 0) {
        free(canonical);
        canonical = NULL;
    }

    char norm[256];
    model_normalize_key(id, norm, sizeof(norm));

    if (b->count == b->cap) {
        int ncap = b->cap ? b->cap * 2 : 512;
        or_model_t *nm = realloc(b->models, (size_t)ncap * sizeof(*nm));
        if (!nm) {
            free(id);
            free(name);
            free(canonical);
            return;
        }
        b->models = nm;
        b->cap = ncap;
    }

    or_model_t *m = &b->models[b->count];
    m->info.alias = id; /* alias == model_id: the raw slug is the name */
    m->info.model_id = id;
    m->info.context_window = ctx_len > 0 ? ctx_len : 131072;
    m->info.max_output = max_out > 0 ? max_out : 16384;
    m->info.input_price = in_p;
    m->info.output_price = out_p;
    m->info.cache_read_price = cache_r;
    m->info.cache_write_price = cache_w;
    m->info.supports_thinking = thinking;
    m->name = name;
    m->org = org_of(id);
    m->canonical = canonical;
    m->norm = norm[0] ? strdup(norm) : NULL;
    m->created = created;
    m->multimodal = multimodal;
    m->tool_capable = tool_capable;
    m->intelligence_index = intelligence_index;
    m->coding_index = coding_index;
    m->agentic_index = agentic_index;
    b->count++;
}

/* Parse a full /models response body into an indexed catalog. NULL on failure. */
static or_catalog_t *catalog_from_json(const char *json) {
    if (!json)
        return NULL;
    build_ctx_t b = {0};
    int n = json_array_foreach(json, "data", on_model, &b);
    if (n <= 0 || b.count == 0) {
        free(b.models);
        return NULL;
    }

    or_catalog_t *cat = calloc(1, sizeof(*cat));
    if (!cat) {
        free(b.models);
        return NULL;
    }
    cat->models = b.models;
    cat->count = b.count;

    /* Build the hash index: up to ~3 keys per model (id, canonical, norm),
     * sized for a <0.5 load factor so probe chains stay short. */
    cat->nbuckets = next_pow2(b.count * 8);
    cat->buckets = calloc((size_t)cat->nbuckets, sizeof(*cat->buckets));
    if (cat->buckets) {
        for (int i = 0; i < cat->count; i++) {
            or_model_t *m = &cat->models[i];
            index_insert(cat, m->info.model_id, i, 0);
            if (m->canonical)
                index_insert(cat, m->canonical, i, 0);
            if (m->norm)
                index_insert(cat, m->norm, i, 1);
        }
    } else {
        cat->nbuckets = 0; /* lookup falls back to miss; foreach still works */
    }
    return cat;
}

static void publish(or_catalog_t *cat) {
    if (cat) {
        atomic_store_explicit(&g_catalog, cat, memory_order_release);
        pthread_mutex_lock(&g_pub_mu);
        pthread_cond_broadcast(&g_pub_cv);
        pthread_mutex_unlock(&g_pub_mu);
    }
}

/* ── disk cache ────────────────────────────────────────────────────────── */

static int cache_path(char *out, size_t n) {
    const char *home = getenv("HOME");
    if (!home || !*home)
        return -1;
    snprintf(out, n, "%s/.dsco", home);
    mkdir(out, 0700); /* ignore EEXIST */
    snprintf(out, n, "%s/.dsco/openrouter_models.json", home);
    return 0;
}

static char *read_file(const char *path, size_t *len_out) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz <= 0) {
        fclose(f);
        return NULL;
    }
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = '\0';
    if (len_out)
        *len_out = rd;
    return buf;
}

static void write_file(const char *path, const char *data, size_t len) {
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f)
        return;
    fwrite(data, 1, len, f);
    fclose(f);
    rename(tmp, path); /* atomic replace */
}

/* Returns true when the on-disk cache is missing or older than the TTL. */
static bool cache_is_stale(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0)
        return true;
    time_t now = time(NULL);
    return (now - st.st_mtime) > OR_CACHE_TTL;
}

/* ── network fetch ─────────────────────────────────────────────────────── */

typedef struct {
    char *data;
    size_t len;
} http_buf_t;

static size_t http_write_cb(char *ptr, size_t sz, size_t nm, void *ud) {
    size_t n = sz * nm;
    http_buf_t *b = ud;
    char *p = realloc(b->data, b->len + n + 1);
    if (!p)
        return 0;
    b->data = p;
    memcpy(b->data + b->len, ptr, n);
    b->len += n;
    b->data[b->len] = '\0';
    return n;
}

static char *http_get(const char *url, size_t *len_out) {
    CURL *c = curl_easy_init();
    dsco_http_pool_apply(c);
    if (!c)
        return NULL;
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
    if (r != CURLE_OK || code < 200 || code >= 300) {
        free(b.data);
        return NULL;
    }
    if (len_out)
        *len_out = b.len;
    return b.data;
}

/* ── load core (shared by background worker and synchronous entry) ─────── */

static int load_catalog(bool allow_network) {
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
    if (allow_network && (!have_path || cache_is_stale(path))) {
        size_t len = 0;
        char *body = http_get(OR_MODELS_URL, &len);
        if (body) {
            or_catalog_t *cat = catalog_from_json(body);
            if (cat) {
                publish(cat);
                if (have_path)
                    write_file(path, body, len);
            }
            free(body);
        }
    }
    return openrouter_cache_count();
}

static void *worker(void *arg) {
    (void)arg;
    load_catalog(true);
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

int openrouter_cache_load_sync(void) {
    return load_catalog(true);
}

int openrouter_cache_wait_ready(int timeout_ms) {
    /* Condvar wait on publish() — wakes the instant the catalog lands
     * instead of 25ms-granularity sleep-polling. */
    int n = openrouter_cache_count();
    if (n > 0 || timeout_ms <= 0)
        return n;
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += timeout_ms / 1000;
    deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&g_pub_mu);
    while ((n = openrouter_cache_count()) == 0) {
        if (pthread_cond_timedwait(&g_pub_cv, &g_pub_mu, &deadline) != 0)
            break;
    }
    pthread_mutex_unlock(&g_pub_mu);
    return openrouter_cache_count();
}

/* ──────────────────────────────────────────────────────────────────────────
 * Task-based routing intelligence
 *
 * Connects the live model catalog to routing decisions. The OpenRouter
 * meta-models (auto, fusion, pareto-code, owl-alpha, free) provide dynamic
 * routing without dsco hardcoding model choices; the catalog-driven helpers
 * select concrete models by capability + budget so the 3000x price spread
 * across tool-capable models becomes an automatic optimization rather than a
 * manual one.
 *
 * Env overrides (all optional, point at any slug):
 *   DSCO_ROUTE_GENERAL   default "openrouter/auto"
 *   DSCO_ROUTE_CODE      default "openrouter/pareto-code"
 *   DSCO_ROUTE_COMPLEX   default "openrouter/fusion"
 *   DSCO_ROUTE_SUBAGENT  default "openrouter/owl-alpha"  (free, 1M ctx, tools)
 *   DSCO_ROUTE_FREE      default "openrouter/free"
 * ────────────────────────────────────────────────────────────────────────── */

static const char *route_env_or(const char *env, const char *fallback) {
    const char *v = getenv(env);
    return (v && v[0]) ? v : fallback;
}

const char *dsco_route_by_task(dsco_task_type_t task) {
    switch (task) {
        case DSCO_TASK_GENERAL:
            return route_env_or("DSCO_ROUTE_GENERAL", "openrouter/auto");
        case DSCO_TASK_CODE:
            return route_env_or("DSCO_ROUTE_CODE", "openrouter/pareto-code");
        case DSCO_TASK_COMPLEX:
            return route_env_or("DSCO_ROUTE_COMPLEX", "openrouter/fusion");
        case DSCO_TASK_SUBAGENT: {
            /* Prefer the free agentic meta-model; fall back to a free
             * tool-capable catalog model, then to the general router. */
            const char *e = getenv("DSCO_ROUTE_SUBAGENT");
            if (e && e[0])
                return e;
            return "openrouter/owl-alpha";
        }
        case DSCO_TASK_FREE: {
            const char *e = getenv("DSCO_ROUTE_FREE");
            if (e && e[0])
                return e;
            const char *fc = dsco_route_free_tool();
            return fc ? fc : "openrouter/free";
        }
        case DSCO_TASK_CHEAP:
            return dsco_route_cheapest_tool();
        case DSCO_TASK_LONG_CTX:
            return dsco_route_largest_ctx_tool();
        case DSCO_TASK_PREMIUM:
            return dsco_route_optimal(0.0, 0);
        default:
            return route_env_or("DSCO_ROUTE_GENERAL", "openrouter/auto");
    }
}

/* Shared scan state for catalog selection passes. */
typedef struct {
    const char *best_id;   /* points into catalog (process-lifetime stable) */
    double      best_price;
    int         best_ctx;
    double      budget;    /* max $/1M input (0 = no cap) */
    int         min_ctx;   /* min context window (0 = any) */
    const char *exclude;   /* slug to skip (failover) */
} route_scan_t;

/* Build the "openrouter/" prefixed form of a slug into a static ring buffer so
 * callers can use it directly as a model id. The catalog stores bare slugs
 * (e.g. "qwen/qwen3-coder") but dsco routes them through OpenRouter. */
static const char *route_prefix_or(const char *slug) {
    if (!slug || !slug[0])
        return NULL;
    if (strncmp(slug, "openrouter/", 11) == 0 || strncmp(slug, "openrouter:", 11) == 0)
        return slug;
    static char ring[4][192];
    static _Atomic unsigned ring_idx = 0;
    unsigned i = atomic_fetch_add_explicit(&ring_idx, 1, memory_order_relaxed) & 3u;
    snprintf(ring[i], sizeof(ring[i]), "openrouter/%s", slug);
    return ring[i];
}

/* cheapest tool-capable, honoring budget + min_ctx constraints */
static void scan_cheapest_tool(const or_model_view_t *m, void *ud) {
    route_scan_t *s = ud;
    if (!m->tool_capable)
        return;
    if (m->input_price <= 0.0) /* skip free (handled separately) and unpriced */
        return;
    if (s->budget > 0.0 && m->input_price > s->budget)
        return;
    if (s->min_ctx > 0 && m->context_window < s->min_ctx)
        return;
    if (s->exclude && strcmp(m->id, s->exclude) == 0)
        return;
    if (!s->best_id || m->input_price < s->best_price) {
        s->best_id = m->id;
        s->best_price = m->input_price;
        s->best_ctx = m->context_window;
    }
}

/* free tool-capable, prefer largest context. Excludes variable-priced
 * meta-routers (pricing == -1), which are not genuinely free. */
static void scan_free_tool(const or_model_view_t *m, void *ud) {
    route_scan_t *s = ud;
    if (!m->tool_capable)
        return;
    if (m->input_price != 0.0) /* skip both priced and variable (-1) */
        return;
    if (s->exclude && strcmp(m->id, s->exclude) == 0)
        return;
    if (!s->best_id || m->context_window > s->best_ctx) {
        s->best_id = m->id;
        s->best_ctx = m->context_window;
    }
}

/* largest-context tool-capable */
static void scan_largest_ctx(const or_model_view_t *m, void *ud) {
    route_scan_t *s = ud;
    if (!m->tool_capable)
        return;
    if (s->min_ctx > 0 && m->context_window < s->min_ctx)
        return;
    if (s->exclude && strcmp(m->id, s->exclude) == 0)
        return;
    if (!s->best_id || m->context_window > s->best_ctx) {
        s->best_id = m->id;
        s->best_ctx = m->context_window;
    }
}

/* highest-priced tool-capable (proxy for premium quality) */
static void scan_premium(const or_model_view_t *m, void *ud) {
    route_scan_t *s = ud;
    if (!m->tool_capable)
        return;
    if (m->input_price <= 0.0)
        return;
    if (s->exclude && strcmp(m->id, s->exclude) == 0)
        return;
    if (!s->best_id || m->input_price > s->best_price) {
        s->best_id = m->id;
        s->best_price = m->input_price;
    }
}

const char *dsco_route_cheapest_tool(void) {
    route_scan_t s = {0};
    openrouter_cache_foreach(scan_cheapest_tool, &s);
    return route_prefix_or(s.best_id);
}

const char *dsco_route_free_tool(void) {
    route_scan_t s = {0};
    openrouter_cache_foreach(scan_free_tool, &s);
    return route_prefix_or(s.best_id);
}

const char *dsco_route_largest_ctx_tool(void) {
    route_scan_t s = {0};
    openrouter_cache_foreach(scan_largest_ctx, &s);
    return route_prefix_or(s.best_id);
}

const char *dsco_route_optimal(double budget_per_1m, int min_ctx) {
    route_scan_t s = {0};
    s.budget = budget_per_1m;
    s.min_ctx = min_ctx;
    if (budget_per_1m > 0.0) {
        /* With a budget cap, pick the cheapest model that still clears the
         * floor — leaves headroom and respects the cap. */
        openrouter_cache_foreach(scan_cheapest_tool, &s);
    } else {
        /* No budget => premium: most expensive tool-capable model as a
         * quality proxy. */
        openrouter_cache_foreach(scan_premium, &s);
    }
    return route_prefix_or(s.best_id);
}

/* Failover candidate accumulator: collect tool-capable models with at least
 * the failed model's context window, sorted by price ascending. */
typedef struct {
    char (*out)[128];
    int    max;
    int    count;
    int    min_ctx;          /* require >= this context window */
    const char *exclude;     /* the failed model */
    /* parallel price array for insertion sort */
    double prices[32];
} failover_acc_t;

static void scan_failover(const or_model_view_t *m, void *ud) {
    failover_acc_t *a = ud;
    if (!m->tool_capable)
        return;
    if (a->min_ctx > 0 && m->context_window < a->min_ctx)
        return;
    if (a->exclude && strcmp(m->id, a->exclude) == 0)
        return;
    /* Skip variable-priced meta-routers (pricing == -1): a failover chain
     * should be made of concrete, deterministically-priced models. Free
     * models (price == 0) are kept — they are valid zero-cost fallbacks. */
    if (m->input_price < 0.0)
        return;
    double price = m->input_price;

    /* Insertion sort into the bounded chain by price ascending. */
    int cap = a->max < 32 ? a->max : 32;
    int pos = a->count < cap ? a->count : cap;
    /* find insertion index */
    int idx = a->count;
    for (int i = 0; i < a->count && i < cap; i++) {
        if (price < a->prices[i]) {
            idx = i;
            break;
        }
    }
    if (idx >= cap)
        return; /* worse than everything we keep */
    /* shift down */
    int last = (a->count < cap) ? a->count : (cap - 1);
    for (int i = last; i > idx; i--) {
        a->prices[i] = a->prices[i - 1];
        snprintf(a->out[i], 128, "%s", a->out[i - 1]);
    }
    a->prices[idx] = price;
    /* store with openrouter/ prefix for direct use */
    if (strncmp(m->id, "openrouter/", 11) == 0)
        snprintf(a->out[idx], 128, "%s", m->id);
    else
        snprintf(a->out[idx], 128, "openrouter/%s", m->id);
    if (a->count < cap)
        a->count++;
    (void)pos;
}

int dsco_route_failover_dynamic(const char *failed_model, char out_models[][128],
                                int max_models) {
    if (!out_models || max_models <= 0)
        return 0;
    for (int i = 0; i < max_models; i++)
        out_models[i][0] = '\0';

    /* Look up the failed model's context window so we keep capability parity. */
    int min_ctx = 0;
    const char *bare = failed_model;
    if (bare && strncmp(bare, "openrouter/", 11) == 0)
        bare += 11;
    if (bare) {
        const model_info_t *mi = openrouter_cache_lookup(bare);
        if (mi && mi->context_window > 0) {
            /* Require at least half the failed model's context so we don't
             * silently downgrade long-context workloads too far. */
            min_ctx = mi->context_window / 2;
        }
    }

    failover_acc_t acc = {0};
    acc.out = out_models;
    acc.max = max_models;
    acc.min_ctx = min_ctx;
    acc.exclude = bare;
    openrouter_cache_foreach(scan_failover, &acc);
    return acc.count;
}
