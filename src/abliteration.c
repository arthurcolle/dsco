#include "abliteration.h"

#include "http_pool.h"
#include "json_util.h"
#include "provider_profiles.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define ABLITERATION_ORIGIN "https://api.abliteration.ai"
#define ABLITERATION_V1_BASE ABLITERATION_ORIGIN "/v1"

static bool ablit_truthy(const char *value) {
    return value && value[0] && strcmp(value, "0") != 0 &&
           strcasecmp(value, "false") != 0 && strcasecmp(value, "no") != 0 &&
           strcasecmp(value, "off") != 0;
}

bool abliteration_is_provider(const char *provider_name) {
    if (!provider_name || !provider_name[0])
        return false;
    const char *canonical = provider_profile_canonical_name(provider_name);
    return canonical && strcmp(canonical, "abliteration-ai") == 0;
}

static const char *ablit_bare_model(const char *model) {
    if (!model)
        return "";
    static const char *const prefixes[] = {
        "abliteration-ai/", "abliteration/", "ablit/", NULL,
    };
    for (int i = 0; prefixes[i]; i++) {
        size_t n = strlen(prefixes[i]);
        if (strncmp(model, prefixes[i], n) == 0)
            return model + n;
    }
    return model;
}

bool abliteration_is_model(const char *model) {
    const char *bare = ablit_bare_model(model);
    return strcmp(bare, "abliterated-model") == 0 ||
           strcmp(bare, "abliterated-model-large-v2") == 0 ||
           strcmp(bare, "abliterated-model-large") == 0;
}

bool abliteration_model_is_base(const char *model) {
    return strcmp(ablit_bare_model(model), "abliterated-model") == 0;
}

bool abliteration_model_supports_vision(const char *model) {
    return abliteration_model_is_base(model);
}

bool abliteration_model_supports_video(const char *model) {
    return abliteration_model_is_base(model);
}

abliteration_api_mode_t abliteration_api_mode(void) {
    const char *mode = getenv("DSCO_ABLITERATION_API");
    if (!mode || !mode[0] || strcasecmp(mode, "chat") == 0 ||
        strcasecmp(mode, "chat-completions") == 0 ||
        strcasecmp(mode, "openai-chat") == 0)
        return ABLITERATION_API_CHAT;
    if (strcasecmp(mode, "responses") == 0 || strcasecmp(mode, "response") == 0)
        return ABLITERATION_API_RESPONSES;
    if (strcasecmp(mode, "anthropic") == 0 || strcasecmp(mode, "messages") == 0)
        return ABLITERATION_API_ANTHROPIC;
    return ABLITERATION_API_CHAT;
}

const char *abliteration_api_mode_name(abliteration_api_mode_t mode) {
    switch (mode) {
        case ABLITERATION_API_RESPONSES: return "responses";
        case ABLITERATION_API_ANTHROPIC: return "anthropic";
        case ABLITERATION_API_CHAT:
        default: return "chat";
    }
}

bool abliteration_policy_gateway_enabled(void) {
    return ablit_truthy(getenv("DSCO_ABLITERATION_POLICY_GATEWAY"));
}

const char *abliteration_endpoint(abliteration_api_mode_t mode, bool policy_gateway) {
    if (policy_gateway) {
        switch (mode) {
            case ABLITERATION_API_RESPONSES:
                return ABLITERATION_ORIGIN "/policy/responses";
            case ABLITERATION_API_ANTHROPIC:
                return ABLITERATION_ORIGIN "/policy/messages";
            case ABLITERATION_API_CHAT:
            default:
                return ABLITERATION_ORIGIN "/policy/chat/completions";
        }
    }
    switch (mode) {
        case ABLITERATION_API_RESPONSES:
            return ABLITERATION_V1_BASE "/responses";
        case ABLITERATION_API_ANTHROPIC:
            return ABLITERATION_V1_BASE "/messages";
        case ABLITERATION_API_CHAT:
        default:
            return ABLITERATION_V1_BASE "/chat/completions";
    }
}

const char *abliteration_normalize_chat_effort(const char *model, const char *effort,
                                               char *out, size_t out_len) {
    (void)model;
    if (!out || out_len == 0)
        return NULL;
    const char *value = effort && effort[0] ? effort : "";
    static const char *const accepted[] = {
        "none", "minimal", "low", "medium", "high", "xhigh", "max", "ultracode", NULL,
    };
    for (int i = 0; accepted[i]; i++) {
        if (strcmp(value, accepted[i]) == 0) {
            snprintf(out, out_len, "%s", value);
            return out;
        }
    }
    out[0] = '\0';
    return out;
}

bool abliteration_chat_param_supported(const char *key) {
    if (!key || !key[0])
        return false;
    /* ChatCompletionRequest.additionalProperties is false in the provider's
     * live OpenAPI schema. Keep this list exact so generic OpenAI options do
     * not turn into provider-side 422s. */
    static const char *const keys[] = {
        "audio", "cache_salt", "flagged_categories", "frequency_penalty",
        "function_call", "functions", "include_reasoning", "logit_bias", "logprobs",
        "metadata", "modalities", "n", "parallel_tool_calls", "prediction",
        "presence_penalty", "prompt_cache_key", "prompt_cache_retention", "reasoning",
        "reasoning_effort", "response_format", "safety_identifier", "seed", "service_tier",
        "stop", "store", "stream_options", "temperature", "thinking", "tool_choice",
        "tools", "top_logprobs", "top_p", "user", "verbosity", "web_search_options",
        NULL,
    };
    for (int i = 0; keys[i]; i++)
        if (strcmp(key, keys[i]) == 0)
            return true;
    return false;
}

const char *abliteration_params_json(void) {
    const char *raw = getenv("DSCO_ABLITERATION_PARAMS");
    if (!raw || !raw[0])
        return NULL;
    while (*raw && isspace((unsigned char)*raw))
        raw++;
    return *raw == '{' ? raw : NULL;
}

static struct curl_slist *ablit_append_env_header(struct curl_slist *headers,
                                                   const char *env_name,
                                                   const char *header_name) {
    const char *value = getenv(env_name);
    if (!value || !value[0])
        return headers;
    char header[1024];
    snprintf(header, sizeof(header), "%s: %s", header_name, value);
    return curl_slist_append(headers, header);
}

struct curl_slist *abliteration_append_policy_headers(struct curl_slist *headers) {
    headers = ablit_append_env_header(headers, "DSCO_ABLITERATION_POLICY_PROJECT",
                                      "X-Policy-Project");
    headers = ablit_append_env_header(headers, "DSCO_ABLITERATION_POLICY_TARGET",
                                      "X-Policy-Target");
    headers = ablit_append_env_header(headers, "DSCO_ABLITERATION_POLICY_USER",
                                      "X-Policy-User");
    return headers;
}

typedef struct {
    jbuf_t body;
} ablit_count_state_t;

static size_t ablit_count_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    size_t total = size * nmemb;
    ablit_count_state_t *state = (ablit_count_state_t *)userdata;
    if (!state)
        return 0;
    jbuf_append_len(&state->body, (const char *)ptr, total);
    return total;
}

int abliteration_count_tokens(const char *api_key, const char *request_json) {
    if (!api_key || !api_key[0] || !request_json || !request_json[0])
        return -1;
    CURL *curl = curl_easy_init();
    if (!curl)
        return -1;
    dsco_http_pool_apply(curl);

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "anthropic-version: 2023-06-01");
    char auth[9216];
    snprintf(auth, sizeof(auth), "Authorization: Bearer %s", api_key);
    headers = curl_slist_append(headers, auth);

    ablit_count_state_t state;
    jbuf_init(&state.body, 1024);
    curl_easy_setopt(curl, CURLOPT_URL, ABLITERATION_V1_BASE "/messages/count_tokens");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request_json);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, ablit_count_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &state);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2TLS);

    CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    int tokens = -1;
    if (rc == CURLE_OK && status == 200 && state.body.data)
        tokens = json_get_int(state.body.data, "input_tokens", -1);
    jbuf_free(&state.body);
    return tokens;
}
