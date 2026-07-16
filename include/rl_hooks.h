#ifndef DSCO_RL_HOOKS_H
#define DSCO_RL_HOOKS_H

#include <stdbool.h>
#include <stddef.h>

/*
 * Local trajectory instrumentation foundation.
 *
 * This module does not train policies, assign rewards, export data, or alter
 * tool execution. It projects already-authorized agent events into a compact
 * per-run trajectory ledger that future evaluators can score and replay.
 *
 * The fields deliberately correspond to the agentic-RL constraints surfaced
 * in the Will Brown / open-environments research: environment identity and
 * fidelity, semantic-horizon position, verifier identity/faithfulness,
 * exploration branch, and reset/checkpoint lineage. They are annotations,
 * not self-issued rewards or training labels.
 *
 * DSCO_RL_HOOKS controls capture:
 *   off       disable projection
 *   metadata  write event names/statuses and costs only (default)
 */

typedef struct {
    const char *run_id;
    unsigned long long sequence;
    long long timestamp_ms;
    const char *event_name;
    const char *category;
    const char *status;
    const char *payload_json;
    double usd_delta;
    int input_tokens;
    int output_tokens;
    /* Optional environment / evaluation annotations. Never inferred as reward. */
    const char *environment_id;
    const char *environment_fidelity;
    const char *verifier_id;
    const char *verifier_version;
    const char *branch_id;
    const char *checkpoint_id;
    int semantic_step;
    int semantic_horizon;
} rl_hook_event_t;

/* Returns false only for invalid input or an I/O failure. Disabled hooks are
 * a successful no-op. Records are projected as `rl.trajectory.event` frames
 * into the existing per-run Chronicle journal. */
bool rl_hooks_record_event(const rl_hook_event_t *event);

/* Pure formatter for tests and alternate durable sinks. */
bool rl_hooks_format_event(const rl_hook_event_t *event, char *out, size_t out_len);

/* Read-only readiness report over a completed or live run journal. run_id may
 * be NULL to inspect the current Chronicle run. Output is JSON. */
bool rl_hooks_inspect_run(const char *run_id, char *out, size_t out_len);

#endif
