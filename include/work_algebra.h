#ifndef DSCO_WORK_ALGEBRA_H
#define DSCO_WORK_ALGEBRA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "config_surface.h"
#include "work_unit.h"

/* Higher-order control flow for work_unit_t.
 *
 * The dispatcher should execute work, not invent orchestration semantics.
 * This module keeps those semantics pure and deterministic: callers compose
 * typed units into graphs, validate them before enqueueing, and use the same
 * join/fixpoint decisions in every runtime surface. */

typedef enum {
    WORK_GRAPH_OK = 0,
    WORK_GRAPH_INVALID_INPUT,
    WORK_GRAPH_EMPTY_ID,
    WORK_GRAPH_DUPLICATE_ID,
    WORK_GRAPH_MISSING_DEPENDENCY,
    WORK_GRAPH_DUPLICATE_DEPENDENCY,
    WORK_GRAPH_SELF_DEPENDENCY,
    WORK_GRAPH_DEPENDENCY_CYCLE,
    WORK_GRAPH_OOM,
} work_graph_error_t;

typedef struct {
    work_graph_error_t code;
    char node_id[WORK_ID_LEN];
    char related_id[WORK_ID_LEN];
    char message[192];
} work_graph_diagnostic_t;

const char *work_graph_error_name(work_graph_error_t code);

/* Validate that `units` is a closed, acyclic work graph. Empty graphs are the
 * identity and therefore valid. Every dependency must name exactly one unit
 * in the supplied set. */
bool work_graph_validate(const work_unit_t *units, size_t count, work_graph_diagnostic_t *diag);

/* Compose units left-to-right by adding unit[i-1] as a dependency of unit[i].
 * Existing dependencies are retained and repeated calls are idempotent.
 * Mutation is transactional: allocation or validation failure leaves every
 * unit exactly as it was. */
bool work_compose_sequence(work_unit_t *units, size_t count, work_graph_diagnostic_t *diag);

/* Compose a map/reduce-style fan-in: `sink` depends on every source. Sources
 * plus sink must form a closed graph. Mutation has the same transactional and
 * idempotent guarantees as work_compose_sequence(). */
bool work_compose_fan_in(work_unit_t *sources, size_t source_count, work_unit_t *sink,
                         work_graph_diagnostic_t *diag);

typedef enum {
    WORK_NODE_PENDING = 0,
    WORK_NODE_RUNNING,
    WORK_NODE_SUCCEEDED,
    WORK_NODE_FAILED,
    WORK_NODE_CANCELLED,
} work_node_state_t;

/* Select the deterministic ready frontier in input order. `states` is aligned
 * with `units`; only pending units whose dependencies all succeeded are ready.
 * Returns the total ready count and writes at most `capacity` indices, which
 * lets callers size a buffer without a second scheduling policy. */
size_t work_graph_ready_wave(const work_unit_t *units, size_t count,
                             const work_node_state_t *states, size_t *indices, size_t capacity,
                             work_graph_diagnostic_t *diag);

typedef enum {
    WORK_FLOW_WAIT = 0, /* more child results are required                */
    WORK_FLOW_ADVANCE,  /* launch the next stage / another bounded cycle  */
    WORK_FLOW_COMPLETE, /* the higher-order operation is complete         */
    WORK_FLOW_FAIL,     /* terminal failure                               */
} work_flow_t;

typedef enum {
    WORK_REASON_PENDING = 0,
    WORK_REASON_JOIN_SATISFIED,
    WORK_REASON_REDUCER_READY,
    WORK_REASON_UPSTREAM_FAILED,
    WORK_REASON_QUORUM_IMPOSSIBLE,
    WORK_REASON_STABLE,
    WORK_REASON_PROGRESS,
    WORK_REASON_CRITICAL_FAILURE,
    WORK_REASON_CYCLE_LIMIT,
    WORK_REASON_ATTEMPT_LIMIT,
    WORK_REASON_COST_LIMIT,
    WORK_REASON_DURATION_LIMIT,
    WORK_REASON_TOKEN_LIMIT,
    WORK_REASON_INVALID,
} work_reason_t;

typedef struct {
    work_flow_t flow;
    work_reason_t reason;
    size_t succeeded;
    size_t failed;
    size_t cancelled;
    size_t pending;
    bool cancel_remaining;
} work_decision_t;

const char *work_flow_name(work_flow_t flow);
const char *work_reason_name(work_reason_t reason);

/* Pure join evaluation. REDUCE returns WORK_FLOW_ADVANCE when all inputs have
 * succeeded: the next action is to run the reducer, not declare completion. */
work_decision_t work_join_decide(work_join_t join, int quorum, const work_node_state_t *states,
                                 size_t count);

typedef struct {
    size_t max_cycles;
    size_t max_attempts;
    double max_cost_usd;
    uint64_t max_duration_ms;
    uint64_t max_tokens;
} work_bounds_t;

/* Machine-readable, default-free control surface for iterative work. Every
 * bound can be set by explicit model/operator text or its named environment
 * variable. Unspecified fields remain unchanged; invalid values fail loudly. */
const dsco_config_spec_t *work_bounds_config_specs(size_t *count);
size_t work_bounds_format_schema(char *buffer, size_t capacity);
bool work_bounds_configure(work_bounds_t *bounds, const char *name, const char *value, char *error,
                           size_t error_capacity);
bool work_bounds_configure_environment(work_bounds_t *bounds, char *error, size_t error_capacity);

typedef struct {
    size_t cycles_completed;
    size_t attempts;
    double cost_usd;
    uint64_t elapsed_ms;
    uint64_t tokens_used;
    uint64_t previous_digest;
    uint64_t current_digest;
    bool critical_failure;
} work_progress_t;

/* Bounded fixpoint evaluation for iterative agents. Zero-valued individual
 * bounds are unbounded, matching work_unit_t economics, but at least one hard
 * bound is required. Equal non-zero digests signal convergence. */
work_decision_t work_fixpoint_decide(const work_bounds_t *bounds, const work_progress_t *progress);

#endif /* DSCO_WORK_ALGEBRA_H */
