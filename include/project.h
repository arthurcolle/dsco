#ifndef DSCO_PROJECT_H
#define DSCO_PROJECT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include <sys/types.h>
#include <pthread.h>
#include <limits.h>

/* ──────────────────────────────────────────────────────────────────────────
 *  Project — first-class persistent unit of work.
 *
 *  A Project owns:
 *    - a root directory  (cwd for tools, VFS sandbox)
 *    - a workspace dir   (.dsco/ — identity, memory, soul)
 *    - an agent runtime  (its own conversation, OODA, scheduler, MCP set)
 *    - a token/$ budget, a tool-policy, a project-scoped killswitch
 *    - a scrollback ring + stream channel so clients can observe in real time
 *
 *  Multiple Projects coexist in one dsco process. The mux runs them in
 *  parallel (one child worker per project) and renders them as panes/tabs
 *  within a single TUI screen.
 *
 *  Global concerns (provider creds, mesh identity, global killswitch,
 *  audit log root) stay process-wide.
 * ────────────────────────────────────────────────────────────────────────── */

#define DSCO_PROJECT_ID_LEN      37    /* UUID v4 string + NUL */
#define DSCO_PROJECT_NAME_MAX    64
#define DSCO_PROJECT_RING_BYTES  (256 * 1024)
#define DSCO_PROJECT_MAX         16    /* max live projects per mux */

typedef enum {
    DSCO_PROJECT_IDLE = 0,        /* loaded, no worker */
    DSCO_PROJECT_RUNNING,         /* worker alive, agent active */
    DSCO_PROJECT_PAUSED,          /* explicitly suspended */
    DSCO_PROJECT_QUARANTINED,     /* budget/policy violation; needs op intervention */
    DSCO_PROJECT_CLOSED,          /* unloaded from memory, persisted on disk */
    DSCO_PROJECT_DEAD,            /* worker exited; carcass kept for inspection */
} dsco_project_state_t;

typedef struct {
    char    id[DSCO_PROJECT_ID_LEN];      /* uuid v4 */
    char    name[DSCO_PROJECT_NAME_MAX];  /* human label */
    char    root[PATH_MAX];               /* absolute project root */
    char    workspace[PATH_MAX];          /* path to .dsco/ (defaults root/.dsco) */
} dsco_project_id_t;

/* ── Budget & policy (per-project caps) ───────────────────────────────── */
typedef struct {
    uint64_t  token_cap;      /* hard cap on tokens this session */
    uint64_t  tokens_used;
    uint64_t  cents_cap;      /* hard cap in USD cents */
    uint64_t  cents_spent;
    uint32_t  rpm_cap;        /* requests per minute */
    uint32_t  rpm_window;     /* rolling window head */
} dsco_project_budget_t;

typedef struct {
    bool      allow_global_fs;    /* default false: VFS clamped to root */
    bool      allow_network;
    bool      allow_shell;
    bool      allow_mcp_default;  /* if false, mcp servers must be enumerated */
    /* opaque allowlist of tool names; serialized to policy.toml */
} dsco_project_policy_t;

/* ── Bounded ring buffer for scrollback ───────────────────────────────── */
typedef struct {
    char            *buf;
    size_t           cap;
    size_t           head;     /* next write offset */
    size_t           tail;     /* oldest valid byte */
    bool             wrapped;
    uint64_t         total_written;  /* monotonic; lets clients detect overflow */
    pthread_mutex_t  mu;
} dsco_ring_t;

void dsco_ring_init(dsco_ring_t *r, size_t cap);
void dsco_ring_free(dsco_ring_t *r);
size_t dsco_ring_write(dsco_ring_t *r, const char *data, size_t len);
/* Snapshot the ring's contents in order (oldest-first) into `out`.
 * Returns bytes written (capped at out_cap-1, NUL-terminated). */
size_t dsco_ring_snapshot(dsco_ring_t *r, char *out, size_t out_cap);

/* ── The Project itself ───────────────────────────────────────────────── */
struct dsco_mux;   /* forward decl */

typedef struct dsco_project {
    dsco_project_id_t       id;
    dsco_project_state_t    state;

    /* worker (child process model — see project_mux.c) */
    pid_t                   worker_pid;
    int                     worker_in_fd;    /* parent → worker stdin  */
    int                     worker_out_fd;   /* worker → parent stdout */

    /* drain thread — does blocking reads on worker_out_fd into scrollback.
     * The main mux loop never touches worker_out_fd, so a slow/bursty worker
     * cannot stall the render path. */
    pthread_t               drain_thread;
    atomic_int              drain_run;       /* 1 while drain thread active */
    int                     wake_fd_w;       /* write end of mux wake-pipe */

    /* I/O */
    dsco_ring_t             scrollback;
    atomic_int              has_unread;      /* renderer reads without ring mutex */
    uint64_t                epoch;           /* bumps on every state transition */

    /* Activity tracker — drain thread bumps `activity_bytes` (atomic), the
     * renderer samples and decays. Used for heatmap shading + sparkline. */
    atomic_ullong           activity_bytes;
    double                  activity_ewma;   /* renderer-owned; bytes/sec smoothed */
    int64_t                 activity_last_sample_ms;
    /* Sparkline ring: 16 buckets of recent bps (1 bucket ≈ 250ms). */
    double                  activity_ring[16];
    int                     activity_ring_head;

    /* lifecycle metadata */
    int64_t                 created_at;
    int64_t                 started_at;
    int64_t                 last_attached_at;

    /* configuration */
    dsco_project_budget_t   budget;
    dsco_project_policy_t   policy;
    char                    model[64];

    /* link back to the mux that owns this project */
    struct dsco_mux        *mux;
} dsco_project_t;

/* ──────────────────────────────────────────────────────────────────────────
 *  Registry — persisted under ~/.dsco/projects/
 *
 *  registry.idx          line-oriented: ULID<TAB>NAME<TAB>ROOT<TAB>FLAGS
 *  <ULID>/project.toml   canonical metadata for one project
 *  <ULID>/scrollback     last scrollback dump (best effort)
 *  <ULID>/state.json     last known runtime state (epoch, used budget)
 * ────────────────────────────────────────────────────────────────────────── */

typedef struct {
    char    id[DSCO_PROJECT_ID_LEN];
    char    name[DSCO_PROJECT_NAME_MAX];
    char    root[PATH_MAX];
    bool    archived;
    int64_t last_opened_at;
} dsco_project_summary_t;

/* Root of project registry (~/.dsco/projects). Returns const string. */
const char *dsco_project_registry_root(void);

/* Make sure ~/.dsco/projects exists. Returns 0 on success. */
int dsco_project_registry_ensure(void);

/* Create a new project rooted at `root` (absolute path).
 * If `name` is NULL, derives one from basename(root).
 * On success, *out points to a heap-allocated project (caller takes ownership
 * unless registered with a mux). */
int dsco_project_create(const char *root, const char *name, dsco_project_t **out);

/* Open by id, name, or path. Lookup order: id-prefix → name (exact) → root match. */
int dsco_project_open(const char *id_or_name_or_path, dsco_project_t **out);

/* Persist metadata to disk (project.toml + registry.idx upsert). */
int dsco_project_save(const dsco_project_t *p);

/* Close the in-memory project. Stops the worker if running, persists, frees. */
int dsco_project_close(dsco_project_t *p);

/* Free without closing — for projects already detached from a mux. */
void dsco_project_free(dsco_project_t *p);

/* Mark archived (remains on disk, hidden from default listing). */
int dsco_project_archive(const char *id);

/* List projects on disk. Writes up to `max` summaries into `out`,
 * returns count. include_archived=false to hide archives. */
int dsco_project_list(dsco_project_summary_t *out, int max, bool include_archived);

/* ──────────────────────────────────────────────────────────────────────────
 *  Lifecycle — worker control.
 *  The worker is a child process (forked dsco running --worker mode) attached
 *  to the parent via pipes. See project_mux.c for the implementation.
 * ────────────────────────────────────────────────────────────────────────── */

/* Spawn the worker process. Sets state=RUNNING on success. */
int dsco_project_start(dsco_project_t *p, const char *api_key);

/* Send SIGSTOP to worker (or graceful pause directive). */
int dsco_project_pause(dsco_project_t *p);
int dsco_project_resume(dsco_project_t *p);

/* Project-scoped killswitch: terminate the worker, mark DEAD. */
int dsco_project_kill(dsco_project_t *p);

/* ── I/O surface ──────────────────────────────────────────────────────── */

/* Write a line into the worker's stdin. Appends newline if missing. */
int dsco_project_send_input(dsco_project_t *p, const char *line);

/* Pull any pending bytes from the worker into the scrollback ring.
 * Non-blocking — call this in a poll/select loop. Returns bytes drained. */
int dsco_project_drain_output(dsco_project_t *p);

/* Get fd for poll() — the worker's stdout side. */
int dsco_project_poll_fd(const dsco_project_t *p);

/* Snapshot the current scrollback into `out` for rendering. */
size_t dsco_project_snapshot(dsco_project_t *p, char *out, size_t out_cap);

/* Sample + decay activity counter. Returns smoothed bytes/sec (renderer-side). */
double dsco_project_activity_bps(dsco_project_t *p);

/* Copy the 16-bucket sparkline ring in chronological order. */
void   dsco_project_activity_ring(dsco_project_t *p, double *out16);

/* ── Helpers ──────────────────────────────────────────────────────────── */

const char *dsco_project_state_name(dsco_project_state_t s);

#endif /* DSCO_PROJECT_H */
