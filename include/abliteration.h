#ifndef DSCO_ABLITERATION_H
#define DSCO_ABLITERATION_H

#include <stdbool.h>
#include <stddef.h>
#include <curl/curl.h>

/* Provider-specific contract for https://api.abliteration.ai.  Keep the
 * OpenAI-compatible transport generic; this module owns the provider's model,
 * endpoint, cache, reasoning, policy, and token-counting rules. */
typedef enum {
    ABLITERATION_API_CHAT = 0,
    ABLITERATION_API_RESPONSES,
    ABLITERATION_API_ANTHROPIC,
} abliteration_api_mode_t;

bool abliteration_is_provider(const char *provider_name);
bool abliteration_is_model(const char *model);
bool abliteration_model_is_base(const char *model);
bool abliteration_model_supports_vision(const char *model);
bool abliteration_model_supports_video(const char *model);

abliteration_api_mode_t abliteration_api_mode(void);
const char *abliteration_api_mode_name(abliteration_api_mode_t mode);
const char *abliteration_endpoint(abliteration_api_mode_t mode, bool policy_gateway);
bool abliteration_policy_gateway_enabled(void);

/* Chat Completions accepts the complete documented effort ladder.  Large V2
 * aliases collapse server-side to low/high/max, but preserving the requested
 * accepted value is important for API compatibility and observability. */
const char *abliteration_normalize_chat_effort(const char *model, const char *effort,
                                               char *out, size_t out_len);

/* Provider-specific params take precedence over the generic OpenAI-compatible
 * override so an Abliteration lane can be configured without changing other
 * providers in the same process. */
const char *abliteration_params_json(void);
bool abliteration_chat_param_supported(const char *key);

/* Add Policy Gateway attribution headers when configured. */
struct curl_slist *abliteration_append_policy_headers(struct curl_slist *headers);

/* Exact Anthropic-compatible count-tokens endpoint. Returns -1 on transport,
 * authentication, validation, or parse failure. */
int abliteration_count_tokens(const char *api_key, const char *request_json);

#endif /* DSCO_ABLITERATION_H */
