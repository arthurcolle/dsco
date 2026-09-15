#include "lingo_chimera.h"
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
#define INPUT_LIMIT (32u * 1024u)
#define SAFE_INTEGER 9007199254740991LL
#define CHIMERA_MODEL "dsco-router/chimera:latest"

const char lingo_chimera_schema[] = "{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\",\"enum\":[\"plan\"]},\"policy\":{\"type\":\"object\",\"properties\":{\"success_weight\":{\"type\":\"number\",\"minimum\":0,\"maximum\":1000000},\"quality_weight\":{\"type\":\"number\",\"minimum\":0,\"maximum\":1000000},\"benchmark_weight\":{\"type\":\"number\",\"minimum\":0,\"maximum\":1000000},\"preference_weight\":{\"type\":\"number\",\"minimum\":0,\"maximum\":1000000},\"cost_weight\":{\"type\":\"number\",\"minimum\":0,\"maximum\":1000000},\"latency_weight\":{\"type\":\"number\",\"minimum\":0,\"maximum\":1000000},\"failure_weight\":{\"type\":\"number\",\"minimum\":0,\"maximum\":1000000},\"uncertainty_weight\":{\"type\":\"number\",\"minimum\":0,\"maximum\":1000000},\"quality_lcb_z\":{\"type\":\"number\",\"minimum\":0,\"maximum\":1000000},\"missing_metadata_penalty\":{\"type\":\"number\",\"minimum\":0,\"maximum\":1000000},\"max_cost_usd\":{\"type\":\"number\",\"minimum\":0,\"maximum\":1000000},\"max_latency_s\":{\"type\":\"number\",\"minimum\":0,\"maximum\":1000000},\"max_failure_probability\":{\"type\":\"number\",\"minimum\":0,\"maximum\":1},\"quality_floor\":{\"type\":\"number\",\"minimum\":0,\"maximum\":1},\"min_context\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":10000000},\"required_modalities\":{\"type\":\"array\",\"maxItems\":25,\"items\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":200}},\"required_output_modalities\":{\"type\":\"array\",\"maxItems\":25,\"items\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":200}},\"required_parameters\":{\"type\":\"array\",\"maxItems\":25,\"items\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":200}},\"allowed_models\":{\"type\":\"array\",\"maxItems\":25,\"items\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":200}},\"denied_models\":{\"type\":\"array\",\"maxItems\":25,\"items\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":200}},\"allowed_providers\":{\"type\":\"array\",\"maxItems\":25,\"items\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":200}},\"denied_providers\":{\"type\":\"array\",\"maxItems\":25,\"items\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":200}},\"allow_additional_output_modalities\":{\"type\":\"boolean\"},\"zdr_required\":{\"type\":\"boolean\"},\"require_known_price\":{\"type\":\"boolean\"},\"include_expired\":{\"type\":\"boolean\"},\"allow_batch\":{\"type\":\"boolean\"},\"allow_router_models\":{\"type\":\"boolean\"},\"required_region\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":64}},\"additionalProperties\":false},\"request\":{\"type\":\"object\",\"properties\":{\"task\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":16384},\"max_output_tokens\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":4096},\"strategy\":{\"type\":\"string\",\"enum\":[\"auto\",\"direct\",\"cascade\",\"parallel\"]},\"max_calls\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":3},\"max_expected_cost_usd\":{\"type\":\"number\",\"minimum\":0,\"maximum\":0.2},\"max_worst_case_cost_usd\":{\"type\":\"number\",\"minimum\":0,\"maximum\":0.25},\"latency_preference\":{\"type\":\"string\",\"enum\":[\"low\",\"balanced\",\"quality\"]},\"max_expected_latency_s\":{\"type\":\"number\",\"minimum\":0,\"maximum\":1000000},\"max_worst_case_latency_s\":{\"type\":\"number\",\"minimum\":0,\"maximum\":1000000},\"allow_cascade\":{\"type\":\"boolean\"},\"allow_parallel\":{\"type\":\"boolean\"}},\"required\":[\"task\"],\"additionalProperties\":false}},\"required\":[\"action\",\"policy\",\"request\"],\"additionalProperties\":false}";
const char lingo_chimera_output_schema[] = "{\"type\":\"object\",\"properties\":{\"ok\":{\"type\":\"boolean\"},\"profile\":{\"const\":\"chimera_plan\"},\"executed\":{\"const\":false},\"scope\":{\"const\":\"request\"},\"observed_at\":{\"type\":\"string\"},\"policy\":{\"type\":\"object\",\"properties\":{\"success_weight\":{\"type\":\"number\",\"minimum\":0,\"maximum\":1000000},\"quality_weight\":{\"type\":\"number\",\"minimum\":0,\"maximum\":1000000},\"benchmark_weight\":{\"type\":\"number\",\"minimum\":0,\"maximum\":1000000},\"preference_weight\":{\"type\":\"number\",\"minimum\":0,\"maximum\":1000000},\"cost_weight\":{\"type\":\"number\",\"minimum\":0,\"maximum\":1000000},\"latency_weight\":{\"type\":\"number\",\"minimum\":0,\"maximum\":1000000},\"failure_weight\":{\"type\":\"number\",\"minimum\":0,\"maximum\":1000000},\"uncertainty_weight\":{\"type\":\"number\",\"minimum\":0,\"maximum\":1000000},\"quality_lcb_z\":{\"type\":\"number\",\"minimum\":0,\"maximum\":1000000},\"missing_metadata_penalty\":{\"type\":\"number\",\"minimum\":0,\"maximum\":1000000},\"max_cost_usd\":{\"type\":\"number\",\"minimum\":0,\"maximum\":1000000},\"max_latency_s\":{\"type\":\"number\",\"minimum\":0,\"maximum\":1000000},\"max_failure_probability\":{\"type\":\"number\",\"minimum\":0,\"maximum\":1},\"quality_floor\":{\"type\":\"number\",\"minimum\":0,\"maximum\":1},\"min_context\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":10000000},\"required_modalities\":{\"type\":\"array\",\"maxItems\":25,\"items\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":200}},\"required_output_modalities\":{\"type\":\"array\",\"maxItems\":25,\"items\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":200}},\"required_parameters\":{\"type\":\"array\",\"maxItems\":25,\"items\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":200}},\"allowed_models\":{\"type\":\"array\",\"maxItems\":25,\"items\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":200}},\"denied_models\":{\"type\":\"array\",\"maxItems\":25,\"items\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":200}},\"allowed_providers\":{\"type\":\"array\",\"maxItems\":25,\"items\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":200}},\"denied_providers\":{\"type\":\"array\",\"maxItems\":25,\"items\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":200}},\"allow_additional_output_modalities\":{\"type\":\"boolean\"},\"zdr_required\":{\"type\":\"boolean\"},\"require_known_price\":{\"type\":\"boolean\"},\"include_expired\":{\"type\":\"boolean\"},\"allow_batch\":{\"type\":\"boolean\"},\"allow_router_models\":{\"type\":\"boolean\"},\"required_region\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":64}},\"additionalProperties\":false},\"request\":{\"type\":\"object\",\"properties\":{\"task\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":16384},\"max_output_tokens\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":4096},\"strategy\":{\"type\":\"string\",\"enum\":[\"auto\",\"direct\",\"cascade\",\"parallel\"]},\"max_calls\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":3},\"max_expected_cost_usd\":{\"type\":\"number\",\"minimum\":0,\"maximum\":0.2},\"max_worst_case_cost_usd\":{\"type\":\"number\",\"minimum\":0,\"maximum\":0.25},\"latency_preference\":{\"type\":\"string\",\"enum\":[\"low\",\"balanced\",\"quality\"]},\"max_expected_latency_s\":{\"type\":\"number\",\"minimum\":0,\"maximum\":1000000},\"max_worst_case_latency_s\":{\"type\":\"number\",\"minimum\":0,\"maximum\":1000000},\"allow_cascade\":{\"type\":\"boolean\"},\"allow_parallel\":{\"type\":\"boolean\"}},\"required\":[\"task\"],\"additionalProperties\":false},\"decision\":{\"type\":\"object\"},\"error\":{\"type\":\"object\",\"properties\":{\"code\":{\"type\":\"string\"},\"message\":{\"type\":\"string\"},\"http_status\":{\"type\":\"integer\"}},\"required\":[\"code\",\"message\"],\"additionalProperties\":false}},\"required\":[\"ok\"],\"additionalProperties\":false,\"oneOf\":[{\"properties\":{\"ok\":{\"const\":true}},\"required\":[\"profile\",\"executed\",\"scope\",\"observed_at\",\"policy\",\"request\",\"decision\"]},{\"properties\":{\"ok\":{\"const\":false}},\"required\":[\"error\"]}]}";
const char lingo_chimera_execute_schema[] = "{\"type\":\"object\",\"properties\":{\"action\":{\"const\":\"execute\"},\"policy\":{\"type\":\"object\",\"properties\":{\"success_weight\":{\"type\":\"number\",\"minimum\":0,\"maximum\":1000000},\"quality_weight\":{\"type\":\"number\",\"minimum\":0,\"maximum\":1000000},\"benchmark_weight\":{\"type\":\"number\",\"minimum\":0,\"maximum\":1000000},\"preference_weight\":{\"type\":\"number\",\"minimum\":0,\"maximum\":1000000},\"cost_weight\":{\"type\":\"number\",\"minimum\":0,\"maximum\":1000000},\"latency_weight\":{\"type\":\"number\",\"minimum\":0,\"maximum\":1000000},\"failure_weight\":{\"type\":\"number\",\"minimum\":0,\"maximum\":1000000},\"uncertainty_weight\":{\"type\":\"number\",\"minimum\":0,\"maximum\":1000000},\"quality_lcb_z\":{\"type\":\"number\",\"minimum\":0,\"maximum\":1000000},\"missing_metadata_penalty\":{\"type\":\"number\",\"minimum\":0,\"maximum\":1000000},\"max_cost_usd\":{\"type\":\"number\",\"minimum\":0,\"maximum\":1000000},\"max_latency_s\":{\"type\":\"number\",\"minimum\":0,\"maximum\":1000000},\"max_failure_probability\":{\"type\":\"number\",\"minimum\":0,\"maximum\":1},\"quality_floor\":{\"type\":\"number\",\"minimum\":0,\"maximum\":1},\"min_context\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":10000000},\"required_modalities\":{\"type\":\"array\",\"maxItems\":25,\"items\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":200}},\"required_output_modalities\":{\"type\":\"array\",\"maxItems\":25,\"items\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":200}},\"required_parameters\":{\"type\":\"array\",\"maxItems\":25,\"items\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":200}},\"allowed_models\":{\"type\":\"array\",\"maxItems\":25,\"items\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":200}},\"denied_models\":{\"type\":\"array\",\"maxItems\":25,\"items\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":200}},\"allowed_providers\":{\"type\":\"array\",\"maxItems\":25,\"items\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":200}},\"denied_providers\":{\"type\":\"array\",\"maxItems\":25,\"items\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":200}},\"allow_additional_output_modalities\":{\"type\":\"boolean\"},\"zdr_required\":{\"type\":\"boolean\"},\"require_known_price\":{\"const\":true},\"include_expired\":{\"type\":\"boolean\"},\"allow_batch\":{\"type\":\"boolean\"},\"allow_router_models\":{\"type\":\"boolean\"},\"required_region\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":64}},\"additionalProperties\":false},\"request\":{\"type\":\"object\",\"properties\":{\"task\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":16384},\"max_output_tokens\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":512},\"strategy\":{\"const\":\"direct\"},\"max_calls\":{\"const\":1},\"max_expected_cost_usd\":{\"type\":\"number\",\"minimum\":0,\"maximum\":0.01},\"max_worst_case_cost_usd\":{\"type\":\"number\",\"minimum\":0,\"maximum\":0.01},\"latency_preference\":{\"type\":\"string\",\"enum\":[\"low\",\"balanced\",\"quality\"]},\"max_expected_latency_s\":{\"type\":\"number\",\"minimum\":0,\"maximum\":1000000},\"max_worst_case_latency_s\":{\"type\":\"number\",\"minimum\":0,\"maximum\":1000000},\"allow_cascade\":{\"type\":\"boolean\"},\"allow_parallel\":{\"type\":\"boolean\"}},\"required\":[\"task\",\"max_output_tokens\",\"strategy\",\"max_calls\",\"max_expected_cost_usd\",\"max_worst_case_cost_usd\"],\"additionalProperties\":false},\"expected_plan\":{\"type\":\"object\",\"properties\":{\"format\":{\"const\":\"dsco.chimera.direct/1\"},\"execution_id\":{\"type\":\"string\",\"minLength\":36,\"maxLength\":36,\"pattern\":\"^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$\"},\"request_sha256\":{\"type\":\"string\",\"minLength\":64,\"maxLength\":64,\"pattern\":\"^[0-9a-f]{64}$\"},\"plan_sha256\":{\"type\":\"string\",\"minLength\":64,\"maxLength\":64,\"pattern\":\"^[0-9a-f]{64}$\"}},\"required\":[\"format\",\"execution_id\",\"request_sha256\",\"plan_sha256\"],\"additionalProperties\":false}},\"required\":[\"action\",\"policy\",\"request\",\"expected_plan\"],\"additionalProperties\":false,\"description\":\"Execute the exact captured direct plan once. At most 512 output tokens and 0.01 USD planner estimates; estimates are not a billing guarantee. No automatic retry after uncertain completion.\"}";
const char lingo_chimera_execute_output_schema[] = "{\"type\":\"object\",\"properties\":{\"ok\":{\"type\":\"boolean\"},\"profile\":{\"const\":\"chimera_completion\"},\"executed\":{\"const\":true},\"scope\":{\"const\":\"request\"},\"observed_at\":{\"type\":\"string\"},\"policy\":{\"type\":\"object\",\"properties\":{\"success_weight\":{\"type\":\"number\",\"minimum\":0,\"maximum\":1000000},\"quality_weight\":{\"type\":\"number\",\"minimum\":0,\"maximum\":1000000},\"benchmark_weight\":{\"type\":\"number\",\"minimum\":0,\"maximum\":1000000},\"preference_weight\":{\"type\":\"number\",\"minimum\":0,\"maximum\":1000000},\"cost_weight\":{\"type\":\"number\",\"minimum\":0,\"maximum\":1000000},\"latency_weight\":{\"type\":\"number\",\"minimum\":0,\"maximum\":1000000},\"failure_weight\":{\"type\":\"number\",\"minimum\":0,\"maximum\":1000000},\"uncertainty_weight\":{\"type\":\"number\",\"minimum\":0,\"maximum\":1000000},\"quality_lcb_z\":{\"type\":\"number\",\"minimum\":0,\"maximum\":1000000},\"missing_metadata_penalty\":{\"type\":\"number\",\"minimum\":0,\"maximum\":1000000},\"max_cost_usd\":{\"type\":\"number\",\"minimum\":0,\"maximum\":1000000},\"max_latency_s\":{\"type\":\"number\",\"minimum\":0,\"maximum\":1000000},\"max_failure_probability\":{\"type\":\"number\",\"minimum\":0,\"maximum\":1},\"quality_floor\":{\"type\":\"number\",\"minimum\":0,\"maximum\":1},\"min_context\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":10000000},\"required_modalities\":{\"type\":\"array\",\"maxItems\":25,\"items\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":200}},\"required_output_modalities\":{\"type\":\"array\",\"maxItems\":25,\"items\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":200}},\"required_parameters\":{\"type\":\"array\",\"maxItems\":25,\"items\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":200}},\"allowed_models\":{\"type\":\"array\",\"maxItems\":25,\"items\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":200}},\"denied_models\":{\"type\":\"array\",\"maxItems\":25,\"items\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":200}},\"allowed_providers\":{\"type\":\"array\",\"maxItems\":25,\"items\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":200}},\"denied_providers\":{\"type\":\"array\",\"maxItems\":25,\"items\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":200}},\"allow_additional_output_modalities\":{\"type\":\"boolean\"},\"zdr_required\":{\"type\":\"boolean\"},\"require_known_price\":{\"type\":\"boolean\"},\"include_expired\":{\"type\":\"boolean\"},\"allow_batch\":{\"type\":\"boolean\"},\"allow_router_models\":{\"type\":\"boolean\"},\"required_region\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":64}},\"additionalProperties\":false},\"request\":{\"type\":\"object\",\"properties\":{\"task\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":16384},\"max_output_tokens\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":4096},\"strategy\":{\"type\":\"string\",\"enum\":[\"auto\",\"direct\",\"cascade\",\"parallel\"]},\"max_calls\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":3},\"max_expected_cost_usd\":{\"type\":\"number\",\"minimum\":0,\"maximum\":0.2},\"max_worst_case_cost_usd\":{\"type\":\"number\",\"minimum\":0,\"maximum\":0.25},\"latency_preference\":{\"type\":\"string\",\"enum\":[\"low\",\"balanced\",\"quality\"]},\"max_expected_latency_s\":{\"type\":\"number\",\"minimum\":0,\"maximum\":1000000},\"max_worst_case_latency_s\":{\"type\":\"number\",\"minimum\":0,\"maximum\":1000000},\"allow_cascade\":{\"type\":\"boolean\"},\"allow_parallel\":{\"type\":\"boolean\"}},\"required\":[\"task\"],\"additionalProperties\":false},\"error\":{\"type\":\"object\",\"properties\":{\"code\":{\"type\":\"string\"},\"message\":{\"type\":\"string\"},\"http_status\":{\"type\":\"integer\"}},\"required\":[\"code\",\"message\"],\"additionalProperties\":false},\"completion\":{\"type\":\"object\",\"required\":[\"model\",\"choices\",\"execution\",\"chimera\"],\"properties\":{\"model\":{\"const\":\"dsco-router/chimera:latest\"},\"choices\":{\"type\":\"array\",\"minItems\":1,\"maxItems\":1},\"execution\":{\"type\":\"object\",\"required\":[\"contract\",\"status\",\"dispatch_attempts\",\"router_retries\",\"provider_fallbacks\",\"budget_basis\",\"measured\"],\"properties\":{\"contract\":{\"type\":\"object\",\"properties\":{\"format\":{\"const\":\"dsco.chimera.direct/1\"},\"execution_id\":{\"type\":\"string\",\"minLength\":36,\"maxLength\":36,\"pattern\":\"^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$\"},\"request_sha256\":{\"type\":\"string\",\"minLength\":64,\"maxLength\":64,\"pattern\":\"^[0-9a-f]{64}$\"},\"plan_sha256\":{\"type\":\"string\",\"minLength\":64,\"maxLength\":64,\"pattern\":\"^[0-9a-f]{64}$\"}},\"required\":[\"format\",\"execution_id\",\"request_sha256\",\"plan_sha256\"],\"additionalProperties\":false},\"status\":{\"const\":\"completed\"},\"dispatch_attempts\":{\"const\":1},\"router_retries\":{\"const\":0},\"provider_fallbacks\":{\"const\":false},\"budget_basis\":{\"const\":\"planner_estimate_not_billing_guarantee\"},\"measured\":{\"type\":\"object\",\"required\":[\"provider\",\"requested_model\",\"reported_model\",\"provider_response_id\",\"usage\",\"elapsed_ms\",\"run_id\",\"attempt_id\",\"request_id\"]}}}}}},\"required\":[\"ok\"],\"additionalProperties\":false,\"oneOf\":[{\"properties\":{\"ok\":{\"const\":true}},\"required\":[\"profile\",\"executed\",\"scope\",\"observed_at\",\"policy\",\"request\",\"completion\"]},{\"properties\":{\"ok\":{\"const\":false}},\"required\":[\"error\"]}]}";
static const char policy_defaults[] = "{\"success_weight\":0.05,\"quality_weight\":0.0,\"benchmark_weight\":0.75,\"preference_weight\":0.01,\"cost_weight\":25.0,\"latency_weight\":0.0005,\"failure_weight\":0.05,\"uncertainty_weight\":0.05,\"quality_lcb_z\":1.0,\"missing_metadata_penalty\":0.05,\"min_context\":0,\"required_modalities\":[\"text\"],\"required_output_modalities\":[\"text\"],\"allow_additional_output_modalities\":false,\"required_parameters\":[],\"allowed_models\":[],\"denied_models\":[],\"allowed_providers\":[],\"denied_providers\":[],\"zdr_required\":false,\"require_known_price\":true,\"include_expired\":false,\"allow_batch\":false,\"allow_router_models\":false}";
static const char request_defaults[] = "{\"max_output_tokens\":256,\"strategy\":\"auto\",\"max_calls\":3,\"max_expected_cost_usd\":0.2,\"max_worst_case_cost_usd\":0.25,\"latency_preference\":\"quality\",\"allow_cascade\":true,\"allow_parallel\":true}";

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
static CURLU *configured_origin(const char *tool_name) {
    const char *base = getenv("CHIMERA_HOST");
    if (!base)
        base = "http://127.0.0.1:8788";
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
    if (!service_destination_allowed(tool_name, base)) {
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


/* A small exact subset of the real Router dataclasses. No aliases or coercion:
 * these fields pass directly to RoutingPolicy and PlanningPolicy. */
typedef enum { F_NUMBER, F_INTEGER, F_STRING, F_ARRAY, F_BOOL } field_kind;
typedef struct { const char *name; field_kind kind; double minimum, maximum; } field_spec;
static const field_spec policy_fields[] = {
    {"success_weight", F_NUMBER, 0, 1000000},
    {"quality_weight", F_NUMBER, 0, 1000000},
    {"benchmark_weight", F_NUMBER, 0, 1000000},
    {"preference_weight", F_NUMBER, 0, 1000000},
    {"cost_weight", F_NUMBER, 0, 1000000},
    {"latency_weight", F_NUMBER, 0, 1000000},
    {"failure_weight", F_NUMBER, 0, 1000000},
    {"uncertainty_weight", F_NUMBER, 0, 1000000},
    {"quality_lcb_z", F_NUMBER, 0, 1000000},
    {"missing_metadata_penalty", F_NUMBER, 0, 1000000},
    {"max_cost_usd", F_NUMBER, 0, 1000000},
    {"max_latency_s", F_NUMBER, 0, 1000000},
    {"max_failure_probability", F_NUMBER, 0, 1},
    {"quality_floor", F_NUMBER, 0, 1},
    {"min_context", F_INTEGER, 0, 10000000},
    {"required_modalities", F_ARRAY, 0, 25},
    {"required_output_modalities", F_ARRAY, 0, 25},
    {"required_parameters", F_ARRAY, 0, 25},
    {"allowed_models", F_ARRAY, 0, 25},
    {"denied_models", F_ARRAY, 0, 25},
    {"allowed_providers", F_ARRAY, 0, 25},
    {"denied_providers", F_ARRAY, 0, 25},
    {"allow_additional_output_modalities", F_BOOL, 0, 32},
    {"zdr_required", F_BOOL, 0, 32},
    {"require_known_price", F_BOOL, 0, 32},
    {"include_expired", F_BOOL, 0, 32},
    {"allow_batch", F_BOOL, 0, 32},
    {"allow_router_models", F_BOOL, 0, 32},
    {"required_region", F_STRING, 1, 64},
};
static const field_spec request_fields[] = {
    {"task", F_STRING, 1, 16384},
    {"max_output_tokens", F_INTEGER, 1, 4096},
    {"strategy", F_STRING, 0, 32},
    {"max_calls", F_INTEGER, 1, 3},
    {"max_expected_cost_usd", F_NUMBER, 0, 0.2},
    {"max_worst_case_cost_usd", F_NUMBER, 0, 0.25},
    {"latency_preference", F_STRING, 0, 32},
    {"max_expected_latency_s", F_NUMBER, 0, 1000000},
    {"max_worst_case_latency_s", F_NUMBER, 0, 1000000},
    {"allow_cascade", F_BOOL, 0, 32},
    {"allow_parallel", F_BOOL, 0, 32},
};

static bool bounded_string(yyjson_val *v, size_t minimum, size_t maximum) {
    return yyjson_is_str(v) && yyjson_get_len(v) >= minimum && yyjson_get_len(v) <= maximum &&
           strlen(yyjson_get_str(v)) == yyjson_get_len(v);
}

static bool validate_record(yyjson_val *record, const field_spec *specs, size_t length) {
    if (!yyjson_is_obj(record)) return false;
    size_t i, count;
    yyjson_val *key, *value;
    yyjson_obj_foreach(record, i, count, key, value) {
        const field_spec *spec = NULL;
        for (size_t j = 0; j < length; ++j)
            if (text_is(key, specs[j].name)) { spec = &specs[j]; break; }
        if (!spec) return false;
        if (spec->kind == F_BOOL) {
            if (!yyjson_is_bool(value)) return false;
        } else if (spec->kind == F_STRING) {
            if (!bounded_string(value, (size_t)spec->minimum, (size_t)spec->maximum)) return false;
        } else if (spec->kind == F_ARRAY) {
            if (!yyjson_is_arr(value) || yyjson_arr_size(value) > (size_t)spec->maximum) return false;
            size_t j, n; yyjson_val *item;
            yyjson_arr_foreach(value, j, n, item)
                if (!bounded_string(item, 1, 200)) return false;
        } else {
            double number = yyjson_get_num(value);
            if (!yyjson_is_num(value) || !isfinite(number) || number < spec->minimum ||
                number > spec->maximum || (spec->kind == F_INTEGER && floor(number) != number)) return false;
        }
    }
    return true;
}

static yyjson_mut_val *normalized(yyjson_mut_doc *doc, const char *defaults, yyjson_val *input) {
    yyjson_doc *base = yyjson_read(defaults, strlen(defaults), 0);
    yyjson_mut_val *output = base ? yyjson_val_mut_copy(doc, yyjson_doc_get_root(base)) : NULL;
    yyjson_doc_free(base);
    if (!output) return NULL;
    size_t i, count; yyjson_val *key, *value;
    yyjson_obj_foreach(input, i, count, key, value) {
        yyjson_mut_val *k = yyjson_val_mut_copy(doc, key), *v = yyjson_val_mut_copy(doc, value);
        if (!k || !v || !yyjson_mut_obj_put(output, k, v)) return NULL;
    }
    return output;
}

static bool string_array(yyjson_val *v, size_t maximum) {
    if (!yyjson_is_arr(v) || yyjson_arr_size(v) > maximum) return false;
    size_t i, count; yyjson_val *item;
    yyjson_arr_foreach(v, i, count, item)
        if (!bounded_string(item, 1, 512)) return false;
    return true;
}

static bool decision_valid(yyjson_val *data, yyjson_mut_val *requested_plan) {
    yyjson_val *selected = yyjson_obj_get(data, "selected_model");
    yyjson_val *patch = yyjson_obj_get(data, "request_patch");
    yyjson_val *receipt = yyjson_obj_get(data, "orchestration");
    yyjson_val *kind = yyjson_obj_get(receipt, "plan_type");
    if (!bounded_string(selected, 1, 512) ||
        !string_array(yyjson_obj_get(data, "fallback_models"), 8) ||
        !string_array(yyjson_obj_get(data, "reasons"), 32) ||
        !text_is(yyjson_obj_get(data, "policy_source"), "request") ||
        !text_is(yyjson_obj_get(data, "model_identity"), CHIMERA_MODEL) ||
        !text_is(yyjson_obj_get(patch, "model"), yyjson_get_str(selected)) ||
        !response_text(yyjson_obj_get(patch, "provider")) ||
        !text_is(yyjson_obj_get(receipt, "model"), CHIMERA_MODEL) ||
        !text_is(yyjson_obj_get(receipt, "selected_model"), yyjson_get_str(selected)) ||
        !response_text(yyjson_obj_get(receipt, "runtime")) ||
        !yyjson_is_arr(yyjson_obj_get(receipt, "stages")) ||
        !(text_is(kind, "direct") || text_is(kind, "cascade") || text_is(kind, "parallel"))) return false;
    /* The ranking winner in explainability may precede plan-level budget
     * filtering. Identity must instead agree with the admitted plan's actual
     * representative role and its unique model-call stage. */
    yyjson_val *roles = yyjson_obj_get(receipt, "roles");
    yyjson_val *primary;
    const char *primary_role;
    if (text_is(kind, "direct")) {
        primary = yyjson_obj_get(roles, "primary"); primary_role = "primary";
    } else if (text_is(kind, "cascade")) {
        primary = yyjson_obj_get(roles, "escalation"); primary_role = "escalation";
    } else {
        primary = yyjson_arr_get(yyjson_obj_get(roles, "candidates"), 0); primary_role = "candidate_a";
    }
    if (!text_is(primary, yyjson_get_str(selected))) return false;
    size_t stage_index, stage_count, matching_stages = 0;
    yyjson_val *stage;
    yyjson_arr_foreach(yyjson_obj_get(receipt, "stages"), stage_index, stage_count, stage) {
        if (text_is(yyjson_obj_get(stage, "role"), primary_role)) {
            if (!text_is(yyjson_obj_get(stage, "kind"), "model_call") ||
                !text_is(yyjson_obj_get(stage, "model"), yyjson_get_str(selected))) return false;
            matching_stages++;
        }
    }
    if (matching_stages != 1) return false;
    yyjson_val *calls = yyjson_obj_get(receipt, "worst_case_calls");
    yyjson_val *expected = yyjson_obj_get(receipt, "expected_cost_usd");
    yyjson_val *worst = yyjson_obj_get(receipt, "worst_case_cost_usd");
    double max_calls = yyjson_mut_get_num(yyjson_mut_obj_get(requested_plan, "max_calls"));
    double max_expected = yyjson_mut_get_num(yyjson_mut_obj_get(requested_plan, "max_expected_cost_usd"));
    double max_worst = yyjson_mut_get_num(yyjson_mut_obj_get(requested_plan, "max_worst_case_cost_usd"));
    if (!yyjson_is_num(calls) || yyjson_get_num(calls) < 1 || yyjson_get_num(calls) > max_calls ||
        floor(yyjson_get_num(calls)) != yyjson_get_num(calls) ||
        !yyjson_is_num(expected) || yyjson_get_num(expected) < 0 || yyjson_get_num(expected) > max_expected + 1e-12 ||
        !yyjson_is_num(worst) || yyjson_get_num(worst) < 0 || yyjson_get_num(worst) > max_worst + 1e-12) return false;
    const char *strategy = yyjson_mut_get_str(yyjson_mut_obj_get(requested_plan, "strategy"));
    if ((!strcmp(strategy, "direct") && !text_is(kind, "direct")) ||
        (!strcmp(strategy, "cascade") && text_is(kind, "parallel")) ||
        (!strcmp(strategy, "parallel") && text_is(kind, "cascade")) ||
        (text_is(kind, "parallel") && !yyjson_mut_get_bool(yyjson_mut_obj_get(requested_plan, "allow_parallel"))) ||
        (text_is(kind, "cascade") && !yyjson_mut_get_bool(yyjson_mut_obj_get(requested_plan, "allow_cascade")))) return false;
    const char *hashes[] = {"operational_artifact_sha256", "quality_artifact_sha256",
                            "hypercube_snapshot_id", "hypercube_manifest_sha256",
                            "hypercube_projection_sha256"};
    for (size_t i = 0; i < sizeof(hashes)/sizeof(*hashes); ++i) {
        yyjson_val *value = yyjson_obj_get(receipt, hashes[i]);
        if (!bounded_string(value, 64, 64)) return false;
        const char *s = yyjson_get_str(value);
        for (size_t j = 0; j < 64; ++j)
            if (!((s[j] >= '0' && s[j] <= '9') || (s[j] >= 'a' && s[j] <= 'f'))) return false;
    }
    return true;
}

static bool refusal(char *out, size_t capacity, yyjson_val *data, long status) {
    yyjson_val *message = yyjson_obj_get(yyjson_obj_get(data, "error"), "message");
    if (!bounded_string(message, 1, 2048) ||
        strncmp(yyjson_get_str(message), "chimera refused the request:", sizeof("chimera refused the request:") - 1)) return false;
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc), *error = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_bool(doc, root, "ok", false);
    yyjson_mut_obj_add_val(doc, root, "error", error);
    yyjson_mut_obj_add_str(doc, error, "code", "route_refused");
    yyjson_mut_obj_add_val(doc, error, "message", yyjson_val_mut_copy(doc, message));
    yyjson_mut_obj_add_int(doc, error, "http_status", status);
    size_t length = 0; char *encoded = yyjson_mut_write(doc, 0, &length);
    bool fits = encoded && length < capacity;
    if (fits) memcpy(out, encoded, length + 1);
    free(encoded); yyjson_mut_doc_free(doc);
    return fits;
}

static bool sha_string(yyjson_val *v) {
    if (!bounded_string(v, 64, 64)) return false;
    const char *s = yyjson_get_str(v);
    for (size_t i = 0; i < 64; ++i)
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) return false;
    return true;
}

static bool contract_valid(yyjson_val *v) {
    if (!yyjson_is_obj(v) || yyjson_obj_size(v) != 4 ||
        !text_is(yyjson_obj_get(v, "format"), "dsco.chimera.direct/1") ||
        !sha_string(yyjson_obj_get(v, "request_sha256")) ||
        !sha_string(yyjson_obj_get(v, "plan_sha256")) ||
        !bounded_string(yyjson_obj_get(v, "execution_id"), 36, 36)) return false;
    const char *id = yyjson_get_str(yyjson_obj_get(v, "execution_id"));
    for (size_t i = 0; i < 36; ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) { if (id[i] != '-') return false; }
        else if (!((id[i] >= '0' && id[i] <= '9') || (id[i] >= 'a' && id[i] <= 'f'))) return false;
    }
    return true;
}

static bool exact_num(yyjson_val *v, double n) {
    return yyjson_is_num(v) && yyjson_get_num(v) == n;
}

static bool completion_valid(yyjson_val *data, yyjson_val *expected) {
    yyjson_val *execution = yyjson_obj_get(data, "execution");
    yyjson_val *contract = yyjson_obj_get(execution, "contract");
    yyjson_val *measured = yyjson_obj_get(execution, "measured");
    yyjson_val *selected = yyjson_obj_get(yyjson_obj_get(data, "chimera"), "selected_model");
    yyjson_val *choices = yyjson_obj_get(data, "choices");
    if (!text_is(yyjson_obj_get(data, "model"), CHIMERA_MODEL) ||
        !contract_valid(contract) || !text_is(yyjson_obj_get(execution, "status"), "completed") ||
        !exact_num(yyjson_obj_get(execution, "dispatch_attempts"), 1) ||
        !exact_num(yyjson_obj_get(execution, "router_retries"), 0) ||
        !yyjson_is_false(yyjson_obj_get(execution, "provider_fallbacks")) ||
        !text_is(yyjson_obj_get(execution, "budget_basis"), "planner_estimate_not_billing_guarantee") ||
        !text_is(yyjson_obj_get(measured, "provider"), "openrouter") ||
        !bounded_string(selected, 1, 512) ||
        !text_is(yyjson_obj_get(measured, "requested_model"), yyjson_get_str(selected)) ||
        !bounded_string(yyjson_obj_get(measured, "reported_model"), 1, 512) ||
        !bounded_string(yyjson_obj_get(measured, "provider_response_id"), 1, 512) ||
        !bounded_string(yyjson_obj_get(measured, "request_id"), 1, 128) ||
        !bounded_string(yyjson_obj_get(measured, "run_id"), 1, 128) ||
        !bounded_string(yyjson_obj_get(measured, "attempt_id"), 1, 128) ||
        !yyjson_is_arr(choices) || yyjson_arr_size(choices) != 1 ||
        !response_text(yyjson_obj_get(yyjson_obj_get(yyjson_arr_get(choices, 0), "message"), "content"))) return false;
    const char *keys[] = {"format", "execution_id", "request_sha256", "plan_sha256"};
    for (size_t i = 0; i < sizeof(keys)/sizeof(*keys); ++i)
        if (!text_is(yyjson_obj_get(contract, keys[i]), yyjson_get_str(yyjson_obj_get(expected, keys[i])))) return false;
    yyjson_val *elapsed = yyjson_obj_get(measured, "elapsed_ms"), *usage = yyjson_obj_get(measured, "usage");
    return yyjson_is_num(elapsed) && yyjson_get_num(elapsed) >= 0 && floor(yyjson_get_num(elapsed)) == yyjson_get_num(elapsed) &&
           (yyjson_is_obj(usage) || yyjson_is_null(usage));
}

static bool chimera_request(const char *input, char *result, size_t capacity, bool executing) {
    if (result && capacity) result[0] = '\0';
    if (!result || capacity < 128) return fail(result, capacity, "response_buffer_too_small", "Result buffer too small", 0);
    size_t input_length = input ? strnlen(input, INPUT_LIMIT + 1) : 0;
    if (!input_length || input_length > INPUT_LIMIT) return fail(result, capacity, "invalid_request", "Expected a bounded planning record", 0);
    yyjson_doc *request = yyjson_read(input, input_length, 0);
    yyjson_val *root = request ? yyjson_doc_get_root(request) : NULL;
    yyjson_val *policy_in = yyjson_obj_get(root, "policy"), *plan_in = yyjson_obj_get(root, "request");
    bool valid = yyjson_is_obj(root) && yyjson_obj_size(root) == (executing ? 4u : 3u) &&
        text_is(yyjson_obj_get(root, "action"), executing ? "execute" : "plan") && validate_json(root, 0) == RESPONSE_VALID &&
        validate_record(policy_in, policy_fields, sizeof(policy_fields)/sizeof(*policy_fields)) &&
        validate_record(plan_in, request_fields, sizeof(request_fields)/sizeof(*request_fields)) &&
        bounded_string(yyjson_obj_get(plan_in, "task"), 1, 16384);
    yyjson_val *strategy = yyjson_obj_get(plan_in, "strategy"), *latency = yyjson_obj_get(plan_in, "latency_preference");
    if (strategy && !(text_is(strategy, "auto") || text_is(strategy, "direct") || text_is(strategy, "cascade") || text_is(strategy, "parallel"))) valid = false;
    if (latency && !(text_is(latency, "low") || text_is(latency, "balanced") || text_is(latency, "quality"))) valid = false;
    if (executing) {
        yyjson_val *tokens = yyjson_obj_get(plan_in, "max_output_tokens");
        yyjson_val *expected_cost = yyjson_obj_get(plan_in, "max_expected_cost_usd");
        yyjson_val *worst_cost = yyjson_obj_get(plan_in, "max_worst_case_cost_usd");
        if (!contract_valid(yyjson_obj_get(root, "expected_plan")) || !text_is(strategy, "direct") ||
            !exact_num(yyjson_obj_get(plan_in, "max_calls"), 1) ||
            !yyjson_is_num(tokens) || yyjson_get_num(tokens) > 512 ||
            !yyjson_is_num(expected_cost) || yyjson_get_num(expected_cost) > 0.01 ||
            !yyjson_is_num(worst_cost) || yyjson_get_num(worst_cost) > 0.01 ||
            yyjson_is_false(yyjson_obj_get(policy_in, "require_known_price"))) valid = false;
    }
    if (!valid) { yyjson_doc_free(request); return fail(result, capacity, "invalid_request", "Invalid Chimera policy or planning arguments", 0); }

    dsco_http_global_init();
    CURLU *url = configured_origin(executing ? "chimera_execute" : "chimera_route"); CURL *easy = NULL; struct curl_slist *headers = NULL;
    body_t body = {0}; yyjson_doc *response = NULL;
    yyjson_mut_doc *wire = yyjson_mut_doc_new(NULL), *output = NULL;
    char *encoded = NULL, *serialized = NULL;
    const char *code = "bad_configuration", *message = "Invalid Chimera connection origin";
    long status = 0; bool success = false, preserved_refusal = false, dispatched = false;
    if (!url) goto done;
    if (!wire) { code = "allocation_failed"; message = "Cannot allocate planning request"; goto done; }
    yyjson_mut_val *policy = normalized(wire, policy_defaults, policy_in);
    yyjson_mut_val *plan = normalized(wire, request_defaults, plan_in);
    yyjson_mut_val *payload = yyjson_mut_obj(wire), *messages = yyjson_mut_arr(wire), *user = yyjson_mut_obj(wire);
    yyjson_mut_val *planning = plan ? yyjson_mut_val_mut_copy(wire, plan) : NULL;
    if (!policy || !plan || !payload || !messages || !user || !planning) { code = "allocation_failed"; message = "Cannot allocate planning request"; goto done; }
    yyjson_mut_doc_set_root(wire, payload);
    yyjson_mut_obj_add_str(wire, payload, "model", CHIMERA_MODEL);
    if (executing) yyjson_mut_obj_add_val(wire, payload, "expected_plan", yyjson_val_mut_copy(wire, yyjson_obj_get(root, "expected_plan")));
    yyjson_mut_obj_add_val(wire, payload, "routing_policy", policy);
    yyjson_mut_obj_add_val(wire, payload, "orchestration_policy", planning);
    yyjson_mut_obj_remove_key(planning, "task"); yyjson_mut_obj_remove_key(planning, "max_output_tokens");
    yyjson_mut_obj_add_val(wire, payload, "max_tokens", yyjson_mut_val_mut_copy(wire, yyjson_mut_obj_get(plan, "max_output_tokens")));
    yyjson_mut_obj_add_str(wire, user, "role", "user");
    yyjson_mut_obj_add_val(wire, user, "content", yyjson_mut_val_mut_copy(wire, yyjson_mut_obj_get(plan, "task")));
    yyjson_mut_arr_append(messages, user); yyjson_mut_obj_add_val(wire, payload, "messages", messages);
    size_t wire_length = 0; encoded = yyjson_mut_write(wire, 0, &wire_length);
    if (!encoded || wire_length > INPUT_LIMIT + 8192) { code = "invalid_request"; message = "Planning request exceeds size limit"; goto done; }
    const char *token = getenv("CHIMERA_API_KEY");
    if (!token || !*token) token = getenv("DSCO_ROUTER_API_KEY");
    if (token && *token) {
        size_t token_length = strnlen(token, 4097);
        message = "Invalid Chimera authentication configuration";
        if (token_length > 4096) goto done;
        for (size_t i = 0; i < token_length; ++i) if ((unsigned char)token[i] < 33 || (unsigned char)token[i] > 126) goto done;
        char auth[4120]; snprintf(auth, sizeof(auth), "Authorization: Bearer %s", token);
        headers = curl_slist_append(NULL, auth);
        if (!headers) { code = "allocation_failed"; message = "Cannot allocate request headers"; goto done; }
    }
    struct curl_slist *next = curl_slist_append(headers, "Content-Type: application/json");
    if (!next) { code = "allocation_failed"; message = "Cannot allocate request headers"; goto done; } headers = next;
    next = curl_slist_append(headers, "Accept: application/json");
    if (!next) { code = "allocation_failed"; message = "Cannot allocate request headers"; goto done; } headers = next;
    easy = curl_easy_init(); body.data = malloc(BODY_LIMIT + 1);
    if (!easy || !body.data) { code = "allocation_failed"; message = "Cannot allocate Chimera transport"; goto done; }
    body.data[0] = '\0'; dsco_http_pool_apply(easy);
    if (curl_url_set(url, CURLUPART_PATH, executing ? "/v1/chat/completions" : "/v1/route", 0) != CURLUE_OK) goto done;
    code = "transport_error"; message = "Chimera decision request failed";
#define SETOPT(option, value) do { if (curl_easy_setopt(easy, option, value) != CURLE_OK) goto done; } while (0)
    SETOPT(CURLOPT_CURLU, url); SETOPT(CURLOPT_POST, 1L);
    SETOPT(CURLOPT_POSTFIELDS, encoded); SETOPT(CURLOPT_POSTFIELDSIZE, (long)wire_length);
    SETOPT(CURLOPT_HTTPHEADER, headers); SETOPT(CURLOPT_WRITEFUNCTION, receive); SETOPT(CURLOPT_WRITEDATA, &body);
    SETOPT(CURLOPT_CONNECTTIMEOUT_MS, 3000L); SETOPT(CURLOPT_TIMEOUT_MS, executing ? 55000L : 10000L); SETOPT(CURLOPT_NOSIGNAL, 1L);
    SETOPT(CURLOPT_SSL_VERIFYPEER, 1L); SETOPT(CURLOPT_SSL_VERIFYHOST, 2L); SETOPT(CURLOPT_FOLLOWLOCATION, 0L);
    SETOPT(CURLOPT_NETRC, (long)CURL_NETRC_IGNORED); SETOPT(CURLOPT_PROXY, ""); SETOPT(CURLOPT_PATH_AS_IS, 1L);
#if LIBCURL_VERSION_NUM >= 0x075500
    SETOPT(CURLOPT_PROTOCOLS_STR, "http,https");
#else
    SETOPT(CURLOPT_PROTOCOLS, (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS));
#endif
#undef SETOPT
    dispatched = executing;
    CURLcode transport = curl_easy_perform(easy); curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &status);
    if (body.too_large) { code = "response_too_large"; message = "Chimera response exceeds 128 KiB"; goto done; }
    if (transport != CURLE_OK) { code = transport == CURLE_OPERATION_TIMEDOUT ? "timeout" : "transport_error"; goto done; }
    response = yyjson_read(body.data, body.length, YYJSON_READ_BIGNUM_AS_RAW);
    yyjson_val *data = response ? yyjson_doc_get_root(response) : NULL;
    response_check integrity = validate_json(data, 0);
    if (status < 200 || status >= 300) {
        if (executing && integrity == RESPONSE_VALID && (status == 400 || status == 403 || status == 409)) {
            yyjson_val *server_code = yyjson_obj_get(yyjson_obj_get(data, "error"), "code");
            const char *known[] = {"invalid_execution_contract", "execution_profile_refused", "plan_changed",
                                  "execution_principal_required", "execution_already_submitted"};
            for (size_t i = 0; i < sizeof(known)/sizeof(*known); ++i) {
                if (text_is(server_code, known[i])) {
                    code = known[i]; message = "Router refused guarded execution before provider dispatch";
                    dispatched = false; goto done;
                }
            }
        }
        if (status == 400 && yyjson_is_obj(data) && integrity == RESPONSE_VALID)
            preserved_refusal = refusal(result, capacity, data, status);
        code = "http_error"; message = "Chimera returned an unsuccessful HTTP status"; goto done;
    }
    if (integrity == RESPONSE_PRECISION) { code = "precision_unsupported"; message = "Chimera response exceeds exact numeric precision"; goto done; }
    if (!yyjson_is_obj(data) || integrity != RESPONSE_VALID || !(executing ? completion_valid(data, yyjson_obj_get(root, "expected_plan")) : decision_valid(data, plan))) {
        code = "malformed_response"; message = "Chimera returned an invalid decision envelope"; goto done;
    }
    char observed[32]; time_t now = time(NULL); struct tm utc;
    if (now == (time_t)-1 || !gmtime_r(&now, &utc) || !strftime(observed, sizeof(observed), "%Y-%m-%dT%H:%M:%SZ", &utc)) {
        code = "clock_error"; message = "Cannot timestamp Chimera observation"; goto done;
    }
    output = yyjson_mut_doc_new(NULL); yyjson_mut_val *out = yyjson_mut_obj(output);
    if (!output || !out) { code = "allocation_failed"; message = "Cannot allocate decision receipt"; goto done; }
    yyjson_mut_doc_set_root(output, out); yyjson_mut_obj_add_bool(output, out, "ok", true);
    yyjson_mut_obj_add_str(output, out, "profile", executing ? "chimera_completion" : "chimera_plan"); yyjson_mut_obj_add_bool(output, out, "executed", executing);
    yyjson_mut_obj_add_str(output, out, "scope", "request"); yyjson_mut_obj_add_strcpy(output, out, "observed_at", observed);
    yyjson_mut_obj_add_val(output, out, "policy", yyjson_mut_val_mut_copy(output, policy));
    yyjson_mut_obj_add_val(output, out, "request", yyjson_mut_val_mut_copy(output, plan));
    yyjson_mut_obj_add_val(output, out, executing ? "completion" : "decision", yyjson_val_mut_copy(output, data));
    size_t output_length = 0; serialized = yyjson_mut_write(output, 0, &output_length);
    if (!serialized || output_length >= capacity) { code = "response_buffer_too_small"; message = "Complete Chimera receipt does not fit the result buffer"; goto done; }
    memcpy(result, serialized, output_length + 1); success = true;
done:
    yyjson_doc_free(response); yyjson_doc_free(request); yyjson_mut_doc_free(output); yyjson_mut_doc_free(wire);
    curl_slist_free_all(headers); curl_easy_cleanup(easy); curl_url_cleanup(url);
    free(body.data); free(encoded); free(serialized);
    if (executing && dispatched && !success && !preserved_refusal) {
        code = "outcome_unknown"; message = "Execution may have reached the provider; inspect its execution ID and do not retry";
    }
    return success ? true : preserved_refusal ? false : fail(result, capacity, code, message, status);
}

/* Separate registry entry: planning never dispatches inference. */
bool lingo_chimera_execute(const char *input, char *result, size_t capacity) {
    return chimera_request(input, result, capacity, false);
}
bool lingo_chimera_complete(const char *input, char *result, size_t capacity) {
    return chimera_request(input, result, capacity, true);
}
