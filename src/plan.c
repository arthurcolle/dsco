/*
 * plan.c — hierarchical planning engine
 *
 * Three-level tree:  plan → step (nestable) → atom (leaf work unit)
 * Plus a dialog pool for user clarification questions.
 *
 * All storage is process-local.  Variable-length strings are heap-allocated;
 * everything else lives in fixed-size static arrays.
 */

#include "plan.h"
#include "json_util.h"
#include "tools.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <time.h>

/* ── Static engine pools ─────────────────────────────────────────────────── */

static plan_t   s_plans[PLAN_MAX];
static step_t   s_steps[STEP_MAX];
static atom_t   s_atoms[ATOM_MAX];
static dialog_t s_dialogs[DIALOG_MAX];

static int s_next_plan_id   = 1;
static int s_next_step_id   = 1;
static int s_next_atom_id   = 1;
static int s_next_dialog_id = 1;

/* ── Engine lifecycle ────────────────────────────────────────────────────── */

void plan_engine_init(void) {
    /* free any previously-allocated strings before zeroing */
    for (int i = 0; i < PLAN_MAX; i++) {
        if (s_plans[i].active) {
            free(s_plans[i].title);
            free(s_plans[i].goal);
        }
    }
    for (int i = 0; i < STEP_MAX; i++) {
        if (s_steps[i].active) {
            free(s_steps[i].title);
            free(s_steps[i].desc);
        }
    }
    for (int i = 0; i < ATOM_MAX; i++) {
        if (s_atoms[i].active) {
            free(s_atoms[i].title);
            free(s_atoms[i].tool_name);
            free(s_atoms[i].input_json);
            free(s_atoms[i].result);
            free(s_atoms[i].prompt);
            free(s_atoms[i].response);
            free(s_atoms[i].condition);
        }
    }
    for (int i = 0; i < DIALOG_MAX; i++) {
        if (s_dialogs[i].active) {
            free(s_dialogs[i].prompt);
            free(s_dialogs[i].response);
        }
    }
    memset(s_plans,   0, sizeof(s_plans));
    memset(s_steps,   0, sizeof(s_steps));
    memset(s_atoms,   0, sizeof(s_atoms));
    memset(s_dialogs, 0, sizeof(s_dialogs));
    s_next_plan_id   = 1;
    s_next_step_id   = 1;
    s_next_atom_id   = 1;
    s_next_dialog_id = 1;
}

/* ── Internal slot allocators ────────────────────────────────────────────── */

static plan_t *plan_alloc(void) {
    for (int i = 0; i < PLAN_MAX; i++) {
        if (!s_plans[i].active) {
            memset(&s_plans[i], 0, sizeof(s_plans[i]));
            s_plans[i].active = true;
            s_plans[i].id     = s_next_plan_id++;
            return &s_plans[i];
        }
    }
    return NULL;
}

static step_t *step_alloc(void) {
    for (int i = 0; i < STEP_MAX; i++) {
        if (!s_steps[i].active) {
            memset(&s_steps[i], 0, sizeof(s_steps[i]));
            s_steps[i].active = true;
            s_steps[i].id     = s_next_step_id++;
            return &s_steps[i];
        }
    }
    return NULL;
}

static atom_t *atom_alloc(void) {
    for (int i = 0; i < ATOM_MAX; i++) {
        if (!s_atoms[i].active) {
            memset(&s_atoms[i], 0, sizeof(s_atoms[i]));
            s_atoms[i].active = true;
            s_atoms[i].id     = s_next_atom_id++;
            return &s_atoms[i];
        }
    }
    return NULL;
}

static dialog_t *dialog_alloc(void) {
    for (int i = 0; i < DIALOG_MAX; i++) {
        if (!s_dialogs[i].active) {
            memset(&s_dialogs[i], 0, sizeof(s_dialogs[i]));
            s_dialogs[i].active = true;
            s_dialogs[i].id     = s_next_dialog_id++;
            return &s_dialogs[i];
        }
    }
    return NULL;
}

/* ── Internal finders ────────────────────────────────────────────────────── */

static plan_t *plan_find(int id) {
    for (int i = 0; i < PLAN_MAX; i++)
        if (s_plans[i].active && s_plans[i].id == id) return &s_plans[i];
    return NULL;
}

static step_t *step_find(int id) {
    for (int i = 0; i < STEP_MAX; i++)
        if (s_steps[i].active && s_steps[i].id == id) return &s_steps[i];
    return NULL;
}

static atom_t *atom_find(int id) {
    for (int i = 0; i < ATOM_MAX; i++)
        if (s_atoms[i].active && s_atoms[i].id == id) return &s_atoms[i];
    return NULL;
}

static dialog_t *dialog_find(int id) {
    for (int i = 0; i < DIALOG_MAX; i++)
        if (s_dialogs[i].active && s_dialogs[i].id == id) return &s_dialogs[i];
    return NULL;
}

/* ── Name helpers ────────────────────────────────────────────────────────── */

const char *plan_status_name(plan_status_t s) {
    switch (s) {
        case PLAN_PENDING:     return "pending";
        case PLAN_IN_PROGRESS: return "in_progress";
        case PLAN_BLOCKED:     return "blocked";
        case PLAN_DONE:        return "done";
        case PLAN_FAILED:      return "failed";
        case PLAN_SKIPPED:     return "skipped";
        case PLAN_CANCELLED:   return "cancelled";
        default:               return "unknown";
    }
}

const char *plan_mode_name(plan_mode_t m) {
    switch (m) {
        case PLAN_MODE_TOP_DOWN:  return "top_down";
        case PLAN_MODE_BOTTOM_UP: return "bottom_up";
        case PLAN_MODE_HYBRID:    return "hybrid";
        default:                  return "unknown";
    }
}

const char *step_type_name(step_type_t t) {
    switch (t) {
        case STEP_COMPOSITE: return "composite";
        case STEP_ATOMIC:    return "atomic";
        case STEP_GATE:      return "gate";
        case STEP_DIALOG:    return "dialog";
        case STEP_MILESTONE: return "milestone";
        default:             return "unknown";
    }
}

const char *atom_type_name(atom_type_t t) {
    switch (t) {
        case ATOM_TOOL_CALL: return "tool_call";
        case ATOM_SHELL:     return "shell";
        case ATOM_DIALOG:    return "dialog";
        case ATOM_ASSERT:    return "assert";
        case ATOM_NOOP:      return "noop";
        default:             return "unknown";
    }
}

plan_mode_t plan_mode_parse(const char *s) {
    if (!s) return PLAN_MODE_HYBRID;
    if (strcmp(s, "top_down") == 0)  return PLAN_MODE_TOP_DOWN;
    if (strcmp(s, "bottom_up") == 0) return PLAN_MODE_BOTTOM_UP;
    return PLAN_MODE_HYBRID;
}

step_type_t step_type_parse(const char *s) {
    if (!s) return STEP_COMPOSITE;
    if (strcmp(s, "atomic")    == 0) return STEP_ATOMIC;
    if (strcmp(s, "gate")      == 0) return STEP_GATE;
    if (strcmp(s, "dialog")    == 0) return STEP_DIALOG;
    if (strcmp(s, "milestone") == 0) return STEP_MILESTONE;
    return STEP_COMPOSITE;
}

atom_type_t atom_type_parse(const char *s) {
    if (!s) return ATOM_TOOL_CALL;
    if (strcmp(s, "shell")     == 0) return ATOM_SHELL;
    if (strcmp(s, "dialog")    == 0) return ATOM_DIALOG;
    if (strcmp(s, "assert")    == 0) return ATOM_ASSERT;
    if (strcmp(s, "noop")      == 0) return ATOM_NOOP;
    return ATOM_TOOL_CALL;
}

plan_status_t plan_status_parse(const char *s) {
    if (!s) return PLAN_PENDING;
    if (strcmp(s, "in_progress") == 0) return PLAN_IN_PROGRESS;
    if (strcmp(s, "blocked")     == 0) return PLAN_BLOCKED;
    if (strcmp(s, "done")        == 0) return PLAN_DONE;
    if (strcmp(s, "failed")      == 0) return PLAN_FAILED;
    if (strcmp(s, "skipped")     == 0) return PLAN_SKIPPED;
    if (strcmp(s, "cancelled")   == 0) return PLAN_CANCELLED;
    return PLAN_PENDING;
}

/* Status glyph for tree rendering */
static const char *status_glyph(plan_status_t s) {
    switch (s) {
        case PLAN_PENDING:     return "○";
        case PLAN_IN_PROGRESS: return "◎";
        case PLAN_BLOCKED:     return "⊘";
        case PLAN_DONE:        return "●";
        case PLAN_FAILED:      return "✗";
        case PLAN_SKIPPED:     return "⊙";
        case PLAN_CANCELLED:   return "⊗";
        default:               return "?";
    }
}

/* ── Plan CRUD ───────────────────────────────────────────────────────────── */

int plan_create(const char *title, const char *goal, plan_mode_t mode) {
    plan_t *p = plan_alloc();
    if (!p) return -1;
    p->title  = safe_strdup(title ? title : "untitled");
    p->goal   = safe_strdup(goal  ? goal  : "");
    p->mode   = mode;
    p->status = PLAN_PENDING;
    return p->id;
}

bool plan_delete(int plan_id) {
    plan_t *p = plan_find(plan_id);
    if (!p) return false;
    free(p->title); p->title = NULL;
    free(p->goal);  p->goal  = NULL;

    /* deactivate owned steps + atoms */
    for (int i = 0; i < STEP_MAX; i++) {
        if (!s_steps[i].active || s_steps[i].plan_id != plan_id) continue;
        for (int j = 0; j < s_steps[i].atom_count; j++) {
            atom_t *a = atom_find(s_steps[i].atom_ids[j]);
            if (!a) continue;
            free(a->title);      a->title      = NULL;
            free(a->tool_name);  a->tool_name  = NULL;
            free(a->input_json); a->input_json = NULL;
            free(a->result);     a->result     = NULL;
            free(a->prompt);     a->prompt     = NULL;
            free(a->response);   a->response   = NULL;
            free(a->condition);  a->condition  = NULL;
            a->active = false;
        }
        free(s_steps[i].title); s_steps[i].title = NULL;
        free(s_steps[i].desc);  s_steps[i].desc  = NULL;
        s_steps[i].active = false;
    }

    /* deactivate dialogs */
    for (int i = 0; i < DIALOG_MAX; i++) {
        if (!s_dialogs[i].active || s_dialogs[i].plan_id != plan_id) continue;
        free(s_dialogs[i].prompt);   s_dialogs[i].prompt   = NULL;
        free(s_dialogs[i].response); s_dialogs[i].response = NULL;
        s_dialogs[i].active = false;
    }

    p->active = false;
    return true;
}

plan_t *plan_get(int plan_id) { return plan_find(plan_id); }

int plan_list(int *ids_out, int max_out) {
    int count = 0;
    for (int i = 0; i < PLAN_MAX && count < max_out; i++)
        if (s_plans[i].active) ids_out[count++] = s_plans[i].id;
    return count;
}

bool plan_set_description(int plan_id, const char *goal) {
    plan_t *p = plan_find(plan_id);
    if (!p) return false;
    free(p->goal);
    p->goal = safe_strdup(goal ? goal : "");
    return true;
}

/* ── Step management ─────────────────────────────────────────────────────── */

int plan_add_step(int plan_id, int parent_step_id,
                  const char *title, step_type_t type) {
    plan_t *p = plan_find(plan_id);
    if (!p) return -1;

    step_t *s = step_alloc();
    if (!s) return -1;

    s->plan_id        = plan_id;
    s->parent_step_id = parent_step_id;
    s->title          = safe_strdup(title ? title : "step");
    s->desc           = safe_strdup("");
    s->type           = type;
    s->status         = PLAN_PENDING;
    s->priority       = 0;

    if (parent_step_id == 0) {
        if (p->root_step_count < PLAN_MAX_ROOT_STEPS)
            p->root_step_ids[p->root_step_count++] = s->id;
    } else {
        step_t *parent = step_find(parent_step_id);
        if (parent && parent->child_step_count < STEP_MAX_CHILDREN)
            parent->child_step_ids[parent->child_step_count++] = s->id;
    }

    return s->id;
}

bool plan_remove_step(int plan_id, int step_id) {
    plan_t *p = plan_find(plan_id);
    step_t *s = step_find(step_id);
    if (!p || !s || s->plan_id != plan_id) return false;

    /* splice out of parent list */
    if (s->parent_step_id == 0) {
        for (int i = 0; i < p->root_step_count; i++) {
            if (p->root_step_ids[i] == step_id) {
                for (int j = i; j < p->root_step_count - 1; j++)
                    p->root_step_ids[j] = p->root_step_ids[j + 1];
                p->root_step_count--;
                break;
            }
        }
    } else {
        step_t *par = step_find(s->parent_step_id);
        if (par) {
            for (int i = 0; i < par->child_step_count; i++) {
                if (par->child_step_ids[i] == step_id) {
                    for (int j = i; j < par->child_step_count - 1; j++)
                        par->child_step_ids[j] = par->child_step_ids[j + 1];
                    par->child_step_count--;
                    break;
                }
            }
        }
    }

    /* deactivate atoms */
    for (int i = 0; i < s->atom_count; i++) {
        atom_t *a = atom_find(s->atom_ids[i]);
        if (!a) continue;
        free(a->title);      a->title      = NULL;
        free(a->tool_name);  a->tool_name  = NULL;
        free(a->input_json); a->input_json = NULL;
        free(a->result);     a->result     = NULL;
        free(a->prompt);     a->prompt     = NULL;
        free(a->response);   a->response   = NULL;
        free(a->condition);  a->condition  = NULL;
        a->active = false;
    }

    free(s->title); s->title = NULL;
    free(s->desc);  s->desc  = NULL;
    s->active = false;
    return true;
}

step_t *step_get(int step_id) { return step_find(step_id); }

bool step_set_title(int step_id, const char *title) {
    step_t *s = step_find(step_id);
    if (!s) return false;
    free(s->title);
    s->title = safe_strdup(title ? title : "step");
    return true;
}

bool step_set_description(int step_id, const char *desc) {
    step_t *s = step_find(step_id);
    if (!s) return false;
    free(s->desc);
    s->desc = safe_strdup(desc ? desc : "");
    return true;
}

bool step_set_priority(int step_id, int priority) {
    step_t *s = step_find(step_id);
    if (!s) return false;
    s->priority = priority;
    return true;
}

bool step_set_status(int step_id, plan_status_t status) {
    step_t *s = step_find(step_id);
    if (!s) return false;
    s->status = status;
    return true;
}

bool step_add_tag(int step_id, const char *tag) {
    step_t *s = step_find(step_id);
    if (!s || !tag || s->tag_count >= PLAN_TAG_MAX) return false;
    strncpy(s->tags[s->tag_count], tag, PLAN_TAG_LEN - 1);
    s->tags[s->tag_count][PLAN_TAG_LEN - 1] = '\0';
    s->tag_count++;
    return true;
}

bool step_add_note(int step_id, const char *note) {
    step_t *s = step_find(step_id);
    if (!s || !note || s->note_count >= PLAN_NOTE_MAX) return false;
    strncpy(s->notes[s->note_count], note, PLAN_NOTE_LEN - 1);
    s->notes[s->note_count][PLAN_NOTE_LEN - 1] = '\0';
    s->note_count++;
    return true;
}

bool step_add_dep(int step_id, int dep_step_id) {
    step_t *s = step_find(step_id);
    if (!s || s->dep_count >= STEP_MAX_DEPS) return false;
    for (int i = 0; i < s->dep_count; i++)
        if (s->dep_ids[i] == dep_step_id) return true;
    s->dep_ids[s->dep_count++] = dep_step_id;
    return true;
}

bool step_remove_dep(int step_id, int dep_step_id) {
    step_t *s = step_find(step_id);
    if (!s) return false;
    for (int i = 0; i < s->dep_count; i++) {
        if (s->dep_ids[i] == dep_step_id) {
            for (int j = i; j < s->dep_count - 1; j++)
                s->dep_ids[j] = s->dep_ids[j + 1];
            s->dep_count--;
            return true;
        }
    }
    return false;
}

bool step_deps_satisfied(int step_id) {
    step_t *s = step_find(step_id);
    if (!s) return false;
    for (int i = 0; i < s->dep_count; i++) {
        step_t *dep = step_find(s->dep_ids[i]);
        if (!dep || dep->status != PLAN_DONE) return false;
    }
    return true;
}

bool step_can_run(int step_id) {
    step_t *s = step_find(step_id);
    if (!s) return false;
    return (s->status == PLAN_PENDING) && step_deps_satisfied(step_id);
}

/* ── Atom management ─────────────────────────────────────────────────────── */

int step_add_atom(int step_id, const char *title, atom_type_t type) {
    step_t *s = step_find(step_id);
    if (!s || s->atom_count >= STEP_MAX_ATOMS) return -1;

    atom_t *a = atom_alloc();
    if (!a) return -1;

    a->step_id = step_id;
    a->title   = safe_strdup(title ? title : "atom");
    a->type    = type;
    a->status  = PLAN_PENDING;

    s->atom_ids[s->atom_count++] = a->id;
    return a->id;
}

bool atom_remove(int step_id, int atom_id) {
    step_t *s = step_find(step_id);
    atom_t *a = atom_find(atom_id);
    if (!s || !a || a->step_id != step_id) return false;

    for (int i = 0; i < s->atom_count; i++) {
        if (s->atom_ids[i] == atom_id) {
            for (int j = i; j < s->atom_count - 1; j++)
                s->atom_ids[j] = s->atom_ids[j + 1];
            s->atom_count--;
            break;
        }
    }

    free(a->title);      a->title      = NULL;
    free(a->tool_name);  a->tool_name  = NULL;
    free(a->input_json); a->input_json = NULL;
    free(a->result);     a->result     = NULL;
    free(a->prompt);     a->prompt     = NULL;
    free(a->response);   a->response   = NULL;
    free(a->condition);  a->condition  = NULL;
    a->active = false;
    return true;
}

atom_t *atom_get(int atom_id) { return atom_find(atom_id); }

bool atom_set_tool(int atom_id, const char *tool_name, const char *input_json) {
    atom_t *a = atom_find(atom_id);
    if (!a) return false;
    free(a->tool_name);  a->tool_name  = safe_strdup(tool_name  ? tool_name  : "");
    free(a->input_json); a->input_json = safe_strdup(input_json ? input_json : "{}");
    return true;
}

bool atom_set_shell(int atom_id, const char *command) {
    atom_t *a = atom_find(atom_id);
    if (!a) return false;
    free(a->input_json);
    a->input_json = safe_strdup(command ? command : "");
    return true;
}

bool atom_set_dialog_prompt(int atom_id, const char *prompt) {
    atom_t *a = atom_find(atom_id);
    if (!a) return false;
    free(a->prompt);
    a->prompt = safe_strdup(prompt ? prompt : "");
    return true;
}

bool atom_set_assert(int atom_id, const char *condition) {
    atom_t *a = atom_find(atom_id);
    if (!a) return false;
    free(a->condition);
    a->condition = safe_strdup(condition ? condition : "");
    return true;
}

bool atom_set_result(int atom_id, const char *result) {
    atom_t *a = atom_find(atom_id);
    if (!a) return false;
    free(a->result);
    a->result = result ? safe_strdup(result) : NULL;
    return true;
}

/* ── atom_run ────────────────────────────────────────────────────────────── */

bool atom_run(int atom_id, char *result_buf, size_t rlen) {
    atom_t *a = atom_find(atom_id);
    if (!a || !result_buf || rlen == 0) return false;

    result_buf[0] = '\0';
    a->status     = PLAN_IN_PROGRESS;

    bool ok = false;

    switch (a->type) {

    case ATOM_TOOL_CALL: {
        if (!a->tool_name || !a->tool_name[0]) {
            snprintf(result_buf, rlen, "error: atom %d has no tool_name", atom_id);
            a->status = PLAN_FAILED;
            break;
        }
        ok = tools_execute(a->tool_name,
                           a->input_json ? a->input_json : "{}",
                           result_buf, rlen);
        a->status = ok ? PLAN_DONE : PLAN_FAILED;
        break;
    }

    case ATOM_SHELL: {
        const char *cmd = a->input_json ? a->input_json : "";
        if (!cmd[0]) {
            snprintf(result_buf, rlen, "error: atom %d has no shell command", atom_id);
            a->status = PLAN_FAILED;
            break;
        }
        FILE *fp = popen(cmd, "r");
        if (!fp) {
            snprintf(result_buf, rlen, "error: popen failed");
            a->status = PLAN_FAILED;
            break;
        }
        size_t total = 0, chunk;
        while (total < rlen - 1 &&
               (chunk = fread(result_buf + total, 1, rlen - 1 - total, fp)) > 0)
            total += chunk;
        result_buf[total] = '\0';
        int rc = pclose(fp);
        a->exit_code = rc;
        ok = (rc == 0);
        a->status = ok ? PLAN_DONE : PLAN_FAILED;
        break;
    }

    case ATOM_DIALOG: {
        /* Record the prompt; caller must supply answer via atom_set_result */
        const char *p = a->prompt ? a->prompt : "(no prompt)";
        snprintf(result_buf, rlen, "dialog_pending: %s", p);
        a->status = PLAN_BLOCKED;
        ok = false;
        break;
    }

    case ATOM_ASSERT: {
        const char *cond = a->condition ? a->condition : "";
        bool passed = (cond[0] != '\0'
                       && strcmp(cond, "false") != 0
                       && strcmp(cond, "0")     != 0);
        snprintf(result_buf, rlen, "assert_%s: %s",
                 passed ? "passed" : "failed", cond);
        a->status = passed ? PLAN_DONE : PLAN_FAILED;
        ok = passed;
        break;
    }

    case ATOM_NOOP:
        snprintf(result_buf, rlen, "noop");
        a->status = PLAN_DONE;
        ok = true;
        break;

    default:
        snprintf(result_buf, rlen, "error: unknown atom type %d", (int)a->type);
        a->status = PLAN_FAILED;
        break;
    }

    free(a->result);
    a->result = safe_strdup(result_buf);
    return ok;
}

/* ── Execution helpers ───────────────────────────────────────────────────── */

static int collect_ready_atoms(int step_id,
                                int *out, int max_out, int count) {
    step_t *s = step_find(step_id);
    if (!s) return count;

    if (step_can_run(step_id)) {
        for (int i = 0; i < s->atom_count && count < max_out; i++) {
            atom_t *a = atom_find(s->atom_ids[i]);
            if (a && a->status == PLAN_PENDING)
                out[count++] = a->id;
        }
    }

    for (int i = 0; i < s->child_step_count && count < max_out; i++)
        count = collect_ready_atoms(s->child_step_ids[i], out, max_out, count);

    return count;
}

int plan_ready_atoms(int plan_id, int *atom_ids_out, int max_out) {
    plan_t *p = plan_find(plan_id);
    if (!p || !atom_ids_out || max_out <= 0) return 0;
    int count = 0;
    for (int i = 0; i < p->root_step_count && count < max_out; i++)
        count = collect_ready_atoms(p->root_step_ids[i], atom_ids_out, max_out, count);
    return count;
}

int plan_run_next(int plan_id) {
    int ids[1];
    if (plan_ready_atoms(plan_id, ids, 1) < 1) return -1;
    char buf[4096];
    atom_run(ids[0], buf, sizeof(buf));
    return ids[0];
}

/* ── Top-down decomposition ──────────────────────────────────────────────── */

int plan_decompose(int plan_id, int focus_step_id,
                   const char **subtitles, int count) {
    plan_t *p = plan_find(plan_id);
    if (!p || !subtitles || count <= 0) return 0;
    int added = 0;
    for (int i = 0; i < count; i++) {
        if (plan_add_step(plan_id, focus_step_id,
                          subtitles[i], STEP_COMPOSITE) >= 0)
            added++;
    }
    return added;
}

/* ── Bottom-up aggregation ───────────────────────────────────────────────── */

int plan_aggregate_atoms(int plan_id, int parent_step_id,
                         const char *step_title,
                         const int *atom_ids, int count) {
    if (!atom_ids || count <= 0) return -1;

    int sid = plan_add_step(plan_id, parent_step_id, step_title, STEP_ATOMIC);
    if (sid < 0) return -1;

    step_t *s = step_find(sid);
    if (!s) return -1;

    for (int i = 0; i < count; i++) {
        atom_t *a = atom_find(atom_ids[i]);
        if (!a || s->atom_count >= STEP_MAX_ATOMS) break;
        /* re-parent: remove from original step */
        step_t *orig = step_find(a->step_id);
        if (orig) {
            for (int j = 0; j < orig->atom_count; j++) {
                if (orig->atom_ids[j] == a->id) {
                    for (int k = j; k < orig->atom_count - 1; k++)
                        orig->atom_ids[k] = orig->atom_ids[k + 1];
                    orig->atom_count--;
                    break;
                }
            }
        }
        a->step_id = sid;
        s->atom_ids[s->atom_count++] = a->id;
    }

    return sid;
}

/* ── Dialog system ───────────────────────────────────────────────────────── */

int plan_dialog_ask(int plan_id, int step_id,
                    const char *prompt,
                    const char **choices, int choice_count) {
    plan_t *p = plan_find(plan_id);
    if (!p) return -1;

    dialog_t *d = dialog_alloc();
    if (!d) return -1;

    d->plan_id  = plan_id;
    d->step_id  = step_id;
    d->prompt   = safe_strdup(prompt ? prompt : "");
    d->chosen   = -1;
    d->answered = false;

    if (choices && choice_count > 0) {
        int n = choice_count < PLAN_CHOICE_MAX ? choice_count : PLAN_CHOICE_MAX;
        for (int i = 0; i < n; i++) {
            strncpy(d->choices[i], choices[i] ? choices[i] : "",
                    PLAN_CHOICE_LEN - 1);
            d->choices[i][PLAN_CHOICE_LEN - 1] = '\0';
        }
        d->choice_count = n;
    }

    return d->id;
}

bool plan_dialog_answer(int dialog_id, const char *response, int chosen) {
    dialog_t *d = dialog_find(dialog_id);
    if (!d || d->answered) return false;
    free(d->response);
    d->response = safe_strdup(response ? response : "");
    d->chosen   = chosen;
    d->answered = true;
    return true;
}

dialog_t *dialog_get(int dialog_id) { return dialog_find(dialog_id); }

int plan_pending_dialogs(int plan_id, int *dialog_ids_out, int max_out) {
    if (!dialog_ids_out || max_out <= 0) return 0;
    int count = 0;
    for (int i = 0; i < DIALOG_MAX && count < max_out; i++) {
        if (s_dialogs[i].active
            && s_dialogs[i].plan_id == plan_id
            && !s_dialogs[i].answered)
            dialog_ids_out[count++] = s_dialogs[i].id;
    }
    return count;
}

/* ── Serialisation ───────────────────────────────────────────────────────── */

static void atom_to_json(jbuf_t *b, atom_t *a) {
    jbuf_append(b, "{");
    jbuf_append(b, "\"id\":"); jbuf_append_int(b, a->id);
    jbuf_append(b, ",\"title\":"); jbuf_append_json_str(b, a->title ? a->title : "");
    jbuf_append(b, ",\"type\":"); jbuf_append_json_str(b, atom_type_name(a->type));
    jbuf_append(b, ",\"status\":"); jbuf_append_json_str(b, plan_status_name(a->status));
    if (a->tool_name && a->tool_name[0]) {
        jbuf_append(b, ",\"tool_name\":"); jbuf_append_json_str(b, a->tool_name);
    }
    if (a->result) {
        /* truncate result in JSON to avoid huge payloads */
        char preview[256];
        size_t rlen = strlen(a->result);
        if (rlen > 200) {
            memcpy(preview, a->result, 200);
            preview[200] = '\0';
        } else {
            memcpy(preview, a->result, rlen + 1);
        }
        jbuf_append(b, ",\"result\":"); jbuf_append_json_str(b, preview);
    }
    jbuf_append(b, "}");
}

static void step_to_json(jbuf_t *b, step_t *s, int depth);

static void step_to_json(jbuf_t *b, step_t *s, int depth) {
    if (depth > 8) { jbuf_append(b, "null"); return; }
    jbuf_append(b, "{");
    jbuf_append(b, "\"id\":"); jbuf_append_int(b, s->id);
    jbuf_append(b, ",\"title\":"); jbuf_append_json_str(b, s->title ? s->title : "");
    jbuf_append(b, ",\"type\":"); jbuf_append_json_str(b, step_type_name(s->type));
    jbuf_append(b, ",\"status\":"); jbuf_append_json_str(b, plan_status_name(s->status));
    jbuf_append(b, ",\"priority\":"); jbuf_append_int(b, s->priority);

    jbuf_append(b, ",\"deps\":[");
    for (int i = 0; i < s->dep_count; i++) {
        if (i > 0) jbuf_append(b, ",");
        jbuf_append_int(b, s->dep_ids[i]);
    }
    jbuf_append(b, "]");

    jbuf_append(b, ",\"atoms\":[");
    for (int i = 0; i < s->atom_count; i++) {
        atom_t *a = atom_find(s->atom_ids[i]);
        if (!a) continue;
        if (i > 0) jbuf_append(b, ",");
        atom_to_json(b, a);
    }
    jbuf_append(b, "]");

    jbuf_append(b, ",\"children\":[");
    for (int i = 0; i < s->child_step_count; i++) {
        step_t *child = step_find(s->child_step_ids[i]);
        if (!child) continue;
        if (i > 0) jbuf_append(b, ",");
        step_to_json(b, child, depth + 1);
    }
    jbuf_append(b, "]}");
}

int plan_to_json(int plan_id, char *buf, size_t len) {
    plan_t *p = plan_find(plan_id);
    if (!p || !buf || len == 0) return 0;

    jbuf_t b; jbuf_init(&b, 4096);
    jbuf_append(&b, "{");
    jbuf_append(&b, "\"id\":"); jbuf_append_int(&b, p->id);
    jbuf_append(&b, ",\"title\":"); jbuf_append_json_str(&b, p->title ? p->title : "");
    jbuf_append(&b, ",\"goal\":"); jbuf_append_json_str(&b, p->goal ? p->goal : "");
    jbuf_append(&b, ",\"mode\":"); jbuf_append_json_str(&b, plan_mode_name(p->mode));
    jbuf_append(&b, ",\"status\":"); jbuf_append_json_str(&b, plan_status_name(p->status));
    jbuf_append(&b, ",\"steps\":[");
    for (int i = 0; i < p->root_step_count; i++) {
        step_t *s = step_find(p->root_step_ids[i]);
        if (!s) continue;
        if (i > 0) jbuf_append(&b, ",");
        step_to_json(&b, s, 0);
    }
    jbuf_append(&b, "]}");

    int written = 0;
    if (b.data && b.len > 0) {
        size_t copy = b.len < len - 1 ? b.len : len - 1;
        memcpy(buf, b.data, copy);
        buf[copy] = '\0';
        written = (int)copy;
    }
    jbuf_free(&b);
    return written;
}

int plan_summary(int plan_id, char *buf, size_t len) {
    plan_t *p = plan_find(plan_id);
    if (!p || !buf || len == 0) return 0;

    int total = 0, done = 0, failed = 0, blocked = 0, in_prog = 0;
    for (int i = 0; i < STEP_MAX; i++) {
        if (!s_steps[i].active || s_steps[i].plan_id != plan_id) continue;
        total++;
        switch (s_steps[i].status) {
            case PLAN_DONE:        done++;    break;
            case PLAN_FAILED:      failed++;  break;
            case PLAN_BLOCKED:     blocked++; break;
            case PLAN_IN_PROGRESS: in_prog++; break;
            default: break;
        }
    }

    return snprintf(buf, len,
                    "plan#%d \"%s\" [%s/%s] "
                    "steps=%d done=%d in_progress=%d blocked=%d failed=%d",
                    p->id,
                    p->title ? p->title : "",
                    plan_mode_name(p->mode),
                    plan_status_name(p->status),
                    total, done, in_prog, blocked, failed);
}

/* ── plan_tree_render ────────────────────────────────────────────────────── */

static void render_step(step_t *s, jbuf_t *b,
                        const char *prefix, bool is_last, int depth) {
    if (depth > 10) return;

    char line[640];
    jbuf_append(b, prefix);
    jbuf_append(b, is_last ? "\xe2\x94\x94\xe2\x94\x80 " : "\xe2\x94\x9c\xe2\x94\x80 ");

    snprintf(line, sizeof(line), "%s [%s] %s",
             status_glyph(s->status),
             step_type_name(s->type),
             s->title ? s->title : "");
    jbuf_append(b, line);

    if (s->dep_count > 0) {
        jbuf_append(b, "  deps:");
        for (int i = 0; i < s->dep_count; i++) {
            char dep_s[16];
            snprintf(dep_s, sizeof(dep_s), " #%d", s->dep_ids[i]);
            jbuf_append(b, dep_s);
        }
    }
    jbuf_append(b, "\n");

    /* child prefix */
    char child_prefix[320];
    snprintf(child_prefix, sizeof(child_prefix), "%s%s",
             prefix, is_last ? "   " : "\xe2\x94\x82  ");

    /* atoms */
    for (int i = 0; i < s->atom_count; i++) {
        atom_t *a = atom_find(s->atom_ids[i]);
        if (!a) continue;
        bool last_leaf = (i == s->atom_count - 1) && (s->child_step_count == 0);
        jbuf_append(b, child_prefix);
        jbuf_append(b, last_leaf
                    ? "\xe2\x94\x94\xe2\x94\x80 "
                    : "\xe2\x94\x9c\xe2\x94\x80 ");
        snprintf(line, sizeof(line), "%s atom[%s/%s] %s\n",
                 status_glyph(a->status),
                 atom_type_name(a->type),
                 plan_status_name(a->status),
                 a->title ? a->title : "");
        jbuf_append(b, line);
    }

    /* child steps */
    for (int i = 0; i < s->child_step_count; i++) {
        step_t *child = step_find(s->child_step_ids[i]);
        if (!child) continue;
        bool last_child = (i == s->child_step_count - 1);
        render_step(child, b, child_prefix, last_child, depth + 1);
    }
}

int plan_tree_render(int plan_id, char *buf, size_t len) {
    plan_t *p = plan_find(plan_id);
    if (!p || !buf || len == 0) return 0;

    jbuf_t b; jbuf_init(&b, 4096);

    char header[256];
    snprintf(header, sizeof(header), "plan#%d  %s\n"
             "goal: %s\n"
             "mode: %s  status: %s\n",
             p->id,
             p->title ? p->title : "",
             (p->goal && p->goal[0]) ? p->goal : "(no goal set)",
             plan_mode_name(p->mode),
             plan_status_name(p->status));
    jbuf_append(&b, header);

    for (int i = 0; i < p->root_step_count; i++) {
        step_t *s = step_find(p->root_step_ids[i]);
        if (!s) continue;
        render_step(s, &b, "", (i == p->root_step_count - 1), 0);
    }

    int written = 0;
    if (b.data && b.len > 0) {
        size_t copy = b.len < len - 1 ? b.len : len - 1;
        memcpy(buf, b.data, copy);
        buf[copy] = '\0';
        written = (int)copy;
    }
    jbuf_free(&b);
    return written;
}
