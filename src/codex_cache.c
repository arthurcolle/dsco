/* Background Codex model catalog -- see include/codex_cache.h. */

#define _POSIX_C_SOURCE 200809L

#include "codex_cache.h"
#include "json_util.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

#define CODEX_CACHE_TTL (60 * 60)
#define CODEX_MODELS_CMD "codex debug models 2>/dev/null"

typedef struct {
    model_info_t info; /* alias == model_id == slug (owned) */
    char *display_name;
    char *default_reasoning_level;
    char *visibility;
    char *norm;
    int supported_in_api;
    int priority;
    int max_context_window;
} codex_model_t;

typedef struct {
    codex_model_t *models;
    int count;
} codex_catalog_t;

static _Atomic(codex_catalog_t *) g_catalog = NULL;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;

static const char *strip_openai_prefix(const char *model) {
    if (!model)
        return NULL;
    if (strncmp(model, "openai/", 7) == 0)
        return model + 7;
    if (strncmp(model, "chatgpt/", 8) == 0)
        return model + 8;
    return model;
}

static int cache_ttl(void) {
    const char *v = getenv("DSCO_MODEL_CACHE_TTL");
    if (v && v[0]) {
        int n = atoi(v);
        if (n >= 60)
            return n;
    }
    return CODEX_CACHE_TTL;
}

static int cache_path(char *out, size_t n) {
    const char *home = getenv("HOME");
    if (!home || !home[0])
        return -1;
    snprintf(out, n, "%s/.dsco", home);
    mkdir(out, 0700);
    snprintf(out, n, "%s/.dsco/codex_models.json", home);
    return 0;
}

static char *read_file(const char *path, size_t *len_out) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long sz = ftell(f);
    if (sz <= 0) {
        fclose(f);
        return NULL;
    }
    rewind(f);
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
    rename(tmp, path);
}

static bool cache_is_stale(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0)
        return true;
    time_t now = time(NULL);
    return (now - st.st_mtime) > cache_ttl();
}

typedef struct {
    codex_model_t *models;
    int count;
    int cap;
} build_ctx_t;

static void on_model(const char *elem, void *ctx) {
    build_ctx_t *b = ctx;
    char *slug = json_get_str(elem, "slug");
    if (!slug || !slug[0]) {
        free(slug);
        return;
    }

    if (b->count == b->cap) {
        int ncap = b->cap ? b->cap * 2 : 16;
        codex_model_t *nm = realloc(b->models, (size_t)ncap * sizeof(*nm));
        if (!nm) {
            free(slug);
            return;
        }
        b->models = nm;
        b->cap = ncap;
    }

    codex_model_t *m = &b->models[b->count];
    memset(m, 0, sizeof(*m));
    m->info.alias = slug;
    m->info.model_id = slug;
    m->display_name = json_get_str(elem, "display_name");
    m->default_reasoning_level = json_get_str(elem, "default_reasoning_level");
    m->visibility = json_get_str(elem, "visibility");
    m->supported_in_api = json_get_bool(elem, "supported_in_api", false) ? 1 : 0;
    m->priority = json_get_int(elem, "priority", 0);

    int ctx_len = json_get_int(elem, "context_window", 0);
    int max_ctx = json_get_int(elem, "max_context_window", 0);
    if (ctx_len <= 0)
        ctx_len = max_ctx;
    m->max_context_window = max_ctx;
    m->info.context_window = ctx_len > 0 ? ctx_len : 272000;
    m->info.max_output = 32768;
    m->info.input_price = 0.0;
    m->info.output_price = 0.0;
    m->info.cache_read_price = 0.0;
    m->info.cache_write_price = 0.0;
    m->info.supports_thinking =
        (m->default_reasoning_level && m->default_reasoning_level[0]) ? 1 : 0;

    char norm[256];
    model_normalize_key(slug, norm, sizeof(norm));
    m->norm = norm[0] ? strdup(norm) : NULL;
    b->count++;
}

static codex_catalog_t *catalog_from_json(const char *json) {
    if (!json)
        return NULL;
    const char *start = strchr(json, '{');
    if (!start)
        return NULL;

    build_ctx_t b = {0};
    int n = json_array_foreach(start, "models", on_model, &b);
    if (n <= 0 || b.count == 0) {
        free(b.models);
        return NULL;
    }

    codex_catalog_t *cat = calloc(1, sizeof(*cat));
    if (!cat) {
        free(b.models);
        return NULL;
    }
    cat->models = b.models;
    cat->count = b.count;
    return cat;
}

static void publish(codex_catalog_t *cat) {
    if (cat)
        atomic_store_explicit(&g_catalog, cat, memory_order_release);
}

static char *read_codex_debug_models(size_t *len_out) {
    FILE *p = popen(CODEX_MODELS_CMD, "r");
    if (!p)
        return NULL;

    jbuf_t b;
    jbuf_init(&b, 64 * 1024);
    char chunk[4096];
    while (fgets(chunk, sizeof(chunk), p)) {
        jbuf_append(&b, chunk);
    }
    int rc = pclose(p);
    if (rc != 0 || b.len == 0) {
        jbuf_free(&b);
        return NULL;
    }

    const char *start = strchr(b.data, '{');
    if (!start) {
        jbuf_free(&b);
        return NULL;
    }
    if (start != b.data) {
        size_t keep = strlen(start);
        memmove(b.data, start, keep + 1);
        b.len = keep;
    }
    if (len_out)
        *len_out = b.len;
    return b.data;
}

static int load_catalog(bool allow_refresh) {
    char path[1024];
    bool have_path = (cache_path(path, sizeof(path)) == 0);

    if (have_path) {
        size_t len = 0;
        char *disk = read_file(path, &len);
        if (disk) {
            publish(catalog_from_json(disk));
            free(disk);
        }
    }

    if (allow_refresh && (!have_path || cache_is_stale(path))) {
        size_t len = 0;
        char *body = read_codex_debug_models(&len);
        if (body) {
            codex_catalog_t *cat = catalog_from_json(body);
            if (cat) {
                publish(cat);
                if (have_path)
                    write_file(path, body, len);
            }
            free(body);
        }
    }
    return codex_cache_count();
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

void codex_cache_init(void) {
    pthread_once(&g_once, start_once);
}

int codex_cache_load_sync(void) {
    return load_catalog(true);
}

int codex_cache_wait_ready(int timeout_ms) {
    const int step_ms = 25;
    int waited = 0;
    for (;;) {
        int n = codex_cache_count();
        if (n > 0)
            return n;
        if (waited >= timeout_ms)
            return 0;
        struct timespec ts = {step_ms / 1000, (long)(step_ms % 1000) * 1000000L};
        nanosleep(&ts, NULL);
        waited += step_ms;
    }
}

int codex_cache_count(void) {
    codex_catalog_t *cat = atomic_load_explicit(&g_catalog, memory_order_acquire);
    return cat ? cat->count : 0;
}

static bool model_matches(const codex_model_t *m, const char *name, const char *norm) {
    if (!m || !name || !name[0])
        return false;
    const char *bare = strip_openai_prefix(name);
    if (strcmp(bare, m->info.model_id) == 0 || strcmp(name, m->info.model_id) == 0)
        return true;
    if (norm && norm[0] && m->norm && strcmp(norm, m->norm) == 0)
        return true;
    return false;
}

static const codex_model_t *find_model(const char *name) {
    if (!name || !name[0])
        return NULL;
    codex_catalog_t *cat = atomic_load_explicit(&g_catalog, memory_order_acquire);
    if (!cat)
        return NULL;

    char norm[256];
    model_normalize_key(strip_openai_prefix(name), norm, sizeof(norm));
    for (int i = 0; i < cat->count; i++) {
        if (model_matches(&cat->models[i], name, norm))
            return &cat->models[i];
    }
    return NULL;
}

const model_info_t *codex_cache_lookup(const char *name) {
    const codex_model_t *m = find_model(name);
    return m ? &m->info : NULL;
}

static bool model_visible_by_default(const codex_model_t *m) {
    if (!m || !m->supported_in_api)
        return false;
    if (!m->visibility || !m->visibility[0])
        return true;
    return strcmp(m->visibility, "hide") != 0;
}

const char *codex_cache_default_model(void) {
    codex_catalog_t *cat = atomic_load_explicit(&g_catalog, memory_order_acquire);
    if (cat) {
        const codex_model_t *best = NULL;
        for (int i = 0; i < cat->count; i++) {
            const codex_model_t *m = &cat->models[i];
            if (!model_visible_by_default(m))
                continue;
            if (!best || best->priority <= 0 ||
                (m->priority > 0 && m->priority < best->priority))
                best = m;
        }
        if (best && best->info.model_id && best->info.model_id[0])
            return best->info.model_id;
    }
    return "gpt-5.5";
}

const char *codex_cache_default_effort(const char *model) {
    const codex_model_t *m = find_model(model ? model : codex_cache_default_model());
    if (m && m->default_reasoning_level && m->default_reasoning_level[0])
        return m->default_reasoning_level;
    return "medium";
}

bool codex_cache_model_supported(const char *model) {
    const char *bare = strip_openai_prefix(model);
    if (!bare || !bare[0])
        return false;

    const codex_model_t *m = find_model(bare);
    if (m)
        return m->supported_in_api != 0;

    /* Fallback for first-run startup before the detached catalog job has
     * published. These are Codex-catalog models in current CLI builds. */
    return strcmp(bare, "gpt-5.5") == 0 ||
           strcmp(bare, "gpt-5.4") == 0 ||
           strcmp(bare, "gpt-5.4-mini") == 0;
}

int codex_cache_foreach(codex_model_cb cb, void *ud) {
    codex_catalog_t *cat = atomic_load_explicit(&g_catalog, memory_order_acquire);
    if (!cat || !cb)
        return 0;
    for (int i = 0; i < cat->count; i++) {
        codex_model_t *m = &cat->models[i];
        codex_model_view_t v = {
            .slug = m->info.model_id,
            .display_name = m->display_name,
            .default_reasoning_level = m->default_reasoning_level,
            .visibility = m->visibility,
            .context_window = m->info.context_window,
            .max_context_window = m->max_context_window,
            .supported_in_api = m->supported_in_api,
            .supports_thinking = m->info.supports_thinking,
            .priority = m->priority,
        };
        cb(&v, ud);
    }
    return cat->count;
}
