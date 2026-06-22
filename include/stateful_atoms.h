#ifndef DSCO_STATEFUL_ATOMS_H
#define DSCO_STATEFUL_ATOMS_H

/*
 * stateful_atoms.h — Priority 2: Stateful Atom execution engine
 *
 * plan_state_t tracks each atom's output for one pipeline run, independent of
 * the global atom pool's ->result fields.  This enables:
 *   - Direct inter-atom data routing without coordinator bottleneck
 *   - Rollback: undo the last N executed atoms
 *   - Safe concurrent runs (each run owns its own state)
 *
 * Workflow:
 *   plan_state_t *st = plan_state_init(plan_id);
 *
 *   atom_wire(a1_id, a3_id, NULL);   // a3 reads a1's full output
 *   atom_wire(a2_id, a3_id, NULL);   // a3 also reads a2's full output
 *
 *   execute_atom_with_input(st, a1_id, buf, sizeof buf);
 *   execute_atom_with_input(st, a2_id, buf, sizeof buf);
 *   execute_atom_with_input(st, a3_id, buf, sizeof buf);
 *   // a3 receives merged JSON of a1 + a2 outputs
 *
 *   plan_state_rollback(st, 1);      // undo a3
 *   plan_state_free(st);
 */

#include "plan.h"
#include <stdbool.h>
#include <stddef.h>

/* Maximum atoms tracked per plan_state_t */
#define PLAN_STATE_MAX_ATOMS   256
/* Maximum rollback checkpoint depth */
#define PLAN_STATE_MAX_HISTORY  64

/* Output record for one atom within a run */
typedef struct {
    int   atom_id;
    char *output_json;   /* heap; NULL = not yet executed */
} atom_output_t;

/* Saved execution position for rollback */
typedef struct {
    int executed_count;
} plan_state_checkpoint_t;

/* Per-run execution state */
typedef struct {
    int plan_id;

    atom_output_t           outputs[PLAN_STATE_MAX_ATOMS];
    int                     output_count;

    int                     exec_order[PLAN_STATE_MAX_ATOMS];
    int                     executed_count;

    plan_state_checkpoint_t history[PLAN_STATE_MAX_HISTORY];
    int                     history_count;
} plan_state_t;

/* Allocate and initialise a fresh state for plan_id. */
plan_state_t *plan_state_init(int plan_id);

/* Release all memory owned by state and the state object itself. */
void          plan_state_free(plan_state_t *state);

/* Return the stored output JSON for atom_id, or NULL if not yet executed. */
const char   *plan_state_get_output(const plan_state_t *state, int atom_id);

/* Store (or overwrite) output_json for atom_id.  Copies the string. */
bool          plan_state_set_output(plan_state_t *state, int atom_id,
                                    const char *output_json);

/* Save a rollback checkpoint at the current execution position. */
bool          plan_state_checkpoint(plan_state_t *state);

/* Undo the last `steps` atom executions:
 *   - Clears each atom's result and wired_input in the atom pool
 *   - Resets each atom's status to PLAN_PENDING
 *   - Removes the outputs from this state
 * Returns the number of atoms actually rolled back (<= steps). */
int           plan_state_rollback(plan_state_t *state, int steps);

/* Execute atom_id, routing upstream outputs from state into its input.
 *
 * For each atom listed in atom->input_from_ids:
 *   1. Look up its output in state (not atom->result)
 *   2. Inject it into the upstream atom's ->result so atom_resolve_inputs()
 *      can build the merged input JSON
 * Then calls atom_run(), stores result in state on success.
 *
 * result_buf / rlen: forwarded to atom_run(); receives the raw result string.
 * Returns true on success. */
bool          execute_atom_with_input(plan_state_t *state, int atom_id,
                                      char *result_buf, size_t rlen);

/* Run all pending atoms in the plan in topological order until no more are
 * ready.  Returns the count of atoms successfully executed. */
int           plan_state_run_all(plan_state_t *state);

#endif /* DSCO_STATEFUL_ATOMS_H */
