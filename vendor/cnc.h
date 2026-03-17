/*
 * cnc.h — Central N Coordinator (CNC) Server
 *
 * A lightweight embedded coordination server for distributed agent swarms.
 * Manages node registration, heartbeats, task distribution, leader election,
 * and consensus via a simple TCP protocol with JSON messages.
 *
 * Features:
 *   - Node registry with heartbeat-based liveness detection
 *   - Consistent hash ring for task routing
 *   - Raft-lite leader election (simplified)
 *   - Task queue with priority + affinity
 *   - Pub/sub event broadcast to all nodes
 *   - Partition-tolerant gossip protocol
 *   - Built-in metrics (latency, throughput, node health)
 *
 * Usage:
 *   cnc_server_t *srv = cnc_server_create(&(cnc_config_t){
 *       .bind_addr = "0.0.0.0",
 *       .port = 9700,
 *       .heartbeat_interval_ms = 3000,
 *       .node_timeout_ms = 10000,
 *       .max_nodes = 256,
 *   });
 *   cnc_server_start(srv);
 *   // ... runs in background thread ...
 *   cnc_server_stop(srv);
 *   cnc_server_destroy(srv);
 *
 * License: MIT
 */

#ifndef CNC_H
#define CNC_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Limits ────────────────────────────────────────────────────────── */

#define CNC_MAX_NODES        256
#define CNC_MAX_TASKS        4096
#define CNC_MAX_SUBS         64
#define CNC_MAX_TOPIC_LEN    64
#define CNC_MAX_NAME_LEN     64
#define CNC_MAX_PAYLOAD      (64 * 1024)
#define CNC_MAX_TAGS         16
#define CNC_RING_VNODES      150
#define CNC_GOSSIP_FANOUT    3
#define CNC_ELECTION_TIMEOUT_MS 5000
#define CNC_MSG_HEADER_SIZE  8

/* ── Message Types ─────────────────────────────────────────────────── */

typedef enum {
    CNC_MSG_REGISTER       = 0x01,
    CNC_MSG_REGISTER_ACK   = 0x02,
    CNC_MSG_HEARTBEAT      = 0x03,
    CNC_MSG_HEARTBEAT_ACK  = 0x04,
    CNC_MSG_TASK_SUBMIT    = 0x10,
    CNC_MSG_TASK_ASSIGN    = 0x11,
    CNC_MSG_TASK_COMPLETE  = 0x12,
    CNC_MSG_TASK_FAIL      = 0x13,
    CNC_MSG_TASK_STATUS    = 0x14,
    CNC_MSG_PUBLISH        = 0x20,
    CNC_MSG_SUBSCRIBE      = 0x21,
    CNC_MSG_EVENT          = 0x22,
    CNC_MSG_VOTE_REQ       = 0x30,
    CNC_MSG_VOTE_RESP      = 0x31,
    CNC_MSG_LEADER_ANNOUNCE= 0x32,
    CNC_MSG_GOSSIP         = 0x40,
    CNC_MSG_GOSSIP_ACK     = 0x41,
    CNC_MSG_QUERY_NODES    = 0x50,
    CNC_MSG_QUERY_RESP     = 0x51,
    CNC_MSG_SHUTDOWN       = 0xFF,
} cnc_msg_type_t;

/* ── Node State ────────────────────────────────────────────────────── */

typedef enum {
    CNC_NODE_JOINING  = 0,
    CNC_NODE_ALIVE    = 1,
    CNC_NODE_SUSPECT  = 2,
    CNC_NODE_DEAD     = 3,
    CNC_NODE_LEFT     = 4,
} cnc_node_state_t;

typedef enum {
    CNC_ROLE_WORKER   = 0,
    CNC_ROLE_LEADER   = 1,
    CNC_ROLE_CANDIDATE= 2,
    CNC_ROLE_OBSERVER = 3,
} cnc_node_role_t;

typedef struct {
    uint32_t          id;
    char              name[CNC_MAX_NAME_LEN];
    char              addr[48];          /* ip:port */
    uint16_t          port;
    cnc_node_state_t  state;
    cnc_node_role_t   role;
    int               fd;                /* socket fd, -1 if not connected */
    uint64_t          last_heartbeat;    /* epoch ms */
    uint64_t          join_time;
    uint64_t          latency_us;        /* last measured RTT */
    uint32_t          tasks_completed;
    uint32_t          tasks_failed;
    uint32_t          load;              /* current queue depth */
    char              tags[CNC_MAX_TAGS][CNC_MAX_NAME_LEN];
    int               tag_count;
    uint64_t          version;           /* gossip vector clock */
} cnc_node_t;

/* ── Task ──────────────────────────────────────────────────────────── */

typedef enum {
    CNC_TASK_PENDING   = 0,
    CNC_TASK_ASSIGNED  = 1,
    CNC_TASK_RUNNING   = 2,
    CNC_TASK_DONE      = 3,
    CNC_TASK_FAILED    = 4,
    CNC_TASK_CANCELLED = 5,
} cnc_task_state_t;

typedef struct {
    uint64_t          id;
    char              type[CNC_MAX_NAME_LEN];
    int               priority;          /* higher = more urgent */
    cnc_task_state_t  state;
    uint32_t          assigned_node;     /* node id, 0 = unassigned */
    char             *payload;           /* JSON payload, heap-allocated */
    size_t            payload_len;
    char             *result;            /* result payload */
    size_t            result_len;
    uint64_t          submit_time;
    uint64_t          assign_time;
    uint64_t          complete_time;
    int               retries;
    int               max_retries;
    char              affinity_tag[CNC_MAX_NAME_LEN]; /* prefer nodes with this tag */
    uint64_t          timeout_ms;        /* 0 = no timeout */
} cnc_task_t;

/* ── Pub/Sub ───────────────────────────────────────────────────────── */

typedef struct {
    uint32_t          node_id;
    char              topic[CNC_MAX_TOPIC_LEN];
} cnc_subscription_t;

/* ── Hash Ring Entry ───────────────────────────────────────────────── */

typedef struct {
    uint32_t          hash;
    uint32_t          node_id;
} cnc_ring_point_t;

/* ── Gossip Digest ─────────────────────────────────────────────────── */

typedef struct {
    uint32_t          node_id;
    cnc_node_state_t  state;
    uint64_t          version;
    uint64_t          timestamp;
} cnc_gossip_entry_t;

/* ── Election State ────────────────────────────────────────────────── */

typedef struct {
    uint64_t          term;
    uint32_t          voted_for;        /* node id we voted for in current term */
    uint32_t          leader_id;        /* current leader, 0 = none */
    uint64_t          election_deadline;
    int               votes_received;
    bool              election_in_progress;
} cnc_election_t;

/* ── Metrics ───────────────────────────────────────────────────────── */

typedef struct {
    uint64_t          msgs_received;
    uint64_t          msgs_sent;
    uint64_t          bytes_received;
    uint64_t          bytes_sent;
    uint64_t          tasks_submitted;
    uint64_t          tasks_completed;
    uint64_t          tasks_failed;
    uint64_t          tasks_timed_out;
    uint64_t          elections_started;
    uint64_t          leader_changes;
    double            avg_latency_us;
    uint64_t          uptime_ms;
    uint64_t          start_time;
} cnc_metrics_t;

/* ── Callbacks ─────────────────────────────────────────────────────── */

typedef void (*cnc_node_event_fn)(const cnc_node_t *node, cnc_node_state_t old_state, void *ctx);
typedef void (*cnc_task_event_fn)(const cnc_task_t *task, cnc_task_state_t old_state, void *ctx);
typedef void (*cnc_leader_change_fn)(uint32_t old_leader, uint32_t new_leader, void *ctx);
typedef int  (*cnc_task_route_fn)(const cnc_task_t *task, const cnc_node_t *nodes, int n, void *ctx);
typedef void (*cnc_msg_hook_fn)(cnc_msg_type_t type, const void *data, size_t len, void *ctx);

/* ── Configuration ─────────────────────────────────────────────────── */

typedef struct {
    const char       *bind_addr;
    uint16_t          port;
    uint32_t          heartbeat_interval_ms;
    uint32_t          node_timeout_ms;
    uint32_t          suspect_timeout_ms;   /* time in suspect before dead */
    int               max_nodes;
    int               max_tasks;
    int               task_max_retries;
    uint32_t          task_default_timeout_ms;
    bool              enable_gossip;
    bool              enable_election;
    uint32_t          gossip_interval_ms;
    uint32_t          election_timeout_ms;
    /* callbacks */
    cnc_node_event_fn    on_node_change;
    cnc_task_event_fn    on_task_change;
    cnc_leader_change_fn on_leader_change;
    cnc_task_route_fn    task_router;        /* custom routing, NULL = hash ring */
    cnc_msg_hook_fn      msg_hook;           /* inspect all messages */
    void                *callback_ctx;
} cnc_config_t;

/* ── Server Handle (opaque) ────────────────────────────────────────── */

typedef struct cnc_server cnc_server_t;

/* ── API ───────────────────────────────────────────────────────────── */

/* Lifecycle */
cnc_server_t   *cnc_server_create(const cnc_config_t *config);
int             cnc_server_start(cnc_server_t *srv);
int             cnc_server_stop(cnc_server_t *srv);
void            cnc_server_destroy(cnc_server_t *srv);

/* Node management */
const cnc_node_t *cnc_get_node(cnc_server_t *srv, uint32_t node_id);
int               cnc_get_nodes(cnc_server_t *srv, cnc_node_t *out, int max);
int               cnc_get_alive_count(cnc_server_t *srv);
int               cnc_kick_node(cnc_server_t *srv, uint32_t node_id);

/* Task management */
uint64_t        cnc_submit_task(cnc_server_t *srv, const char *type, int priority,
                                const char *payload, size_t payload_len,
                                const char *affinity_tag);
int             cnc_cancel_task(cnc_server_t *srv, uint64_t task_id);
const cnc_task_t *cnc_get_task(cnc_server_t *srv, uint64_t task_id);
int             cnc_get_pending_count(cnc_server_t *srv);
int             cnc_drain_tasks(cnc_server_t *srv);  /* wait for all tasks to complete */

/* Pub/sub */
int             cnc_publish(cnc_server_t *srv, const char *topic,
                            const char *data, size_t len);
int             cnc_subscribe(cnc_server_t *srv, uint32_t node_id, const char *topic);

/* Election */
uint32_t        cnc_get_leader(cnc_server_t *srv);
int             cnc_force_election(cnc_server_t *srv);

/* Hash ring */
uint32_t        cnc_ring_lookup(cnc_server_t *srv, const char *key, size_t key_len);
int             cnc_ring_lookup_n(cnc_server_t *srv, const char *key, size_t key_len,
                                  uint32_t *out_nodes, int n);

/* Metrics */
cnc_metrics_t   cnc_get_metrics(cnc_server_t *srv);
void            cnc_reset_metrics(cnc_server_t *srv);

/* Gossip */
int             cnc_gossip_inject(cnc_server_t *srv, const cnc_gossip_entry_t *entries, int n);

/* ── Client API (for nodes connecting to the coordinator) ──────────── */

typedef struct cnc_client cnc_client_t;

cnc_client_t   *cnc_client_create(const char *server_addr, uint16_t port,
                                   const char *node_name);
int             cnc_client_connect(cnc_client_t *cli);
int             cnc_client_register(cnc_client_t *cli, const char **tags, int tag_count);
int             cnc_client_heartbeat(cnc_client_t *cli);
int             cnc_client_task_complete(cnc_client_t *cli, uint64_t task_id,
                                          const char *result, size_t result_len);
int             cnc_client_task_fail(cnc_client_t *cli, uint64_t task_id,
                                      const char *reason);
int             cnc_client_publish(cnc_client_t *cli, const char *topic,
                                    const char *data, size_t len);
int             cnc_client_subscribe(cnc_client_t *cli, const char *topic);

typedef void (*cnc_client_task_fn)(uint64_t task_id, const char *type,
                                    const char *payload, size_t len, void *ctx);
typedef void (*cnc_client_event_fn)(const char *topic,
                                     const char *data, size_t len, void *ctx);

int             cnc_client_set_task_handler(cnc_client_t *cli, cnc_client_task_fn fn, void *ctx);
int             cnc_client_set_event_handler(cnc_client_t *cli, cnc_client_event_fn fn, void *ctx);
int             cnc_client_run(cnc_client_t *cli);  /* blocking event loop */
int             cnc_client_run_async(cnc_client_t *cli);  /* background thread */
int             cnc_client_stop(cnc_client_t *cli);
void            cnc_client_destroy(cnc_client_t *cli);

#ifdef __cplusplus
}
#endif

#endif /* CNC_H */
