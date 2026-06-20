#ifndef DSCO_MESH_H
#define DSCO_MESH_H

/* ── Encrypted peer-to-peer mesh ──────────────────────────────────────────
 *
 * Transport : POSIX TCP sockets + pthreads
 * Crypto    : libsodium crypto_box (Curve25519 / XSalsa20-Poly1305)
 *
 * Wire protocol per connection:
 *   Handshake (36 bytes, each side sends immediately after connect):
 *     [uint32_t MESH_MAGIC big-endian] [uint8_t pubkey[32]]
 *
 *   Message frame:
 *     [uint32_t wire_body_len big-endian]      -- NONCE_LEN + ciphertext
 *     [uint8_t nonce[24]]
 *     [ciphertext of: uint8_t type | uint32_t payload_len | payload]
 * ─────────────────────────────────────────────────────────────────────── */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MESH_PUBKEY_LEN   32
#define MESH_MAX_PEERS    64
#define MESH_MAX_PAYLOAD  (128 * 1024)
#define MESH_MAGIC        0x44534D48u  /* "DSMH" */

typedef struct {
    uint8_t pubkey[MESH_PUBKEY_LEN];
    char    addr[48];
    bool    outbound;
} mesh_peer_info_t;

typedef void (*mesh_on_message_fn)(const uint8_t *from_pk,
                                   const void *data, size_t len,
                                   void *ctx);
typedef void (*mesh_on_peer_fn)(const mesh_peer_info_t *peer, void *ctx);

typedef struct mesh_node mesh_node_t;

mesh_node_t   *mesh_node_create(uint16_t port);
void           mesh_node_destroy(mesh_node_t *n);
bool           mesh_node_start(mesh_node_t *n);
void           mesh_node_stop(mesh_node_t *n);
const uint8_t *mesh_node_pubkey(mesh_node_t *n);

bool mesh_node_connect(mesh_node_t *n, const char *host, uint16_t port);
bool mesh_node_send_to(mesh_node_t *n, const uint8_t *peer_pk,
                       const void *data, size_t len);
int  mesh_node_broadcast(mesh_node_t *n, const void *data, size_t len);
int  mesh_node_peers(mesh_node_t *n, mesh_peer_info_t *out, int max);

void mesh_node_set_on_message(mesh_node_t *n, mesh_on_message_fn cb, void *ctx);
void mesh_node_set_on_connect(mesh_node_t *n, mesh_on_peer_fn cb, void *ctx);
void mesh_node_set_on_disconnect(mesh_node_t *n, mesh_on_peer_fn cb, void *ctx);

void mesh_pubkey_to_hex(const uint8_t *pk, char out[65]);
bool mesh_pubkey_from_hex(const char *hex, uint8_t pk[MESH_PUBKEY_LEN]);

#endif /* DSCO_MESH_H */
