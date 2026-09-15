#ifndef DSCO_EXECUTION_EVENTS_H
#define DSCO_EXECUTION_EVENTS_H
#include "execution_kernel.h"

/* Scoped attribution for nested, synchronous tool dispatch and gate stages. */
const execution_attempt_t *execution_events_enter(const execution_attempt_t *attempt);
void execution_events_leave(const execution_attempt_t *previous);
bool execution_events_record(const char *event, const execution_attempt_t *attempt,
                             const char *detail);
void execution_events_stage(const char *stage, double elapsed_ms,
                            bool would_deny, bool enforced);
#endif
