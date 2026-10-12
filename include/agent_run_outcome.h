#ifndef DSCO_AGENT_RUN_OUTCOME_H
#define DSCO_AGENT_RUN_OUTCOME_H
#include "process_capture.h"
/* Observed vendor protocol state, never semantic acceptance or route proof. */
typedef enum {
    AGENT_RUN_UNSUPPORTED, AGENT_RUN_UNKNOWN, AGENT_RUN_COMPLETED, AGENT_RUN_FAILED
} agent_run_state_t;
typedef struct {
    agent_run_state_t state;
    bool stream_invalid;
    size_t failure_offset, failure_length; /* first failure's raw captured JSONL */
} agent_run_outcome_t;
agent_run_outcome_t agent_run_outcome(const char *adapter,
                                     const process_capture_t *capture);
const char *agent_run_state_name(agent_run_state_t state);
#endif
