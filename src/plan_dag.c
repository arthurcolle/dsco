#include "plan_dag.h"
#include "plan.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static void diag_set(plan_graph_diagnostic_t *diag, plan_graph_error_t code,
                     int node_id, int related_id, const char *fmt, ...) {
    if (!diag)
        return;
    memset(diag, 0, sizeof(*diag));
    diag->code = code;
    diag->node_id = node_id;
    diag->related_id = related_id;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(diag->message, sizeof(diag->message), fmt, ap);
    va_end(ap);
}

const char *plan_graph_error_name(plan_graph_error_t code) {
    switch (code) {
        case PLAN_GRAPH_OK: return "ok";
        case PLAN_GRAPH_MISSING_PARENT: return "missing_parent";
        case PLAN_GRAPH_MISSING_DEP: return "missing_dependency";
        case PLAN_GRAPH_CROSS_PLAN_EDGE: return "cross_plan_edge";
        case PLAN_GRAPH_PARENT_CYCLE: return "parent_cycle";
        case PLAN_GRAPH_DEP_CYCLE: return "dependency_cycle";
        case PLAN_GRAPH_CAPACITY: return "capacity";
        default: return "unknown";
    }
}

static int collect_plan_steps(int plan_id, int *ids, int max_ids) {
    int count = 0;
    for (int id = 1; id > 0 && count < max_ids; id++) {
        step_t *step = step_get(id);
        if (step && step->plan_id == plan_id)
            ids[count++] = id;
        /* IDs are monotonic and bounded by the fixed pool. Once the scan is
         * beyond the pool plus collected nodes, no later active ID can exist. */
        if (id > STEP_MAX + max_ids)
            break;
    }
    return count;
}

static int index_of(const int *ids, int count, int id) {
    for (int i = 0; i < count; i++)
        if (ids[i] == id)
            return i;
    return -1;
}

static bool validate_parent_chain(int plan_id, const int *ids, int count,
                                  int start_index, plan_graph_diagnostic_t *diag) {
    bool seen[STEP_MAX];
    memset(seen, 0, sizeof(seen));
    int current_index = start_index;
    while (current_index >= 0) {
        if (seen[current_index]) {
            int node_id = ids[start_index];
            diag_set(diag, PLAN_GRAPH_PARENT_CYCLE, node_id, ids[current_index],
                     "parent cycle includes step %d", ids[current_index]);
            return false;
        }
        seen[current_index] = true;
        step_t *step = step_get(ids[current_index]);
        if (!step)
            return false;
        if (step->parent_step_id == 0)
            return true;
        step_t *parent = step_get(step->parent_step_id);
        if (!parent) {
            diag_set(diag, PLAN_GRAPH_MISSING_PARENT, step->id,
                     step->parent_step_id, "step %d references missing parent %d",
                     step->id, step->parent_step_id);
            return false;
        }
        if (parent->plan_id != plan_id) {
            diag_set(diag, PLAN_GRAPH_CROSS_PLAN_EDGE, step->id,
                     parent->id, "step %d parent %d belongs to another plan",
                     step->id, parent->id);
            return false;
        }
        current_index = index_of(ids, count, parent->id);
        if (current_index < 0) {
            diag_set(diag, PLAN_GRAPH_MISSING_PARENT, step->id, parent->id,
                     "step %d parent %d is not present in plan %d",
                     step->id, parent->id, plan_id);
            return false;
        }
    }
    return true;
}

bool plan_graph_validate(int plan_id, plan_graph_diagnostic_t *diag) {
    if (diag)
        memset(diag, 0, sizeof(*diag));
    if (!plan_get(plan_id)) {
        diag_set(diag, PLAN_GRAPH_MISSING_PARENT, plan_id, 0,
                 "plan %d does not exist", plan_id);
        return false;
    }

    int ids[STEP_MAX];
    int count = collect_plan_steps(plan_id, ids, STEP_MAX);
    if (count >= STEP_MAX) {
        diag_set(diag, PLAN_GRAPH_CAPACITY, plan_id, 0,
                 "plan %d reached the step validation capacity", plan_id);
        return false;
    }

    for (int i = 0; i < count; i++) {
        if (!validate_parent_chain(plan_id, ids, count, i, diag))
            return false;
    }

    int indegree[STEP_MAX];
    bool removed[STEP_MAX];
    memset(indegree, 0, sizeof(indegree));
    memset(removed, 0, sizeof(removed));

    for (int i = 0; i < count; i++) {
        step_t *step = step_get(ids[i]);
        if (!step)
            continue;
        for (int j = 0; j < step->dep_count; j++) {
            step_t *dep = step_get(step->dep_ids[j]);
            if (!dep) {
                diag_set(diag, PLAN_GRAPH_MISSING_DEP, step->id,
                         step->dep_ids[j], "step %d references missing dependency %d",
                         step->id, step->dep_ids[j]);
                return false;
            }
            if (dep->plan_id != plan_id) {
                diag_set(diag, PLAN_GRAPH_CROSS_PLAN_EDGE, step->id, dep->id,
                         "step %d dependency %d belongs to another plan",
                         step->id, dep->id);
                return false;
            }
            if (index_of(ids, count, dep->id) < 0) {
                diag_set(diag, PLAN_GRAPH_MISSING_DEP, step->id, dep->id,
                         "step %d dependency %d is not present in plan %d",
                         step->id, dep->id, plan_id);
                return false;
            }
            indegree[i]++;
        }
    }

    int removed_count = 0;
    while (removed_count < count) {
        int wave[STEP_MAX];
        int wave_count = 0;
        for (int i = 0; i < count; i++)
            if (!removed[i] && indegree[i] == 0)
                wave[wave_count++] = i;
        if (wave_count == 0) {
            int node_id = 0;
            for (int i = 0; i < count; i++)
                if (!removed[i]) { node_id = ids[i]; break; }
            diag_set(diag, PLAN_GRAPH_DEP_CYCLE, node_id, 0,
                     "step dependency cycle includes step %d", node_id);
            return false;
        }
        for (int w = 0; w < wave_count; w++) {
            int done_index = wave[w];
            int done_id = ids[done_index];
            removed[done_index] = true;
            removed_count++;
            for (int i = 0; i < count; i++) {
                if (removed[i])
                    continue;
                step_t *step = step_get(ids[i]);
                if (!step)
                    continue;
                for (int j = 0; j < step->dep_count; j++)
                    if (step->dep_ids[j] == done_id)
                        indegree[i]--;
            }
        }
    }

    diag_set(diag, PLAN_GRAPH_OK, 0, 0, "ok");
    return true;
}

static bool parent_eligible(const step_t *step) {
    int parent_id = step ? step->parent_step_id : 0;
    while (parent_id != 0) {
        step_t *parent = step_get(parent_id);
        if (!parent)
            return false;
        if (parent->status == PLAN_FAILED || parent->status == PLAN_BLOCKED ||
            parent->status == PLAN_CANCELLED || parent->status == PLAN_SKIPPED ||
            parent->status == PLAN_DONE)
            return false;
        parent_id = parent->parent_step_id;
    }
    return true;
}

static void sort_steps(int *ids, int count) {
    for (int i = 1; i < count; i++) {
        int value = ids[i];
        step_t *value_step = step_get(value);
        int j = i - 1;
        while (j >= 0) {
            step_t *left = step_get(ids[j]);
            bool move = left && value_step &&
                        (left->priority < value_step->priority ||
                         (left->priority == value_step->priority && ids[j] > value));
            if (!move)
                break;
            ids[j + 1] = ids[j];
            j--;
        }
        ids[j + 1] = value;
    }
}

int plan_step_ready_wave(int plan_id, int *step_ids, int max_ids) {
    if (!step_ids || max_ids <= 0 || !plan_get(plan_id))
        return 0;
    int all[STEP_MAX];
    int all_count = collect_plan_steps(plan_id, all, STEP_MAX);
    int count = 0;
    for (int i = 0; i < all_count && count < max_ids; i++) {
        step_t *step = step_get(all[i]);
        if (step && step_can_run(step->id) && parent_eligible(step))
            step_ids[count++] = step->id;
    }
    sort_steps(step_ids, count);
    return count;
}

static int step_priority_for_atom(int atom_id) {
    atom_t *atom = atom_get(atom_id);
    step_t *step = atom ? step_get(atom->step_id) : NULL;
    return step ? step->priority : 0;
}

static void sort_atoms(int *ids, int count) {
    for (int i = 1; i < count; i++) {
        int value = ids[i];
        int value_priority = step_priority_for_atom(value);
        int j = i - 1;
        while (j >= 0) {
            int left_priority = step_priority_for_atom(ids[j]);
            bool move = left_priority < value_priority ||
                        (left_priority == value_priority && ids[j] > value);
            if (!move)
                break;
            ids[j + 1] = ids[j];
            j--;
        }
        ids[j + 1] = value;
    }
}

int plan_atom_ready_wave(int plan_id, int *atom_ids, int max_ids) {
    if (!atom_ids || max_ids <= 0 || !plan_get(plan_id))
        return 0;
    int candidates[ATOM_MAX];
    int candidate_count = plan_ready_atoms(plan_id, candidates, ATOM_MAX);
    int count = candidate_count < max_ids ? candidate_count : max_ids;
    for (int i = 0; i < count; i++)
        atom_ids[i] = candidates[i];
    sort_atoms(atom_ids, count);
    return count;
}
