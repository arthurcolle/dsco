#ifndef DSCO_DHT_H
#define DSCO_DHT_H

/* ── Distributed peer discovery (Kademlia / BitTorrent DHT) ────────────────
 *
 * Phase 1: WAN peer discovery over a PRIVATE overlay, bridged to the existing
 * encrypted mesh. Wraps the vendored dht.c (Juliusz Chroboczek).
 *
 *   • Node identity : 20-byte id, persisted at ~/.dsco/dht_id
 *   • Rendezvous    : infohash = sha256(swarm_key)[:20]; all nodes sharing the
 *                     swarm key converge on the same key in the keyspace
 *   • Announce      : we announce our mesh TCP port on the rendezvous infohash
 *   • Discovery     : peers found via get_peers are written to ~/.dsco/peers.txt
 *                     and peer_bootstrap_reseed() dials them over the mesh
 *
 * Private overlay: bootstrap only from configured nodes (DSCO_DHT_BOOTSTRAP /
 * ~/.dsco/dht_bootstrap.txt / dsco_dht_bootstrap()). It never contacts the
 * public BitTorrent network unless you point it there.
 *
 * Gated on HAVE_LIBSODIUM (uses randombytes + crypto_hash_sha256). When built
 * without libsodium, all functions are no-ops / return NULL.
 *
 * Phase 2 (not yet): a key→value blob overlay on top of this routing.
 * ─────────────────────────────────────────────────────────────────────── */

#include <stdbool.h>
#include <stdint.h>

typedef struct dsco_dht dsco_dht_t;

typedef struct {
    uint16_t    udp_port;    /* DHT UDP port to bind (0 → default 7600) */
    uint16_t    mesh_port;   /* mesh TCP port to announce (what peers dial) */
    const char *swarm_key;   /* shared secret string → rendezvous infohash */
} dsco_dht_config_t;

typedef struct {
    int  good, dubious, cached, incoming;  /* routing-table node counts */
    int  peers_found;                       /* distinct peers discovered */
    int  searches;                          /* searches launched */
    bool running;
} dsco_dht_stats_t;

/* Bind the UDP socket, load/generate the persistent node id, seed bootstrap
 * nodes (arg-less sources: DSCO_DHT_BOOTSTRAP + ~/.dsco/dht_bootstrap.txt),
 * and start the background poll thread. Returns NULL on failure or when built
 * without libsodium. Idempotent: a second call returns the existing instance. */
dsco_dht_t *dsco_dht_start(const dsco_dht_config_t *cfg);

/* Add a bootstrap node "host:port" (DNS resolved). Safe before or after start. */
bool dsco_dht_bootstrap(dsco_dht_t *d, const char *host, uint16_t port);

/* Force a (re)search + announce for the swarm rendezvous immediately. */
void dsco_dht_find_peers(dsco_dht_t *d);

/* Snapshot current stats. */
void dsco_dht_get_stats(dsco_dht_t *d, dsco_dht_stats_t *out);

/* Stop the poll thread and release the socket. */
void dsco_dht_stop(dsco_dht_t *d);

/* Process-global handle (set by dsco_dht_start), for tool/slash access. */
dsco_dht_t *dsco_dht_global(void);

#endif /* DSCO_DHT_H */
