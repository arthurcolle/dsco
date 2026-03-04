/* integrations.c — External service API integrations for dsco.
 *
 * Each tool wraps a REST API call via libcurl. API keys are read from
 * environment variables at call time. If a key is missing, the tool
 * returns an actionable error message telling the user how to set it.
 *
 * All tools follow the standard dsco tool signature:
 *   bool tool_xxx(const char *input_json, char *result, size_t rlen);
 */

#include "integrations.h"
#include "json_util.h"
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>

/* ── Shared HTTP helper ────────────────────────────────────────────────── */

typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} http_buf_t;

static size_t http_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    size_t total = size * nmemb;
    http_buf_t *b = (http_buf_t *)userdata;
    if (b->len + total >= b->cap) {
        b->cap = (b->len + total) * 2 + 1;
        b->data = realloc(b->data, b->cap);
        if (!b->data) return 0;
    }
    memcpy(b->data + b->len, ptr, total);
    b->len += total;
    b->data[b->len] = '\0';
    return total;
}

/* Perform an HTTP request with JSON body and custom headers.
 * Returns HTTP status code, response body in out_buf (caller frees out_buf->data). */
static long http_json_request(const char *method, const char *url,
                                const char *body,
                                const char *headers[], int header_count,
                                http_buf_t *out_buf) {
    CURL *curl = curl_easy_init();
    if (!curl) return -1;

    out_buf->data = malloc(4096);
    out_buf->len = 0;
    out_buf->cap = 4096;
    out_buf->data[0] = '\0';

    struct curl_slist *hdrs = NULL;
    hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
    hdrs = curl_slist_append(hdrs, "Accept: application/json");
    for (int i = 0; i < header_count; i++) {
        hdrs = curl_slist_append(hdrs, headers[i]);
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    if (body && strcmp(method, "GET") != 0) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
        if (strcmp(method, "PUT") == 0)
            curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
        else if (strcmp(method, "PATCH") == 0)
            curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PATCH");
        else if (strcmp(method, "DELETE") == 0)
            curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
    }
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, http_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, out_buf);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    if (res == CURLE_OK)
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        snprintf(out_buf->data, out_buf->cap, "curl error: %s", curl_easy_strerror(res));
        return -1;
    }
    return http_code;
}

/* Simple GET request with auth header */
static long http_get_authed(const char *url, const char *auth_header,
                              http_buf_t *out_buf) {
    CURL *curl = curl_easy_init();
    if (!curl) return -1;

    out_buf->data = malloc(8192);
    out_buf->len = 0;
    out_buf->cap = 8192;
    out_buf->data[0] = '\0';

    struct curl_slist *hdrs = NULL;
    hdrs = curl_slist_append(hdrs, "Accept: application/json");
    if (auth_header) hdrs = curl_slist_append(hdrs, auth_header);

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, http_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, out_buf);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "dsco/" DSCO_VERSION);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    if (res == CURLE_OK)
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);
    return res == CURLE_OK ? http_code : -1;
}

/* Check if API key is set, return false with error message if not */
static bool require_key(const char *env_var, const char *service,
                           char *result, size_t rlen, const char **out_key) {
    const char *key = getenv(env_var);
    if (!key || !key[0]) {
        snprintf(result, rlen,
                 "%s API key not set. Run: export %s=your_key_here\n"
                 "Or use /setup to auto-detect from environment.",
                 service, env_var);
        return false;
    }
    *out_key = key;
    return true;
}

/* Truncate response to fit in result buffer with room for wrapper text */
static void truncate_response(const char *src, char *dst, size_t dst_len, size_t reserved) {
    size_t available = dst_len - reserved;
    size_t src_len = strlen(src);
    if (src_len <= available) {
        memcpy(dst + strlen(dst), src, src_len + 1);
    } else {
        size_t pos = strlen(dst);
        memcpy(dst + pos, src, available - 20);
        dst[pos + available - 20] = '\0';
        strcat(dst, "\n[truncated]");
    }
}

/* ══════════════════════════════════════════════════════════════════════════
 *  TAVILY WEB SEARCH
 * ══════════════════════════════════════════════════════════════════════════ */

bool tool_tavily_search(const char *input, char *result, size_t rlen) {
    const char *api_key;
    if (!require_key("TAVILY_API_KEY", "Tavily", result, rlen, &api_key)) return false;

    char *query = json_get_str(input, "query");
    if (!query || !query[0]) {
        free(query);
        snprintf(result, rlen, "missing required parameter: query");
        return false;
    }

    int max_results = json_get_int(input, "max_results", 5);
    bool include_answer = json_get_bool(input, "include_answer", true);
    char *search_depth = json_get_str(input, "search_depth");

    jbuf_t body;
    jbuf_init(&body, 1024);
    jbuf_append(&body, "{\"api_key\":");
    jbuf_append_json_str(&body, api_key);
    jbuf_append(&body, ",\"query\":");
    jbuf_append_json_str(&body, query);
    jbuf_append(&body, ",\"max_results\":");
    jbuf_append_int(&body, max_results);
    if (include_answer) jbuf_append(&body, ",\"include_answer\":true");
    jbuf_append(&body, ",\"search_depth\":");
    jbuf_append_json_str(&body, search_depth ? search_depth : "basic");
    jbuf_append(&body, "}");

    http_buf_t resp = {0};
    long status = http_json_request("POST", "https://api.tavily.com/search",
                                      body.data, NULL, 0, &resp);
    jbuf_free(&body);
    free(query);
    free(search_depth);

    if (status != 200) {
        snprintf(result, rlen, "Tavily API error (HTTP %ld): %.500s",
                 status, resp.data ? resp.data : "no response");
        free(resp.data);
        return false;
    }

    /* Format response */
    result[0] = '\0';
    if (resp.data) {
        char *answer = json_get_str(resp.data, "answer");
        if (answer && answer[0]) {
            snprintf(result, rlen, "Answer: %s\n\nSources:\n", answer);
        }
        free(answer);
        truncate_response(resp.data, result, rlen, 64);
    }
    free(resp.data);
    return true;
}

/* ══════════════════════════════════════════════════════════════════════════
 *  BRAVE SEARCH
 * ══════════════════════════════════════════════════════════════════════════ */

bool tool_brave_search(const char *input, char *result, size_t rlen) {
    const char *api_key;
    if (!require_key("BRAVE_API_KEY", "Brave Search", result, rlen, &api_key)) return false;

    char *query = json_get_str(input, "query");
    if (!query || !query[0]) {
        free(query);
        snprintf(result, rlen, "missing required parameter: query");
        return false;
    }

    int count = json_get_int(input, "count", 5);

    /* URL-encode query */
    CURL *curl = curl_easy_init();
    char *encoded = curl_easy_escape(curl, query, 0);
    char url[2048];
    snprintf(url, sizeof(url),
             "https://api.search.brave.com/res/v1/web/search?q=%s&count=%d",
             encoded, count);
    curl_free(encoded);
    curl_easy_cleanup(curl);

    char auth[512];
    snprintf(auth, sizeof(auth), "X-Subscription-Token: %s", api_key);

    http_buf_t resp = {0};
    long status = http_get_authed(url, auth, &resp);
    free(query);

    if (status != 200) {
        snprintf(result, rlen, "Brave Search error (HTTP %ld): %.500s",
                 status, resp.data ? resp.data : "no response");
        free(resp.data);
        return false;
    }

    result[0] = '\0';
    truncate_response(resp.data ? resp.data : "{}", result, rlen, 32);
    free(resp.data);
    return true;
}

/* ══════════════════════════════════════════════════════════════════════════
 *  GITHUB API
 * ══════════════════════════════════════════════════════════════════════════ */

static bool github_api(const char *path, const char *method, const char *body,
                         char *result, size_t rlen) {
    const char *token;
    if (!require_key("GITHUB_TOKEN", "GitHub", result, rlen, &token)) return false;

    char url[2048];
    if (strncmp(path, "http", 4) == 0)
        snprintf(url, sizeof(url), "%s", path);
    else
        snprintf(url, sizeof(url), "https://api.github.com%s", path);

    char auth[512];
    snprintf(auth, sizeof(auth), "Authorization: Bearer %s", token);
    const char *headers[] = { auth, "X-GitHub-Api-Version: 2022-11-28" };

    http_buf_t resp = {0};
    long status;
    if (body && strcmp(method, "GET") != 0) {
        status = http_json_request(method, url, body, headers, 2, &resp);
    } else {
        http_buf_t r = {0};
        CURL *curl = curl_easy_init();
        if (!curl) { snprintf(result, rlen, "curl init failed"); return false; }
        r.data = malloc(16384); r.len = 0; r.cap = 16384; r.data[0] = '\0';
        struct curl_slist *hdrs = NULL;
        hdrs = curl_slist_append(hdrs, "Accept: application/vnd.github+json");
        hdrs = curl_slist_append(hdrs, auth);
        hdrs = curl_slist_append(hdrs, "X-GitHub-Api-Version: 2022-11-28");
        hdrs = curl_slist_append(hdrs, "User-Agent: dsco/" DSCO_VERSION);
        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, http_write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &r);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
        CURLcode res = curl_easy_perform(curl);
        status = 0;
        if (res == CURLE_OK) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        curl_slist_free_all(hdrs);
        curl_easy_cleanup(curl);
        resp = r;
    }

    if (status < 200 || status >= 300) {
        snprintf(result, rlen, "GitHub API error (HTTP %ld): %.500s",
                 status, resp.data ? resp.data : "no response");
        free(resp.data);
        return false;
    }

    result[0] = '\0';
    truncate_response(resp.data ? resp.data : "{}", result, rlen, 32);
    free(resp.data);
    return true;
}

bool tool_github_search(const char *input, char *result, size_t rlen) {
    char *query = json_get_str(input, "query");
    char *type = json_get_str(input, "type");
    if (!query || !query[0]) {
        free(query); free(type);
        snprintf(result, rlen, "missing required parameter: query");
        return false;
    }

    const char *search_type = (type && type[0]) ? type : "repositories";
    CURL *curl = curl_easy_init();
    char *enc = curl_easy_escape(curl, query, 0);
    char path[2048];
    snprintf(path, sizeof(path), "/search/%s?q=%s&per_page=10", search_type, enc);
    curl_free(enc);
    curl_easy_cleanup(curl);
    free(query); free(type);

    return github_api(path, "GET", NULL, result, rlen);
}

bool tool_github_issue(const char *input, char *result, size_t rlen) {
    char *repo = json_get_str(input, "repo");
    int number = json_get_int(input, "number", 0);
    if (!repo || !repo[0]) {
        free(repo);
        snprintf(result, rlen, "missing required parameter: repo (e.g. owner/name)");
        return false;
    }

    char path[512];
    if (number > 0)
        snprintf(path, sizeof(path), "/repos/%s/issues/%d", repo, number);
    else
        snprintf(path, sizeof(path), "/repos/%s/issues?per_page=10&state=open", repo);
    free(repo);

    return github_api(path, "GET", NULL, result, rlen);
}

bool tool_github_pr(const char *input, char *result, size_t rlen) {
    char *repo = json_get_str(input, "repo");
    int number = json_get_int(input, "number", 0);
    if (!repo || !repo[0]) {
        free(repo);
        snprintf(result, rlen, "missing required parameter: repo");
        return false;
    }

    char path[512];
    if (number > 0)
        snprintf(path, sizeof(path), "/repos/%s/pulls/%d", repo, number);
    else
        snprintf(path, sizeof(path), "/repos/%s/pulls?per_page=10&state=open", repo);
    free(repo);

    return github_api(path, "GET", NULL, result, rlen);
}

bool tool_github_repo(const char *input, char *result, size_t rlen) {
    char *repo = json_get_str(input, "repo");
    if (!repo || !repo[0]) {
        free(repo);
        snprintf(result, rlen, "missing required parameter: repo");
        return false;
    }

    char path[512];
    snprintf(path, sizeof(path), "/repos/%s", repo);
    free(repo);

    return github_api(path, "GET", NULL, result, rlen);
}

bool tool_github_create_issue(const char *input, char *result, size_t rlen) {
    char *repo = json_get_str(input, "repo");
    char *title = json_get_str(input, "title");
    char *body_text = json_get_str(input, "body");
    if (!repo || !repo[0] || !title || !title[0]) {
        free(repo); free(title); free(body_text);
        snprintf(result, rlen, "missing required parameters: repo, title");
        return false;
    }

    jbuf_t body;
    jbuf_init(&body, 1024);
    jbuf_append(&body, "{\"title\":");
    jbuf_append_json_str(&body, title);
    if (body_text && body_text[0]) {
        jbuf_append(&body, ",\"body\":");
        jbuf_append_json_str(&body, body_text);
    }
    jbuf_append(&body, "}");

    char path[512];
    snprintf(path, sizeof(path), "/repos/%s/issues", repo);
    free(repo); free(title); free(body_text);

    bool ok = github_api(path, "POST", body.data, result, rlen);
    jbuf_free(&body);
    return ok;
}

/* ══════════════════════════════════════════════════════════════════════════
 *  ALPHA VANTAGE — Financial time series
 * ══════════════════════════════════════════════════════════════════════════ */

bool tool_alpha_vantage(const char *input, char *result, size_t rlen) {
    const char *api_key;
    if (!require_key("ALPHA_VANTAGE_API_KEY", "Alpha Vantage", result, rlen, &api_key))
        return false;

    char *symbol = json_get_str(input, "symbol");
    char *function = json_get_str(input, "function");
    char *interval = json_get_str(input, "interval");

    if (!symbol || !symbol[0]) {
        free(symbol); free(function); free(interval);
        snprintf(result, rlen, "missing required parameter: symbol");
        return false;
    }

    const char *fn = function ? function : "TIME_SERIES_DAILY";

    char url[2048];
    if (interval && interval[0])
        snprintf(url, sizeof(url),
                 "https://www.alphavantage.co/query?function=%s&symbol=%s&interval=%s&apikey=%s",
                 fn, symbol, interval, api_key);
    else
        snprintf(url, sizeof(url),
                 "https://www.alphavantage.co/query?function=%s&symbol=%s&apikey=%s",
                 fn, symbol, api_key);

    free(symbol); free(function); free(interval);

    http_buf_t resp = {0};
    long status = http_get_authed(url, NULL, &resp);

    if (status != 200) {
        snprintf(result, rlen, "Alpha Vantage error (HTTP %ld)", status);
        free(resp.data);
        return false;
    }

    result[0] = '\0';
    truncate_response(resp.data ? resp.data : "{}", result, rlen, 32);
    free(resp.data);
    return true;
}

/* ── AV helper: generic query with extra params ────────────────────────── */

static bool av_query(const char *function, const char *params,
                     char *result, size_t rlen) {
    const char *api_key;
    if (!require_key("ALPHA_VANTAGE_API_KEY", "Alpha Vantage", result, rlen, &api_key))
        return false;

    char url[2048];
    if (params && params[0])
        snprintf(url, sizeof(url),
                 "https://www.alphavantage.co/query?function=%s&%s&apikey=%s",
                 function, params, api_key);
    else
        snprintf(url, sizeof(url),
                 "https://www.alphavantage.co/query?function=%s&apikey=%s",
                 function, api_key);

    http_buf_t resp = {0};
    long status = http_get_authed(url, NULL, &resp);

    if (status != 200) {
        snprintf(result, rlen, "Alpha Vantage error (HTTP %ld)", status);
        free(resp.data);
        return false;
    }

    /* Check for AV error messages */
    if (resp.data && strstr(resp.data, "Error Message")) {
        char *msg = json_get_str(resp.data, "Error Message");
        if (msg) {
            snprintf(result, rlen, "Alpha Vantage: %s", msg);
            free(msg);
            free(resp.data);
            return false;
        }
    }

    result[0] = '\0';
    truncate_response(resp.data ? resp.data : "{}", result, rlen, 48);
    free(resp.data);
    return true;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Alpha Vantage — Generic Table-Driven Dispatch (covers 100+ AV endpoints)
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Extract any JSON value (string, number, bool) as a malloc'd C string */
static char *json_val_str(const char *json, const char *key) {
    if (!json || !key) return NULL;
    char needle[128];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = json;
    while ((p = strstr(p, needle)) != NULL) {
        /* Ensure the char before the quote isn't alphanumeric (partial key match) */
        if (p > json && p[-1] != '{' && p[-1] != ',' && p[-1] != ' '
            && p[-1] != '\n' && p[-1] != '\t') { p++; continue; }
        p += strlen(needle);
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
        if (*p != ':') continue;
        p++;
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
        if (*p == '"') {
            p++;
            const char *end = p;
            while (*end && *end != '"') { if (*end == '\\') end++; end++; }
            size_t len = (size_t)(end - p);
            char *r = malloc(len + 1);
            if (!r) return NULL;
            memcpy(r, p, len);
            r[len] = '\0';
            return r;
        } else if (*p == 't') { return strdup("true");
        } else if (*p == 'f') { return strdup("false");
        } else if (*p == 'n') { return NULL; /* null */
        } else if (*p == '-' || (*p >= '0' && *p <= '9')) {
            const char *s = p;
            while (*p == '-' || *p == '.' || *p == 'e' || *p == 'E'
                   || *p == '+' || (*p >= '0' && *p <= '9')) p++;
            size_t len = (size_t)(p - s);
            char *r = malloc(len + 1);
            if (!r) return NULL;
            memcpy(r, s, len);
            r[len] = '\0';
            return r;
        }
        break;
    }
    return NULL;
}

/* All known AV query parameter names */
static const char *av_param_names[] = {
    "symbol","symbols","interval","outputsize","adjusted","extended_hours",
    "month","from_symbol","to_symbol","from_currency","to_currency","market",
    "time_period","series_type","fastperiod","slowperiod","signalperiod",
    "fastkperiod","slowkperiod","slowdperiod","slowkmatype","slowdmatype",
    "fastdperiod","fastdmatype","fastlimit","slowlimit",
    "nbdevup","nbdevdn","matype","fastmatype","slowmatype","signalmatype",
    "acceleration","maximum","timeperiod1","timeperiod2","timeperiod3",
    "maturity","horizon","date","state","tickers","topics","limit",
    "range","window_size","calculations","ohlc","quarter","contract",
    "require_greeks","entitlement","keywords","indicator",
    NULL
};

/* Generic AV dispatch — extracts all params from JSON, validates required ones */
static bool av_generic(const char *av_func, const char *input,
                       const char *req1, const char *req2,
                       char *result, size_t rlen) {
    if (req1) {
        char *v = json_val_str(input, req1);
        if (!v || !v[0]) {
            free(v);
            snprintf(result, rlen, "missing required parameter: %s", req1);
            return false;
        }
        free(v);
    }
    if (req2) {
        char *v = json_val_str(input, req2);
        if (!v || !v[0]) {
            free(v);
            snprintf(result, rlen, "missing required parameter: %s", req2);
            return false;
        }
        free(v);
    }

    char params[4096] = "";
    int off = 0;
    for (int i = 0; av_param_names[i]; i++) {
        char *val = json_val_str(input, av_param_names[i]);
        if (val && val[0]) {
            off += snprintf(params + off, (int)sizeof(params) - off,
                            "%s%s=%s", off ? "&" : "", av_param_names[i], val);
        }
        free(val);
    }

    return av_query(av_func, off ? params : NULL, result, rlen);
}

/* ── Macro-generated tool wrappers ────────────────────────────────────── */

#define AV_TOOL_0(cname, func) \
bool tool_av_##cname(const char *in, char *r, size_t l) { \
    (void)in; return av_generic(func, in, NULL, NULL, r, l); }

#define AV_TOOL_1(cname, func, req) \
bool tool_av_##cname(const char *in, char *r, size_t l) { \
    return av_generic(func, in, req, NULL, r, l); }

#define AV_TOOL_2(cname, func, r1, r2) \
bool tool_av_##cname(const char *in, char *r, size_t l) { \
    return av_generic(func, in, r1, r2, r, l); }

/* ── Core Stock Time Series ───────────────────────────────────────────── */
AV_TOOL_2(time_series_intraday, "TIME_SERIES_INTRADAY", "symbol", "interval")
AV_TOOL_1(time_series_daily, "TIME_SERIES_DAILY", "symbol")
AV_TOOL_1(time_series_daily_adj, "TIME_SERIES_DAILY_ADJUSTED", "symbol")
AV_TOOL_1(time_series_weekly, "TIME_SERIES_WEEKLY", "symbol")
AV_TOOL_1(time_series_weekly_adj, "TIME_SERIES_WEEKLY_ADJUSTED", "symbol")
AV_TOOL_1(time_series_monthly, "TIME_SERIES_MONTHLY", "symbol")
AV_TOOL_1(time_series_monthly_adj, "TIME_SERIES_MONTHLY_ADJUSTED", "symbol")
AV_TOOL_1(quote, "GLOBAL_QUOTE", "symbol")
AV_TOOL_1(bulk_quotes, "REALTIME_BULK_QUOTES", "symbol")

/* ── Search & Market Status ───────────────────────────────────────────── */
AV_TOOL_1(search, "SYMBOL_SEARCH", "keywords")
AV_TOOL_0(market_status, "MARKET_STATUS")

/* ── Options ──────────────────────────────────────────────────────────── */
AV_TOOL_1(realtime_options, "REALTIME_OPTIONS", "symbol")
AV_TOOL_1(historical_options, "HISTORICAL_OPTIONS", "symbol")

/* ── News & Sentiment ─────────────────────────────────────────────────── */
AV_TOOL_0(news, "NEWS_SENTIMENT")

/* ── Company Fundamentals ─────────────────────────────────────────────── */
AV_TOOL_1(overview, "OVERVIEW", "symbol")
AV_TOOL_1(etf, "ETF_PROFILE", "symbol")
AV_TOOL_1(income, "INCOME_STATEMENT", "symbol")
AV_TOOL_1(balance, "BALANCE_SHEET", "symbol")
AV_TOOL_1(cashflow, "CASH_FLOW", "symbol")
AV_TOOL_1(earnings, "EARNINGS", "symbol")
AV_TOOL_1(earnings_estimates, "EARNINGS_ESTIMATES", "symbol")
AV_TOOL_1(dividends, "DIVIDENDS", "symbol")
AV_TOOL_1(splits, "SPLITS", "symbol")
AV_TOOL_1(insider, "INSIDER_TRANSACTIONS", "symbol")
AV_TOOL_1(institutional, "INSTITUTIONAL_HOLDINGS", "symbol")

/* ── Earnings Call Transcript ─────────────────────────────────────────── */
AV_TOOL_2(transcript, "EARNINGS_CALL_TRANSCRIPT", "symbol", "quarter")

/* ── Corporate Events & Calendar ──────────────────────────────────────── */
AV_TOOL_0(movers, "TOP_GAINERS_LOSERS")
AV_TOOL_0(listing_status, "LISTING_STATUS")
AV_TOOL_0(earnings_calendar, "EARNINGS_CALENDAR")
AV_TOOL_0(ipo_calendar, "IPO_CALENDAR")

/* ── Advanced Analytics ───────────────────────────────────────────────── */
AV_TOOL_1(analytics_fixed, "ANALYTICS_FIXED_WINDOW", "symbols")
AV_TOOL_1(analytics_sliding, "ANALYTICS_SLIDING_WINDOW", "symbols")

/* ── Forex ────────────────────────────────────────────────────────────── */
AV_TOOL_2(forex, "CURRENCY_EXCHANGE_RATE", "from_currency", "to_currency")
AV_TOOL_2(fx_intraday, "FX_INTRADAY", "from_symbol", "to_symbol")
AV_TOOL_1(fx_daily, "FX_DAILY", "from_symbol")
AV_TOOL_1(fx_weekly, "FX_WEEKLY", "from_symbol")
AV_TOOL_1(fx_monthly, "FX_MONTHLY", "from_symbol")

/* ── Crypto ───────────────────────────────────────────────────────────── */
AV_TOOL_1(crypto, "DIGITAL_CURRENCY_DAILY", "symbol")
AV_TOOL_2(crypto_intraday, "CRYPTO_INTRADAY", "symbol", "market")
AV_TOOL_1(crypto_weekly, "DIGITAL_CURRENCY_WEEKLY", "symbol")
AV_TOOL_1(crypto_monthly, "DIGITAL_CURRENCY_MONTHLY", "symbol")

/* ── Commodities ──────────────────────────────────────────────────────── */
AV_TOOL_0(wti, "WTI")
AV_TOOL_0(brent, "BRENT")
AV_TOOL_0(natural_gas, "NATURAL_GAS")
AV_TOOL_0(copper, "COPPER")
AV_TOOL_0(aluminum, "ALUMINUM")
AV_TOOL_0(wheat, "WHEAT")
AV_TOOL_0(corn, "CORN")
AV_TOOL_0(cotton, "COTTON")
AV_TOOL_0(sugar, "SUGAR")
AV_TOOL_0(coffee, "COFFEE")
AV_TOOL_0(all_commodities, "ALL_COMMODITIES")

/* ── Precious Metals ──────────────────────────────────────────────────── */
AV_TOOL_1(gold_spot, "GOLD_SILVER_SPOT", "symbol")
AV_TOOL_1(gold_history, "GOLD_SILVER_HISTORY", "symbol")

/* ── Economic Indicators ──────────────────────────────────────────────── */
AV_TOOL_0(real_gdp, "REAL_GDP")
AV_TOOL_0(real_gdp_per_capita, "REAL_GDP_PER_CAPITA")
AV_TOOL_0(treasury_yield, "TREASURY_YIELD")
AV_TOOL_0(federal_funds_rate, "FEDERAL_FUNDS_RATE")
AV_TOOL_0(cpi, "CPI")
AV_TOOL_0(inflation, "INFLATION")
AV_TOOL_0(retail_sales, "RETAIL_SALES")
AV_TOOL_0(durables, "DURABLES")
AV_TOOL_0(unemployment, "UNEMPLOYMENT")
AV_TOOL_0(nonfarm_payroll, "NONFARM_PAYROLL")

/* ── Technical Indicators ─────────────────────────────────────────────── */
AV_TOOL_1(sma, "SMA", "symbol")
AV_TOOL_1(ema, "EMA", "symbol")
AV_TOOL_1(wma, "WMA", "symbol")
AV_TOOL_1(dema, "DEMA", "symbol")
AV_TOOL_1(tema, "TEMA", "symbol")
AV_TOOL_1(trima, "TRIMA", "symbol")
AV_TOOL_1(kama, "KAMA", "symbol")
AV_TOOL_1(mama, "MAMA", "symbol")
AV_TOOL_1(vwap, "VWAP", "symbol")
AV_TOOL_1(t3, "T3", "symbol")
AV_TOOL_1(macd, "MACD", "symbol")
AV_TOOL_1(macdext, "MACDEXT", "symbol")
AV_TOOL_1(stoch, "STOCH", "symbol")
AV_TOOL_1(stochf, "STOCHF", "symbol")
AV_TOOL_1(rsi, "RSI", "symbol")
AV_TOOL_1(stochrsi, "STOCHRSI", "symbol")
AV_TOOL_1(willr, "WILLR", "symbol")
AV_TOOL_1(adx, "ADX", "symbol")
AV_TOOL_1(adxr, "ADXR", "symbol")
AV_TOOL_1(apo, "APO", "symbol")
AV_TOOL_1(ppo, "PPO", "symbol")
AV_TOOL_1(mom, "MOM", "symbol")
AV_TOOL_1(bop, "BOP", "symbol")
AV_TOOL_1(cci, "CCI", "symbol")
AV_TOOL_1(cmo, "CMO", "symbol")
AV_TOOL_1(roc, "ROC", "symbol")
AV_TOOL_1(rocr, "ROCR", "symbol")
AV_TOOL_1(aroon, "AROON", "symbol")
AV_TOOL_1(aroonosc, "AROONOSC", "symbol")
AV_TOOL_1(mfi, "MFI", "symbol")
AV_TOOL_1(trix_ind, "TRIX", "symbol")
AV_TOOL_1(ultosc, "ULTOSC", "symbol")
AV_TOOL_1(dx, "DX", "symbol")
AV_TOOL_1(minus_di, "MINUS_DI", "symbol")
AV_TOOL_1(plus_di, "PLUS_DI", "symbol")
AV_TOOL_1(minus_dm, "MINUS_DM", "symbol")
AV_TOOL_1(plus_dm, "PLUS_DM", "symbol")
AV_TOOL_1(bbands, "BBANDS", "symbol")
AV_TOOL_1(midpoint, "MIDPOINT", "symbol")
AV_TOOL_1(midprice, "MIDPRICE", "symbol")
AV_TOOL_1(sar, "SAR", "symbol")
AV_TOOL_1(trange, "TRANGE", "symbol")
AV_TOOL_1(atr, "ATR", "symbol")
AV_TOOL_1(natr, "NATR", "symbol")
AV_TOOL_1(ad_line, "AD", "symbol")
AV_TOOL_1(adosc, "ADOSC", "symbol")
AV_TOOL_1(obv, "OBV", "symbol")
AV_TOOL_1(ht_trendline, "HT_TRENDLINE", "symbol")
AV_TOOL_1(ht_sine, "HT_SINE", "symbol")
AV_TOOL_1(ht_trendmode, "HT_TRENDMODE", "symbol")
AV_TOOL_1(ht_dcperiod, "HT_DCPERIOD", "symbol")
AV_TOOL_1(ht_dcphase, "HT_DCPHASE", "symbol")
AV_TOOL_1(ht_phasor, "HT_PHASOR", "symbol")

/* ══════════════════════════════════════════════════════════════════════════
 *  FRED — Federal Reserve Economic Data
 * ══════════════════════════════════════════════════════════════════════════ */

bool tool_fred_series(const char *input, char *result, size_t rlen) {
    const char *api_key;
    if (!require_key("FRED_API_KEY", "FRED", result, rlen, &api_key)) return false;

    char *series_id = json_get_str(input, "series_id");
    if (!series_id || !series_id[0]) {
        free(series_id);
        snprintf(result, rlen,
                 "missing required parameter: series_id\n"
                 "Examples: GDP, UNRATE, CPIAUCSL, DFF, T10Y2Y, VIXCLS");
        return false;
    }

    int limit = json_get_int(input, "limit", 30);
    char *sort = json_get_str(input, "sort_order");

    char url[2048];
    snprintf(url, sizeof(url),
             "https://api.stlouisfed.org/fred/series/observations"
             "?series_id=%s&api_key=%s&file_type=json&limit=%d&sort_order=%s",
             series_id, api_key, limit, sort ? sort : "desc");

    free(series_id);
    free(sort);

    http_buf_t resp = {0};
    long status = http_get_authed(url, NULL, &resp);

    if (status != 200) {
        snprintf(result, rlen, "FRED API error (HTTP %ld)", status);
        free(resp.data);
        return false;
    }

    result[0] = '\0';
    truncate_response(resp.data ? resp.data : "{}", result, rlen, 32);
    free(resp.data);
    return true;
}

/* ══════════════════════════════════════════════════════════════════════════
 *  SLACK — Post messages
 * ══════════════════════════════════════════════════════════════════════════ */

bool tool_slack_post(const char *input, char *result, size_t rlen) {
    const char *token;
    if (!require_key("SLACK_BOT_TOKEN", "Slack", result, rlen, &token)) return false;

    char *channel = json_get_str(input, "channel");
    char *text = json_get_str(input, "text");
    if (!channel || !channel[0] || !text || !text[0]) {
        free(channel); free(text);
        snprintf(result, rlen, "missing required parameters: channel, text");
        return false;
    }

    jbuf_t body;
    jbuf_init(&body, 1024);
    jbuf_append(&body, "{\"channel\":");
    jbuf_append_json_str(&body, channel);
    jbuf_append(&body, ",\"text\":");
    jbuf_append_json_str(&body, text);
    jbuf_append(&body, "}");

    char auth[512];
    snprintf(auth, sizeof(auth), "Authorization: Bearer %s", token);
    const char *headers[] = { auth };

    http_buf_t resp = {0};
    long status = http_json_request("POST", "https://slack.com/api/chat.postMessage",
                                      body.data, headers, 1, &resp);
    jbuf_free(&body);
    free(channel); free(text);

    if (status != 200) {
        snprintf(result, rlen, "Slack API error (HTTP %ld): %.500s",
                 status, resp.data ? resp.data : "");
        free(resp.data);
        return false;
    }

    /* Check Slack's ok field */
    bool ok = json_get_bool(resp.data ? resp.data : "{}", "ok", false);
    if (!ok) {
        char *err = json_get_str(resp.data, "error");
        snprintf(result, rlen, "Slack error: %s", err ? err : "unknown");
        free(err);
        free(resp.data);
        return false;
    }

    snprintf(result, rlen, "message posted successfully");
    free(resp.data);
    return true;
}

/* ══════════════════════════════════════════════════════════════════════════
 *  NOTION — Search and read pages
 * ══════════════════════════════════════════════════════════════════════════ */

static bool notion_api(const char *method, const char *path, const char *body,
                         char *result, size_t rlen) {
    const char *token;
    if (!require_key("NOTION_API_KEY", "Notion", result, rlen, &token)) return false;

    char url[2048];
    snprintf(url, sizeof(url), "https://api.notion.com/v1%s", path);

    char auth[512];
    snprintf(auth, sizeof(auth), "Authorization: Bearer %s", token);
    const char *headers[] = { auth, "Notion-Version: 2022-06-28" };

    http_buf_t resp = {0};
    long status = http_json_request(method, url, body, headers, 2, &resp);

    if (status < 200 || status >= 300) {
        snprintf(result, rlen, "Notion API error (HTTP %ld): %.500s",
                 status, resp.data ? resp.data : "");
        free(resp.data);
        return false;
    }

    result[0] = '\0';
    truncate_response(resp.data ? resp.data : "{}", result, rlen, 32);
    free(resp.data);
    return true;
}

bool tool_notion_search(const char *input, char *result, size_t rlen) {
    char *query = json_get_str(input, "query");

    jbuf_t body;
    jbuf_init(&body, 512);
    jbuf_append(&body, "{");
    if (query && query[0]) {
        jbuf_append(&body, "\"query\":");
        jbuf_append_json_str(&body, query);
    }
    jbuf_append(&body, ",\"page_size\":10}");

    bool ok = notion_api("POST", "/search", body.data, result, rlen);
    jbuf_free(&body);
    free(query);
    return ok;
}

bool tool_notion_page(const char *input, char *result, size_t rlen) {
    char *page_id = json_get_str(input, "page_id");
    if (!page_id || !page_id[0]) {
        free(page_id);
        snprintf(result, rlen, "missing required parameter: page_id");
        return false;
    }

    char path[512];
    snprintf(path, sizeof(path), "/blocks/%s/children?page_size=100", page_id);
    free(page_id);

    return notion_api("GET", path, NULL, result, rlen);
}

/* ══════════════════════════════════════════════════════════════════════════
 *  OPENWEATHERMAP — Weather data
 * ══════════════════════════════════════════════════════════════════════════ */

bool tool_weather(const char *input, char *result, size_t rlen) {
    const char *api_key;
    if (!require_key("OPENWEATHERMAP_API_KEY", "OpenWeatherMap", result, rlen, &api_key))
        return false;

    char *location = json_get_str(input, "location");
    if (!location || !location[0]) {
        free(location);
        snprintf(result, rlen, "missing required parameter: location");
        return false;
    }

    char *units = json_get_str(input, "units");
    const char *u = (units && units[0]) ? units : "metric";

    CURL *curl = curl_easy_init();
    char *enc = curl_easy_escape(curl, location, 0);
    char url[2048];
    snprintf(url, sizeof(url),
             "https://api.openweathermap.org/data/2.5/weather?q=%s&appid=%s&units=%s",
             enc, api_key, u);
    curl_free(enc);
    curl_easy_cleanup(curl);
    free(location); free(units);

    http_buf_t resp = {0};
    long status = http_get_authed(url, NULL, &resp);

    if (status != 200) {
        snprintf(result, rlen, "Weather API error (HTTP %ld): %.500s",
                 status, resp.data ? resp.data : "");
        free(resp.data);
        return false;
    }

    /* Format a readable response */
    if (resp.data) {
        char *name = json_get_str(resp.data, "name");
        char *main = json_get_raw(resp.data, "main");
        char *weather = json_get_raw(resp.data, "weather");
        char *wind = json_get_raw(resp.data, "wind");

        double temp = main ? json_get_int(main, "temp", 0) : 0;
        double feels = main ? json_get_int(main, "feels_like", 0) : 0;
        int humidity = main ? json_get_int(main, "humidity", 0) : 0;
        double wind_speed = wind ? json_get_int(wind, "speed", 0) : 0;
        char *desc = weather ? json_get_str(weather, "description") : NULL;

        snprintf(result, rlen,
                 "%s: %.1f°C (feels like %.1f°C)\n"
                 "Conditions: %s\n"
                 "Humidity: %d%%\n"
                 "Wind: %.1f m/s",
                 name ? name : "Unknown",
                 temp, feels,
                 desc ? desc : "unknown",
                 humidity, wind_speed);

        free(name); free(main); free(weather); free(wind); free(desc);
    }
    free(resp.data);
    return true;
}

/* ══════════════════════════════════════════════════════════════════════════
 *  FIRECRAWL — Structured web scraping
 * ══════════════════════════════════════════════════════════════════════════ */

bool tool_firecrawl(const char *input, char *result, size_t rlen) {
    const char *api_key;
    if (!require_key("FIRECRAWL_API_KEY", "Firecrawl", result, rlen, &api_key))
        return false;

    char *url_str = json_get_str(input, "url");
    if (!url_str || !url_str[0]) {
        free(url_str);
        snprintf(result, rlen, "missing required parameter: url");
        return false;
    }

    char *formats = json_get_str(input, "formats");

    jbuf_t body;
    jbuf_init(&body, 1024);
    jbuf_append(&body, "{\"url\":");
    jbuf_append_json_str(&body, url_str);
    jbuf_append(&body, ",\"formats\":[");
    if (formats && formats[0]) {
        jbuf_append_json_str(&body, formats);
    } else {
        jbuf_append(&body, "\"markdown\"");
    }
    jbuf_append(&body, "]}");

    char auth[512];
    snprintf(auth, sizeof(auth), "Authorization: Bearer %s", api_key);
    const char *headers[] = { auth };

    http_buf_t resp = {0};
    long status = http_json_request("POST", "https://api.firecrawl.dev/v1/scrape",
                                      body.data, headers, 1, &resp);
    jbuf_free(&body);
    free(url_str); free(formats);

    if (status != 200) {
        snprintf(result, rlen, "Firecrawl error (HTTP %ld): %.500s",
                 status, resp.data ? resp.data : "");
        free(resp.data);
        return false;
    }

    result[0] = '\0';
    truncate_response(resp.data ? resp.data : "{}", result, rlen, 32);
    free(resp.data);
    return true;
}

/* ══════════════════════════════════════════════════════════════════════════
 *  JINA READER — AI-powered web content extraction
 * ══════════════════════════════════════════════════════════════════════════ */

bool tool_jina_read(const char *input, char *result, size_t rlen) {
    char *url_str = json_get_str(input, "url");
    if (!url_str || !url_str[0]) {
        free(url_str);
        snprintf(result, rlen, "missing required parameter: url");
        return false;
    }

    char jina_url[2048];
    snprintf(jina_url, sizeof(jina_url), "https://r.jina.ai/%s", url_str);
    free(url_str);

    const char *jina_key = getenv("JINA_API_KEY");
    char auth[512] = "";
    if (jina_key && jina_key[0])
        snprintf(auth, sizeof(auth), "Authorization: Bearer %s", jina_key);

    http_buf_t resp = {0};
    long status = http_get_authed(jina_url, auth[0] ? auth : NULL, &resp);

    if (status != 200) {
        snprintf(result, rlen, "Jina Reader error (HTTP %ld)", status);
        free(resp.data);
        return false;
    }

    result[0] = '\0';
    truncate_response(resp.data ? resp.data : "", result, rlen, 32);
    free(resp.data);
    return true;
}

/* ══════════════════════════════════════════════════════════════════════════
 *  SERPAPI — Google Search results
 * ══════════════════════════════════════════════════════════════════════════ */

bool tool_serpapi(const char *input, char *result, size_t rlen) {
    const char *api_key;
    if (!require_key("SERPAPI_API_KEY", "SerpAPI", result, rlen, &api_key)) return false;

    char *query = json_get_str(input, "query");
    if (!query || !query[0]) {
        free(query);
        snprintf(result, rlen, "missing required parameter: query");
        return false;
    }

    CURL *curl = curl_easy_init();
    char *enc = curl_easy_escape(curl, query, 0);
    char url[2048];
    snprintf(url, sizeof(url),
             "https://serpapi.com/search.json?q=%s&api_key=%s&num=5",
             enc, api_key);
    curl_free(enc);
    curl_easy_cleanup(curl);
    free(query);

    http_buf_t resp = {0};
    long status = http_get_authed(url, NULL, &resp);

    if (status != 200) {
        snprintf(result, rlen, "SerpAPI error (HTTP %ld)", status);
        free(resp.data);
        return false;
    }

    result[0] = '\0';
    truncate_response(resp.data ? resp.data : "{}", result, rlen, 32);
    free(resp.data);
    return true;
}

/* ══════════════════════════════════════════════════════════════════════════
 *  DISCORD — Post messages via webhook or bot
 * ══════════════════════════════════════════════════════════════════════════ */

bool tool_discord_post(const char *input, char *result, size_t rlen) {
    char *webhook_url = json_get_str(input, "webhook_url");
    char *text = json_get_str(input, "text");
    char *channel_id = json_get_str(input, "channel_id");

    if (!text || !text[0]) {
        free(webhook_url); free(text); free(channel_id);
        snprintf(result, rlen, "missing required parameter: text");
        return false;
    }

    jbuf_t body;
    jbuf_init(&body, 512);
    jbuf_append(&body, "{\"content\":");
    jbuf_append_json_str(&body, text);
    jbuf_append(&body, "}");

    if (webhook_url && webhook_url[0]) {
        http_buf_t resp = {0};
        long status = http_json_request("POST", webhook_url, body.data, NULL, 0, &resp);
        jbuf_free(&body);
        free(webhook_url); free(text); free(channel_id);
        if (status >= 200 && status < 300) {
            snprintf(result, rlen, "message posted to Discord webhook");
            free(resp.data);
            return true;
        }
        snprintf(result, rlen, "Discord webhook error (HTTP %ld)", status);
        free(resp.data);
        return false;
    }

    const char *token = getenv("DISCORD_TOKEN");
    free(webhook_url);
    if (!token || !token[0]) {
        free(text); free(channel_id);
        jbuf_free(&body);
        snprintf(result, rlen, "DISCORD_TOKEN not set and no webhook_url provided");
        return false;
    }
    if (!channel_id || !channel_id[0]) {
        free(text); free(channel_id);
        jbuf_free(&body);
        snprintf(result, rlen, "missing channel_id for bot mode");
        return false;
    }

    char url[512];
    snprintf(url, sizeof(url), "https://discord.com/api/v10/channels/%s/messages", channel_id);
    char auth[512];
    snprintf(auth, sizeof(auth), "Authorization: Bot %s", token);
    const char *headers[] = { auth };

    http_buf_t resp = {0};
    long status = http_json_request("POST", url, body.data, headers, 1, &resp);
    jbuf_free(&body);
    free(channel_id); free(text);

    if (status >= 200 && status < 300) {
        snprintf(result, rlen, "message posted to Discord channel");
        free(resp.data);
        return true;
    }
    snprintf(result, rlen, "Discord API error (HTTP %ld)", status);
    free(resp.data);
    return false;
}

/* ══════════════════════════════════════════════════════════════════════════
 *  TWILIO — Send SMS
 * ══════════════════════════════════════════════════════════════════════════ */

bool tool_twilio_sms(const char *input, char *result, size_t rlen) {
    const char *auth_token;
    if (!require_key("TWILIO_AUTH_TOKEN", "Twilio", result, rlen, &auth_token)) return false;
    const char *account_sid = getenv("TWILIO_ACCOUNT_SID");
    const char *from_number = getenv("TWILIO_FROM_NUMBER");
    if (!account_sid || !from_number) {
        snprintf(result, rlen, "TWILIO_ACCOUNT_SID and TWILIO_FROM_NUMBER required");
        return false;
    }

    char *to = json_get_str(input, "to");
    char *body_text = json_get_str(input, "body");
    if (!to || !to[0] || !body_text || !body_text[0]) {
        free(to); free(body_text);
        snprintf(result, rlen, "missing required parameters: to, body");
        return false;
    }

    char url[512];
    snprintf(url, sizeof(url), "https://api.twilio.com/2010-04-01/Accounts/%s/Messages.json", account_sid);

    CURL *curl = curl_easy_init();
    if (!curl) { free(to); free(body_text); return false; }
    char *enc_to = curl_easy_escape(curl, to, 0);
    char *enc_body = curl_easy_escape(curl, body_text, 0);
    char *enc_from = curl_easy_escape(curl, from_number, 0);
    char post_data[4096];
    snprintf(post_data, sizeof(post_data), "To=%s&From=%s&Body=%s", enc_to, enc_from, enc_body);
    curl_free(enc_to); curl_free(enc_body); curl_free(enc_from);

    http_buf_t resp = {0};
    resp.data = malloc(4096); resp.len = 0; resp.cap = 4096; resp.data[0] = '\0';
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_data);
    curl_easy_setopt(curl, CURLOPT_USERNAME, account_sid);
    curl_easy_setopt(curl, CURLOPT_PASSWORD, auth_token);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, http_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

    CURLcode res = curl_easy_perform(curl);
    long status = 0;
    if (res == CURLE_OK) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    free(to); free(body_text);

    if (status == 201) { snprintf(result, rlen, "SMS sent"); free(resp.data); return true; }
    snprintf(result, rlen, "Twilio error (HTTP %ld)", status);
    free(resp.data);
    return false;
}

/* ══════════════════════════════════════════════════════════════════════════
 *  ELEVENLABS — Text-to-speech
 * ══════════════════════════════════════════════════════════════════════════ */

bool tool_elevenlabs_tts(const char *input, char *result, size_t rlen) {
    const char *api_key;
    if (!require_key("ELEVENLABS_API_KEY", "ElevenLabs", result, rlen, &api_key)) return false;

    char *text = json_get_str(input, "text");
    char *voice = json_get_str(input, "voice_id");
    char *output_path = json_get_str(input, "output");
    if (!text || !text[0]) { free(text); free(voice); free(output_path); snprintf(result, rlen, "missing: text"); return false; }

    const char *vid = (voice && voice[0]) ? voice : "21m00Tcm4TlvDq8ikWAM";
    const char *out = (output_path && output_path[0]) ? output_path : "/tmp/dsco_tts.mp3";

    char url[512];
    snprintf(url, sizeof(url), "https://api.elevenlabs.io/v1/text-to-speech/%s", vid);

    jbuf_t body; jbuf_init(&body, 512);
    jbuf_append(&body, "{\"text\":"); jbuf_append_json_str(&body, text);
    jbuf_append(&body, ",\"model_id\":\"eleven_multilingual_v2\"}");

    CURL *curl = curl_easy_init();
    FILE *fp = fopen(out, "wb");
    if (!curl || !fp) { if (fp) fclose(fp); if (curl) curl_easy_cleanup(curl); free(text); free(voice); free(output_path); jbuf_free(&body); return false; }

    char auth[512]; snprintf(auth, sizeof(auth), "xi-api-key: %s", api_key);
    struct curl_slist *hdrs = NULL;
    hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
    hdrs = curl_slist_append(hdrs, auth);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.data);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);

    CURLcode res = curl_easy_perform(curl);
    long status = 0;
    if (res == CURLE_OK) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    fclose(fp); curl_slist_free_all(hdrs); curl_easy_cleanup(curl);
    jbuf_free(&body); free(text); free(voice); free(output_path);

    if (status == 200) { snprintf(result, rlen, "audio saved to %s", out); return true; }
    snprintf(result, rlen, "ElevenLabs error (HTTP %ld)", status);
    return false;
}

/* ══════════════════════════════════════════════════════════════════════════
 *  PINECONE — Vector database query
 * ══════════════════════════════════════════════════════════════════════════ */

bool tool_pinecone_query(const char *input, char *result, size_t rlen) {
    const char *api_key;
    if (!require_key("PINECONE_API_KEY", "Pinecone", result, rlen, &api_key)) return false;

    char *host = json_get_str(input, "host");
    int top_k = json_get_int(input, "top_k", 5);
    if (!host || !host[0]) { free(host); snprintf(result, rlen, "missing: host (Pinecone index URL)"); return false; }

    char url[2048]; snprintf(url, sizeof(url), "https://%s/query", host);
    jbuf_t body; jbuf_init(&body, 256);
    jbuf_append(&body, "{\"topK\":"); jbuf_append_int(&body, top_k);
    jbuf_append(&body, ",\"includeMetadata\":true}");

    char auth[512]; snprintf(auth, sizeof(auth), "Api-Key: %s", api_key);
    const char *headers[] = { auth };
    http_buf_t resp = {0};
    long status = http_json_request("POST", url, body.data, headers, 1, &resp);
    jbuf_free(&body); free(host);

    if (status != 200) { snprintf(result, rlen, "Pinecone error (HTTP %ld)", status); free(resp.data); return false; }
    result[0] = '\0'; truncate_response(resp.data ? resp.data : "{}", result, rlen, 32);
    free(resp.data); return true;
}

/* ══════════════════════════════════════════════════════════════════════════
 *  STRIPE — Payment operations
 * ══════════════════════════════════════════════════════════════════════════ */

bool tool_stripe(const char *input, char *result, size_t rlen) {
    const char *api_key;
    if (!require_key("STRIPE_API_KEY", "Stripe", result, rlen, &api_key)) return false;

    char *action = json_get_str(input, "action");
    if (!action || !action[0]) { free(action); snprintf(result, rlen, "missing: action (list_charges, list_customers, get_balance, list_invoices)"); return false; }

    char url[512] = "";
    if (strcmp(action, "list_charges") == 0) snprintf(url, sizeof(url), "https://api.stripe.com/v1/charges?limit=10");
    else if (strcmp(action, "list_customers") == 0) snprintf(url, sizeof(url), "https://api.stripe.com/v1/customers?limit=10");
    else if (strcmp(action, "get_balance") == 0) snprintf(url, sizeof(url), "https://api.stripe.com/v1/balance");
    else if (strcmp(action, "list_invoices") == 0) snprintf(url, sizeof(url), "https://api.stripe.com/v1/invoices?limit=10");
    else { snprintf(result, rlen, "unknown action: %s", action); free(action); return false; }
    free(action);

    char auth[512]; snprintf(auth, sizeof(auth), "Authorization: Bearer %s", api_key);
    http_buf_t resp = {0};
    long status = http_get_authed(url, auth, &resp);
    if (status != 200) { snprintf(result, rlen, "Stripe error (HTTP %ld)", status); free(resp.data); return false; }
    result[0] = '\0'; truncate_response(resp.data ? resp.data : "{}", result, rlen, 32);
    free(resp.data); return true;
}

/* ══════════════════════════════════════════════════════════════════════════
 *  SUPABASE — Postgres + Auth
 * ══════════════════════════════════════════════════════════════════════════ */

bool tool_supabase_query(const char *input, char *result, size_t rlen) {
    const char *api_key;
    if (!require_key("SUPABASE_API_KEY", "Supabase", result, rlen, &api_key)) return false;
    const char *supabase_url = getenv("SUPABASE_URL");
    if (!supabase_url) { snprintf(result, rlen, "SUPABASE_URL not set"); return false; }

    char *table = json_get_str(input, "table");
    char *select = json_get_str(input, "select");
    if (!table || !table[0]) { free(table); free(select); snprintf(result, rlen, "missing: table"); return false; }

    char url[2048];
    snprintf(url, sizeof(url), "%s/rest/v1/%s?select=%s", supabase_url, table, (select && select[0]) ? select : "*");
    char auth[512]; snprintf(auth, sizeof(auth), "Authorization: Bearer %s", api_key);
    char apikey_hdr[512]; snprintf(apikey_hdr, sizeof(apikey_hdr), "apikey: %s", api_key);

    CURL *curl = curl_easy_init();
    http_buf_t resp = {0};
    resp.data = malloc(8192); resp.len = 0; resp.cap = 8192; resp.data[0] = '\0';
    struct curl_slist *hdrs = NULL;
    hdrs = curl_slist_append(hdrs, "Accept: application/json");
    hdrs = curl_slist_append(hdrs, auth);
    hdrs = curl_slist_append(hdrs, apikey_hdr);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, http_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    CURLcode res = curl_easy_perform(curl);
    long status = 0;
    if (res == CURLE_OK) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(hdrs); curl_easy_cleanup(curl);
    free(table); free(select);

    if (status != 200) { snprintf(result, rlen, "Supabase error (HTTP %ld)", status); free(resp.data); return false; }
    result[0] = '\0'; truncate_response(resp.data ? resp.data : "[]", result, rlen, 32);
    free(resp.data); return true;
}

/* ══════════════════════════════════════════════════════════════════════════
 *  HUGGING FACE — Model inference
 * ══════════════════════════════════════════════════════════════════════════ */

bool tool_huggingface(const char *input, char *result, size_t rlen) {
    const char *api_key;
    if (!require_key("HF_TOKEN", "Hugging Face", result, rlen, &api_key)) return false;

    char *model = json_get_str(input, "model");
    char *text = json_get_str(input, "text");
    if (!model || !model[0] || !text || !text[0]) { free(model); free(text); snprintf(result, rlen, "missing: model, text"); return false; }

    char url[512]; snprintf(url, sizeof(url), "https://api-inference.huggingface.co/models/%s", model);
    jbuf_t body; jbuf_init(&body, 256);
    jbuf_append(&body, "{\"inputs\":"); jbuf_append_json_str(&body, text); jbuf_append(&body, "}");

    char auth[512]; snprintf(auth, sizeof(auth), "Authorization: Bearer %s", api_key);
    const char *headers[] = { auth };
    http_buf_t resp = {0};
    long status = http_json_request("POST", url, body.data, headers, 1, &resp);
    jbuf_free(&body); free(model); free(text);

    if (status != 200) { snprintf(result, rlen, "HuggingFace error (HTTP %ld)", status); free(resp.data); return false; }
    result[0] = '\0'; truncate_response(resp.data ? resp.data : "{}", result, rlen, 32);
    free(resp.data); return true;
}

/* ══════════════════════════════════════════════════════════════════════════
 *  GITHUB ACTIONS — Workflow runs
 * ══════════════════════════════════════════════════════════════════════════ */

bool tool_github_actions(const char *input, char *result, size_t rlen) {
    char *repo = json_get_str(input, "repo");
    char *action = json_get_str(input, "action");
    if (!repo || !repo[0]) { free(repo); free(action); snprintf(result, rlen, "missing: repo"); return false; }
    const char *act = (action && action[0]) ? action : "list_runs";

    char url[512];
    if (strcmp(act, "list_runs") == 0) snprintf(url, sizeof(url), "https://api.github.com/repos/%s/actions/runs?per_page=5", repo);
    else if (strcmp(act, "list_workflows") == 0) snprintf(url, sizeof(url), "https://api.github.com/repos/%s/actions/workflows", repo);
    else { snprintf(result, rlen, "unknown action: %s", act); free(repo); free(action); return false; }
    free(repo); free(action);

    const char *token = getenv("GITHUB_TOKEN");
    if (!token) { snprintf(result, rlen, "GITHUB_TOKEN not set"); return false; }
    char auth[512]; snprintf(auth, sizeof(auth), "Authorization: Bearer %s", token);
    http_buf_t resp = {0};
    long status = http_get_authed(url, auth, &resp);
    if (status != 200) { snprintf(result, rlen, "GitHub Actions error (HTTP %ld)", status); free(resp.data); return false; }
    result[0] = '\0'; truncate_response(resp.data ? resp.data : "{}", result, rlen, 32);
    free(resp.data); return true;
}

/* ══════════════════════════════════════════════════════════════════════════
 *  MAPBOX — Geocoding
 * ══════════════════════════════════════════════════════════════════════════ */

bool tool_mapbox_geocode(const char *input, char *result, size_t rlen) {
    const char *api_key;
    if (!require_key("MAPBOX_API_KEY", "Mapbox", result, rlen, &api_key)) return false;

    char *query = json_get_str(input, "query");
    if (!query || !query[0]) { free(query); snprintf(result, rlen, "missing: query"); return false; }

    CURL *curl = curl_easy_init();
    char *enc = curl_easy_escape(curl, query, 0);
    char url[2048]; snprintf(url, sizeof(url), "https://api.mapbox.com/geocoding/v5/mapbox.places/%s.json?access_token=%s&limit=3", enc, api_key);
    curl_free(enc); curl_easy_cleanup(curl); free(query);

    http_buf_t resp = {0};
    long status = http_get_authed(url, NULL, &resp);
    if (status != 200) { snprintf(result, rlen, "Mapbox error (HTTP %ld)", status); free(resp.data); return false; }
    result[0] = '\0'; truncate_response(resp.data ? resp.data : "{}", result, rlen, 32);
    free(resp.data); return true;
}
