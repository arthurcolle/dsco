/*
 * control_flow.c — Priority 6: Conditional Branching (DAG → Turing-complete)
 *
 * Implements if/while/for/switch/try-catch on top of the plan engine.
 * Linear DAG execution is unchanged; control flow is opt-in per step.
 */

#include "control_flow.h"
#include "plan.h"
#include "json_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <stdbool.h>

/* ── Static registry ──────────────────────────────────────────────────── */

#define CF_REGISTRY_CAP STEP_MAX

typedef struct {
    int            step_id;
    control_flow_t cf;
} cf_entry_t;

static cf_entry_t s_registry[CF_REGISTRY_CAP];
static int        s_registry_count = 0;

/* ── Internal helpers ─────────────────────────────────────────────────── */

static void trim_inplace(char *s) {
    char *p = s;
    while (*p && isspace((unsigned char)*p)) p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    int n = (int)strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1])) s[--n] = '\0';
}

/* Extract a field from ctx_json into buf (up to bufsz-1 chars).
 * JSON strings are returned unquoted. Numbers/bools as raw text.
 * Returns true if the field was found. */
static bool json_extract_field(const char *ctx_json, const char *key,
                                char *buf, size_t bufsz) {
    if (!ctx_json || !key || !*key || !buf || bufsz == 0) return false;

    /* Try as string first: returns heap-allocated unquoted value */
    char *sv = json_get_str(ctx_json, key);
    if (sv) {
        strncpy(buf, sv, bufsz - 1);
        buf[bufsz - 1] = '\0';
        free(sv);
        return true;
    }

    /* Try as raw value (number, bool, null): heap-allocated */
    char *raw = json_get_raw(ctx_json, key);
    if (raw) {
        strncpy(buf, raw, bufsz - 1);
        buf[bufsz - 1] = '\0';
        free(raw);
        return true;
    }

    return false;
}

/* ── control_type_name ────────────────────────────────────────────────── */

const char *control_type_name(control_type_t t) {
    switch (t) {
    case CF_NONE:   return "none";
    case CF_IF:     return "if";
    case CF_WHILE:  return "while";
    case CF_FOR:    return "for";
    case CF_SWITCH: return "switch";
    case CF_TRY:    return "try";
    default:        return "unknown";
    }
}

/* ── condition_parse ─────────────────────────────────────────────────── */

bool condition_parse(const char *expr_str, condition_t *cond_out) {
    if (!expr_str || !cond_out) return false;
    memset(cond_out, 0, sizeof(*cond_out));

    char buf[512];
    strncpy(buf, expr_str, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    trim_inplace(buf);

    /* Leading '!' means negate the result */
    const char *p = buf;
    if (*p == '!') {
        cond_out->negate = true;
        p++;
        while (isspace((unsigned char)*p)) p++;
    }

    /* Scan for operators — longest match wins; pick the earliest position */
    const char *op_pos = NULL;
    cond_op_t   op     = COP_TRUTHY;

    /* Two-char operators */
    const char *le  = strstr(p, "<=");
    const char *ge  = strstr(p, ">=");
    const char *eq  = strstr(p, "==");
    const char *neq = strstr(p, "!=");

    /* Single-char operators (must not overlap with two-char) */
    const char *lt = strchr(p, '<');
    if (lt && lt == le) lt = (le ? strchr(le + 2, '<') : NULL);  /* skip <= */
    const char *gt = strchr(p, '>');
    if (gt && gt == ge) gt = (ge ? strchr(ge + 2, '>') : NULL);  /* skip >= */

    /* Keyword operators */
    const char *contains = strstr(p, " contains ");
    const char *exists   = strstr(p, " exists");

    /* Pick earliest match (lowest pointer address) */
#define TRY_OP(ptr, candidate_op) \
    if ((ptr) && (!(op_pos) || (ptr) < op_pos)) { op_pos = (ptr); op = (candidate_op); }

    TRY_OP(le,       COP_LE);
    TRY_OP(ge,       COP_GE);
    TRY_OP(eq,       COP_EQ);
    TRY_OP(neq,      COP_NEQ);
    TRY_OP(lt,       COP_LT);
    TRY_OP(gt,       COP_GT);
    TRY_OP(contains, COP_CONTAINS);
    TRY_OP(exists,   COP_EXISTS);

#undef TRY_OP

    cond_out->op = op;

    if (op == COP_TRUTHY || !op_pos) {
        /* Whole expression is the lhs field name */
        strncpy(cond_out->lhs, p, COND_OPERAND_LEN - 1);
        trim_inplace(cond_out->lhs);
        return true;
    }

    /* lhs: everything before op_pos */
    size_t lhs_len = (size_t)(op_pos - p);
    if (lhs_len >= COND_OPERAND_LEN) lhs_len = COND_OPERAND_LEN - 1;
    memcpy(cond_out->lhs, p, lhs_len);
    cond_out->lhs[lhs_len] = '\0';
    trim_inplace(cond_out->lhs);

    /* rhs: skip past the operator token */
    const char *rhs_start = op_pos;
    switch (op) {
    case COP_LE: case COP_GE: case COP_EQ: case COP_NEQ: rhs_start += 2; break;
    case COP_LT: case COP_GT:                             rhs_start += 1; break;
    case COP_CONTAINS: rhs_start += strlen(" contains ");                  break;
    case COP_EXISTS:   rhs_start += strlen(" exists");                     break;
    default: break;
    }
    strncpy(cond_out->rhs, rhs_start, COND_OPERAND_LEN - 1);
    cond_out->rhs[COND_OPERAND_LEN - 1] = '\0';
    trim_inplace(cond_out->rhs);

    return true;
}

/* ── condition_evaluate ─────────────────────────────────────────────── */

bool condition_evaluate(const condition_t *cond, const char *ctx_json) {
    if (!cond) return false;

    /* COP_EXISTS: just check if the key is present */
    if (cond->op == COP_EXISTS) {
        if (!ctx_json || !*ctx_json) return cond->negate ? true : false;
        char tmp[COND_OPERAND_LEN];
        bool found = json_extract_field(ctx_json, cond->lhs, tmp, sizeof(tmp));
        return cond->negate ? !found : found;
    }

    /* Resolve lhs: look up in JSON, fall back to literal */
    char lhs_str[COND_OPERAND_LEN] = {0};
    if (ctx_json && *ctx_json) {
        if (!json_extract_field(ctx_json, cond->lhs, lhs_str, sizeof(lhs_str)))
            strncpy(lhs_str, cond->lhs, COND_OPERAND_LEN - 1);
    } else {
        strncpy(lhs_str, cond->lhs, COND_OPERAND_LEN - 1);
    }

    /* COP_TRUTHY: non-empty, non-zero, not "false", not "null" */
    if (cond->op == COP_TRUTHY) {
        bool truthy = (lhs_str[0] != '\0' &&
                       strcmp(lhs_str, "0")     != 0 &&
                       strcmp(lhs_str, "false") != 0 &&
                       strcmp(lhs_str, "null")  != 0);
        return cond->negate ? !truthy : truthy;
    }

    /* COP_CONTAINS: substring match */
    if (cond->op == COP_CONTAINS) {
        bool hit = (strstr(lhs_str, cond->rhs) != NULL);
        return cond->negate ? !hit : hit;
    }

    /* Numeric vs string comparison */
    char   *ep;
    double  lhs_num   = strtod(lhs_str,   &ep);
    bool    lhs_is_n  = (ep != lhs_str && *ep == '\0');
    double  rhs_num   = strtod(cond->rhs, &ep);
    bool    rhs_is_n  = (ep != cond->rhs && *ep == '\0');
    bool    both_num  = lhs_is_n && rhs_is_n;

    bool result = false;
    switch (cond->op) {
    case COP_EQ:
        result = both_num ? (fabs(lhs_num - rhs_num) < 1e-9)
                          : (strcmp(lhs_str, cond->rhs) == 0);
        break;
    case COP_NEQ:
        result = both_num ? (fabs(lhs_num - rhs_num) >= 1e-9)
                          : (strcmp(lhs_str, cond->rhs) != 0);
        break;
    case COP_LT:
        result = both_num ? (lhs_num <  rhs_num) : (strcmp(lhs_str, cond->rhs) <  0);
        break;
    case COP_LE:
        result = both_num ? (lhs_num <= rhs_num) : (strcmp(lhs_str, cond->rhs) <= 0);
        break;
    case COP_GT:
        result = both_num ? (lhs_num >  rhs_num) : (strcmp(lhs_str, cond->rhs) >  0);
        break;
    case COP_GE:
        result = both_num ? (lhs_num >= rhs_num) : (strcmp(lhs_str, cond->rhs) >= 0);
        break;
    default:
        result = false;
        break;
    }

    return cond->negate ? !result : result;
}

/* ── Registry ─────────────────────────────────────────────────────────── */

bool step_set_control_flow(int step_id, const control_flow_t *cf) {
    if (step_id <= 0 || !cf) return false;

    for (int i = 0; i < s_registry_count; i++) {
        if (s_registry[i].step_id == step_id) {
            s_registry[i].cf = *cf;
            return true;
        }
    }

    if (s_registry_count >= CF_REGISTRY_CAP) return false;
    s_registry[s_registry_count].step_id = step_id;
    s_registry[s_registry_count].cf      = *cf;
    s_registry_count++;
    return true;
}

const control_flow_t *step_get_control_flow(int step_id) {
    for (int i = 0; i < s_registry_count; i++) {
        if (s_registry[i].step_id == step_id)
            return &s_registry[i].cf;
    }
    return NULL;
}

/* ── step_execute_atoms ─────────────────────────────────────────────── */

plan_status_t step_execute_atoms(int step_id) {
    step_t *s = step_get(step_id);
    if (!s) return PLAN_FAILED;

    step_set_status(step_id, PLAN_IN_PROGRESS);

    char result_buf[4096];
    for (int i = 0; i < s->atom_count; i++) {
        bool ok = atom_run(s->atom_ids[i], result_buf, sizeof(result_buf));
        if (!ok) {
            step_set_status(step_id, PLAN_FAILED);
            return PLAN_FAILED;
        }
    }

    step_set_status(step_id, PLAN_DONE);
    return PLAN_DONE;
}

/* ── execute_step_with_control ──────────────────────────────────────── */

/* Run a list of steps in order; nested control flow is respected. */
static plan_status_t run_step_list(const int *ids, int count,
                                   const char *state_json) {
    for (int i = 0; i < count; i++) {
        step_t *s = step_get(ids[i]);
        if (!s) continue;

        const control_flow_t *nested = step_get_control_flow(ids[i]);
        plan_status_t st =
            (nested && nested->type != CF_NONE)
            ? execute_step_with_control(ids[i], nested, state_json)
            : step_execute_atoms(ids[i]);

        if (st == PLAN_FAILED) return PLAN_FAILED;
    }
    return PLAN_DONE;
}

plan_status_t execute_step_with_control(
    int step_id,
    const control_flow_t *cf,
    const char *state_json)
{
    if (!cf || cf->type == CF_NONE)
        return step_execute_atoms(step_id);

    switch (cf->type) {

    case CF_IF: {
        bool cond = condition_evaluate(&cf->condition, state_json);
        if (cond) {
            plan_status_t st =
                cf->body_step_count > 0
                ? run_step_list(cf->body_step_ids, cf->body_step_count, state_json)
                : step_execute_atoms(step_id);
            step_set_status(step_id, st);
            return st;
        } else if (cf->else_step_count > 0) {
            plan_status_t st =
                run_step_list(cf->else_step_ids, cf->else_step_count, state_json);
            step_set_status(step_id, st);
            return st;
        } else {
            step_set_status(step_id, PLAN_SKIPPED);
            return PLAN_DONE;
        }
    }

    case CF_WHILE: {
        int cap  = cf->loop_max > 0 ? cf->loop_max : CF_DEFAULT_LOOP_MAX;
        int iter = 0;
        while (condition_evaluate(&cf->condition, state_json) && iter < cap) {
            plan_status_t st =
                cf->body_step_count > 0
                ? run_step_list(cf->body_step_ids, cf->body_step_count, state_json)
                : step_execute_atoms(step_id);
            if (st == PLAN_FAILED) {
                step_set_status(step_id, PLAN_FAILED);
                return PLAN_FAILED;
            }
            iter++;
        }
        step_set_status(step_id, PLAN_DONE);
        return PLAN_DONE;
    }

    case CF_FOR: {
        int iters = cf->loop_max > 0 ? cf->loop_max : 1;
        for (int i = 0; i < iters; i++) {
            plan_status_t st =
                cf->body_step_count > 0
                ? run_step_list(cf->body_step_ids, cf->body_step_count, state_json)
                : step_execute_atoms(step_id);
            if (st == PLAN_FAILED) {
                step_set_status(step_id, PLAN_FAILED);
                return PLAN_FAILED;
            }
        }
        step_set_status(step_id, PLAN_DONE);
        return PLAN_DONE;
    }

    case CF_SWITCH: {
        /* Treat as a single-case IF: condition → body, else → else */
        bool cond = condition_evaluate(&cf->condition, state_json);
        plan_status_t st;
        if (cond && cf->body_step_count > 0) {
            st = run_step_list(cf->body_step_ids, cf->body_step_count, state_json);
        } else if (!cond && cf->else_step_count > 0) {
            st = run_step_list(cf->else_step_ids, cf->else_step_count, state_json);
        } else {
            st = PLAN_DONE;
            step_set_status(step_id, PLAN_SKIPPED);
        }
        if (st != PLAN_DONE) step_set_status(step_id, st);
        return st;
    }

    case CF_TRY: {
        plan_status_t body_st =
            cf->body_step_count > 0
            ? run_step_list(cf->body_step_ids, cf->body_step_count, state_json)
            : step_execute_atoms(step_id);

        if (body_st == PLAN_FAILED && cf->catch_step_count > 0) {
            /* Run catch / recovery steps */
            plan_status_t catch_st =
                run_step_list(cf->catch_step_ids, cf->catch_step_count, state_json);
            step_set_status(step_id, catch_st);
            return catch_st;
        }

        step_set_status(step_id, body_st);
        return body_st;
    }

    default:
        return step_execute_atoms(step_id);
    }
}
