#ifndef DSCO_EXECUTION_KERNEL_H
#define DSCO_EXECUTION_KERNEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define EXECUTION_KERNEL_ID_LEN 37
#define EXECUTION_KERNEL_HASH_LEN 65

typedef enum {
    EXECUTION_ATTEMPT_PROPOSED = 0,
    EXECUTION_ATTEMPT_ADMITTED,
    EXECUTION_ATTEMPT_STARTED,
    EXECUTION_ATTEMPT_DENIED,
    EXECUTION_ATTEMPT_SUCCEEDED,
    EXECUTION_ATTEMPT_FAILED,
    EXECUTION_ATTEMPT_CANCELLED,
    EXECUTION_ATTEMPT_EFFECT_UNKNOWN,
} execution_attempt_status_t;

/* One invocation of the production tool gate. Raw arguments and results never
 * enter this record; their hashes provide correlation without duplicating
 * potentially secret user data into another storage surface. */
typedef struct {
    char execution_id[EXECUTION_KERNEL_ID_LEN];
    char parent_execution_id[EXECUTION_KERNEL_ID_LEN];
    char previous_execution_id[EXECUTION_KERNEL_ID_LEN];
    char session_id[EXECUTION_KERNEL_ID_LEN];
    char trace_id[EXECUTION_KERNEL_ID_LEN];
    char turn_id[EXECUTION_KERNEL_ID_LEN];
    char tool_call_id[EXECUTION_KERNEL_ID_LEN];
    char tool_name[128];
    char dispatch_tool_name[128];
    char tier[32];
    char principal[64];
    char provider[64];
    char model[128];
    char executor[64];
    char effect[32];
    char capabilities[128];
    char input_sha256[EXECUTION_KERNEL_HASH_LEN];
    char dispatch_input_sha256[EXECUTION_KERNEL_HASH_LEN];
    char result_sha256[EXECUTION_KERNEL_HASH_LEN];
    uint64_t sequence;
    unsigned capability_mask;
    execution_attempt_status_t status;
    double proposed_at_ms;
    double started_at_ms;
    double finished_at_ms;
    double elapsed_ms;
    size_t result_bytes;
    bool effectful;
    bool admitted;
    bool started;
    bool terminal;
} execution_attempt_t;

const char *execution_attempt_status_name(execution_attempt_status_t status);

/* Begin before any capability/governance decision. Context IDs, provider and
 * model may be NULL when no model route invoked the tool. The executor records
 * the leaf runtime (dsco-native); principal comes from the process envelope. */
void execution_kernel_begin(execution_attempt_t *attempt, const char *tool_name,
                            const char *input_json, const char *tier, unsigned capability_mask,
                            const char *capabilities, const char *provider, const char *model,
                            const char *trace_id, const char *turn_id, const char *tool_call_id);

/* Admission means policy checks passed. Started is emitted immediately before
 * the leaf call, after alias/routing and input normalization. */
void execution_kernel_admit(execution_attempt_t *attempt);
void execution_kernel_start(execution_attempt_t *attempt, const char *dispatch_tool_name,
                            const char *dispatch_input_json);

/* A false result before admission is a denial; after admission it is a failed
 * execution. This observer never changes the tool's return value. */
void execution_kernel_finish(execution_attempt_t *attempt, bool ok, const char *result);

/* Bounded, concurrent in-process receipt queries. */
bool execution_kernel_get(const char *execution_id, execution_attempt_t *out);
bool execution_kernel_latest(execution_attempt_t *out);
bool execution_kernel_recent_json(char *out, size_t out_len, int limit);
void execution_kernel_reset_for_test(void);

#endif
