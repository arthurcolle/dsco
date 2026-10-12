#ifndef DSCO_PROVIDER_RESPONSES_H
#define DSCO_PROVIDER_RESPONSES_H

#include "provider.h"
#include "provider_profiles.h"

/* Public Responses gateways have no ChatGPT account, OAuth or Codex model
 * defaults. They share only DSCO's conversation/tool serialization and SSE
 * parser. Execution of returned tools remains in the normal capability gate. */
provider_t *provider_public_responses_create(const provider_profile_t *profile);
stream_result_t provider_public_responses_stream(
    provider_t *p, const char *api_key, const char *request_json, stream_text_cb text_cb,
    stream_tool_start_cb tool_cb, stream_tool_arg_delta_cb tool_delta_cb,
    stream_thinking_cb thinking_cb, void *cb_ctx);

/* Internal serialization/transport hooks owned by provider.c. */
bool provider_responses_append_context(jbuf_t *b, conversation_t *conv,
                                      session_state_t *session);
stream_result_t provider_responses_stream_once(
    provider_t *p, const char *api_key, const char *request_json, stream_text_cb text_cb,
    stream_tool_start_cb tool_cb, stream_tool_arg_delta_cb tool_delta_cb,
    stream_thinking_cb thinking_cb, void *cb_ctx);

#endif
