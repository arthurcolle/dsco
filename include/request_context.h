#ifndef DSCO_REQUEST_CONTEXT_H
#define DSCO_REQUEST_CONTEXT_H
#include "provider.h"
/* Shared interactive/headless admission: archive verified completed exchanges
 * through the governed tool gate before projecting or rejecting a request. */
char *request_context_build(provider_t *provider, conversation_t *conv,
                            session_state_t *session, const char *key, int max_tokens);
bool request_context_archive(conversation_t *conv, session_state_t *session, int keep);
#endif
