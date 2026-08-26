#ifndef DSCO_TOOL_CALL_NORMALIZER_H
#define DSCO_TOOL_CALL_NORMALIZER_H

#include <stdbool.h>
#include <stddef.h>

#define TOOL_CALL_NORMALIZER_MAX_CALLS 64
#define TOOL_CALL_NORMALIZER_NAME_MAX 128

typedef struct {
    char name[TOOL_CALL_NORMALIZER_NAME_MAX];
    char *arguments; /* malloc'd canonical JSON object */
} normalized_tool_call_t;

typedef struct {
    normalized_tool_call_t calls[TOOL_CALL_NORMALIZER_MAX_CALLS];
    size_t count;
    bool truncated;
    char error[192];
} normalized_tool_calls_t;

/* Normalize common local-model encodings:
 *   OpenAI tool_calls JSON, {name,arguments} JSON arrays/objects,
 *   <tools> JSON streams, and <tool_name key="value"/> shorthand.
 * Parsing is bounded, rejects malformed arguments, and never executes tools. */
bool tool_calls_normalize(const char *text, normalized_tool_calls_t *out);
void tool_calls_normalized_free(normalized_tool_calls_t *out);

#endif
