/*
 * stateful_atoms.c — Priority 2: Stateful Atom execution engine
 *
 * Implements plan_state_t and execute_atom_with_input().
 * See include/stateful_atoms.h for the full API contract.
 */

#include "stateful_atoms.h"
#include "plan.h"
#include "json_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

/* ── Lifecycle ───────────────────────────────────────────────────────────── */

plan_state_t *plan_state_init(int plan_id) {
    plan_state_t *st = safe_malloc(sizeof(plan_state_t));
    memset(st, 0, sizeof(*st));
    st->plan_id = plan_id;
    return st;
}

void plan_state_free(plan_state_t *state) {
    if (!state) return;
    for (int i = 0; i < state->output_count; i++)
        free(state->outputs[i].output_json);
    free(state);
}

/* ── Output store ────────────────────────────────────────────────────────── */

const char *plan_state_get_output(const plan_state_t *state, int atom_id) {
    if (!state) return NULL;
    for (int i = 0; i < state->output_count; i++)
        if (state->outputs[i].atom_id == atom_id)
            return state->outputs[i].output_json;
    return NULL;
}

bool plan_state_set_output(plan_state_t *state, int atom_id,
                           const char *output_json) {
    if (!state) return false;

    /* update existing entry */
    for (int i = 0; i < state->output_count; i++) {
        if (state->outputs[i].atom_id == atom_id) {
            free(state->outputs[i].output_json);
            state->outputs[i].output_json =
                output_json ? safe_strdup(output_json) : NULL;
            return true;
        }
    }

    /* add new entry */
    if (state->output_count >= PLAN_STATE_MAX_ATOMS) return false;
    int idx = state->output_count++;
    state->outputs[idx].atom_id     = atom_id;
    state->outputs[idx].output_json =
        output_json ? safe_strdup(output_json) : NULL;
    return true;
}

/* ── Checkpoints ─────────────────────────────────────────────────────────── */

bool plan_state_checkpoint(plan_state_t *state) {
    if (!state || state->history_count >= PLAN_STATE_MAX_HISTORY) return false;
    state->history[state->history_count].executed_count = state->executed_count;
    state->history_count++;
    return true;
}

/* ── Rollback ────────────────────────────────────────────────────────────── */

int plan_state_rollback(plan_state_t *state, int steps) {
    if (!state || steps <= 0) return 0;

    int available = state->executed_count;
    int to_undo   = steps < available ? steps : available;

    for (int i = 0; i < to_undo; i++) {
        int atom_id = state->exec_order[state->executed_count - 1 - i];
        atom_t *a = atom_get(atom_id);

        if (a) {
            free(a->result);       a->result      = NULL;
            free(a->wired_input);  a->wired_input = NULL;
            a->status = PLAN_PENDING;
        }

        /* remove from output store */
        for (int j = 0; j < state->output_count; j++) {
            if (state->outputs[j].atom_id == atom_id) {
                free(state->outputs[j].output_json);
                /* compact array */
                int tail = state->output_count - j - 1;
                if (tail > 0)
                    memmove(&state->outputs[j], &state->outputs[j + 1],
                            (size_t)tail * sizeof(state->outputs[0]));
                state->output_count--;
                break;
            }
        }
    }

    state->executed_count -= to_undo;
    return to_undo;
}

/* ── execute_atom_with_input ─────────────────────────────────────────────── */

/*
 * Build a merged input JSON from state-stored upstream outputs.
 * Mirrors atom_resolve_inputs() but sources data from plan_state_t rather
 * than atom->result, so rollback-cleansed outputs don't bleed across runs.
 *
 * Returns heap-allocated JSON string; caller must free.
 */
static char *build_input_from_state(const plan_state_t *state,
                                    const atom_t *dst) {
    if (!state || !dst || dst->input_from_count == 0) return NULL;

    size_t cap = 64 + (size_t)dst->input_from_count * 2048;
    char  *buf = safe_malloc(cap);
    size_t pos = 0;
    buf[pos++] = '{';

    for (int i = 0; i < dst->input_from_count; i++) {
        int         up_id = dst->input_from_ids[i];
        const char *data  = plan_state_get_output(state, up_id);
        if (!data) data = "null";
        const char *key   = dst->input_keys[i];

        if (i > 0 && pos < cap - 1) buf[pos++] = ',';

        char field_name[80];
        if (key && *key)
            snprintf(field_name, sizeof(field_name), "%s", key);
        else
            snprintf(field_name, sizeof(field_name), "atom_%d", up_id);

        /* key-extraction: pull named field out of a JSON object */
        if (key && *key && data[0] == '{') {
            char search[80];
            snprintf(search, sizeof(search), "\"%s\":", key);
            const char *found = strstr(data, search);
            if (found) {
                found += strlen(search);
                while (*found == ' ' || *found == '\t') found++;
                int wrote = snprintf(buf + pos, cap - pos, "\"%s\":", field_name);
                pos += (size_t)wrote;
                int  depth  = 0;
                bool in_str = false;
                while (*found && pos < cap - 2) {
                    char c = *found;
                    if (!in_str) {
                        if      (c == '{' || c == '[') depth++;
                        else if ((c == '}' || c == ']') && depth == 0) break;
                        else if ((c == '}' || c == ']'))                depth--;
                        else if ( c == ','              && depth == 0)  break;
                    }
                    if (c == '"' && (found == data || *(found - 1) != '\\'))
                        in_str = !in_str;
                    buf[pos++] = c;
                    found++;
                }
                continue;
            }
        }

        /* default: embed full result */
        int wrote = snprintf(buf + pos, cap - pos, "\"%s\":", field_name);
        pos += (size_t)wrote;
        if (data[0] == '{' || data[0] == '[' || data[0] == '"'
            || strcmp(data, "null") == 0) {
            size_t dlen = strlen(data);
            if (pos + dlen < cap - 2) { memcpy(buf + pos, data, dlen); pos += dlen; }
        } else {
            buf[pos++] = '"';
            for (const char *p = data; *p && pos < cap - 4; p++) {
                if (*p == '"' || *p == '\\') buf[pos++] = '\\';
                buf[pos++] = *p;
            }
            buf[pos++] = '"';
        }
    }

    if (pos < cap - 1) buf[pos++] = '}';
    buf[pos] = '\0';
    return buf;
}

bool execute_atom_with_input(plan_state_t *state, int atom_id,
                             char *result_buf, size_t rlen) {
    if (!state || !result_buf || rlen == 0) return false;

    atom_t *a = atom_get(atom_id);
    if (!a) return false;

    /*
     * Inject state-stored upstream outputs into each upstream atom's ->result
     * field so that atom_run() / atom_resolve_inputs() sees the state data
     * rather than whatever was left from a previous (possibly rolled-back) run.
     */
    for (int i = 0; i < a->input_from_count; i++) {
        int         up_id  = a->input_from_ids[i];
        const char *stored = plan_state_get_output(state, up_id);
        atom_set_result(up_id, stored);   /* NULL clears it */
    }

    /*
     * If the atom has upstream wires, build the merged input from state and
     * override atom->input_json so the tool/shell receives the pipeline data
     * even if it was explicitly set to something else.
     */
    if (a->input_from_count > 0) {
        char *merged = build_input_from_state(state, a);
        if (merged) {
            free(a->wired_input);
            a->wired_input = merged;
            /* Override input_json so ATOM_TOOL_CALL and ATOM_SHELL see it */
            free(a->input_json);
            a->input_json = safe_strdup(merged);
        }
    }

    bool ok = atom_run(atom_id, result_buf, rlen);

    /* Record output in state regardless of success/failure so rollback can
     * undo a partial execution. */
    plan_state_set_output(state, atom_id, result_buf[0] ? result_buf : NULL);

    /* Append to execution order (guard against overflow) */
    if (state->executed_count < PLAN_STATE_MAX_ATOMS)
        state->exec_order[state->executed_count++] = atom_id;

    return ok;
}

/* ── plan_state_run_all ──────────────────────────────────────────────────── */

/*
 * Check whether all upstream atoms for atom_id have their outputs in state.
 * Used to determine if an atom is ready to execute.
 */
static bool upstream_done(const plan_state_t *state, int atom_id) {
    const atom_t *a = atom_get(atom_id);
    if (!a) return false;
    for (int i = 0; i < a->input_from_count; i++) {
        if (!plan_state_get_output(state, a->input_from_ids[i]))
            return false;
    }
    return true;
}

int plan_state_run_all(plan_state_t *state) {
    if (!state) return 0;

    /* Collect all pending atoms for this plan */
    int pending[PLAN_STATE_MAX_ATOMS];
    int npending = plan_ready_atoms(state->plan_id, pending, PLAN_STATE_MAX_ATOMS);

    int executed = 0;
    bool progress = true;

    while (progress) {
        progress = false;

        /* Refresh ready list each iteration (dependencies change as atoms complete) */
        npending = plan_ready_atoms(state->plan_id, pending, PLAN_STATE_MAX_ATOMS);

        for (int i = 0; i < npending; i++) {
            int aid = pending[i];
            atom_t *a = atom_get(aid);
            if (!a || a->status != PLAN_PENDING) continue;

            /* Skip if upstream data not yet available in state */
            if (!upstream_done(state, aid)) continue;

            char buf[4096];
            execute_atom_with_input(state, aid, buf, sizeof buf);
            executed++;
            progress = true;
        }
    }

    return executed;
}
