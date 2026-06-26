/*
 * test_control_flow.c — Priority 6: Conditional Branching tests
 *
 * Standalone compile (from repo root):
 *   cc -std=c2y -I include \
 *      src/control_flow.c src/plan.c src/json_util.c src/arena_alloc.c \
 *      tests/test_control_flow.c \
 *      -o test_control_flow -lm && ./test_control_flow
 *
 * Covers:
 *   1. condition_parse  — expression string tokenisation
 *   2. condition_evaluate — JSON context lookup + numeric/string comparison
 *   3. CF_IF branch    — body vs else selection
 *   4. CF_WHILE exit   — loop_max terminates the loop
 *   5. CF_TRY-catch    — failing body triggers catch, result is PLAN_DONE
 */

#include "control_flow.h"
#include "plan.h"
#include "json_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <math.h>

/* ── Stub: plan.c calls tools_execute for ATOM_TOOL_CALL; none of our test
 *   atoms use that type, but the linker requires the symbol. ────────────── */
bool tools_execute(const char *name, const char *input_json, char *result, size_t result_len) {
    (void)name;
    (void)input_json;
    snprintf(result, result_len, "{\"stub\":true}");
    return true;
}

/* ── Minimal test harness ─────────────────────────────────────────────── */

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, msg)                                                                           \
    do {                                                                                           \
        if (cond) {                                                                                \
            fprintf(stderr, "  PASS  %s\n", msg);                                                  \
            g_pass++;                                                                              \
        } else {                                                                                   \
            fprintf(stderr, "  FAIL  %s  (line %d)\n", msg, __LINE__);                             \
            g_fail++;                                                                              \
        }                                                                                          \
    } while (0)

/* ══════════════════════════════════════════════════════════════════════
 * Test 1: condition_parse
 * ══════════════════════════════════════════════════════════════════════ */

static void test_condition_parse(void) {
    fprintf(stderr, "\n[condition_parse]\n");
    condition_t c;

    /* numeric LT */
    CHECK(condition_parse("score < 0.8", &c), "parse returns true");
    CHECK(strcmp(c.lhs, "score") == 0, "lhs == score");
    CHECK(c.op == COP_LT, "op == COP_LT");
    CHECK(strcmp(c.rhs, "0.8") == 0, "rhs == 0.8");
    CHECK(!c.negate, "negate false");

    /* string equality */
    CHECK(condition_parse("status == success", &c), "parse status ==");
    CHECK(strcmp(c.lhs, "status") == 0, "lhs == status");
    CHECK(c.op == COP_EQ, "op == COP_EQ");
    CHECK(strcmp(c.rhs, "success") == 0, "rhs == success");

    /* GE */
    CHECK(condition_parse("retries >= 3", &c), "parse retries >=");
    CHECK(c.op == COP_GE, "op == COP_GE");
    CHECK(strcmp(c.rhs, "3") == 0, "rhs == 3");

    /* NEQ */
    CHECK(condition_parse("mode != idle", &c), "parse mode !=");
    CHECK(c.op == COP_NEQ, "op == COP_NEQ");

    /* negate with leading ! */
    CHECK(condition_parse("!done", &c), "parse !done");
    CHECK(strcmp(c.lhs, "done") == 0, "lhs == done");
    CHECK(c.op == COP_TRUTHY, "op == COP_TRUTHY");
    CHECK(c.negate, "negate true");

    /* contains */
    CHECK(condition_parse("message contains error", &c), "parse contains");
    CHECK(c.op == COP_CONTAINS, "op == COP_CONTAINS");
    CHECK(strcmp(c.rhs, "error") == 0, "rhs == error");

    /* exists */
    CHECK(condition_parse("field exists", &c), "parse exists");
    CHECK(c.op == COP_EXISTS, "op == COP_EXISTS");
    CHECK(strcmp(c.lhs, "field") == 0, "lhs == field");

    /* bare truthy */
    CHECK(condition_parse("active", &c), "parse bare truthy");
    CHECK(c.op == COP_TRUTHY, "op == COP_TRUTHY");
    CHECK(strcmp(c.lhs, "active") == 0, "lhs == active");
    CHECK(!c.negate, "negate false");
}

/* ══════════════════════════════════════════════════════════════════════
 * Test 2: condition_evaluate
 * ══════════════════════════════════════════════════════════════════════ */

static void test_condition_evaluate(void) {
    fprintf(stderr, "\n[condition_evaluate]\n");
    condition_t c;

    /* numeric LT: 0.65 < 0.8 → true */
    condition_parse("score < 0.8", &c);
    CHECK(condition_evaluate(&c, "{\"score\": 0.65}"), "0.65 < 0.8 → true");
    CHECK(!condition_evaluate(&c, "{\"score\": 0.9}"), "0.9 < 0.8 → false");
    CHECK(!condition_evaluate(&c, "{\"score\": 0.8}"), "0.8 < 0.8 → false (strict)");

    /* GE */
    condition_parse("retries >= 3", &c);
    CHECK(condition_evaluate(&c, "{\"retries\": 5}"), "5 >= 3 → true");
    CHECK(condition_evaluate(&c, "{\"retries\": 3}"), "3 >= 3 → true");
    CHECK(!condition_evaluate(&c, "{\"retries\": 2}"), "2 >= 3 → false");

    /* string EQ */
    condition_parse("status == success", &c);
    CHECK(condition_evaluate(&c, "{\"status\": \"success\"}"), "status eq true");
    CHECK(!condition_evaluate(&c, "{\"status\": \"error\"}"), "status eq false");

    /* NEQ */
    condition_parse("mode != idle", &c);
    CHECK(condition_evaluate(&c, "{\"mode\": \"active\"}"), "mode neq true");
    CHECK(!condition_evaluate(&c, "{\"mode\": \"idle\"}"), "mode neq false");

    /* contains */
    condition_parse("msg contains timeout", &c);
    CHECK(condition_evaluate(&c, "{\"msg\": \"connection timeout occurred\"}"), "contains match");
    CHECK(!condition_evaluate(&c, "{\"msg\": \"all good\"}"), "contains no-match");

    /* negate */
    condition_parse("!done", &c);
    CHECK(condition_evaluate(&c, "{\"done\": \"false\"}"), "!done(false) → true");
    CHECK(!condition_evaluate(&c, "{\"done\": \"true\"}"), "!done(true) → false");

    /* exists */
    condition_parse("field exists", &c);
    CHECK(condition_evaluate(&c, "{\"field\": 1}"), "field exists → true");
    CHECK(!condition_evaluate(&c, "{\"other\": 1}"), "field not present → false");

    /* NULL context falls back to lhs literal */
    condition_parse("score < 0.8", &c);
    CHECK(!condition_evaluate(&c, NULL), "null ctx: literal 'score' not numeric");
}

/* ══════════════════════════════════════════════════════════════════════
 * Test 3: CF_IF — body vs else selection
 * ══════════════════════════════════════════════════════════════════════ */

static void test_cf_if_branch(void) {
    fprintf(stderr, "\n[CF_IF branch]\n");

    plan_engine_init();

    int pid = plan_create("p", "if-test", PLAN_MODE_HYBRID);
    int s = plan_add_step(pid, 0, "if_step", STEP_ATOMIC);
    int body = plan_add_step(pid, 0, "body_step", STEP_ATOMIC);
    int els = plan_add_step(pid, 0, "else_step", STEP_ATOMIC);

    step_add_atom(body, "body_noop", ATOM_NOOP);
    step_add_atom(els, "else_noop", ATOM_NOOP);

    control_flow_t cf;
    memset(&cf, 0, sizeof(cf));
    cf.type = CF_IF;
    condition_parse("score >= 0.5", &cf.condition);
    cf.body_step_ids[0] = body;
    cf.body_step_count = 1;
    cf.else_step_ids[0] = els;
    cf.else_step_count = 1;

    /* condition TRUE: body runs, else skipped */
    plan_status_t r = execute_step_with_control(s, &cf, "{\"score\": 0.8}");
    CHECK(r == PLAN_DONE, "IF true → PLAN_DONE");
    CHECK(step_get(body)->status == PLAN_DONE, "body step executed");
    CHECK(step_get(els)->status == PLAN_PENDING, "else step NOT executed");

    /* Reset step statuses for second run */
    step_set_status(body, PLAN_PENDING);
    step_set_status(els, PLAN_PENDING);

    /* condition FALSE: else runs, body skipped */
    r = execute_step_with_control(s, &cf, "{\"score\": 0.3}");
    CHECK(r == PLAN_DONE, "IF false → PLAN_DONE");
    CHECK(step_get(els)->status == PLAN_DONE, "else step executed");
    CHECK(step_get(body)->status == PLAN_PENDING, "body step NOT executed");
}

/* ══════════════════════════════════════════════════════════════════════
 * Test 4: CF_WHILE — loop_max terminates the loop
 * ══════════════════════════════════════════════════════════════════════ */

static void test_cf_while_loop_max(void) {
    fprintf(stderr, "\n[CF_WHILE loop_max]\n");

    plan_engine_init();

    int pid = plan_create("p", "while-test", PLAN_MODE_HYBRID);
    int s = plan_add_step(pid, 0, "while_step", STEP_ATOMIC);
    int body = plan_add_step(pid, 0, "body_step", STEP_ATOMIC);

    step_add_atom(body, "body_noop", ATOM_NOOP);

    control_flow_t cf;
    memset(&cf, 0, sizeof(cf));
    cf.type = CF_WHILE;
    /* Condition is always true — only loop_max stops it */
    condition_parse("active == 1", &cf.condition);
    cf.loop_max = 5;
    cf.body_step_ids[0] = body;
    cf.body_step_count = 1;

    /* With always-true condition and loop_max=5, must terminate */
    plan_status_t r = execute_step_with_control(s, &cf, "{\"active\": 1}");
    CHECK(r == PLAN_DONE, "WHILE loop_max=5 terminates with PLAN_DONE");
    CHECK(step_get(s)->status == PLAN_DONE, "while step status PLAN_DONE");

    /* Condition immediately false → zero iterations → PLAN_DONE */
    plan_engine_init();
    pid = plan_create("p", "while-zero", PLAN_MODE_HYBRID);
    s = plan_add_step(pid, 0, "while_step", STEP_ATOMIC);
    body = plan_add_step(pid, 0, "body_step", STEP_ATOMIC);
    step_add_atom(body, "body_noop", ATOM_NOOP);

    memset(&cf, 0, sizeof(cf));
    cf.type = CF_WHILE;
    condition_parse("active == 1", &cf.condition);
    cf.loop_max = 10;
    cf.body_step_ids[0] = body;
    cf.body_step_count = 1;

    r = execute_step_with_control(s, &cf, "{\"active\": 0}");
    CHECK(r == PLAN_DONE, "WHILE false-condition → 0 iters → PLAN_DONE");
    CHECK(step_get(body)->status == PLAN_PENDING, "body not executed when condition false");
}

/* ══════════════════════════════════════════════════════════════════════
 * Test 5: CF_TRY-catch — failing body triggers catch, overall PLAN_DONE
 * ══════════════════════════════════════════════════════════════════════ */

static void test_cf_try_catch(void) {
    fprintf(stderr, "\n[CF_TRY-catch]\n");

    plan_engine_init();

    int pid = plan_create("p", "try-test", PLAN_MODE_HYBRID);
    int s = plan_add_step(pid, 0, "try_step", STEP_ATOMIC);
    int body = plan_add_step(pid, 0, "body_step", STEP_ATOMIC);
    int catch = plan_add_step(pid, 0, "catch_step", STEP_ATOMIC);

    /* body: ATOM_ASSERT with "false" → always fails */
    int ba = step_add_atom(body, "failing_assert", ATOM_ASSERT);
    atom_set_assert(ba, "false");

    /* catch: ATOM_NOOP → always succeeds */
    step_add_atom(catch, "recovery_noop", ATOM_NOOP);

    control_flow_t cf;
    memset(&cf, 0, sizeof(cf));
    cf.type = CF_TRY;
    cf.body_step_ids[0] = body;
    cf.body_step_count = 1;
    cf.catch_step_ids[0] = catch;
    cf.catch_step_count = 1;

    plan_status_t r = execute_step_with_control(s, &cf, NULL);
    CHECK(r == PLAN_DONE, "TRY-catch → PLAN_DONE after recovery");
    CHECK(step_get(catch)->status == PLAN_DONE, "catch step executed");
    CHECK(step_get(body)->status == PLAN_FAILED, "body step marked PLAN_FAILED");

    /* TRY with no catch: failure propagates */
    plan_engine_init();
    pid = plan_create("p", "try-nocatch", PLAN_MODE_HYBRID);
    s = plan_add_step(pid, 0, "try_step", STEP_ATOMIC);
    body = plan_add_step(pid, 0, "body_step", STEP_ATOMIC);
    ba = step_add_atom(body, "failing_assert", ATOM_ASSERT);
    atom_set_assert(ba, "false");

    memset(&cf, 0, sizeof(cf));
    cf.type = CF_TRY;
    cf.body_step_ids[0] = body;
    cf.body_step_count = 1;
    /* no catch */

    r = execute_step_with_control(s, &cf, NULL);
    CHECK(r == PLAN_FAILED, "TRY no-catch → PLAN_FAILED propagates");

    /* TRY with passing body: no catch needed */
    plan_engine_init();
    pid = plan_create("p", "try-pass", PLAN_MODE_HYBRID);
    s = plan_add_step(pid, 0, "try_step", STEP_ATOMIC);
    body = plan_add_step(pid, 0, "body_step", STEP_ATOMIC);
    step_add_atom(body, "pass_noop", ATOM_NOOP);

    memset(&cf, 0, sizeof(cf));
    cf.type = CF_TRY;
    cf.body_step_ids[0] = body;
    cf.body_step_count = 1;

    r = execute_step_with_control(s, &cf, NULL);
    CHECK(r == PLAN_DONE, "TRY passing body → PLAN_DONE, no catch needed");
}

/* ══════════════════════════════════════════════════════════════════════
 * Test 6: CF_FOR — fixed iterations
 * ══════════════════════════════════════════════════════════════════════ */

static void test_cf_for(void) {
    fprintf(stderr, "\n[CF_FOR fixed iterations]\n");

    plan_engine_init();

    int pid = plan_create("p", "for-test", PLAN_MODE_HYBRID);
    int s = plan_add_step(pid, 0, "for_step", STEP_ATOMIC);
    int body = plan_add_step(pid, 0, "body_step", STEP_ATOMIC);
    step_add_atom(body, "body_noop", ATOM_NOOP);

    control_flow_t cf;
    memset(&cf, 0, sizeof(cf));
    cf.type = CF_FOR;
    cf.loop_max = 4;
    cf.body_step_ids[0] = body;
    cf.body_step_count = 1;

    plan_status_t r = execute_step_with_control(s, &cf, NULL);
    CHECK(r == PLAN_DONE, "FOR loop_max=4 → PLAN_DONE");
    CHECK(step_get(s)->status == PLAN_DONE, "for step status PLAN_DONE");
}

/* ══════════════════════════════════════════════════════════════════════
 * Test 7: Registry — step_set/get_control_flow
 * ══════════════════════════════════════════════════════════════════════ */

static void test_registry(void) {
    fprintf(stderr, "\n[Registry]\n");

    plan_engine_init();
    int pid = plan_create("p", "registry-test", PLAN_MODE_HYBRID);
    int s = plan_add_step(pid, 0, "step", STEP_ATOMIC);

    CHECK(step_get_control_flow(s) == NULL, "no CF before set");

    control_flow_t cf;
    memset(&cf, 0, sizeof(cf));
    cf.type = CF_IF;
    condition_parse("x > 1", &cf.condition);

    CHECK(step_set_control_flow(s, &cf), "set_control_flow succeeds");

    const control_flow_t *got = step_get_control_flow(s);
    CHECK(got != NULL, "get_control_flow returns non-NULL");
    CHECK(got->type == CF_IF, "type preserved");
    CHECK(strcmp(got->condition.lhs, "x") == 0, "condition lhs preserved");
    CHECK(got->condition.op == COP_GT, "condition op preserved");

    /* Overwrite */
    cf.type = CF_WHILE;
    CHECK(step_set_control_flow(s, &cf), "overwrite succeeds");
    CHECK(step_get_control_flow(s)->type == CF_WHILE, "type updated to CF_WHILE");

    /* Invalid inputs */
    CHECK(!step_set_control_flow(0, &cf), "invalid step_id 0 rejected");
    CHECK(!step_set_control_flow(s, NULL), "NULL cf rejected");
    CHECK(step_get_control_flow(9999) == NULL, "unknown step → NULL");
}

/* ══════════════════════════════════════════════════════════════════════
 * main
 * ══════════════════════════════════════════════════════════════════════ */

int main(void) {
    fprintf(stderr, "=== test_control_flow ===\n");

    test_condition_parse();
    test_condition_evaluate();
    test_cf_if_branch();
    test_cf_while_loop_max();
    test_cf_try_catch();
    test_cf_for();
    test_registry();

    fprintf(stderr, "\nResults: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
