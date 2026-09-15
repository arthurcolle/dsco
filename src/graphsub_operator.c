#include "graphsub_operator.h"
#include "http_pool.h"
#include "service_boundary.h"
#include "../vendor/yyjson.h"

#include <arpa/inet.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define BODY_LIMIT (128u * 1024u)
#define INPUT_LIMIT 8192u
#define SAFE_INTEGER 9007199254740991LL

const char graphsub_operator_schema[] =
    "{\"type\":\"object\",\"properties\":{"
    "\"action\":{\"type\":\"string\",\"enum\":[\"status\",\"schema\",\"list\",\"read\"],"
    "\"description\":\"Live native browse only. schema returns the dynamic-schema catalog, "
    "not a complete static field registry.\"},"
    "\"connection\":{\"type\":\"string\",\"enum\":[\"default\"],\"default\":\"default\"},"
    "\"shard_id\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":2147483647,\"default\":0},"
    "\"limit\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":100,\"default\":25,"
    "\"description\":\"list only\"},"
    "\"offset\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":10000000,\"default\":0,"
    "\"description\":\"list only\"},"
    "\"node_id\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":256,"
    "\"pattern\":\"^[A-Za-z0-9_-]+$\","
    "\"not\":{\"pattern\":\"[^A-Za-z0-9_-]\"},"
    "\"description\":\"read only; opaque native base64url node ID from list, at most 256 bytes\"}"
    "},\"required\":[\"action\"],\"additionalProperties\":false,\"oneOf\":["
    "{\"properties\":{\"action\":{\"enum\":[\"status\",\"schema\"]}},\"not\":{\"anyOf\":["
    "{\"required\":[\"node_id\"]},{\"required\":[\"limit\"]},{\"required\":[\"offset\"]}]}},"
    "{\"properties\":{\"action\":{\"enum\":[\"list\"]}},\"not\":{\"required\":[\"node_id\"]}},"
    "{\"properties\":{\"action\":{\"enum\":[\"read\"]}},\"required\":[\"node_id\"],"
    "\"not\":{\"anyOf\":[{\"required\":[\"limit\"]},{\"required\":[\"offset\"]}]}}]}";

const char graphsub_operator_output_schema[] =
    "{\"type\":\"object\",\"properties\":{\"ok\":{\"type\":\"boolean\"},"
    "\"profile\":{\"type\":\"string\",\"enum\":[\"browse\"]},"
    "\"consistency\":{\"type\":\"string\",\"enum\":[\"live\"]},"
    "\"snapshot\":{\"type\":\"boolean\",\"enum\":[false]},"
    "\"connection\":{\"type\":\"string\",\"enum\":[\"default\"]},"
    "\"shard_id\":{\"type\":\"integer\"},\"action\":{\"type\":\"string\"},"
    "\"observed_at\":{\"type\":\"string\"},\"data\":{\"type\":\"object\"},"
    "\"error\":{\"type\":\"object\",\"properties\":{\"code\":{\"type\":\"string\"},"
    "\"message\":{\"type\":\"string\"},\"http_status\":{\"type\":\"integer\"}},"
    "\"required\":[\"code\",\"message\"],\"additionalProperties\":false}},"
    "\"required\":[\"ok\"],\"additionalProperties\":false,\"oneOf\":["
    "{\"properties\":{\"ok\":{\"enum\":[true]}},\"required\":[\"profile\",\"consistency\","
    "\"snapshot\",\"connection\",\"shard_id\",\"action\",\"observed_at\",\"data\"]},"
    "{\"properties\":{\"ok\":{\"enum\":[false]}},\"required\":[\"error\"]}]}";

/* All messages are fixed literals: transport configuration, credentials and
 * untrusted response text never enter error output. */
static bool fail(char *out, size_t cap, const char *code, const char *message, long status) {
    char error[512];
    int n = status > 0
                ? snprintf(error, sizeof(error), "{\"ok\":false,\"error\":{\"code\":\"%s\","
                           "\"message\":\"%s\",\"http_status\":%ld}}", code, message, status)
                : snprintf(error, sizeof(error), "{\"ok\":false,\"error\":{\"code\":\"%s\","
                           "\"message\":\"%s\"}}", code, message);
    if (!out || !cap)
        return false;
    if (n >= 0 && (size_t)n < sizeof(error) && (size_t)n < cap)
        memcpy(out, error, (size_t)n + 1);
    else {
        const char small[] = "{\"ok\":false,\"error\":{\"code\":\"response_buffer_too_small\","
                             "\"message\":\"Result buffer too small\"}}";
        if (sizeof(small) <= cap)
            memcpy(out, small, sizeof(small));
        else
            out[0] = '\0';
    }
    return false;
}

static bool text_is(yyjson_val *v, const char *s) {
    return yyjson_is_str(v) && yyjson_get_len(v) == strlen(s) &&
           !memcmp(yyjson_get_str(v), s, strlen(s));
}

static bool uint_arg(yyjson_val *v, uint64_t min, uint64_t max, unsigned *out) {
    if (!v)
        return true;
    double n = yyjson_get_num(v);
    if (!yyjson_is_num(v) || !isfinite(n) || floor(n) != n || n < (double)min || n > (double)max)
        return false;
    *out = (unsigned)n;
    return true;
}

static bool native_id(yyjson_val *v) {
    if (!yyjson_is_str(v) || !yyjson_get_len(v) || yyjson_get_len(v) > 256)
        return false;
    const unsigned char *s = (const unsigned char *)yyjson_get_str(v);
    for (size_t i = 0; i < yyjson_get_len(v); ++i)
        if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z') ||
              (s[i] >= '0' && s[i] <= '9') || s[i] == '_' || s[i] == '-'))
            return false;
    return true;
}

typedef struct {
    char *data;
    size_t length;
    bool too_large;
} body_t;

static size_t receive(void *data, size_t size, size_t count, void *opaque) {
    body_t *body = opaque;
    if ((size && count > BODY_LIMIT / size) || size * count > BODY_LIMIT - body->length) {
        body->too_large = true;
        return 0;
    }
    size_t n = size * count;
    memcpy(body->data + body->length, data, n);
    body->length += n;
    body->data[body->length] = '\0';
    return n;
}

static bool loopback(const char *host) {
    struct in_addr v4;
    struct in6_addr v6;
    if (inet_pton(AF_INET, host, &v4) == 1)
        return (ntohl(v4.s_addr) >> 24) == 127;
    if (!strcmp(host, "[::1]") || !strcmp(host, "::1"))
        return true;
    return inet_pton(AF_INET6, host, &v6) == 1 && IN6_IS_ADDR_LOOPBACK(&v6);
}

/* Only an origin is configurable. No userinfo, base path, query or fragment.
 * localhost over HTTP is pinned numerically so DNS cannot move it off-host. */
static CURLU *configured_origin(void) {
    const char *base = getenv("GRAPHSUB_HOST");
    if (!base)
        base = "http://127.0.0.1:7879";
    size_t length = strnlen(base, 2049);
    if (!length || length > 2048 || strchr(base, '?') || strchr(base, '#') ||
        (strncmp(base, "http://", 7) && strncmp(base, "https://", 8)))
        return NULL;
    const char *raw_path = strchr(base + (!strncmp(base, "https://", 8) ? 8 : 7), '/');
    if (raw_path && strcmp(raw_path, "/"))
        return NULL;
    for (size_t i = 0; i < length; ++i)
        if ((unsigned char)base[i] <= 32 || (unsigned char)base[i] == 127)
            return NULL;
    CURLU *url = curl_url();
    char *scheme = NULL, *host = NULL, *path = NULL;
    bool valid = url && curl_url_set(url, CURLUPART_URL, base, CURLU_DISALLOW_USER) == CURLUE_OK &&
                 curl_url_get(url, CURLUPART_SCHEME, &scheme, 0) == CURLUE_OK &&
                 curl_url_get(url, CURLUPART_HOST, &host, 0) == CURLUE_OK &&
                 curl_url_get(url, CURLUPART_PATH, &path, 0) == CURLUE_OK && !strcmp(path, "/");
    if (valid && !strcmp(scheme, "http")) {
        if (!strcasecmp(host, "localhost"))
            valid = curl_url_set(url, CURLUPART_HOST, "127.0.0.1", 0) == CURLUE_OK;
        else
            valid = loopback(host);
    } else if (valid)
        valid = !strcmp(scheme, "https");
    curl_free(scheme);
    curl_free(host);
    curl_free(path);
    if (!valid) {
        curl_url_cleanup(url);
        return NULL;
    }
    if (!service_destination_allowed("graphsub_operator", base)) {
        curl_url_cleanup(url);
        return NULL;
    }
    return url;
}

/* Reject values that LuaJIT/JSON consumers cannot carry without changing an
 * integer. BIGNUM_AS_RAW prevents yyjson converting oversized integers first. */
typedef enum { RESPONSE_VALID, RESPONSE_MALFORMED, RESPONSE_PRECISION } response_check;

static response_check validate_json(yyjson_val *v, unsigned depth) {
    if (depth > 64)
        return RESPONSE_MALFORMED;
    if (yyjson_is_raw(v))
        return RESPONSE_PRECISION;
    if (yyjson_is_uint(v))
        return yyjson_get_uint(v) <= (uint64_t)SAFE_INTEGER ? RESPONSE_VALID : RESPONSE_PRECISION;
    if (yyjson_is_sint(v))
        return yyjson_get_sint(v) >= -SAFE_INTEGER && yyjson_get_sint(v) <= SAFE_INTEGER
                   ? RESPONSE_VALID : RESPONSE_PRECISION;
    if (yyjson_is_real(v))
        return isfinite(yyjson_get_real(v)) && fabs(yyjson_get_real(v)) <= (double)SAFE_INTEGER
                   ? RESPONSE_VALID : RESPONSE_PRECISION;
    size_t i, count;
    yyjson_val *key, *value;
    if (yyjson_is_arr(v)) {
        yyjson_arr_foreach(v, i, count, value) {
            response_check check = validate_json(value, depth + 1);
            if (check != RESPONSE_VALID)
                return check;
        }
    } else if (yyjson_is_obj(v)) {
        if (yyjson_obj_size(v) > 4096)
            return RESPONSE_MALFORMED;
        yyjson_obj_foreach(v, i, count, key, value) {
            const char *name = yyjson_get_str(key);
            if (strlen(name) != yyjson_get_len(key) || yyjson_obj_get(v, name) != value)
                return RESPONSE_MALFORMED;
            response_check check = validate_json(value, depth + 1);
            if (check != RESPONSE_VALID)
                return check;
        }
    }
    return RESPONSE_VALID;
}

static bool response_text(yyjson_val *v) {
    return yyjson_is_str(v) && yyjson_get_len(v) > 0 &&
           strlen(yyjson_get_str(v)) == yyjson_get_len(v);
}

static bool response_uint(yyjson_val *v) {
    return yyjson_is_num(v) && yyjson_get_num(v) >= 0 &&
           floor(yyjson_get_num(v)) == yyjson_get_num(v);
}

static bool node_fields(yyjson_val *v) {
    return yyjson_is_obj(v) && native_id(yyjson_obj_get(v, "id")) &&
           yyjson_is_str(yyjson_obj_get(v, "name")) && yyjson_is_str(yyjson_obj_get(v, "type"));
}

static bool envelope_valid(yyjson_val *data, const char *action, yyjson_val *requested_id,
                           unsigned offset, unsigned limit) {
    if (!strcmp(action, "status"))
        return text_is(yyjson_obj_get(data, "service"), "graphsub") &&
               text_is(yyjson_obj_get(data, "status"), "healthy") &&
               response_text(yyjson_obj_get(data, "version"));
    if (!strcmp(action, "schema"))
        return yyjson_is_arr(yyjson_obj_get(data, "schemas"));
    if (!strcmp(action, "list")) {
        yyjson_val *nodes = yyjson_obj_get(data, "nodes");
        yyjson_val *total = yyjson_obj_get(data, "total");
        yyjson_val *off = yyjson_obj_get(data, "offset"), *lim = yyjson_obj_get(data, "limit");
        if (!yyjson_is_arr(nodes) || !response_uint(total) || !response_uint(off) ||
            !response_uint(lim) || yyjson_get_num(off) != fmin(offset, yyjson_get_num(total)) ||
            yyjson_get_num(lim) != limit || yyjson_arr_size(nodes) > limit ||
            yyjson_arr_size(nodes) > yyjson_get_num(total) - yyjson_get_num(off))
            return false;
        size_t i, count;
        yyjson_val *node;
        yyjson_arr_foreach(nodes, i, count, node)
            if (!node_fields(node))
                return false;
        return true;
    }
    if (!node_fields(data) || !yyjson_is_obj(yyjson_obj_get(data, "metadata")) ||
        !response_uint(yyjson_obj_get(data, "node_id")) || !yyjson_obj_get(data, "payload"))
        return false;
    const char *requested = yyjson_get_str(requested_id);
    if (text_is(yyjson_obj_get(data, "id"), requested))
        return true;
    /* Native REST also accepts decimal IDs. Never convert an opaque ID or a
     * decimal ID beyond exact JSON precision to a number. */
    uint64_t number = 0;
    for (size_t i = 0; i < yyjson_get_len(requested_id); ++i) {
        if (requested[i] < '0' || requested[i] > '9' ||
            number > ((uint64_t)SAFE_INTEGER - (unsigned)(requested[i] - '0')) / 10)
            return false;
        number = number * 10 + (unsigned)(requested[i] - '0');
    }
    return (double)number == yyjson_get_num(yyjson_obj_get(data, "node_id"));
}

bool graphsub_operator_execute(const char *input, char *result, size_t capacity) {
    if (result && capacity)
        result[0] = '\0';
    if (!result || capacity < 128)
        return fail(result, capacity, "response_buffer_too_small", "Result buffer too small", 0);
    size_t input_len = input ? strnlen(input, INPUT_LIMIT + 1) : 0;
    if (!input_len || input_len > INPUT_LIMIT)
        return fail(result, capacity, "invalid_request", "Expected a bounded JSON object", 0);
    yyjson_doc *request = yyjson_read(input, input_len, 0);
    yyjson_val *root = request ? yyjson_doc_get_root(request) : NULL;
    yyjson_val *av = yyjson_obj_get(root, "action"), *cv = yyjson_obj_get(root, "connection");
    yyjson_val *nv = yyjson_obj_get(root, "node_id");
    const char *action = text_is(av, "status") ? "status" : text_is(av, "schema") ? "schema"
                         : text_is(av, "list") ? "list" : text_is(av, "read") ? "read" : NULL;
    unsigned shard = 0, limit = 25, offset = 0;
    bool valid = yyjson_is_obj(root) && action && (!cv || text_is(cv, "default")) &&
                 uint_arg(yyjson_obj_get(root, "shard_id"), 0, 2147483647u, &shard) &&
                 uint_arg(yyjson_obj_get(root, "limit"), 1, 100, &limit) &&
                 uint_arg(yyjson_obj_get(root, "offset"), 0, 10000000, &offset);
    size_t i, count;
    yyjson_val *key, *value;
    yyjson_obj_foreach(root, i, count, key, value) {
        const char *name = yyjson_get_str(key);
        if (strlen(name) != yyjson_get_len(key) || yyjson_obj_get(root, name) != value ||
            (strcmp(name, "action") && strcmp(name, "connection") && strcmp(name, "shard_id") &&
             strcmp(name, "limit") && strcmp(name, "offset") && strcmp(name, "node_id")))
            valid = false;
    }
    if (valid && strcmp(action, "list") &&
        (yyjson_obj_get(root, "limit") || yyjson_obj_get(root, "offset")))
        valid = false;
    if (valid && !strcmp(action, "read"))
        valid = native_id(nv);
    else if (nv)
        valid = false;
    if (!valid) {
        yyjson_doc_free(request);
        return fail(result, capacity, "invalid_request", "Invalid action, fields or browse arguments", 0);
    }

    dsco_http_global_init();
    CURLU *url = configured_origin();
    CURL *easy = NULL;
    struct curl_slist *headers = NULL;
    yyjson_doc *response = NULL;
    body_t body = {0};
    const char *code = "bad_configuration", *message = "Invalid GraphSub connection origin";
    long status = 0;
    bool success = false;
    if (!url)
        goto done;
    const char *token = getenv("GRAPHSUB_API_KEY");
    if (token && *token) {
        size_t token_len = strnlen(token, 4097);
        message = "Invalid GraphSub authentication configuration";
        if (token_len > 4096)
            goto done;
        for (size_t j = 0; j < token_len; ++j)
            if ((unsigned char)token[j] < 33 || (unsigned char)token[j] > 126)
                goto done;
        char auth[4120];
        snprintf(auth, sizeof(auth), "Authorization: Bearer %s", token);
        headers = curl_slist_append(NULL, auth);
        if (!headers) {
            code = "allocation_failed";
            message = "Cannot allocate request headers";
            goto done;
        }
    }
    struct curl_slist *updated = curl_slist_append(headers, "Accept: application/json");
    if (!updated) {
        code = "allocation_failed";
        message = "Cannot allocate request headers";
        goto done;
    }
    headers = updated;
    easy = curl_easy_init();
    body.data = malloc(BODY_LIMIT + 1);
    code = "allocation_failed";
    message = "Cannot allocate GraphSub request";
    if (!easy || !body.data)
        goto done;
    body.data[0] = '\0';
    dsco_http_pool_apply(easy);
    char path[1024], query[96];
    if (!strcmp(action, "status"))
        snprintf(path, sizeof(path), "/health");
    else if (!strcmp(action, "schema"))
        snprintf(path, sizeof(path), "/api/v1/dynamic-schemas");
    else if (!strcmp(action, "list"))
        snprintf(path, sizeof(path), "/api/v1/shards/%u/nodes", shard);
    else {
        char *encoded = curl_easy_escape(easy, yyjson_get_str(nv), (int)yyjson_get_len(nv));
        if (!encoded)
            goto done;
        snprintf(path, sizeof(path), "/api/v1/shards/%u/nodes/%s", shard, encoded);
        curl_free(encoded);
    }
    code = "bad_configuration";
    message = "Cannot construct GraphSub request URL";
    if (curl_url_set(url, CURLUPART_PATH, path, 0) != CURLUE_OK)
        goto done;
    if (!strcmp(action, "list")) {
        snprintf(query, sizeof(query), "offset=%u&limit=%u", offset, limit);
        if (curl_url_set(url, CURLUPART_QUERY, query, 0) != CURLUE_OK)
            goto done;
    }
    code = "transport_error";
    message = "Cannot configure GraphSub transport";
#define SETOPT(option, value) do { if (curl_easy_setopt(easy, option, value) != CURLE_OK) goto done; } while (0)
    SETOPT(CURLOPT_CURLU, url);
    SETOPT(CURLOPT_HTTPGET, 1L);
    SETOPT(CURLOPT_HTTPHEADER, headers);
    SETOPT(CURLOPT_WRITEFUNCTION, receive);
    SETOPT(CURLOPT_WRITEDATA, &body);
    SETOPT(CURLOPT_CONNECTTIMEOUT_MS, 3000L);
    SETOPT(CURLOPT_TIMEOUT_MS, 10000L);
    SETOPT(CURLOPT_NOSIGNAL, 1L);
    SETOPT(CURLOPT_SSL_VERIFYPEER, 1L);
    SETOPT(CURLOPT_SSL_VERIFYHOST, 2L);
    SETOPT(CURLOPT_FOLLOWLOCATION, 0L);
    SETOPT(CURLOPT_NETRC, (long)CURL_NETRC_IGNORED);
    SETOPT(CURLOPT_PROXY, "");
    SETOPT(CURLOPT_PATH_AS_IS, 1L);
#if LIBCURL_VERSION_NUM >= 0x075500
    SETOPT(CURLOPT_PROTOCOLS_STR, "http,https");
#else
    SETOPT(CURLOPT_PROTOCOLS, (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS));
#endif
#undef SETOPT
    CURLcode transport = curl_easy_perform(easy);
    curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &status);
    if (body.too_large) {
        code = "response_too_large";
        message = "GraphSub response exceeds 128 KiB; request a smaller page";
        goto done;
    }
    if (transport != CURLE_OK) {
        code = transport == CURLE_OPERATION_TIMEDOUT ? "timeout" : "transport_error";
        message = "GraphSub request failed";
        goto done;
    }
    if (status < 200 || status >= 300) {
        code = "http_error";
        message = "GraphSub returned an unsuccessful HTTP status";
        goto done;
    }
    response = yyjson_read(body.data, body.length, YYJSON_READ_BIGNUM_AS_RAW);
    yyjson_val *data = response ? yyjson_doc_get_root(response) : NULL;
    if (!yyjson_is_obj(data)) {
        code = "malformed_response";
        message = "GraphSub response is not a JSON object";
        goto done;
    }
    response_check check = validate_json(data, 0);
    if (check == RESPONSE_PRECISION) {
        code = "precision_unsupported";
        message = "Response exceeds supported JSON numeric precision";
        goto done;
    }
    if (check != RESPONSE_VALID || !envelope_valid(data, action, nv, offset, limit)) {
        code = "malformed_response";
        message = "GraphSub response does not match the native browse contract";
        goto done;
    }
    char observed[32], prefix[320];
    time_t now = time(NULL);
    struct tm utc;
    if (now == (time_t)-1 || !gmtime_r(&now, &utc) ||
        !strftime(observed, sizeof(observed), "%Y-%m-%dT%H:%M:%SZ", &utc)) {
        code = "clock_error";
        message = "Cannot timestamp GraphSub observation";
        goto done;
    }
    int prefix_len = snprintf(prefix, sizeof(prefix), "{\"ok\":true,\"profile\":\"browse\","
                              "\"consistency\":\"live\",\"snapshot\":false,\"connection\":\"default\","
                              "\"shard_id\":%u,\"action\":\"%s\",\"observed_at\":\"%s\",\"data\":",
                              shard, action, observed);
    if (!result || prefix_len < 0 || (size_t)prefix_len >= sizeof(prefix) ||
        (size_t)prefix_len + body.length + 2 > capacity) {
        code = "response_buffer_too_small";
        message = "Complete GraphSub response does not fit the result buffer";
        goto done;
    }
    memcpy(result, prefix, (size_t)prefix_len);
    memcpy(result + prefix_len, body.data, body.length);
    result[prefix_len + body.length] = '}';
    result[prefix_len + body.length + 1] = '\0';
    success = true;
done:
    yyjson_doc_free(response);
    yyjson_doc_free(request);
    curl_slist_free_all(headers);
    curl_easy_cleanup(easy);
    curl_url_cleanup(url);
    free(body.data);
    return success ? true : fail(result, capacity, code, message, status);
}
