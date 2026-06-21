#ifndef DSCO_PROVIDER_H
#define DSCO_PROVIDER_H

#include <stdbool.h>
#include <stddef.h>
#include "llm.h"
#include "json_util.h"

/* Provider abstraction for multi-model support.
 *
 * Currently implements:
 *   - Anthropic Claude (Messages API)
 *   - OpenAI-compatible (Chat Completions API)
 *
 * Usage:
 *   provider_t *p = provider_create("anthropic");
 *   char *req = p->build_request(p, &conv, &session, max_tokens);
 *   stream_result_t sr = p->stream(p, api_key, req, ...);
 */

typedef struct provider provider_t;

struct provider {
    const char *name;
    const char *api_url;

    /* Build API request JSON from conversation + session state */
    char *(*build_request)(provider_t *p, conversation_t *conv,
                            session_state_t *session, int max_tokens,
                            const char *credential);

    /* Build HTTP headers for API request */
    struct curl_slist *(*build_headers)(provider_t *p, const char *api_key);

    /* Stream API call with callbacks */
    stream_result_t (*stream)(provider_t *p, const char *api_key,
                               const char *request_json,
                               stream_text_cb text_cb,
                               stream_tool_start_cb tool_cb,
                               stream_thinking_cb thinking_cb,
                               void *cb_ctx);

    /* Provider-specific data */
    void *data;
};

/* Create a provider by name. Returns NULL if unknown. */
provider_t *provider_create(const char *name);

/* Free a provider */
void provider_free(provider_t *p);

/* Prepare reusable provider transport state. OpenAI-compatible providers keep
 * a persistent CURL easy handle so DNS/TCP/TLS/HTTP2 state can be reused across
 * turns. Providers without native reuse support return true as a no-op. */
bool provider_prepare(provider_t *p);

/* Stream using prepared transport state when available. Falls back to the
 * provider's legacy stream callback when no reusable transport exists. */
stream_result_t provider_stream_reuse(provider_t *p, const char *api_key,
                                      const char *request_json,
                                      stream_text_cb text_cb,
                                      stream_tool_start_cb tool_cb,
                                      stream_thinking_cb thinking_cb,
                                      void *cb_ctx);

/* Drop reusable transport state and force the next call to reconnect. */
void provider_reset_connection(provider_t *p);

/* Detect provider from model name or API key pattern */
const char *provider_detect(const char *model, const char *api_key);

/* Detect the underlying model family (anthropic/openai/xai/google/...) even
 * when the request will ultimately route through OpenRouter. */
const char *provider_model_family(const char *model);

/* Resolve the API key env var for a provider */
const char *provider_resolve_api_key(const char *provider_name);

/* Whether a provider exposes a custom API base override like FOO_API_BASE. */
bool provider_has_custom_api_base(const char *provider_name);

/* Whether a provider can be used with its env key or the current session key */
bool provider_has_usable_key(const char *provider_name, const char *fallback_api_key);

/* Select the provider that should service a model for the current session */
const char *provider_route_for_model(const char *model,
                                     const char *fallback_api_key,
                                     const char *provider_override);

/* Resolve the actual request key for an already-selected provider */
const char *provider_resolve_request_api_key(const char *provider_name,
                                             const char *fallback_api_key);

/* Export the resolved credential into child-process env vars so spawned
 * workers keep the parent's provider/auth mode. */
void provider_export_child_process_credentials_for_provider(const char *provider_name,
                                                            const char *resolved_key);
void provider_export_child_process_credentials(const char *model,
                                               const char *resolved_key);

/* Auth-mode debugging helpers */
bool provider_debug_auth_enabled(void);
const char *provider_auth_mode(const char *provider_name, const char *resolved_key);
void provider_debug_log_request(const char *provider_name, const char *model,
                                const char *resolved_key);
const char *provider_claude_code_oauth_source(void);

/* Fill subscription_type_out and rate_limit_tier_out (both at least 64 bytes).
 * Returns true when at least one field was found. */
bool provider_claude_code_get_account_info(char *subscription_type_out, size_t st_len,
                                           char *rate_limit_tier_out,   size_t rl_len);

/* Classify an error body/message as a credit/billing failure. Shared across
 * Anthropic and OpenAI-compat streaming paths so both can mark the stream
 * result as "credit_too_low" and trigger the fallback chain. */
bool provider_msg_is_credit_too_low(const char *msg);

/* Classify a provider error body as a context/prompt-length rejection
 * ("prompt is too long", "context_length_exceeded", "maximum context length",
 * etc.) so the agent loop can react with reactive compaction + retry instead
 * of ending the turn. Shared by the Anthropic and OpenAI-compat paths. */
bool provider_msg_is_context_overflow(const char *msg);

/* True when a model honors Anthropic `cache_control` breakpoints forwarded by
 * OpenRouter (Claude models). The OpenAI-compat request builder marks the
 * system block ephemeral for these so the static tools+system prefix is cached;
 * other providers ignore/reject the marker. DSCO_OR_CACHE=0/1 forces off/on. */
bool provider_model_supports_cache_control(const char *model);

/* Check whether a model is routable, returning the routed provider when asked */
bool provider_model_is_routable(const char *model,
                                const char *fallback_api_key,
                                const char *provider_override,
                                const char **out_provider_name);

/* Build a default cross-lab fallback chain for a primary model. Returns the
 * number of models written into out_models. */
int provider_build_default_fallback_models(const char *model,
                                           char out_models[][128],
                                           int max_models);

/* Pick a sensible default primary model for dsco-native execution. When
 * prefer_code is true, bias toward coding-oriented families; otherwise bias
 * toward cost-effective frontier models, currently favoring Grok. */
const char *provider_select_default_primary_model(bool prefer_code);

#endif
