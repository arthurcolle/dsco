/*
 * cnc.c — Central N Coordinator (CNC) Server Implementation
 *
 * Full implementation of the coordination server with:
 *   - Epoll/kqueue-based event loop
 *   - FNV-1a hash ring for consistent routing
 *   - Raft-lite leader election
 *   - Priority task queue (min-heap)
 *   - SWIM-lite gossip protocol
 *   - JSON wire protocol over TCP
 *
 * License: MIT
 */

#include "cnc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/time.h>
#include <signal.h>

#ifdef __linux__
#include <sys/epoll.h>
#define USE_EPOLL 1
#elif defined(__APPLE__) || defined(__FreeBSD__)
#include <sys/event.h>
#define USE_KQUEUE 1
#else
#include <poll.h>
#define USE_POLL 1
#endif

/* ── Utilities ─────────────────────────────────────────────────────── */

static uint64_t cnc_now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static uint32_t fnv1a(const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int set_tcp_nodelay(int fd) {
    int val = 1;
    return setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &val, sizeof(val));
}

/* ── Internal Server Structure ─────────────────────────────────────── */

struct cnc_server {
    cnc_config_t      config;
    int               listen_fd;
    int               event_fd;          /* epoll/kqueue fd */
    pthread_t         thread;
    volatile bool     running;
    pthread_mutex_t   lock;

    /* Nodes */
    cnc_node_t        nodes[CNC_MAX_NODES];
    int               node_count;
    uint32_t          next_node_id;

    /* Tasks */
    cnc_task_t        tasks[CNC_MAX_TASKS];
    int               task_count;
    uint64_t          next_task_id;

    /* Pub/Sub */
    cnc_subscription_t subs[CNC_MAX_SUBS * CNC_MAX_NODES];
    int                sub_count;

    /* Hash Ring */
    cnc_ring_point_t  ring[CNC_MAX_NODES * CNC_RING_VNODES];
    int               ring_size;

    /* Election */
    cnc_election_t    election;

    /* Metrics */
    cnc_metrics_t     metrics;

    /* Read buffer per connection (simplified: one global buffer) */
    uint8_t           recv_buf[CNC_MAX_PAYLOAD + CNC_MSG_HEADER_SIZE];
};

/* ── Ring Comparator ───────────────────────────────────────────────── */

static int ring_cmp(const void *a, const void *b) {
    uint32_t ha = ((const cnc_ring_point_t *)a)->hash;
    uint32_t hb = ((const cnc_ring_point_t *)b)->hash;
    return (ha > hb) - (ha < hb);
}

/* ── Rebuild Hash Ring ─────────────────────────────────────────────── */

static void rebuild_ring(cnc_server_t *srv) {
    srv->ring_size = 0;
    for (int i = 0; i < srv->node_count; i++) {
        if (srv->nodes[i].state != CNC_NODE_ALIVE) continue;
        for (int v = 0; v < CNC_RING_VNODES; v++) {
            char key[128];
            int klen = snprintf(key, sizeof(key), "%u:%d", srv->nodes[i].id, v);
            srv->ring[srv->ring_size].hash = fnv1a(key, klen);
            srv->ring[srv->ring_size].node_id = srv->nodes[i].id;
            srv->ring_size++;
        }
    }
    qsort(srv->ring, srv->ring_size, sizeof(cnc_ring_point_t), ring_cmp);
}

/* ── Find Node by ID ───────────────────────────────────────────────── */

static cnc_node_t *find_node(cnc_server_t *srv, uint32_t id) {
    for (int i = 0; i < srv->node_count; i++) {
        if (srv->nodes[i].id == id) return &srv->nodes[i];
    }
    return NULL;
}

static cnc_node_t *find_node_by_fd(cnc_server_t *srv, int fd) {
    for (int i = 0; i < srv->node_count; i++) {
        if (srv->nodes[i].fd == fd) return &srv->nodes[i];
    }
    return NULL;
}

/* ── Find Task by ID ───────────────────────────────────────────────── */

static cnc_task_t *find_task(cnc_server_t *srv, uint64_t id) {
    for (int i = 0; i < srv->task_count; i++) {
        if (srv->tasks[i].id == id) return &srv->tasks[i];
    }
    return NULL;
}

/* ── Wire Protocol ─────────────────────────────────────────────────── */
/*
 * Message format:
 *   [1 byte type] [3 bytes reserved] [4 bytes payload length (network order)] [payload]
 */

static int send_msg(int fd, cnc_msg_type_t type, const void *payload, uint32_t len) {
    uint8_t hdr[CNC_MSG_HEADER_SIZE];
    hdr[0] = (uint8_t)type;
    hdr[1] = hdr[2] = hdr[3] = 0;
    uint32_t nlen = htonl(len);
    memcpy(hdr + 4, &nlen, 4);

    /* Send header */
    ssize_t n = send(fd, hdr, CNC_MSG_HEADER_SIZE, MSG_NOSIGNAL);
    if (n != CNC_MSG_HEADER_SIZE) return -1;

    /* Send payload */
    if (len > 0 && payload) {
        size_t sent = 0;
        while (sent < len) {
            n = send(fd, (const uint8_t *)payload + sent, len - sent, MSG_NOSIGNAL);
            if (n <= 0) return -1;
            sent += n;
        }
    }
    return 0;
}

static int recv_msg(int fd, cnc_msg_type_t *type, void *buf, uint32_t *len, uint32_t max_len) {
    uint8_t hdr[CNC_MSG_HEADER_SIZE];
    ssize_t n = recv(fd, hdr, CNC_MSG_HEADER_SIZE, MSG_WAITALL);
    if (n != CNC_MSG_HEADER_SIZE) return -1;

    *type = (cnc_msg_type_t)hdr[0];
    uint32_t plen;
    memcpy(&plen, hdr + 4, 4);
    plen = ntohl(plen);

    if (plen > max_len) return -1;
    *len = plen;

    if (plen > 0) {
        size_t got = 0;
        while (got < plen) {
            n = recv(fd, (uint8_t *)buf + got, plen - got, 0);
            if (n <= 0) return -1;
            got += n;
        }
    }
    return 0;
}

/* ── Simple JSON Builder/Parser (minimal, no allocations) ──────────── */

static int json_snprintf_kv(char *buf, size_t sz, const char *key, const char *val) {
    return snprintf(buf, sz, "\"%s\":\"%s\"", key, val);
}

static int json_snprintf_kv_int(char *buf, size_t sz, const char *key, int64_t val) {
    return snprintf(buf, sz, "\"%s\":%lld", key, (long long)val);
}

/* Extract a string value for a key from a flat JSON object (no nesting) */
static int json_extract_str(const char *json, const char *key, char *out, size_t out_sz) {
    char needle[128];
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char *p = strstr(json, needle);
    if (!p) return -1;
    p += strlen(needle);
    while (*p == ' ') p++;
    if (*p == '"') {
        p++;
        size_t i = 0;
        while (*p && *p != '"' && i < out_sz - 1) {
            out[i++] = *p++;
        }
        out[i] = '\0';
        return (int)i;
    }
    return -1;
}

static int64_t json_extract_int(const char *json, const char *key) {
    char needle[128];
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char *p = strstr(json, needle);
    if (!p) return -1;
    p += strlen(needle);
    while (*p == ' ') p++;
    return strtoll(p, NULL, 10);
}

/* ── Handle Registration ──────────────────────────────────────────── */

static void handle_register(cnc_server_t *srv, int fd, const char *payload, uint32_t len) {
    (void)len;
    pthread_mutex_lock(&srv->lock);

    if (srv->node_count >= srv->config.max_nodes) {
        const char *err = "{\"error\":\"max nodes reached\"}";
        send_msg(fd, CNC_MSG_REGISTER_ACK, err, strlen(err));
        pthread_mutex_unlock(&srv->lock);
        return;
    }

    cnc_node_t *node = &srv->nodes[srv->node_count];
    memset(node, 0, sizeof(*node));
    node->id = ++srv->next_node_id;
    node->fd = fd;
    node->state = CNC_NODE_ALIVE;
    node->role = CNC_ROLE_WORKER;
    node->join_time = cnc_now_ms();
    node->last_heartbeat = node->join_time;

    json_extract_str(payload, "name", node->name, CNC_MAX_NAME_LEN);
    if (!node->name[0]) snprintf(node->name, CNC_MAX_NAME_LEN, "node-%u", node->id);

    /* Extract peer address */
    struct sockaddr_in peer;
    socklen_t plen = sizeof(peer);
    if (getpeername(fd, (struct sockaddr *)&peer, &plen) == 0) {
        snprintf(node->addr, sizeof(node->addr), "%s:%d",
                 inet_ntoa(peer.sin_addr), ntohs(peer.sin_port));
    }

    srv->node_count++;
    rebuild_ring(srv);

    /* Send ACK */
    char resp[256];
    snprintf(resp, sizeof(resp), "{\"node_id\":%u,\"name\":\"%s\",\"status\":\"ok\"}",
             node->id, node->name);
    send_msg(fd, CNC_MSG_REGISTER_ACK, resp, strlen(resp));

    /* Notify callback */
    if (srv->config.on_node_change) {
        srv->config.on_node_change(node, CNC_NODE_JOINING, srv->config.callback_ctx);
    }

    pthread_mutex_unlock(&srv->lock);
}

/* ── Handle Heartbeat ──────────────────────────────────────────────── */

static void handle_heartbeat(cnc_server_t *srv, int fd, const char *payload, uint32_t len) {
    (void)payload; (void)len;
    pthread_mutex_lock(&srv->lock);

    cnc_node_t *node = find_node_by_fd(srv, fd);
    if (node) {
        uint64_t now = cnc_now_ms();
        node->latency_us = (now - node->last_heartbeat) * 1000;
        node->last_heartbeat = now;

        if (node->state == CNC_NODE_SUSPECT) {
            cnc_node_state_t old = node->state;
            node->state = CNC_NODE_ALIVE;
            rebuild_ring(srv);
            if (srv->config.on_node_change)
                srv->config.on_node_change(node, old, srv->config.callback_ctx);
        }

        node->load = (uint32_t)json_extract_int(payload, "load");

        char ack[64];
        snprintf(ack, sizeof(ack), "{\"status\":\"ok\",\"time\":%llu}",
                 (unsigned long long)now);
        send_msg(fd, CNC_MSG_HEARTBEAT_ACK, ack, strlen(ack));
    }

    pthread_mutex_unlock(&srv->lock);
}

/* ── Task Assignment ───────────────────────────────────────────────── */

static cnc_node_t *pick_node_for_task(cnc_server_t *srv, cnc_task_t *task) {
    /* Custom router */
    if (srv->config.task_router) {
        int idx = srv->config.task_router(task, srv->nodes, srv->node_count,
                                           srv->config.callback_ctx);
        if (idx >= 0 && idx < srv->node_count) return &srv->nodes[idx];
    }

    /* Affinity tag match */
    if (task->affinity_tag[0]) {
        cnc_node_t *best = NULL;
        uint32_t min_load = UINT32_MAX;
        for (int i = 0; i < srv->node_count; i++) {
            cnc_node_t *n = &srv->nodes[i];
            if (n->state != CNC_NODE_ALIVE) continue;
            for (int t = 0; t < n->tag_count; t++) {
                if (strcmp(n->tags[t], task->affinity_tag) == 0) {
                    if (n->load < min_load) {
                        min_load = n->load;
                        best = n;
                    }
                    break;
                }
            }
        }
        if (best) return best;
    }

    /* Hash ring lookup */
    if (srv->ring_size > 0) {
        char key[128];
        int klen = snprintf(key, sizeof(key), "task:%llu", (unsigned long long)task->id);
        uint32_t h = fnv1a(key, klen);

        /* Binary search for first ring point >= h */
        int lo = 0, hi = srv->ring_size - 1;
        while (lo < hi) {
            int mid = (lo + hi) / 2;
            if (srv->ring[mid].hash < h) lo = mid + 1;
            else hi = mid;
        }

        uint32_t nid = srv->ring[lo].node_id;
        return find_node(srv, nid);
    }

    /* Fallback: least loaded alive node */
    cnc_node_t *best = NULL;
    uint32_t min_load = UINT32_MAX;
    for (int i = 0; i < srv->node_count; i++) {
        if (srv->nodes[i].state == CNC_NODE_ALIVE && srv->nodes[i].load < min_load) {
            min_load = srv->nodes[i].load;
            best = &srv->nodes[i];
        }
    }
    return best;
}

static void try_assign_tasks(cnc_server_t *srv) {
    for (int i = 0; i < srv->task_count; i++) {
        cnc_task_t *t = &srv->tasks[i];
        if (t->state != CNC_TASK_PENDING) continue;

        cnc_node_t *node = pick_node_for_task(srv, t);
        if (!node || node->fd < 0) continue;

        t->state = CNC_TASK_ASSIGNED;
        t->assigned_node = node->id;
        t->assign_time = cnc_now_ms();
        node->load++;

        /* Send task to node */
        char msg[CNC_MAX_PAYLOAD];
        int n = snprintf(msg, sizeof(msg),
                         "{\"task_id\":%llu,\"type\":\"%s\",\"priority\":%d,\"payload\":",
                         (unsigned long long)t->id, t->type, t->priority);
        if (t->payload && t->payload_len > 0) {
            size_t rem = sizeof(msg) - n - 2;
            size_t copy = t->payload_len < rem ? t->payload_len : rem;
            memcpy(msg + n, t->payload, copy);
            n += copy;
        } else {
            msg[n++] = 'n'; msg[n++] = 'u'; msg[n++] = 'l'; msg[n++] = 'l';
        }
        msg[n++] = '}';
        msg[n] = '\0';

        send_msg(node->fd, CNC_MSG_TASK_ASSIGN, msg, n);

        if (srv->config.on_task_change)
            srv->config.on_task_change(t, CNC_TASK_PENDING, srv->config.callback_ctx);
    }
}

/* ── Handle Task Submission ────────────────────────────────────────── */

static void handle_task_submit(cnc_server_t *srv, int fd, const char *payload, uint32_t len) {
    (void)fd;
    pthread_mutex_lock(&srv->lock);

    if (srv->task_count >= srv->config.max_tasks) {
        pthread_mutex_unlock(&srv->lock);
        return;
    }

    cnc_task_t *t = &srv->tasks[srv->task_count];
    memset(t, 0, sizeof(*t));
    t->id = ++srv->next_task_id;
    t->state = CNC_TASK_PENDING;
    t->submit_time = cnc_now_ms();
    t->max_retries = srv->config.task_max_retries > 0 ? srv->config.task_max_retries : 3;
    t->timeout_ms = srv->config.task_default_timeout_ms;

    json_extract_str(payload, "type", t->type, CNC_MAX_NAME_LEN);
    t->priority = (int)json_extract_int(payload, "priority");
    json_extract_str(payload, "affinity", t->affinity_tag, CNC_MAX_NAME_LEN);

    /* Copy payload */
    if (len > 0) {
        t->payload = malloc(len + 1);
        if (t->payload) {
            memcpy(t->payload, payload, len);
            t->payload[len] = '\0';
            t->payload_len = len;
        }
    }

    srv->task_count++;
    srv->metrics.tasks_submitted++;

    try_assign_tasks(srv);

    /* Send task ID back */
    char resp[64];
    snprintf(resp, sizeof(resp), "{\"task_id\":%llu}", (unsigned long long)t->id);
    send_msg(fd, CNC_MSG_TASK_STATUS, resp, strlen(resp));

    pthread_mutex_unlock(&srv->lock);
}

/* ── Handle Task Complete/Fail ─────────────────────────────────────── */

static void handle_task_complete(cnc_server_t *srv, int fd, const char *payload, uint32_t len) {
    (void)fd;
    pthread_mutex_lock(&srv->lock);

    uint64_t task_id = (uint64_t)json_extract_int(payload, "task_id");
    cnc_task_t *t = find_task(srv, task_id);
    if (t && (t->state == CNC_TASK_ASSIGNED || t->state == CNC_TASK_RUNNING)) {
        cnc_task_state_t old = t->state;
        t->state = CNC_TASK_DONE;
        t->complete_time = cnc_now_ms();

        /* Store result */
        if (len > 0) {
            t->result = malloc(len + 1);
            if (t->result) {
                memcpy(t->result, payload, len);
                t->result[len] = '\0';
                t->result_len = len;
            }
        }

        cnc_node_t *node = find_node(srv, t->assigned_node);
        if (node) {
            node->tasks_completed++;
            if (node->load > 0) node->load--;
        }

        srv->metrics.tasks_completed++;

        if (srv->config.on_task_change)
            srv->config.on_task_change(t, old, srv->config.callback_ctx);
    }

    pthread_mutex_unlock(&srv->lock);
}

static void handle_task_fail(cnc_server_t *srv, int fd, const char *payload, uint32_t len) {
    (void)fd; (void)len;
    pthread_mutex_lock(&srv->lock);

    uint64_t task_id = (uint64_t)json_extract_int(payload, "task_id");
    cnc_task_t *t = find_task(srv, task_id);
    if (t && (t->state == CNC_TASK_ASSIGNED || t->state == CNC_TASK_RUNNING)) {
        cnc_node_t *node = find_node(srv, t->assigned_node);
        if (node) {
            node->tasks_failed++;
            if (node->load > 0) node->load--;
        }

        t->retries++;
        if (t->retries < t->max_retries) {
            /* Re-queue */
            t->state = CNC_TASK_PENDING;
            t->assigned_node = 0;
            try_assign_tasks(srv);
        } else {
            t->state = CNC_TASK_FAILED;
            t->complete_time = cnc_now_ms();
            srv->metrics.tasks_failed++;
            if (srv->config.on_task_change)
                srv->config.on_task_change(t, CNC_TASK_ASSIGNED, srv->config.callback_ctx);
        }
    }

    pthread_mutex_unlock(&srv->lock);
}

/* ── Handle Pub/Sub ────────────────────────────────────────────────── */

static void handle_subscribe(cnc_server_t *srv, int fd, const char *payload, uint32_t len) {
    (void)len;
    pthread_mutex_lock(&srv->lock);

    cnc_node_t *node = find_node_by_fd(srv, fd);
    if (!node || srv->sub_count >= CNC_MAX_SUBS * CNC_MAX_NODES) {
        pthread_mutex_unlock(&srv->lock);
        return;
    }

    cnc_subscription_t *sub = &srv->subs[srv->sub_count++];
    sub->node_id = node->id;
    json_extract_str(payload, "topic", sub->topic, CNC_MAX_TOPIC_LEN);

    pthread_mutex_unlock(&srv->lock);
}

static void handle_publish(cnc_server_t *srv, int fd, const char *payload, uint32_t len) {
    (void)fd;
    pthread_mutex_lock(&srv->lock);

    char topic[CNC_MAX_TOPIC_LEN];
    json_extract_str(payload, "topic", topic, CNC_MAX_TOPIC_LEN);

    /* Broadcast to all subscribers */
    for (int i = 0; i < srv->sub_count; i++) {
        if (strcmp(srv->subs[i].topic, topic) != 0) continue;
        cnc_node_t *node = find_node(srv, srv->subs[i].node_id);
        if (node && node->fd >= 0 && node->state == CNC_NODE_ALIVE) {
            send_msg(node->fd, CNC_MSG_EVENT, payload, len);
        }
    }

    pthread_mutex_unlock(&srv->lock);
}

/* ── Leader Election (Raft-lite) ───────────────────────────────────── */

static void start_election(cnc_server_t *srv) {
    srv->election.term++;
    srv->election.election_in_progress = true;
    srv->election.votes_received = 1;  /* vote for self (coordinator) */
    srv->election.voted_for = 0;       /* coordinator votes for itself */
    srv->election.election_deadline = cnc_now_ms() + CNC_ELECTION_TIMEOUT_MS;
    srv->metrics.elections_started++;

    /* Request votes from all alive nodes */
    char msg[128];
    snprintf(msg, sizeof(msg), "{\"term\":%llu,\"candidate_id\":0}",
             (unsigned long long)srv->election.term);

    for (int i = 0; i < srv->node_count; i++) {
        if (srv->nodes[i].state == CNC_NODE_ALIVE && srv->nodes[i].fd >= 0) {
            send_msg(srv->nodes[i].fd, CNC_MSG_VOTE_REQ, msg, strlen(msg));
        }
    }
}

static void handle_vote_resp(cnc_server_t *srv, int fd, const char *payload, uint32_t len) {
    (void)fd; (void)len;
    pthread_mutex_lock(&srv->lock);

    uint64_t term = (uint64_t)json_extract_int(payload, "term");
    int64_t granted = json_extract_int(payload, "granted");

    if (term == srv->election.term && granted && srv->election.election_in_progress) {
        srv->election.votes_received++;

        int alive = cnc_get_alive_count(srv);
        int majority = (alive / 2) + 1;

        if (srv->election.votes_received >= majority) {
            uint32_t old_leader = srv->election.leader_id;
            srv->election.leader_id = 0; /* coordinator is leader */
            srv->election.election_in_progress = false;

            /* Announce leadership */
            char msg[128];
            snprintf(msg, sizeof(msg), "{\"term\":%llu,\"leader_id\":0}",
                     (unsigned long long)srv->election.term);
            for (int i = 0; i < srv->node_count; i++) {
                if (srv->nodes[i].state == CNC_NODE_ALIVE && srv->nodes[i].fd >= 0) {
                    send_msg(srv->nodes[i].fd, CNC_MSG_LEADER_ANNOUNCE, msg, strlen(msg));
                }
            }

            srv->metrics.leader_changes++;
            if (srv->config.on_leader_change)
                srv->config.on_leader_change(old_leader, 0, srv->config.callback_ctx);
        }
    }

    pthread_mutex_unlock(&srv->lock);
}

/* ── Gossip Protocol ───────────────────────────────────────────────── */

static void gossip_round(cnc_server_t *srv) {
    if (srv->node_count < 2) return;

    /* Build digest of all node states */
    char digest[CNC_MAX_PAYLOAD];
    int n = snprintf(digest, sizeof(digest), "{\"entries\":[");

    for (int i = 0; i < srv->node_count && n < (int)sizeof(digest) - 100; i++) {
        if (i > 0) digest[n++] = ',';
        n += snprintf(digest + n, sizeof(digest) - n,
                      "{\"id\":%u,\"state\":%d,\"ver\":%llu}",
                      srv->nodes[i].id, srv->nodes[i].state,
                      (unsigned long long)srv->nodes[i].version);
    }
    n += snprintf(digest + n, sizeof(digest) - n, "]}");

    /* Send to random subset of alive nodes (fanout) */
    int sent = 0;
    for (int i = 0; i < srv->node_count && sent < CNC_GOSSIP_FANOUT; i++) {
        /* Simple: just iterate, a real impl would randomize */
        int idx = (int)(cnc_now_ms() + i) % srv->node_count;
        cnc_node_t *target = &srv->nodes[idx];
        if (target->state == CNC_NODE_ALIVE && target->fd >= 0) {
            send_msg(target->fd, CNC_MSG_GOSSIP, digest, n);
            sent++;
        }
    }
}

/* ── Liveness Check ────────────────────────────────────────────────── */

static void check_liveness(cnc_server_t *srv) {
    uint64_t now = cnc_now_ms();

    for (int i = 0; i < srv->node_count; i++) {
        cnc_node_t *n = &srv->nodes[i];
        if (n->state == CNC_NODE_DEAD || n->state == CNC_NODE_LEFT) continue;

        uint64_t elapsed = now - n->last_heartbeat;

        if (n->state == CNC_NODE_ALIVE && elapsed > srv->config.node_timeout_ms) {
            cnc_node_state_t old = n->state;
            n->state = CNC_NODE_SUSPECT;
            if (srv->config.on_node_change)
                srv->config.on_node_change(n, old, srv->config.callback_ctx);
        }

        uint32_t suspect_timeout = srv->config.suspect_timeout_ms > 0
            ? srv->config.suspect_timeout_ms : srv->config.node_timeout_ms * 2;

        if (n->state == CNC_NODE_SUSPECT && elapsed > suspect_timeout) {
            cnc_node_state_t old = n->state;
            n->state = CNC_NODE_DEAD;
            if (n->fd >= 0) { close(n->fd); n->fd = -1; }
            rebuild_ring(srv);

            /* Re-queue any tasks assigned to dead node */
            for (int j = 0; j < srv->task_count; j++) {
                cnc_task_t *t = &srv->tasks[j];
                if (t->assigned_node == n->id &&
                    (t->state == CNC_TASK_ASSIGNED || t->state == CNC_TASK_RUNNING)) {
                    t->state = CNC_TASK_PENDING;
                    t->assigned_node = 0;
                    t->retries++;
                }
            }

            if (srv->config.on_node_change)
                srv->config.on_node_change(n, old, srv->config.callback_ctx);
        }
    }

    /* Check task timeouts */
    for (int i = 0; i < srv->task_count; i++) {
        cnc_task_t *t = &srv->tasks[i];
        if (t->timeout_ms > 0 && t->state == CNC_TASK_ASSIGNED) {
            if (now - t->assign_time > t->timeout_ms) {
                t->state = CNC_TASK_PENDING;
                t->assigned_node = 0;
                t->retries++;
                srv->metrics.tasks_timed_out++;
                if (t->retries >= t->max_retries) {
                    t->state = CNC_TASK_FAILED;
                    srv->metrics.tasks_failed++;
                }
            }
        }
    }

    try_assign_tasks(srv);
}

/* ── Process a message ─────────────────────────────────────────────── */

static void process_message(cnc_server_t *srv, int fd, cnc_msg_type_t type,
                             const char *payload, uint32_t len) {
    srv->metrics.msgs_received++;
    srv->metrics.bytes_received += len + CNC_MSG_HEADER_SIZE;

    if (srv->config.msg_hook) {
        srv->config.msg_hook(type, payload, len, srv->config.callback_ctx);
    }

    switch (type) {
        case CNC_MSG_REGISTER:       handle_register(srv, fd, payload, len); break;
        case CNC_MSG_HEARTBEAT:      handle_heartbeat(srv, fd, payload, len); break;
        case CNC_MSG_TASK_SUBMIT:    handle_task_submit(srv, fd, payload, len); break;
        case CNC_MSG_TASK_COMPLETE:  handle_task_complete(srv, fd, payload, len); break;
        case CNC_MSG_TASK_FAIL:      handle_task_fail(srv, fd, payload, len); break;
        case CNC_MSG_SUBSCRIBE:      handle_subscribe(srv, fd, payload, len); break;
        case CNC_MSG_PUBLISH:        handle_publish(srv, fd, payload, len); break;
        case CNC_MSG_VOTE_RESP:      handle_vote_resp(srv, fd, payload, len); break;
        case CNC_MSG_QUERY_NODES: {
            pthread_mutex_lock(&srv->lock);
            char resp[CNC_MAX_PAYLOAD];
            int n = snprintf(resp, sizeof(resp), "{\"nodes\":[");
            for (int i = 0; i < srv->node_count && n < (int)sizeof(resp) - 200; i++) {
                if (i > 0) resp[n++] = ',';
                n += snprintf(resp + n, sizeof(resp) - n,
                    "{\"id\":%u,\"name\":\"%s\",\"state\":%d,\"load\":%u,\"tasks\":%u}",
                    srv->nodes[i].id, srv->nodes[i].name, srv->nodes[i].state,
                    srv->nodes[i].load, srv->nodes[i].tasks_completed);
            }
            n += snprintf(resp + n, sizeof(resp) - n, "]}");
            send_msg(fd, CNC_MSG_QUERY_RESP, resp, n);
            pthread_mutex_unlock(&srv->lock);
            break;
        }
        default: break;
    }
}

/* ── Accept Connection ─────────────────────────────────────────────── */

static void accept_connection(cnc_server_t *srv) {
    struct sockaddr_in addr;
    socklen_t alen = sizeof(addr);
    int fd = accept(srv->listen_fd, (struct sockaddr *)&addr, &alen);
    if (fd < 0) return;

    set_nonblocking(fd);
    set_tcp_nodelay(fd);

#ifdef USE_KQUEUE
    struct kevent ev;
    EV_SET(&ev, fd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, NULL);
    kevent(srv->event_fd, &ev, 1, NULL, 0, NULL);
#elif defined(USE_EPOLL)
    struct epoll_event ev = { .events = EPOLLIN | EPOLLET, .data.fd = fd };
    epoll_ctl(srv->event_fd, EPOLL_CTL_ADD, fd, &ev);
#endif
}

/* ── Event Loop ────────────────────────────────────────────────────── */

static void *server_loop(void *arg) {
    cnc_server_t *srv = (cnc_server_t *)arg;
    srv->metrics.start_time = cnc_now_ms();

    uint64_t last_liveness = 0;
    uint64_t last_gossip = 0;

#ifdef USE_KQUEUE
    struct kevent events[64];
#elif defined(USE_EPOLL)
    struct epoll_event events[64];
#endif

    while (srv->running) {
        uint64_t now = cnc_now_ms();
        int timeout_ms = 100;

#ifdef USE_KQUEUE
        struct timespec ts = { .tv_sec = 0, .tv_nsec = timeout_ms * 1000000L };
        int nev = kevent(srv->event_fd, NULL, 0, events, 64, &ts);
#elif defined(USE_EPOLL)
        int nev = epoll_wait(srv->event_fd, events, 64, timeout_ms);
#else
        int nev = 0;
        usleep(timeout_ms * 1000);
#endif

        for (int i = 0; i < nev; i++) {
#ifdef USE_KQUEUE
            int fd = (int)events[i].ident;
#elif defined(USE_EPOLL)
            int fd = events[i].data.fd;
#else
            int fd = -1;
#endif

            if (fd == srv->listen_fd) {
                accept_connection(srv);
            } else {
                cnc_msg_type_t type;
                uint32_t len = 0;
                if (recv_msg(fd, &type, srv->recv_buf, &len, CNC_MAX_PAYLOAD) == 0) {
                    srv->recv_buf[len] = '\0';
                    process_message(srv, fd, type, (const char *)srv->recv_buf, len);
                } else {
                    /* Connection closed or error */
                    pthread_mutex_lock(&srv->lock);
                    cnc_node_t *node = find_node_by_fd(srv, fd);
                    if (node) {
                        cnc_node_state_t old = node->state;
                        node->state = CNC_NODE_DEAD;
                        node->fd = -1;
                        rebuild_ring(srv);
                        if (srv->config.on_node_change)
                            srv->config.on_node_change(node, old, srv->config.callback_ctx);
                    }
                    close(fd);
                    pthread_mutex_unlock(&srv->lock);
                }
            }
        }

        /* Periodic tasks */
        now = cnc_now_ms();

        if (now - last_liveness > srv->config.heartbeat_interval_ms) {
            pthread_mutex_lock(&srv->lock);
            check_liveness(srv);
            pthread_mutex_unlock(&srv->lock);
            last_liveness = now;
        }

        uint32_t gossip_interval = srv->config.gossip_interval_ms > 0
            ? srv->config.gossip_interval_ms : 5000;
        if (srv->config.enable_gossip && now - last_gossip > gossip_interval) {
            pthread_mutex_lock(&srv->lock);
            gossip_round(srv);
            pthread_mutex_unlock(&srv->lock);
            last_gossip = now;
        }

        /* Election timeout check */
        if (srv->config.enable_election && srv->election.election_in_progress) {
            if (now > srv->election.election_deadline) {
                pthread_mutex_lock(&srv->lock);
                srv->election.election_in_progress = false;
                start_election(srv);
                pthread_mutex_unlock(&srv->lock);
            }
        }

        srv->metrics.uptime_ms = now - srv->metrics.start_time;
    }

    return NULL;
}

/* ── Public API: Lifecycle ─────────────────────────────────────────── */

cnc_server_t *cnc_server_create(const cnc_config_t *config) {
    cnc_server_t *srv = calloc(1, sizeof(*srv));
    if (!srv) return NULL;

    srv->config = *config;
    if (srv->config.max_nodes <= 0 || srv->config.max_nodes > CNC_MAX_NODES)
        srv->config.max_nodes = CNC_MAX_NODES;
    if (srv->config.max_tasks <= 0 || srv->config.max_tasks > CNC_MAX_TASKS)
        srv->config.max_tasks = CNC_MAX_TASKS;
    if (srv->config.heartbeat_interval_ms == 0)
        srv->config.heartbeat_interval_ms = 3000;
    if (srv->config.node_timeout_ms == 0)
        srv->config.node_timeout_ms = 10000;

    srv->listen_fd = -1;
    srv->event_fd = -1;
    srv->running = false;
    pthread_mutex_init(&srv->lock, NULL);

    return srv;
}

int cnc_server_start(cnc_server_t *srv) {
    if (!srv) return -1;

    /* Create listening socket */
    srv->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (srv->listen_fd < 0) return -1;

    int opt = 1;
    setsockopt(srv->listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#ifdef SO_REUSEPORT
    setsockopt(srv->listen_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
#endif

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(srv->config.port),
    };
    if (srv->config.bind_addr) {
        inet_pton(AF_INET, srv->config.bind_addr, &addr.sin_addr);
    } else {
        addr.sin_addr.s_addr = INADDR_ANY;
    }

    if (bind(srv->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(srv->listen_fd);
        srv->listen_fd = -1;
        return -1;
    }

    if (listen(srv->listen_fd, 128) < 0) {
        close(srv->listen_fd);
        srv->listen_fd = -1;
        return -1;
    }

    set_nonblocking(srv->listen_fd);

    /* Create event mechanism */
#ifdef USE_KQUEUE
    srv->event_fd = kqueue();
    struct kevent ev;
    EV_SET(&ev, srv->listen_fd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, NULL);
    kevent(srv->event_fd, &ev, 1, NULL, 0, NULL);
#elif defined(USE_EPOLL)
    srv->event_fd = epoll_create1(0);
    struct epoll_event ev = { .events = EPOLLIN, .data.fd = srv->listen_fd };
    epoll_ctl(srv->event_fd, EPOLL_CTL_ADD, srv->listen_fd, &ev);
#endif

    srv->running = true;

    signal(SIGPIPE, SIG_IGN);

    if (pthread_create(&srv->thread, NULL, server_loop, srv) != 0) {
        srv->running = false;
        close(srv->listen_fd);
        srv->listen_fd = -1;
        return -1;
    }

    return 0;
}

int cnc_server_stop(cnc_server_t *srv) {
    if (!srv || !srv->running) return -1;
    srv->running = false;
    pthread_join(srv->thread, NULL);

    /* Close all node connections */
    for (int i = 0; i < srv->node_count; i++) {
        if (srv->nodes[i].fd >= 0) {
            close(srv->nodes[i].fd);
            srv->nodes[i].fd = -1;
        }
    }

    if (srv->listen_fd >= 0) { close(srv->listen_fd); srv->listen_fd = -1; }
    if (srv->event_fd >= 0) { close(srv->event_fd); srv->event_fd = -1; }

    return 0;
}

void cnc_server_destroy(cnc_server_t *srv) {
    if (!srv) return;
    if (srv->running) cnc_server_stop(srv);

    /* Free task payloads */
    for (int i = 0; i < srv->task_count; i++) {
        free(srv->tasks[i].payload);
        free(srv->tasks[i].result);
    }

    pthread_mutex_destroy(&srv->lock);
    free(srv);
}

/* ── Public API: Node Management ───────────────────────────────────── */

const cnc_node_t *cnc_get_node(cnc_server_t *srv, uint32_t node_id) {
    pthread_mutex_lock(&srv->lock);
    cnc_node_t *n = find_node(srv, node_id);
    pthread_mutex_unlock(&srv->lock);
    return n;
}

int cnc_get_nodes(cnc_server_t *srv, cnc_node_t *out, int max) {
    pthread_mutex_lock(&srv->lock);
    int n = srv->node_count < max ? srv->node_count : max;
    memcpy(out, srv->nodes, n * sizeof(cnc_node_t));
    pthread_mutex_unlock(&srv->lock);
    return n;
}

int cnc_get_alive_count(cnc_server_t *srv) {
    int count = 0;
    for (int i = 0; i < srv->node_count; i++) {
        if (srv->nodes[i].state == CNC_NODE_ALIVE) count++;
    }
    return count;
}

int cnc_kick_node(cnc_server_t *srv, uint32_t node_id) {
    pthread_mutex_lock(&srv->lock);
    cnc_node_t *n = find_node(srv, node_id);
    if (!n) { pthread_mutex_unlock(&srv->lock); return -1; }

    if (n->fd >= 0) {
        send_msg(n->fd, CNC_MSG_SHUTDOWN, "{}", 2);
        close(n->fd);
        n->fd = -1;
    }
    n->state = CNC_NODE_LEFT;
    rebuild_ring(srv);
    pthread_mutex_unlock(&srv->lock);
    return 0;
}

/* ── Public API: Tasks ─────────────────────────────────────────────── */

uint64_t cnc_submit_task(cnc_server_t *srv, const char *type, int priority,
                          const char *payload, size_t payload_len,
                          const char *affinity_tag) {
    char msg[CNC_MAX_PAYLOAD];
    int n = snprintf(msg, sizeof(msg),
                     "{\"type\":\"%s\",\"priority\":%d", type, priority);
    if (affinity_tag && affinity_tag[0]) {
        n += snprintf(msg + n, sizeof(msg) - n, ",\"affinity\":\"%s\"", affinity_tag);
    }
    if (payload && payload_len > 0) {
        n += snprintf(msg + n, sizeof(msg) - n, ",\"data\":");
        size_t rem = sizeof(msg) - n - 2;
        size_t copy = payload_len < rem ? payload_len : rem;
        memcpy(msg + n, payload, copy);
        n += copy;
    }
    msg[n++] = '}';
    msg[n] = '\0';

    handle_task_submit(srv, -1, msg, n);
    return srv->next_task_id;
}

int cnc_cancel_task(cnc_server_t *srv, uint64_t task_id) {
    pthread_mutex_lock(&srv->lock);
    cnc_task_t *t = find_task(srv, task_id);
    if (!t) { pthread_mutex_unlock(&srv->lock); return -1; }
    t->state = CNC_TASK_CANCELLED;
    pthread_mutex_unlock(&srv->lock);
    return 0;
}

const cnc_task_t *cnc_get_task(cnc_server_t *srv, uint64_t task_id) {
    pthread_mutex_lock(&srv->lock);
    cnc_task_t *t = find_task(srv, task_id);
    pthread_mutex_unlock(&srv->lock);
    return t;
}

int cnc_get_pending_count(cnc_server_t *srv) {
    pthread_mutex_lock(&srv->lock);
    int count = 0;
    for (int i = 0; i < srv->task_count; i++) {
        if (srv->tasks[i].state == CNC_TASK_PENDING) count++;
    }
    pthread_mutex_unlock(&srv->lock);
    return count;
}

int cnc_drain_tasks(cnc_server_t *srv) {
    while (1) {
        pthread_mutex_lock(&srv->lock);
        bool all_done = true;
        for (int i = 0; i < srv->task_count; i++) {
            cnc_task_state_t s = srv->tasks[i].state;
            if (s == CNC_TASK_PENDING || s == CNC_TASK_ASSIGNED || s == CNC_TASK_RUNNING) {
                all_done = false;
                break;
            }
        }
        pthread_mutex_unlock(&srv->lock);
        if (all_done) return 0;
        usleep(50000); /* 50ms */
    }
}

/* ── Public API: Pub/Sub ───────────────────────────────────────────── */

int cnc_publish(cnc_server_t *srv, const char *topic, const char *data, size_t len) {
    char msg[CNC_MAX_PAYLOAD];
    int n = snprintf(msg, sizeof(msg), "{\"topic\":\"%s\",\"data\":", topic);
    if (data && len > 0) {
        size_t rem = sizeof(msg) - n - 2;
        size_t copy = len < rem ? len : rem;
        memcpy(msg + n, data, copy);
        n += copy;
    } else {
        msg[n++] = 'n'; msg[n++] = 'u'; msg[n++] = 'l'; msg[n++] = 'l';
    }
    msg[n++] = '}';
    msg[n] = '\0';

    handle_publish(srv, -1, msg, n);
    return 0;
}

int cnc_subscribe(cnc_server_t *srv, uint32_t node_id, const char *topic) {
    pthread_mutex_lock(&srv->lock);
    if (srv->sub_count >= CNC_MAX_SUBS * CNC_MAX_NODES) {
        pthread_mutex_unlock(&srv->lock);
        return -1;
    }
    cnc_subscription_t *sub = &srv->subs[srv->sub_count++];
    sub->node_id = node_id;
    strncpy(sub->topic, topic, CNC_MAX_TOPIC_LEN - 1);
    sub->topic[CNC_MAX_TOPIC_LEN - 1] = '\0';
    pthread_mutex_unlock(&srv->lock);
    return 0;
}

/* ── Public API: Election ──────────────────────────────────────────── */

uint32_t cnc_get_leader(cnc_server_t *srv) {
    pthread_mutex_lock(&srv->lock);
    uint32_t leader = srv->election.leader_id;
    pthread_mutex_unlock(&srv->lock);
    return leader;
}

int cnc_force_election(cnc_server_t *srv) {
    pthread_mutex_lock(&srv->lock);
    start_election(srv);
    pthread_mutex_unlock(&srv->lock);
    return 0;
}

/* ── Public API: Hash Ring ─────────────────────────────────────────── */

uint32_t cnc_ring_lookup(cnc_server_t *srv, const char *key, size_t key_len) {
    pthread_mutex_lock(&srv->lock);
    if (srv->ring_size == 0) { pthread_mutex_unlock(&srv->lock); return 0; }

    uint32_t h = fnv1a(key, key_len);
    int lo = 0, hi = srv->ring_size - 1;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (srv->ring[mid].hash < h) lo = mid + 1;
        else hi = mid;
    }
    uint32_t nid = srv->ring[lo % srv->ring_size].node_id;
    pthread_mutex_unlock(&srv->lock);
    return nid;
}

int cnc_ring_lookup_n(cnc_server_t *srv, const char *key, size_t key_len,
                       uint32_t *out_nodes, int n) {
    pthread_mutex_lock(&srv->lock);
    if (srv->ring_size == 0) { pthread_mutex_unlock(&srv->lock); return 0; }

    uint32_t h = fnv1a(key, key_len);
    int lo = 0, hi = srv->ring_size - 1;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (srv->ring[mid].hash < h) lo = mid + 1;
        else hi = mid;
    }

    int found = 0;
    for (int i = 0; i < srv->ring_size && found < n; i++) {
        int idx = (lo + i) % srv->ring_size;
        uint32_t nid = srv->ring[idx].node_id;

        /* Deduplicate */
        bool dup = false;
        for (int j = 0; j < found; j++) {
            if (out_nodes[j] == nid) { dup = true; break; }
        }
        if (!dup) out_nodes[found++] = nid;
    }

    pthread_mutex_unlock(&srv->lock);
    return found;
}

/* ── Public API: Metrics ───────────────────────────────────────────── */

cnc_metrics_t cnc_get_metrics(cnc_server_t *srv) {
    pthread_mutex_lock(&srv->lock);
    cnc_metrics_t m = srv->metrics;
    pthread_mutex_unlock(&srv->lock);
    return m;
}

void cnc_reset_metrics(cnc_server_t *srv) {
    pthread_mutex_lock(&srv->lock);
    uint64_t start = srv->metrics.start_time;
    memset(&srv->metrics, 0, sizeof(srv->metrics));
    srv->metrics.start_time = start;
    pthread_mutex_unlock(&srv->lock);
}

/* ── Public API: Gossip ────────────────────────────────────────────── */

int cnc_gossip_inject(cnc_server_t *srv, const cnc_gossip_entry_t *entries, int n) {
    pthread_mutex_lock(&srv->lock);
    for (int i = 0; i < n; i++) {
        cnc_node_t *node = find_node(srv, entries[i].node_id);
        if (node && entries[i].version > node->version) {
            cnc_node_state_t old = node->state;
            node->state = entries[i].state;
            node->version = entries[i].version;
            if (old != node->state) {
                if (node->state == CNC_NODE_DEAD || node->state == CNC_NODE_LEFT) {
                    rebuild_ring(srv);
                }
                if (srv->config.on_node_change)
                    srv->config.on_node_change(node, old, srv->config.callback_ctx);
            }
        }
    }
    pthread_mutex_unlock(&srv->lock);
    return 0;
}

/* ── Client Implementation ─────────────────────────────────────────── */

struct cnc_client {
    char              server_addr[48];
    uint16_t          server_port;
    char              name[CNC_MAX_NAME_LEN];
    int               fd;
    uint32_t          node_id;
    volatile bool     running;
    pthread_t         thread;
    cnc_client_task_fn  task_handler;
    void               *task_ctx;
    cnc_client_event_fn event_handler;
    void               *event_ctx;
    uint8_t            recv_buf[CNC_MAX_PAYLOAD + CNC_MSG_HEADER_SIZE];
};

cnc_client_t *cnc_client_create(const char *server_addr, uint16_t port,
                                 const char *node_name) {
    cnc_client_t *cli = calloc(1, sizeof(*cli));
    if (!cli) return NULL;

    strncpy(cli->server_addr, server_addr, sizeof(cli->server_addr) - 1);
    cli->server_port = port;
    strncpy(cli->name, node_name ? node_name : "unnamed", CNC_MAX_NAME_LEN - 1);
    cli->fd = -1;

    return cli;
}

int cnc_client_connect(cnc_client_t *cli) {
    cli->fd = socket(AF_INET, SOCK_STREAM, 0);
    if (cli->fd < 0) return -1;

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(cli->server_port),
    };
    inet_pton(AF_INET, cli->server_addr, &addr.sin_addr);

    if (connect(cli->fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(cli->fd);
        cli->fd = -1;
        return -1;
    }

    set_tcp_nodelay(cli->fd);
    return 0;
}

int cnc_client_register(cnc_client_t *cli, const char **tags, int tag_count) {
    char msg[1024];
    int n = snprintf(msg, sizeof(msg), "{\"name\":\"%s\"", cli->name);
    if (tags && tag_count > 0) {
        n += snprintf(msg + n, sizeof(msg) - n, ",\"tags\":[");
        for (int i = 0; i < tag_count; i++) {
            if (i > 0) msg[n++] = ',';
            n += snprintf(msg + n, sizeof(msg) - n, "\"%s\"", tags[i]);
        }
        msg[n++] = ']';
    }
    msg[n++] = '}';
    msg[n] = '\0';

    if (send_msg(cli->fd, CNC_MSG_REGISTER, msg, n) != 0) return -1;

    /* Wait for ACK */
    cnc_msg_type_t type;
    uint32_t len = 0;
    if (recv_msg(cli->fd, &type, cli->recv_buf, &len, CNC_MAX_PAYLOAD) != 0) return -1;
    if (type != CNC_MSG_REGISTER_ACK) return -1;

    cli->recv_buf[len] = '\0';
    cli->node_id = (uint32_t)json_extract_int((const char *)cli->recv_buf, "node_id");
    return 0;
}

int cnc_client_heartbeat(cnc_client_t *cli) {
    const char *msg = "{\"status\":\"alive\"}";
    return send_msg(cli->fd, CNC_MSG_HEARTBEAT, msg, strlen(msg));
}

int cnc_client_task_complete(cnc_client_t *cli, uint64_t task_id,
                               const char *result, size_t result_len) {
    char msg[CNC_MAX_PAYLOAD];
    int n = snprintf(msg, sizeof(msg), "{\"task_id\":%llu,\"result\":",
                     (unsigned long long)task_id);
    if (result && result_len > 0) {
        size_t rem = sizeof(msg) - n - 2;
        size_t copy = result_len < rem ? result_len : rem;
        memcpy(msg + n, result, copy);
        n += copy;
    } else {
        n += snprintf(msg + n, sizeof(msg) - n, "null");
    }
    msg[n++] = '}';
    return send_msg(cli->fd, CNC_MSG_TASK_COMPLETE, msg, n);
}

int cnc_client_task_fail(cnc_client_t *cli, uint64_t task_id, const char *reason) {
    char msg[512];
    int n = snprintf(msg, sizeof(msg), "{\"task_id\":%llu,\"reason\":\"%s\"}",
                     (unsigned long long)task_id, reason ? reason : "unknown");
    return send_msg(cli->fd, CNC_MSG_TASK_FAIL, msg, n);
}

int cnc_client_publish(cnc_client_t *cli, const char *topic,
                        const char *data, size_t len) {
    char msg[CNC_MAX_PAYLOAD];
    int n = snprintf(msg, sizeof(msg), "{\"topic\":\"%s\",\"data\":", topic);
    if (data && len > 0) {
        msg[n++] = '"';
        size_t rem = sizeof(msg) - n - 3;
        size_t copy = len < rem ? len : rem;
        memcpy(msg + n, data, copy);
        n += copy;
        msg[n++] = '"';
    } else {
        n += snprintf(msg + n, sizeof(msg) - n, "null");
    }
    msg[n++] = '}';
    return send_msg(cli->fd, CNC_MSG_PUBLISH, msg, n);
}

int cnc_client_subscribe(cnc_client_t *cli, const char *topic) {
    char msg[128];
    int n = snprintf(msg, sizeof(msg), "{\"topic\":\"%s\"}", topic);
    return send_msg(cli->fd, CNC_MSG_SUBSCRIBE, msg, n);
}

int cnc_client_set_task_handler(cnc_client_t *cli, cnc_client_task_fn fn, void *ctx) {
    cli->task_handler = fn;
    cli->task_ctx = ctx;
    return 0;
}

int cnc_client_set_event_handler(cnc_client_t *cli, cnc_client_event_fn fn, void *ctx) {
    cli->event_handler = fn;
    cli->event_ctx = ctx;
    return 0;
}

static void *client_loop(void *arg) {
    cnc_client_t *cli = (cnc_client_t *)arg;
    uint64_t last_hb = cnc_now_ms();

    set_nonblocking(cli->fd);

    while (cli->running) {
        cnc_msg_type_t type;
        uint32_t len = 0;

        /* Non-blocking receive */
        if (recv_msg(cli->fd, &type, cli->recv_buf, &len, CNC_MAX_PAYLOAD) == 0) {
            cli->recv_buf[len] = '\0';
            const char *payload = (const char *)cli->recv_buf;

            switch (type) {
                case CNC_MSG_TASK_ASSIGN:
                    if (cli->task_handler) {
                        uint64_t tid = (uint64_t)json_extract_int(payload, "task_id");
                        char ttype[CNC_MAX_NAME_LEN];
                        json_extract_str(payload, "type", ttype, sizeof(ttype));
                        cli->task_handler(tid, ttype, payload, len, cli->task_ctx);
                    }
                    break;
                case CNC_MSG_EVENT:
                    if (cli->event_handler) {
                        char topic[CNC_MAX_TOPIC_LEN];
                        json_extract_str(payload, "topic", topic, sizeof(topic));
                        cli->event_handler(topic, payload, len, cli->event_ctx);
                    }
                    break;
                case CNC_MSG_SHUTDOWN:
                    cli->running = false;
                    break;
                default:
                    break;
            }
        }

        /* Send heartbeat */
        uint64_t now = cnc_now_ms();
        if (now - last_hb > 3000) {
            cnc_client_heartbeat(cli);
            last_hb = now;
        }

        usleep(10000); /* 10ms */
    }

    return NULL;
}

int cnc_client_run(cnc_client_t *cli) {
    cli->running = true;
    client_loop(cli);
    return 0;
}

int cnc_client_run_async(cnc_client_t *cli) {
    cli->running = true;
    return pthread_create(&cli->thread, NULL, client_loop, cli);
}

int cnc_client_stop(cnc_client_t *cli) {
    cli->running = false;
    pthread_join(cli->thread, NULL);
    return 0;
}

void cnc_client_destroy(cnc_client_t *cli) {
    if (!cli) return;
    if (cli->running) cnc_client_stop(cli);
    if (cli->fd >= 0) close(cli->fd);
    free(cli);
}
