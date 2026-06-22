#ifndef DSCO_CONTROL_FLOW_H
#define DSCO_CONTROL_FLOW_H

/*
 * control_flow.h — Priority 6: Conditional Branching (DAG → Turing-complete)
 *
 * Adds if/while/for/switch/try-catch to the plan engine without touching
 * the existing step_t / atom_t / plan_t definitions in plan.h.
 *
 * Usage:
 *   control_flow_t cf = {0};
 *   cf.type = CF_IF;
 *   condition_parse("score >= 0.8", &cf.condition);
 *   cf.body_step_ids[0] = body_step; cf.body_step_count = 1;
 *   step_set_control_flow(step_id, &cf);
 *   execute_step_with_control(step_id, &cf, "{\"score\": 0.9}");
 */

#include <stdbool.h>
#include <stddef.h>
#include "plan.h"

/* Default WHILE loop iteration cap — prevents infinite loops */
#define CF_DEFAULT_LOOP_MAX 256

/* ── Control flow type ────────────────────────────────────────────────── */

typedef enum {
    CF_NONE   = 0,
    CF_IF,
    CF_WHILE,
    CF_FOR,
    CF_SWITCH,
    CF_TRY,
} control_type_t;

/* ── Condition operators ───────────────────────────────────────────────── */

typedef enum {
    COP_TRUTHY  = 0,  /* lhs is truthy (non-zero, non-"false", non-empty) */
    COP_EXISTS,       /* JSON field is present in context */
    COP_EQ,           /* == */
    COP_NEQ,          /* != */
    COP_LT,           /* <  */
    COP_LE,           /* <= */
    COP_GT,           /* >  */
    COP_GE,           /* >= */
    COP_CONTAINS,     /* substring match */
} cond_op_t;

#define COND_OPERAND_LEN 128

/* ── condition_t ──────────────────────────────────────────────────────── */

typedef struct {
    char      lhs[COND_OPERAND_LEN];  /* JSON field name to look up, or literal */
    cond_op_t op;
    char      rhs[COND_OPERAND_LEN];  /* comparison target (literal) */
    bool      negate;                  /* if true, invert the result */
} condition_t;

/* ── control_flow_t ───────────────────────────────────────────────────── */

#define CF_MAX_BODY_STEPS  16
#define CF_MAX_ELSE_STEPS  16
#define CF_MAX_CATCH_STEPS 16

typedef struct {
    control_type_t type;
    condition_t    condition;

    /* WHILE / FOR: maximum number of iterations; 0 → CF_DEFAULT_LOOP_MAX */
    int            loop_max;

    /* Steps executed when condition is true (IF body, WHILE/FOR body) */
    int            body_step_ids[CF_MAX_BODY_STEPS];
    int            body_step_count;

    /* IF-else branch steps (run when condition is false) */
    int            else_step_ids[CF_MAX_ELSE_STEPS];
    int            else_step_count;

    /* TRY-catch branch steps (run when body fails) */
    int            catch_step_ids[CF_MAX_CATCH_STEPS];
    int            catch_step_count;

    /* Populated by TRY on failure: the error message from the failing atom */
    char           exception_buf[256];
} control_flow_t;

/* ── Parsing ──────────────────────────────────────────────────────────── */

/*
 * Parse a condition expression string into a condition_t.
 *
 * Supported forms:
 *   "score < 0.8"          → COP_LT comparison
 *   "status == success"    → COP_EQ comparison
 *   "retries >= 3"         → COP_GE comparison
 *   "error contains timeout" → COP_CONTAINS match
 *   "field exists"         → COP_EXISTS check
 *   "flag"                 → COP_TRUTHY check
 *   "!flag"                → COP_TRUTHY check, negate=true
 *
 * Returns true on success; cond_out is filled.
 */
bool condition_parse(const char *expr_str, condition_t *cond_out);

/* ── Evaluation ───────────────────────────────────────────────────────── */

/*
 * Evaluate a condition against a JSON context string.
 *
 * ctx_json: a JSON object like {"score": 0.65, "status": "running"}, may be NULL.
 * The lhs field is looked up in ctx_json; if absent the lhs literal is used directly.
 * Returns true when the condition is satisfied (after applying cond->negate).
 */
bool condition_evaluate(const condition_t *cond, const char *ctx_json);

/* ── Step execution ───────────────────────────────────────────────────── */

/*
 * Run all atoms in a step sequentially; updates step status.
 * Returns PLAN_DONE on success, PLAN_FAILED if any atom fails.
 */
plan_status_t step_execute_atoms(int step_id);

/*
 * Execute a step according to its control_flow_t.
 *
 * If cf is NULL or cf->type == CF_NONE, falls back to step_execute_atoms.
 * state_json: current execution context (JSON object), may be NULL.
 *
 * Returns PLAN_DONE, PLAN_FAILED, or PLAN_SKIPPED.
 *
 * Semantics per type:
 *   CF_IF:     evaluate condition; run body_step_ids if true, else_step_ids if false.
 *   CF_WHILE:  loop while condition is true; cap at loop_max (or CF_DEFAULT_LOOP_MAX).
 *   CF_FOR:    run body_step_ids loop_max times unconditionally.
 *   CF_SWITCH: like CF_IF (one condition → body or else).
 *   CF_TRY:    run body_step_ids; on PLAN_FAILED, run catch_step_ids and recover.
 */
plan_status_t execute_step_with_control(
    int step_id,
    const control_flow_t *cf,
    const char *state_json);

/* ── Registry ─────────────────────────────────────────────────────────── */

/* Attach a control_flow_t to a step.  Overwrites any previous entry. */
bool step_set_control_flow(int step_id, const control_flow_t *cf);

/* Retrieve the control_flow_t registered for a step, or NULL if none. */
const control_flow_t *step_get_control_flow(int step_id);

/* ── Name helper ──────────────────────────────────────────────────────── */

const char *control_type_name(control_type_t t);

#endif /* DSCO_CONTROL_FLOW_H */
