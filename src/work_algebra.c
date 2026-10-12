#include "work_algebra.h"
#include "dsco_meta.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    WORK_BOUND_MAX_CYCLES = 0,
    WORK_BOUND_MAX_ATTEMPTS,
    WORK_BOUND_MAX_COST_USD,
    WORK_BOUND_MAX_DURATION_MS,
    WORK_BOUND_MAX_TOKENS,
    WORK_BOUND_COUNT,
} work_bound_index_t;

static const dsco_config_spec_t WORK_BOUND_SPECS[WORK_BOUND_COUNT] = {
    DSCO_CONFIG_SIZE_SPEC(
        "max_cycles", "DSCO_WORK_MAX_CYCLES",
        "Completed fixpoint cycles allowed; zero leaves this dimension unbounded."),
    DSCO_CONFIG_SIZE_SPEC("max_attempts", "DSCO_WORK_MAX_ATTEMPTS",
                          "Execution attempts allowed; zero leaves this dimension unbounded."),
    {
        .name = "max_cost_usd",
        .environment = "DSCO_WORK_MAX_COST_USD",
        .description = "Metered cost allowed in USD; zero leaves this dimension unbounded.",
        .kind = DSCO_CONFIG_DOUBLE,
        .has_minimum = true,
        .minimum.number = 0,
    },
    DSCO_CONFIG_U64_SPEC("max_duration_ms", "DSCO_WORK_MAX_DURATION_MS",
                         "Elapsed milliseconds allowed; zero leaves this dimension unbounded."),
    DSCO_CONFIG_U64_SPEC("max_tokens", "DSCO_WORK_MAX_TOKENS",
                         "Tokens allowed; zero leaves this dimension unbounded."),
};

const dsco_config_spec_t *work_bounds_config_specs(size_t *count) {
    if (count)
        *count = WORK_BOUND_COUNT;
    return WORK_BOUND_SPECS;
}

size_t work_bounds_format_schema(char *buffer, size_t capacity) {
    return dsco_config_format_schema(WORK_BOUND_SPECS, WORK_BOUND_COUNT, buffer, capacity);
}

static void work_bounds_apply(work_bounds_t *bounds, work_bound_index_t index,
                              dsco_config_value_t value) {
    switch (index) {
        case WORK_BOUND_MAX_CYCLES:
            bounds->max_cycles = value.size;
            break;
        case WORK_BOUND_MAX_ATTEMPTS:
            bounds->max_attempts = value.size;
            break;
        case WORK_BOUND_MAX_COST_USD:
            bounds->max_cost_usd = value.number;
            break;
        case WORK_BOUND_MAX_DURATION_MS:
            bounds->max_duration_ms = value.u64;
            break;
        case WORK_BOUND_MAX_TOKENS:
            bounds->max_tokens = value.u64;
            break;
        case WORK_BOUND_COUNT:
            break;
    }
}

bool work_bounds_configure(work_bounds_t *bounds, const char *name, const char *value, char *error,
                           size_t error_capacity) {
    if (error && error_capacity)
        error[0] = '\0';
    if (!bounds || !name || !name[0] || !value) {
        if (error && error_capacity)
            snprintf(error, error_capacity, "bound name and value are required");
        return false;
    }
    for (size_t i = 0; i < WORK_BOUND_COUNT; i++) {
        if (strcmp(name, WORK_BOUND_SPECS[i].name))
            continue;
        dsco_config_resolved_t resolved;
        if (!dsco_config_resolve(&WORK_BOUND_SPECS[i], value, &resolved, error, error_capacity))
            return false;
        work_bounds_apply(bounds, (work_bound_index_t)i, resolved.value);
        return true;
    }
    if (error && error_capacity)
        snprintf(error, error_capacity, "unknown work bound: %s", name);
    return false;
}

bool work_bounds_configure_environment(work_bounds_t *bounds, char *error, size_t error_capacity) {
    if (error && error_capacity)
        error[0] = '\0';
    if (!bounds) {
        if (error && error_capacity)
            snprintf(error, error_capacity, "work bounds are required");
        return false;
    }
    work_bounds_t candidate = *bounds;
    for (size_t i = 0; i < WORK_BOUND_COUNT; i++) {
        dsco_config_resolved_t resolved;
        if (!dsco_config_resolve(&WORK_BOUND_SPECS[i], NULL, &resolved, error, error_capacity))
            return false;
        if (resolved.is_set)
            work_bounds_apply(&candidate, (work_bound_index_t)i, resolved.value);
    }
    *bounds = candidate;
    return true;
}

typedef char work_dep_id_t[WORK_ID_LEN];

typedef struct {
    work_unit_t *unit;
    work_dep_id_t *old_deps;
    size_t old_count;
    work_dep_id_t *new_deps;
    size_t new_count;
    bool changed;
} dependency_patch_t;

static bool work_id_valid(const char id[WORK_ID_LEN]) {
    return id && id[0] && memchr(id, '\0', WORK_ID_LEN) != NULL;
}

static bool work_id_equal(const char a[WORK_ID_LEN], const char b[WORK_ID_LEN]) {
    return strncmp(a, b, WORK_ID_LEN) == 0;
}

static void copy_id(char dst[WORK_ID_LEN], const char *src) {
    if (!dst)
        return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    snprintf(dst, WORK_ID_LEN, "%s", src);
}

static void graph_diag_set(work_graph_diagnostic_t *diag, work_graph_error_t code,
                           const char *node_id, const char *related_id, const char *fmt, ...) {
    if (!diag)
        return;
    memset(diag, 0, sizeof(*diag));
    diag->code = code;
    copy_id(diag->node_id, node_id);
    copy_id(diag->related_id, related_id);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(diag->message, sizeof(diag->message), fmt, ap);
    va_end(ap);
}

const char *work_graph_error_name(work_graph_error_t code) {
    switch (code) {
        case WORK_GRAPH_OK:
            return "ok";
        case WORK_GRAPH_INVALID_INPUT:
            return "invalid_input";
        case WORK_GRAPH_EMPTY_ID:
            return "empty_id";
        case WORK_GRAPH_DUPLICATE_ID:
            return "duplicate_id";
        case WORK_GRAPH_MISSING_DEPENDENCY:
            return "missing_dependency";
        case WORK_GRAPH_DUPLICATE_DEPENDENCY:
            return "duplicate_dependency";
        case WORK_GRAPH_SELF_DEPENDENCY:
            return "self_dependency";
        case WORK_GRAPH_DEPENDENCY_CYCLE:
            return "dependency_cycle";
        case WORK_GRAPH_OOM:
            return "oom";
        default:
            return "unknown";
    }
}

static ptrdiff_t unit_index(const work_unit_t *units, size_t count, const char id[WORK_ID_LEN]) {
    for (size_t i = 0; i < count; i++)
        if (work_id_equal(units[i].id, id))
            return (ptrdiff_t)i;
    return -1;
}

bool work_graph_validate(const work_unit_t *units, size_t count, work_graph_diagnostic_t *diag) {
    if (diag)
        memset(diag, 0, sizeof(*diag));
    if (count == 0) {
        graph_diag_set(diag, WORK_GRAPH_OK, NULL, NULL, "ok");
        return true;
    }
    if (!units) {
        graph_diag_set(diag, WORK_GRAPH_INVALID_INPUT, NULL, NULL, "non-empty graph has no units");
        return false;
    }

    for (size_t i = 0; i < count; i++) {
        if (!work_id_valid(units[i].id)) {
            graph_diag_set(diag, WORK_GRAPH_EMPTY_ID, NULL, NULL,
                           "unit %zu has an empty or unterminated id", i);
            return false;
        }
        for (size_t j = 0; j < i; j++) {
            if (work_id_equal(units[i].id, units[j].id)) {
                graph_diag_set(diag, WORK_GRAPH_DUPLICATE_ID, units[i].id, units[j].id,
                               "duplicate work id %s", units[i].id);
                return false;
            }
        }
    }

    if (count > SIZE_MAX / sizeof(size_t) || count > SIZE_MAX / sizeof(bool)) {
        graph_diag_set(diag, WORK_GRAPH_OOM, NULL, NULL, "work graph is too large to validate");
        return false;
    }
    size_t *indegree = calloc(count, sizeof(*indegree));
    size_t *wave = malloc(count * sizeof(*wave));
    bool *removed = calloc(count, sizeof(*removed));
    DSCO_DEFER(free, indegree);
    DSCO_DEFER(free, wave);
    DSCO_DEFER(free, removed);
    if (!indegree || !wave || !removed) {
        graph_diag_set(diag, WORK_GRAPH_OOM, NULL, NULL,
                       "could not allocate graph validation state");
        return false;
    }

    for (size_t i = 0; i < count; i++) {
        const work_unit_t *unit = &units[i];
        if (unit->depends_count && !unit->depends_on) {
            graph_diag_set(diag, WORK_GRAPH_INVALID_INPUT, unit->id, NULL,
                           "unit %s has a dependency count but no dependency array", unit->id);
            return false;
        }
        for (size_t d = 0; d < unit->depends_count; d++) {
            const char *dep = unit->depends_on[d];
            if (!work_id_valid(dep)) {
                graph_diag_set(diag, WORK_GRAPH_EMPTY_ID, unit->id, NULL,
                               "unit %s has an empty or unterminated dependency", unit->id);
                return false;
            }
            if (work_id_equal(unit->id, dep)) {
                graph_diag_set(diag, WORK_GRAPH_SELF_DEPENDENCY, unit->id, dep,
                               "unit %s depends on itself", unit->id);
                return false;
            }
            for (size_t prior = 0; prior < d; prior++) {
                if (work_id_equal(dep, unit->depends_on[prior])) {
                    graph_diag_set(diag, WORK_GRAPH_DUPLICATE_DEPENDENCY, unit->id, dep,
                                   "unit %s names dependency %s more than once", unit->id, dep);
                    return false;
                }
            }
            if (unit_index(units, count, dep) < 0) {
                graph_diag_set(diag, WORK_GRAPH_MISSING_DEPENDENCY, unit->id, dep,
                               "unit %s references missing dependency %s", unit->id, dep);
                return false;
            }
            indegree[i]++;
        }
    }

    size_t removed_count = 0;
    while (removed_count < count) {
        size_t wave_count = 0;
        for (size_t i = 0; i < count; i++) {
            if (removed[i] || indegree[i] != 0)
                continue;
            wave[wave_count++] = i;
        }
        if (wave_count == 0) {
            const char *cycle_id = NULL;
            for (size_t i = 0; i < count; i++) {
                if (!removed[i]) {
                    cycle_id = units[i].id;
                    break;
                }
            }
            graph_diag_set(diag, WORK_GRAPH_DEPENDENCY_CYCLE, cycle_id, NULL,
                           "dependency cycle includes unit %s", cycle_id ? cycle_id : "unknown");
            return false;
        }

        /* Kahn wave: remove every ready node together, then decrement exactly
         * the outgoing edges from this wave. */
        for (size_t w = 0; w < wave_count; w++) {
            size_t done = wave[w];
            removed[done] = true;
            removed_count++;
            for (size_t i = 0; i < count; i++) {
                if (removed[i])
                    continue;
                for (size_t d = 0; d < units[i].depends_count; d++) {
                    if (work_id_equal(units[i].depends_on[d], units[done].id)) {
                        indegree[i]--;
                        break;
                    }
                }
            }
        }
    }

    graph_diag_set(diag, WORK_GRAPH_OK, NULL, NULL, "ok");
    return true;
}

static bool dependency_present(const work_unit_t *unit, const char id[WORK_ID_LEN]) {
    for (size_t i = 0; i < unit->depends_count; i++)
        if (work_id_equal(unit->depends_on[i], id))
            return true;
    return false;
}

static bool dependency_patch_build(dependency_patch_t *patch, work_unit_t *unit,
                                   const work_dep_id_t *ids, size_t id_count) {
    memset(patch, 0, sizeof(*patch));
    patch->unit = unit;
    patch->old_deps = unit->depends_on;
    patch->old_count = unit->depends_count;

    size_t additions = 0;
    for (size_t i = 0; i < id_count; i++) {
        if (dependency_present(unit, ids[i]))
            continue;
        bool repeated = false;
        for (size_t prior = 0; prior < i; prior++)
            if (work_id_equal(ids[i], ids[prior])) {
                repeated = true;
                break;
            }
        if (!repeated)
            additions++;
    }
    if (additions == 0)
        return true;
    size_t next_count;
    size_t bytes;
    if (dsco_size_add_overflow(unit->depends_count, additions, &next_count) ||
        dsco_size_mul_overflow(next_count, sizeof(work_dep_id_t), &bytes))
        return false;

    patch->new_count = next_count;
    patch->new_deps = malloc(bytes);
    if (!patch->new_deps)
        return false;
    if (unit->depends_count)
        memcpy(patch->new_deps, unit->depends_on, unit->depends_count * sizeof(*patch->new_deps));
    size_t out = unit->depends_count;
    for (size_t i = 0; i < id_count; i++) {
        bool exists = dependency_present(unit, ids[i]);
        for (size_t prior = 0; !exists && prior < i; prior++)
            exists = work_id_equal(ids[i], ids[prior]);
        if (!exists)
            copy_id(patch->new_deps[out++], ids[i]);
    }
    patch->changed = true;
    return true;
}

static void dependency_patch_apply(dependency_patch_t *patch) {
    if (!patch->changed)
        return;
    patch->unit->depends_on = patch->new_deps;
    patch->unit->depends_count = patch->new_count;
}

static void dependency_patch_commit(dependency_patch_t *patch) {
    if (!patch->changed)
        return;
    free(patch->old_deps);
    patch->old_deps = NULL;
    patch->new_deps = NULL;
    patch->changed = false;
}

static void dependency_patch_rollback(dependency_patch_t *patch) {
    if (!patch->changed)
        return;
    patch->unit->depends_on = patch->old_deps;
    patch->unit->depends_count = patch->old_count;
    free(patch->new_deps);
    patch->new_deps = NULL;
    patch->changed = false;
}

bool work_compose_sequence(work_unit_t *units, size_t count, work_graph_diagnostic_t *diag) {
    if (!work_graph_validate(units, count, diag))
        return false;
    if (count < 2)
        return true;
    if (count - 1 > SIZE_MAX / sizeof(dependency_patch_t)) {
        graph_diag_set(diag, WORK_GRAPH_OOM, NULL, NULL, "sequence is too large to compose");
        return false;
    }
    dependency_patch_t *patches = calloc(count - 1, sizeof(*patches));
    if (!patches) {
        graph_diag_set(diag, WORK_GRAPH_OOM, NULL, NULL, "could not allocate sequence transaction");
        return false;
    }

    size_t built = 0;
    for (size_t i = 1; i < count; i++) {
        work_dep_id_t dependency;
        copy_id(dependency, units[i - 1].id);
        if (!dependency_patch_build(&patches[built], &units[i], &dependency, 1)) {
            graph_diag_set(diag, WORK_GRAPH_OOM, units[i].id, dependency,
                           "could not compose dependency %s -> %s", dependency, units[i].id);
            for (size_t j = 0; j < built; j++)
                dependency_patch_rollback(&patches[j]);
            free(patches);
            return false;
        }
        dependency_patch_apply(&patches[built]);
        built++;
    }

    if (!work_graph_validate(units, count, diag)) {
        for (size_t i = 0; i < built; i++)
            dependency_patch_rollback(&patches[i]);
        free(patches);
        return false;
    }
    for (size_t i = 0; i < built; i++)
        dependency_patch_commit(&patches[i]);
    free(patches);
    return true;
}

static bool copy_closed_graph(const work_unit_t *sources, size_t source_count,
                              const work_unit_t *sink, work_unit_t **copy_out,
                              work_graph_diagnostic_t *diag) {
    if (!sink || (source_count && !sources)) {
        graph_diag_set(diag, WORK_GRAPH_INVALID_INPUT, NULL, NULL,
                       "fan-in requires a sink and all declared sources");
        return false;
    }
    size_t graph_count;
    size_t graph_bytes;
    if (dsco_size_add_overflow(source_count, 1, &graph_count) ||
        dsco_size_mul_overflow(graph_count, sizeof(work_unit_t), &graph_bytes)) {
        graph_diag_set(diag, WORK_GRAPH_OOM, NULL, NULL, "fan-in is too large to compose");
        return false;
    }
    work_unit_t *copy = malloc(graph_bytes);
    if (!copy) {
        graph_diag_set(diag, WORK_GRAPH_OOM, NULL, NULL,
                       "could not allocate fan-in validation graph");
        return false;
    }
    if (source_count)
        memcpy(copy, sources, source_count * sizeof(*copy));
    memcpy(&copy[source_count], sink, sizeof(*copy));
    *copy_out = copy;
    return true;
}

bool work_compose_fan_in(work_unit_t *sources, size_t source_count, work_unit_t *sink,
                         work_graph_diagnostic_t *diag) {
    work_unit_t *closed = NULL;
    if (!copy_closed_graph(sources, source_count, sink, &closed, diag))
        return false;
    bool valid = work_graph_validate(closed, source_count + 1, diag);
    free(closed);
    if (!valid)
        return false;
    if (source_count == 0)
        return true;

    work_dep_id_t *ids = malloc(source_count * sizeof(*ids));
    if (!ids) {
        graph_diag_set(diag, WORK_GRAPH_OOM, sink->id, NULL,
                       "could not allocate fan-in dependency set");
        return false;
    }
    for (size_t i = 0; i < source_count; i++)
        copy_id(ids[i], sources[i].id);

    dependency_patch_t patch;
    if (!dependency_patch_build(&patch, sink, ids, source_count)) {
        graph_diag_set(diag, WORK_GRAPH_OOM, sink->id, NULL, "could not compose fan-in for %s",
                       sink->id);
        free(ids);
        return false;
    }
    free(ids);
    dependency_patch_apply(&patch);

    if (!copy_closed_graph(sources, source_count, sink, &closed, diag)) {
        dependency_patch_rollback(&patch);
        return false;
    }
    valid = work_graph_validate(closed, source_count + 1, diag);
    free(closed);
    if (!valid) {
        dependency_patch_rollback(&patch);
        return false;
    }
    dependency_patch_commit(&patch);
    return true;
}

size_t work_graph_ready_wave(const work_unit_t *units, size_t count,
                             const work_node_state_t *states, size_t *indices, size_t capacity,
                             work_graph_diagnostic_t *diag) {
    if (!work_graph_validate(units, count, diag))
        return 0;
    if (count == 0)
        return 0;
    if (!states || (capacity && !indices)) {
        graph_diag_set(diag, WORK_GRAPH_INVALID_INPUT, NULL, NULL,
                       "ready-wave selection requires states and an output buffer");
        return 0;
    }
    for (size_t i = 0; i < count; i++) {
        if (states[i] < WORK_NODE_PENDING || states[i] > WORK_NODE_CANCELLED) {
            graph_diag_set(diag, WORK_GRAPH_INVALID_INPUT, units[i].id, NULL,
                           "unit %s has invalid runtime state", units[i].id);
            return 0;
        }
    }

    size_t ready = 0;
    for (size_t i = 0; i < count; i++) {
        if (states[i] != WORK_NODE_PENDING)
            continue;
        bool deps_succeeded = true;
        for (size_t d = 0; d < units[i].depends_count; d++) {
            ptrdiff_t dep_index = unit_index(units, count, units[i].depends_on[d]);
            if (dep_index < 0 || states[(size_t)dep_index] != WORK_NODE_SUCCEEDED) {
                deps_succeeded = false;
                break;
            }
        }
        if (!deps_succeeded)
            continue;
        if (ready < capacity)
            indices[ready] = i;
        ready++;
    }
    graph_diag_set(diag, WORK_GRAPH_OK, NULL, NULL, "ok");
    return ready;
}

const char *work_flow_name(work_flow_t flow) {
    switch (flow) {
        case WORK_FLOW_WAIT:
            return "wait";
        case WORK_FLOW_ADVANCE:
            return "advance";
        case WORK_FLOW_COMPLETE:
            return "complete";
        case WORK_FLOW_FAIL:
            return "fail";
        default:
            return "unknown";
    }
}

const char *work_reason_name(work_reason_t reason) {
    switch (reason) {
        case WORK_REASON_PENDING:
            return "pending";
        case WORK_REASON_JOIN_SATISFIED:
            return "join_satisfied";
        case WORK_REASON_REDUCER_READY:
            return "reducer_ready";
        case WORK_REASON_UPSTREAM_FAILED:
            return "upstream_failed";
        case WORK_REASON_QUORUM_IMPOSSIBLE:
            return "quorum_impossible";
        case WORK_REASON_STABLE:
            return "stable";
        case WORK_REASON_PROGRESS:
            return "progress";
        case WORK_REASON_CRITICAL_FAILURE:
            return "critical_failure";
        case WORK_REASON_CYCLE_LIMIT:
            return "cycle_limit";
        case WORK_REASON_ATTEMPT_LIMIT:
            return "attempt_limit";
        case WORK_REASON_COST_LIMIT:
            return "cost_limit";
        case WORK_REASON_DURATION_LIMIT:
            return "duration_limit";
        case WORK_REASON_TOKEN_LIMIT:
            return "token_limit";
        case WORK_REASON_INVALID:
            return "invalid";
        default:
            return "unknown";
    }
}

static work_decision_t decision(work_flow_t flow, work_reason_t reason) {
    work_decision_t out;
    memset(&out, 0, sizeof(out));
    out.flow = flow;
    out.reason = reason;
    return out;
}

work_decision_t work_join_decide(work_join_t join, int quorum, const work_node_state_t *states,
                                 size_t count) {
    work_decision_t out = decision(WORK_FLOW_WAIT, WORK_REASON_PENDING);
    if (!states || count == 0 || join < WORK_JOIN_ALL || join > WORK_JOIN_REDUCE ||
        (join == WORK_JOIN_QUORUM && (quorum <= 0 || (size_t)quorum > count))) {
        return decision(WORK_FLOW_FAIL, WORK_REASON_INVALID);
    }

    for (size_t i = 0; i < count; i++) {
        switch (states[i]) {
            case WORK_NODE_PENDING:
            case WORK_NODE_RUNNING:
                out.pending++;
                break;
            case WORK_NODE_SUCCEEDED:
                out.succeeded++;
                break;
            case WORK_NODE_FAILED:
                out.failed++;
                break;
            case WORK_NODE_CANCELLED:
                out.cancelled++;
                break;
            default:
                return decision(WORK_FLOW_FAIL, WORK_REASON_INVALID);
        }
    }
    size_t unsuccessful = out.failed + out.cancelled;

    switch (join) {
        case WORK_JOIN_ALL:
            if (unsuccessful) {
                out.flow = WORK_FLOW_FAIL;
                out.reason = WORK_REASON_UPSTREAM_FAILED;
                out.cancel_remaining = out.pending > 0;
            } else if (out.succeeded == count) {
                out.flow = WORK_FLOW_COMPLETE;
                out.reason = WORK_REASON_JOIN_SATISFIED;
            }
            break;
        case WORK_JOIN_ANY:
            if (out.succeeded) {
                out.flow = WORK_FLOW_COMPLETE;
                out.reason = WORK_REASON_JOIN_SATISFIED;
                out.cancel_remaining = out.pending > 0;
            } else if (unsuccessful == count) {
                out.flow = WORK_FLOW_FAIL;
                out.reason = WORK_REASON_UPSTREAM_FAILED;
            }
            break;
        case WORK_JOIN_QUORUM:
            if (out.succeeded >= (size_t)quorum) {
                out.flow = WORK_FLOW_COMPLETE;
                out.reason = WORK_REASON_JOIN_SATISFIED;
                out.cancel_remaining = out.pending > 0;
            } else if (out.succeeded + out.pending < (size_t)quorum) {
                out.flow = WORK_FLOW_FAIL;
                out.reason = WORK_REASON_QUORUM_IMPOSSIBLE;
                out.cancel_remaining = out.pending > 0;
            }
            break;
        case WORK_JOIN_REDUCE:
            if (unsuccessful) {
                out.flow = WORK_FLOW_FAIL;
                out.reason = WORK_REASON_UPSTREAM_FAILED;
                out.cancel_remaining = out.pending > 0;
            } else if (out.succeeded == count) {
                out.flow = WORK_FLOW_ADVANCE;
                out.reason = WORK_REASON_REDUCER_READY;
            }
            break;
        default:
            return decision(WORK_FLOW_FAIL, WORK_REASON_INVALID);
    }
    return out;
}

work_decision_t work_fixpoint_decide(const work_bounds_t *bounds, const work_progress_t *progress) {
    if (!bounds || !progress || !isfinite(bounds->max_cost_usd) || !isfinite(progress->cost_usd) ||
        bounds->max_cost_usd < 0 || progress->cost_usd < 0 ||
        (!bounds->max_cycles && !bounds->max_attempts && bounds->max_cost_usd == 0 &&
         !bounds->max_duration_ms && !bounds->max_tokens))
        return decision(WORK_FLOW_FAIL, WORK_REASON_INVALID);

    if (progress->critical_failure)
        return decision(WORK_FLOW_FAIL, WORK_REASON_CRITICAL_FAILURE);
    if (progress->previous_digest && progress->current_digest &&
        progress->previous_digest == progress->current_digest)
        return decision(WORK_FLOW_COMPLETE, WORK_REASON_STABLE);
    if (bounds->max_cycles && progress->cycles_completed >= bounds->max_cycles)
        return decision(WORK_FLOW_FAIL, WORK_REASON_CYCLE_LIMIT);
    if (bounds->max_attempts && progress->attempts >= bounds->max_attempts)
        return decision(WORK_FLOW_FAIL, WORK_REASON_ATTEMPT_LIMIT);
    if (bounds->max_cost_usd > 0 && progress->cost_usd >= bounds->max_cost_usd)
        return decision(WORK_FLOW_FAIL, WORK_REASON_COST_LIMIT);
    if (bounds->max_duration_ms && progress->elapsed_ms >= bounds->max_duration_ms)
        return decision(WORK_FLOW_FAIL, WORK_REASON_DURATION_LIMIT);
    if (bounds->max_tokens && progress->tokens_used >= bounds->max_tokens)
        return decision(WORK_FLOW_FAIL, WORK_REASON_TOKEN_LIMIT);
    return decision(WORK_FLOW_ADVANCE, WORK_REASON_PROGRESS);
}
