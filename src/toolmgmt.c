/* Tool Management API client — discover, execute, and fan out across many
 * remote tools served by the dsco-autobot registry. See include/toolmgmt.h. */
#include "toolmgmt.h"
#include "http_pool.h"
#include "json_util.h"
#include "tools.h"
#include "config.h"
#include "crypto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <pthread.h>
#include <time.h>
#include <stdbool.h>
#include <curl/curl.h>

/* ── Configuration ─────────────────────────────────────────────────────── */

static char s_url_override[512];
static char s_tok_override[1024];

const char *toolmgmt_base_url(void) {
    if (s_url_override[0])
        return s_url_override;
    const char *e = getenv("TOOLS_API_URL");
    if (e && e[0])
        return e;
    e = getenv("TOOL_MANAGEMENT_API_URL");
    if (e && e[0])
        return e;
    return TOOLMGMT_API_URL_DEFAULT;
}

const char *toolmgmt_token(void) {
    if (s_tok_override[0])
        return s_tok_override;
    const char *e = getenv("TOOLS_API_TOKEN");
    if (e && e[0])
        return e;
    e = getenv("AUTH_TOKEN");
    if (e && e[0])
        return e;
    return NULL;
}

void toolmgmt_set_base_url(const char *url) {
    if (url)
        snprintf(s_url_override, sizeof(s_url_override), "%s", url);
}
void toolmgmt_set_token(const char *token) {
    if (token)
        snprintf(s_tok_override, sizeof(s_tok_override), "%s", token);
}

static long tm_timeout_secs(void) {
    const char *e = getenv("TOOLS_API_TIMEOUT");
    if (e && e[0]) {
        long v = atol(e);
        if (v > 0)
            return v;
    }
    return 60;
}

/* Number of automatic retries on transport error / 429 / 5xx (default 2). */
static int tm_max_retries(void) {
    const char *e = getenv("TOOLS_API_RETRIES");
    if (e && e[0]) {
        int v = atoi(e);
        if (v >= 0)
            return v;
    }
    return 2;
}

/* curl_global_init is not thread-safe; do it exactly once before any worker
 * thread spins up its own easy handle. */
static pthread_once_t s_curl_once = PTHREAD_ONCE_INIT;
static void tm_curl_global_init(void) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

/* ── HTTP ──────────────────────────────────────────────────────────────── */

typedef struct {
    char *data;
    size_t len, cap;
} tm_buf_t;

static size_t tm_write_cb(void *ptr, size_t size, size_t nmemb, void *ud) {
    size_t total = size * nmemb;
    tm_buf_t *b = (tm_buf_t *)ud;
    if (b->len + total + 1 > b->cap) {
        size_t ncap = (b->len + total + 1) * 2;
        char *nd = realloc(b->data, ncap);
        if (!nd)
            return 0;
        b->data = nd;
        b->cap = ncap;
    }
    memcpy(b->data + b->len, ptr, total);
    b->len += total;
    b->data[b->len] = '\0';
    return total;
}

static long tm_request_once(const char *method, const char *path, const char *body, char **out) {
    if (out)
        *out = NULL;
    pthread_once(&s_curl_once, tm_curl_global_init);

    CURL *c = curl_easy_init();
    dsco_http_pool_apply(c);
    if (!c)
        return -1;

    /* Join base + path, dropping any trailing slash on the base. */
    char url[1024];
    const char *base = toolmgmt_base_url();
    size_t bl = strlen(base);
    while (bl > 0 && base[bl - 1] == '/')
        bl--;
    snprintf(url, sizeof(url), "%.*s%s", (int)bl, base, path);

    tm_buf_t buf = {0};
    buf.data = malloc(4096);
    buf.cap = 4096;
    if (!buf.data) {
        curl_easy_cleanup(c);
        return -1;
    }
    buf.data[0] = '\0';

    struct curl_slist *hdrs = NULL;
    hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
    hdrs = curl_slist_append(hdrs, "Accept: application/json");
    char auth[1152];
    const char *tok = toolmgmt_token();
    if (tok && tok[0]) {
        snprintf(auth, sizeof(auth), "Authorization: Bearer %s", tok);
        hdrs = curl_slist_append(hdrs, auth);
    }

    if (getenv("TOOLS_API_DEBUG"))
        fprintf(stderr, "[tm] %s %s body=%s\n", method, url, body ? body : "(none)");

    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);
    if (strcmp(method, "POST") == 0) {
        curl_easy_setopt(c, CURLOPT_POST, 1L);
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, body ? body : "{}");
    } else if (strcmp(method, "GET") != 0) {
        curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, method);
        if (body)
            curl_easy_setopt(c, CURLOPT_POSTFIELDS, body);
    }
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, tm_write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, tm_timeout_secs());
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);

    CURLcode res = curl_easy_perform(c);
    long code = -1;
    if (res == CURLE_OK)
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);

    curl_slist_free_all(hdrs);
    curl_easy_cleanup(c);

    if (out)
        *out = buf.data;
    else
        free(buf.data);
    return code;
}

/* Retrying wrapper: transport errors, 429, and 5xx are retried with
 * exponential backoff + jitter (bounded by TOOLS_API_RETRIES). GET and POST
 * are both retried — the API's mutating calls accept idempotency keys, and the
 * read-only ones are naturally safe. */
long toolmgmt_request(const char *method, const char *path, const char *body, char **out) {
    int retries = tm_max_retries();
    long code = -1;
    char *resp = NULL;
    for (int attempt = 0;; attempt++) {
        free(resp);
        resp = NULL;
        code = tm_request_once(method, path, body, &resp);
        bool retryable = (code < 0 || code == 429 || (code >= 500 && code < 600));
        if (!retryable || attempt >= retries)
            break;
        /* backoff: 200ms, 400ms, 800ms … plus up to 100ms jitter */
        long base_ms = 200L << attempt;
        uint32_t jitter_word;
        long jitter = crypto_random_bytes((uint8_t *)&jitter_word, sizeof(jitter_word))
                          ? (long)(jitter_word % 100)
                          : 0;
        struct timespec ts = {(base_ms + jitter) / 1000, ((base_ms + jitter) % 1000) * 1000000L};
        nanosleep(&ts, NULL);
    }
    if (out)
        *out = resp;
    else
        free(resp);
    return code;
}

/* ── High-level operations ─────────────────────────────────────────────── */

static const char *tm_skip_ws(const char *p) {
    while (p && *p && isspace((unsigned char)*p))
        p++;
    return p;
}

static const char *tm_find_matching_array_end(const char *arr) {
    if (!arr || *arr != '[')
        return NULL;
    int depth = 0;
    bool in_str = false, esc = false;
    for (const char *p = arr; *p; p++) {
        if (in_str) {
            if (esc)
                esc = false;
            else if (*p == '\\')
                esc = true;
            else if (*p == '"')
                in_str = false;
            continue;
        }
        if (*p == '"')
            in_str = true;
        else if (*p == '[')
            depth++;
        else if (*p == ']') {
            depth--;
            if (depth == 0)
                return p;
        }
    }
    return NULL;
}

static bool tm_array_inner_nonempty(const char *start, const char *end) {
    for (const char *p = start; p && p < end; p++)
        if (!isspace((unsigned char)*p))
            return true;
    return false;
}

static bool tm_append_array_items(jbuf_t *out, const char *body, bool *first) {
    if (!out || !body || !first)
        return false;
    char *owned = json_get_raw(body, "tools");
    if (!owned)
        owned = json_get_raw(body, "items");
    if (!owned)
        owned = json_get_raw(body, "results");
    const char *scope = owned ? owned : body;
    const char *arr = tm_skip_ws(scope);
    if (!arr || *arr != '[') {
        free(owned);
        return false;
    }
    const char *end = tm_find_matching_array_end(arr);
    if (!end) {
        free(owned);
        return false;
    }
    const char *inner = arr + 1;
    if (!tm_array_inner_nonempty(inner, end)) {
        free(owned);
        return true;
    }
    if (!*first)
        jbuf_append(out, ",");
    jbuf_append_len(out, inner, (size_t)(end - inner));
    *first = false;
    free(owned);
    return true;
}

typedef struct {
    int count;
} tm_count_ctx_t;
static void tm_count_cb(const char *el, void *ctx) {
    (void)el;
    ((tm_count_ctx_t *)ctx)->count++;
}

static int tm_count_tools_in_body(const char *body) {
    if (!body)
        return 0;
    tm_count_ctx_t c = {0};
    if (json_array_foreach(body, "tools", tm_count_cb, &c) > 0)
        return c.count;
    if (json_array_foreach(body, "items", tm_count_cb, &c) > 0)
        return c.count;
    if (json_array_foreach(body, "results", tm_count_cb, &c) > 0)
        return c.count;
    const char *p = tm_skip_ws(body);
    if (p && *p == '[') {
        size_t n = strlen(body);
        char *wrapped = malloc(n + 32);
        if (!wrapped)
            return 0;
        snprintf(wrapped, n + 32, "{\"__tm_items__\":%s}", body);
        c.count = 0;
        json_array_foreach(wrapped, "__tm_items__", tm_count_cb, &c);
        free(wrapped);
    }
    return c.count;
}

/* Execute one tool; always returns the response body (even on error) so the
 * caller can inspect it, and reports the HTTP status via *status. */
static char *tm_exec(const char *tool, const char *args_json, int timeout_ms, long *status) {
    (void)timeout_ms; /* per-tool execute has no timeout field; honored via HTTP timeout */
    jbuf_t b;
    jbuf_init(&b, 512);
    jbuf_append(&b, "{\"inputs\":");
    jbuf_append(&b, (args_json && args_json[0]) ? args_json : "{}");
    jbuf_append(&b, "}");

    /* /api/v1/tools/{tool}/execute is the synchronous per-tool path. */
    char *esc = curl_easy_escape(NULL, tool, 0);
    char path[256];
    snprintf(path, sizeof(path), "/api/v1/tools/%s/execute", esc ? esc : tool);
    if (esc)
        curl_free(esc);

    char *out = NULL;
    long st = toolmgmt_request("POST", path, b.data, &out);
    jbuf_free(&b);
    if (status)
        *status = st;
    return out;
}

char *toolmgmt_execute(const char *tool, const char *args_json, int timeout_ms) {
    long st = -1;
    char *r = tm_exec(tool, args_json, timeout_ms, &st);
    if (st >= 200 && st < 300)
        return r;
    free(r);
    return NULL;
}

char *toolmgmt_list_tools_paginated(int offset, int limit) {
    if (offset < 0)
        offset = 0;
    if (limit <= 0)
        limit = 100;
    char path[160];
    snprintf(path, sizeof(path), "/api/v1/tools?offset=%d&limit=%d", offset, limit);
    char *out = NULL;
    long st = toolmgmt_request("GET", path, NULL, &out);
    if (st >= 200 && st < 300)
        return out;
    free(out);
    return NULL;
}

char *toolmgmt_list_tools_all(int page_limit) {
    if (page_limit <= 0)
        page_limit = 100;
    if (page_limit > 500)
        page_limit = 500;

    jbuf_t all;
    jbuf_init(&all, 65536);
    jbuf_append(&all, "[");
    bool first = true;
    int total = -1;
    int offset = 0;
    int guard_pages = 0;

    while (guard_pages++ < 1000) {
        char *page = toolmgmt_list_tools_paginated(offset, page_limit);
        if (!page)
            break;
        int count = json_get_int(page, "count", -1);
        if (total < 0)
            total = json_get_int(page, "total", -1);
        if (count < 0)
            count = tm_count_tools_in_body(page);
        bool has_more = json_get_bool(page, "has_more", false);
        tm_append_array_items(&all, page, &first);
        free(page);
        if (count <= 0)
            break;
        offset += count;
        if (total >= 0 && offset >= total)
            break;
        if (!has_more)
            break;
    }
    jbuf_append(&all, "]");

    if (offset > 0 || all.len > 2)
        return all.data;
    jbuf_free(&all);
    return NULL;
}

char *toolmgmt_list_tools(int limit) {
    int requested = limit > 0 ? limit : 1000;
    char *all = toolmgmt_list_tools_all(requested > 500 ? 500 : requested);
    if (all)
        return all;

    char path[128];
    snprintf(path, sizeof(path), "/api/v1/tools?limit=%d", requested);
    char *out = NULL;
    long st = toolmgmt_request("GET", path, NULL, &out);
    if (st >= 200 && st < 300)
        return out;
    free(out);
    return NULL;
}

char *toolmgmt_batch(const char *calls_json, bool parallel) {
    jbuf_t b;
    jbuf_init(&b, 512);
    jbuf_append(&b, "{\"calls\":");
    jbuf_append(&b, (calls_json && calls_json[0]) ? calls_json : "[]");
    jbuf_append(&b, ",\"parallel\":");
    jbuf_append(&b, parallel ? "true" : "false");
    jbuf_append(&b, "}");
    char *out = NULL;
    long st = toolmgmt_request("POST", "/api/v1/batch", b.data, &out);
    jbuf_free(&b);
    if (st >= 200 && st < 300)
        return out;
    free(out);
    return NULL;
}

char *toolmgmt_recommend(const char *intent, const char *query, int max_steps) {
    jbuf_t b;
    jbuf_init(&b, 512);
    jbuf_append(&b, "{\"intent\":");
    jbuf_append_json_str(&b, intent ? intent : "");
    jbuf_append(&b, ",\"query\":");
    jbuf_append_json_str(&b, query ? query : "");
    jbuf_append(&b, ",\"max_steps\":");
    jbuf_append_int(&b, max_steps > 0 ? max_steps : 5);
    jbuf_append(&b, ",\"available_data\":{}}");
    char *out = NULL;
    long st = toolmgmt_request("POST", "/api/v1/orchestration/recommend", b.data, &out);
    jbuf_free(&b);
    if (st >= 200 && st < 300)
        return out;
    free(out);
    return NULL;
}

/* ── Parallel fan-out ──────────────────────────────────────────────────── */

typedef struct {
    tm_call_t *calls;
    int n;
    volatile int next;
} tm_pool_t;

static void *tm_worker(void *arg) {
    tm_pool_t *p = (tm_pool_t *)arg;
    for (;;) {
        int i = __atomic_fetch_add(&p->next, 1, __ATOMIC_SEQ_CST);
        if (i >= p->n)
            break;
        tm_call_t *c = &p->calls[i];
        c->result = tm_exec(c->tool, c->args_json, c->timeout_ms, &c->status);
    }
    return NULL;
}

int toolmgmt_parallel(tm_call_t *calls, int n, int max_concurrency) {
    if (!calls || n <= 0)
        return 0;
    if (max_concurrency <= 0)
        max_concurrency = 8;
    if (max_concurrency > n)
        max_concurrency = n;

    pthread_once(&s_curl_once, tm_curl_global_init);

    tm_pool_t pool = {calls, n, 0};
    pthread_t *th = calloc((size_t)max_concurrency, sizeof(pthread_t));
    if (!th) { /* fall back to serial */
        tm_worker(&pool);
    } else {
        int spawned = 0;
        for (int i = 0; i < max_concurrency; i++)
            if (pthread_create(&th[i], NULL, tm_worker, &pool) == 0)
                spawned++;
        if (spawned == 0)
            tm_worker(&pool); /* none spawned: run inline */
        for (int i = 0; i < spawned; i++)
            pthread_join(th[i], NULL);
        free(th);
    }

    int ok = 0;
    for (int i = 0; i < n; i++)
        if (calls[i].status >= 200 && calls[i].status < 300)
            ok++;
    return ok;
}

/* ── Dynamic tool registration ─────────────────────────────────────────── */

static char *tm_external_cb(const char *name, const char *input_json, void *ctx) {
    const char *remote_tool = (ctx && ((const char *)ctx)[0]) ? (const char *)ctx : name;
    long st = -1;
    char *body = tm_exec(remote_tool, input_json, 0, &st);
    if (!body)
        return safe_strdup("{\"error\":\"tool management request failed\"}");
    return body; /* caller (tool dispatch) frees */
}

static unsigned long tm_fnv1a(const char *s) {
    unsigned long h = 1469598103934665603UL;
    for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; p++) {
        h ^= (unsigned long)*p;
        h *= 1099511628211UL;
    }
    return h;
}

static void tm_make_dsco_name(const char *tool_id, const char *name, char *out, size_t out_len) {
    if (!out || out_len == 0)
        return;
    out[0] = '\0';
    const char *src = (tool_id && tool_id[0]) ? tool_id : name;
    if (!src || !src[0])
        src = "remote_tool";

    char raw[256];
    size_t pos = 0;
    int n = snprintf(raw, sizeof(raw), "tm__");
    if (n < 0)
        return;
    pos = (size_t)n;
    bool last_us = false;
    for (const char *p = src; *p && pos + 1 < sizeof(raw); p++) {
        unsigned char c = (unsigned char)*p;
        char ch = ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                   (c >= '0' && c <= '9'))
                      ? (char)c
                      : '_';
        if (ch == '_' && last_us)
            continue;
        raw[pos++] = ch;
        last_us = (ch == '_');
    }
    while (pos > 4 && raw[pos - 1] == '_')
        pos--;
    raw[pos] = '\0';

    /* Anthropic/OpenAI-compatible tool names must remain short. Preserve a
     * readable prefix and add a stable hash suffix to avoid collisions. */
    const size_t api_cap = 64;
    if (strlen(raw) < api_cap) {
        snprintf(out, out_len, "%s", raw);
        return;
    }
    unsigned long h = tm_fnv1a(src);
    char suffix[24];
    snprintf(suffix, sizeof(suffix), "_%08lx", h & 0xffffffffUL);
    size_t keep = api_cap - strlen(suffix) - 1;
    if (keep >= out_len)
        keep = out_len > strlen(suffix) + 1 ? out_len - strlen(suffix) - 1 : 0;
    snprintf(out, out_len, "%.*s%s", (int)keep, raw, suffix);
}

typedef struct {
    int count;
} tm_reg_ctx_t;

/* Builds a JSON-Schema object from the registry's `inputs` array. Each input
 * is {"name","type","required",...}; we map type straight through (number,
 * string, boolean, object, array) defaulting to string. */
typedef struct {
    jbuf_t props;
    jbuf_t req;
    int nprops;
    int nreq;
} tm_schema_ctx_t;

static void tm_input_cb(const char *el, void *ctx) {
    tm_schema_ctx_t *sc = (tm_schema_ctx_t *)ctx;
    char *iname = json_get_str(el, "name");
    if (!iname || !iname[0]) {
        free(iname);
        return;
    }
    char *itype = json_get_str(el, "type");
    char *idesc = json_get_str(el, "description");
    const char *t = (itype && itype[0]) ? itype : "string";

    if (sc->nprops++)
        jbuf_append(&sc->props, ",");
    jbuf_append_json_str(&sc->props, iname);
    jbuf_append(&sc->props, ":{\"type\":");
    jbuf_append_json_str(&sc->props, t);
    if (idesc && idesc[0]) {
        jbuf_append(&sc->props, ",\"description\":");
        jbuf_append_json_str(&sc->props, idesc);
    }
    jbuf_append(&sc->props, "}");

    if (json_get_bool(el, "required", false)) {
        if (sc->nreq++)
            jbuf_append(&sc->req, ",");
        jbuf_append_json_str(&sc->req, iname);
    }
    free(iname);
    free(itype);
    free(idesc);
}

static void tm_reg_cb(const char *el, void *ctx) {
    tm_reg_ctx_t *rc = (tm_reg_ctx_t *)ctx;
    char *tool_id = json_get_str(el, "tool_id");
    char *tool_obj = json_get_raw(el, "tool");
    const char *scope = tool_obj ? tool_obj : el;
    char *name = json_get_str(scope, "name");
    if (!name || !name[0]) {
        free(tool_id);
        free(tool_obj);
        free(name);
        return;
    }
    char *desc = json_get_str(scope, "description");
    char *backend = json_get_str(scope, "backend");
    if (!backend)
        backend = json_get_str(el, "backend");

    char *schema_raw = json_get_raw(scope, "input_schema");
    if (!schema_raw)
        schema_raw = json_get_raw(scope, "inputSchema");
    if (!schema_raw)
        schema_raw = json_get_raw(scope, "schema");

    jbuf_t schema;
    jbuf_init(&schema, 384);
    if (schema_raw && schema_raw[0]) {
        jbuf_append(&schema, schema_raw);
    } else {
        tm_schema_ctx_t sc = {0};
        jbuf_init(&sc.props, 256);
        jbuf_init(&sc.req, 64);
        json_array_foreach(scope, "inputs", tm_input_cb, &sc);
        jbuf_append(&schema, "{\"type\":\"object\",\"properties\":{");
        jbuf_append(&schema, sc.props.data ? sc.props.data : "");
        jbuf_append(&schema, "},\"required\":[");
        jbuf_append(&schema, sc.req.data ? sc.req.data : "");
        jbuf_append(&schema, "]}");
        jbuf_free(&sc.props);
        jbuf_free(&sc.req);
    }

    char dsco_name[256];
    const char *remote_id = (tool_id && tool_id[0]) ? tool_id : name;
    tm_make_dsco_name(remote_id, name, dsco_name, sizeof(dsco_name));

    jbuf_t full_desc;
    jbuf_init(&full_desc, 512);
    if (desc && desc[0])
        jbuf_append(&full_desc, desc);
    else
        jbuf_append(&full_desc, "Tool Management API tool");
    if (backend && backend[0])
        jbuf_appendf(&full_desc, " [backend:%s]", backend);
    if (remote_id && remote_id[0])
        jbuf_appendf(&full_desc, " [tool_id:%s]", remote_id);

    tools_register_external(dsco_name, full_desc.data ? full_desc.data : "", schema.data,
                            tm_external_cb, safe_strdup(remote_id));
    rc->count++;

    jbuf_free(&full_desc);
    jbuf_free(&schema);
    free(schema_raw);
    free(tool_id);
    free(tool_obj);
    free(name);
    free(desc);
    free(backend);
}

int toolmgmt_register_tools(void) {
    char *body = toolmgmt_list_tools(1000);
    if (!body)
        return -1;
    /* The catalog is a top-level JSON array; json_array_foreach expects an
     * array under a key, so wrap it under a collision-proof sentinel key. */
    size_t n = strlen(body);
    char *wrapped = malloc(n + 32);
    tm_reg_ctx_t rc = {0};
    if (wrapped) {
        snprintf(wrapped, n + 32, "{\"__tm_items__\":%s}", body);
        json_array_foreach(wrapped, "__tm_items__", tm_reg_cb, &rc);
        free(wrapped);
    }
    free(body);
    return rc.count;
}

/* ── CLI ───────────────────────────────────────────────────────────────── */

static bool is_json_scalar(const char *v) {
    if (!v || !v[0])
        return false;
    if (strcmp(v, "true") == 0 || strcmp(v, "false") == 0 || strcmp(v, "null") == 0)
        return true;
    if (v[0] == '{' || v[0] == '[' || v[0] == '"')
        return true;
    /* numeric? */
    const char *p = v;
    if (*p == '-' || *p == '+')
        p++;
    bool digit = false, dot = false;
    for (; *p; p++) {
        if (isdigit((unsigned char)*p))
            digit = true;
        else if (*p == '.' && !dot)
            dot = true;
        else
            return false;
    }
    return digit;
}

/* Build a JSON object from "key=value" tokens. Values that look like JSON
 * scalars/literals are emitted raw; everything else is quoted as a string. */
static void kv_to_json(jbuf_t *b, char **pairs, int n) {
    jbuf_append(b, "{");
    int emitted = 0;
    for (int i = 0; i < n; i++) {
        char *eq = strchr(pairs[i], '=');
        if (!eq)
            continue;
        *eq = '\0';
        const char *key = pairs[i];
        const char *val = eq + 1;
        if (emitted)
            jbuf_append(b, ",");
        jbuf_append_json_str(b, key);
        jbuf_append(b, ":");
        if (is_json_scalar(val))
            jbuf_append(b, val);
        else
            jbuf_append_json_str(b, val);
        *eq = '=';
        emitted++;
    }
    jbuf_append(b, "}");
}

static void tm_print_name_cb(const char *el, void *ctx) {
    (void)ctx;
    char *name = json_get_str(el, "name");
    if (name && name[0]) {
        char *desc = json_get_str(el, "description");
        if (desc && desc[0]) {
            /* one line: truncate long descriptions */
            char short_d[100];
            snprintf(short_d, sizeof(short_d), "%s", desc);
            printf("  %-32s %s\n", name, short_d);
        } else {
            printf("  %s\n", name);
        }
        free(desc);
    }
    free(name);
}

static int tm_cli_list(int argc, char **argv) {
    int limit = 1000;
    for (int i = 0; i < argc; i++)
        if (strcmp(argv[i], "--limit") == 0 && i + 1 < argc)
            limit = atoi(argv[i + 1]);
    char path[128];
    snprintf(path, sizeof(path), "/api/v1/tools?limit=%d", limit > 0 ? limit : 1000);
    char *body = NULL;
    long st = toolmgmt_request("GET", path, NULL, &body);
    if (st < 0) {
        fprintf(stderr, "tools list: cannot reach %s (transport error)\n", toolmgmt_base_url());
        free(body);
        return 1;
    }
    if (st < 200 || st >= 300) {
        fprintf(stderr, "tools list: HTTP %ld from %s\n", st, toolmgmt_base_url());
        if (body && body[0])
            fprintf(stderr, "  %s\n", body);
        free(body);
        return 1;
    }
    size_t n = strlen(body);
    char *wrapped = malloc(n + 32);
    if (wrapped) {
        snprintf(wrapped, n + 32, "{\"__tm_items__\":%s}", body);
        int c = json_array_foreach(wrapped, "__tm_items__", tm_print_name_cb, NULL);
        printf("\n%d tools available at %s\n", c, toolmgmt_base_url());
        free(wrapped);
    }
    free(body);
    return 0;
}

/* discover/search returns {"results":[{"tool_id","tool":{name,description,…}}]} */
static void tm_search_cb(const char *el, void *ctx) {
    (void)ctx;
    char *tid = json_get_str(el, "tool_id");
    char *tool = json_get_raw(el, "tool");
    char *name = tool ? json_get_str(tool, "name") : NULL;
    char *desc = tool ? json_get_str(tool, "description") : NULL;
    const char *label = (name && name[0]) ? name : (tid ? tid : "?");
    if (desc && desc[0]) {
        char shortd[80];
        snprintf(shortd, sizeof(shortd), "%s", desc);
        printf("  %-32s %s\n", label, shortd);
    } else {
        printf("  %s\n", label);
    }
    free(tid);
    free(tool);
    free(name);
    free(desc);
}

static int tm_cli_search(int argc, char **argv) {
    /* argv: "<query>" [--limit N] [--backend B] */
    const char *query = NULL, *backend = NULL;
    int limit = 10;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--limit") == 0 && i + 1 < argc)
            limit = atoi(argv[++i]);
        else if (strcmp(argv[i], "--backend") == 0 && i + 1 < argc)
            backend = argv[++i];
        else if (!query)
            query = argv[i];
    }
    if (!query) {
        fprintf(stderr, "usage: dsco tools search \"<query>\" [--limit N] [--backend B]\n");
        return 2;
    }

    char *eq = curl_easy_escape(NULL, query, 0);
    char path[512];
    if (backend) {
        char *eb = curl_easy_escape(NULL, backend, 0);
        snprintf(path, sizeof(path), "/api/v1/discover/search?query=%s&limit=%d&backend=%s",
                 eq ? eq : query, limit, eb ? eb : backend);
        if (eb)
            curl_free(eb);
    } else {
        snprintf(path, sizeof(path), "/api/v1/discover/search?query=%s&limit=%d", eq ? eq : query,
                 limit);
    }
    if (eq)
        curl_free(eq);

    char *body = NULL;
    long st = toolmgmt_request("GET", path, NULL, &body);
    if (st < 200 || st >= 300) {
        fprintf(stderr, "tools search: HTTP %ld from %s\n", st, toolmgmt_base_url());
        if (body && body[0])
            fprintf(stderr, "  %s\n", body);
        free(body);
        return 1;
    }
    int c = json_array_foreach(body, "results", tm_search_cb, NULL);
    printf("\n%d matches for \"%s\"\n", c, query);
    free(body);
    return 0;
}

/* Finds one tool by name in the full catalog and prints its raw JSON. The
 * single-tool GET endpoint is unreliable server-side, so we filter the list
 * client-side instead. */
typedef struct {
    const char *want;
    char *found;
} tm_find_ctx_t;

static void tm_find_cb(const char *el, void *ctx) {
    tm_find_ctx_t *fc = (tm_find_ctx_t *)ctx;
    if (fc->found)
        return;
    char *name = json_get_str(el, "name");
    if (name && fc->want && strcmp(name, fc->want) == 0)
        fc->found = safe_strdup(el);
    free(name);
}

static int tm_cli_get(int argc, char **argv) {
    if (argc < 1) {
        fprintf(stderr, "usage: dsco tools get <tool>\n");
        return 2;
    }
    const char *want = argv[0];
    char *body = toolmgmt_list_tools(1000);
    if (!body) {
        fprintf(stderr, "tools get: catalog fetch from %s failed\n", toolmgmt_base_url());
        return 1;
    }
    size_t n = strlen(body);
    char *wrapped = malloc(n + 32);
    tm_find_ctx_t fc = {want, NULL};
    if (wrapped) {
        snprintf(wrapped, n + 32, "{\"__tm_items__\":%s}", body);
        json_array_foreach(wrapped, "__tm_items__", tm_find_cb, &fc);
        free(wrapped);
    }
    free(body);
    if (!fc.found) {
        fprintf(stderr, "tools get: no tool named '%s'\n", want);
        return 1;
    }
    printf("%s\n", fc.found);
    free(fc.found);
    return 0;
}

static int tm_cli_run(int argc, char **argv) {
    /* argv: <tool> [k=v ...] [--timeout-ms N] */
    if (argc < 1) {
        fprintf(stderr, "usage: dsco tools run <tool> [k=v ...]\n");
        return 2;
    }
    const char *tool = argv[0];
    int timeout_ms = 0;
    char *kv[256];
    int kvn = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--timeout-ms") == 0 && i + 1 < argc) {
            timeout_ms = atoi(argv[++i]);
            continue;
        }
        if (kvn < 256)
            kv[kvn++] = argv[i];
    }
    jbuf_t args;
    jbuf_init(&args, 256);
    kv_to_json(&args, kv, kvn);
    long st = -1;
    char *r = tm_exec(tool, args.data, timeout_ms, &st);
    jbuf_free(&args);
    if (r) {
        printf("%s\n", r);
        free(r);
    } else
        fprintf(stderr, "tools run: request to %s failed\n", toolmgmt_base_url());
    return (st >= 200 && st < 300) ? 0 : 1;
}

/* Build an inputs object like kv_to_json, but substitute placeholders that
 * reference earlier step outputs:
 *   {{prev}}   → the immediately-preceding step's output (raw JSON)
 *   {{stepN}}  → step N's output (1-based)
 * A placeholder is only honored as a whole value (val == "{{...}}"); the
 * substituted text is the raw output JSON so types (number/string/array) are
 * preserved. Missing references emit null. */
static void kv_to_json_chain(jbuf_t *b, char **pairs, int n, char **outputs, int ndone) {
    jbuf_append(b, "{");
    int emitted = 0;
    for (int i = 0; i < n; i++) {
        char *eq = strchr(pairs[i], '=');
        if (!eq)
            continue;
        *eq = '\0';
        const char *key = pairs[i];
        const char *val = eq + 1;
        if (emitted)
            jbuf_append(b, ",");
        jbuf_append_json_str(b, key);
        jbuf_append(b, ":");

        const char *subst = NULL;
        if (strcmp(val, "{{prev}}") == 0) {
            subst = (ndone > 0 && outputs[ndone - 1]) ? outputs[ndone - 1] : "null";
        } else if (strncmp(val, "{{step", 6) == 0) {
            int k = atoi(val + 6); /* 1-based */
            subst = (k >= 1 && k <= ndone && outputs[k - 1]) ? outputs[k - 1] : "null";
        }
        if (subst)
            jbuf_append(b, subst);
        else if (is_json_scalar(val))
            jbuf_append(b, val);
        else
            jbuf_append_json_str(b, val);

        *eq = '=';
        emitted++;
    }
    jbuf_append(b, "}");
}

static int tm_cli_chain(int argc, char **argv) {
    /* argv: <tool> k=v ... -- <tool> k=v ... -- ...   (sequential, outputs
     * thread forward via {{prev}} / {{stepN}} placeholders). */
    if (argc < 1) {
        fprintf(stderr, "usage: dsco tools chain <tool> [k=v ...] [-- <tool> [k=v ...]] ...\n"
                        "  reference earlier results with {{prev}} or {{stepN}}\n");
        return 2;
    }

    /* Pre-split into steps (tool + its k=v args). */
    const char *tools[64];
    char **kvs[64];
    int kvns[64];
    int nsteps = 0;
    char *kvbuf[1024];
    int kvbuf_n = 0;
    int i = 0;
    while (i < argc && nsteps < 64) {
        tools[nsteps] = argv[i++];
        kvs[nsteps] = &kvbuf[kvbuf_n];
        int cnt = 0;
        while (i < argc && strcmp(argv[i], "--") != 0) {
            if (kvbuf_n < 1024) {
                kvbuf[kvbuf_n++] = argv[i];
                cnt++;
            }
            i++;
        }
        if (i < argc && strcmp(argv[i], "--") == 0)
            i++;
        kvns[nsteps] = cnt;
        nsteps++;
    }

    char *outputs[64] = {0}; /* raw `output` JSON per completed step */
    int rc = 0;
    int done = 0;
    for (int s = 0; s < nsteps; s++) {
        jbuf_t args;
        jbuf_init(&args, 256);
        kv_to_json_chain(&args, kvs[s], kvns[s], outputs, done);
        long st = -1;
        char *resp = tm_exec(tools[s], args.data, 0, &st);
        jbuf_free(&args);

        char *status = resp ? json_get_str(resp, "status") : NULL;
        char *out_raw = resp ? json_get_raw(resp, "output") : NULL;
        bool ok = (st >= 200 && st < 300) && (!status || strcmp(status, "success") == 0);

        printf("=== [step %d] %s  (HTTP %ld%s%s) ===\n", s + 1, tools[s], st, status ? " " : "",
               status ? status : "");
        if (resp)
            printf("%s\n", resp);

        outputs[done++] = out_raw ? out_raw : safe_strdup("null");
        free(status);
        free(resp);

        if (!ok) {
            fprintf(stderr, "chain: step %d (%s) failed; stopping\n", s + 1, tools[s]);
            rc = 1;
            break;
        }
    }

    for (int k = 0; k < done; k++)
        free(outputs[k]);
    return rc;
}

static int tm_cli_batch(int argc, char **argv) {
    /* argv: <tool> k=v ... -- <tool> k=v ... -- ...   (client-side parallel) */
    if (argc < 1) {
        fprintf(stderr, "usage: dsco tools batch <tool> [k=v ...] [-- <tool> [k=v ...]] ...\n");
        return 2;
    }
    int max_conc = 8;
    /* Split argv into groups on "--". */
    tm_call_t calls[64];
    jbuf_t argbufs[64];
    int ncalls = 0;
    int i = 0;
    while (i < argc && ncalls < 64) {
        const char *tool = argv[i++];
        char *kv[256];
        int kvn = 0;
        while (i < argc && strcmp(argv[i], "--") != 0) {
            if (kvn < 256)
                kv[kvn++] = argv[i];
            i++;
        }
        if (i < argc && strcmp(argv[i], "--") == 0)
            i++; /* skip separator */
        jbuf_init(&argbufs[ncalls], 256);
        kv_to_json(&argbufs[ncalls], kv, kvn);
        calls[ncalls].tool = tool;
        calls[ncalls].args_json = argbufs[ncalls].data;
        calls[ncalls].timeout_ms = 0;
        calls[ncalls].result = NULL;
        calls[ncalls].status = -1;
        ncalls++;
    }

    int ok = toolmgmt_parallel(calls, ncalls, max_conc);

    for (int k = 0; k < ncalls; k++) {
        printf("=== [%d] %s  (HTTP %ld) ===\n", k, calls[k].tool, calls[k].status);
        if (calls[k].result)
            printf("%s\n", calls[k].result);
        free(calls[k].result);
        jbuf_free(&argbufs[k]);
    }
    fprintf(stderr, "batch: %d/%d succeeded\n", ok, ncalls);
    return ok == ncalls ? 0 : 1;
}

static int tm_cli_plan(int argc, char **argv) {
    /* argv: "<query>" [--intent X] [--max-steps N] */
    const char *query = NULL, *intent = NULL;
    int max_steps = 5;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--intent") == 0 && i + 1 < argc)
            intent = argv[++i];
        else if (strcmp(argv[i], "--max-steps") == 0 && i + 1 < argc)
            max_steps = atoi(argv[++i]);
        else if (!query)
            query = argv[i];
    }
    if (!query) {
        fprintf(stderr, "usage: dsco tools plan \"<query>\" [--intent X] [--max-steps N]\n");
        return 2;
    }
    char *r = toolmgmt_recommend(intent, query, max_steps);
    if (r) {
        printf("%s\n", r);
        free(r);
        return 0;
    }
    fprintf(stderr, "tools plan: request to %s failed\n", toolmgmt_base_url());
    return 1;
}

static void tm_cli_usage(void) {
    fprintf(stderr,
            "usage: dsco tools <command> [args]\n\n"
            "commands:\n"
            "  list [--limit N]                 list remote tools\n"
            "  search \"<query>\" [--limit N]     semantic tool discovery\n"
            "  get <tool>                       show one tool's schema\n"
            "  run <tool> [k=v ...]             execute one tool\n"
            "  batch <tool> [k=v] [-- ...]      execute many tools in parallel\n"
            "  chain <tool> [k=v] [-- ...]      execute in sequence; thread outputs\n"
            "                                   via {{prev}} / {{stepN}} placeholders\n"
            "  plan \"<query>\" [--intent X]      recommend a tool pipeline\n"
            "  register                         register remote tools as dsco tools\n\n"
            "config (env, overridable by --tm-url / --tm-token):\n"
            "  TOOLS_API_URL / TOOL_MANAGEMENT_API_URL   base URL (default %s)\n"
            "  TOOLS_API_TOKEN / AUTH_TOKEN              bearer token\n"
            "  TOOLS_API_TIMEOUT                         request timeout seconds\n",
            TOOLMGMT_API_URL_DEFAULT);
}

int toolmgmt_cli(int argc, char **argv) {
    /* argv[0]="dsco", argv[1]="tools"; subcommand at argv[2]. Also strip
     * global --tm-url/--tm-token overrides anywhere in the remaining args. */
    char *rest[256];
    int rn = 0;
    for (int i = 2; i < argc && rn < 256; i++) {
        if (strcmp(argv[i], "--tm-url") == 0 && i + 1 < argc) {
            toolmgmt_set_base_url(argv[++i]);
            continue;
        }
        if (strcmp(argv[i], "--tm-token") == 0 && i + 1 < argc) {
            toolmgmt_set_token(argv[++i]);
            continue;
        }
        rest[rn++] = argv[i];
    }

    if (rn == 0 || strcmp(rest[0], "list") == 0)
        return tm_cli_list(rn > 0 ? rn - 1 : 0, rest + (rn > 0 ? 1 : 0));
    if (strcmp(rest[0], "search") == 0)
        return tm_cli_search(rn - 1, rest + 1);
    if (strcmp(rest[0], "get") == 0)
        return tm_cli_get(rn - 1, rest + 1);
    if (strcmp(rest[0], "run") == 0)
        return tm_cli_run(rn - 1, rest + 1);
    if (strcmp(rest[0], "batch") == 0)
        return tm_cli_batch(rn - 1, rest + 1);
    if (strcmp(rest[0], "chain") == 0)
        return tm_cli_chain(rn - 1, rest + 1);
    if (strcmp(rest[0], "plan") == 0 || strcmp(rest[0], "recommend") == 0)
        return tm_cli_plan(rn - 1, rest + 1);
    if (strcmp(rest[0], "register") == 0) {
        tools_init();
        int c = toolmgmt_register_tools();
        if (c < 0) {
            fprintf(stderr, "register: catalog fetch from %s failed\n", toolmgmt_base_url());
            return 1;
        }
        printf("registered %d remote tools as dsco external tools\n", c);
        return 0;
    }
    if (strcmp(rest[0], "-h") == 0 || strcmp(rest[0], "--help") == 0) {
        tm_cli_usage();
        return 0;
    }
    fprintf(stderr, "unknown command: %s\n", rest[0]);
    tm_cli_usage();
    return 2;
}
