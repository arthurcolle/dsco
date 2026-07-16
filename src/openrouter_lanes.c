#include "openrouter_lanes.h"

#include "http_pool.h"
#include "json_util.h"
#include "openrouter_cache.h"

#include <ctype.h>
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OR_LANE_CANDIDATE_MAX 512
#define OR_LANE_DEFAULT_MODELS 32
#define OR_LANE_MAX_ENDPOINTS 16

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} or_http_buf_t;

typedef struct {
    openrouter_lane_t rows[OR_LANE_CANDIDATE_MAX];
    int count;
    const openrouter_lane_query_t *query;
} or_lane_collect_t;

static size_t or_lane_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    or_http_buf_t *b = userdata;
    size_t n = size * nmemb;
    if (b->len + n + 1 > b->cap) {
        size_t cap = (b->len + n + 1) * 2;
        char *p = realloc(b->data, cap);
        if (!p)
            return 0;
        b->data = p;
        b->cap = cap;
    }
    memcpy(b->data + b->len, ptr, n);
    b->len += n;
    b->data[b->len] = '\0';
    return n;
}

static bool or_lane_task_has(const char *task, const char *needle) {
    if (!task || !needle)
        return false;
    size_t nn = strlen(needle);
    for (const char *p = task; *p; p++) {
        size_t i = 0;
        while (i < nn && p[i] && tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i]))
            i++;
        if (i == nn)
            return true;
    }
    return false;
}

static double or_lane_quality(const or_model_view_t *m, const char *task) {
    double general = m->intelligence_index >= 0 ? m->intelligence_index : 0.0;
    double coding = m->coding_index >= 0 ? m->coding_index : general;
    double agentic = m->agentic_index >= 0 ? m->agentic_index : general;
    if (or_lane_task_has(task, "code") || or_lane_task_has(task, "debug") ||
        or_lane_task_has(task, "implement"))
        return coding;
    if (or_lane_task_has(task, "agent") || or_lane_task_has(task, "tool") ||
        or_lane_task_has(task, "swarm"))
        return agentic > 0 ? agentic : coding;
    return general > 0 ? general : coding;
}

static double or_lane_score(const openrouter_lane_t *l) {
    double price = l->input_price_per_m + l->output_price_per_m * 0.35;
    if (price < 0)
        price = 1000000.0; /* variable-priced meta routers sort last */
    /* Quality floors are enforced before sorting. Within the feasible set,
     * price dominates and quality breaks close-price ties. */
    return price * 100.0 - l->quality;
}

static int or_lane_cmp(const void *a, const void *b) {
    const openrouter_lane_t *la = a, *lb = b;
    double sa = or_lane_score(la), sb = or_lane_score(lb);
    if (sa < sb)
        return -1;
    if (sa > sb)
        return 1;
    return strcmp(la->model, lb->model);
}

static void or_lane_collect_cb(const or_model_view_t *m, void *userdata) {
    or_lane_collect_t *c = userdata;
    const openrouter_lane_query_t *q = c->query;
    if (!m || !m->id || c->count >= OR_LANE_CANDIDATE_MAX)
        return;
    if (m->id[0] == '~')
        return;
    if (q->require_tools && !m->tool_capable)
        return;
    if (q->min_context > 0 && m->context_window < q->min_context)
        return;
    if (q->min_output > 0 && m->max_output < q->min_output)
        return;
    bool is_free = m->input_price == 0.0 && m->output_price == 0.0;
    if (q->free_only && !is_free)
        return;
    if (m->input_price < 0 || m->output_price < 0)
        return; /* fully unrolled lanes require deterministic pricing */
    if (q->max_input_price_per_m > 0 && m->input_price > q->max_input_price_per_m)
        return;
    if (q->max_output_price_per_m > 0 && m->output_price > q->max_output_price_per_m)
        return;
    double quality = or_lane_quality(m, q->task);
    if (q->min_quality > 0 && quality < q->min_quality)
        return;

    openrouter_lane_t *l = &c->rows[c->count++];
    memset(l, 0, sizeof(*l));
    snprintf(l->model, sizeof(l->model), "%s", m->id);
    snprintf(l->tag, sizeof(l->tag), "%s", m->id);
    l->input_price_per_m = m->input_price;
    l->output_price_per_m = m->output_price;
    l->quality = quality;
    l->context_window = m->context_window;
    l->max_output = m->max_output;
    l->free = is_free;
    l->tool_capable = m->tool_capable != 0;
}

static bool or_lane_org_seen(const openrouter_lane_t *rows, int count, const char *model) {
    const char *slash = strchr(model, '/');
    size_t n = slash ? (size_t)(slash - model) : strlen(model);
    for (int i = 0; i < count; i++) {
        const char *other = strchr(rows[i].model, '/');
        size_t on = other ? (size_t)(other - rows[i].model) : strlen(rows[i].model);
        if (n == on && strncmp(rows[i].model, model, n) == 0)
            return true;
    }
    return false;
}

typedef struct {
    openrouter_lane_t rows[OR_LANE_MAX_ENDPOINTS];
    int count;
    const openrouter_lane_t *base;
} or_endpoint_collect_t;

static void or_endpoint_cb(const char *element, void *userdata) {
    or_endpoint_collect_t *c = userdata;
    if (!c || c->count >= OR_LANE_MAX_ENDPOINTS)
        return;
    char *provider = json_get_str(element, "provider_name");
    char *tag = json_get_str(element, "tag");
    char *quantization = json_get_str(element, "quantization");
    int status = json_get_int(element, "status", -999);
    double uptime = json_get_double(element, "uptime_last_30m", 0.0);
    char *params = json_get_raw(element, "supported_parameters");
    bool tools = params && strstr(params, "\"tools\"");
    free(params);
    if (!provider || !provider[0] || status != 0 || !tools || uptime < 95.0) {
        free(provider);
        free(tag);
        free(quantization);
        return;
    }
    char *pricing = json_get_raw(element, "pricing");
    double in_price = c->base->input_price_per_m;
    double out_price = c->base->output_price_per_m;
    if (pricing) {
        char *v = json_get_str(pricing, "prompt");
        if (v) {
            in_price = strtod(v, NULL) * 1e6;
            free(v);
        }
        v = json_get_str(pricing, "completion");
        if (v) {
            out_price = strtod(v, NULL) * 1e6;
            free(v);
        }
        free(pricing);
    }
    openrouter_lane_t *l = &c->rows[c->count++];
    *l = *c->base;
    /* OpenRouter's request contract pins provider by provider_name/slug; the
     * endpoint tag may include quantization and is retained separately. */
    const char *route_slug = tag && tag[0] ? tag : provider;
    char route_buf[OR_LANE_UPSTREAM_MAX];
    snprintf(route_buf, sizeof(route_buf), "%s", route_slug);
    char *slash = strchr(route_buf, '/');
    if (slash)
        *slash = '\0';
    for (char *p = route_buf; *p; p++)
        *p = (char)tolower((unsigned char)*p);
    snprintf(l->upstream, sizeof(l->upstream), "%s", route_buf);
    snprintf(l->quantization, sizeof(l->quantization), "%s",
             quantization && quantization[0] ? quantization : "unknown");
    snprintf(l->tag, sizeof(l->tag), "%s@%s/%s", l->model, l->upstream, l->quantization);
    l->input_price_per_m = in_price;
    l->output_price_per_m = out_price;
    l->free = in_price == 0.0 && out_price == 0.0;
    /* Uptime is a feasibility threshold; price remains dominant after that. */
    l->quality += (uptime - 95.0) * 0.02;
    free(provider);
    free(tag);
    free(quantization);
}

static int or_lane_fetch_endpoints(const openrouter_lane_t *base, int limit,
                                   openrouter_lane_t *out, int max) {
    if (!base || limit <= 0 || max <= 0)
        return 0;
    CURL *curl = curl_easy_init();
    if (!curl)
        return 0;
    char *escaped = curl_easy_escape(curl, base->model, 0);
    char url[512];
    snprintf(url, sizeof(url), "https://openrouter.ai/api/v1/models/%s/endpoints", escaped);
    curl_free(escaped);
    or_http_buf_t body = {0};
    dsco_http_pool_apply(curl);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, or_lane_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    CURLcode code = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    if (code != CURLE_OK || status != 200 || !body.data) {
        free(body.data);
        return 0;
    }
    char *data = json_get_raw(body.data, "data");
    or_endpoint_collect_t c = {.base = base};
    if (data)
        json_array_foreach(data, "endpoints", or_endpoint_cb, &c);
    free(data);
    free(body.data);
    qsort(c.rows, (size_t)c.count, sizeof(c.rows[0]), or_lane_cmp);
    int n = c.count < limit ? c.count : limit;
    if (n > max)
        n = max;
    memcpy(out, c.rows, (size_t)n * sizeof(out[0]));
    return n;
}

int openrouter_lanes_build(const openrouter_lane_query_t *query, openrouter_lane_t *out, int max) {
    if (!query || !out || max <= 0)
        return 0;
    if (openrouter_cache_wait_ready(250) <= 0)
        openrouter_cache_load_sync();
    or_lane_collect_t c = {.query = query};
    openrouter_cache_foreach(or_lane_collect_cb, &c);
    qsort(c.rows, (size_t)c.count, sizeof(c.rows[0]), or_lane_cmp);

    int model_limit = query->max_models > 0 ? query->max_models : OR_LANE_DEFAULT_MODELS;
    if (model_limit > c.count)
        model_limit = c.count;
    int n = 0;
    for (int i = 0; i < c.count && n < max && model_limit > 0; i++) {
        if (query->diversify_org && or_lane_org_seen(out, n, c.rows[i].model))
            continue;
        model_limit--;
        if (query->endpoints_per_model > 0) {
            int want = query->endpoints_per_model;
            if (want > OR_LANE_MAX_ENDPOINTS)
                want = OR_LANE_MAX_ENDPOINTS;
            int got = or_lane_fetch_endpoints(&c.rows[i], want, out + n, max - n);
            if (got > 0) {
                n += got;
                continue;
            }
        }
        out[n++] = c.rows[i];
    }
    return n;
}

static void or_json_escape(char *out, size_t out_len, const char *s) {
    size_t n = 0;
    if (!out_len)
        return;
    for (; s && *s && n + 2 < out_len; s++) {
        if (*s == '\\' || *s == '"')
            out[n++] = '\\';
        out[n++] = *s;
    }
    out[n] = '\0';
}

bool tool_openrouter_lanes(const char *input, char *result, size_t rlen) {
    if (!result || rlen == 0)
        return false;
    char *task = json_get_str(input, "task");
    openrouter_lane_query_t q = {
        .min_context = json_get_int(input, "min_context", 0),
        .min_output = json_get_int(input, "min_output", 0),
        .min_quality = json_get_double(input, "min_quality", 0.0),
        .max_input_price_per_m = json_get_double(input, "max_input_price_per_m", 0.0),
        .max_output_price_per_m = json_get_double(input, "max_output_price_per_m", 0.0),
        .max_models = json_get_int(input, "max_models", OR_LANE_DEFAULT_MODELS),
        .endpoints_per_model = json_get_int(input, "endpoints_per_model", 0),
        .require_tools = json_get_bool(input, "require_tools", true),
        .free_only = json_get_bool(input, "free_only", false),
        .diversify_org = json_get_bool(input, "diversify_org", false),
        .task = task,
    };
    int cap = json_get_int(input, "limit", 64);
    if (cap < 1)
        cap = 1;
    if (cap > 256)
        cap = 256;
    openrouter_lane_t *lanes = calloc((size_t)cap, sizeof(*lanes));
    if (!lanes) {
        free(task);
        snprintf(result, rlen, "{\"error\":\"allocation failed\"}");
        return false;
    }
    int n = openrouter_lanes_build(&q, lanes, cap);
    size_t used = (size_t)snprintf(result, rlen,
        "{\"count\":%d,\"fully_unrolled\":%s,\"catalog_models\":%d,\"lanes\":[",
        n, q.endpoints_per_model > 0 ? "true" : "false", openrouter_cache_count());
    for (int i = 0; i < n && used + 256 < rlen; i++) {
        char model[OR_LANE_MODEL_MAX * 2], upstream[OR_LANE_UPSTREAM_MAX * 2], tag[OR_LANE_TAG_MAX * 2];
        or_json_escape(model, sizeof(model), lanes[i].model);
        or_json_escape(upstream, sizeof(upstream), lanes[i].upstream);
        or_json_escape(tag, sizeof(tag), lanes[i].tag);
        int w = snprintf(result + used, rlen - used,
            "%s{\"model\":\"%s\",\"upstream\":\"%s\",\"quantization\":\"%s\",\"tag\":\"%s\","
            "\"input_price_per_m\":%.6f,\"output_price_per_m\":%.6f,\"quality\":%.2f,"
            "\"context\":%d,\"max_output\":%d,\"free\":%s}",
            i ? "," : "", model, upstream, lanes[i].quantization, tag, lanes[i].input_price_per_m,
            lanes[i].output_price_per_m, lanes[i].quality, lanes[i].context_window,
            lanes[i].max_output, lanes[i].free ? "true" : "false");
        if (w < 0 || (size_t)w >= rlen - used)
            break;
        used += (size_t)w;
    }
    if (used + 3 < rlen)
        snprintf(result + used, rlen - used, "]}");
    free(lanes);
    free(task);
    return true;
}
