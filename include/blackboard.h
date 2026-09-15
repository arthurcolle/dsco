#ifndef DSCO_BLACKBOARD_H
#define DSCO_BLACKBOARD_H

#include <stdbool.h>
#include <stddef.h>

/* Shared, same-host coordination. Every public invocation goes through the
 * normal tool gate; verification also dispatches bash through that gate. */
extern const char blackboard_tool_schema[];
bool blackboard_execute(const char *input, char *result, size_t result_len);

#endif
