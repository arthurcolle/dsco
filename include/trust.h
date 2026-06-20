#ifndef DSCO_TRUST_H
#define DSCO_TRUST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ──────────────────────────────────────────────────────────────────────────
 *  Trust telemetry — signed metadata to trust.distributed.systems
 *
 *  What we send (and only this):
 *    - fingerprint_id (SHA-256, not the raw UUID)
 *    - dsco version + binary SHA-256
 *    - host capability features (neon, amx, neural_engine, etc.)
 *    - opaque session counters (uptime, project count, no names/paths)
 *
 *  How we send it:
 *    - HTTPS POST to a configurable endpoint (default trust.distributed.systems)
 *    - HMAC-SHA256 signed (key from sealed_store or DSCO_TRUST_KEY env)
 *    - From a dedicated background thread; never blocks the main loop
 *    - On network failure: spooled to ~/.dsco/trust/pending/, retried with
 *      exponential backoff (cap 1 hour, max queue 256 events)
 *
 *  Privacy:
 *    DSCO_TRUST_OPT_OUT=1       — disable entirely
 *    DSCO_TRUST_URL=<url>       — override endpoint
 *    DSCO_TRUST_INCLUDE_UUID=1  — opt-in to send raw device UUID
 *    DSCO_TRUST_DRY_RUN=1       — log payloads to stderr instead of POSTing
 * ────────────────────────────────────────────────────────────────────────── */

#define DSCO_TRUST_DEFAULT_URL "https://trust.distributed.systems/v1/attest"

typedef struct {
    char    endpoint_url[256];
    char    hmac_key_hex[129];      /* SHA-256-sized; hex-encoded */
    bool    opt_out;
    bool    include_uuid;
    bool    dry_run;
    int     queue_max;
    int     retry_max_seconds;
} dsco_trust_config_t;

void dsco_trust_default_config(dsco_trust_config_t *cfg);

/* Lifecycle. Starts the background sender thread. Returns 0 on success. */
int  dsco_trust_init(const dsco_trust_config_t *cfg);
void dsco_trust_shutdown(void);

/* Send the fingerprint attestation. Builds the payload from the cached
 * fingerprint, signs it, and enqueues. Returns 0 on accepted, <0 on error. */
int  dsco_trust_emit_attest(void);

/* Periodic runtime heartbeat (mux project count, session token total, etc).
 * Pass NULL fields you don't have. */
typedef struct {
    int      projects_active;
    int      projects_total;
    uint64_t tokens_in;
    uint64_t tokens_out;
    uint64_t bytes_in;
    uint64_t bytes_out;
    int      cents_spent;
} dsco_trust_runtime_t;

int  dsco_trust_emit_heartbeat(const dsco_trust_runtime_t *r);

/* Caller-driven custom event (event_type is short ASCII, payload is JSON). */
int  dsco_trust_emit(const char *event_type, const char *json_payload);

/* Stats for the status overlay. */
typedef struct {
    uint64_t sent_ok;
    uint64_t sent_failed;
    uint64_t queued;
    uint64_t dropped;
    int64_t  last_send_unix;
    char     last_error[128];
} dsco_trust_stats_t;
void dsco_trust_stats(dsco_trust_stats_t *out);

/* The configured endpoint, for diagnostics. NULL if init didn't run. */
const char *dsco_trust_endpoint(void);
bool        dsco_trust_is_active(void);

#endif /* DSCO_TRUST_H */
