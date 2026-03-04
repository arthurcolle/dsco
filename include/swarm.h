#ifndef DSCO_SWARM_H
#define DSCO_SWARM_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

/* ── Sub-dsco process handle ──────────────────────────────────────────── */

#define SWARM_MAX_CHILDREN  32
#define SWARM_MAX_GROUPS    8
#define SWARM_MAX_OUTPUT    (256 * 1024)
#define SWARM_LABEL_LEN     128
#define SWARM_GROUP_NAME_LEN 64
#define SWARM_MAX_DEPTH     4   /* max nesting depth for hierarchical swarms */

typedef enum {
    SWARM_PENDING,
    SWARM_RUNNING,
    SWARM_STREAMING,
    SWARM_DONE,
    SWARM_ERROR,
    SWARM_KILLED,
} swarm_status_t;

typedef void (*swarm_stream_cb)(int child_id, const char *data, size_t len, void *ctx);

typedef struct {
    int            id;
    pid_t          pid;
    int            pipe_fd;        /* read end of child's stdout */
    int            err_fd;         /* read end of child's stderr */
    swarm_status_t status;
    int            exit_code;
    char           task[SWARM_LABEL_LEN];
    char           model[128];

    /* Accumulated output */
    char          *output;
    size_t         output_len;
    size_t         output_cap;

    /* Streaming */
    char          *stream_buf;     /* partial line buffer */
    size_t         stream_buf_len;

    /* Timing */
    double         start_time;
    double         end_time;

    /* Group membership */
    int            group_id;       /* -1 if ungrouped */
} swarm_child_t;

typedef struct {
    int   id;
    char  name[SWARM_GROUP_NAME_LEN];
    int   child_ids[SWARM_MAX_CHILDREN];
    int   child_count;
    char  coordinator_task[SWARM_LABEL_LEN];
    bool  active;
} swarm_group_t;

typedef struct {
    swarm_child_t  children[SWARM_MAX_CHILDREN];
    int            child_count;
    swarm_group_t  groups[SWARM_MAX_GROUPS];
    int            group_count;

    /* Global stream callback */
    swarm_stream_cb stream_cb;
    void           *stream_ctx;

    /* API config (inherited by children) */
    const char    *api_key;
    const char    *default_model;
    const char    *dsco_path;     /* path to dsco binary */
} swarm_t;

/* ── Lifecycle ────────────────────────────────────────────────────────── */
void swarm_init(swarm_t *s, const char *api_key, const char *model);
void swarm_destroy(swarm_t *s);

/* ── Spawn sub-dsco ───────────────────────────────────────────────────── */
int  swarm_spawn(swarm_t *s, const char *task, const char *model);
int  swarm_spawn_in_group(swarm_t *s, int group_id, const char *task, const char *model);

/* ── Groups ───────────────────────────────────────────────────────────── */
int  swarm_group_create(swarm_t *s, const char *name);
int  swarm_group_dispatch(swarm_t *s, int group_id, const char **tasks, int task_count,
                          const char *model);
bool swarm_group_complete(swarm_t *s, int group_id);

/* ── Streaming & polling ──────────────────────────────────────────────── */
/* Poll all children for output. timeout_ms=-1 blocks, 0=nonblock */
int  swarm_poll(swarm_t *s, int timeout_ms);

/* Poll and invoke stream callback for each chunk received */
int  swarm_poll_stream(swarm_t *s, int timeout_ms, swarm_stream_cb cb, void *ctx);

/* ── Status & results ─────────────────────────────────────────────────── */
swarm_child_t  *swarm_get(swarm_t *s, int child_id);
const char     *swarm_status_str(swarm_status_t st);
int             swarm_active_count(swarm_t *s);

/* Kill a child */
bool swarm_kill(swarm_t *s, int child_id);

/* Kill all children in a group */
void swarm_group_kill(swarm_t *s, int group_id);

/* ── Formatted output ─────────────────────────────────────────────────── */
int  swarm_status_json(swarm_t *s, char *buf, size_t len);
int  swarm_child_output(swarm_t *s, int child_id, char *buf, size_t len);
int  swarm_group_status_json(swarm_t *s, int group_id, char *buf, size_t len);

#endif
