#ifndef DSCO_PLAN_H
#define DSCO_PLAN_H

/*
 * plan.h — hierarchical top-down / bottom-up planning engine
 *
 * Three-level tree:
 *   plan_t               (goal + metadata)
 *     └─ step_t[]        (phases / milestones; may nest)
 *          └─ atom_t[]   (atomic units: tool_call / shell / dialog / assert / noop)
 *
 * Plus dialog_t for user clarification dialogs.
 *
 * All IDs are small positive ints allocated from static pools.
 */

#include <stdbool.h>
#include <stddef.h>

/* ── Capacities ──────────────────────────────────────────────────────────── */

#define PLAN_MAX             64
#define STEP_MAX            512
#define ATOM_MAX            512
#define DIALOG_MAX          256

#define PLAN_MAX_ROOT_STEPS  32   /* ordered root steps per plan */
#define STEP_MAX_CHILDREN    32   /* child steps per step */
#define STEP_MAX_DEPS        16   /* dependency step ids per step */
#define STEP_MAX_ATOMS       64   /* atoms per step */

#define PLAN_TAG_MAX         16
#define PLAN_TAG_LEN         48
#define PLAN_NOTE_MAX         8
#define PLAN_NOTE_LEN       256
#define PLAN_CHOICE_MAX      16
#define PLAN_CHOICE_LEN     128

/* ── Status ──────────────────────────────────────────────────────────────── */

typedef enum {
    PLAN_PENDING = 0,
    PLAN_IN_PROGRESS,
    PLAN_BLOCKED,
    PLAN_DONE,
    PLAN_FAILED,
    PLAN_SKIPPED,
    PLAN_CANCELLED,
} plan_status_t;

/* ── Mode ────────────────────────────────────────────────────────────────── */

typedef enum {
    PLAN_MODE_TOP_DOWN = 0,   /* decompose goal → steps → atoms */
    PLAN_MODE_BOTTOM_UP,      /* aggregate atoms → steps → goal */
    PLAN_MODE_HYBRID,         /* mixed; default */
} plan_mode_t;

/* ── Step type ───────────────────────────────────────────────────────────── */

typedef enum {
    STEP_COMPOSITE = 0,   /* container of child steps */
    STEP_ATOMIC,          /* leaf; contains atoms */
    STEP_GATE,            /* decision / branching */
    STEP_DIALOG,          /* requires user input */
    STEP_MILESTONE,       /* named checkpoint; no execution */
} step_type_t;

/* ── Atom type ───────────────────────────────────────────────────────────── */

typedef enum {
    ATOM_TOOL_CALL = 0,
    ATOM_SHELL,
    ATOM_DIALOG,
    ATOM_ASSERT,
    ATOM_NOOP,
} atom_type_t;

/* ── Data structures ─────────────────────────────────────────────────────── */

typedef struct {
    int           id;
    bool          active;
    char         *title;              /* heap */
    char         *goal;               /* heap */
    plan_mode_t   mode;
    plan_status_t status;
    int           root_step_ids[PLAN_MAX_ROOT_STEPS];
    int           root_step_count;
} plan_t;

typedef struct {
    int           id;
    bool          active;
    int           plan_id;
    int           parent_step_id;     /* 0 = root step */
    char         *title;              /* heap */
    char         *desc;               /* heap */
    step_type_t   type;
    plan_status_t status;
    int           child_step_ids[STEP_MAX_CHILDREN];
    int           child_step_count;
    int           atom_ids[STEP_MAX_ATOMS];
    int           atom_count;
    int           dep_ids[STEP_MAX_DEPS];
    int           dep_count;
    int           priority;
    char          tags[PLAN_TAG_MAX][PLAN_TAG_LEN];
    int           tag_count;
    char          notes[PLAN_NOTE_MAX][PLAN_NOTE_LEN];
    int           note_count;
} step_t;

typedef struct {
    int           id;
    bool          active;
    int           step_id;
    char         *title;              /* heap */
    atom_type_t   type;
    plan_status_t status;
    int           exit_code;
    /* TOOL_CALL */
    char         *tool_name;          /* heap */
    char         *input_json;         /* heap; also used as shell command */
    char         *result;             /* heap; filled after atom_run */
    /* DIALOG */
    char         *prompt;             /* heap */
    char         *response;           /* heap */
    /* ASSERT */
    char         *condition;          /* heap */
} atom_t;

typedef struct {
    int   id;
    bool  active;
    int   plan_id;
    int   step_id;                    /* -1 = plan-level */
    char *prompt;                     /* heap */
    char  choices[PLAN_CHOICE_MAX][PLAN_CHOICE_LEN];
    int   choice_count;
    char *response;                   /* heap; set when answered */
    int   chosen;                     /* index into choices[], -1 = freeform */
    bool  answered;
} dialog_t;

/* ── Engine lifecycle ────────────────────────────────────────────────────── */

void plan_engine_init(void);

/* ── Plan CRUD ───────────────────────────────────────────────────────────── */

int     plan_create(const char *title, const char *goal, plan_mode_t mode);
bool    plan_delete(int plan_id);
plan_t *plan_get(int plan_id);
int     plan_list(int *ids_out, int max_out);
bool    plan_set_description(int plan_id, const char *goal);

/* ── Step management ─────────────────────────────────────────────────────── */

/* parent_step_id = 0 → root step of the plan */
int     plan_add_step(int plan_id, int parent_step_id,
                      const char *title, step_type_t type);
bool    plan_remove_step(int plan_id, int step_id);
step_t *step_get(int step_id);

bool    step_set_title(int step_id, const char *title);
bool    step_set_description(int step_id, const char *desc);
bool    step_set_priority(int step_id, int priority);
bool    step_set_status(int step_id, plan_status_t status);
bool    step_add_tag(int step_id, const char *tag);
bool    step_add_note(int step_id, const char *note);

bool    step_add_dep(int step_id, int dep_step_id);
bool    step_remove_dep(int step_id, int dep_step_id);
bool    step_deps_satisfied(int step_id);
bool    step_can_run(int step_id);

/* ── Atom management ─────────────────────────────────────────────────────── */

int     step_add_atom(int step_id, const char *title, atom_type_t type);
bool    atom_remove(int step_id, int atom_id);
atom_t *atom_get(int atom_id);

bool    atom_set_tool(int atom_id, const char *tool_name, const char *input_json);
bool    atom_set_shell(int atom_id, const char *command);
bool    atom_set_dialog_prompt(int atom_id, const char *prompt);
bool    atom_set_assert(int atom_id, const char *condition);
bool    atom_set_result(int atom_id, const char *result);

/* Execute one atom; fills result_buf; updates atom status. */
bool    atom_run(int atom_id, char *result_buf, size_t rlen);

/* ── Execution helpers ───────────────────────────────────────────────────── */

int  plan_ready_atoms(int plan_id, int *atom_ids_out, int max_out);
int  plan_run_next(int plan_id);    /* returns atom_id run, or -1 */

/* ── Top-down decomposition ──────────────────────────────────────────────── */

/* Bulk-add child steps under focus_step_id (0 = root).
   Returns number of steps added. */
int  plan_decompose(int plan_id, int focus_step_id,
                    const char **subtitles, int count);

/* ── Bottom-up aggregation ───────────────────────────────────────────────── */

/* Wrap existing atoms into a new named step.  Returns new step_id or -1. */
int  plan_aggregate_atoms(int plan_id, int parent_step_id,
                           const char *step_title,
                           const int *atom_ids, int count);

/* ── Dialog system ───────────────────────────────────────────────────────── */

int       plan_dialog_ask(int plan_id, int step_id,
                          const char *prompt,
                          const char **choices, int choice_count);
bool      plan_dialog_answer(int dialog_id, const char *response, int chosen);
dialog_t *dialog_get(int dialog_id);
int       plan_pending_dialogs(int plan_id, int *dialog_ids_out, int max_out);

/* ── Serialisation & display ─────────────────────────────────────────────── */

int  plan_to_json(int plan_id, char *buf, size_t len);
int  plan_summary(int plan_id, char *buf, size_t len);
int  plan_tree_render(int plan_id, char *buf, size_t len);

/* ── Name helpers ────────────────────────────────────────────────────────── */

const char *plan_status_name(plan_status_t s);
const char *plan_mode_name(plan_mode_t m);
const char *step_type_name(step_type_t t);
const char *atom_type_name(atom_type_t t);
plan_mode_t plan_mode_parse(const char *s);
step_type_t step_type_parse(const char *s);
atom_type_t atom_type_parse(const char *s);
plan_status_t plan_status_parse(const char *s);

#endif /* DSCO_PLAN_H */
