#include "dsco_swim.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#define SWIM_MAX_MEMBERS 64

typedef struct {
    uint8_t           id[MESH_PUBKEY_LEN];
    dsco_swim_state_t state;
    uint64_t          incarnation;
    uint64_t          last_update_ms;
    uint64_t          suspect_deadline_ms;
    bool              occupied;
} dsco_swim_member_t;

struct dsco_swim {
    dsco_swim_config_t cfg;
    dsco_swim_member_t members[SWIM_MAX_MEMBERS];
    int count;
};

static uint64_t swim_wall_ms(void *ctx) {
    (void)ctx;
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)(tv.tv_usec / 1000);
}

static uint64_t swim_now(dsco_swim_t *swim) {
    if (swim && swim->cfg.now_ms)
        return swim->cfg.now_ms(swim->cfg.now_ctx);
    return swim_wall_ms(NULL);
}

static uint64_t swim_suspicion_timeout(dsco_swim_t *swim) {
    return swim && swim->cfg.suspicion_timeout_ms ? swim->cfg.suspicion_timeout_ms : 5000ULL;
}

static int swim_find(dsco_swim_t *swim, const uint8_t id[MESH_PUBKEY_LEN]) {
    if (!swim || !id)
        return -1;
    for (int i = 0; i < SWIM_MAX_MEMBERS; i++) {
        if (swim->members[i].occupied &&
            memcmp(swim->members[i].id, id, MESH_PUBKEY_LEN) == 0)
            return i;
    }
    return -1;
}

static int swim_alloc(dsco_swim_t *swim, const uint8_t id[MESH_PUBKEY_LEN]) {
    if (!swim || !id || swim->count >= SWIM_MAX_MEMBERS)
        return -1;
    for (int i = 0; i < SWIM_MAX_MEMBERS; i++) {
        if (!swim->members[i].occupied) {
            memset(&swim->members[i], 0, sizeof(swim->members[i]));
            memcpy(swim->members[i].id, id, MESH_PUBKEY_LEN);
            swim->members[i].occupied = true;
            swim->members[i].state = DSCO_SWIM_ALIVE;
            swim->members[i].last_update_ms = swim_now(swim);
            swim->count++;
            return i;
        }
    }
    return -1;
}

dsco_swim_t *dsco_swim_create(const dsco_swim_config_t *cfg) {
    dsco_swim_t *swim = calloc(1, sizeof(*swim));
    if (!swim)
        return NULL;
    if (cfg)
        swim->cfg = *cfg;
    if (swim->cfg.ping_req_k <= 0)
        swim->cfg.ping_req_k = 3;
    return swim;
}

void dsco_swim_destroy(dsco_swim_t *swim) {
    free(swim);
}

bool dsco_swim_upsert(dsco_swim_t *swim, const uint8_t node_id[MESH_PUBKEY_LEN],
                      dsco_swim_state_t state, uint64_t incarnation) {
    int idx = swim_find(swim, node_id);
    if (idx < 0)
        idx = swim_alloc(swim, node_id);
    if (idx < 0)
        return false;

    dsco_swim_member_t *m = &swim->members[idx];
    if (incarnation < m->incarnation)
        return true;
    if (incarnation == m->incarnation && m->state == DSCO_SWIM_DEAD && state != DSCO_SWIM_DEAD)
        return true;

    m->state = state;
    m->incarnation = incarnation;
    m->last_update_ms = swim_now(swim);
    m->suspect_deadline_ms =
        state == DSCO_SWIM_SUSPECT ? m->last_update_ms + swim_suspicion_timeout(swim) : 0;
    return true;
}

bool dsco_swim_mark_suspect(dsco_swim_t *swim, const uint8_t node_id[MESH_PUBKEY_LEN],
                            uint64_t incarnation) {
    return dsco_swim_upsert(swim, node_id, DSCO_SWIM_SUSPECT, incarnation);
}

bool dsco_swim_refute(dsco_swim_t *swim, const uint8_t node_id[MESH_PUBKEY_LEN],
                      uint64_t higher_incarnation) {
    int idx = swim_find(swim, node_id);
    if (idx < 0)
        idx = swim_alloc(swim, node_id);
    if (idx < 0)
        return false;
    dsco_swim_member_t *m = &swim->members[idx];
    if (higher_incarnation <= m->incarnation)
        return false;
    m->state = DSCO_SWIM_ALIVE;
    m->incarnation = higher_incarnation;
    m->last_update_ms = swim_now(swim);
    m->suspect_deadline_ms = 0;
    return true;
}

int dsco_swim_tick(dsco_swim_t *swim) {
    if (!swim)
        return 0;
    int changed = 0;
    uint64_t now = swim_now(swim);
    for (int i = 0; i < SWIM_MAX_MEMBERS; i++) {
        dsco_swim_member_t *m = &swim->members[i];
        if (!m->occupied || m->state != DSCO_SWIM_SUSPECT)
            continue;
        if (m->suspect_deadline_ms && now >= m->suspect_deadline_ms) {
            m->state = DSCO_SWIM_DEAD;
            m->last_update_ms = now;
            changed++;
        }
    }
    return changed;
}

dsco_swim_state_t dsco_swim_state(dsco_swim_t *swim,
                                  const uint8_t node_id[MESH_PUBKEY_LEN]) {
    int idx = swim_find(swim, node_id);
    return idx >= 0 ? swim->members[idx].state : DSCO_SWIM_DEAD;
}

uint64_t dsco_swim_incarnation(dsco_swim_t *swim,
                               const uint8_t node_id[MESH_PUBKEY_LEN]) {
    int idx = swim_find(swim, node_id);
    return idx >= 0 ? swim->members[idx].incarnation : 0;
}

int dsco_swim_count(dsco_swim_t *swim) {
    return swim ? swim->count : 0;
}

static bool swim_send(dsco_swim_t *swim, const uint8_t target[MESH_PUBKEY_LEN],
                      const char *verb, const uint8_t subject[MESH_PUBKEY_LEN]) {
    if (!swim || !target || !verb)
        return false;
    char subject_hex[65];
    subject_hex[0] = '\0';
    if (subject) {
        static const char hexdigits[] = "0123456789abcdef";
        for (int i = 0; i < MESH_PUBKEY_LEN; i++) {
            subject_hex[i * 2] = hexdigits[(subject[i] >> 4) & 0xf];
            subject_hex[i * 2 + 1] = hexdigits[subject[i] & 0xf];
        }
        subject_hex[64] = '\0';
    }
    char payload[96];
    snprintf(payload, sizeof(payload), "swim:%s:%s", verb, subject ? subject_hex : "");
    if (!swim->cfg.mesh)
        return true;
#ifdef HAVE_LIBSODIUM
    return mesh_node_send_to(swim->cfg.mesh, target, payload, strlen(payload));
#else
    (void)target;
    (void)payload;
    return false;
#endif
}

bool dsco_swim_ping(dsco_swim_t *swim, const uint8_t target[MESH_PUBKEY_LEN]) {
    return swim_send(swim, target, "ping", NULL);
}

bool dsco_swim_ping_req(dsco_swim_t *swim, const uint8_t target[MESH_PUBKEY_LEN]) {
    if (!swim || !target)
        return false;
    char target_hex[65];
    static const char hexdigits[] = "0123456789abcdef";
    for (int i = 0; i < MESH_PUBKEY_LEN; i++) {
        target_hex[i * 2] = hexdigits[(target[i] >> 4) & 0xf];
        target_hex[i * 2 + 1] = hexdigits[target[i] & 0xf];
    }
    target_hex[64] = '\0';
    char payload[96];
    snprintf(payload, sizeof(payload), "swim:ping_req:%s", target_hex);
    if (!swim->cfg.mesh)
        return true;
#ifdef HAVE_LIBSODIUM
    mesh_peer_info_t peers[MESH_MAX_PEERS];
    int n = mesh_node_peers(swim->cfg.mesh, peers, MESH_MAX_PEERS);
    int sent = 0;
    for (int i = 0; i < n && sent < swim->cfg.ping_req_k; i++) {
        if (memcmp(peers[i].pubkey, target, MESH_PUBKEY_LEN) == 0)
            continue;
        if (mesh_node_send_to(swim->cfg.mesh, peers[i].pubkey, payload, strlen(payload)))
            sent++;
    }
    return sent > 0;
#else
    (void)payload;
    return false;
#endif
}

bool dsco_swim_ack(dsco_swim_t *swim, const uint8_t target[MESH_PUBKEY_LEN]) {
    return swim_send(swim, target, "ack", NULL);
}

bool dsco_swim_handle_message(dsco_swim_t *swim, const uint8_t from[MESH_PUBKEY_LEN],
                              const void *data, size_t len) {
    if (!swim || !from || !data || len == 0 || len >= 128)
        return false;
    char msg[128];
    memcpy(msg, data, len);
    msg[len] = '\0';
    if (strncmp(msg, "swim:ping", 9) == 0) {
        dsco_swim_upsert(swim, from, DSCO_SWIM_ALIVE, dsco_swim_incarnation(swim, from));
        return dsco_swim_ack(swim, from);
    }
    if (strncmp(msg, "swim:ack", 8) == 0)
        return dsco_swim_upsert(swim, from, DSCO_SWIM_ALIVE,
                                dsco_swim_incarnation(swim, from));
    if (strncmp(msg, "swim:suspect:", 13) == 0) {
        uint64_t inc = strtoull(msg + 13, NULL, 10);
        return dsco_swim_mark_suspect(swim, from, inc);
    }
    if (strncmp(msg, "swim:alive:", 11) == 0) {
        uint64_t inc = strtoull(msg + 11, NULL, 10);
        return dsco_swim_refute(swim, from, inc);
    }
    return false;
}
