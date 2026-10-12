#include "work_algebra.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(condition, message)                                                                  \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "FAIL: %s (line %d)\n", (message), __LINE__);                          \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

static work_unit_t make_unit(const char *id) {
    work_unit_t unit;
    memset(&unit, 0, sizeof(unit));
    snprintf(unit.id, sizeof(unit.id), "%s", id);
    unit.temperature = -1;
    unit.top_p = -1;
    unit.top_k = -1;
    unit.thinking_budget = -1;
    unit.max_turns = -1;
    return unit;
}

static void add_dep(work_unit_t *unit, const char *id) {
    void *grown = realloc(unit->depends_on, (unit->depends_count + 1) * sizeof(*unit->depends_on));
    if (!grown) {
        fprintf(stderr, "test setup OOM\n");
        exit(2);
    }
    unit->depends_on = grown;
    snprintf(unit->depends_on[unit->depends_count], WORK_ID_LEN, "%s", id);
    unit->depends_count++;
}

static void free_units(work_unit_t *units, size_t count) {
    for (size_t i = 0; i < count; i++)
        free(units[i].depends_on);
}

static void test_graph_validation(void) {
    work_graph_diagnostic_t diag;
    work_unit_t diamond[4] = {make_unit("root"), make_unit("left"), make_unit("right"),
                              make_unit("sink")};
    add_dep(&diamond[1], "root");
    add_dep(&diamond[2], "root");
    add_dep(&diamond[3], "left");
    add_dep(&diamond[3], "right");
    CHECK(work_graph_validate(diamond, 4, &diag), "diamond graph is valid");
    CHECK(diag.code == WORK_GRAPH_OK, "valid graph reports ok");

    add_dep(&diamond[0], "sink");
    CHECK(!work_graph_validate(diamond, 4, &diag), "cycle is rejected");
    CHECK(diag.code == WORK_GRAPH_DEPENDENCY_CYCLE, "cycle is diagnosed");
    free_units(diamond, 4);

    work_unit_t missing = make_unit("consumer");
    add_dep(&missing, "not-present");
    CHECK(!work_graph_validate(&missing, 1, &diag), "missing dependency is rejected");
    CHECK(diag.code == WORK_GRAPH_MISSING_DEPENDENCY, "missing dependency is diagnosed");
    free(missing.depends_on);

    work_unit_t duplicate[2] = {make_unit("same"), make_unit("same")};
    CHECK(!work_graph_validate(duplicate, 2, &diag), "duplicate ids are rejected");
    CHECK(diag.code == WORK_GRAPH_DUPLICATE_ID, "duplicate id is diagnosed");
}

static void test_sequence_is_transactional_and_idempotent(void) {
    work_graph_diagnostic_t diag;
    work_unit_t units[3] = {make_unit("a"), make_unit("b"), make_unit("c")};
    add_dep(&units[2], "a");

    CHECK(work_compose_sequence(units, 3, &diag), "sequence composes");
    CHECK(units[0].depends_count == 0, "sequence root remains independent");
    CHECK(units[1].depends_count == 1 && !strcmp(units[1].depends_on[0], "a"),
          "second unit depends on first");
    CHECK(units[2].depends_count == 2 && !strcmp(units[2].depends_on[0], "a") &&
              !strcmp(units[2].depends_on[1], "b"),
          "existing dependencies are retained before new sequence edge");
    CHECK(work_compose_sequence(units, 3, &diag), "sequence can be recomposed");
    CHECK(units[1].depends_count == 1 && units[2].depends_count == 2,
          "sequence composition is idempotent");
    free_units(units, 3);

    work_unit_t cycle[2] = {make_unit("a"), make_unit("b")};
    add_dep(&cycle[0], "b");
    CHECK(!work_compose_sequence(cycle, 2, &diag),
          "sequence that would create a cycle is rejected");
    CHECK(diag.code == WORK_GRAPH_DEPENDENCY_CYCLE, "failed sequence reports the cycle");
    CHECK(cycle[0].depends_count == 1 && cycle[1].depends_count == 0,
          "failed sequence rolls every mutation back");
    free_units(cycle, 2);
}

static void test_ready_wave(void) {
    work_graph_diagnostic_t diag;
    work_unit_t units[4] = {make_unit("root"), make_unit("map-a"), make_unit("map-b"),
                            make_unit("reduce")};
    add_dep(&units[1], "root");
    add_dep(&units[2], "root");
    add_dep(&units[3], "map-a");
    add_dep(&units[3], "map-b");
    work_node_state_t states[4] = {WORK_NODE_PENDING, WORK_NODE_PENDING, WORK_NODE_PENDING,
                                   WORK_NODE_PENDING};
    size_t indices[4] = {99, 99, 99, 99};

    size_t ready = work_graph_ready_wave(units, 4, states, indices, 4, &diag);
    CHECK(ready == 1 && indices[0] == 0, "root is the first ready wave");

    states[0] = WORK_NODE_SUCCEEDED;
    ready = work_graph_ready_wave(units, 4, states, indices, 1, &diag);
    CHECK(ready == 2 && indices[0] == 1,
          "ready wave reports total parallelism even with a short output buffer");

    states[1] = WORK_NODE_SUCCEEDED;
    states[2] = WORK_NODE_SUCCEEDED;
    ready = work_graph_ready_wave(units, 4, states, indices, 4, &diag);
    CHECK(ready == 1 && indices[0] == 3, "fan-in sink becomes ready after both maps");
    free_units(units, 4);
}

static void test_fan_in_is_transactional_and_idempotent(void) {
    work_graph_diagnostic_t diag;
    work_unit_t sources[3] = {make_unit("map-a"), make_unit("map-b"), make_unit("map-c")};
    work_unit_t sink = make_unit("reduce");
    CHECK(work_compose_fan_in(sources, 3, &sink, &diag), "fan-in composes");
    CHECK(sink.depends_count == 3, "reducer depends on every mapper");
    CHECK(work_compose_fan_in(sources, 3, &sink, &diag), "fan-in can be recomposed");
    CHECK(sink.depends_count == 3, "fan-in composition is idempotent");
    free_units(sources, 3);
    free(sink.depends_on);

    work_unit_t source = make_unit("source");
    work_unit_t cyclic_sink = make_unit("sink");
    add_dep(&source, "sink");
    CHECK(!work_compose_fan_in(&source, 1, &cyclic_sink, &diag),
          "fan-in that would create a cycle is rejected");
    CHECK(cyclic_sink.depends_count == 0, "failed fan-in rolls sink back");
    free(source.depends_on);
}

static void test_join_semantics(void) {
    work_node_state_t all_ok[] = {WORK_NODE_SUCCEEDED, WORK_NODE_SUCCEEDED};
    work_decision_t d = work_join_decide(WORK_JOIN_ALL, 0, all_ok, 2);
    CHECK(d.flow == WORK_FLOW_COMPLETE && d.reason == WORK_REASON_JOIN_SATISFIED,
          "all completes after every success");

    work_node_state_t any[] = {WORK_NODE_RUNNING, WORK_NODE_SUCCEEDED, WORK_NODE_PENDING};
    d = work_join_decide(WORK_JOIN_ANY, 0, any, 3);
    CHECK(d.flow == WORK_FLOW_COMPLETE && d.cancel_remaining,
          "any completes and cancels unfinished siblings");

    work_node_state_t impossible[] = {WORK_NODE_SUCCEEDED, WORK_NODE_FAILED, WORK_NODE_FAILED,
                                      WORK_NODE_PENDING};
    d = work_join_decide(WORK_JOIN_QUORUM, 3, impossible, 4);
    CHECK(d.flow == WORK_FLOW_FAIL && d.reason == WORK_REASON_QUORUM_IMPOSSIBLE,
          "quorum fails as soon as success becomes impossible");
    CHECK(d.cancel_remaining, "impossible quorum cancels remaining work");

    d = work_join_decide(WORK_JOIN_REDUCE, 0, all_ok, 2);
    CHECK(d.flow == WORK_FLOW_ADVANCE && d.reason == WORK_REASON_REDUCER_READY,
          "reduce advances to its reducer after mapper success");

    d = work_join_decide(WORK_JOIN_QUORUM, 0, all_ok, 2);
    CHECK(d.flow == WORK_FLOW_FAIL && d.reason == WORK_REASON_INVALID,
          "invalid quorum fails closed");
}

static void test_bounded_fixpoint(void) {
    work_bounds_t bounds = {
        .max_cycles = 5,
        .max_attempts = 8,
        .max_cost_usd = 2.0,
        .max_duration_ms = 60000,
        .max_tokens = 10000,
    };
    work_progress_t progress = {
        .cycles_completed = 2,
        .attempts = 2,
        .cost_usd = 0.25,
        .elapsed_ms = 5000,
        .tokens_used = 1000,
        .previous_digest = 11,
        .current_digest = 12,
    };
    work_decision_t d = work_fixpoint_decide(&bounds, &progress);
    CHECK(d.flow == WORK_FLOW_ADVANCE && d.reason == WORK_REASON_PROGRESS,
          "bounded loop advances while progress is changing");

    progress.current_digest = 11;
    d = work_fixpoint_decide(&bounds, &progress);
    CHECK(d.flow == WORK_FLOW_COMPLETE && d.reason == WORK_REASON_STABLE,
          "equal non-zero digests converge");

    progress.current_digest = 12;
    progress.cycles_completed = 5;
    d = work_fixpoint_decide(&bounds, &progress);
    CHECK(d.flow == WORK_FLOW_FAIL && d.reason == WORK_REASON_CYCLE_LIMIT,
          "cycle ceiling terminates a non-converging loop");

    progress.cycles_completed = 1;
    progress.critical_failure = true;
    d = work_fixpoint_decide(&bounds, &progress);
    CHECK(d.flow == WORK_FLOW_FAIL && d.reason == WORK_REASON_CRITICAL_FAILURE,
          "critical failures dominate convergence checks");

    work_bounds_t unbounded = {0};
    memset(&progress, 0, sizeof(progress));
    d = work_fixpoint_decide(&unbounded, &progress);
    CHECK(d.flow == WORK_FLOW_FAIL && d.reason == WORK_REASON_INVALID,
          "accidentally unbounded loop is rejected");
}

static void test_model_configurable_bounds(void) {
    work_bounds_t bounds = {0};
    char error[BUFSIZ];
    char value[sizeof(uint64_t) * CHAR_BIT + 1];
    size_t configured_cycles = sizeof(uint64_t) * CHAR_BIT;
    snprintf(value, sizeof(value), "%zu", configured_cycles);
    CHECK(work_bounds_configure(&bounds, "max_cycles", value, error, sizeof(error)) &&
              bounds.max_cycles == configured_cycles,
          "explicit model value configures a work bound");
    CHECK(!work_bounds_configure(&bounds, "max_cost_usd", "-1", error, sizeof(error)) &&
              bounds.max_cost_usd == 0,
          "invalid cost fails instead of clamping or mutating");

    uint64_t configured_tokens = sizeof(work_progress_t) * CHAR_BIT;
    snprintf(value, sizeof(value), "%llu", (unsigned long long)configured_tokens);
    setenv("DSCO_WORK_MAX_TOKENS", value, 1);
    CHECK(work_bounds_configure_environment(&bounds, error, sizeof(error)) &&
              bounds.max_tokens == configured_tokens,
          "environment configures the same typed surface");
    unsetenv("DSCO_WORK_MAX_TOKENS");

    size_t spec_count = 0;
    CHECK(work_bounds_config_specs(&spec_count) != NULL && spec_count > 0,
          "work bounds expose descriptors to callers");
    size_t needed = work_bounds_format_schema(NULL, 0);
    char *schema = needed == SIZE_MAX ? NULL : malloc(needed + 1);
    CHECK(schema && work_bounds_format_schema(schema, needed + 1) == needed,
          "work bounds expose an exactly sized JSON Schema");
    if (schema) {
        CHECK(strstr(schema, "\"max_cycles\"") && strstr(schema, "\"max_attempts\"") &&
                  strstr(schema, "\"max_cost_usd\"") && strstr(schema, "\"max_duration_ms\"") &&
                  strstr(schema, "\"max_tokens\"") && !strstr(schema, "\"default\""),
              "every operational bound is visible with no invented default");
        free(schema);
    }
}

int main(void) {
    test_graph_validation();
    test_sequence_is_transactional_and_idempotent();
    test_ready_wave();
    test_fan_in_is_transactional_and_idempotent();
    test_join_semantics();
    test_bounded_fixpoint();
    test_model_configurable_bounds();
    if (failures) {
        fprintf(stderr, "%d work algebra test(s) failed\n", failures);
        return 1;
    }
    puts("work algebra: all tests passed");
    return 0;
}
