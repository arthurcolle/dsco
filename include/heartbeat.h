#ifndef DSCO_HEARTBEAT_H
#define DSCO_HEARTBEAT_H

/* ── Heartbeat Beacon ──────────────────────────────────────────────────────
 *
 * Periodic background thread that:
 *   1. Writes a watchdog_ping() timestamp (local liveness).
 *   2. Optionally POSTs a JSON beacon to a configured endpoint so a remote
 *      fleet manager knows this node is alive.
 *   3. Includes a signed nonce so the receiver can verify identity.
 *
 * Configuration via environment / sealed_store:
 *   DSCO_BEACON_URL   — HTTPS endpoint to POST to (empty → local only)
 *   DSCO_NODE_ID      — human name for this node (default: hostname)
 *   DSCO_BEACON_SECS  — interval in seconds (default: 60)
 *
 * Beacon JSON payload:
 *   { "node": "...", "ts": 1234567890, "seq": 42,
 *     "uptime_s": 300, "sig": "<hex-hmac>" }
 *
 * The HMAC signs ts+seq+node using the DSCO_NET_AUTH_KEY or mesh keypair.
 * ─────────────────────────────────────────────────────────────────────── */

#include <stdbool.h>

/* Start the beacon background thread.  No-op if already running. */
void heartbeat_start(void);

/* Stop the beacon thread (blocks until it exits). */
void heartbeat_stop(void);

/* True if the beacon thread is running. */
bool heartbeat_running(void);

/* Force an immediate beacon emission (still async). */
void heartbeat_poke(void);

#endif /* DSCO_HEARTBEAT_H */
