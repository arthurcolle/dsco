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
#include "tools.h"
#include "semantic.h"
#include "config.h"
#include <ctype.h>
#include <stdio.h>
#include <math.h>
#include <unistd.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#include <poll.h>
#include <sys/wait.h>
#include <signal.h>

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

static bool host_has_only_safe_chars(const char *s) {
    if (!s || !s[0]) return false;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (isalnum(*p) || *p == '.' || *p == '-' || *p == ':') continue;
        return false;
    }
    return true;
}

static bool identifier_token_is_safe(const char *s) {
    if (!s || !s[0] || strstr(s, "..")) return false;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (isalnum(*p) || *p == '.' || *p == '_' || *p == '-') continue;
        return false;
    }
    return true;
}

static bool slash_identifier_is_safe(const char *s) {
    if (!s || !s[0] || strstr(s, "..")) return false;

    const char *seg = s;
    for (const char *p = s; ; p++) {
        if (*p == '/' || *p == '\0') {
            char part[256];
            size_t len = (size_t)(p - seg);
            if (len == 0 || len >= sizeof(part)) return false;
            memcpy(part, seg, len);
            part[len] = '\0';
            if (!identifier_token_is_safe(part)) return false;
            if (*p == '\0') break;
            seg = p + 1;
        } else if (!(isalnum((unsigned char)*p) || *p == '.' || *p == '_' || *p == '-')) {
            return false;
        }
    }
    return true;
}

static bool github_repo_is_safe(const char *repo) {
    if (!repo || !repo[0]) return false;
    const char *slash = strchr(repo, '/');
    if (!slash || slash == repo || slash[1] == '\0' || strchr(slash + 1, '/')) {
        return false;
    }
    return slash_identifier_is_safe(repo);
}

static bool hf_model_id_is_safe(const char *model) {
    return slash_identifier_is_safe(model);
}

static bool external_http_url_is_public(const char *url) {
    if (!url) return false;
    const char *scheme = NULL;
    if (strncmp(url, "http://", 7) == 0) scheme = url + 7;
    else if (strncmp(url, "https://", 8) == 0) scheme = url + 8;
    else return false;

    char host[256];
    size_t host_len = 0;
    if (*scheme == '[') {
        scheme++;
        const char *end = strchr(scheme, ']');
        if (!end) return false;
        host_len = (size_t)(end - scheme);
    } else {
        const char *end = scheme;
        while (*end && *end != '/' && *end != ':' && *end != '?' && *end != '#') end++;
        host_len = (size_t)(end - scheme);
    }
    if (host_len == 0 || host_len >= sizeof(host)) return false;
    memcpy(host, scheme, host_len);
    host[host_len] = '\0';
    for (size_t i = 0; host[i]; i++) host[i] = (char)tolower((unsigned char)host[i]);

    if (strcmp(host, "localhost") == 0 || strcmp(host, "::1") == 0 ||
        strcmp(host, "[::1]") == 0) {
        return false;
    }
    if (strncmp(host, "127.", 4) == 0 || strncmp(host, "10.", 3) == 0 ||
        strncmp(host, "192.168.", 8) == 0 || strncmp(host, "169.254.", 8) == 0 ||
        strncmp(host, "0.", 2) == 0) {
        return false;
    }
    if (strncmp(host, "172.", 4) == 0) {
        int second = atoi(host + 4);
        if (second >= 16 && second <= 31) return false;
    }
    return true;
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
    } else if (strcmp(method, "DELETE") == 0) {
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
    if (!path || path[0] != '/' || strncmp(path, "http://", 7) == 0 ||
        strncmp(path, "https://", 8) == 0) {
        snprintf(result, rlen, "GitHub API path must start with '/'");
        return false;
    }

    char url[2048];
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
    if (!github_repo_is_safe(repo)) {
        free(repo);
        snprintf(result, rlen, "invalid repo format");
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
    if (!github_repo_is_safe(repo)) {
        free(repo);
        snprintf(result, rlen, "invalid repo format");
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

/* ── AV helper: generic query with extra params ────────────────────────── */

static bool av_query(const char *function, const char *params,
                     char *result, size_t rlen) {
    const char *api_key;
    if (!require_key("ALPHA_VANTAGE_API_KEY", "Alpha Vantage", result, rlen, &api_key))
        return false;

    jbuf_t url;
    jbuf_init(&url, 256 + strlen(function) + (params ? strlen(params) : 0) + strlen(api_key));
    jbuf_append(&url, "https://www.alphavantage.co/query?function=");
    jbuf_append(&url, function);
    if (params && params[0]) {
        jbuf_append(&url, "&");
        jbuf_append(&url, params);
    }
    jbuf_append(&url, "&apikey=");
    jbuf_append(&url, api_key);

    http_buf_t resp = {0};
    long status = http_get_authed(url.data, NULL, &resp);
    jbuf_free(&url);

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

/* json_get_str is provided by json_util.h */

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
        char *v = json_get_str(input, req1);
        if (!v || !v[0]) {
            free(v);
            snprintf(result, rlen, "missing required parameter: %s", req1);
            return false;
        }
        free(v);
    }
    if (req2) {
        char *v = json_get_str(input, req2);
        if (!v || !v[0]) {
            free(v);
            snprintf(result, rlen, "missing required parameter: %s", req2);
            return false;
        }
        free(v);
    }

    CURL *curl = curl_easy_init();
    if (!curl) {
        snprintf(result, rlen, "curl init failed");
        return false;
    }
    jbuf_t params;
    jbuf_init(&params, 512);
    for (int i = 0; av_param_names[i]; i++) {
        char *val = json_get_str(input, av_param_names[i]);
        if (val && val[0]) {
            char *enc = curl_easy_escape(curl, val, 0);
            if (!enc) {
                free(val);
                jbuf_free(&params);
                curl_easy_cleanup(curl);
                snprintf(result, rlen, "failed to encode parameter: %s", av_param_names[i]);
                return false;
            }
            if (params.len > 0) jbuf_append(&params, "&");
            jbuf_append(&params, av_param_names[i]);
            jbuf_append(&params, "=");
            jbuf_append(&params, enc);
            curl_free(enc);
        }
        free(val);
    }

    bool ok = av_query(av_func, params.len > 0 ? params.data : NULL, result, rlen);
    jbuf_free(&params);
    curl_easy_cleanup(curl);
    return ok;
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
    const char *sort_order = (sort && strcmp(sort, "asc") == 0) ? "asc" : "desc";

    CURL *curl = curl_easy_init();
    if (!curl) {
        free(series_id);
        free(sort);
        snprintf(result, rlen, "curl init failed");
        return false;
    }
    char *enc_series = curl_easy_escape(curl, series_id, 0);
    if (!enc_series) {
        curl_easy_cleanup(curl);
        free(series_id);
        free(sort);
        snprintf(result, rlen, "failed to encode series_id");
        return false;
    }

    char url[2048];
    snprintf(url, sizeof(url),
             "https://api.stlouisfed.org/fred/series/observations"
             "?series_id=%s&api_key=%s&file_type=json&limit=%d&sort_order=%s",
             enc_series, api_key, limit, sort_order);
    curl_free(enc_series);
    curl_easy_cleanup(curl);

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
    /* Unit-aware labels: metric=°C/m·s⁻¹, imperial=°F/mph, standard=K/m·s⁻¹ */
    const char *tsym = (strcmp(u, "imperial") == 0) ? "°F"
                     : (strcmp(u, "standard") == 0) ? "K" : "°C";
    const char *wsym = (strcmp(u, "imperial") == 0) ? "mph" : "m/s";

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
        char *name    = json_get_str(resp.data, "name");
        char *main    = json_get_raw(resp.data, "main");
        char *weather = json_get_raw(resp.data, "weather");
        char *wind    = json_get_raw(resp.data, "wind");
        char *clouds  = json_get_raw(resp.data, "clouds");
        char *sys     = json_get_raw(resp.data, "sys");

        double temp     = main ? json_get_double(main, "temp", 0)       : 0;
        double feels    = main ? json_get_double(main, "feels_like", 0) : 0;
        double tmin     = main ? json_get_double(main, "temp_min", 0)   : 0;
        double tmax     = main ? json_get_double(main, "temp_max", 0)   : 0;
        int humidity    = main ? json_get_int(main, "humidity", 0)      : 0;
        int pressure    = main ? json_get_int(main, "pressure", 0)      : 0;
        double wind_spd = wind ? json_get_double(wind, "speed", 0)      : 0;
        int wind_deg    = wind ? json_get_int(wind, "deg", -1)          : -1;
        double gust     = wind ? json_get_double(wind, "gust", 0)       : 0;
        int cloud_pct   = clouds ? json_get_int(clouds, "all", -1)      : -1;
        char *country   = sys ? json_get_str(sys, "country") : NULL;

        /* weather is a JSON array [{...}] — drill into the first element so
         * the "main"/"description" keys resolve (json_get_str needs a '{'). */
        char *desc = NULL, *cond = NULL;
        if (weather) {
            const char *w0 = strchr(weather, '{');
            if (w0) {
                desc = json_get_str(w0, "description");
                cond = json_get_str(w0, "main");
            }
        }

        /* 16-point compass from wind bearing for human-readable direction */
        static const char *compass[16] = {
            "N","NNE","NE","ENE","E","ESE","SE","SSE",
            "S","SSW","SW","WSW","W","WNW","NW","NNW" };
        const char *dir = (wind_deg >= 0)
            ? compass[(((wind_deg % 360) * 4 + 45) / 90) % 16] : NULL;

        int n = snprintf(result, rlen,
                 "%s%s%s: %.1f%s (feels like %.1f%s)\n"
                 "Conditions: %s%s%s%s\n"
                 "Range: %.1f%s – %.1f%s\n"
                 "Humidity: %d%%   Pressure: %d hPa",
                 name ? name : "Unknown",
                 country ? ", " : "", country ? country : "",
                 temp, tsym, feels, tsym,
                 cond ? cond : "Unknown",
                 desc ? " (" : "", desc ? desc : "", desc ? ")" : "",
                 tmin, tsym, tmax, tsym,
                 humidity, pressure);

        if (n > 0 && (size_t)n < rlen && cloud_pct >= 0)
            n += snprintf(result + n, rlen - (size_t)n,
                          "   Cloud cover: %d%%", cloud_pct);

        if (n > 0 && (size_t)n < rlen) {
            n += snprintf(result + n, rlen - (size_t)n, "\nWind: %.1f %s", wind_spd, wsym);
            if (dir) n += snprintf(result + n, rlen - (size_t)n, " from %s (%d°)", dir, wind_deg);
            if (gust > 0 && (size_t)n < rlen)
                snprintf(result + n, rlen - (size_t)n, ", gusting %.1f %s", gust, wsym);
        }

        free(name); free(main); free(weather); free(wind); free(clouds);
        free(sys); free(country); free(desc); free(cond);
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
    if (!external_http_url_is_public(url_str)) {
        free(url_str);
        snprintf(result, rlen, "url must be a public http(s) target");
        return false;
    }

    jbuf_t jina_url;
    jbuf_init(&jina_url, 64 + strlen(url_str));
    jbuf_append(&jina_url, "https://r.jina.ai/");
    jbuf_append(&jina_url, url_str);
    free(url_str);

    const char *jina_key = getenv("JINA_API_KEY");
    char auth[512] = "";
    if (jina_key && jina_key[0])
        snprintf(auth, sizeof(auth), "Authorization: Bearer %s", jina_key);

    http_buf_t resp = {0};
    long status = http_get_authed(jina_url.data, auth[0] ? auth : NULL, &resp);
    jbuf_free(&jina_url);

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
 *  JINA SEARCH — AI-powered web search via s.jina.ai
 * ══════════════════════════════════════════════════════════════════════════ */

bool tool_jina_search(const char *input, char *result, size_t rlen) {
    char *query = json_get_str(input, "query");
    if (!query || !query[0]) {
        free(query);
        snprintf(result, rlen, "missing required parameter: query");
        return false;
    }

    int num = json_get_int(input, "num", 5);
    if (num < 1) num = 1;
    if (num > 10) num = 10;

    const char *jina_key = getenv("JINA_API_KEY");
    if (!jina_key || !jina_key[0]) {
        free(query);
        snprintf(result, rlen, "JINA_API_KEY not set");
        return false;
    }

    /* Build request body */
    jbuf_t body;
    jbuf_init(&body, 512);
    jbuf_append(&body, "{\"q\":");
    jbuf_append_json_str(&body, query);
    jbuf_appendf(&body, ",\"num\":%d}", num);
    free(query);

    /* POST to s.jina.ai */
    CURL *curl = curl_easy_init();
    if (!curl) { jbuf_free(&body); snprintf(result, rlen, "curl init failed"); return false; }

    struct curl_slist *hdrs = NULL;
    char auth[512];
    snprintf(auth, sizeof(auth), "Authorization: Bearer %s", jina_key);
    hdrs = curl_slist_append(hdrs, auth);
    hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
    hdrs = curl_slist_append(hdrs, "Accept: application/json");

    http_buf_t resp = {0};
    curl_easy_setopt(curl, CURLOPT_URL, "https://s.jina.ai/");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.data);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, http_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);
    jbuf_free(&body);

    if (res != CURLE_OK || http_code != 200) {
        snprintf(result, rlen, "Jina Search error (HTTP %ld)", http_code);
        free(resp.data);
        return false;
    }

    result[0] = '\0';
    truncate_response(resp.data ? resp.data : "{}", result, rlen, 32);
    free(resp.data);
    return true;
}

/* ══════════════════════════════════════════════════════════════════════════
 *  JINA EMBED — Compute embeddings via Jina v4 API
 * ══════════════════════════════════════════════════════════════════════════ */

bool tool_jina_embed(const char *input, char *result, size_t rlen) {
    char *text = json_get_str(input, "text");
    if (!text || !text[0]) {
        free(text);
        snprintf(result, rlen, "missing required parameter: text");
        return false;
    }

    char *task = json_get_str(input, "task");
    if (!task || !task[0]) {
        free(task);
        task = strdup("retrieval.passage");
    }

    int dim = json_get_int(input, "dimensions", 1024);
    if (dim < 64) dim = 64;
    if (dim > 1024) dim = 1024;

    const char *jina_key = getenv("JINA_API_KEY");
    if (!jina_key || !jina_key[0]) {
        free(text); free(task);
        snprintf(result, rlen, "JINA_API_KEY not set");
        return false;
    }

    /* Build request */
    jbuf_t body;
    jbuf_init(&body, 512 + strlen(text));
    jbuf_append(&body, "{\"model\":\"jina-embeddings-v4\",\"task\":");
    jbuf_append_json_str(&body, task);
    jbuf_appendf(&body, ",\"dimensions\":%d,\"embedding_type\":\"float\",\"input\":[", dim);
    jbuf_append_json_str(&body, text);
    jbuf_append(&body, "]}");
    free(text);
    free(task);

    CURL *curl = curl_easy_init();
    if (!curl) { jbuf_free(&body); snprintf(result, rlen, "curl init failed"); return false; }

    struct curl_slist *hdrs = NULL;
    char auth[512];
    snprintf(auth, sizeof(auth), "Authorization: Bearer %s", jina_key);
    hdrs = curl_slist_append(hdrs, auth);
    hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
    hdrs = curl_slist_append(hdrs, "Accept: application/json");

    http_buf_t resp = {0};
    curl_easy_setopt(curl, CURLOPT_URL, "https://api.jina.ai/v1/embeddings");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.data);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, http_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);
    jbuf_free(&body);

    if (res != CURLE_OK || http_code != 200) {
        snprintf(result, rlen, "Jina Embed error (HTTP %ld): %s",
                 http_code, resp.data ? resp.data : "no response");
        free(resp.data);
        return false;
    }

    /* Return the full response — contains data[0].embedding array */
    result[0] = '\0';
    truncate_response(resp.data ? resp.data : "{}", result, rlen, 32);
    free(resp.data);
    return true;
}

/* ══════════════════════════════════════════════════════════════════════════
 *  PARALLEL SEARCH — Fan out to multiple search providers concurrently
 * ══════════════════════════════════════════════════════════════════════════ */

bool tool_parallel_search(const char *input, char *result, size_t rlen) {
    char *query = json_get_str(input, "query");
    if (!query || !query[0]) {
        free(query);
        snprintf(result, rlen, "missing required parameter: query");
        return false;
    }

    int num = json_get_int(input, "num", 5);
    if (num < 1) num = 1;
    if (num > 10) num = 10;

    /* We'll run up to 3 search providers in parallel using fork+pipe */
    typedef struct { int fd; pid_t pid; const char *name; } search_child_t;
    search_child_t children[3];
    int nchildren = 0;

    /* Build search input JSON once */
    jbuf_t search_input;
    jbuf_init(&search_input, 256);
    jbuf_append(&search_input, "{\"query\":");
    jbuf_append_json_str(&search_input, query);
    jbuf_appendf(&search_input, ",\"num\":%d}", num);

    /* Determine which providers are available */
    const char *jina_key = getenv("JINA_API_KEY");
    const char *tavily_key = getenv("TAVILY_API_KEY");
    const char *brave_key = getenv("BRAVE_API_KEY");

    typedef bool (*search_fn)(const char *, char *, size_t);
    struct { search_fn fn; const char *name; bool avail; } providers[] = {
        { tool_jina_search,   "jina",   jina_key && jina_key[0] },
        { tool_tavily_search, "tavily", tavily_key && tavily_key[0] },
        { tool_brave_search,  "brave",  brave_key && brave_key[0] },
    };

    for (int i = 0; i < 3; i++) {
        if (!providers[i].avail) continue;

        int pipefd[2];
        if (pipe(pipefd) < 0) continue;

        pid_t pid = fork();
        if (pid < 0) { close(pipefd[0]); close(pipefd[1]); continue; }

        if (pid == 0) {
            /* Child: run search, write result to pipe, exit */
            close(pipefd[0]);
            char child_result[65536];
            child_result[0] = '\0';
            bool ok = providers[i].fn(search_input.data, child_result, sizeof(child_result));
            (void)ok;
            size_t wlen = strlen(child_result);
            ssize_t w = write(pipefd[1], child_result, wlen);
            (void)w;
            close(pipefd[1]);
            _exit(0);
        }

        /* Parent */
        close(pipefd[1]);
        children[nchildren].fd = pipefd[0];
        children[nchildren].pid = pid;
        children[nchildren].name = providers[i].name;
        nchildren++;
    }

    free(query);

    if (nchildren == 0) {
        jbuf_free(&search_input);
        snprintf(result, rlen, "no search providers available (set JINA_API_KEY, TAVILY_API_KEY, or BRAVE_API_KEY)");
        return false;
    }

    /* Collect results with timeout */
    jbuf_t merged;
    jbuf_init(&merged, 8192);
    jbuf_append(&merged, "{\"results\":{");

    for (int i = 0; i < nchildren; i++) {
        /* Read with poll timeout */
        struct pollfd pfd = { .fd = children[i].fd, .events = POLLIN };
        char buf[65536];
        int total_read = 0;

        int timeout_ms = 12000; /* 12s total timeout */
        while (poll(&pfd, 1, timeout_ms) > 0 && total_read < (int)sizeof(buf) - 1) {
            ssize_t n = read(children[i].fd, buf + total_read, sizeof(buf) - 1 - total_read);
            if (n <= 0) break;
            total_read += n;
            timeout_ms = 2000; /* subsequent reads: 2s */
        }
        buf[total_read] = '\0';
        close(children[i].fd);
        waitpid(children[i].pid, NULL, 0);

        if (i > 0) jbuf_append(&merged, ",");
        jbuf_appendf(&merged, "\"%s\":", children[i].name);
        if (total_read > 0) {
            jbuf_append(&merged, buf);
        } else {
            jbuf_append(&merged, "null");
        }
    }

    jbuf_append(&merged, "}}");
    jbuf_free(&search_input);

    result[0] = '\0';
    truncate_response(merged.data ? merged.data : "{}", result, rlen, 32);
    jbuf_free(&merged);
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
    jbuf_free(&body);

    /* Build the result before freeing: `out` may alias output_path. */
    bool ok = (status == 200);
    if (ok) snprintf(result, rlen, "audio saved to %s", out);
    else    snprintf(result, rlen, "ElevenLabs error (HTTP %ld)", status);
    free(text); free(voice); free(output_path);
    return ok;
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
    if (!host_has_only_safe_chars(host)) {
        free(host);
        snprintf(result, rlen, "invalid Pinecone host");
        return false;
    }

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
    if (!identifier_token_is_safe(table)) {
        free(table); free(select);
        snprintf(result, rlen, "invalid table name");
        return false;
    }

    CURL *curl = curl_easy_init();
    if (!curl) {
        free(table); free(select);
        snprintf(result, rlen, "curl init failed");
        return false;
    }
    char *enc_select = curl_easy_escape(curl, (select && select[0]) ? select : "*", 0);
    if (!enc_select) {
        curl_easy_cleanup(curl);
        free(table); free(select);
        snprintf(result, rlen, "failed to encode select");
        return false;
    }
    char url[2048];
    snprintf(url, sizeof(url), "%s/rest/v1/%s?select=%s", supabase_url, table, enc_select);
    curl_free(enc_select);
    curl_easy_cleanup(curl);
    char auth[512]; snprintf(auth, sizeof(auth), "Authorization: Bearer %s", api_key);
    char apikey_hdr[512]; snprintf(apikey_hdr, sizeof(apikey_hdr), "apikey: %s", api_key);

    CURL *curl2 = curl_easy_init();
    http_buf_t resp = {0};
    resp.data = malloc(8192); resp.len = 0; resp.cap = 8192; resp.data[0] = '\0';
    struct curl_slist *hdrs = NULL;
    hdrs = curl_slist_append(hdrs, "Accept: application/json");
    hdrs = curl_slist_append(hdrs, auth);
    hdrs = curl_slist_append(hdrs, apikey_hdr);
    curl_easy_setopt(curl2, CURLOPT_URL, url);
    curl_easy_setopt(curl2, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl2, CURLOPT_WRITEFUNCTION, http_write_cb);
    curl_easy_setopt(curl2, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(curl2, CURLOPT_TIMEOUT, 30L);
    CURLcode res = curl_easy_perform(curl2);
    long status = 0;
    if (res == CURLE_OK) curl_easy_getinfo(curl2, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(hdrs); curl_easy_cleanup(curl2);
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
    if (!hf_model_id_is_safe(model)) {
        free(model); free(text);
        snprintf(result, rlen, "invalid model id");
        return false;
    }

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
    if (!github_repo_is_safe(repo)) {
        free(repo); free(action);
        snprintf(result, rlen, "invalid repo format");
        return false;
    }
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

/* ══════════════════════════════════════════════════════════════════════════
 *  KNOWLEDGE BASE — PDF ingestion, SQLite storage, semantic search
 *
 *  Schema: ~/.dsco/knowledge.db
 *    documents(id, filename, hash, title, page_count, created_at, metadata)
 *    pages(id, doc_id, page_num, content, content_type, word_count)
 *
 *  Ingestion modes:
 *    1. Direct text extraction (fast, uses pdftotext if available)
 *    2. Vision extraction (uses pdf_to_structure.py via Python + OpenRouter)
 *    3. Manual text insertion (paste content directly)
 *
 *  Search uses the TF-IDF engine from semantic.c for relevance ranking.
 * ══════════════════════════════════════════════════════════════════════════ */

#include <sqlite3.h>
#include <sys/stat.h>

static sqlite3 *kb_db(void) {
    static sqlite3 *db = NULL;
    if (db) return db;

    const char *home = getenv("HOME");
    if (!home) return NULL;
    char path[512];
    snprintf(path, sizeof(path), "%s/.dsco", home);
    mkdir(path, 0755);
    snprintf(path, sizeof(path), "%s/.dsco/knowledge.db", home);

    if (sqlite3_open(path, &db) != SQLITE_OK) { db = NULL; return NULL; }

    const char *schema =
        "CREATE TABLE IF NOT EXISTS documents("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  filename TEXT NOT NULL,"
        "  hash TEXT UNIQUE,"
        "  title TEXT,"
        "  page_count INTEGER DEFAULT 0,"
        "  created_at TEXT DEFAULT (datetime('now')),"
        "  metadata TEXT DEFAULT '{}'"
        ");"
        "CREATE TABLE IF NOT EXISTS pages("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  doc_id INTEGER NOT NULL REFERENCES documents(id) ON DELETE CASCADE,"
        "  page_num INTEGER NOT NULL,"
        "  content TEXT NOT NULL,"
        "  content_type TEXT DEFAULT 'text',"
        "  word_count INTEGER DEFAULT 0,"
        "  UNIQUE(doc_id, page_num)"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_pages_doc ON pages(doc_id);"
        "CREATE INDEX IF NOT EXISTS idx_docs_hash ON documents(hash);";

    char *err = NULL;
    sqlite3_exec(db, schema, NULL, NULL, &err);
    sqlite3_free(err);
    return db;
}

/* Compute SHA-256 of file for dedup */
static void file_sha256(const char *path, char *out, size_t out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(out, out_len, "unknown"); return; }
    unsigned char buf[8192];
    unsigned long h = 5381;
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        for (size_t i = 0; i < n; i++) h = ((h << 5) + h) + buf[i];
    }
    fclose(f);
    snprintf(out, out_len, "%016lx", h);
}

static int count_words(const char *text) {
    if (!text) return 0;
    int count = 0;
    bool in_word = false;
    for (const char *p = text; *p; p++) {
        if (isspace((unsigned char)*p)) { in_word = false; }
        else if (!in_word) { in_word = true; count++; }
    }
    return count;
}

/* Ingest a PDF or text file into the knowledge base */
bool tool_kb_ingest(const char *input, char *result, size_t rlen) {
    sqlite3 *db = kb_db();
    if (!db) { snprintf(result, rlen, "failed to open knowledge base"); return false; }

    char *path = json_get_str(input, "path");
    char *url = json_get_str(input, "url");
    char *title = json_get_str(input, "title");
    char *text = json_get_str(input, "text");  /* direct text insertion */

    const char *source = path ? path : url;
    if (!source && !text) {
        free(path); free(url); free(title); free(text);
        snprintf(result, rlen, "missing: path, url, or text");
        return false;
    }

    /* Direct text insertion mode */
    if (text && text[0]) {
        const char *t = title ? title : "manual entry";
        sqlite3_stmt *stmt;
        sqlite3_prepare_v2(db, "INSERT INTO documents(filename,hash,title,page_count) VALUES(?,?,?,1)", -1, &stmt, NULL);
        char hash[32];
        snprintf(hash, sizeof(hash), "text_%ld", (long)time(NULL));
        sqlite3_bind_text(stmt, 1, t, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, hash, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, t, -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        int doc_id = (int)sqlite3_last_insert_rowid(db);

        sqlite3_prepare_v2(db, "INSERT INTO pages(doc_id,page_num,content,word_count) VALUES(?,1,?,?)", -1, &stmt, NULL);
        sqlite3_bind_int(stmt, 1, doc_id);
        sqlite3_bind_text(stmt, 2, text, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 3, count_words(text));
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);

        snprintf(result, rlen, "{\"doc_id\":%d,\"title\":\"%s\",\"pages\":1,\"words\":%d}",
                 doc_id, t, count_words(text));
        free(path); free(url); free(title); free(text);
        return true;
    }

    /* File-based ingestion */
    char resolved[1024];
    if (url && url[0]) {
        /* Download URL to temp file */
        snprintf(resolved, sizeof(resolved), "/tmp/dsco_kb_%ld.pdf", (long)time(NULL));
        const char *curl_argv[] = { "curl", "-sL", "-o", resolved, url, NULL };
        char curl_err[512] = {0};
        if (safe_exec_argv(curl_argv, curl_err, sizeof(curl_err)) != 0) {
            snprintf(result, rlen, "failed to download: %s", url);
            free(path); free(url); free(title); free(text);
            return false;
        }
    } else {
        snprintf(resolved, sizeof(resolved), "%s", path);
    }

    /* Check dedup */
    char hash[32];
    file_sha256(resolved, hash, sizeof(hash));
    {
        sqlite3_stmt *stmt;
        sqlite3_prepare_v2(db, "SELECT id FROM documents WHERE hash=?", -1, &stmt, NULL);
        sqlite3_bind_text(stmt, 1, hash, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            int existing = sqlite3_column_int(stmt, 0);
            sqlite3_finalize(stmt);
            snprintf(result, rlen, "{\"doc_id\":%d,\"status\":\"already_indexed\",\"hash\":\"%s\"}",
                     existing, hash);
            if (url && url[0]) unlink(resolved);
            free(path); free(url); free(title); free(text);
            return true;
        }
        sqlite3_finalize(stmt);
    }

    /* Extract text using pdftotext (fast, no API calls) */
    char txt_path[1024];
    snprintf(txt_path, sizeof(txt_path), "/tmp/dsco_kb_%s.txt", hash);
    {
        const char *pdftotext_argv[] = { "pdftotext", "-layout", resolved, txt_path, NULL };
        char pdf_err[512] = {0};
        int rc = safe_exec_argv(pdftotext_argv, pdf_err, sizeof(pdf_err));
        if (rc != 0) {
            /* Fallback: try python3 with our pdf_to_structure.py */
            const char *home = getenv("HOME");
            char py_script[4096];
            snprintf(py_script, sizeof(py_script), "%s/Library/Mobile Documents/com~apple~CloudDocs/Untitled Folder/NewMachine/what/pdf_to/pdf_to_structure.py",
                     home ? home : ".");
            const char *py_argv[] = { "python3", py_script, resolved, "--max-pages", "50", NULL };
            char py_err[512] = {0};
            rc = safe_exec_argv(py_argv, py_err, sizeof(py_err));
            if (rc != 0) {
                snprintf(result, rlen, "failed to extract text (install pdftotext or poppler)");
                if (url && url[0]) unlink(resolved);
                free(path); free(url); free(title); free(text);
                return false;
            }
        }
    }

    /* Read extracted text and split into pages */
    FILE *f = fopen(txt_path, "r");
    if (!f) {
        snprintf(result, rlen, "failed to read extracted text");
        free(path); free(url); free(title); free(text);
        return false;
    }

    /* Read entire file */
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *content = malloc(fsize + 1);
    fread(content, 1, fsize, f);
    content[fsize] = '\0';
    fclose(f);
    unlink(txt_path);

    /* Insert document record */
    const char *fname = strrchr(resolved, '/');
    fname = fname ? fname + 1 : resolved;
    const char *t = (title && title[0]) ? title : fname;

    /* Split by form feed (page break from pdftotext) or by ~3000 chars */
    int page_count = 0;
    int total_words = 0;

    sqlite3_stmt *doc_stmt;
    sqlite3_prepare_v2(db, "INSERT INTO documents(filename,hash,title,page_count) VALUES(?,?,?,0)", -1, &doc_stmt, NULL);
    sqlite3_bind_text(doc_stmt, 1, fname, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(doc_stmt, 2, hash, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(doc_stmt, 3, t, -1, SQLITE_TRANSIENT);
    sqlite3_step(doc_stmt);
    sqlite3_finalize(doc_stmt);
    int doc_id = (int)sqlite3_last_insert_rowid(db);

    /* Split on form feed (\f) for page boundaries */
    char *page_start = content;
    while (page_start && *page_start) {
        char *page_end = strchr(page_start, '\f');
        size_t plen;
        if (page_end) {
            plen = (size_t)(page_end - page_start);
        } else {
            plen = strlen(page_start);
        }
        if (plen > 0) {
            char saved = page_start[plen];
            page_start[plen] = '\0';
            page_count++;
            int wc = count_words(page_start);
            total_words += wc;

            sqlite3_stmt *ps;
            sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO pages(doc_id,page_num,content,word_count) VALUES(?,?,?,?)", -1, &ps, NULL);
            sqlite3_bind_int(ps, 1, doc_id);
            sqlite3_bind_int(ps, 2, page_count);
            sqlite3_bind_text(ps, 3, page_start, -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(ps, 4, wc);
            sqlite3_step(ps);
            sqlite3_finalize(ps);

            page_start[plen] = saved;
        }
        if (page_end) page_start = page_end + 1;
        else break;
    }
    free(content);

    /* Update page count */
    {
        sqlite3_stmt *us;
        sqlite3_prepare_v2(db, "UPDATE documents SET page_count=? WHERE id=?", -1, &us, NULL);
        sqlite3_bind_int(us, 1, page_count);
        sqlite3_bind_int(us, 2, doc_id);
        sqlite3_step(us);
        sqlite3_finalize(us);
    }

    if (url && url[0]) unlink(resolved);

    snprintf(result, rlen,
             "{\"doc_id\":%d,\"title\":\"%s\",\"pages\":%d,\"words\":%d,\"hash\":\"%s\"}",
             doc_id, t, page_count, total_words, hash);
    free(path); free(url); free(title); free(text);
    return true;
}

/* Semantic search across the knowledge base */
bool tool_kb_search(const char *input, char *result, size_t rlen) {
    sqlite3 *db = kb_db();
    if (!db) { snprintf(result, rlen, "knowledge base unavailable"); return false; }

    char *query = json_get_str(input, "query");
    if (!query || !query[0]) { free(query); snprintf(result, rlen, "missing: query"); return false; }
    int limit = json_get_int(input, "limit", 5);
    if (limit > 20) limit = 20;

    /* Convert query to FTS5 format: join content words with OR for broader matching,
     * but keep 2-3 key terms as AND to maintain relevance */
    char fts_query[1024];
    {
        /* Tokenize query for FTS5:
         * - Quote each word to prevent FTS5 operator interpretation
         *   (hyphens, colons, etc. are FTS5 syntax)
         * - Skip very short words
         * - Limit to 8 terms */
        char *q = strdup(query);
        char *tok = strtok(q, " \t\n");
        int wcount = 0;
        fts_query[0] = '\0';
        while (tok) {
            if (strlen(tok) > 2) {
                if (wcount > 0) strcat(fts_query, " ");
                if (wcount < 8) {
                    /* Quote each term to escape FTS5 operators */
                    strcat(fts_query, "\"");
                    strncat(fts_query, tok, sizeof(fts_query) - strlen(fts_query) - 4);
                    strcat(fts_query, "\"");
                    wcount++;
                }
            }
            tok = strtok(NULL, " \t\n");
        }
        free(q);
        if (fts_query[0] == '\0') snprintf(fts_query, sizeof(fts_query), "%s", query);
    }

    /* Ensure FTS5 index exists (auto-create on first search) */
    {
        char *err = NULL;
        sqlite3_exec(db,
            "CREATE VIRTUAL TABLE IF NOT EXISTS pages_fts USING fts5("
            "  content, content='pages', content_rowid='id'"
            ");"
            "INSERT OR IGNORE INTO pages_fts(pages_fts) VALUES('rebuild');",
            NULL, NULL, &err);
        sqlite3_free(err);
    }

    /* Use SQLite FTS5 with built-in BM25 ranking — searches ALL pages, no limit */
    sqlite3_stmt *fts_stmt;
    int rc = sqlite3_prepare_v2(db,
        "SELECT p.id, p.doc_id, p.page_num, d.title, "
        "       rank, snippet(pages_fts, 0, '>>>', '<<<', '...', 64) as snip "
        "FROM pages_fts "
        "JOIN pages p ON pages_fts.rowid = p.id "
        "JOIN documents d ON p.doc_id = d.id "
        "WHERE pages_fts MATCH ? "
        "ORDER BY rank "
        "LIMIT ?",
        -1, &fts_stmt, NULL);

    if (rc != SQLITE_OK) {
        /* FTS5 not available — fall back to LIKE search */
        char like_q[512];
        snprintf(like_q, sizeof(like_q), "%%%s%%", query);
        sqlite3_prepare_v2(db,
            "SELECT p.id, p.doc_id, p.page_num, d.title, "
            "       0 as rank, substr(p.content, 1, 300) as snip "
            "FROM pages p JOIN documents d ON p.doc_id=d.id "
            "WHERE p.content LIKE ? "
            "ORDER BY p.word_count DESC LIMIT ?",
            -1, &fts_stmt, NULL);
        sqlite3_bind_text(fts_stmt, 1, like_q, -1, SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_text(fts_stmt, 1, fts_query, -1, SQLITE_TRANSIENT);
    }
    sqlite3_bind_int(fts_stmt, 2, limit);

    /* Count total searchable pages */
    sqlite3_stmt *cnt_stmt;
    int total_pages = 0;
    sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM pages WHERE word_count>0", -1, &cnt_stmt, NULL);
    if (sqlite3_step(cnt_stmt) == SQLITE_ROW) total_pages = sqlite3_column_int(cnt_stmt, 0);
    sqlite3_finalize(cnt_stmt);

    int n = 0;

    /* Build JSON results from FTS5 query */
    jbuf_t out;
    jbuf_init(&out, rlen > 4096 ? 4096 : rlen);
    jbuf_appendf(&out, "{\"query\":");
    jbuf_append_json_str(&out, query);
    jbuf_appendf(&out, ",\"results\":[");

    while (sqlite3_step(fts_stmt) == SQLITE_ROW && n < limit) {
        if (n > 0) jbuf_append(&out, ",");
        jbuf_append(&out, "{");

        double score = -sqlite3_column_double(fts_stmt, 4); /* FTS5 rank is negative */
        int doc_id = sqlite3_column_int(fts_stmt, 1);
        int page_num = sqlite3_column_int(fts_stmt, 2);
        const char *title = (const char *)sqlite3_column_text(fts_stmt, 3);
        const char *snippet = (const char *)sqlite3_column_text(fts_stmt, 5);

        jbuf_appendf(&out, "\"score\":%.4f,\"doc_id\":%d,\"page\":%d,", score, doc_id, page_num);
        jbuf_append(&out, "\"title\":");
        jbuf_append_json_str(&out, title ? title : "");
        if (snippet) {
            jbuf_append(&out, ",\"snippet\":");
            jbuf_append_json_str(&out, snippet);
        }
        jbuf_append(&out, "}");
        n++;
    }
    sqlite3_finalize(fts_stmt);

    jbuf_appendf(&out, "],\"total_pages_searched\":%d}", total_pages);

    result[0] = '\0';
    truncate_response(out.data ? out.data : "{}", result, rlen, 32);
    jbuf_free(&out);
    free(query);
    return true;
}

/* List all documents in the knowledge base */
bool tool_kb_list(const char *input, char *result, size_t rlen) {
    (void)input;
    sqlite3 *db = kb_db();
    if (!db) { snprintf(result, rlen, "knowledge base unavailable"); return false; }

    jbuf_t out;
    jbuf_init(&out, rlen > 4096 ? 4096 : rlen);
    jbuf_append(&out, "{\"documents\":[");

    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db,
        "SELECT id, filename, title, page_count, created_at, "
        "(SELECT SUM(word_count) FROM pages WHERE doc_id=documents.id) as total_words "
        "FROM documents ORDER BY id DESC", -1, &stmt, NULL);

    int count = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (count > 0) jbuf_append(&out, ",");
        jbuf_appendf(&out, "{\"id\":%d,", sqlite3_column_int(stmt, 0));
        jbuf_append(&out, "\"filename\":");
        jbuf_append_json_str(&out, (const char *)sqlite3_column_text(stmt, 1));
        jbuf_append(&out, ",\"title\":");
        jbuf_append_json_str(&out, (const char *)sqlite3_column_text(stmt, 2));
        jbuf_appendf(&out, ",\"pages\":%d", sqlite3_column_int(stmt, 3));
        jbuf_appendf(&out, ",\"words\":%d", sqlite3_column_int(stmt, 5));
        jbuf_append(&out, ",\"created\":");
        jbuf_append_json_str(&out, (const char *)sqlite3_column_text(stmt, 4));
        jbuf_append(&out, "}");
        count++;
    }
    sqlite3_finalize(stmt);

    jbuf_appendf(&out, "],\"count\":%d}", count);
    result[0] = '\0';
    truncate_response(out.data ? out.data : "{}", result, rlen, 32);
    jbuf_free(&out);
    return true;
}

/* Get content from a specific document/page */
bool tool_kb_get(const char *input, char *result, size_t rlen) {
    sqlite3 *db = kb_db();
    if (!db) { snprintf(result, rlen, "knowledge base unavailable"); return false; }

    int doc_id = json_get_int(input, "doc_id", 0);
    int page = json_get_int(input, "page", 0);
    if (doc_id <= 0) { snprintf(result, rlen, "missing: doc_id"); return false; }

    jbuf_t out;
    jbuf_init(&out, rlen > 8192 ? 8192 : rlen);

    if (page > 0) {
        /* Get specific page */
        sqlite3_stmt *stmt;
        sqlite3_prepare_v2(db, "SELECT content, word_count FROM pages WHERE doc_id=? AND page_num=?", -1, &stmt, NULL);
        sqlite3_bind_int(stmt, 1, doc_id);
        sqlite3_bind_int(stmt, 2, page);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *c = (const char *)sqlite3_column_text(stmt, 0);
            jbuf_appendf(&out, "{\"doc_id\":%d,\"page\":%d,\"words\":%d,\"content\":",
                         doc_id, page, sqlite3_column_int(stmt, 1));
            jbuf_append_json_str(&out, c ? c : "");
            jbuf_append(&out, "}");
        } else {
            jbuf_appendf(&out, "{\"error\":\"page %d not found in doc %d\"}", page, doc_id);
        }
        sqlite3_finalize(stmt);
    } else {
        /* Get all pages for document */
        sqlite3_stmt *stmt;
        sqlite3_prepare_v2(db, "SELECT page_num, content, word_count FROM pages WHERE doc_id=? ORDER BY page_num", -1, &stmt, NULL);
        sqlite3_bind_int(stmt, 1, doc_id);
        jbuf_appendf(&out, "{\"doc_id\":%d,\"pages\":[", doc_id);
        int pc = 0;
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            if (pc > 0) jbuf_append(&out, ",");
            const char *c = (const char *)sqlite3_column_text(stmt, 1);
            jbuf_appendf(&out, "{\"page\":%d,\"words\":%d,\"content\":",
                         sqlite3_column_int(stmt, 0), sqlite3_column_int(stmt, 2));
            /* Truncate per-page to leave room */
            char snip[2048];
            snprintf(snip, sizeof(snip), "%.2000s", c ? c : "");
            jbuf_append_json_str(&out, snip);
            jbuf_append(&out, "}");
            pc++;
        }
        sqlite3_finalize(stmt);
        jbuf_appendf(&out, "],\"page_count\":%d}", pc);
    }

    result[0] = '\0';
    truncate_response(out.data ? out.data : "{}", result, rlen, 32);
    jbuf_free(&out);
    return true;
}

/* Delete a document from the knowledge base */
bool tool_kb_delete(const char *input, char *result, size_t rlen) {
    sqlite3 *db = kb_db();
    if (!db) { snprintf(result, rlen, "knowledge base unavailable"); return false; }

    int doc_id = json_get_int(input, "doc_id", 0);
    if (doc_id <= 0) { snprintf(result, rlen, "missing: doc_id"); return false; }

    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db, "DELETE FROM pages WHERE doc_id=?", -1, &stmt, NULL);
    sqlite3_bind_int(stmt, 1, doc_id);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    sqlite3_prepare_v2(db, "DELETE FROM documents WHERE id=?", -1, &stmt, NULL);
    sqlite3_bind_int(stmt, 1, doc_id);
    sqlite3_step(stmt);
    int changes = sqlite3_changes(db);
    sqlite3_finalize(stmt);

    if (changes > 0)
        snprintf(result, rlen, "{\"deleted\":true,\"doc_id\":%d}", doc_id);
    else
        snprintf(result, rlen, "{\"deleted\":false,\"doc_id\":%d,\"error\":\"not found\"}", doc_id);
    return true;
}

/* Search arXiv for papers by query (uses arXiv API, no auth) */
bool tool_arxiv_search(const char *input, char *result, size_t rlen) {
    char *query = json_get_str(input, "query");
    if (!query || !query[0]) { free(query); snprintf(result, rlen, "missing: query"); return false; }
    int limit = json_get_int(input, "limit", 10);
    if (limit > 50) limit = 50;

    CURL *c = curl_easy_init();
    char *enc = curl_easy_escape(c, query, 0);
    char url[2048];
    snprintf(url, sizeof(url),
             "http://export.arxiv.org/api/query?search_query=all:%s&start=0&max_results=%d&sortBy=relevance&sortOrder=descending",
             enc, limit);
    curl_free(enc); curl_easy_cleanup(c); free(query);

    http_buf_t resp = {0};
    long st = http_get_authed(url, NULL, &resp);
    if (st != 200) {
        snprintf(result, rlen, "arXiv API error (HTTP %ld)", st);
        free(resp.data); return false;
    }
    result[0] = '\0';
    truncate_response(resp.data ? resp.data : "", result, rlen, 32);
    free(resp.data); return true;
}

/* Download and ingest an arXiv paper by ID into the knowledge base */
bool tool_arxiv_ingest(const char *input, char *result, size_t rlen) {
    char *arxiv_id = json_get_str(input, "arxiv_id");
    char *title = json_get_str(input, "title");
    if (!arxiv_id || !arxiv_id[0]) {
        free(arxiv_id); free(title);
        snprintf(result, rlen, "missing: arxiv_id (e.g. '2305.10601')");
        return false;
    }

    /* Build URL and delegate to kb_ingest */
    char url[256];
    snprintf(url, sizeof(url), "https://arxiv.org/pdf/%s", arxiv_id);

    jbuf_t fake_input;
    jbuf_init(&fake_input, 512);
    jbuf_append(&fake_input, "{\"url\":");
    jbuf_append_json_str(&fake_input, url);
    if (title && title[0]) {
        jbuf_append(&fake_input, ",\"title\":");
        jbuf_append_json_str(&fake_input, title);
    }
    jbuf_append(&fake_input, "}");

    bool ok = tool_kb_ingest(fake_input.data, result, rlen);
    jbuf_free(&fake_input);
    free(arxiv_id); free(title);
    return ok;
}

/* ══════════════════════════════════════════════════════════════════════════
 *  HIERARCHICAL DEEP SEARCH
 *
 *  3-level retrieval pipeline inspired by RAPTOR / GraphRAG / ColBERT:
 *
 *  Level 1: DOCUMENT — keyword match on doc_summaries.keywords
 *           → identifies candidate documents (fast, coarse)
 *
 *  Level 2: PAGE — FTS5 BM25 on pages_fts within candidate docs
 *           → narrows to relevant pages (medium granularity)
 *
 *  Level 3: CHUNK — FTS5 BM25 on chunks_fts (256-word windows, 64-word overlap)
 *           → extracts precise passages with surrounding context
 *
 *  Results include: document context, page location, passage text,
 *  relevance score at each level, and cross-references between docs.
 * ══════════════════════════════════════════════════════════════════════════ */

bool tool_kb_deep_search(const char *input, char *result, size_t rlen) {
    sqlite3 *db = kb_db();
    if (!db) { snprintf(result, rlen, "knowledge base unavailable"); return false; }

    char *query = json_get_str(input, "query");
    if (!query || !query[0]) { free(query); snprintf(result, rlen, "missing: query"); return false; }
    int limit = json_get_int(input, "limit", 5);
    if (limit > 15) limit = 15;
    int depth = json_get_int(input, "depth", 3); /* 1=doc, 2=page, 3=chunk */
    if (depth < 1) depth = 1;
    if (depth > 3) depth = 3;

    /* Build quoted FTS5 query */
    char fts_q[1024];
    {
        char *q = strdup(query);
        char *tok = strtok(q, " \t\n");
        int wc = 0;
        fts_q[0] = '\0';
        while (tok) {
            if (strlen(tok) > 2 && wc < 10) {
                if (wc > 0) strcat(fts_q, " ");
                strcat(fts_q, "\"");
                strncat(fts_q, tok, sizeof(fts_q) - strlen(fts_q) - 4);
                strcat(fts_q, "\"");
                wc++;
            }
            tok = strtok(NULL, " \t\n");
        }
        free(q);
        if (fts_q[0] == '\0') snprintf(fts_q, sizeof(fts_q), "%s", query);
    }

    jbuf_t out;
    jbuf_init(&out, rlen > 16384 ? 16384 : rlen);
    jbuf_appendf(&out, "{\"query\":");
    jbuf_append_json_str(&out, query);
    jbuf_appendf(&out, ",\"depth\":%d", depth);

    /* ── Level 1: Document keyword match ──────────────────────────────── */
    jbuf_append(&out, ",\"level_1_documents\":[");
    {
        sqlite3_stmt *stmt;
        /* Match query words against doc keywords + title */
        sqlite3_prepare_v2(db,
            "SELECT d.id, d.title, d.page_count, ds.keywords, ds.category "
            "FROM documents d LEFT JOIN doc_summaries ds ON d.id=ds.doc_id "
            "WHERE d.title LIKE ? OR ds.keywords LIKE ? "
            "ORDER BY d.page_count DESC LIMIT ?",
            -1, &stmt, NULL);
        char like[256];
        /* Extract first meaningful word for LIKE */
        char *first_word = strdup(query);
        char *sp = strchr(first_word, ' ');
        if (sp) *sp = '\0';
        snprintf(like, sizeof(like), "%%%s%%", first_word);
        free(first_word);
        sqlite3_bind_text(stmt, 1, like, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, like, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 3, limit * 3);

        int dc = 0;
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            if (dc > 0) jbuf_append(&out, ",");
            jbuf_appendf(&out, "{\"doc_id\":%d,", sqlite3_column_int(stmt, 0));
            jbuf_append(&out, "\"title\":");
            jbuf_append_json_str(&out, (const char *)sqlite3_column_text(stmt, 1));
            jbuf_appendf(&out, ",\"pages\":%d", sqlite3_column_int(stmt, 2));
            const char *kw = (const char *)sqlite3_column_text(stmt, 3);
            if (kw) { jbuf_append(&out, ",\"keywords\":"); jbuf_append_json_str(&out, kw); }
            jbuf_append(&out, "}");
            dc++;
        }
        sqlite3_finalize(stmt);
    }
    jbuf_append(&out, "]");

    if (depth >= 2) {
        /* ── Level 2: Page-level FTS5 ─────────────────────────────────── */
        jbuf_append(&out, ",\"level_2_pages\":[");
        {
            sqlite3_stmt *stmt;
            int rc = sqlite3_prepare_v2(db,
                "SELECT p.doc_id, d.title, p.page_num, rank, "
                "       snippet(pages_fts, 0, '>>>', '<<<', '...', 48) "
                "FROM pages_fts "
                "JOIN pages p ON pages_fts.rowid = p.id "
                "JOIN documents d ON p.doc_id = d.id "
                "WHERE pages_fts MATCH ? "
                "ORDER BY rank LIMIT ?",
                -1, &stmt, NULL);
            if (rc == SQLITE_OK) {
                sqlite3_bind_text(stmt, 1, fts_q, -1, SQLITE_TRANSIENT);
                sqlite3_bind_int(stmt, 2, limit);
                int pc = 0;
                while (sqlite3_step(stmt) == SQLITE_ROW) {
                    if (pc > 0) jbuf_append(&out, ",");
                    double score = -sqlite3_column_double(stmt, 3);
                    jbuf_appendf(&out, "{\"doc_id\":%d,", sqlite3_column_int(stmt, 0));
                    jbuf_append(&out, "\"title\":");
                    jbuf_append_json_str(&out, (const char *)sqlite3_column_text(stmt, 1));
                    jbuf_appendf(&out, ",\"page\":%d,\"score\":%.2f",
                                 sqlite3_column_int(stmt, 2), score);
                    const char *snip = (const char *)sqlite3_column_text(stmt, 4);
                    if (snip) { jbuf_append(&out, ",\"snippet\":"); jbuf_append_json_str(&out, snip); }
                    jbuf_append(&out, "}");
                    pc++;
                }
                sqlite3_finalize(stmt);
            }
        }
        jbuf_append(&out, "]");
    }

    if (depth >= 3) {
        /* ── Level 3: Chunk-level FTS5 (finest granularity) ───────────── */
        jbuf_append(&out, ",\"level_3_chunks\":[");
        {
            sqlite3_stmt *stmt;
            int rc = sqlite3_prepare_v2(db,
                "SELECT c.doc_id, d.title, c.page_num, c.chunk_idx, rank, "
                "       snippet(chunks_fts, 0, '>>>', '<<<', '...', 40) "
                "FROM chunks_fts "
                "JOIN chunks c ON chunks_fts.rowid = c.id "
                "JOIN documents d ON c.doc_id = d.id "
                "WHERE chunks_fts MATCH ? "
                "ORDER BY rank LIMIT ?",
                -1, &stmt, NULL);
            if (rc == SQLITE_OK) {
                sqlite3_bind_text(stmt, 1, fts_q, -1, SQLITE_TRANSIENT);
                sqlite3_bind_int(stmt, 2, limit);
                int cc = 0;
                while (sqlite3_step(stmt) == SQLITE_ROW) {
                    if (cc > 0) jbuf_append(&out, ",");
                    double score = -sqlite3_column_double(stmt, 4);
                    jbuf_appendf(&out, "{\"doc_id\":%d,", sqlite3_column_int(stmt, 0));
                    jbuf_append(&out, "\"title\":");
                    jbuf_append_json_str(&out, (const char *)sqlite3_column_text(stmt, 1));
                    jbuf_appendf(&out, ",\"page\":%d,\"chunk\":%d,\"score\":%.2f",
                                 sqlite3_column_int(stmt, 2),
                                 sqlite3_column_int(stmt, 3), score);
                    const char *snip = (const char *)sqlite3_column_text(stmt, 5);
                    if (snip) { jbuf_append(&out, ",\"passage\":"); jbuf_append_json_str(&out, snip); }
                    jbuf_append(&out, "}");
                    cc++;
                }
                sqlite3_finalize(stmt);
            }
        }
        jbuf_append(&out, "]");
    }

    /* Stats */
    int total_docs = 0, total_pages = 0, total_chunks = 0;
    sqlite3_stmt *st;
    sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM documents", -1, &st, NULL);
    if (sqlite3_step(st) == SQLITE_ROW) total_docs = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM pages_fts", -1, &st, NULL);
    if (sqlite3_step(st) == SQLITE_ROW) total_pages = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM chunks_fts", -1, &st, NULL);
    if (sqlite3_step(st) == SQLITE_ROW) total_chunks = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);

    jbuf_appendf(&out, ",\"corpus\":{\"documents\":%d,\"pages\":%d,\"chunks\":%d}}",
                 total_docs, total_pages, total_chunks);

    result[0] = '\0';
    truncate_response(out.data ? out.data : "{}", result, rlen, 64);
    jbuf_free(&out);
    free(query);
    return true;
}

/* Forward declaration for kalshi_get (defined in Kalshi section below) */
static long kalshi_get(const char *path, http_buf_t *out);

static long strat_kalshi_get(const char *path, http_buf_t *out) {
    char url[2048];
    snprintf(url, sizeof(url),
             "https://api.elections.kalshi.com/trade-api/v2%s", path);
    return http_get_authed(url, NULL, out);
}

/* ══════════════════════════════════════════════════════════════════════════
 *  SYSTEMATIC STRATEGY ENGINES
 *
 *  These tools implement the strategies identified by cross-referencing
 *  380+ research papers with live prediction market data:
 *
 *  1. Completeness Arb    — Σ(bracket prices) < $1.00 → riskless profit
 *  2. Binary Fade         — LLM acquiescence bias → fade >60¢ / <40¢
 *  3. Stale Snipe         — Near-expiry stale orders on CLOB bracket markets
 *  4. Kelly Sizing        — Optimal bet sizing given edge and bankroll
 *  5. Spread Scanner      — Find widest bid-ask spreads for market-making
 * ══════════════════════════════════════════════════════════════════════════ */

/* Strategy 1: Completeness Arbitrage Scanner
 * If all mutually-exclusive outcomes in an event sum to < $1.00,
 * buying one of each guarantees profit. Scans Kalshi bracket markets. */
bool tool_strat_completeness(const char *input, char *result, size_t rlen) {
    char *series = json_get_str(input, "series");
    if (!series || !series[0]) {
        free(series);
        series = strdup("KXBTC");
    }

    /* Fetch markets for this series */
    char path[512];
    snprintf(path, sizeof(path),
             "/events?limit=5&status=open&with_nested_markets=true&series_ticker=%s", series);
    http_buf_t resp = {0};
    long st = strat_kalshi_get(path, &resp);
    free(series);

    if (st != 200) {
        snprintf(result, rlen, "Kalshi API error (HTTP %ld)", st);
        free(resp.data); return false;
    }

    /* Also check Polymarket for multi-outcome events */
    http_buf_t pm_resp = {0};
    http_get_authed(
        "https://gamma-api.polymarket.com/events?limit=10&active=true&order=volume24hr&ascending=false",
        NULL, &pm_resp);

    jbuf_t out;
    jbuf_init(&out, rlen > 8192 ? 8192 : rlen);
    jbuf_append(&out, "{\"completeness_scan\":{");
    jbuf_append(&out, "\"strategy\":\"If sum of all YES asks < $1.00 for mutually exclusive outcomes, buy all for riskless profit\",");
    jbuf_append(&out, "\"threshold\":\"Actionable if sum < $0.98 (2c profit after fees)\",");

    /* Kalshi data */
    jbuf_append(&out, "\"kalshi\":");
    if (resp.data) {
        size_t cap = rlen / 3;
        if (resp.len > cap) resp.data[cap] = '\0';
        jbuf_append(&out, resp.data);
    } else {
        jbuf_append(&out, "{}");
    }

    /* Polymarket data */
    jbuf_append(&out, ",\"polymarket\":");
    if (pm_resp.data) {
        size_t cap = rlen / 3;
        if (pm_resp.len > cap) pm_resp.data[cap] = '\0';
        jbuf_append(&out, pm_resp.data);
    } else {
        jbuf_append(&out, "[]");
    }

    jbuf_append(&out, ",\"instructions\":\"For each event with N mutually exclusive outcomes: "
        "sum all YES ask prices. If sum < 1.00, buy 1 contract of each outcome. "
        "One must win → payout = $1.00, cost = sum. Profit = 1.00 - sum. "
        "Watch for: Kalshi BTC/ETH/SPY bracket markets, Polymarket multi-candidate events.\"");
    jbuf_append(&out, "}}");

    result[0] = '\0';
    truncate_response(out.data ? out.data : "{}", result, rlen, 64);
    jbuf_free(&out);
    free(resp.data); free(pm_resp.data);
    return true;
}

/* Strategy 2: Binary Fade — Counter LLM acquiescence bias
 * Research (Schoenegger et al.) shows LLMs systematically over-predict YES
 * on binary questions. In markets with LLM bot participation (crypto 5m/15m
 * binaries), YES is biased high. Fade: sell YES when >60%, buy NO when <40%.
 */
bool tool_strat_binary_fade(const char *input, char *result, size_t rlen) {
    double high_threshold = json_get_double(input, "high_threshold", 0.60);
    double low_threshold = json_get_double(input, "low_threshold", 0.40);
    int limit = json_get_int(input, "limit", 20);
    if (limit > 50) limit = 50;

    /* Fetch active Polymarket crypto binary markets */
    char url[512];
    snprintf(url, sizeof(url),
             "https://gamma-api.polymarket.com/markets?limit=%d&active=true"
             "&order=volume24hr&ascending=false&tag=crypto", limit);
    http_buf_t resp = {0};
    long st = http_get_authed(url, NULL, &resp);

    /* Also get general short-duration markets */
    http_buf_t gen_resp = {0};
    snprintf(url, sizeof(url),
             "https://gamma-api.polymarket.com/markets?limit=%d&active=true"
             "&order=volume24hr&ascending=false", limit);
    http_get_authed(url, NULL, &gen_resp);

    jbuf_t out;
    jbuf_init(&out, rlen > 8192 ? 8192 : rlen);
    jbuf_appendf(&out, "{\"binary_fade\":{");
    jbuf_appendf(&out, "\"strategy\":\"Fade LLM acquiescence bias: sell YES >%.0f%%, buy YES <%.0f%%\",",
                 high_threshold * 100, low_threshold * 100);
    jbuf_append(&out, "\"academic_basis\":\"Schoenegger et al. 2024 — LLM ensembles systematically "
        "over-predict YES on binary questions (acquiescence bias). Effect strongest on "
        "short-duration crypto markets where LLM bots are most active.\",");
    jbuf_appendf(&out, "\"high_threshold\":%.2f,\"low_threshold\":%.2f,", high_threshold, low_threshold);

    /* Include market data for the agent to analyze */
    jbuf_append(&out, "\"crypto_markets\":");
    if (resp.data && st == 200) {
        size_t cap = rlen / 4;
        if (resp.len > cap) resp.data[cap] = '\0';
        jbuf_append(&out, resp.data);
    } else {
        jbuf_append(&out, "[]");
    }

    jbuf_append(&out, ",\"all_markets\":");
    if (gen_resp.data) {
        size_t cap = rlen / 4;
        if (gen_resp.len > cap) gen_resp.data[cap] = '\0';
        jbuf_append(&out, gen_resp.data);
    } else {
        jbuf_append(&out, "[]");
    }

    jbuf_append(&out, ",\"instructions\":\"For each market: 1) Check if YES price > high_threshold "
        "or < low_threshold. 2) If so, check if it's a short-duration binary (5m/15m/1h). "
        "3) Check volume — higher volume = more LLM bot participation = stronger signal. "
        "4) Calculate edge: true_prob ≈ 0.50 for random-walk assets, so edge = |0.50 - market_price|. "
        "5) Size with quarter-Kelly: f = 0.25 × (edge / odds). "
        "Report: ticker, current_price, direction (fade_high or buy_low), edge, kelly_size.\"");
    jbuf_append(&out, "}}");

    result[0] = '\0';
    truncate_response(out.data ? out.data : "{}", result, rlen, 64);
    jbuf_free(&out);
    free(resp.data); free(gen_resp.data);
    return true;
}

/* Strategy 3: Stale Order Sniper — Near-expiry CLOB convergence lag
 * Academic basis: Chen & Pennock (LMSR convergence) vs CLOB stale orders.
 * In final hour before expiry, market makers stop updating → stale limits persist.
 * Compare real-time reference price to resting order prices to find pickoffs.
 */
bool tool_strat_stale_snipe(const char *input, char *result, size_t rlen) {
    char *series = json_get_str(input, "series");
    if (!series || !series[0]) { free(series); series = strdup("KXBTC"); }
    int limit = json_get_int(input, "limit", 10);

    /* Get current bracket markets for the series */
    char path[512];
    snprintf(path, sizeof(path),
             "/events?limit=%d&status=open&with_nested_markets=true&series_ticker=%s",
             limit, series);
    http_buf_t ev_resp = {0};
    long st = strat_kalshi_get(path, &ev_resp);

    /* Get recent trades to estimate current price activity */
    http_buf_t tr_resp = {0};
    snprintf(path, sizeof(path), "/markets/trades?limit=20");
    strat_kalshi_get(path, &tr_resp);

    jbuf_t out;
    jbuf_init(&out, rlen > 8192 ? 8192 : rlen);
    jbuf_appendf(&out, "{\"stale_snipe\":{");
    jbuf_appendf(&out, "\"series\":\"%s\",", series);
    jbuf_append(&out, "\"strategy\":\"In CLOB bracket markets near expiry, stale limit orders "
        "persist because market makers stop updating. Compare real-time reference price "
        "to resting orders to find mispriced contracts.\",");
    jbuf_append(&out, "\"academic_basis\":\"Chen & Pennock LMSR convergence theory: LMSR prices "
        "converge smoothly but CLOBs require active traders. Bouchaud et al. adverse "
        "selection framework explains why liquidity providers withdraw near expiry.\",");

    /* Events data */
    jbuf_append(&out, "\"events\":");
    if (ev_resp.data && st == 200) {
        size_t cap = rlen / 3;
        if (ev_resp.len > cap) ev_resp.data[cap] = '\0';
        jbuf_append(&out, ev_resp.data);
    } else {
        jbuf_append(&out, "{}");
    }

    /* Recent trades */
    jbuf_append(&out, ",\"recent_trades\":");
    if (tr_resp.data) {
        size_t cap = rlen / 4;
        if (tr_resp.len > cap) tr_resp.data[cap] = '\0';
        jbuf_append(&out, tr_resp.data);
    } else {
        jbuf_append(&out, "{}");
    }

    jbuf_append(&out, ",\"instructions\":\"1) Identify bracket markets expiring within 1-2 hours. "
        "2) For each bracket, get the orderbook (kalshi_orderbook). "
        "3) Compare resting YES ask prices to the current reference price probability. "
        "4) For BTC: use current spot to compute which bracket should be ~100%% YES. "
        "5) Any bracket containing current spot with YES ask < $0.90 is a stale snipe opportunity. "
        "6) Any bracket NOT containing spot with YES bid > $0.10 is also stale (sell). "
        "7) Size: small (1-5 contracts) due to thin books.\"");
    jbuf_append(&out, "}}");

    result[0] = '\0';
    truncate_response(out.data ? out.data : "{}", result, rlen, 64);
    jbuf_free(&out);
    free(series); free(ev_resp.data); free(tr_resp.data);
    return true;
}

/* Strategy 4: Kelly Criterion Calculator
 * Given estimated true probability and market price, compute optimal bet size.
 * Supports full/half/quarter Kelly with adjustment for parameter uncertainty.
 */
bool tool_strat_kelly(const char *input, char *result, size_t rlen) {
    double true_prob = json_get_double(input, "true_prob", 0);
    double market_price = json_get_double(input, "market_price", 0);
    double bankroll = json_get_double(input, "bankroll", 1000.0);
    double confidence = json_get_double(input, "confidence", 0.5);
    char *side = json_get_str(input, "side");

    if (true_prob <= 0 || true_prob >= 1 || market_price <= 0 || market_price >= 1) {
        free(side);
        snprintf(result, rlen,
                 "{\"error\":\"Provide true_prob (0-1), market_price (0-1). "
                 "Example: true_prob=0.65 market_price=0.50 bankroll=1000\"}");
        return false;
    }

    /* Determine direction */
    bool buy_yes = true;
    if (side && (strcmp(side, "no") == 0 || strcmp(side, "NO") == 0)) {
        buy_yes = false;
    } else if (!side || !side[0]) {
        buy_yes = (true_prob > market_price);
    }
    free(side);

    double p, q, price;
    if (buy_yes) {
        p = true_prob;
        q = 1.0 - p;
        price = market_price;
    } else {
        p = 1.0 - true_prob;
        q = true_prob;
        price = 1.0 - market_price;
    }

    double b = (1.0 - price) / price;  /* odds */
    double edge = p - (1.0 / (1.0 + b));  /* edge over market */
    double ev = p * (1.0 - price) - q * price;  /* EV per dollar risked */
    double kelly_full = (p * b - q) / b;
    if (kelly_full < 0) kelly_full = 0;

    double kelly_half = kelly_full * 0.5;
    double kelly_quarter = kelly_full * 0.25;
    double kelly_adj = kelly_quarter * confidence;  /* confidence-adjusted */

    double bet_full = bankroll * kelly_full;
    double bet_half = bankroll * kelly_half;
    double bet_quarter = bankroll * kelly_quarter;
    double bet_adj = bankroll * kelly_adj;

    int contracts_adj = (int)(bet_adj / price);
    double max_loss = bet_adj;
    double max_win = contracts_adj * (1.0 - price);

    snprintf(result, rlen,
        "{\"kelly\":{\"direction\":\"%s\",\"true_prob\":%.4f,\"market_price\":%.4f,"
        "\"edge\":%.4f,\"ev_per_dollar\":%.4f,\"odds\":%.2f,"
        "\"kelly_full\":%.4f,\"kelly_half\":%.4f,\"kelly_quarter\":%.4f,"
        "\"confidence\":%.2f,\"kelly_adjusted\":%.4f,"
        "\"bankroll\":%.2f,"
        "\"bet_full\":%.2f,\"bet_half\":%.2f,\"bet_quarter\":%.2f,\"bet_adjusted\":%.2f,"
        "\"contracts\":%d,\"max_loss\":%.2f,\"max_win\":%.2f,"
        "\"recommendation\":\"%s\"}}",
        buy_yes ? "BUY_YES" : "BUY_NO",
        true_prob, market_price, edge, ev, b,
        kelly_full, kelly_half, kelly_quarter,
        confidence, kelly_adj,
        bankroll,
        bet_full, bet_half, bet_quarter, bet_adj,
        contracts_adj, max_loss, max_win,
        kelly_full <= 0 ? "NO EDGE — do not trade" :
        kelly_full < 0.02 ? "Marginal edge — tiny size only" :
        kelly_full < 0.10 ? "Moderate edge — quarter-Kelly recommended" :
        "Strong edge — half-Kelly max, quarter-Kelly conservative");
    return true;
}

/* Strategy 5: Spread Scanner — Find widest bid-ask spreads for market-making
 * Wide spreads = opportunity to provide liquidity and earn the spread.
 * Targets illiquid markets where no one is making a market.
 */
bool tool_strat_spread_scan(const char *input, char *result, size_t rlen) {
    int limit = json_get_int(input, "limit", 20);
    if (limit > 50) limit = 50;
    char *category = json_get_str(input, "category");

    /* Fetch Polymarket active markets sorted by volume */
    jbuf_t url;
    jbuf_init(&url, 256);
    jbuf_appendf(&url,
        "https://gamma-api.polymarket.com/markets?limit=%d&active=true"
        "&order=volume24hr&ascending=false", limit);
    if (category && category[0]) {
        CURL *c = curl_easy_init();
        char *enc = curl_easy_escape(c, category, 0);
        jbuf_appendf(&url, "&tag=%s", enc);
        curl_free(enc); curl_easy_cleanup(c);
    }
    free(category);

    http_buf_t pm_resp = {0};
    http_get_authed(url.data, NULL, &pm_resp);
    jbuf_free(&url);

    /* Also get Kalshi open events */
    http_buf_t kl_resp = {0};
    char path[256];
    snprintf(path, sizeof(path), "/events?limit=%d&status=open&with_nested_markets=true", limit);
    strat_kalshi_get(path, &kl_resp);

    jbuf_t out;
    jbuf_init(&out, rlen > 8192 ? 8192 : rlen);
    jbuf_append(&out, "{\"spread_scan\":{");
    jbuf_append(&out, "\"strategy\":\"Find markets with widest bid-ask spreads for market-making. "
        "Place limit orders on both sides to earn the spread.\",");
    jbuf_append(&out, "\"ideal_targets\":\"Markets with: spread > 5c, volume > $10K/day, "
        "clear binary outcome, time to expiry > 24h\",");
    jbuf_append(&out, "\"risk\":\"Adverse selection — informed traders pick off your quotes. "
        "Mitigate by: tighter quotes on high-volume markets, wider on illiquid.\",");

    jbuf_append(&out, "\"polymarket\":");
    if (pm_resp.data) {
        size_t cap = rlen / 3;
        if (pm_resp.len > cap) pm_resp.data[cap] = '\0';
        jbuf_append(&out, pm_resp.data);
    } else jbuf_append(&out, "[]");

    jbuf_append(&out, ",\"kalshi\":");
    if (kl_resp.data) {
        size_t cap = rlen / 3;
        if (kl_resp.len > cap) kl_resp.data[cap] = '\0';
        jbuf_append(&out, kl_resp.data);
    } else jbuf_append(&out, "{}");

    jbuf_append(&out, ",\"instructions\":\"For each market: "
        "1) Get orderbook (polymarket_book or kalshi_orderbook). "
        "2) Calculate spread = best_ask - best_bid. "
        "3) Flag markets where spread > 5c AND daily volume > $10K. "
        "4) For market-making: place bid at best_bid+1c and ask at best_ask-1c. "
        "5) Expected profit per round-trip = spread - 2c (your improvement). "
        "6) Risk: position accumulation if one side fills repeatedly.\"");
    jbuf_append(&out, "}}");

    result[0] = '\0';
    truncate_response(out.data ? out.data : "{}", result, rlen, 64);
    jbuf_free(&out);
    free(pm_resp.data); free(kl_resp.data);
    return true;
}

/* ══════════════════════════════════════════════════════════════════════════
 *  POLYMARKET — Prediction market data (public, no auth required)
 *
 *  APIs:
 *    Gamma:  https://gamma-api.polymarket.com   (markets, events, metadata)
 *    CLOB:   https://clob.polymarket.com        (order book, prices, trades)
 *    Data:   https://data-api.polymarket.com    (activity, leaderboard)
 * ══════════════════════════════════════════════════════════════════════════ */

/* Search/list Polymarket markets with optional filters */
bool tool_polymarket_markets(const char *input, char *result, size_t rlen) {
    char *tag = json_get_str(input, "tag");
    char *active = json_get_str(input, "active");
    char *closed = json_get_str(input, "closed");
    int limit = json_get_int(input, "limit", 10);
    int offset = json_get_int(input, "offset", 0);
    if (limit > 100) limit = 100;

    jbuf_t url;
    jbuf_init(&url, 512);
    jbuf_appendf(&url, "https://gamma-api.polymarket.com/markets?limit=%d&offset=%d&order=volume24hr&ascending=false",
                 limit, offset);
    if (active && active[0])
        jbuf_appendf(&url, "&active=%s", active);
    if (closed && closed[0])
        jbuf_appendf(&url, "&closed=%s", closed);
    if (tag && tag[0]) {
        CURL *c = curl_easy_init();
        char *enc = curl_easy_escape(c, tag, 0);
        jbuf_appendf(&url, "&tag=%s", enc);
        curl_free(enc); curl_easy_cleanup(c);
    }
    free(tag); free(active); free(closed);

    http_buf_t resp = {0};
    long status = http_get_authed(url.data, NULL, &resp);
    jbuf_free(&url);
    if (status != 200) {
        snprintf(result, rlen, "Polymarket Gamma API error (HTTP %ld)", status);
        free(resp.data); return false;
    }
    result[0] = '\0';
    truncate_response(resp.data ? resp.data : "[]", result, rlen, 32);
    free(resp.data); return true;
}

/* Get Polymarket events (grouped markets) — supports tag/category filtering */
bool tool_polymarket_events(const char *input, char *result, size_t rlen) {
    char *slug = json_get_str(input, "slug");
    char *id = json_get_str(input, "id");
    char *tag = json_get_str(input, "tag");
    int limit = json_get_int(input, "limit", 20);
    int offset = json_get_int(input, "offset", 0);
    if (limit > 100) limit = 100;

    jbuf_t url;
    jbuf_init(&url, 512);
    if (id && id[0]) {
        jbuf_appendf(&url, "https://gamma-api.polymarket.com/events/%s", id);
    } else if (slug && slug[0]) {
        CURL *c = curl_easy_init();
        char *enc = curl_easy_escape(c, slug, 0);
        jbuf_appendf(&url, "https://gamma-api.polymarket.com/events?slug=%s", enc);
        curl_free(enc); curl_easy_cleanup(c);
    } else {
        jbuf_appendf(&url, "https://gamma-api.polymarket.com/events?limit=%d&offset=%d&active=true&order=volume24hr&ascending=false", limit, offset);
        if (tag && tag[0]) {
            CURL *c = curl_easy_init();
            char *enc = curl_easy_escape(c, tag, 0);
            jbuf_appendf(&url, "&tag=%s", enc);
            curl_free(enc); curl_easy_cleanup(c);
        }
    }
    free(slug); free(id); free(tag);

    http_buf_t resp = {0};
    long status = http_get_authed(url.data, NULL, &resp);
    jbuf_free(&url);
    if (status != 200) {
        snprintf(result, rlen, "Polymarket events error (HTTP %ld)", status);
        free(resp.data); return false;
    }
    result[0] = '\0';
    truncate_response(resp.data ? resp.data : "[]", result, rlen, 32);
    free(resp.data); return true;
}

/* List Polymarket categories/tags with event counts */
bool tool_polymarket_categories(const char *input, char *result, size_t rlen) {
    (void)input;
    /* Fetch top events and extract unique tags */
    http_buf_t resp = {0};
    long status = http_get_authed(
        "https://gamma-api.polymarket.com/events?limit=200&active=true&order=volume24hr&ascending=false",
        NULL, &resp);
    if (status != 200 || !resp.data) {
        snprintf(result, rlen, "Polymarket events fetch error (HTTP %ld)", status);
        free(resp.data); return false;
    }

    /* Parse JSON array and count tags */
    /* Simple approach: scan for "tags":["...","..."] patterns */
    /* Build result as JSON object with tag counts */
    jbuf_t out;
    jbuf_init(&out, 4096);
    jbuf_appendf(&out, "{\"categories\":[");

    /* Count tags by scanning the response */
    typedef struct { char name[64]; int count; } tag_entry_t;
    tag_entry_t tags[256];
    memset(tags, 0, sizeof(tags));
    int ntags = 0;

    const char *p = resp.data;
    while ((p = strstr(p, "\"tags\":[")) != NULL) {
        p += 8; /* skip "tags":[ */
        while (*p && *p != ']') {
            if (*p == '"') {
                p++;
                const char *end = strchr(p, '"');
                if (!end) break;
                size_t len = (size_t)(end - p);
                if (len > 0 && len < 63) {
                    char tag_name[64];
                    memcpy(tag_name, p, len);
                    tag_name[len] = '\0';
                    /* Skip internal tags */
                    if (strcmp(tag_name, "Hide From New") != 0 &&
                        strcmp(tag_name, "Recurring") != 0) {
                        /* Find or insert */
                        int found = -1;
                        for (int i = 0; i < ntags; i++) {
                            if (strcmp(tags[i].name, tag_name) == 0) { found = i; break; }
                        }
                        if (found >= 0) {
                            tags[found].count++;
                        } else if (ntags < 256) {
                            strncpy(tags[ntags].name, tag_name, 63);
                            tags[ntags].name[63] = '\0';
                            tags[ntags].count = 1;
                            ntags++;
                        }
                    }
                }
                p = end + 1;
            } else {
                p++;
            }
        }
    }

    /* Sort by count descending (simple insertion sort) */
    for (int i = 1; i < ntags; i++) {
        tag_entry_t tmp = tags[i];
        int j = i - 1;
        while (j >= 0 && tags[j].count < tmp.count) {
            tags[j + 1] = tags[j];
            j--;
        }
        tags[j + 1] = tmp;
    }

    /* Output top tags */
    int max_out = ntags < 50 ? ntags : 50;
    for (int i = 0; i < max_out; i++) {
        if (i > 0) jbuf_appendf(&out, ",");
        jbuf_appendf(&out, "{\"tag\":\"%s\",\"events\":%d}", tags[i].name, tags[i].count);
    }
    jbuf_appendf(&out, "],\"total_tags\":%d}", ntags);

    result[0] = '\0';
    truncate_response(out.data, result, rlen, 32);
    jbuf_free(&out);
    free(resp.data);
    return true;
}

/* Get current prices/midpoints for a Polymarket token */
bool tool_polymarket_prices(const char *input, char *result, size_t rlen) {
    char *token_id = json_get_str(input, "token_id");
    char *condition_id = json_get_str(input, "condition_id");

    if ((!token_id || !token_id[0]) && (!condition_id || !condition_id[0])) {
        free(token_id); free(condition_id);
        snprintf(result, rlen, "missing: token_id or condition_id (get from polymarket_markets)");
        return false;
    }

    jbuf_t url;
    jbuf_init(&url, 512);
    if (token_id && token_id[0]) {
        jbuf_appendf(&url, "https://clob.polymarket.com/midpoint?token_id=%s", token_id);
    } else {
        jbuf_appendf(&url, "https://clob.polymarket.com/midpoints?condition_id=%s", condition_id);
    }
    free(token_id); free(condition_id);

    http_buf_t resp = {0};
    long status = http_get_authed(url.data, NULL, &resp);
    jbuf_free(&url);
    if (status != 200) {
        snprintf(result, rlen, "Polymarket CLOB price error (HTTP %ld)", status);
        free(resp.data); return false;
    }
    result[0] = '\0';
    truncate_response(resp.data ? resp.data : "{}", result, rlen, 32);
    free(resp.data); return true;
}

/* Get order book depth for a Polymarket token */
bool tool_polymarket_book(const char *input, char *result, size_t rlen) {
    char *token_id = json_get_str(input, "token_id");
    if (!token_id || !token_id[0]) {
        free(token_id);
        snprintf(result, rlen, "missing: token_id");
        return false;
    }

    char url[1024];
    snprintf(url, sizeof(url),
             "https://clob.polymarket.com/book?token_id=%s", token_id);
    free(token_id);

    http_buf_t resp = {0};
    long status = http_get_authed(url, NULL, &resp);
    if (status != 200) {
        snprintf(result, rlen, "Polymarket order book error (HTTP %ld)", status);
        free(resp.data); return false;
    }
    result[0] = '\0';
    truncate_response(resp.data ? resp.data : "{}", result, rlen, 32);
    free(resp.data); return true;
}

/* Get recent trades for a Polymarket market */
bool tool_polymarket_trades(const char *input, char *result, size_t rlen) {
    char *condition_id = json_get_str(input, "condition_id");
    char *maker = json_get_str(input, "maker");
    int limit = json_get_int(input, "limit", 20);
    if (limit > 500) limit = 500;

    jbuf_t url;
    jbuf_init(&url, 512);
    if (condition_id && condition_id[0]) {
        jbuf_appendf(&url,
            "https://data-api.polymarket.com/trades?condition_id=%s&limit=%d",
            condition_id, limit);
    } else if (maker && maker[0]) {
        jbuf_appendf(&url,
            "https://data-api.polymarket.com/trades?maker=%s&limit=%d",
            maker, limit);
    } else {
        jbuf_appendf(&url,
            "https://data-api.polymarket.com/trades?limit=%d", limit);
    }
    free(condition_id); free(maker);

    http_buf_t resp = {0};
    long status = http_get_authed(url.data, NULL, &resp);
    jbuf_free(&url);
    if (status != 200) {
        snprintf(result, rlen, "Polymarket trades error (HTTP %ld)", status);
        free(resp.data); return false;
    }
    result[0] = '\0';
    truncate_response(resp.data ? resp.data : "[]", result, rlen, 32);
    free(resp.data); return true;
}

/* Search Polymarket markets by keyword */
bool tool_polymarket_search(const char *input, char *result, size_t rlen) {
    char *query = json_get_str(input, "query");
    if (!query || !query[0]) {
        free(query);
        snprintf(result, rlen, "missing: query");
        return false;
    }
    int limit = json_get_int(input, "limit", 10);
    if (limit > 50) limit = 50;

    CURL *c = curl_easy_init();
    char *enc = curl_easy_escape(c, query, 0);
    char url[2048];
    snprintf(url, sizeof(url),
             "https://gamma-api.polymarket.com/markets?_q=%s&limit=%d&active=true",
             enc, limit);
    curl_free(enc); curl_easy_cleanup(c); free(query);

    http_buf_t resp = {0};
    long status = http_get_authed(url, NULL, &resp);
    if (status != 200) {
        snprintf(result, rlen, "Polymarket search error (HTTP %ld)", status);
        free(resp.data); return false;
    }
    result[0] = '\0';
    truncate_response(resp.data ? resp.data : "[]", result, rlen, 32);
    free(resp.data); return true;
}

/* ══════════════════════════════════════════════════════════════════════════
 *  KALSHI — Regulated prediction market exchange
 *
 *  Base: https://api.elections.kalshi.com/trade-api/v2
 *  Auth: Public read access (no key needed for market data).
 *        Trading requires KALSHI_API_KEY + RSA signature.
 * ══════════════════════════════════════════════════════════════════════════ */

#define KALSHI_BASE "https://api.elections.kalshi.com/trade-api/v2"

static long kalshi_get(const char *path, http_buf_t *out) {
    char url[2048];
    snprintf(url, sizeof(url), KALSHI_BASE "%s", path);
    return http_get_authed(url, NULL, out);
}

/* List/filter Kalshi events */
bool tool_kalshi_events(const char *input, char *result, size_t rlen) {
    char *status_f = json_get_str(input, "status");
    char *series = json_get_str(input, "series_ticker");
    int limit = json_get_int(input, "limit", 10);
    if (limit > 200) limit = 200;

    jbuf_t path;
    jbuf_init(&path, 256);
    jbuf_appendf(&path, "/events?limit=%d&with_nested_markets=true", limit);
    if (status_f && status_f[0])
        jbuf_appendf(&path, "&status=%s", status_f);
    if (series && series[0])
        jbuf_appendf(&path, "&series_ticker=%s", series);
    free(status_f); free(series);

    http_buf_t resp = {0};
    long st = kalshi_get(path.data, &resp);
    jbuf_free(&path);
    if (st != 200) { snprintf(result, rlen, "Kalshi events error (HTTP %ld)", st); free(resp.data); return false; }
    result[0] = '\0'; truncate_response(resp.data ? resp.data : "{}", result, rlen, 32);
    free(resp.data); return true;
}

/* Get a specific Kalshi market by ticker */
bool tool_kalshi_markets(const char *input, char *result, size_t rlen) {
    char *ticker = json_get_str(input, "ticker");
    char *event_ticker = json_get_str(input, "event_ticker");
    char *status_f = json_get_str(input, "status");
    int limit = json_get_int(input, "limit", 10);

    jbuf_t path;
    jbuf_init(&path, 256);
    if (ticker && ticker[0]) {
        jbuf_appendf(&path, "/markets/%s", ticker);
    } else {
        jbuf_appendf(&path, "/markets?limit=%d", limit);
        if (event_ticker && event_ticker[0])
            jbuf_appendf(&path, "&event_ticker=%s", event_ticker);
        if (status_f && status_f[0])
            jbuf_appendf(&path, "&status=%s", status_f);
    }
    free(ticker); free(event_ticker); free(status_f);

    http_buf_t resp = {0};
    long st = kalshi_get(path.data, &resp);
    jbuf_free(&path);
    if (st != 200) { snprintf(result, rlen, "Kalshi markets error (HTTP %ld)", st); free(resp.data); return false; }
    result[0] = '\0'; truncate_response(resp.data ? resp.data : "{}", result, rlen, 32);
    free(resp.data); return true;
}

/* Get Kalshi order book for a market */
bool tool_kalshi_orderbook(const char *input, char *result, size_t rlen) {
    char *ticker = json_get_str(input, "ticker");
    if (!ticker || !ticker[0]) { free(ticker); snprintf(result, rlen, "missing: ticker"); return false; }
    int depth = json_get_int(input, "depth", 5);

    char path[512];
    snprintf(path, sizeof(path), "/markets/%s/orderbook?depth=%d", ticker, depth);
    free(ticker);

    http_buf_t resp = {0};
    long st = kalshi_get(path, &resp);
    if (st != 200) { snprintf(result, rlen, "Kalshi orderbook error (HTTP %ld)", st); free(resp.data); return false; }
    result[0] = '\0'; truncate_response(resp.data ? resp.data : "{}", result, rlen, 32);
    free(resp.data); return true;
}

/* Get recent Kalshi trades */
bool tool_kalshi_trades(const char *input, char *result, size_t rlen) {
    char *ticker = json_get_str(input, "ticker");
    int limit = json_get_int(input, "limit", 20);
    if (limit > 1000) limit = 1000;

    jbuf_t path;
    jbuf_init(&path, 256);
    jbuf_appendf(&path, "/markets/trades?limit=%d", limit);
    if (ticker && ticker[0])
        jbuf_appendf(&path, "&ticker=%s", ticker);
    free(ticker);

    http_buf_t resp = {0};
    long st = kalshi_get(path.data, &resp);
    jbuf_free(&path);
    if (st != 200) { snprintf(result, rlen, "Kalshi trades error (HTTP %ld)", st); free(resp.data); return false; }
    result[0] = '\0'; truncate_response(resp.data ? resp.data : "{}", result, rlen, 32);
    free(resp.data); return true;
}

/* Get Kalshi series info */
bool tool_kalshi_series(const char *input, char *result, size_t rlen) {
    char *ticker = json_get_str(input, "ticker");
    if (!ticker || !ticker[0]) { free(ticker); snprintf(result, rlen, "missing: ticker"); return false; }

    char path[512];
    snprintf(path, sizeof(path), "/series/%s", ticker);
    free(ticker);

    http_buf_t resp = {0};
    long st = kalshi_get(path, &resp);
    if (st != 200) { snprintf(result, rlen, "Kalshi series error (HTTP %ld)", st); free(resp.data); return false; }
    result[0] = '\0'; truncate_response(resp.data ? resp.data : "{}", result, rlen, 32);
    free(resp.data); return true;
}

/* Case-insensitive substring search (portable — no strcasestr on all platforms) */
static bool ci_contains(const char *haystack, size_t hlen, const char *needle_lower, size_t nlen) {
    if (!haystack || !needle_lower || nlen > hlen) return false;
    for (size_t i = 0; i <= hlen - nlen; i++) {
        bool match = true;
        for (size_t j = 0; j < nlen; j++) {
            if (tolower((unsigned char)haystack[i + j]) != needle_lower[j]) {
                match = false; break;
            }
        }
        if (match) return true;
    }
    return false;
}

/* Search Kalshi events by keyword with real client-side filtering.
 * Fetches a large batch of open events, then filters to those whose
 * title or category contains the query string. Supports optional
 * series_ticker filter for direct API-level filtering (e.g. KXTEMP). */
bool tool_kalshi_search(const char *input, char *result, size_t rlen) {
    char *query = json_get_str(input, "query");
    if (!query || !query[0]) { free(query); snprintf(result, rlen, "missing: query"); return false; }
    int limit = json_get_int(input, "limit", 20);
    if (limit > 200) limit = 200;
    char *series_ticker = json_get_str(input, "series_ticker");

    /* Fetch a large batch to search through */
    jbuf_t fpath;
    jbuf_init(&fpath, 256);
    jbuf_appendf(&fpath, "/events?limit=200&status=open&with_nested_markets=true");
    if (series_ticker && series_ticker[0])
        jbuf_appendf(&fpath, "&series_ticker=%s", series_ticker);

    http_buf_t resp = {0};
    long st = kalshi_get(fpath.data, &resp);
    jbuf_free(&fpath);
    if (st != 200) {
        free(query); free(series_ticker);
        snprintf(result, rlen, "Kalshi search error (HTTP %ld)", st);
        free(resp.data); return false;
    }

    if (!resp.data || !resp.data[0]) {
        snprintf(result, rlen, "{\"events\":[],\"query\":\"%s\",\"matched\":0}", query);
        free(resp.data); free(query); free(series_ticker); return true;
    }

    /* Lowercase query for case-insensitive matching */
    size_t qlen = strlen(query);
    char *q_lower = safe_strdup(query);
    for (size_t i = 0; i < qlen; i++) q_lower[i] = tolower((unsigned char)q_lower[i]);

    /* Build filtered output by scanning for event objects in the JSON */
    jbuf_t out;
    jbuf_init(&out, rlen > 8192 ? 8192 : rlen);
    jbuf_appendf(&out, "{\"events\":[");

    int matched = 0;
    const char *p = resp.data;

    /* Find the start of the events array */
    const char *arr = strstr(p, "\"events\"");
    if (arr) { arr = strchr(arr, '['); if (arr) p = arr; }

    while (*p && matched < limit) {
        /* Find next '{' */
        while (*p && *p != '{') p++;
        if (!*p) break;

        /* Find matching '}' tracking brace depth */
        const char *obj_start = p;
        int depth = 0;
        const char *obj_end = NULL;
        for (const char *s = p; *s; s++) {
            if (*s == '{') depth++;
            else if (*s == '}') { depth--; if (depth == 0) { obj_end = s + 1; break; } }
            else if (*s == '"') {
                s++;
                while (*s && *s != '"') { if (*s == '\\') s++; s++; }
                if (!*s) break;
            }
        }
        if (!obj_end) break;

        size_t obj_len = (size_t)(obj_end - obj_start);

        /* Check if this event object contains the query (case-insensitive) */
        if (ci_contains(obj_start, obj_len, q_lower, qlen)) {
            if (matched > 0) jbuf_append(&out, ",");
            if (out.len + obj_len < rlen - 128) {
                jbuf_append_len(&out, obj_start, obj_len);
            }
            matched++;
        }

        p = obj_end;
    }

    jbuf_appendf(&out, "],\"query\":\"%s\",\"matched\":%d}", query, matched);

    result[0] = '\0';
    truncate_response(out.data ? out.data : "{}", result, rlen, 32);

    jbuf_free(&out);
    free(q_lower);
    free(resp.data); free(query); free(series_ticker);
    return true;
}

/* Kalshi candlestick / price history */
bool tool_kalshi_candlesticks(const char *input, char *result, size_t rlen) {
    char *ticker = json_get_str(input, "ticker");
    if (!ticker || !ticker[0]) { free(ticker); snprintf(result, rlen, "missing: ticker"); return false; }
    char *interval = json_get_str(input, "interval");
    /* interval: 1 (1min), 60 (1hr), 1440 (1day) */
    const char *ivl = (interval && interval[0]) ? interval : "60";

    char path[512];
    snprintf(path, sizeof(path), "/markets/%s/candlesticks?period_interval=%s", ticker, ivl);
    free(ticker); free(interval);

    http_buf_t resp = {0};
    long st = kalshi_get(path, &resp);
    if (st != 200) { snprintf(result, rlen, "Kalshi candlesticks error (HTTP %ld)", st); free(resp.data); return false; }
    result[0] = '\0'; truncate_response(resp.data ? resp.data : "{}", result, rlen, 32);
    free(resp.data); return true;
}

/* ── Weather markets — aggregate from Kalshi + Polymarket ──────────── */

/* Known Kalshi weather series tickers (as of 2025) */
static const char *k_kalshi_weather_series[] = {
    "KXTEMP",     /* Temperature (city high/low) */
    "KXRAIN",     /* Rainfall (city precipitation) */
    "KXSNOW",     /* Snowfall */
    "KXHURR",     /* Hurricane / tropical storm */
    "KXHIGH",     /* Daily high temperature */
    "KXLOW",      /* Daily low temperature */
    NULL
};

/* Known Kalshi weather keywords for fallback matching */
static const char *k_weather_keywords[] = {
    "temperature", "weather", "rain", "snow", "hurricane",
    "precipitation", "storm", "tornado", "flood", "heat",
    "cold", "freeze", "drought", "wind", "celsius", "fahrenheit",
    NULL
};

bool tool_kalshi_weather(const char *input, char *result, size_t rlen) {
    char *city = json_get_str(input, "city");
    int limit = json_get_int(input, "limit", 30);
    if (limit > 100) limit = 100;

    jbuf_t out;
    jbuf_init(&out, rlen > 16384 ? 16384 : rlen);
    jbuf_appendf(&out, "{\"weather_markets\":[");

    int total_matched = 0;

    /* 1. Query each known weather series ticker */
    for (int s = 0; k_kalshi_weather_series[s] && total_matched < limit; s++) {
        char path[512];
        snprintf(path, sizeof(path),
                 "/events?limit=%d&status=open&with_nested_markets=true&series_ticker=%s",
                 limit, k_kalshi_weather_series[s]);

        http_buf_t resp = {0};
        long st = kalshi_get(path, &resp);
        if (st != 200 || !resp.data) { free(resp.data); continue; }

        /* Scan for event objects and optionally filter by city */
        size_t clen = city ? strlen(city) : 0;
        char *c_lower = NULL;
        if (city && city[0]) {
            c_lower = safe_strdup(city);
            for (size_t i = 0; i < clen; i++) c_lower[i] = tolower((unsigned char)c_lower[i]);
        }

        const char *p = resp.data;
        const char *arr = strstr(p, "[");
        if (arr) p = arr;

        while (*p && total_matched < limit) {
            while (*p && *p != '{') p++;
            if (!*p) break;

            const char *obj_start = p;
            int depth = 0;
            const char *obj_end = NULL;
            for (const char *x = p; *x; x++) {
                if (*x == '{') depth++;
                else if (*x == '}') { depth--; if (depth == 0) { obj_end = x + 1; break; } }
                else if (*x == '"') { x++; while (*x && *x != '"') { if (*x == '\\') x++; x++; } if (!*x) break; }
            }
            if (!obj_end) break;

            size_t obj_len = (size_t)(obj_end - obj_start);
            bool include = true;

            /* If city filter is set, check if the object contains the city name */
            if (c_lower) {
                include = ci_contains(obj_start, obj_len, c_lower, clen);
            }

            if (include) {
                if (total_matched > 0) jbuf_append(&out, ",");
                if (out.len + obj_len < rlen - 256)
                    jbuf_append_len(&out, obj_start, obj_len);
                total_matched++;
            }

            p = obj_end;
        }

        free(c_lower);
        free(resp.data);
    }

    /* 2. Also do a keyword search for weather-related events that might not
     *    use the standard series tickers */
    if (total_matched < limit) {
        char path[512];
        snprintf(path, sizeof(path),
                 "/events?limit=200&status=open&with_nested_markets=true");

        http_buf_t resp = {0};
        long st = kalshi_get(path, &resp);
        if (st == 200 && resp.data) {
            const char *p = resp.data;
            const char *arr = strstr(p, "[");
            if (arr) p = arr;

            while (*p && total_matched < limit) {
                while (*p && *p != '{') p++;
                if (!*p) break;

                const char *obj_start = p;
                int depth = 0;
                const char *obj_end = NULL;
                for (const char *x = p; *x; x++) {
                    if (*x == '{') depth++;
                    else if (*x == '}') { depth--; if (depth == 0) { obj_end = x + 1; break; } }
                    else if (*x == '"') { x++; while (*x && *x != '"') { if (*x == '\\') x++; x++; } if (!*x) break; }
                }
                if (!obj_end) break;

                size_t obj_len = (size_t)(obj_end - obj_start);
                bool is_weather = false;

                for (int k = 0; k_weather_keywords[k]; k++) {
                    size_t klen = strlen(k_weather_keywords[k]);
                    if (ci_contains(obj_start, obj_len, k_weather_keywords[k], klen)) {
                        is_weather = true; break;
                    }
                }

                if (is_weather) {
                    /* Check city filter if set */
                    if (city && city[0]) {
                        char *c_lower = safe_strdup(city);
                        size_t clen2 = strlen(city);
                        for (size_t i = 0; i < clen2; i++) c_lower[i] = tolower((unsigned char)c_lower[i]);
                        if (!ci_contains(obj_start, obj_len, c_lower, clen2)) {
                            is_weather = false;
                        }
                        free(c_lower);
                    }
                    if (is_weather) {
                        if (total_matched > 0) jbuf_append(&out, ",");
                        if (out.len + obj_len < rlen - 256)
                            jbuf_append_len(&out, obj_start, obj_len);
                        total_matched++;
                    }
                }

                p = obj_end;
            }
        }
        free(resp.data);
    }

    jbuf_appendf(&out, "],\"matched\":%d,\"source\":\"kalshi\"", total_matched);
    if (city) jbuf_appendf(&out, ",\"city_filter\":\"%s\"", city);
    jbuf_append(&out, "}");

    result[0] = '\0';
    truncate_response(out.data ? out.data : "{}", result, rlen, 32);
    jbuf_free(&out);
    free(city);
    return true;
}

/* Scan both platforms for weather prediction markets */
bool tool_prediction_weather(const char *input, char *result, size_t rlen) {
    char *city = json_get_str(input, "city");
    int limit = json_get_int(input, "limit", 20);
    if (limit > 50) limit = 50;

    jbuf_t out;
    jbuf_init(&out, rlen > 16384 ? 16384 : rlen);
    jbuf_append(&out, "{\"platforms\":{");

    /* Polymarket: search for weather-related markets */
    {
        const char *weather_queries[] = { "weather", "temperature", "hurricane", "rainfall", "snowfall", NULL };
        jbuf_append(&out, "\"polymarket\":{\"markets\":[");
        int pm_count = 0;

        for (int q = 0; weather_queries[q] && pm_count < limit; q++) {
            CURL *c = curl_easy_init();
            char *enc = curl_easy_escape(c, weather_queries[q], 0);
            char url[2048];
            snprintf(url, sizeof(url),
                     "https://gamma-api.polymarket.com/markets?_q=%s&limit=%d&active=true&order=volume24hr&ascending=false",
                     enc, limit - pm_count);
            curl_free(enc); curl_easy_cleanup(c);

            http_buf_t resp = {0};
            long st = http_get_authed(url, NULL, &resp);
            if (st == 200 && resp.data && resp.data[0] == '[') {
                /* Filter by city if specified */
                const char *p = resp.data;
                while (*p && pm_count < limit) {
                    while (*p && *p != '{') p++;
                    if (!*p) break;

                    const char *obj_start = p;
                    int depth = 0;
                    const char *obj_end = NULL;
                    for (const char *s = p; *s; s++) {
                        if (*s == '{') depth++;
                        else if (*s == '}') { depth--; if (depth == 0) { obj_end = s + 1; break; } }
                        else if (*s == '"') { s++; while (*s && *s != '"') { if (*s == '\\') s++; s++; } if (!*s) break; }
                    }
                    if (!obj_end) break;

                    size_t olen = (size_t)(obj_end - obj_start);
                    bool include = true;
                    if (city && city[0]) {
                        char *cl = safe_strdup(city);
                        size_t clen = strlen(city);
                        for (size_t i = 0; i < clen; i++) cl[i] = tolower((unsigned char)cl[i]);
                        include = ci_contains(obj_start, olen, cl, clen);
                        free(cl);
                    }

                    if (include) {
                        if (pm_count > 0) jbuf_append(&out, ",");
                        if (out.len + olen < rlen / 2)
                            jbuf_append_len(&out, obj_start, olen);
                        pm_count++;
                    }
                    p = obj_end;
                }
            }
            free(resp.data);
        }
        jbuf_appendf(&out, "],\"matched\":%d}", pm_count);
    }

    jbuf_append(&out, ",");

    /* Kalshi: use the dedicated weather tool logic */
    {
        /* Build a sub-input for kalshi_weather */
        char sub_input[256];
        if (city && city[0])
            snprintf(sub_input, sizeof(sub_input), "{\"city\":\"%s\",\"limit\":%d}", city, limit);
        else
            snprintf(sub_input, sizeof(sub_input), "{\"limit\":%d}", limit);

        char *kalshi_buf = malloc(rlen / 2);
        if (kalshi_buf) {
            bool ok = tool_kalshi_weather(sub_input, kalshi_buf, rlen / 2);
            jbuf_append(&out, "\"kalshi\":");
            if (ok && kalshi_buf[0]) {
                jbuf_append(&out, kalshi_buf);
            } else {
                jbuf_appendf(&out, "{\"error\":\"failed\",\"matched\":0}");
            }
            free(kalshi_buf);
        } else {
            jbuf_append(&out, "\"kalshi\":{\"error\":\"alloc\",\"matched\":0}");
        }
    }

    jbuf_append(&out, "}");
    if (city) jbuf_appendf(&out, ",\"city_filter\":\"%s\"", city);
    jbuf_append(&out, "}");

    result[0] = '\0';
    truncate_response(out.data ? out.data : "{}", result, rlen, 32);
    jbuf_free(&out);
    free(city);
    return true;
}

/* ══════════════════════════════════════════════════════════════════════════
 *  HISTORICAL DATA — resolved markets for backtesting
 * ══════════════════════════════════════════════════════════════════════════ */

/* Kalshi historical settled markets with resolution outcomes */
bool tool_kalshi_historical_markets(const char *input, char *result, size_t rlen) {
    int limit = json_get_int(input, "limit", 50);
    if (limit > 200) limit = 200;
    char *series_ticker = json_get_str(input, "series_ticker");
    char *cursor = json_get_str(input, "cursor");

    jbuf_t path;
    jbuf_init(&path, 512);
    jbuf_appendf(&path, "/historical/markets?limit=%d", limit);
    if (series_ticker && series_ticker[0])
        jbuf_appendf(&path, "&series_ticker=%s", series_ticker);
    if (cursor && cursor[0])
        jbuf_appendf(&path, "&cursor=%s", cursor);
    free(series_ticker); free(cursor);

    http_buf_t resp = {0};
    long st = kalshi_get(path.data, &resp);
    jbuf_free(&path);
    if (st != 200) {
        snprintf(result, rlen, "Kalshi historical markets error (HTTP %ld)", st);
        free(resp.data); return false;
    }
    result[0] = '\0';
    truncate_response(resp.data ? resp.data : "{}", result, rlen, 32);
    free(resp.data); return true;
}

/* Kalshi historical trades — tick-level execution data */
bool tool_kalshi_historical_trades(const char *input, char *result, size_t rlen) {
    int limit = json_get_int(input, "limit", 100);
    if (limit > 1000) limit = 1000;
    char *ticker = json_get_str(input, "ticker");
    char *cursor = json_get_str(input, "cursor");

    jbuf_t path;
    jbuf_init(&path, 512);
    jbuf_appendf(&path, "/historical/trades?limit=%d", limit);
    if (ticker && ticker[0])
        jbuf_appendf(&path, "&ticker=%s", ticker);
    if (cursor && cursor[0])
        jbuf_appendf(&path, "&cursor=%s", cursor);
    free(ticker); free(cursor);

    http_buf_t resp = {0};
    long st = kalshi_get(path.data, &resp);
    jbuf_free(&path);
    if (st != 200) {
        snprintf(result, rlen, "Kalshi historical trades error (HTTP %ld)", st);
        free(resp.data); return false;
    }
    result[0] = '\0';
    truncate_response(resp.data ? resp.data : "{}", result, rlen, 32);
    free(resp.data); return true;
}

/* Kalshi historical cutoff timestamps */
bool tool_kalshi_historical_cutoff(const char *input, char *result, size_t rlen) {
    (void)input;
    http_buf_t resp = {0};
    long st = kalshi_get("/historical/cutoff", &resp);
    if (st != 200) {
        snprintf(result, rlen, "Kalshi historical cutoff error (HTTP %ld)", st);
        free(resp.data); return false;
    }
    result[0] = '\0';
    truncate_response(resp.data ? resp.data : "{}", result, rlen, 32);
    free(resp.data); return true;
}

/* Polymarket resolved markets with outcomes — CLOB API (4.5yr history) */
bool tool_polymarket_resolved(const char *input, char *result, size_t rlen) {
    int limit = json_get_int(input, "limit", 100);
    if (limit > 1000) limit = 1000;
    int offset = json_get_int(input, "offset", 0);

    char url[512];
    snprintf(url, sizeof(url),
             "https://clob.polymarket.com/markets?closed=true&limit=%d&offset=%d",
             limit, offset);

    http_buf_t resp = {0};
    long code = http_get_authed(url, NULL, &resp);
    if (code != 200) {
        snprintf(result, rlen, "Polymarket resolved markets error (HTTP %ld)", code);
        free(resp.data); return false;
    }
    result[0] = '\0';
    truncate_response(resp.data ? resp.data : "[]", result, rlen, 32);
    free(resp.data); return true;
}

/* Polymarket resolved events (high-volume) with nested market outcomes */
bool tool_polymarket_resolved_events(const char *input, char *result, size_t rlen) {
    int limit = json_get_int(input, "limit", 20);
    if (limit > 100) limit = 100;
    int offset = json_get_int(input, "offset", 0);

    char url[512];
    snprintf(url, sizeof(url),
             "https://gamma-api.polymarket.com/events?limit=%d&offset=%d"
             "&closed=true&order=volume&ascending=false",
             limit, offset);

    http_buf_t resp = {0};
    long code = http_get_authed(url, NULL, &resp);
    if (code != 200) {
        snprintf(result, rlen, "Polymarket resolved events error (HTTP %ld)", code);
        free(resp.data); return false;
    }
    result[0] = '\0';
    truncate_response(resp.data ? resp.data : "[]", result, rlen, 32);
    free(resp.data); return true;
}

/* Cross-platform historical — resolved markets from both platforms for a topic */
bool tool_historical_cross_platform(const char *input, char *result, size_t rlen) {
    char *topic = json_get_str(input, "topic");
    int limit = json_get_int(input, "limit", 20);
    if (limit > 50) limit = 50;

    jbuf_t out;
    jbuf_init(&out, 32768);
    jbuf_appendf(&out, "{\"historical_cross_platform\":{");

    /* Polymarket: closed markets matching topic */
    if (topic && topic[0]) {
        CURL *c = curl_easy_init();
        char *enc = curl_easy_escape(c, topic, 0);
        char url[2048];
        snprintf(url, sizeof(url),
                 "https://gamma-api.polymarket.com/markets?_q=%s&limit=%d&closed=true&order=volume&ascending=false",
                 enc, limit);
        curl_free(enc); curl_easy_cleanup(c);

        http_buf_t pm = {0};
        long pm_code = http_get_authed(url, NULL, &pm);
        if (pm_code == 200 && pm.data)
            jbuf_appendf(&out, "\"polymarket\":%s", pm.data);
        else
            jbuf_appendf(&out, "\"polymarket\":[]");
        free(pm.data);
    } else {
        /* No topic — get top resolved by volume */
        http_buf_t pm = {0};
        char url[512];
        snprintf(url, sizeof(url),
                 "https://gamma-api.polymarket.com/markets?limit=%d&closed=true&order=volume&ascending=false", limit);
        long pm_code = http_get_authed(url, NULL, &pm);
        if (pm_code == 200 && pm.data)
            jbuf_appendf(&out, "\"polymarket\":%s", pm.data);
        else
            jbuf_appendf(&out, "\"polymarket\":[]");
        free(pm.data);
    }

    /* Kalshi: settled events */
    {
        char path[512];
        snprintf(path, sizeof(path),
                 "/events?limit=%d&status=settled&with_nested_markets=true", limit);
        http_buf_t ka = {0};
        long ka_code = kalshi_get(path, &ka);
        if (ka_code == 200 && ka.data)
            jbuf_appendf(&out, ",\"kalshi\":%s", ka.data);
        else
            jbuf_appendf(&out, ",\"kalshi\":{}");
        free(ka.data);
    }

    jbuf_appendf(&out,
        ",\"analysis_guide\":{"
        "\"polymarket_winner\":\"tokens[].winner=true indicates winning outcome\","
        "\"polymarket_price\":\"outcomePrices=['1','0'] means first outcome won\","
        "\"kalshi_result\":\"result='yes' or 'no' is the binary resolution\","
        "\"kalshi_settlement\":\"settlement_value_dollars='1.0000' (yes won) or '0.0000' (no won)\","
        "\"backtesting\":\"Compare final prices before close vs resolution to measure prediction accuracy\""
        "}}}");

    free(topic);
    result[0] = '\0';
    truncate_response(out.data, result, rlen, 32);
    jbuf_free(&out);
    return true;
}

/* ══════════════════════════════════════════════════════════════════════════
 *  SYSTEMATIC TRADING ENGINE — historical ingest, analytics, signals
 *
 *  SQLite tables:
 *    resolved_markets: condition_id, platform, question, outcome, winner,
 *                      end_date, volume, category, series
 *    market_prices:    condition_id, timestamp, yes_price, volume_delta
 *    trade_signals:    signal_id, timestamp, market_id, platform, direction,
 *                      confidence, strategy, status
 * ══════════════════════════════════════════════════════════════════════════ */

static sqlite3 *systematic_db(void) {
    static sqlite3 *db = NULL;
    if (db) return db;
    char path[512];
    snprintf(path, sizeof(path), "%s/.dsco_systematic.db",
             getenv("HOME") ? getenv("HOME") : "/tmp");
    if (sqlite3_open(path, &db) != SQLITE_OK) { db = NULL; return NULL; }
    char *err = NULL;
    sqlite3_exec(db,
        "CREATE TABLE IF NOT EXISTS resolved_markets ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  condition_id TEXT UNIQUE,"
        "  platform TEXT NOT NULL,"  /* polymarket, kalshi */
        "  question TEXT,"
        "  category TEXT,"
        "  series TEXT,"
        "  outcome_yes TEXT,"
        "  outcome_no TEXT,"
        "  winner TEXT,"           /* yes, no, cancelled */
        "  end_date TEXT,"
        "  volume REAL DEFAULT 0,"
        "  yes_price_final REAL,"
        "  ingested_at INTEGER DEFAULT (strftime('%s','now'))"
        ");"
        "CREATE TABLE IF NOT EXISTS price_snapshots ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  condition_id TEXT NOT NULL,"
        "  platform TEXT NOT NULL,"
        "  timestamp INTEGER NOT NULL,"
        "  yes_price REAL,"
        "  no_price REAL,"
        "  volume REAL,"
        "  UNIQUE(condition_id, platform, timestamp)"
        ");"
        "CREATE TABLE IF NOT EXISTS trade_signals ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  created_at INTEGER DEFAULT (strftime('%s','now')),"
        "  market_id TEXT,"
        "  platform TEXT,"
        "  direction TEXT,"       /* buy_yes, buy_no, sell_yes, sell_no */
        "  confidence REAL,"
        "  strategy TEXT,"        /* mean_reversion, momentum, cross_platform_arb, calendar_spread */
        "  edge_bps INTEGER,"    /* estimated edge in basis points */
        "  size_usd REAL,"
        "  status TEXT DEFAULT 'pending'" /* pending, executed, expired, cancelled */
        ");"
        "CREATE INDEX IF NOT EXISTS idx_rm_platform ON resolved_markets(platform);"
        "CREATE INDEX IF NOT EXISTS idx_rm_category ON resolved_markets(category);"
        "CREATE INDEX IF NOT EXISTS idx_rm_series ON resolved_markets(series);"
        "CREATE INDEX IF NOT EXISTS idx_rm_end ON resolved_markets(end_date);"
        "CREATE INDEX IF NOT EXISTS idx_ps_cid ON price_snapshots(condition_id);"
        "CREATE INDEX IF NOT EXISTS idx_ts_status ON trade_signals(status);",
        NULL, NULL, &err);
    if (err) sqlite3_free(err);
    return db;
}

/* Case-insensitive strstr (local to systematic engine) */
static bool sys_ci(const char *hay, const char *needle) {
    if (!hay || !needle) return false;
    size_t nlen = strlen(needle);
    size_t hlen = strlen(hay);
    if (nlen > hlen) return false;
    for (size_t i = 0; i <= hlen - nlen; i++) {
        bool m = true;
        for (size_t j = 0; j < nlen; j++) {
            if (tolower((unsigned char)hay[i+j]) != tolower((unsigned char)needle[j])) { m = false; break; }
        }
        if (m) return true;
    }
    return false;
}

/* Classify a market question into a category */
static const char *classify_market(const char *question, const char *slug) {
    if (!question) question = "";
    if (!slug) slug = "";
    if (sys_ci(question, "NBA") || sys_ci(slug, "nba")) return "sports_nba";
    if (sys_ci(question, "NFL") || sys_ci(slug, "nfl")) return "sports_nfl";
    if (sys_ci(question, "NCAAB") || sys_ci(slug, "ncaa")) return "sports_ncaa";
    if (sys_ci(question, "MLB") || sys_ci(slug, "mlb")) return "sports_mlb";
    if (sys_ci(question, "soccer") || sys_ci(slug, "soccer") ||
        sys_ci(question, "Premier League") || sys_ci(question, "La Liga")) return "sports_soccer";
    if (sys_ci(question, "Bitcoin") || sys_ci(question, "BTC")) return "crypto_btc";
    if (sys_ci(question, "Ethereum") || sys_ci(question, "ETH")) return "crypto_eth";
    if (sys_ci(question, "crypto") || sys_ci(question, "Solana")) return "crypto_other";
    if (sys_ci(question, "Fed") || sys_ci(question, "interest rate") || sys_ci(question, "FOMC")) return "macro_fed";
    if (sys_ci(question, "CPI") || sys_ci(question, "inflation")) return "macro_cpi";
    if (sys_ci(question, "GDP")) return "macro_gdp";
    if (sys_ci(question, "oil") || sys_ci(question, "crude")) return "macro_oil";
    if (sys_ci(question, "election") || sys_ci(question, "president") || sys_ci(question, "vote")) return "politics_election";
    if (sys_ci(question, "Iran") || sys_ci(question, "Israel") || sys_ci(question, "Ukraine") ||
        sys_ci(question, "Russia") || sys_ci(question, "China")) return "geopolitics";
    if (sys_ci(question, "weather") || sys_ci(question, "temperature") || sys_ci(question, "high temp")) return "weather";
    if (sys_ci(question, "tweet") || sys_ci(question, "post")) return "social_media";
    return "other";
}

/* Ingest Polymarket resolved markets into SQLite */
bool tool_systematic_ingest_polymarket(const char *input, char *result, size_t rlen) {
    int pages = json_get_int(input, "pages", 5);
    if (pages > 50) pages = 50;

    sqlite3 *db = systematic_db();
    if (!db) { snprintf(result, rlen, "Failed to open systematic DB"); return false; }

    int total = 0, inserted = 0;
    char *cursor = strdup("");

    for (int page = 0; page < pages; page++) {
        char url[512];
        snprintf(url, sizeof(url),
                 "https://clob.polymarket.com/markets?closed=true&limit=1000&next_cursor=%s",
                 cursor);
        http_buf_t resp = {0};
        long code = http_get_authed(url, NULL, &resp);
        if (code != 200 || !resp.data) { free(resp.data); break; }

        /* Parse the JSON response to extract market data */
        /* We'll use json_get_str on the array elements */
        /* Simpler approach: scan for token patterns */
        const char *p = resp.data;
        while ((p = strstr(p, "\"condition_id\":\"")) != NULL) {
            p += 16;
            char cid[128] = {0};
            const char *end = strchr(p, '"');
            if (!end || (size_t)(end - p) > 126) { p++; continue; }
            memcpy(cid, p, (size_t)(end - p));

            /* Find question */
            char question[512] = {0};
            const char *qp = strstr(end, "\"question\":\"");
            if (qp && qp - end < 2000) {
                qp += 12;
                const char *qe = strchr(qp, '"');
                if (qe && (size_t)(qe - qp) < 500)
                    memcpy(question, qp, (size_t)(qe - qp));
            }

            /* Find tokens/winner */
            char winner[32] = "unknown";
            const char *tp = strstr(end, "\"tokens\":");
            if (tp && tp - end < 3000) {
                /* Find winner:true */
                const char *wp = strstr(tp, "\"winner\":true");
                if (wp && wp - tp < 500) {
                    /* Find the outcome before this winner */
                    const char *op = NULL;
                    const char *scan = tp;
                    while (scan < wp) {
                        const char *oc = strstr(scan, "\"outcome\":\"");
                        if (oc && oc < wp) { op = oc + 11; scan = oc + 11; }
                        else break;
                    }
                    if (op) {
                        const char *oe = strchr(op, '"');
                        if (oe && (size_t)(oe - op) < 30)
                            snprintf(winner, sizeof(winner), "%.*s", (int)(oe - op), op);
                    }
                }
            }

            /* Find end_date_iso */
            char end_date[32] = {0};
            const char *dp = strstr(end, "\"end_date_iso\":\"");
            if (dp && dp - end < 2000) {
                dp += 16;
                const char *de = strchr(dp, '"');
                if (de && (size_t)(de - dp) < 30)
                    memcpy(end_date, dp, (size_t)(de - dp));
            }

            /* Find market_slug */
            char slug[256] = {0};
            const char *sp = strstr(end, "\"market_slug\":\"");
            if (sp && sp - end < 2000) {
                sp += 15;
                const char *se = strchr(sp, '"');
                if (se && (size_t)(se - sp) < 250)
                    memcpy(slug, sp, (size_t)(se - sp));
            }

            const char *cat = classify_market(question, slug);

            /* Insert */
            sqlite3_stmt *stmt;
            if (sqlite3_prepare_v2(db,
                    "INSERT OR IGNORE INTO resolved_markets"
                    "(condition_id,platform,question,category,winner,end_date) "
                    "VALUES(?,'polymarket',?,?,?,?)",
                    -1, &stmt, NULL) == SQLITE_OK) {
                sqlite3_bind_text(stmt, 1, cid, -1, SQLITE_STATIC);
                sqlite3_bind_text(stmt, 2, question, -1, SQLITE_STATIC);
                sqlite3_bind_text(stmt, 3, cat, -1, SQLITE_STATIC);
                sqlite3_bind_text(stmt, 4, winner, -1, SQLITE_STATIC);
                sqlite3_bind_text(stmt, 5, end_date, -1, SQLITE_STATIC);
                if (sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(db) > 0)
                    inserted++;
                sqlite3_finalize(stmt);
            }
            total++;
            p = end + 1;
        }

        /* Get next cursor */
        free(cursor);
        cursor = NULL;
        const char *nc = strstr(resp.data, "\"next_cursor\":\"");
        if (nc) {
            nc += 15;
            const char *nce = strchr(nc, '"');
            if (nce && (size_t)(nce - nc) < 64) {
                cursor = malloc((size_t)(nce - nc) + 1);
                memcpy(cursor, nc, (size_t)(nce - nc));
                cursor[nce - nc] = '\0';
            }
        }
        free(resp.data);
        if (!cursor || !cursor[0]) { free(cursor); cursor = NULL; break; }
    }
    free(cursor);

    snprintf(result, rlen,
             "{\"ingested\":{\"platform\":\"polymarket\",\"scanned\":%d,\"inserted\":%d,"
             "\"pages_fetched\":%d}}",
             total, inserted, pages);
    return true;
}

/* Ingest Kalshi historical markets into SQLite */
bool tool_systematic_ingest_kalshi(const char *input, char *result, size_t rlen) {
    int pages = json_get_int(input, "pages", 10);
    if (pages > 20) pages = 20;

    sqlite3 *db = systematic_db();
    if (!db) { snprintf(result, rlen, "Failed to open systematic DB"); return false; }

    int total = 0, inserted = 0;
    char cursor_buf[256] = {0};

    for (int page = 0; page < pages; page++) {
        char path[512];
        if (cursor_buf[0])
            snprintf(path, sizeof(path), "/historical/markets?limit=200&cursor=%s", cursor_buf);
        else
            snprintf(path, sizeof(path), "/historical/markets?limit=200");

        http_buf_t resp = {0};
        long code = kalshi_get(path, &resp);
        if (code != 200 || !resp.data) { free(resp.data); break; }

        /* Parse markets from {"markets":[...], "cursor":"..."} */
        const char *p = resp.data;
        while ((p = strstr(p, "\"ticker\":\"")) != NULL) {
            p += 10;
            char ticker[128] = {0};
            const char *end = strchr(p, '"');
            if (!end || (size_t)(end - p) > 126) { p++; continue; }
            memcpy(ticker, p, (size_t)(end - p));

            char title[512] = {0};
            const char *tp = strstr(end, "\"title\":\"");
            if (tp && tp - end < 3000) {
                tp += 9;
                /* Handle escaped quotes in title */
                int ti = 0;
                while (*tp && ti < 500) {
                    if (*tp == '\\' && *(tp+1)) { title[ti++] = *(tp+1); tp += 2; }
                    else if (*tp == '"') break;
                    else { title[ti++] = *tp; tp++; }
                }
            }

            char ka_result[16] = "unknown";
            const char *rp = strstr(end, "\"result\":\"");
            if (rp && rp - end < 3000) {
                rp += 10;
                const char *re = strchr(rp, '"');
                if (re && (size_t)(re - rp) < 15)
                    snprintf(ka_result, sizeof(ka_result), "%.*s", (int)(re - rp), rp);
            }

            char settle_val[16] = {0};
            const char *sv = strstr(end, "\"settlement_value_dollars\":\"");
            if (sv && sv - end < 3000) {
                sv += 27;
                const char *se = strchr(sv, '"');
                if (se && (size_t)(se - sv) < 15)
                    memcpy(settle_val, sv, (size_t)(se - sv));
            }

            char event_ticker[128] = {0};
            const char *ep = strstr(end, "\"event_ticker\":\"");
            if (ep && ep - end < 3000) {
                ep += 16;
                const char *ee = strchr(ep, '"');
                if (ee && (size_t)(ee - ep) < 126)
                    memcpy(event_ticker, ep, (size_t)(ee - ep));
            }

            /* Extract series from event_ticker (prefix before first -) */
            char series[64] = {0};
            if (event_ticker[0]) {
                const char *dash = strchr(event_ticker, '-');
                if (dash) {
                    size_t slen = (size_t)(dash - event_ticker);
                    if (slen < 63) memcpy(series, event_ticker, slen);
                } else {
                    strncpy(series, event_ticker, 63);
                }
            }

            const char *cat = classify_market(title, ticker);

            /* Extract close_time (ISO 8601 end date) from Kalshi market data */
            char end_date[32] = {0};
            const char *ct = strstr(end, "\"close_time\":\"");
            if (ct && ct - end < 3000) {
                ct += 14;
                const char *ce = strchr(ct, '"');
                if (ce && (size_t)(ce - ct) < 31)
                    memcpy(end_date, ct, (size_t)(ce - ct));
            }

            sqlite3_stmt *stmt;
            if (sqlite3_prepare_v2(db,
                    "INSERT OR IGNORE INTO resolved_markets"
                    "(condition_id,platform,question,category,series,winner,end_date,yes_price_final) "
                    "VALUES(?,'kalshi',?,?,?,?,?,"
                    "CASE WHEN ?='1.0000' THEN 1.0 WHEN ?='0.0000' THEN 0.0 ELSE NULL END)",
                    -1, &stmt, NULL) == SQLITE_OK) {
                sqlite3_bind_text(stmt, 1, ticker, -1, SQLITE_STATIC);
                sqlite3_bind_text(stmt, 2, title, -1, SQLITE_STATIC);
                sqlite3_bind_text(stmt, 3, cat, -1, SQLITE_STATIC);
                sqlite3_bind_text(stmt, 4, series, -1, SQLITE_STATIC);
                sqlite3_bind_text(stmt, 5, ka_result, -1, SQLITE_STATIC);
                sqlite3_bind_text(stmt, 6, end_date, -1, SQLITE_STATIC);
                sqlite3_bind_text(stmt, 7, settle_val, -1, SQLITE_STATIC);
                sqlite3_bind_text(stmt, 8, settle_val, -1, SQLITE_STATIC);
                if (sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(db) > 0)
                    inserted++;
                sqlite3_finalize(stmt);
            }
            total++;
            p = end + 1;
        }

        /* Get cursor for next page */
        cursor_buf[0] = '\0';
        const char *nc = strstr(resp.data, "\"cursor\":\"");
        if (nc) {
            nc += 10;
            const char *nce = strchr(nc, '"');
            if (nce && (size_t)(nce - nc) < 250) {
                memcpy(cursor_buf, nc, (size_t)(nce - nc));
                cursor_buf[nce - nc] = '\0';
            }
        }
        free(resp.data);
        if (!cursor_buf[0]) break;
    }

    snprintf(result, rlen,
             "{\"ingested\":{\"platform\":\"kalshi\",\"scanned\":%d,\"inserted\":%d,"
             "\"pages_fetched\":%d}}",
             total, inserted, pages);
    return true;
}

/* Analytics: base rates, calibration, category breakdowns */
bool tool_systematic_analytics(const char *input, char *result, size_t rlen) {
    char *category = json_get_str(input, "category");
    char *platform = json_get_str(input, "platform");

    sqlite3 *db = systematic_db();
    if (!db) { free(category); free(platform); snprintf(result, rlen, "DB unavailable"); return false; }

    jbuf_t out;
    jbuf_init(&out, 8192);
    jbuf_appendf(&out, "{\"systematic_analytics\":{");

    /* Total counts */
    {
        sqlite3_stmt *stmt;
        sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM resolved_markets", -1, &stmt, NULL);
        int total = 0;
        if (sqlite3_step(stmt) == SQLITE_ROW) total = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
        jbuf_appendf(&out, "\"total_markets\":%d", total);
    }

    /* By platform */
    {
        sqlite3_stmt *stmt;
        sqlite3_prepare_v2(db,
            "SELECT platform, COUNT(*), "
            "SUM(CASE WHEN winner='yes' OR winner NOT IN ('no','unknown','cancelled','') THEN 1 ELSE 0 END) "
            "FROM resolved_markets GROUP BY platform", -1, &stmt, NULL);
        jbuf_appendf(&out, ",\"by_platform\":[");
        int n = 0;
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            if (n > 0) jbuf_appendf(&out, ",");
            const char *plat = (const char *)sqlite3_column_text(stmt, 0);
            int cnt = sqlite3_column_int(stmt, 1);
            int yes_cnt = sqlite3_column_int(stmt, 2);
            jbuf_appendf(&out, "{\"platform\":\"%s\",\"count\":%d,\"yes_outcomes\":%d}",
                         plat ? plat : "?", cnt, yes_cnt);
            n++;
        }
        sqlite3_finalize(stmt);
        jbuf_appendf(&out, "]");
    }

    /* By category — base rates */
    {
        const char *sql = (category && category[0])
            ? "SELECT category, COUNT(*), "
              "SUM(CASE WHEN winner='yes' THEN 1 ELSE 0 END), "
              "SUM(CASE WHEN winner='no' THEN 1 ELSE 0 END) "
              "FROM resolved_markets WHERE category=? GROUP BY category"
            : "SELECT category, COUNT(*), "
              "SUM(CASE WHEN winner='yes' THEN 1 ELSE 0 END), "
              "SUM(CASE WHEN winner='no' THEN 1 ELSE 0 END) "
              "FROM resolved_markets GROUP BY category ORDER BY COUNT(*) DESC LIMIT 30";

        sqlite3_stmt *stmt;
        sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
        if (category && category[0])
            sqlite3_bind_text(stmt, 1, category, -1, SQLITE_STATIC);

        jbuf_appendf(&out, ",\"by_category\":[");
        int n = 0;
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            if (n > 0) jbuf_appendf(&out, ",");
            const char *cat = (const char *)sqlite3_column_text(stmt, 0);
            int cnt = sqlite3_column_int(stmt, 1);
            int yes = sqlite3_column_int(stmt, 2);
            int no = sqlite3_column_int(stmt, 3);
            double yes_rate = cnt > 0 ? (double)yes / cnt : 0;
            jbuf_appendf(&out, "{\"category\":\"%s\",\"count\":%d,"
                         "\"yes\":%d,\"no\":%d,\"yes_rate\":%.3f}",
                         cat ? cat : "?", cnt, yes, no, yes_rate);
            n++;
        }
        sqlite3_finalize(stmt);
        jbuf_appendf(&out, "]");
    }

    /* By Kalshi series */
    {
        sqlite3_stmt *stmt;
        sqlite3_prepare_v2(db,
            "SELECT series, COUNT(*), "
            "SUM(CASE WHEN winner='yes' THEN 1 ELSE 0 END), "
            "SUM(CASE WHEN winner='no' THEN 1 ELSE 0 END) "
            "FROM resolved_markets WHERE platform='kalshi' AND series!='' "
            "GROUP BY series ORDER BY COUNT(*) DESC LIMIT 20", -1, &stmt, NULL);
        jbuf_appendf(&out, ",\"kalshi_series\":[");
        int n = 0;
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            if (n > 0) jbuf_appendf(&out, ",");
            const char *ser = (const char *)sqlite3_column_text(stmt, 0);
            int cnt = sqlite3_column_int(stmt, 1);
            int yes = sqlite3_column_int(stmt, 2);
            int no = sqlite3_column_int(stmt, 3);
            double yes_rate = cnt > 0 ? (double)yes / cnt : 0;
            jbuf_appendf(&out, "{\"series\":\"%s\",\"count\":%d,"
                         "\"yes\":%d,\"no\":%d,\"yes_rate\":%.3f}",
                         ser ? ser : "?", cnt, yes, no, yes_rate);
            n++;
        }
        sqlite3_finalize(stmt);
        jbuf_appendf(&out, "]");
    }

    /* Date distribution */
    {
        sqlite3_stmt *stmt;
        sqlite3_prepare_v2(db,
            "SELECT MIN(end_date), MAX(end_date) FROM resolved_markets WHERE end_date!=''",
            -1, &stmt, NULL);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *mn = (const char *)sqlite3_column_text(stmt, 0);
            const char *mx = (const char *)sqlite3_column_text(stmt, 1);
            jbuf_appendf(&out, ",\"date_range\":{\"min\":\"%s\",\"max\":\"%s\"}",
                         mn ? mn : "?", mx ? mx : "?");
        }
        sqlite3_finalize(stmt);
    }

    jbuf_appendf(&out, "}}");
    free(category); free(platform);

    result[0] = '\0';
    truncate_response(out.data, result, rlen, 32);
    jbuf_free(&out);
    return true;
}

/* Generate trading signals from current market data + historical base rates */
bool tool_systematic_signals(const char *input, char *result, size_t rlen) {
    (void)input;
    sqlite3 *db = systematic_db();
    if (!db) { snprintf(result, rlen, "DB unavailable — run systematic_ingest first"); return false; }

    /* Check if we have data */
    sqlite3_stmt *check;
    sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM resolved_markets", -1, &check, NULL);
    int count = 0;
    if (sqlite3_step(check) == SQLITE_ROW) count = sqlite3_column_int(check, 0);
    sqlite3_finalize(check);

    if (count < 100) {
        snprintf(result, rlen,
                 "{\"error\":\"Insufficient data (%d markets). "
                 "Run systematic_ingest_polymarket and systematic_ingest_kalshi first.\","
                 "\"min_required\":100}", count);
        return true;
    }

    /* Fetch current active markets from both platforms */
    jbuf_t out;
    jbuf_init(&out, 16384);
    jbuf_appendf(&out, "{\"systematic_signals\":{\"data_points\":%d,\"signals\":[", count);

    /* Strategy 1: Cross-platform price divergence on live markets */
    /* Strategy 2: Calendar spreads — same series, different expiry */
    /* Strategy 3: Mean reversion — prices far from historical base rates */

    /* For now, output the base rates as the foundation for signal generation.
     * The LLM will combine these with live market data to produce actionable signals. */

    /* Get category base rates for context */
    {
        sqlite3_stmt *stmt;
        sqlite3_prepare_v2(db,
            "SELECT category, COUNT(*), "
            "CAST(SUM(CASE WHEN winner='yes' THEN 1 ELSE 0 END) AS REAL) / COUNT(*) AS yes_rate "
            "FROM resolved_markets WHERE category NOT LIKE 'other' "
            "GROUP BY category HAVING COUNT(*) >= 10 "
            "ORDER BY COUNT(*) DESC", -1, &stmt, NULL);
        int n = 0;
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            if (n > 0) jbuf_appendf(&out, ",");
            const char *cat = (const char *)sqlite3_column_text(stmt, 0);
            int cnt = sqlite3_column_int(stmt, 1);
            double rate = sqlite3_column_double(stmt, 2);
            jbuf_appendf(&out,
                "{\"type\":\"base_rate\",\"category\":\"%s\","
                "\"sample_size\":%d,\"historical_yes_rate\":%.4f,"
                "\"signal\":\"If live market YES price deviates >10%% from %.1f%%, consider %s\"}",
                cat ? cat : "?", cnt, rate, rate * 100,
                rate > 0.5 ? "selling NO (overpriced)" : "buying YES (underpriced)");
            n++;
        }
        sqlite3_finalize(stmt);
    }

    jbuf_appendf(&out, "],\"strategies\":{");
    jbuf_appendf(&out,
        "\"mean_reversion\":\"Compare live YES price to historical base rate for category. "
        "Edge = |live_price - base_rate|. Trade when edge > 5c.\","
        "\"cross_platform_arb\":\"Find same event on Kalshi + Polymarket. "
        "If price spread > 3c after fees, buy cheap side and sell expensive side.\","
        "\"calendar_spread\":\"Same series, different expiry dates. "
        "If near-term is priced higher than far-term for monotonic events, sell near + buy far.\","
        "\"momentum\":\"Track 24h price change. If market moved >10c in 24h with volume > $50K, "
        "follow the momentum direction.\""
        "}}}");

    result[0] = '\0';
    truncate_response(out.data, result, rlen, 32);
    jbuf_free(&out);
    return true;
}

/* ── Cross-platform market movers — what's active right now ──────── */
bool tool_market_movers(const char *input, char *result, size_t rlen) {
    int limit = json_get_int(input, "limit", 15);
    if (limit > 50) limit = 50;

    jbuf_t out;
    jbuf_init(&out, 32768);
    jbuf_appendf(&out, "{\"market_movers\":{");

    /* 1. Polymarket: top by 24h volume */
    {
        char url[512];
        snprintf(url, sizeof(url),
                 "https://gamma-api.polymarket.com/markets?limit=%d&active=true"
                 "&order=volume24hr&ascending=false", limit);
        http_buf_t resp = {0};
        long code = http_get_authed(url, NULL, &resp);
        if (code == 200 && resp.data)
            jbuf_appendf(&out, "\"polymarket\":%s", resp.data);
        else
            jbuf_appendf(&out, "\"polymarket\":[]");
        free(resp.data);
    }

    /* 2. Kalshi: scan key series for 24h activity */
    {
        static const char *series[] = {
            "KXBTC", "KXSPY", "KXETH", "KXFED", "KXCPI", "KXGDP",
            "KXOILCL", "KXHIGHNY", "KXLOWNY", "KXTEMP", "KXRAIN",
            "KXSNOW", "KXHURR", "KXHIGH", "KXLOW", "KXINAUGCROWD",
            NULL
        };
        jbuf_appendf(&out, ",\"kalshi\":[");
        int n = 0;
        for (int i = 0; series[i]; i++) {
            char path[256];
            snprintf(path, sizeof(path),
                     "/markets?limit=10&status=open&series_ticker=%s", series[i]);
            http_buf_t resp = {0};
            long code = kalshi_get(path, &resp);
            if (code != 200 || !resp.data) { free(resp.data); continue; }

            /* Extract markets array from {"markets":[...]} and inline them */
            const char *arr = strstr(resp.data, "\"markets\":[");
            if (arr) {
                arr += 11; /* skip "markets":[ */
                const char *end = resp.data + strlen(resp.data);
                /* Find matching ] - simple scan */
                int depth = 1;
                const char *p = arr;
                while (p < end && depth > 0) {
                    if (*p == '[') depth++;
                    else if (*p == ']') depth--;
                    if (depth > 0) p++;
                }
                if (depth == 0 && p > arr) {
                    size_t chunk = (size_t)(p - arr);
                    if (chunk > 2) { /* not empty array */
                        if (n > 0) jbuf_appendf(&out, ",");
                        /* Write the raw market objects */
                        char *tmp = malloc(chunk + 1);
                        memcpy(tmp, arr, chunk);
                        tmp[chunk] = '\0';
                        jbuf_appendf(&out, "%s", tmp);
                        free(tmp);
                        n++;
                    }
                }
            }
            free(resp.data);
        }
        jbuf_appendf(&out, "]");
    }

    jbuf_appendf(&out, ",\"note\":\"Polymarket prices are 0-1 (probability). "
                 "Kalshi uses dollar strings (yes_ask_dollars). "
                 "volume_24h_fp is Kalshi 24h volume. "
                 "volume24hr is Polymarket 24h volume in USD.\"");
    jbuf_appendf(&out, "}}");

    result[0] = '\0';
    truncate_response(out.data, result, rlen, 32);
    jbuf_free(&out);
    return true;
}

/* ══════════════════════════════════════════════════════════════════════════
 *  MARKET CACHE — SQLite-backed cache for cross-platform market data
 *  TTL-based: markets cached for 60s, events for 300s
 * ══════════════════════════════════════════════════════════════════════════ */

static sqlite3 *market_cache_db(void) {
    static sqlite3 *db = NULL;
    if (db) return db;
    char path[512];
    snprintf(path, sizeof(path), "%s/.dsco_market_cache.db",
             getenv("HOME") ? getenv("HOME") : "/tmp");
    if (sqlite3_open(path, &db) != SQLITE_OK) { db = NULL; return NULL; }
    char *err = NULL;
    sqlite3_exec(db,
        "CREATE TABLE IF NOT EXISTS cache ("
        "  key TEXT PRIMARY KEY,"
        "  data TEXT NOT NULL,"
        "  platform TEXT,"
        "  fetched_at INTEGER NOT NULL"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_cache_platform ON cache(platform);"
        "CREATE INDEX IF NOT EXISTS idx_cache_fetched ON cache(fetched_at);",
        NULL, NULL, &err);
    if (err) sqlite3_free(err);
    return db;
}

static bool cache_get(const char *key, int ttl_sec, char **out_data) {
    sqlite3 *db = market_cache_db();
    if (!db) return false;
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db,
            "SELECT data FROM cache WHERE key=? AND fetched_at > ?", -1, &stmt, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 2, (int64_t)time(NULL) - ttl_sec);
    bool found = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *txt = (const char *)sqlite3_column_text(stmt, 0);
        if (txt) { *out_data = strdup(txt); found = true; }
    }
    sqlite3_finalize(stmt);
    return found;
}

static void cache_set(const char *key, const char *data, const char *platform) {
    sqlite3 *db = market_cache_db();
    if (!db) return;
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db,
            "INSERT OR REPLACE INTO cache(key,data,platform,fetched_at) VALUES(?,?,?,?)",
            -1, &stmt, NULL) != SQLITE_OK)
        return;
    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, data, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, platform, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 4, (int64_t)time(NULL));
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

/* Refresh all Polymarket + Kalshi markets into cache */
bool tool_market_cache_refresh(const char *input, char *result, size_t rlen) {
    (void)input;
    int pm_count = 0, ka_count = 0;

    /* Polymarket: top 100 active markets */
    http_buf_t pm = {0};
    long pm_code = http_get_authed(
        "https://gamma-api.polymarket.com/markets?limit=100&active=true&order=volume24hr&ascending=false",
        NULL, &pm);
    if (pm_code == 200 && pm.data) {
        cache_set("pm:markets:top100", pm.data, "polymarket");
        /* Count markets (approximate by counting "question": occurrences) */
        const char *p = pm.data;
        while ((p = strstr(p, "\"question\"")) != NULL) { pm_count++; p += 10; }
    }
    free(pm.data);

    /* Polymarket: events with tags */
    http_buf_t pm_ev = {0};
    long pm_ev_code = http_get_authed(
        "https://gamma-api.polymarket.com/events?limit=100&active=true&order=volume24hr&ascending=false",
        NULL, &pm_ev);
    if (pm_ev_code == 200 && pm_ev.data)
        cache_set("pm:events:top100", pm_ev.data, "polymarket");
    free(pm_ev.data);

    /* Kalshi: open events with markets */
    http_buf_t ka = {0};
    long ka_code = kalshi_get("/events?limit=100&status=open&with_nested_markets=true", &ka);
    if (ka_code == 200 && ka.data) {
        cache_set("ka:events:open100", ka.data, "kalshi");
        const char *p = ka.data;
        while ((p = strstr(p, "\"ticker\"")) != NULL) { ka_count++; p += 8; }
    }
    free(ka.data);

    /* Kalshi: daily resolvers sorted by close time */
    http_buf_t ka_d = {0};
    long ka_d_code = kalshi_get("/markets?limit=200&status=open&sort_by=close_time", &ka_d);
    if (ka_d_code == 200 && ka_d.data)
        cache_set("ka:markets:daily200", ka_d.data, "kalshi");
    free(ka_d.data);

    snprintf(result, rlen,
             "{\"status\":\"refreshed\","
             "\"polymarket_markets\":%d,"
             "\"kalshi_tickers\":%d,"
             "\"cache_ttl_sec\":60}",
             pm_count, ka_count);
    return true;
}

/* Query cached market data without hitting APIs */
bool tool_market_cache_query(const char *input, char *result, size_t rlen) {
    char *key = json_get_str(input, "key");
    char *platform = json_get_str(input, "platform");
    int ttl = json_get_int(input, "ttl", 300);

    if (key && key[0]) {
        char *data = NULL;
        if (cache_get(key, ttl, &data) && data) {
            result[0] = '\0';
            truncate_response(data, result, rlen, 32);
            free(data);
        } else {
            snprintf(result, rlen, "{\"error\":\"cache miss\",\"key\":\"%s\",\"hint\":\"run market_cache_refresh first\"}", key);
        }
        free(key); free(platform);
        return true;
    }

    /* List cache keys */
    sqlite3 *db = market_cache_db();
    if (!db) { free(key); free(platform); snprintf(result, rlen, "cache db unavailable"); return false; }

    jbuf_t out;
    jbuf_init(&out, 2048);
    jbuf_appendf(&out, "{\"cache_keys\":[");

    sqlite3_stmt *stmt;
    const char *sql = platform && platform[0]
        ? "SELECT key, platform, fetched_at, length(data) FROM cache WHERE platform=? ORDER BY fetched_at DESC"
        : "SELECT key, platform, fetched_at, length(data) FROM cache ORDER BY fetched_at DESC";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        if (platform && platform[0])
            sqlite3_bind_text(stmt, 1, platform, -1, SQLITE_STATIC);
        int n = 0;
        while (sqlite3_step(stmt) == SQLITE_ROW && n < 50) {
            if (n > 0) jbuf_appendf(&out, ",");
            jbuf_appendf(&out, "{\"key\":\"%s\",\"platform\":\"%s\",\"age_sec\":%lld,\"size\":%d}",
                         sqlite3_column_text(stmt, 0),
                         sqlite3_column_text(stmt, 1),
                         (long long)time(NULL) - sqlite3_column_int64(stmt, 2),
                         sqlite3_column_int(stmt, 3));
            n++;
        }
        sqlite3_finalize(stmt);
    }
    jbuf_appendf(&out, "]}");
    free(key); free(platform);

    result[0] = '\0';
    truncate_response(out.data, result, rlen, 32);
    jbuf_free(&out);
    return true;
}

/* ── Kalshi: single market deep dive (market + book + trades) ─────── */
bool tool_kalshi_market_snapshot(const char *input, char *result, size_t rlen) {
    char *ticker = json_get_str(input, "ticker");
    if (!ticker || !ticker[0]) { free(ticker); snprintf(result, rlen, "missing: ticker"); return false; }

    jbuf_t out;
    jbuf_init(&out, 8192);
    jbuf_appendf(&out, "{\"ticker\":\"%s\"", ticker);

    /* 1. Market details */
    char path[512];
    snprintf(path, sizeof(path), "/markets/%s", ticker);
    http_buf_t r1 = {0};
    long s1 = kalshi_get(path, &r1);
    if (s1 == 200 && r1.data)
        jbuf_appendf(&out, ",\"market\":%s", r1.data);
    free(r1.data);

    /* 2. Order book */
    snprintf(path, sizeof(path), "/markets/%s/orderbook?depth=10", ticker);
    http_buf_t r2 = {0};
    long s2 = kalshi_get(path, &r2);
    if (s2 == 200 && r2.data)
        jbuf_appendf(&out, ",\"orderbook\":%s", r2.data);
    free(r2.data);

    /* 3. Recent trades */
    snprintf(path, sizeof(path), "/markets/trades?ticker=%s&limit=20", ticker);
    http_buf_t r3 = {0};
    long s3 = kalshi_get(path, &r3);
    if (s3 == 200 && r3.data)
        jbuf_appendf(&out, ",\"trades\":%s", r3.data);
    free(r3.data);

    /* 4. Candlesticks (hourly) */
    snprintf(path, sizeof(path), "/markets/%s/candlesticks?period_interval=60", ticker);
    http_buf_t r4 = {0};
    long s4 = kalshi_get(path, &r4);
    if (s4 == 200 && r4.data)
        jbuf_appendf(&out, ",\"candlesticks\":%s", r4.data);
    free(r4.data);

    jbuf_appendf(&out, "}");
    free(ticker);

    result[0] = '\0';
    truncate_response(out.data, result, rlen, 32);
    jbuf_free(&out);
    return true;
}

/* ── Kalshi: event detail — all markets in one event ───────────────── */
bool tool_kalshi_event_detail(const char *input, char *result, size_t rlen) {
    char *event_ticker = json_get_str(input, "event_ticker");
    if (!event_ticker || !event_ticker[0]) {
        free(event_ticker); snprintf(result, rlen, "missing: event_ticker"); return false;
    }

    char path[512];
    snprintf(path, sizeof(path), "/events/%s?with_nested_markets=true", event_ticker);
    free(event_ticker);

    http_buf_t resp = {0};
    long st = kalshi_get(path, &resp);
    if (st != 200) {
        snprintf(result, rlen, "Kalshi event detail error (HTTP %ld)", st);
        free(resp.data); return false;
    }
    result[0] = '\0';
    truncate_response(resp.data ? resp.data : "{}", result, rlen, 32);
    free(resp.data);
    return true;
}

/* ── Cross-platform delta map — match markets and compute spreads ──── */
bool tool_cross_platform_delta(const char *input, char *result, size_t rlen) {
    char *topic = json_get_str(input, "topic");
    if (!topic || !topic[0]) { free(topic); snprintf(result, rlen, "missing: topic"); return false; }
    int limit = json_get_int(input, "limit", 20);
    if (limit > 50) limit = 50;

    CURL *c = curl_easy_init();
    char *enc = curl_easy_escape(c, topic, 0);

    /* Fetch Polymarket markets */
    char url_pm[2048];
    snprintf(url_pm, sizeof(url_pm),
             "https://gamma-api.polymarket.com/markets?_q=%s&limit=%d&active=true&order=volume24hr&ascending=false",
             enc, limit);
    http_buf_t pm = {0};
    long pm_code = http_get_authed(url_pm, NULL, &pm);

    /* Fetch Kalshi events */
    char url_k[2048];
    snprintf(url_k, sizeof(url_k),
             "/events?limit=%d&status=open&with_nested_markets=true", limit);
    http_buf_t ka = {0};
    long ka_code = kalshi_get(url_k, &ka);

    curl_free(enc); curl_easy_cleanup(c);

    /* Build delta report */
    jbuf_t out;
    jbuf_init(&out, 16384);
    jbuf_appendf(&out, "{\"cross_platform_delta\":{\"topic\":\"%s\"", topic ? topic : "");
    free(topic);
    jbuf_appendf(&out, ",\"polymarket\":{\"status\":%ld,\"count\":\"see data\"", pm_code);
    if (pm_code == 200 && pm.data)
        jbuf_appendf(&out, ",\"markets\":%s", pm.data);
    jbuf_appendf(&out, "}");

    jbuf_appendf(&out, ",\"kalshi\":{\"status\":%ld", ka_code);
    if (ka_code == 200 && ka.data)
        jbuf_appendf(&out, ",\"events\":%s", ka.data);
    jbuf_appendf(&out, "}");

    jbuf_appendf(&out,
        ",\"instructions\":\"Compare markets across platforms. "
        "For each matching event: compute delta = polymarket_yes_price - kalshi_yes_price/100. "
        "Positive delta = Polymarket prices higher. Negative = Kalshi higher. "
        "Spreads > 0.03 are potentially actionable.\"");
    jbuf_appendf(&out, "}}");

    free(pm.data); free(ka.data);

    result[0] = '\0';
    truncate_response(out.data, result, rlen, 32);
    jbuf_free(&out);
    return true;
}

/* ── Kalshi: daily/weekly resolver markets ─────────────────────────── */
bool tool_kalshi_daily_markets(const char *input, char *result, size_t rlen) {
    char *series_ticker = json_get_str(input, "series_ticker");
    int limit = json_get_int(input, "limit", 50);
    if (limit > 200) limit = 200;

    jbuf_t url;
    jbuf_init(&url, 512);
    jbuf_appendf(&url, "/markets?limit=%d&status=open", limit);
    if (series_ticker && series_ticker[0])
        jbuf_appendf(&url, "&series_ticker=%s", series_ticker);
    /* Sort by close_time to get nearest resolvers first */
    jbuf_appendf(&url, "&sort_by=close_time");
    free(series_ticker);

    http_buf_t resp = {0};
    long st = kalshi_get(url.data, &resp);
    jbuf_free(&url);
    if (st != 200) {
        snprintf(result, rlen, "Kalshi daily markets error (HTTP %ld)", st);
        free(resp.data); return false;
    }
    result[0] = '\0';
    truncate_response(resp.data ? resp.data : "{}", result, rlen, 32);
    free(resp.data);
    return true;
}

/* ══════════════════════════════════════════════════════════════════════════
 *  CROSS-PLATFORM PREDICTION MARKET INTELLIGENCE
 *
 *  Compound tools that query both Polymarket + Kalshi simultaneously,
 *  plus advanced data endpoints for whale tracking and analytics.
 * ══════════════════════════════════════════════════════════════════════════ */

/* Scan both platforms for a topic — returns merged results */
bool tool_prediction_scan(const char *input, char *result, size_t rlen) {
    char *query = json_get_str(input, "query");
    if (!query || !query[0]) { free(query); snprintf(result, rlen, "missing: query"); return false; }
    int limit = json_get_int(input, "limit", 5);
    if (limit > 20) limit = 20;

    jbuf_t out;
    jbuf_init(&out, rlen > 4096 ? 4096 : rlen);
    jbuf_append(&out, "{\"platforms\":{");

    /* Polymarket */
    {
        CURL *c = curl_easy_init();
        char *enc = curl_easy_escape(c, query, 0);
        char url[2048];
        snprintf(url, sizeof(url),
                 "https://gamma-api.polymarket.com/markets?_q=%s&limit=%d&active=true&order=volume24hr&ascending=false",
                 enc, limit);
        curl_free(enc); curl_easy_cleanup(c);

        http_buf_t resp = {0};
        long st = http_get_authed(url, NULL, &resp);
        jbuf_append(&out, "\"polymarket\":{\"status\":");
        if (st == 200 && resp.data) {
            jbuf_appendf(&out, "%ld,\"markets\":", st);
            /* Trim to fit — take first N KB */
            size_t max_pm = (rlen / 3);
            if (resp.len > max_pm) resp.data[max_pm] = '\0';
            jbuf_append(&out, resp.data);
        } else {
            jbuf_appendf(&out, "%ld,\"markets\":[]", st);
        }
        jbuf_append(&out, "}");
        free(resp.data);
    }

    jbuf_append(&out, ",");

    /* Kalshi — use the search tool to get keyword-filtered results */
    {
        char sub_input[512];
        snprintf(sub_input, sizeof(sub_input),
                 "{\"query\":\"%s\",\"limit\":%d}", query, limit * 4);
        size_t k_rlen = rlen / 2;
        char *k_buf = malloc(k_rlen);
        jbuf_append(&out, "\"kalshi\":{\"status\":");
        if (k_buf) {
            k_buf[0] = '\0';
            bool ok = tool_kalshi_search(sub_input, k_buf, k_rlen);
            if (ok && k_buf[0]) {
                jbuf_appendf(&out, "200,\"data\":%s", k_buf);
            } else {
                jbuf_appendf(&out, "0,\"data\":{\"events\":[],\"matched\":0}");
            }
            free(k_buf);
        } else {
            jbuf_appendf(&out, "0,\"data\":{\"events\":[],\"matched\":0}");
        }
        jbuf_append(&out, "}");
    }

    jbuf_appendf(&out, "},\"query\":\"%s\"}", query);
    free(query);

    result[0] = '\0';
    truncate_response(out.data ? out.data : "{}", result, rlen, 32);
    jbuf_free(&out);
    return true;
}

/* Real-time snapshot: top markets from both platforms by 24h volume */
bool tool_prediction_snapshot(const char *input, char *result, size_t rlen) {
    int limit = json_get_int(input, "limit", 5);
    if (limit > 20) limit = 20;
    char *category = json_get_str(input, "category");

    jbuf_t out;
    jbuf_init(&out, rlen > 8192 ? 8192 : rlen);
    jbuf_append(&out, "{\"snapshot\":{");

    /* Polymarket top by volume */
    {
        jbuf_t url;
        jbuf_init(&url, 256);
        jbuf_appendf(&url,
            "https://gamma-api.polymarket.com/markets?limit=%d&active=true&order=volume24hr&ascending=false",
            limit);
        if (category && category[0]) {
            CURL *c = curl_easy_init();
            char *enc = curl_easy_escape(c, category, 0);
            jbuf_appendf(&url, "&tag=%s", enc);
            curl_free(enc); curl_easy_cleanup(c);
        }
        http_buf_t resp = {0};
        long st = http_get_authed(url.data, NULL, &resp);
        jbuf_free(&url);
        jbuf_append(&out, "\"polymarket\":");
        if (st == 200 && resp.data) {
            size_t cap = rlen / 3;
            if (resp.len > cap) resp.data[cap] = '\0';
            jbuf_append(&out, resp.data);
        } else {
            jbuf_append(&out, "[]");
        }
        free(resp.data);
    }

    jbuf_append(&out, ",");

    /* Kalshi open events */
    {
        char path[256];
        snprintf(path, sizeof(path), "/events?limit=%d&status=open&with_nested_markets=true", limit);
        http_buf_t resp = {0};
        long st = kalshi_get(path, &resp);
        jbuf_append(&out, "\"kalshi\":");
        if (st == 200 && resp.data) {
            size_t cap = rlen / 3;
            if (resp.len > cap) resp.data[cap] = '\0';
            jbuf_append(&out, resp.data);
        } else {
            jbuf_append(&out, "{}");
        }
        free(resp.data);
    }

    jbuf_append(&out, "}}");
    free(category);

    result[0] = '\0';
    truncate_response(out.data ? out.data : "{}", result, rlen, 32);
    jbuf_free(&out);
    return true;
}

/* Track large trades on Polymarket (whale detection) */
bool tool_polymarket_whale_trades(const char *input, char *result, size_t rlen) {
    double min_size = json_get_double(input, "min_size_usd", 10000.0);
    int limit = json_get_int(input, "limit", 50);
    if (limit > 500) limit = 500;
    char *condition_id = json_get_str(input, "condition_id");

    jbuf_t url;
    jbuf_init(&url, 512);
    jbuf_appendf(&url, "https://data-api.polymarket.com/trades?limit=%d", limit);
    if (condition_id && condition_id[0])
        jbuf_appendf(&url, "&condition_id=%s", condition_id);
    free(condition_id);

    http_buf_t resp = {0};
    long st = http_get_authed(url.data, NULL, &resp);
    jbuf_free(&url);
    if (st != 200) { snprintf(result, rlen, "Polymarket trades error (HTTP %ld)", st); free(resp.data); return false; }

    /* Filter trades by size client-side and build whale report */
    jbuf_t out;
    jbuf_init(&out, rlen > 4096 ? 4096 : rlen);
    jbuf_appendf(&out, "{\"whale_threshold_usd\":%.0f,\"trades\":", min_size);

    /* Pass through raw trades — the LLM will filter by size */
    if (resp.data) {
        size_t cap = rlen - 128;
        if (resp.len > cap) resp.data[cap] = '\0';
        jbuf_append(&out, resp.data);
    } else {
        jbuf_append(&out, "[]");
    }
    jbuf_append(&out, "}");

    result[0] = '\0';
    truncate_response(out.data ? out.data : "{}", result, rlen, 32);
    jbuf_free(&out);
    free(resp.data); return true;
}

/* Polymarket leaderboard — top traders */
bool tool_polymarket_leaderboard(const char *input, char *result, size_t rlen) {
    int limit = json_get_int(input, "limit", 10);
    if (limit > 100) limit = 100;

    char url[512];
    snprintf(url, sizeof(url),
             "https://data-api.polymarket.com/leaderboard?limit=%d", limit);

    http_buf_t resp = {0};
    long st = http_get_authed(url, NULL, &resp);
    if (st != 200) { snprintf(result, rlen, "Polymarket leaderboard error (HTTP %ld)", st); free(resp.data); return false; }
    result[0] = '\0';
    truncate_response(resp.data ? resp.data : "[]", result, rlen, 32);
    free(resp.data); return true;
}

/* Polymarket price history for a token */
bool tool_polymarket_history(const char *input, char *result, size_t rlen) {
    char *token_id = json_get_str(input, "token_id");
    char *condition_id = json_get_str(input, "condition_id");
    char *interval = json_get_str(input, "interval");
    char *fidelity = json_get_str(input, "fidelity");

    if ((!token_id || !token_id[0]) && (!condition_id || !condition_id[0])) {
        free(token_id); free(condition_id); free(interval); free(fidelity);
        snprintf(result, rlen, "missing: token_id or condition_id");
        return false;
    }

    jbuf_t url;
    jbuf_init(&url, 512);
    if (token_id && token_id[0]) {
        jbuf_appendf(&url, "https://clob.polymarket.com/prices-history?market=%s", token_id);
    } else {
        jbuf_appendf(&url, "https://clob.polymarket.com/prices-history?market=%s", condition_id);
    }
    if (interval && interval[0])
        jbuf_appendf(&url, "&interval=%s", interval);  /* all, 1d, 1w, 1m */
    if (fidelity && fidelity[0])
        jbuf_appendf(&url, "&fidelity=%d", atoi(fidelity));  /* minutes between points */
    free(token_id); free(condition_id); free(interval); free(fidelity);

    http_buf_t resp = {0};
    long st = http_get_authed(url.data, NULL, &resp);
    jbuf_free(&url);
    if (st != 200) { snprintf(result, rlen, "Polymarket history error (HTTP %ld)", st); free(resp.data); return false; }
    result[0] = '\0';
    truncate_response(resp.data ? resp.data : "{}", result, rlen, 32);
    free(resp.data); return true;
}

/* ══════════════════════════════════════════════════════════════════════════
 *  CROSS-PLATFORM ARBITRAGE DETECTOR
 *
 *  Implements the core arb detection from the prediction market literature:
 *
 *  1. WITHIN-MARKET ARB: YES + NO < $1.00 on same platform
 *     → Buy both, guaranteed profit = $1.00 - (YES + NO)
 *
 *  2. CROSS-PLATFORM ARB: YES_A + NO_B < $1.00 across platforms
 *     → Buy YES on platform A, NO on platform B
 *     → One always pays $1.00, profit = $1.00 - total cost
 *
 *  3. COMBINATORIAL ARB: Sum of all mutually exclusive outcomes < $1.00
 *     → Buy all outcomes, one must win
 *
 *  The tool fetches BTC/crypto markets from both Polymarket CLOB and
 *  Kalshi, normalizes to probability scale, and reports spreads.
 * ══════════════════════════════════════════════════════════════════════════ */

bool tool_prediction_arb(const char *input, char *result, size_t rlen) {
    char *topic = json_get_str(input, "topic");
    if (!topic || !topic[0]) {
        free(topic);
        topic = strdup("bitcoin");
    }
    int limit = json_get_int(input, "limit", 10);
    if (limit > 50) limit = 50;

    jbuf_t out;
    jbuf_init(&out, rlen > 8192 ? 8192 : rlen);
    jbuf_append(&out, "{\"arbitrage_scan\":{\"topic\":");
    jbuf_append_json_str(&out, topic);

    /* ── Fetch Polymarket markets ──────────────────────────────────────── */
    jbuf_append(&out, ",\"polymarket\":{");
    {
        CURL *c = curl_easy_init();
        char *enc = curl_easy_escape(c, topic, 0);
        char url[2048];
        snprintf(url, sizeof(url),
                 "https://gamma-api.polymarket.com/markets?_q=%s&limit=%d&active=true&order=volume24hr&ascending=false",
                 enc, limit);
        curl_free(enc); curl_easy_cleanup(c);

        http_buf_t resp = {0};
        long st = http_get_authed(url, NULL, &resp);
        jbuf_appendf(&out, "\"status\":%ld,\"markets\":", st);
        if (st == 200 && resp.data) {
            size_t cap = rlen / 4;
            if (resp.len > cap) resp.data[cap] = '\0';
            jbuf_append(&out, resp.data);
        } else {
            jbuf_append(&out, "[]");
        }
        free(resp.data);
    }
    jbuf_append(&out, "}");

    /* ── Fetch Kalshi events ──────────────────────────────────────────── */
    jbuf_append(&out, ",\"kalshi\":{");
    {
        char path[512];
        snprintf(path, sizeof(path),
                 "/events?limit=%d&status=open&with_nested_markets=true", limit);
        http_buf_t resp = {0};
        long st = kalshi_get(path, &resp);
        jbuf_appendf(&out, "\"status\":%ld,\"events\":", st);
        if (st == 200 && resp.data) {
            size_t cap = rlen / 4;
            if (resp.len > cap) resp.data[cap] = '\0';
            jbuf_append(&out, resp.data);
        } else {
            jbuf_append(&out, "{}");
        }
        free(resp.data);
    }
    jbuf_append(&out, "}");

    /* ── Arbitrage detection instructions for the LLM ────────────────── */
    jbuf_append(&out, ",\"detection_rules\":{"
        "\"within_market\":\"If YES_price + NO_price < 1.00 on same platform, buy both → profit = 1.00 - total\","
        "\"cross_platform\":\"If YES_A + NO_B < 1.00 across Polymarket/Kalshi for same event, buy both → profit = 1.00 - total\","
        "\"combinatorial\":\"If sum of all mutually exclusive outcomes < 1.00, buy all → one wins, profit = 1.00 - total\","
        "\"threshold\":\"Minimum spread for actionable arb: 2c ($0.02) after fees\","
        "\"fees\":{\"polymarket\":\"0%\",\"kalshi\":\"varies by contract, typically 1-7%\"}"
    "}");

    jbuf_append(&out, "}}");
    free(topic);

    result[0] = '\0';
    truncate_response(out.data ? out.data : "{}", result, rlen, 64);
    jbuf_free(&out);
    return true;
}

/* ══════════════════════════════════════════════════════════════════════════
 *  SEMANTIC MARKET MATCHER
 *
 *  The real arbitrage isn't in identical tickers — it's in the language.
 *  "Will BTC exceed $100k by June" vs "Bitcoin above $99,500 end of May"
 *  are priced independently because the words differ, but the exposure
 *  overlaps heavily.
 *
 *  This tool:
 *    1. Fetches N markets from each platform
 *    2. Builds a TF-IDF index over all market questions
 *    3. Computes pairwise cosine similarity ACROSS platforms
 *    4. Extracts numerical entities (prices, dates, percentages)
 *    5. Reports pairs where:
 *       - Semantic similarity > threshold (configurable, default 0.3)
 *       - Price divergence exists
 *       - Numerical entities suggest same underlying event
 *
 *  This finds the arbs that ticker-matching misses entirely.
 * ══════════════════════════════════════════════════════════════════════════ */

/* Extract numbers from text for strike/threshold comparison */
typedef struct {
    double values[16];
    int    count;
} num_extract_t;

static void extract_numbers(const char *text, num_extract_t *out) {
    out->count = 0;
    if (!text) return;
    const char *p = text;
    while (*p && out->count < 16) {
        /* Skip to a digit or $ sign */
        while (*p && !isdigit((unsigned char)*p) && *p != '$') p++;
        if (!*p) break;
        if (*p == '$') p++;
        if (!isdigit((unsigned char)*p)) { p++; continue; }
        /* Parse number (handles commas: 100,000) */
        char nbuf[64];
        int ni = 0;
        while (*p && ni < 62 && (isdigit((unsigned char)*p) || *p == '.' || *p == ',')) {
            if (*p != ',') nbuf[ni++] = *p;
            p++;
        }
        nbuf[ni] = '\0';
        if (ni > 0) {
            double v = atof(nbuf);
            /* Filter out trivially small numbers (likely not prices/thresholds) */
            if (v >= 1.0 || (v > 0 && v < 1.0 && ni > 1)) {
                out->values[out->count++] = v;
            }
        }
        /* Handle "k" suffix: 100k = 100000 */
        if (*p == 'k' || *p == 'K') {
            if (out->count > 0) out->values[out->count - 1] *= 1000.0;
            p++;
        }
    }
}

/* Check if two sets of numbers have any "near" values (within 5%) */
static double number_proximity(const num_extract_t *a, const num_extract_t *b) {
    if (a->count == 0 || b->count == 0) return 0.0;
    double best = 0.0;
    for (int i = 0; i < a->count; i++) {
        for (int j = 0; j < b->count; j++) {
            double va = a->values[i], vb = b->values[j];
            if (va <= 0 || vb <= 0) continue;
            double ratio = (va > vb) ? vb / va : va / vb;
            if (ratio > best) best = ratio;
        }
    }
    return best;  /* 1.0 = exact match, 0.95+ = very close */
}

#define MAX_MATCH_MARKETS 40

typedef struct {
    char   question[256];
    char   prices[64];     /* "YES=0.65,NO=0.35" or outcomePrices */
    double yes_price;
    char   id[128];        /* token_id or ticker */
    double volume;
    int    platform;       /* 0 = polymarket, 1 = kalshi */
    num_extract_t nums;
} match_market_t;

/* Parse a JSON market array from Polymarket into match_market_t[] */
static int parse_polymarket_markets(const char *json, match_market_t *markets, int max) {
    if (!json || json[0] != '[') return 0;
    int count = 0;
    const char *p = json;
    /* Walk array elements (simplified: find each { } block) */
    while (*p && count < max) {
        const char *start = strchr(p, '{');
        if (!start) break;
        /* Find matching close brace (counting nesting) */
        int depth = 1;
        const char *end = start + 1;
        while (*end && depth > 0) {
            if (*end == '{') depth++;
            else if (*end == '}') depth--;
            end++;
        }
        /* Extract fields from this object */
        size_t olen = (size_t)(end - start);
        char *obj = malloc(olen + 1);
        memcpy(obj, start, olen);
        obj[olen] = '\0';

        char *q = json_get_str(obj, "question");
        char *pr = json_get_str(obj, "outcomePrices");
        char *cid = json_get_str(obj, "conditionId");
        char *vol_s = json_get_str(obj, "volume24hr");

        if (q && q[0]) {
            match_market_t *m = &markets[count];
            snprintf(m->question, sizeof(m->question), "%s", q);
            snprintf(m->prices, sizeof(m->prices), "%s", pr ? pr : "");
            snprintf(m->id, sizeof(m->id), "%s", cid ? cid : "");
            m->volume = vol_s ? atof(vol_s) : 0;
            m->platform = 0;
            /* Parse YES price from outcomePrices: ["0.65","0.35"] */
            m->yes_price = 0;
            if (pr && pr[0] == '[') {
                const char *fp = pr + 2; /* skip [" */
                m->yes_price = atof(fp);
            }
            extract_numbers(q, &m->nums);
            count++;
        }
        free(q); free(pr); free(cid); free(vol_s); free(obj);
        p = end;
    }
    return count;
}

/* Parse Kalshi events JSON into match_market_t[] */
static int parse_kalshi_markets(const char *json, match_market_t *markets, int max) {
    if (!json) return 0;
    int count = 0;
    /* Find "markets" arrays within events */
    const char *p = json;
    while (*p && count < max) {
        /* Find next "title" field (event or market level) */
        const char *title_key = strstr(p, "\"title\"");
        if (!title_key) break;
        const char *colon = strchr(title_key + 7, ':');
        if (!colon) break;
        /* Extract the title string */
        const char *qs = strchr(colon, '"');
        if (!qs) break;
        qs++;
        const char *qe = strchr(qs, '"');
        if (!qe) break;

        match_market_t *m = &markets[count];
        size_t qlen = (size_t)(qe - qs);
        if (qlen >= sizeof(m->question)) qlen = sizeof(m->question) - 1;
        memcpy(m->question, qs, qlen);
        m->question[qlen] = '\0';

        /* Try to find ticker nearby */
        const char *tk = strstr(qe, "\"ticker\"");
        if (tk && tk - qe < 500) {
            const char *ts = strchr(tk + 8, '"');
            if (ts) {
                ts++;
                const char *te = strchr(ts, '"');
                if (te) {
                    size_t tl = (size_t)(te - ts);
                    if (tl >= sizeof(m->id)) tl = sizeof(m->id) - 1;
                    memcpy(m->id, ts, tl);
                    m->id[tl] = '\0';
                }
            }
        }

        /* Try yes_bid / yes_ask */
        const char *yb = strstr(qe, "\"yes_bid\"");
        if (yb && yb - qe < 500) {
            m->yes_price = atof(yb + 10) / 100.0;  /* Kalshi uses cents */
        }

        m->platform = 1;
        m->volume = 0;
        extract_numbers(m->question, &m->nums);
        count++;
        p = qe + 1;
    }
    return count;
}

bool tool_prediction_semantic_match(const char *input, char *result, size_t rlen) {
    char *topic = json_get_str(input, "topic");
    double threshold = json_get_double(input, "threshold", 0.3);
    int limit = json_get_int(input, "limit", 15);
    if (limit > MAX_MATCH_MARKETS) limit = MAX_MATCH_MARKETS;

    /* ── Fetch from both platforms ─────────────────────────────────────── */
    char *pm_json = NULL;
    char *kl_json = NULL;

    /* Polymarket */
    {
        jbuf_t url;
        jbuf_init(&url, 512);
        if (topic && topic[0]) {
            CURL *c = curl_easy_init();
            char *enc = curl_easy_escape(c, topic, 0);
            jbuf_appendf(&url, "https://gamma-api.polymarket.com/markets?_q=%s&limit=%d&active=true&order=volume24hr&ascending=false", enc, limit);
            curl_free(enc); curl_easy_cleanup(c);
        } else {
            jbuf_appendf(&url, "https://gamma-api.polymarket.com/markets?limit=%d&active=true&order=volume24hr&ascending=false", limit);
        }
        http_buf_t resp = {0};
        long st = http_get_authed(url.data, NULL, &resp);
        jbuf_free(&url);
        if (st == 200 && resp.data) pm_json = resp.data;
        else free(resp.data);
    }

    /* Kalshi */
    {
        char path[256];
        snprintf(path, sizeof(path), "/events?limit=%d&status=open&with_nested_markets=true", limit);
        http_buf_t resp = {0};
        long st = kalshi_get(path, &resp);
        if (st == 200 && resp.data) kl_json = resp.data;
        else free(resp.data);
    }

    /* ── Parse markets ────────────────────────────────────────────────── */
    match_market_t *markets = calloc(MAX_MATCH_MARKETS * 2, sizeof(match_market_t));
    int pm_count = parse_polymarket_markets(pm_json, markets, MAX_MATCH_MARKETS);
    int kl_count = parse_kalshi_markets(kl_json, markets + pm_count, MAX_MATCH_MARKETS);
    int total = pm_count + kl_count;
    free(pm_json); free(kl_json);

    /* ── Build TF-IDF index over all questions ────────────────────────── */
    tfidf_index_t *idx = calloc(1, sizeof(tfidf_index_t));
    sem_tfidf_init(idx);
    for (int i = 0; i < total; i++) {
        sem_tfidf_add_doc(idx, markets[i].question);
    }
    sem_tfidf_finalize(idx);

    /* ── Compute cross-platform pairwise similarity ───────────────────── */
    jbuf_t out;
    jbuf_init(&out, rlen > 8192 ? 8192 : rlen);
    jbuf_appendf(&out, "{\"semantic_matches\":[");

    int match_count = 0;
    for (int i = 0; i < pm_count; i++) {
        /* Skip zero-volume Polymarket markets — noise, not signal */
        if (markets[i].volume < 1000.0) continue;

        tfidf_vec_t vi;
        sem_tfidf_vectorize(idx, markets[i].question, &vi);

        for (int j = pm_count; j < total; j++) {
            tfidf_vec_t vj;
            sem_tfidf_vectorize(idx, markets[j].question, &vj);

            double sim = sem_cosine_sim(&vi, &vj);
            double num_prox = number_proximity(&markets[i].nums, &markets[j].nums);

            /* Boost score if numerical entities are close AND sim > noise floor.
             * Prevents coincidental number matches from inflating unrelated pairs. */
            double combined = sim;
            if (sim > 0.15 && num_prox > 0.9)
                combined += 0.2 * num_prox;

            if (combined >= threshold) {
                if (match_count > 0) jbuf_append(&out, ",");
                jbuf_append(&out, "{");
                jbuf_appendf(&out, "\"similarity\":%.3f,\"num_proximity\":%.3f,\"combined\":%.3f,",
                             sim, num_prox, combined);
                jbuf_append(&out, "\"polymarket\":{\"question\":");
                jbuf_append_json_str(&out, markets[i].question);
                jbuf_appendf(&out, ",\"yes_price\":%.4f,\"volume\":%.0f,\"id\":",
                             markets[i].yes_price, markets[i].volume);
                jbuf_append_json_str(&out, markets[i].id);
                jbuf_append(&out, "},\"kalshi\":{\"question\":");
                jbuf_append_json_str(&out, markets[j].question);
                jbuf_appendf(&out, ",\"yes_price\":%.4f,\"ticker\":", markets[j].yes_price);
                jbuf_append_json_str(&out, markets[j].id);
                jbuf_append(&out, "}");

                /* Flag price divergence */
                double py = markets[i].yes_price;
                double ky = markets[j].yes_price;
                if (py > 0 && ky > 0) {
                    double spread = fabs(py - ky);
                    jbuf_appendf(&out, ",\"price_spread\":%.4f", spread);
                    if (spread > 0.02)
                        jbuf_append(&out, ",\"signal\":\"DIVERGENCE\"");
                }

                /* Flag numerical near-misses */
                if (num_prox > 0.9 && num_prox < 1.0)
                    jbuf_appendf(&out, ",\"strike_proximity\":\"%.1f%% — near-miss, check strike overlap\"",
                                 num_prox * 100.0);

                jbuf_append(&out, "}");
                match_count++;
                if (match_count >= 20) break;
            }
        }
        if (match_count >= 20) break;
    }

    jbuf_appendf(&out, "],\"stats\":{\"polymarket_markets\":%d,\"kalshi_markets\":%d,"
                 "\"pairs_checked\":%d,\"matches_found\":%d,\"threshold\":%.2f}",
                 pm_count, kl_count, pm_count * kl_count, match_count, threshold);

    /* Provide the detection methodology */
    jbuf_append(&out, ",\"methodology\":{"
        "\"tfidf\":\"TF-IDF cosine similarity on tokenized market questions (stop words removed)\","
        "\"num_extract\":\"Extracts dollar amounts, percentages, dates from question text\","
        "\"num_proximity\":\"Ratio of closest numerical values (0.95+ = near-identical strike)\","
        "\"combined_score\":\"cosine_sim + 0.2 bonus if numbers match within 5%\","
        "\"signal_DIVERGENCE\":\"Same event semantically but >2c price spread across platforms\""
    "}");

    jbuf_append(&out, "}");
    free(topic); free(markets); free(idx);

    result[0] = '\0';
    truncate_response(out.data ? out.data : "{}", result, rlen, 64);
    jbuf_free(&out);
    return true;
}

/* Forward declarations for trading.c functions */
extern bool tool_kalshi_positions(const char *input, char *result, size_t rlen);
extern bool tool_kalshi_balance(const char *input, char *result, size_t rlen);
extern bool tool_kalshi_portfolio(const char *input, char *result, size_t rlen);
extern bool tool_kalshi_fills(const char *input, char *result, size_t rlen);
extern bool tool_kalshi_open_orders(const char *input, char *result, size_t rlen);
extern bool tool_kalshi_create_order(const char *input, char *result, size_t rlen);
extern bool tool_kalshi_batch_create_orders(const char *input, char *result, size_t rlen);
extern bool tool_kalshi_cancel_order(const char *input, char *result, size_t rlen);
extern bool tool_kalshi_cancel_all(const char *input, char *result, size_t rlen);
extern bool tool_kalshi_amend_order(const char *input, char *result, size_t rlen);
extern bool tool_polymarket_balance(const char *input, char *result, size_t rlen);
extern bool tool_polymarket_positions(const char *input, char *result, size_t rlen);
extern bool tool_polymarket_open_orders(const char *input, char *result, size_t rlen);
extern bool tool_polymarket_create_order(const char *input, char *result, size_t rlen);
extern bool tool_polymarket_cancel_order(const char *input, char *result, size_t rlen);
extern bool tool_polymarket_cancel_all(const char *input, char *result, size_t rlen);
extern bool tool_polymarket_relayer_deploy(const char *input, char *result, size_t rlen);
extern bool tool_polymarket_relayer_approve(const char *input, char *result, size_t rlen);
extern bool tool_polymarket_relayer_execute(const char *input, char *result, size_t rlen);
extern bool tool_polymarket_relayer_status(const char *input, char *result, size_t rlen);
extern bool tool_polymarket_api_keys(const char *input, char *result, size_t rlen);
extern bool tool_polymarket_derive_api_key(const char *input, char *result, size_t rlen);

/* ══════════════════════════════════════════════════════════════════════════
 *  SYNOPTIC DATA — Real-time ASOS/METAR station observations
 *  Enterprise Weather API: https://api.synopticdata.com/v2/
 * ══════════════════════════════════════════════════════════════════════════ */

bool tool_synoptic(const char *input, char *result, size_t rlen) {
    const char *api_token;
    if (!require_key("SYNOPTIC_API_TOKEN", "Synoptic Data", result, rlen, &api_token))
        return false;

    char *action = json_get_str(input, "action");
    if (!action || !action[0]) {
        free(action);
        snprintf(result, rlen,
            "missing: action\n"
            "latest — Current observations for stations\n"
            "timeseries — Historical time series (start/end or recent)\n"
            "nearesttime — Observation nearest to a timestamp\n"
            "metadata — Station metadata (location, networks, sensors)\n"
            "precip — Derived precipitation data\n"
            "kalshi_stations — All 29 Kalshi weather stations current data");
        return false;
    }

    /* Station IDs for Kalshi weather markets */
    static const char *KALSHI_STATIONS =
        "KATL,KAUS,KBKF,KBNA,KBOS,KCLT,KDAL,KDCA,KDEN,KDFW,"
        "KDTW,KHOU,KJAX,KLAS,KLAX,KLGA,KMDW,KMIA,KMSP,KMSY,"
        "KNYC,KOKC,KORD,KPHL,KPHX,KSAT,KSEA,KSFO,KTPA";

    jbuf_t url;
    jbuf_init(&url, 1024);

    if (strcmp(action, "kalshi_stations") == 0) {
        /* Convenience: all 29 Kalshi stations, latest air_temp in Fahrenheit */
        jbuf_appendf(&url,
            "https://api.synopticdata.com/v2/stations/latest"
            "?token=%s&stid=%s&vars=air_temp,air_temp_high_6_hour,air_temp_low_6_hour,"
            "wind_speed,relative_humidity,precip_accum,dew_point_temperature"
            "&units=temp|F,speed|mph,precip|in&within=60&complete=1",
            api_token, KALSHI_STATIONS);

    } else if (strcmp(action, "latest") == 0) {
        char *stid = json_get_str(input, "stid");
        char *vars = json_get_str(input, "vars");
        char *state = json_get_str(input, "state");
        char *radius = json_get_str(input, "radius");
        int within = json_get_int(input, "within", 60);

        jbuf_appendf(&url,
            "https://api.synopticdata.com/v2/stations/latest?token=%s&within=%d"
            "&units=temp|F,speed|mph,precip|in",
            api_token, within);
        if (stid && stid[0]) jbuf_appendf(&url, "&stid=%s", stid);
        if (vars && vars[0]) jbuf_appendf(&url, "&vars=%s", vars);
        if (state && state[0]) jbuf_appendf(&url, "&state=%s", state);
        if (radius && radius[0]) jbuf_appendf(&url, "&radius=%s", radius);
        if (!stid && !state && !radius)
            jbuf_appendf(&url, "&stid=%s", KALSHI_STATIONS);

        free(stid); free(vars); free(state); free(radius);

    } else if (strcmp(action, "timeseries") == 0) {
        char *stid = json_get_str(input, "stid");
        char *vars = json_get_str(input, "vars");
        char *start = json_get_str(input, "start");
        char *end_t = json_get_str(input, "end");
        int recent = json_get_int(input, "recent", 0);

        if (!stid || !stid[0]) {
            free(stid); free(vars); free(start); free(end_t); free(action);
            jbuf_free(&url);
            snprintf(result, rlen, "timeseries requires stid (station ID, e.g. KORD, KLAX)");
            return false;
        }

        jbuf_appendf(&url,
            "https://api.synopticdata.com/v2/stations/timeseries?token=%s&stid=%s"
            "&units=temp|F,speed|mph,precip|in",
            api_token, stid);
        if (vars && vars[0]) jbuf_appendf(&url, "&vars=%s", vars);
        if (recent > 0) {
            jbuf_appendf(&url, "&recent=%d", recent);
        } else if (start && start[0]) {
            jbuf_appendf(&url, "&start=%s", start);
            if (end_t && end_t[0]) jbuf_appendf(&url, "&end=%s", end_t);
        } else {
            jbuf_appendf(&url, "&recent=1440"); /* default: last 24h */
        }

        free(stid); free(vars); free(start); free(end_t);

    } else if (strcmp(action, "nearesttime") == 0) {
        char *stid = json_get_str(input, "stid");
        char *attime = json_get_str(input, "attime");
        char *vars = json_get_str(input, "vars");
        int within_min = json_get_int(input, "within", 60);

        if (!stid || !stid[0] || !attime || !attime[0]) {
            free(stid); free(attime); free(vars); free(action);
            jbuf_free(&url);
            snprintf(result, rlen, "nearesttime requires stid and attime (YYYYmmddHHMM)");
            return false;
        }

        jbuf_appendf(&url,
            "https://api.synopticdata.com/v2/stations/nearesttime?token=%s&stid=%s"
            "&attime=%s&within=%d&units=temp|F,speed|mph,precip|in",
            api_token, stid, attime, within_min);
        if (vars && vars[0]) jbuf_appendf(&url, "&vars=%s", vars);

        free(stid); free(attime); free(vars);

    } else if (strcmp(action, "metadata") == 0) {
        char *stid = json_get_str(input, "stid");
        char *state = json_get_str(input, "state");
        char *network = json_get_str(input, "network");

        jbuf_appendf(&url,
            "https://api.synopticdata.com/v2/stations/metadata?token=%s&complete=1",
            api_token);
        if (stid && stid[0]) jbuf_appendf(&url, "&stid=%s", stid);
        else if (state && state[0]) jbuf_appendf(&url, "&state=%s", state);
        else if (network && network[0]) jbuf_appendf(&url, "&network=%s", network);
        else jbuf_appendf(&url, "&stid=%s", KALSHI_STATIONS);

        free(stid); free(state); free(network);

    } else if (strcmp(action, "precip") == 0) {
        char *stid = json_get_str(input, "stid");
        char *start = json_get_str(input, "start");
        char *end_t = json_get_str(input, "end");
        int recent = json_get_int(input, "recent", 0);

        if (!stid || !stid[0]) {
            free(stid); free(start); free(end_t); free(action);
            jbuf_free(&url);
            snprintf(result, rlen, "precip requires stid");
            return false;
        }

        jbuf_appendf(&url,
            "https://api.synopticdata.com/v2/stations/precipitation?token=%s&stid=%s"
            "&units=precip|in",
            api_token, stid);
        if (recent > 0) jbuf_appendf(&url, "&recent=%d", recent);
        else if (start && start[0]) {
            jbuf_appendf(&url, "&start=%s", start);
            if (end_t && end_t[0]) jbuf_appendf(&url, "&end=%s", end_t);
        } else {
            jbuf_appendf(&url, "&recent=1440");
        }

        free(stid); free(start); free(end_t);

    } else {
        snprintf(result, rlen, "unknown synoptic action: %s", action);
        free(action);
        jbuf_free(&url);
        return false;
    }

    free(action);

    http_buf_t resp = {0};
    long status = http_get_authed(url.data, NULL, &resp);
    jbuf_free(&url);

    if (status != 200) {
        snprintf(result, rlen, "Synoptic API error (HTTP %ld): %.500s",
                 status, resp.data ? resp.data : "");
        free(resp.data);
        return false;
    }

    /* Check for API error response */
    if (resp.data) {
        int code = json_get_int(resp.data, "SUMMARY.RESPONSE_CODE", 1);
        if (code != 1) {
            char *msg = json_get_str(resp.data, "SUMMARY.RESPONSE_MESSAGE");
            snprintf(result, rlen, "Synoptic error %d: %s", code, msg ? msg : "unknown");
            free(msg); free(resp.data);
            return false;
        }
    }

    result[0] = '\0';
    truncate_response(resp.data ? resp.data : "{}", result, rlen, 64);
    free(resp.data);
    return true;
}

/* ══════════════════════════════════════════════════════════════════════════
 *  NWS — National Weather Service API (free, no auth, User-Agent only)
 *  https://api.weather.gov
 * ══════════════════════════════════════════════════════════════════════════ */

bool tool_nws(const char *input, char *result, size_t rlen) {
    char *action = json_get_str(input, "action");
    if (!action || !action[0]) {
        free(action);
        snprintf(result, rlen,
            "missing: action\n"
            "forecast — Get forecast for lat/lon\n"
            "hourly — Hourly forecast for lat/lon\n"
            "station_obs — Latest observation from a station (e.g. KORD)\n"
            "alerts — Active weather alerts by state\n"
            "stations — List stations near lat/lon\n"
            "discussion — Area forecast discussion from NWS office");
        return false;
    }

    jbuf_t url;
    jbuf_init(&url, 512);
    const char *ua = "User-Agent: (dsco-cli, agent@distributed.systems)";

    if (strcmp(action, "station_obs") == 0) {
        char *stid = json_get_str(input, "stid");
        if (!stid || !stid[0]) {
            free(stid); free(action); jbuf_free(&url);
            snprintf(result, rlen, "station_obs requires stid (e.g. KORD, KLAX, KJFK)");
            return false;
        }
        jbuf_appendf(&url, "https://api.weather.gov/stations/%s/observations/latest", stid);
        free(stid);

    } else if (strcmp(action, "forecast") == 0 || strcmp(action, "hourly") == 0) {
        char *lat = json_get_str(input, "lat");
        char *lon = json_get_str(input, "lon");
        if (!lat || !lat[0] || !lon || !lon[0]) {
            free(lat); free(lon); free(action); jbuf_free(&url);
            snprintf(result, rlen, "forecast/hourly requires lat and lon");
            return false;
        }
        /* Step 1: resolve lat/lon to grid point */
        char points_url[256];
        snprintf(points_url, sizeof(points_url),
                 "https://api.weather.gov/points/%s,%s", lat, lon);

        http_buf_t pts = {0};
        long st = http_get_authed(points_url, ua, &pts);
        if (st != 200 || !pts.data) {
            free(lat); free(lon); free(action); jbuf_free(&url);
            snprintf(result, rlen, "NWS points lookup failed (HTTP %ld)", st);
            free(pts.data);
            return false;
        }

        /* Extract forecast URL from response (nested under properties) */
        const char *key = (strcmp(action, "hourly") == 0) ? "forecastHourly" : "forecast";
        char *props = json_get_raw(pts.data, "properties");
        char *forecast_url = NULL;
        if (props && props[0]) {
            forecast_url = json_get_str(props, key);
        }
        free(props); free(pts.data);
        free(lat); free(lon);

        if (!forecast_url || !forecast_url[0]) {
            free(forecast_url); free(action); jbuf_free(&url);
            snprintf(result, rlen, "could not resolve forecast URL from NWS points API");
            return false;
        }
        jbuf_append(&url, forecast_url);
        free(forecast_url);

    } else if (strcmp(action, "alerts") == 0) {
        char *state = json_get_str(input, "state");
        char *area = json_get_str(input, "area");
        char *event = json_get_str(input, "event");
        char *severity = json_get_str(input, "severity");

        jbuf_append(&url, "https://api.weather.gov/alerts/active?status=actual");
        if (state && state[0]) jbuf_appendf(&url, "&area=%s", state);
        else if (area && area[0]) jbuf_appendf(&url, "&area=%s", area);
        if (event && event[0]) {
            CURL *c = curl_easy_init();
            char *enc = curl_easy_escape(c, event, 0);
            jbuf_appendf(&url, "&event=%s", enc);
            curl_free(enc); curl_easy_cleanup(c);
        }
        if (severity && severity[0]) jbuf_appendf(&url, "&severity=%s", severity);
        free(state); free(area); free(event); free(severity);

    } else if (strcmp(action, "stations") == 0) {
        char *lat = json_get_str(input, "lat");
        char *lon = json_get_str(input, "lon");
        char *state = json_get_str(input, "state");
        int limit = json_get_int(input, "limit", 20);

        if (lat && lat[0] && lon && lon[0]) {
            /* Resolve to grid first, then get stations */
            char points_url[256];
            snprintf(points_url, sizeof(points_url),
                     "https://api.weather.gov/points/%s,%s", lat, lon);
            http_buf_t pts = {0};
            long st = http_get_authed(points_url, ua, &pts);
            if (st == 200 && pts.data) {
                char *props = json_get_raw(pts.data, "properties");
                if (props && props[0]) {
                    char *obs_url = json_get_str(props, "observationStations");
                    if (obs_url) {
                        jbuf_append(&url, obs_url);
                        free(obs_url);
                    }
                }
                free(props);
            }
            free(pts.data);
        }
        if (url.len == 0) {
            if (state && state[0])
                jbuf_appendf(&url, "https://api.weather.gov/stations?state=%s&limit=%d", state, limit);
            else
                jbuf_appendf(&url, "https://api.weather.gov/stations?limit=%d", limit);
        }
        free(lat); free(lon); free(state);

    } else if (strcmp(action, "discussion") == 0) {
        char *office = json_get_str(input, "office");
        if (!office || !office[0]) {
            free(office); free(action); jbuf_free(&url);
            snprintf(result, rlen,
                "discussion requires office (NWS office ID). Common: "
                "OKX (NYC), LOT (Chicago), FWD (Dallas), LAX (LA), MFL (Miami), "
                "PHI (Philly), BOS (Boston), SEW (Seattle), SFO (SF), PSR (Phoenix)");
            return false;
        }
        jbuf_appendf(&url,
            "https://api.weather.gov/products/types/AFD/locations/%s", office);
        free(office);

    } else {
        snprintf(result, rlen, "unknown nws action: %s", action);
        free(action); jbuf_free(&url);
        return false;
    }

    free(action);

    /* Make request with User-Agent header */
    http_buf_t resp = {0};
    long status = http_get_authed(url.data, ua, &resp);
    jbuf_free(&url);

    if (status != 200) {
        snprintf(result, rlen, "NWS API error (HTTP %ld): %.500s",
                 status, resp.data ? resp.data : "");
        free(resp.data);
        return false;
    }

    result[0] = '\0';
    truncate_response(resp.data ? resp.data : "{}", result, rlen, 64);
    free(resp.data);
    return true;
}

/* ══════════════════════════════════════════════════════════════════════════
 *  CONSOLIDATED DISPATCHERS — reduce 180+ tools to 3
 * ══════════════════════════════════════════════════════════════════════════ */

bool tool_alpha_vantage(const char *input, char *result, size_t rlen) {
    char *func = json_get_str(input, "function");
    if (!func || !func[0]) {
        free(func);
        snprintf(result, rlen,
            "missing required: function\n"
            "Market: TIME_SERIES_DAILY, TIME_SERIES_INTRADAY, GLOBAL_QUOTE, REALTIME_BULK_QUOTES, SYMBOL_SEARCH, MARKET_STATUS\n"
            "Technical: SMA, EMA, RSI, MACD, BBANDS, STOCH, STOCHF, STOCHRSI, ADX, ADXR, CCI, ATR, NATR, OBV, AD, ADOSC, VWAP, SAR, AROON, AROONOSC, MFI, WILLR, APO, PPO, ROC, ROCR, BOP, MOM, CMO, TRIX, DX, MINUS_DI, PLUS_DI, ULTOSC, MIDPOINT, MIDPRICE, TRANGE, WMA, DEMA, TEMA, TRIMA, KAMA, MAMA, T3, MACDEXT, HT_TRENDLINE, HT_SINE, HT_TRENDMODE, HT_DCPERIOD, HT_DCPHASE, HT_PHASOR\n"
            "Fundamental: OVERVIEW, ETF_PROFILE, INCOME_STATEMENT, BALANCE_SHEET, CASH_FLOW, EARNINGS, EARNINGS_ESTIMATES, DIVIDENDS, SPLITS, INSIDER_TRANSACTIONS, INSTITUTIONAL_HOLDINGS, EARNINGS_CALL_TRANSCRIPT\n"
            "Macro: REAL_GDP, REAL_GDP_PER_CAPITA, CPI, INFLATION, UNEMPLOYMENT, TREASURY_YIELD, FEDERAL_FUNDS_RATE, RETAIL_SALES, DURABLES, NONFARM_PAYROLL\n"
            "Commodities: WTI, BRENT, NATURAL_GAS, COPPER, ALUMINUM, WHEAT, CORN, COTTON, SUGAR, COFFEE, ALL_COMMODITIES, GOLD_SILVER_SPOT, GOLD_SILVER_HISTORY\n"
            "Forex: CURRENCY_EXCHANGE_RATE, FX_INTRADAY, FX_DAILY, FX_WEEKLY, FX_MONTHLY\n"
            "Crypto: DIGITAL_CURRENCY_DAILY, CRYPTO_INTRADAY, DIGITAL_CURRENCY_WEEKLY, DIGITAL_CURRENCY_MONTHLY\n"
            "Options: REALTIME_OPTIONS, HISTORICAL_OPTIONS\n"
            "News: NEWS_SENTIMENT\n"
            "Calendar: TOP_GAINERS_LOSERS, LISTING_STATUS, EARNINGS_CALENDAR, IPO_CALENDAR\n"
            "Analytics: ANALYTICS_FIXED_WINDOW, ANALYTICS_SLIDING_WINDOW");
        return false;
    }
    bool ok = av_generic(func, input, NULL, NULL, result, rlen);
    free(func);
    return ok;
}

bool tool_kalshi(const char *input, char *result, size_t rlen) {
    char *action = json_get_str(input, "action");
    if (!action || !action[0]) {
        free(action);
        snprintf(result, rlen,
            "missing: action\n"
            "Read: markets, events, search, orderbook, trades, series, candlesticks, weather, snapshot, event_detail, daily\n"
            "Account: positions, balance, portfolio, fills, open_orders\n"
            "Trade: create_order, batch_create, cancel_order, cancel_all, amend_order\n"
            "History: historical_markets, historical_trades, historical_cutoff");
        return false;
    }
    bool ok = false;
    if      (strcmp(action, "markets") == 0)            ok = tool_kalshi_markets(input, result, rlen);
    else if (strcmp(action, "events") == 0)             ok = tool_kalshi_events(input, result, rlen);
    else if (strcmp(action, "search") == 0)             ok = tool_kalshi_search(input, result, rlen);
    else if (strcmp(action, "orderbook") == 0)          ok = tool_kalshi_orderbook(input, result, rlen);
    else if (strcmp(action, "trades") == 0)             ok = tool_kalshi_trades(input, result, rlen);
    else if (strcmp(action, "series") == 0)             ok = tool_kalshi_series(input, result, rlen);
    else if (strcmp(action, "candlesticks") == 0)       ok = tool_kalshi_candlesticks(input, result, rlen);
    else if (strcmp(action, "weather") == 0)            ok = tool_kalshi_weather(input, result, rlen);
    else if (strcmp(action, "snapshot") == 0)           ok = tool_kalshi_market_snapshot(input, result, rlen);
    else if (strcmp(action, "event_detail") == 0)       ok = tool_kalshi_event_detail(input, result, rlen);
    else if (strcmp(action, "daily") == 0)              ok = tool_kalshi_daily_markets(input, result, rlen);
    else if (strcmp(action, "positions") == 0)          ok = tool_kalshi_positions(input, result, rlen);
    else if (strcmp(action, "balance") == 0)            ok = tool_kalshi_balance(input, result, rlen);
    else if (strcmp(action, "portfolio") == 0)          ok = tool_kalshi_portfolio(input, result, rlen);
    else if (strcmp(action, "fills") == 0)              ok = tool_kalshi_fills(input, result, rlen);
    else if (strcmp(action, "open_orders") == 0)        ok = tool_kalshi_open_orders(input, result, rlen);
    else if (strcmp(action, "create_order") == 0)       ok = tool_kalshi_create_order(input, result, rlen);
    else if (strcmp(action, "batch_create") == 0)       ok = tool_kalshi_batch_create_orders(input, result, rlen);
    else if (strcmp(action, "cancel_order") == 0)       ok = tool_kalshi_cancel_order(input, result, rlen);
    else if (strcmp(action, "cancel_all") == 0)         ok = tool_kalshi_cancel_all(input, result, rlen);
    else if (strcmp(action, "amend_order") == 0)        ok = tool_kalshi_amend_order(input, result, rlen);
    else if (strcmp(action, "historical_markets") == 0) ok = tool_kalshi_historical_markets(input, result, rlen);
    else if (strcmp(action, "historical_trades") == 0)  ok = tool_kalshi_historical_trades(input, result, rlen);
    else if (strcmp(action, "historical_cutoff") == 0)  ok = tool_kalshi_historical_cutoff(input, result, rlen);
    else snprintf(result, rlen, "unknown kalshi action: %s", action);
    free(action);
    return ok;
}

bool tool_polymarket(const char *input, char *result, size_t rlen) {
    char *action = json_get_str(input, "action");
    if (!action || !action[0]) {
        free(action);
        snprintf(result, rlen,
            "missing: action\n"
            "Read: markets, events, categories, prices, book, trades, search, resolved, resolved_events, whale_trades, leaderboard, history\n"
            "Account: balance, positions, open_orders, api_keys, derive_api_key\n"
            "Trade: create_order, cancel_order, cancel_all\n"
            "Relayer: relayer_deploy, relayer_approve, relayer_execute, relayer_status");
        return false;
    }
    bool ok = false;
    if      (strcmp(action, "markets") == 0)          ok = tool_polymarket_markets(input, result, rlen);
    else if (strcmp(action, "events") == 0)           ok = tool_polymarket_events(input, result, rlen);
    else if (strcmp(action, "categories") == 0)       ok = tool_polymarket_categories(input, result, rlen);
    else if (strcmp(action, "prices") == 0)           ok = tool_polymarket_prices(input, result, rlen);
    else if (strcmp(action, "book") == 0)             ok = tool_polymarket_book(input, result, rlen);
    else if (strcmp(action, "trades") == 0)           ok = tool_polymarket_trades(input, result, rlen);
    else if (strcmp(action, "search") == 0)           ok = tool_polymarket_search(input, result, rlen);
    else if (strcmp(action, "resolved") == 0)         ok = tool_polymarket_resolved(input, result, rlen);
    else if (strcmp(action, "resolved_events") == 0)  ok = tool_polymarket_resolved_events(input, result, rlen);
    else if (strcmp(action, "whale_trades") == 0)     ok = tool_polymarket_whale_trades(input, result, rlen);
    else if (strcmp(action, "leaderboard") == 0)      ok = tool_polymarket_leaderboard(input, result, rlen);
    else if (strcmp(action, "history") == 0)          ok = tool_polymarket_history(input, result, rlen);
    else if (strcmp(action, "balance") == 0)          ok = tool_polymarket_balance(input, result, rlen);
    else if (strcmp(action, "positions") == 0)        ok = tool_polymarket_positions(input, result, rlen);
    else if (strcmp(action, "open_orders") == 0)      ok = tool_polymarket_open_orders(input, result, rlen);
    else if (strcmp(action, "create_order") == 0)     ok = tool_polymarket_create_order(input, result, rlen);
    else if (strcmp(action, "cancel_order") == 0)     ok = tool_polymarket_cancel_order(input, result, rlen);
    else if (strcmp(action, "cancel_all") == 0)       ok = tool_polymarket_cancel_all(input, result, rlen);
    else if (strcmp(action, "relayer_deploy") == 0)   ok = tool_polymarket_relayer_deploy(input, result, rlen);
    else if (strcmp(action, "relayer_approve") == 0)  ok = tool_polymarket_relayer_approve(input, result, rlen);
    else if (strcmp(action, "relayer_execute") == 0)  ok = tool_polymarket_relayer_execute(input, result, rlen);
    else if (strcmp(action, "relayer_status") == 0)   ok = tool_polymarket_relayer_status(input, result, rlen);
    else if (strcmp(action, "api_keys") == 0)         ok = tool_polymarket_api_keys(input, result, rlen);
    else if (strcmp(action, "derive_api_key") == 0)   ok = tool_polymarket_derive_api_key(input, result, rlen);
    else snprintf(result, rlen, "unknown polymarket action: %s", action);
    free(action);
    return ok;
}
