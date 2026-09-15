#ifndef DSCO_TOOL_GROUNDING_H
#define DSCO_TOOL_GROUNDING_H

#include "json_util.h"

/* Append a fresh, bounded runtime inventory to a request's system prompt.
 * Accepts a tools array, a {"tools":[...]} object, or the serializer fragment
 * ,"tools":[...] (possibly followed by other fields). Uses the exact wire
 * names; does not retrieve tools, mutate residency, execute, or grant access.
 * Empty/invalid input appends nothing. Callers omit this for tool_choice=none.
 * Rebuild per request rather than persisting it in conversation history. */
void tool_grounding_append(jbuf_t *prompt, const char *tools_json);

#endif
