#ifndef DSCO_GOAL_NATIVE_H
#define DSCO_GOAL_NATIVE_H
#include "llm.h"
#include <stddef.h>
#include <stdbool.h>

/* Read-only snapshot of the agent-owned goal controller for existing terminal
 * and explicitly opened native session details. Never leases or mutates work. */
bool goal_native_format(const session_state_t *session, char *out, size_t cap);
#endif
