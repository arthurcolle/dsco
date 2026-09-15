#ifndef DSCO_TOOL_EFFECTS_H
#define DSCO_TOOL_EFFECTS_H

#include <stdbool.h>

/* HTTP method semantics for scheduling/caching. Malformed or ambiguous input
 * and every method other than an omitted method, GET, or HEAD are mutating.
 * A supplied body is conservatively treated as potentially mutating too. */
bool tool_http_request_is_read_only(const char *input_json);
bool tools_call_is_read_only(const char *name, const char *input_json);

#endif
