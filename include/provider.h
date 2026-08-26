#ifndef DSCO_PROVIDER_H
#define DSCO_PROVIDER_H

#include <stdbool.h>
#include <stddef.h>
#include <time.h>
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
                               stream_tool_arg_delta_cb tool_delta_cb,
                               stream_thinking_cb thinking_cb,
                               void *cb_ctx);

    /* Provider-specific data */
    void *data;

    /* Reusable transport owned by providers whose wire implementation is not
     * backed by openai_data_t (currently the native ChatGPT Responses lane). */
    CURL *transport_curl;
};

/* Create a provider by name. Returns NULL if unknown. */
provider_t *provider_create(const char *name);

/* Free a provider */
void provider_free(provider_t *p);

/* Prepare reusable provider transport state. OpenAI-compatible and native
 * ChatGPT providers keep a persistent CURL easy handle so the connection
 * established by the first request can be reused across turns. Providers
 * without native reuse support return true as a no-op. */
bool provider_prepare(provider_t *p);

/* Stream using prepared transport state when available. Falls back to the
 * provider's legacy stream callback when no reusable transport exists. */
stream_result_t provider_stream_reuse(provider_t *p, const char *api_key,
                                      const char *request_json,
                                      stream_text_cb text_cb,
                                      stream_tool_start_cb tool_cb,
                                      stream_tool_arg_delta_cb tool_delta_cb,
                                      stream_thinking_cb thinking_cb,
                                      void *cb_ctx);

/* Drop reusable transport state and force the next call to reconnect. */
void provider_reset_connection(provider_t *p);

/* Detect provider from model name or API key pattern */
const char *provider_detect(const char *model, const char *api_key);

/* Detect the underlying model family (anthropic/openai/xai/google/...) even
 * when the request will ultimately route through OpenRouter. */
const char *provider_model_family(const char *model);
bool provider_model_supports_cache_control(const char *model);
bool provider_model_supports_automatic_prompt_cache(const char *model);
bool provider_model_supports_prompt_cache_key(const char *model);
bool provider_model_supports_prompt_cache_retention(const char *model);

/* Resolve the API key env var for a provider */
const char *provider_resolve_api_key(const char *provider_name);

/* Whether a provider exposes a custom API base override like FOO_API_BASE. */
bool provider_has_custom_api_base(const char *provider_name);

/* Whether a provider can be used with its env key or the current session key */
bool provider_has_usable_key(const char *provider_name, const char *fallback_api_key);

/* Whether a provider name is a local/self-hosted loopback endpoint. */
bool provider_is_local_endpoint(const char *provider_name);

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
/* True when the resolved auth class is covered by a subscription/coding plan
 * rather than billed as ordinary metered API usage. */
bool provider_usage_is_included(const char *provider_name, const char *resolved_key);
/* Account-local scheduler wait observed by the most recent stream on this
 * thread (currently the cross-process ChatGPT subscription gate). */
long provider_last_subscription_queue_ms(void);
void provider_debug_log_request(const char *provider_name, const char *model,
                                const char *resolved_key);
const char *provider_claude_code_oauth_source(void);
/* Explicitly import the user's Claude Code OAuth bundle into DSCO's 0600 cache.
 * This may invoke the OS credential UI and is therefore only called by login. */
bool provider_claude_code_import_credentials(void);
/* Claude Code subscription requests bind the bearer to the local Claude
 * account/device/session identity via metadata.user_id. */
const char *provider_claude_code_metadata_user_id(void);
const char *provider_claude_code_session_id(void);

/* Sakana/Fugu supports both flat-rate subscription keys and metered PAYG keys.
 * The subscription key remains the default when both are present; set
 * DSCO_SAKANA_KEY_CLASS=payg or DSCO_FUGU_KEY_CLASS=payg for explicit PAYG. */
bool provider_sakana_current_key_is_subscription(void);
const char *provider_sakana_subscription_request_key(void);
bool provider_sakana_has_payg_key(void);
const char *provider_sakana_payg_request_key(void);

/* Fill subscription_type_out and rate_limit_tier_out (both at least 64 bytes).
 * Returns true when at least one field was found. */
bool provider_claude_code_get_account_info(char *subscription_type_out, size_t st_len,
                                           char *rate_limit_tier_out,   size_t rl_len);

/* Classify an error body/message as a credit/billing failure. Shared across
 * Anthropic and OpenAI-compat streaming paths so both can mark the stream
 * result as "credit_too_low" and trigger the fallback chain. */
bool provider_msg_is_credit_too_low(const char *msg);

/* ChatGPT 429s use structured error type/code fields to distinguish exhausted
 * subscription allocation from a transient edge throttle. */
bool provider_chatgpt_429_is_quota(const char *error_json_or_message);

/* Provider-aware credit classification. In particular, an openai-codex 429
 * is transient unless the native stream marked it with credit_too_low. */
bool provider_stream_result_is_credit_exhausted(const char *provider_name,
                                                const stream_result_t *result);

/* Classify an error as a policy/gating rejection (HTTP 403, model not
 * available, access not granted, regulatory gating). Distinct from
 * credit_too_low: gating is not fixed by paying, so callers should route to
 * a different model rather than retry or top up credit. */
bool provider_msg_is_gated(const char *msg);
time_t provider_credit_reset_at_from_value(const char *value, time_t now);
time_t provider_credit_reset_at_from_text(const char *text, time_t now);
bool provider_credit_reset_at_from_header_line(const char *line, time_t now,
                                               time_t *reset_at);

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

/* Build a default cross-lab fallback chain for a primary model. When
 * DSCO_LOCAL_FALLBACK_MODEL names a local provider, reserve the final slot for
 * that always-available lane. Returns the number of models written. */
int provider_build_default_fallback_models(const char *model,
                                           char out_models[][128],
                                           int max_models);

/* Pick a sensible default primary model for dsco-native execution. When
 * prefer_code is true, bias toward coding-oriented families; otherwise bias
 * toward cost-effective frontier models, currently favoring Grok. */
const char *provider_select_default_primary_model(bool prefer_code);

/* Map a raw API key to its native provider purely from its prefix
 * (e.g. "fish_" -> sakana, "sk-ant-" -> anthropic, "xai-" -> xai). Unlike
 * provider_detect(), this never falls back to a default: an unrecognized
 * prefix returns NULL. */
const char *provider_provider_for_api_key(const char *api_key);

/* Publish a raw key under its provider's canonical env var (only if that var
 * is not already set) so every downstream credential lookup recognizes it.
 * Returns the detected provider name, or NULL if the prefix is unknown. */
const char *provider_publish_api_key_env(const char *api_key);

/* Public wrapper: the primary model id for a given provider family, honoring
 * which credentials are actually available. Returns NULL if the family has no
 * usable route. */
const char *provider_primary_model_for(const char *family, bool prefer_code);

#ifdef DSCO_INTERNAL_TESTS
typedef struct {
    parsed_response_t parsed;
    char *reasoning_stream;
    char *tool_arg_delta_stream;
    bool done;
    bool terminal_success;
    usage_t usage;
    int reasoning_tokens;
    int cached_tokens;
} provider_test_openai_sse_result_t;

/* Feed raw OpenAI-compatible SSE bytes through the production line parser.
 * Every allocation in the result is released by
 * provider_test_free_openai_sse_result(). */
bool provider_test_parse_openai_sse(const char *bytes, size_t len,
                                    provider_test_openai_sse_result_t *out);
bool provider_test_parse_openai_sse_for_model(const char *bytes, size_t len,
                                              const char *source_provider,
                                              const char *request_model,
                                              provider_test_openai_sse_result_t *out);
void provider_test_free_openai_sse_result(provider_test_openai_sse_result_t *result);
long provider_test_chatgpt_retry_after_ms(const char *text);
#endif

#endif
