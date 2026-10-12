#ifndef DSCO_WORKER_RUNTIME_H
#define DSCO_WORKER_RUNTIME_H
#include "llm.h"
/* Cooperative wait telemetry only; never changes admission or consumes messages. */
int worker_runtime_turn_ceiling(int backstop);
void worker_runtime_wait(const char *phase);
/* Apply addressed mailbox messages at a model boundary, not mid-response. */
void worker_runtime_messages(conversation_t *conv);
#endif
