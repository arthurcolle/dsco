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
                            session_state_t *session, int max_tokens);

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

/* Detect provider from model name or API key pattern */
const char *provider_detect(const char *model, const char *api_key);

/* Resolve the API key env var for a provider */
const char *provider_resolve_api_key(const char *provider_name);

#endif
