#ifndef DSCO_REQUEST_INTENT_H
#define DSCO_REQUEST_INTENT_H

#include <stdbool.h>

/* True only when the prompt contains everything needed for a direct model
 * answer. False is the safe default: callers retain tools and agentic state. */
bool request_intent_prefers_direct(const char *prompt);

#endif
