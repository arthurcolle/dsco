/*
 * test_stateful_atoms.c — Pipeline data-flow test for Priority 2: Stateful Atoms
 *
 * Compile (standalone, from the repo root):
 *   cc -std=c2y -I include \
 *      src/stateful_atoms.c src/plan.c src/json_util.c src/arena_alloc.c \
 *      tests/test_stateful_atoms.c \
 *      -o test_stateful_atoms -lm && ./test_stateful_atoms
 *
 * Scenario (mirrors the roadmap example):
 *   atom1 (SHELL) → { "ticker":"MSFT", "price":380, "pe":28.5 }
 *   atom2 (SHELL) → { "ticker":"AAPL", "price":150, "pe":29.1 }
 *   atom3 (NOOP)  ← wired from atom1 + atom2
 *                 → wired_input contains merged JSON of atom1 + atom2
 */

#include "stateful_atoms.h"
#include "plan.h"
#include "json_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

/* ── Stub: tools_execute ─────────────────────────────────────────────────── */
/* plan.c calls this for ATOM_TOOL_CALL; none of our test atoms use it, but
 * the linker needs the symbol.  We don't use tools.c here. */
bool tools_execute(const char *name, const char *input_json,
                   char *result, size_t result_len) {
    (void)name; (void)input_json;
    snprintf(result, result_len, "{\"stub\":true}");
    return true;
}

/* ── Minimal test harness ────────────────────────────────────────────────── */

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { \
        fprintf(stderr, "  PASS  %s\n", msg); \
        g_pass++; \
    } else { \
        fprintf(stderr, "  FAIL  %s\n", msg); \
        g_fail++; \
    } \
} while (0)

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static bool contains(const char *haystack, const char *needle) {
    return haystack && needle && strstr(haystack, needle) != NULL;
}

/* ── Tests ───────────────────────────────────────────────────────────────── */

/* Basic state CRUD */
static void test_state_init_free(void) {
    fprintf(stderr, "\n[test_state_init_free]\n");

    plan_state_t *st = plan_state_init(42);
    CHECK(st != NULL,              "init returns non-NULL");
    CHECK(st->plan_id == 42,       "plan_id stored");
    CHECK(st->output_count == 0,   "output_count starts at 0");
    CHECK(st->executed_count == 0, "executed_count starts at 0");
    plan_state_free(st);
    CHECK(true, "free does not crash");
}

static void test_set_get_output(void) {
    fprintf(stderr, "\n[test_set_get_output]\n");

    plan_state_t *st = plan_state_init(1);

    CHECK(plan_state_get_output(st, 99) == NULL, "missing atom → NULL");

    bool ok = plan_state_set_output(st, 10, "{\"x\":1}");
    CHECK(ok, "set_output returns true");

    const char *out = plan_state_get_output(st, 10);
    CHECK(out != NULL && strcmp(out, "{\"x\":1}") == 0, "get_output round-trips");

    /* overwrite */
    plan_state_set_output(st, 10, "{\"x\":2}");
    out = plan_state_get_output(st, 10);
    CHECK(out && strcmp(out, "{\"x\":2}") == 0, "overwrite replaces value");

    plan_state_free(st);
}

/* Pipeline: atom1 → atom2 → atom3 (linear chain) */
static void test_linear_pipeline(void) {
    fprintf(stderr, "\n[test_linear_pipeline — atom1 -> atom2 -> atom3]\n");

    plan_engine_init();

    int plan_id = plan_create("pipeline-test", "linear data flow", PLAN_MODE_TOP_DOWN);
    CHECK(plan_id > 0, "plan created");

    int step_id = plan_add_step(plan_id, 0, "pipeline", STEP_ATOMIC);
    CHECK(step_id > 0, "step created");

    /* atom1: shell command that emits MSFT JSON */
    int a1 = step_add_atom(step_id, "fetch_msft", ATOM_SHELL);
    atom_set_shell(a1, "printf '{\"ticker\":\"MSFT\",\"price\":380,\"pe\":28.5}'");

    /* atom2: shell command that emits AAPL JSON */
    int a2 = step_add_atom(step_id, "fetch_aapl", ATOM_SHELL);
    atom_set_shell(a2, "printf '{\"ticker\":\"AAPL\",\"price\":150,\"pe\":29.1}'");

    /* atom3: noop that reads from atom1 and atom2 */
    int a3 = step_add_atom(step_id, "compare", ATOM_NOOP);
    atom_wire(a1, a3, NULL);   /* a3 reads a1 full output as "atom_<id>" */
    atom_wire(a2, a3, NULL);   /* a3 reads a2 full output as "atom_<id>" */

    CHECK(a1 > 0 && a2 > 0 && a3 > 0, "atoms allocated");

    plan_state_t *st = plan_state_init(plan_id);

    /* Execute atom1 */
    char buf[2048];
    bool ok1 = execute_atom_with_input(st, a1, buf, sizeof buf);
    CHECK(ok1, "atom1 executes successfully");
    CHECK(contains(buf, "MSFT"), "atom1 output contains MSFT");
    CHECK(plan_state_get_output(st, a1) != NULL, "atom1 output stored in state");
    CHECK(contains(plan_state_get_output(st, a1), "380"), "atom1 state has price");

    /* Execute atom2 */
    bool ok2 = execute_atom_with_input(st, a2, buf, sizeof buf);
    CHECK(ok2, "atom2 executes successfully");
    CHECK(contains(buf, "AAPL"), "atom2 output contains AAPL");
    CHECK(plan_state_get_output(st, a2) != NULL, "atom2 output stored in state");

    /* Execute atom3 — should receive merged input from atom1 + atom2 */
    bool ok3 = execute_atom_with_input(st, a3, buf, sizeof buf);
    CHECK(ok3, "atom3 executes successfully");

    atom_t *a3_ptr = atom_get(a3);
    CHECK(a3_ptr != NULL, "atom3 pointer valid");
    CHECK(a3_ptr->wired_input != NULL, "atom3 has wired_input");

    const char *wi = a3_ptr->wired_input;
    CHECK(contains(wi, "MSFT"),  "wired_input contains MSFT from atom1");
    CHECK(contains(wi, "AAPL"),  "wired_input contains AAPL from atom2");
    CHECK(contains(wi, "380"),   "wired_input has atom1 price field");
    CHECK(contains(wi, "29.1"),  "wired_input has atom2 pe field");

    fprintf(stderr, "        wired_input = %.120s...\n", wi ? wi : "(null)");

    CHECK(st->executed_count == 3, "executed_count is 3");

    plan_state_free(st);
    plan_engine_init();  /* reset engine for next test */
}

/* Key-extraction wiring: atom_wire(src, dst, "price") */
static void test_key_extraction(void) {
    fprintf(stderr, "\n[test_key_extraction — extract 'price' field]\n");

    plan_engine_init();

    int plan_id = plan_create("key-extract", "extract specific field", PLAN_MODE_HYBRID);
    int step_id = plan_add_step(plan_id, 0, "steps", STEP_ATOMIC);

    int a1 = step_add_atom(step_id, "source", ATOM_SHELL);
    atom_set_shell(a1, "printf '{\"ticker\":\"MSFT\",\"price\":380,\"pe\":28.5}'");

    int a2 = step_add_atom(step_id, "consumer", ATOM_NOOP);
    atom_wire(a1, a2, "price");   /* extract only the "price" field */

    plan_state_t *st = plan_state_init(plan_id);

    char buf[2048];
    execute_atom_with_input(st, a1, buf, sizeof buf);
    execute_atom_with_input(st, a2, buf, sizeof buf);

    atom_t *a2_ptr = atom_get(a2);
    const char *wi = a2_ptr ? a2_ptr->wired_input : NULL;

    CHECK(wi != NULL, "consumer has wired_input");
    CHECK(contains(wi, "380"),   "extracted price value present");
    CHECK(!contains(wi, "MSFT"), "ticker NOT present (only 'price' extracted)");
    CHECK(!contains(wi, "pe"),   "'pe' NOT present (only 'price' extracted)");

    fprintf(stderr, "        wired_input = %.80s\n", wi ? wi : "(null)");

    plan_state_free(st);
    plan_engine_init();
}

/* Rollback: undo last atom execution */
static void test_rollback(void) {
    fprintf(stderr, "\n[test_rollback — undo last N atoms]\n");

    plan_engine_init();

    int plan_id = plan_create("rollback-test", "rollback", PLAN_MODE_HYBRID);
    int step_id = plan_add_step(plan_id, 0, "steps", STEP_ATOMIC);

    int a1 = step_add_atom(step_id, "a1", ATOM_SHELL);
    atom_set_shell(a1, "printf '{\"v\":1}'");

    int a2 = step_add_atom(step_id, "a2", ATOM_SHELL);
    atom_set_shell(a2, "printf '{\"v\":2}'");

    int a3 = step_add_atom(step_id, "a3", ATOM_NOOP);
    atom_wire(a1, a3, NULL);
    atom_wire(a2, a3, NULL);

    plan_state_t *st = plan_state_init(plan_id);

    char buf[2048];
    execute_atom_with_input(st, a1, buf, sizeof buf);
    execute_atom_with_input(st, a2, buf, sizeof buf);
    execute_atom_with_input(st, a3, buf, sizeof buf);

    CHECK(st->executed_count == 3, "3 atoms executed before rollback");
    CHECK(plan_state_get_output(st, a3) != NULL, "a3 output in state before rollback");

    /* Save a checkpoint and then rollback 1 step */
    plan_state_checkpoint(st);
    int rolled = plan_state_rollback(st, 1);

    CHECK(rolled == 1,               "rolled back 1 atom");
    CHECK(st->executed_count == 2,   "executed_count decremented to 2");
    CHECK(plan_state_get_output(st, a3) == NULL, "a3 output removed from state");
    CHECK(plan_state_get_output(st, a1) != NULL, "a1 output still in state");
    CHECK(plan_state_get_output(st, a2) != NULL, "a2 output still in state");

    atom_t *a3_ptr = atom_get(a3);
    CHECK(a3_ptr && a3_ptr->status == PLAN_PENDING, "a3 status reset to PENDING");
    CHECK(a3_ptr && a3_ptr->result == NULL,         "a3 result cleared");

    /* Re-execute a3 after rollback — data should flow again */
    bool ok = execute_atom_with_input(st, a3, buf, sizeof buf);
    CHECK(ok, "re-execute a3 after rollback succeeds");
    CHECK(st->executed_count == 3, "executed_count back to 3 after re-run");

    /* Rollback 2 steps */
    rolled = plan_state_rollback(st, 2);
    CHECK(rolled == 2,             "rolled back 2 atoms");
    CHECK(st->executed_count == 1, "executed_count is 1");

    plan_state_free(st);
    plan_engine_init();
}

/* Checkpoint and rollback to checkpoint */
static void test_checkpoint(void) {
    fprintf(stderr, "\n[test_checkpoint]\n");

    plan_engine_init();

    int plan_id = plan_create("checkpoint-test", "checkpoint", PLAN_MODE_HYBRID);
    int step_id = plan_add_step(plan_id, 0, "steps", STEP_ATOMIC);

    int a1 = step_add_atom(step_id, "a1", ATOM_NOOP);
    int a2 = step_add_atom(step_id, "a2", ATOM_NOOP);
    int a3 = step_add_atom(step_id, "a3", ATOM_NOOP);

    plan_state_t *st = plan_state_init(plan_id);

    char buf[256];
    execute_atom_with_input(st, a1, buf, sizeof buf);
    execute_atom_with_input(st, a2, buf, sizeof buf);

    bool cp = plan_state_checkpoint(st);
    CHECK(cp, "checkpoint saved");
    CHECK(st->history_count == 1, "history_count is 1");
    CHECK(st->history[0].executed_count == 2, "checkpoint records 2 executed");

    execute_atom_with_input(st, a3, buf, sizeof buf);
    CHECK(st->executed_count == 3, "3 executed after checkpoint");

    /* rollback past the checkpoint */
    int rolled = plan_state_rollback(st, 2);
    CHECK(rolled == 2, "rolled back 2 to reach checkpoint");
    CHECK(st->executed_count == 1, "at checkpoint position");

    plan_state_free(st);
    plan_engine_init();
}

/* plan_state_run_all convenience runner */
static void test_run_all(void) {
    fprintf(stderr, "\n[test_run_all]\n");

    plan_engine_init();

    int plan_id = plan_create("run-all-test", "run all", PLAN_MODE_HYBRID);
    int step_id = plan_add_step(plan_id, 0, "steps", STEP_ATOMIC);

    int a1 = step_add_atom(step_id, "a1", ATOM_NOOP);
    int a2 = step_add_atom(step_id, "a2", ATOM_NOOP);
    atom_wire(a1, a2, NULL);

    plan_state_t *st = plan_state_init(plan_id);

    int ran = plan_state_run_all(st);
    CHECK(ran >= 1,   "plan_state_run_all executed at least 1 atom");
    CHECK(plan_state_get_output(st, a1) != NULL, "a1 output stored");

    plan_state_free(st);
    plan_engine_init();
}

/* ── main ────────────────────────────────────────────────────────────────── */

int main(void) {
    fprintf(stderr, "=== test_stateful_atoms ===\n");

    test_state_init_free();
    test_set_get_output();
    test_linear_pipeline();
    test_key_extraction();
    test_rollback();
    test_checkpoint();
    test_run_all();

    fprintf(stderr, "\n=== Results: %d passed, %d failed ===\n",
            g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}
