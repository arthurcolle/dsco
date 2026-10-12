#ifndef DSCO_PLAN_DAG_H
#define DSCO_PLAN_DAG_H

#include <stdbool.h>
#include <stddef.h>

/* Validation and deterministic ready-frontier helpers for the existing
 * plan_t -> step_t -> atom_t planning engine. Hierarchy and execution
 * dependencies are validated independently: a parent edge does not imply a
 * dependency edge. */

typedef enum {
    PLAN_GRAPH_OK = 0,
    PLAN_GRAPH_MISSING_PARENT,
    PLAN_GRAPH_MISSING_DEP,
    PLAN_GRAPH_CROSS_PLAN_EDGE,
    PLAN_GRAPH_PARENT_CYCLE,
    PLAN_GRAPH_DEP_CYCLE,
    PLAN_GRAPH_CAPACITY,
} plan_graph_error_t;

typedef struct {
    plan_graph_error_t code;
    int node_id;
    int related_id;
    char message[256];
} plan_graph_diagnostic_t;

const char *plan_graph_error_name(plan_graph_error_t code);

/* Validate parent lineage and step dependency edges for one plan. */
bool plan_graph_validate(int plan_id, plan_graph_diagnostic_t *diag);

/* Return the current deterministic ready frontier. Steps are ordered by
 * descending priority then ascending numeric ID. Atoms are ordered by their
 * owning step's priority then ascending atom ID. These functions do not mutate
 * plan state. */
int plan_step_ready_wave(int plan_id, int *step_ids, int max_ids);
int plan_atom_ready_wave(int plan_id, int *atom_ids, int max_ids);

#endif /* DSCO_PLAN_DAG_H */
