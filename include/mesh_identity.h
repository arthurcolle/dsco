#ifndef DSCO_MESH_IDENTITY_H
#define DSCO_MESH_IDENTITY_H

#include <stdbool.h>
#include <stdint.h>

#include "mesh.h"

/* Loads or safely creates the node identity. Keys are 32-byte crypto_box keys. */
bool mesh_identity_load(uint8_t public_key[MESH_PUBKEY_LEN],
                        uint8_t secret_key[MESH_PUBKEY_LEN]);
/* The allowlist is authoritative: false includes a missing or empty list. */
bool mesh_identity_allowed(const uint8_t public_key[MESH_PUBKEY_LEN]);

#endif /* DSCO_MESH_IDENTITY_H */
