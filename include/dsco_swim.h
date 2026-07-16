#ifndef DSCO_SWIM_H
#define DSCO_SWIM_H

#include "mesh.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    DSCO_SWIM_ALIVE = 0,
    DSCO_SWIM_SUSPECT,
    DSCO_SWIM_DEAD,
} dsco_swim_state_t;

typedef uint64_t (*dsco_swim_now_fn)(void *ctx);

typedef struct {
    dsco_swim_now_fn now_ms;
    void            *now_ctx;
    uint64_t         suspicion_timeout_ms;
    int              ping_req_k;
    mesh_node_t     *mesh;
} dsco_swim_config_t;

typedef struct dsco_swim dsco_swim_t;

dsco_swim_t *dsco_swim_create(const dsco_swim_config_t *cfg);
void dsco_swim_destroy(dsco_swim_t *swim);

bool dsco_swim_upsert(dsco_swim_t *swim, const uint8_t node_id[MESH_PUBKEY_LEN],
                      dsco_swim_state_t state, uint64_t incarnation);
bool dsco_swim_mark_suspect(dsco_swim_t *swim, const uint8_t node_id[MESH_PUBKEY_LEN],
                            uint64_t incarnation);
bool dsco_swim_refute(dsco_swim_t *swim, const uint8_t node_id[MESH_PUBKEY_LEN],
                      uint64_t higher_incarnation);
int dsco_swim_tick(dsco_swim_t *swim);

dsco_swim_state_t dsco_swim_state(dsco_swim_t *swim,
                                  const uint8_t node_id[MESH_PUBKEY_LEN]);
uint64_t dsco_swim_incarnation(dsco_swim_t *swim,
                               const uint8_t node_id[MESH_PUBKEY_LEN]);
int dsco_swim_count(dsco_swim_t *swim);

bool dsco_swim_ping(dsco_swim_t *swim, const uint8_t target[MESH_PUBKEY_LEN]);
bool dsco_swim_ping_req(dsco_swim_t *swim, const uint8_t target[MESH_PUBKEY_LEN]);
bool dsco_swim_ack(dsco_swim_t *swim, const uint8_t target[MESH_PUBKEY_LEN]);
bool dsco_swim_handle_message(dsco_swim_t *swim, const uint8_t from[MESH_PUBKEY_LEN],
                              const void *data, size_t len);

#endif /* DSCO_SWIM_H */
