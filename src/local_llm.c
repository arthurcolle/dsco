/* local_llm.c — discovery for local OpenAI-compatible inference servers. */
#include "local_llm.h"
#include "json_util.h"

#include <ctype.h>
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

typedef struct {
    const char *name;
    const char *host;        /* scheme+host:port, no trailing slash */
    const char *models_path; /* listing endpoint */
    int ollama_style;        /* 1: /api/tags {"models":[{"name"}]}; 0: /v1/models {"data":[{"id"}]} */
} local_server_def_t;

static const local_server_def_t LOCAL_SERVERS[] = {
    {"ollama", "http://localhost:11434", "/api/tags", 1},
    {"lmstudio", "http://localhost:1234", "/v1/models", 0},
    {"mlx", "http://localhost:8181", "/v1/models", 0},
    {"vllm", "http://localhost:8000", "/v1/models", 0},
    {"llamacpp", "http://localhost:8080", "/v1/models", 0},
    {"jan", "http://localhost:1337", "/v1/models", 0},
    {"gpt4all", "http://localhost:4891", "/v1/models", 0},
    {"koboldcpp", "http://localhost:5001", "/v1/models", 0},
    {"textgen", "http://localhost:5000", "/v1/models", 0},
    {"tgi", "http://localhost:3000", "/v1/models", 0},
    {"sglang", "http://localhost:30000", "/v1/models", 0},
};
static const int LOCAL_SERVER_COUNT =
    (int)(sizeof(LOCAL_SERVERS) / sizeof(LOCAL_SERVERS[0]));

static const local_server_def_t *local_find(const char *server) {
    if (!server)
        return NULL;
    for (int i = 0; i < LOCAL_SERVER_COUNT; i++)
        if (strcasecmp(server, LOCAL_SERVERS[i].name) == 0)
            return &LOCAL_SERVERS[i];
    return NULL;
}

/* Resolve the configured host for a server, honoring <SERVER>_HOST overrides. */
static const char *local_host(const local_server_def_t *def) {
    char env[64];
    snprintf(env, sizeof(env), "%s_HOST", def->name);
    for (char *p = env; *p; p++)
        *p = (char)toupper((unsigned char)*p);
    const char *override = getenv(env);
    return (override && override[0]) ? override : def->host;
}

bool local_llm_base_url(const char *server, char *out, size_t out_len) {
    const local_server_def_t *def = local_find(server);
    if (!def)
        return false;
    snprintf(out, out_len, "%s/v1", local_host(def));
    return true;
}

/* ── tiny HTTP buffer ───────────────────────────────────────────────────── */
typedef struct {
    char *data;
    size_t len;
} ll_buf_t;

static size_t ll_write_cb(char *ptr, size_t size, size_t nmemb, void *ud) {
    size_t total = size * nmemb;
    ll_buf_t *b = (ll_buf_t *)ud;
    char *g = realloc(b->data, b->len + total + 1);
    if (!g)
        return 0;
    b->data = g;
    memcpy(b->data + b->len, ptr, total);
    b->len += total;
    b->data[b->len] = '\0';
    return total;
}

/* GET (or POST when body!=NULL) with a short timeout. Returns malloc'd body. */
static char *ll_http(const char *url, const char *post_body, long timeout_ms) {
    CURL *c = curl_easy_init();
    if (!c)
        return NULL;
    ll_buf_t b = {0};
    struct curl_slist *h = NULL;
    h = curl_slist_append(h, "Accept: application/json");
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, ll_write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &b);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT_MS, timeout_ms);
    curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, timeout_ms * 3);
    if (post_body) {
        h = curl_slist_append(h, "Content-Type: application/json");
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, post_body);
    }
    CURLcode r = curl_easy_perform(c);
    long code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    curl_slist_free_all(h);
    curl_easy_cleanup(c);
    if (r != CURLE_OK || code < 200 || code >= 300) {
        free(b.data);
        return NULL;
    }
    return b.data;
}

bool local_llm_server_up(const char *server) {
    const local_server_def_t *def = local_find(server);
    if (!def)
        return false;
    char url[256];
    snprintf(url, sizeof(url), "%s%s", local_host(def), def->models_path);
    char *body = ll_http(url, NULL, 600);
    bool up = body != NULL;
    free(body);
    return up;
}

int local_llm_probe_servers(local_server_t *out, int max) {
    int n = 0;
    for (int i = 0; i < LOCAL_SERVER_COUNT && n < max; i++) {
        const local_server_def_t *def = &LOCAL_SERVERS[i];
        char url[256];
        snprintf(url, sizeof(url), "%s%s", local_host(def), def->models_path);
        char *body = ll_http(url, NULL, 600);
        local_server_t *s = &out[n++];
        memset(s, 0, sizeof(*s));
        snprintf(s->server, sizeof(s->server), "%s", def->name);
        snprintf(s->base_url, sizeof(s->base_url), "%s/v1", local_host(def));
        s->up = body != NULL;
        if (body) {
            /* count models cheaply */
            const char *key = def->ollama_style ? "\"name\"" : "\"id\"";
            for (const char *p = body; (p = strstr(p, key)) != NULL; p++)
                s->model_count++;
            free(body);
        }
    }
    return n;
}

/* ── model listing ──────────────────────────────────────────────────────── */
typedef struct {
    local_model_t *out;
    int max;
    int count;
    const local_server_def_t *def;
} ll_list_ctx_t;

static void ll_collect_model(const char *element, void *ctx) {
    ll_list_ctx_t *lc = (ll_list_ctx_t *)ctx;
    if (lc->count >= lc->max)
        return;
    char *id = json_get_str(element, lc->def->ollama_style ? "name" : "id");
    if (!id || !id[0]) {
        free(id);
        return;
    }
    local_model_t *m = &lc->out[lc->count++];
    memset(m, 0, sizeof(*m));
    snprintf(m->server, sizeof(m->server), "%s", lc->def->name);
    snprintf(m->model, sizeof(m->model), "%s", id);
    snprintf(m->qualified, sizeof(m->qualified), "%s:%s", lc->def->name, id);
    m->context_window = 0; /* resolved lazily by local_llm_context_window */
    free(id);
}

int local_llm_list_models(local_model_t *out, int max) {
    int total = 0;
    for (int i = 0; i < LOCAL_SERVER_COUNT && total < max; i++) {
        const local_server_def_t *def = &LOCAL_SERVERS[i];
        char url[256];
        snprintf(url, sizeof(url), "%s%s", local_host(def), def->models_path);
        char *body = ll_http(url, NULL, 800);
        if (!body)
            continue;
        ll_list_ctx_t lc = {out, max, total, def};
        json_array_foreach(body, def->ollama_style ? "models" : "data", ll_collect_model, &lc);
        total = lc.count;
        free(body);
    }
    return total;
}

/* ── context window resolution ──────────────────────────────────────────── */
bool local_llm_is_local_ref(const char *name) {
    if (!name)
        return false;
    const char *colon = strchr(name, ':');
    if (!colon || colon == name)
        return false;
    size_t plen = (size_t)(colon - name);
    if (plen >= 16)
        return false;
    char pfx[16];
    memcpy(pfx, name, plen);
    pfx[plen] = '\0';
    if (strcmp(pfx, "local") == 0)
        return true;
    return local_find(pfx) != NULL;
}

/* Pull the "<arch>.context_length" value out of an Ollama /api/show model_info
 * object. The arch prefix varies (llama./qwen2./gemma3.), so scan for the
 * ".context_length" suffix. */
static int ll_ollama_ctx(const char *host, const char *model) {
    char url[256];
    snprintf(url, sizeof(url), "%s/api/show", host);
    char body[256];
    snprintf(body, sizeof(body), "{\"name\":\"%s\"}", model);
    char *resp = ll_http(url, body, 1500);
    if (!resp)
        return 0;
    int ctx = 0;
    const char *needle = "context_length";
    char *p = strstr(resp, needle);
    if (p) {
        p += strlen(needle);
        while (*p && (*p == '"' || *p == ':' || *p == ' '))
            p++;
        ctx = atoi(p);
    }
    free(resp);
    return ctx;
}

int local_llm_context_window(const char *server, const char *model) {
    const local_server_def_t *def = local_find(server);
    if (!def || !model || !model[0])
        return 0;
    if (def->ollama_style) {
        int ctx = ll_ollama_ctx(local_host(def), model);
        if (ctx > 0)
            return ctx;
    }
    /* LM Studio / MLX expose no portable context field; fall back to a
     * conservative default that covers most local models. */
    return 32768;
}
