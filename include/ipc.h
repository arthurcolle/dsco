#ifndef DSCO_IPC_H
#define DSCO_IPC_H

#include <stdbool.h>
#include <stddef.h>

/* ═══════════════════════════════════════════════════════════════════════════
 * Inter-Agent Communication Layer
 *
 * SQLite-backed shared state enabling:
 *   - Agent registry with heartbeat + lifecycle management
 *   - Point-to-point and broadcast messaging between agents
 *   - Priority task queue with assignment and stealing
 *   - Shared scratchpad (key-value store) for collaborative state
 *   - Dynamic toolkit specification per agent
 *
 * All agents in a hierarchy share one SQLite database (WAL mode for
 * concurrent read/write). Path propagated via DSCO_IPC_DB env var.
 * ═══════════════════════════════════════════════════════════════════════════ */

#define IPC_MAX_AGENT_ID     64
#define IPC_MAX_TOPIC        64
#define IPC_MAX_BODY         (64 * 1024)
#define IPC_MAX_KEY          128
#define IPC_HEARTBEAT_SEC    5
#define IPC_STALE_SEC        30      /* agent considered dead after this */

/* ── Agent Status ──────────────────────────────────────────────────────── */

typedef enum {
    IPC_AGENT_STARTING,
    IPC_AGENT_IDLE,
    IPC_AGENT_WORKING,
    IPC_AGENT_DONE,
    IPC_AGENT_ERROR,
    IPC_AGENT_DEAD,
} ipc_agent_status_t;

typedef struct {
    char   id[IPC_MAX_AGENT_ID];
    char   parent_id[IPC_MAX_AGENT_ID];
    int    pid;
    int    depth;
    ipc_agent_status_t status;
    char   role[64];             /* e.g., "researcher", "coder", "reviewer" */
    char   current_task[256];
    double started_at;
    double last_heartbeat;
    char   toolkit[2048];        /* JSON array of tool names, or "*" for all */
} ipc_agent_info_t;

/* ── Message ───────────────────────────────────────────────────────────── */

typedef struct {
    int    id;
    char   from_agent[IPC_MAX_AGENT_ID];
    char   to_agent[IPC_MAX_AGENT_ID];  /* empty = broadcast */
    char   topic[IPC_MAX_TOPIC];
    char  *body;                         /* caller frees */
    double created_at;
    bool   read;
} ipc_message_t;

/* ── Task ──────────────────────────────────────────────────────────────── */

typedef enum {
    IPC_TASK_PENDING,
    IPC_TASK_ASSIGNED,
    IPC_TASK_RUNNING,
    IPC_TASK_DONE,
    IPC_TASK_FAILED,
} ipc_task_status_t;

typedef struct {
    int    id;
    char   assigned_to[IPC_MAX_AGENT_ID]; /* empty = unassigned */
    char   created_by[IPC_MAX_AGENT_ID];
    int    parent_task_id;                 /* for sub-tasks */
    int    priority;                       /* higher = more urgent */
    ipc_task_status_t status;
    char   description[2048];
    char  *result;                         /* caller frees */
    double created_at;
    double started_at;
    double completed_at;
} ipc_task_t;

/* ── Lifecycle ─────────────────────────────────────────────────────────── */

/* Initialize IPC. Creates or opens shared SQLite DB.
   db_path=NULL auto-generates from DSCO_IPC_DB env or /tmp/dsco_ipc_<ppid>.db
   agent_id=NULL auto-generates a unique ID.
   Returns true on success. */
bool ipc_init(const char *db_path, const char *agent_id);

/* Shut down IPC, mark agent as DONE. */
void ipc_shutdown(void);

/* Get this agent's ID */
const char *ipc_self_id(void);

/* Get the IPC database path (for passing to children) */
const char *ipc_db_path(void);

/* ── Agent Registry ────────────────────────────────────────────────────── */

/* Register this agent in the shared registry */
bool ipc_register(const char *parent_id, int depth, const char *role,
                  const char *toolkit);

/* Update own status */
bool ipc_set_status(ipc_agent_status_t status, const char *current_task);

/* Heartbeat — call periodically to indicate liveness */
bool ipc_heartbeat(void);

/* List all agents. Returns count. Caller provides array. */
int ipc_list_agents(ipc_agent_info_t *out, int max);

/* Get info about a specific agent. Returns true if found. */
bool ipc_get_agent(const char *agent_id, ipc_agent_info_t *out);

/* Check if an agent is still alive (heartbeat recent enough) */
bool ipc_agent_alive(const char *agent_id);

/* Mark agents as 'dead' if they haven't heartbeated in stale_s seconds.
 * Returns number of agents reaped. */
int ipc_reap_dead_agents(double stale_s);

/* ── Messaging ─────────────────────────────────────────────────────────── */

/* Send a message to a specific agent (or broadcast if to_agent=NULL) */
bool ipc_send(const char *to_agent, const char *topic, const char *body);

/* Read unread messages for this agent. Returns count. Caller frees body ptrs. */
int ipc_recv(ipc_message_t *out, int max);

/* Read messages on a specific topic (for this agent + broadcasts). */
int ipc_recv_topic(const char *topic, ipc_message_t *out, int max);

/* Get message count (unread) */
int ipc_unread_count(void);

/* ── Task Queue ────────────────────────────────────────────────────────── */

/* Submit a task to the queue. Returns task ID or -1 on error. */
int ipc_task_submit(const char *description, int priority, int parent_task_id);

/* Claim the highest-priority unassigned task. Returns true + fills out. */
bool ipc_task_claim(ipc_task_t *out);

/* Mark a task as running */
bool ipc_task_start(int task_id);

/* Complete a task with result */
bool ipc_task_complete(int task_id, const char *result);

/* Fail a task with error */
bool ipc_task_fail(int task_id, const char *error);

/* List tasks (optionally filtered by assigned_to, NULL=all). */
int ipc_task_list(const char *assigned_to, ipc_task_t *out, int max);

/* Get count of pending (unassigned) tasks */
int ipc_task_pending_count(void);

/* Requeue tasks stuck in assigned/running state beyond timeout_s seconds.
 * Returns number of tasks requeued back to pending. */
int ipc_task_requeue_stale(double timeout_s);

/* ── Scratchpad (Shared Key-Value Store) ───────────────────────────────── */

/* Write a key-value pair. Overwrites if key exists. */
bool ipc_scratch_put(const char *key, const char *value);

/* Read a value by key. Returns malloc'd string or NULL. Caller frees. */
char *ipc_scratch_get(const char *key);

/* Delete a key */
bool ipc_scratch_del(const char *key);

/* List keys matching a prefix. Returns count. */
int ipc_scratch_keys(const char *prefix, char keys[][IPC_MAX_KEY], int max);

/* ── Convenience ───────────────────────────────────────────────────────── */

/* Check for pending messages + tasks. Returns bitmask:
   bit 0 = has unread messages, bit 1 = has claimable tasks */
int ipc_poll(void);

/* Format a status summary as JSON */
int ipc_status_json(char *buf, size_t len);

#endif
