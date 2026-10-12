#ifndef DSCO_EXECUTION_CONTRACT_H
#define DSCO_EXECUTION_CONTRACT_H

#include "llm.h"

typedef struct { unsigned checks; unsigned repairs; } execution_contract_t;
typedef enum { EXECUTION_CONTRACT_PASS, EXECUTION_CONTRACT_RETRY,
               EXECUTION_CONTRACT_BLOCK } execution_contract_decision_t;

/* Run the operator's acceptance command in the SAME capability/taint session.
 * Only ordinary command failures may trigger bounded model repair. */
execution_contract_decision_t execution_contract_check(
    execution_contract_t *state, const char *command, const char *tier,
    conversation_t *conversation, char *diagnostic, size_t diagnostic_size);

#endif
