#ifndef DSCO_PEER_BOOTSTRAP_H
#define DSCO_PEER_BOOTSTRAP_H

/* ── Peer Bootstrap / LAN Discovery ───────────────────────────────────────
 *
 * Automatic mesh peer discovery on the local network, using two mechanisms:
 *
 *  1. mDNS/DNS-SD (macOS: dns_sd / Bonjour; Linux: avahi-client or raw mDNS)
 *     Advertises _dsco._tcp.local. with TXT record containing the node's
 *     mesh public key.  Resolves peers that advertise the same service.
 *
 *  2. Flat-file seed list  (DSCO_PEERS env var or ~/.dsco/peers.txt)
 *     One "host:port" per line.  Attempted at startup and periodically
 *     re-read so new entries take effect without restart.
 *
 * When a new peer is found, peer_bootstrap calls mesh_node_connect() so
 * the mesh layer handles the cryptographic handshake.
 *
 * Usage:
 *   peer_bootstrap_init(node, port)   — start advertising + discovery
 *   peer_bootstrap_stop()             — tear down
 * ─────────────────────────────────────────────────────────────────────── */

#include <stdint.h>

/* Start mDNS advertisement on *port* and background peer resolution.
 * node must be the mesh_node_t* returned by mesh_node_create().
 * No-op if HAVE_LIBSODIUM is not defined. */
void peer_bootstrap_init(void *mesh_node, uint16_t port);

/* Stop advertisement and resolver threads. */
void peer_bootstrap_stop(void);

/* Re-read the flat-file seed list and attempt to connect new peers.
 * Called internally on a timer; also safe to call from the main loop. */
void peer_bootstrap_reseed(void);

#endif /* DSCO_PEER_BOOTSTRAP_H */
