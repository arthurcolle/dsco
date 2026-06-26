#ifndef DSCO_EXECUTION_LAYER_H
#define DSCO_EXECUTION_LAYER_H

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    EXEC_EFFECT_READ = 0,
    EXEC_EFFECT_WRITE_FILE,
    EXEC_EFFECT_SHELL,
    EXEC_EFFECT_NETWORK,
    EXEC_EFFECT_PROCESS,
    EXEC_EFFECT_EXTERNAL_WRITE,
    EXEC_EFFECT_RUNTIME_CONTROL,
    EXEC_EFFECT_UNKNOWN,
} execution_effect_t;

typedef enum {
    EXEC_STATUS_PENDING = 0,
    EXEC_STATUS_DENIED,
    EXEC_STATUS_EXECUTED,
    EXEC_STATUS_FAILED,
} execution_status_t;

typedef bool (*execution_leaf_fn)(const char *name, const char *input_json,
                                  const char *tier, char *result,
                                  size_t result_len);

typedef struct {
    char principal[64];
    char tier[32];
    char tool_name[128];
    const char *input_json;
    execution_effect_t effect;
    double estimated_gsu;
    bool requires_confirmation;
    bool has_rollback;
    char rollback_hint[256];
    execution_leaf_fn execute;
} execution_intent_t;

typedef struct {
    char execution_id[37];
    char tool_name[128];
    char tier[32];
    execution_effect_t effect;
    execution_status_t status;
    bool allowed;
    bool executed;
    bool verified;
    bool rolled_back;
    char denied_pass[32];
    char denial_reason[256];
    double charged_gsu;
    double elapsed_ms;
} execution_receipt_t;

const char *execution_effect_name(execution_effect_t effect);
const char *execution_status_name(execution_status_t status);

bool execution_submit(const execution_intent_t *intent,
                      execution_receipt_t *receipt,
                      char *result,
                      size_t result_len);

bool execution_last_receipt_json(char *out, size_t out_len);

#endif /* DSCO_EXECUTION_LAYER_H */
