#ifndef DSCO_PROVIDER_POOL_H
#define DSCO_PROVIDER_POOL_H

#include <stdbool.h>
#include <stddef.h>
#include <time.h>
#include "provider.h"

/* Durable provider pool.
 *
 * Keeps a set of live provider_t instances "always up" — each with its own
 * warm transport (persistent CURL handle via provider_prepare) and resolved
 * credential — so the agent can route between providers (e.g. flat-rate or
 * coding-plan lanes: Anthropic/Claude, OpenAI/ChatGPT, Sakana/Fugu, Z.AI)
 * without tearing down and rebuilding the connection on every switch.
 *
 * The pool OWNS every provider_t it hands out: callers must never provider_free()
 * a pooled provider. provider_pool_shutdown() frees them all.
 *
 * It also tracks per-provider health (latency, failures) with a simple circuit
 * breaker so routing/fallback can avoid a provider that is currently failing,
 * and so /providers can show the live state of each subscription. */

#define PROVIDER_POOL_MAX 32

typedef enum {
    POOL_SLOT_EMPTY = 0, /* unused slot */
    POOL_SLOT_UP,        /* prepared, credential present, healthy */
    POOL_SLOT_NOKEY,     /* known provider but no usable credential */
    POOL_SLOT_TRIPPED,   /* circuit breaker open after repeated failures */
} pool_slot_state_t;

typedef struct {
    char name[64];           /* canonical provider name */
    char default_model[128]; /* provider's default model id */
    provider_t *provider;    /* live, prepared; pool-owned (NULL until acquired) */
    pool_slot_state_t state;
    bool is_subscription; /* flat-rate core subscription (zero marginal) */
    double last_latency_ms;
    double lat_ewma_ms;  /* EWMA of request latency (0 = no data) */
    double success_ewma; /* EWMA of success rate (1.0 = optimistic) */
    int consec_failures;
    long total_requests;
    long total_failures;
    time_t tripped_until;   /* 0 = not tripped */
    time_t exhausted_until; /* subscription allocation reset; 0 = available/unknown */
    time_t last_used;
} provider_slot_t;

typedef struct {
    provider_slot_t slots[PROVIDER_POOL_MAX];
    int count;
    char session_key[512]; /* CLI -k fallback credential (may be empty) */
    bool initialized;
} provider_pool_t;

/* Process-wide singleton. */
provider_pool_t *provider_pool(void);

/* Register + warm the core subscriptions and any provider with a usable key.
 * session_key is the CLI -k fallback (may be NULL). Idempotent: safe to call
 * more than once; later calls refresh credential/health without rebuilding
 * already-warm transports. */
void provider_pool_init(const char *session_key);

/* Return a warm, prepared provider for `name` (canonicalized), creating and
 * preparing it on first use and caching it for reuse. Returns NULL only if the
 * provider name is unknown/uncreatable. The returned provider_t is owned by the
 * pool — do NOT free it. */
provider_t *provider_pool_acquire(const char *name);

/* Slot lookup for a provider (canonicalized), or NULL if not registered. */
provider_slot_t *provider_pool_slot(const char *name);

/* Record a request outcome for health + circuit-breaker tracking. */
void provider_pool_report(const char *name, bool ok, double latency_ms);

/* Persist and query provider-supplied subscription exhaustion reset times. */
void provider_pool_mark_subscription_exhausted(const char *name, time_t exhausted_until);
time_t provider_pool_subscription_exhausted_until(const char *name);

/* True if the provider is registered, has a credential, and is not tripped. */
bool provider_pool_healthy(const char *name);

/* Render a human-readable status table into out (for the /providers command). */
void provider_pool_render(char *out, size_t out_len);

/* Free all pooled providers and reset the pool. */
void provider_pool_shutdown(void);

/* Fire-and-forget: on a detached background thread, open a TLS connection to
 * each warm provider's endpoint so the shared DNS + TLS-session cache is
 * primed before the first real request. The first turn then resumes TLS
 * instead of paying a full cold handshake (~50-150ms saved per provider).
 * Snapshots endpoint URLs on the calling thread, so it is safe to invoke
 * right after provider_pool_init() without racing later pool mutation. */
void provider_pool_prewarm_async(void);

/* Start the connection keep-warm heartbeat: a detached thread that re-warms
 * pooled provider connections on an interval (default 45s, override with
 * DSCO_KEEPWARM_SECS) so a warm socket survives NAT/proxy idle timeouts and
 * the next turn after a quiet gap doesn't pay a cold handshake. Idempotent;
 * disabled entirely by DSCO_NO_PREWARM. Warms lowest-scoring (least healthy /
 * slowest) providers first so the connections most at risk get priority. */
void provider_pool_keepwarm_start(void);

/* ── Latency-aware scoring ──────────────────────────────────────────────────
 * Lower is better. A provider's score blends its EWMA latency with a penalty
 * for its recent failure rate, discounted for zero-marginal-cost subscription
 * lanes. Unavailable providers (tripped / exhausted / unkeyed) score 1e9. */

/* Pure scoring core — no pool state; exposed for unit testing and reuse.
 * lat_ewma_ms<=0 is treated as a neutral prior; success_ewma is clamped to
 * [0,1]; `available` false returns the disqualifying 1e9. */
double provider_pool_score_components(double lat_ewma_ms, double success_ewma, bool is_subscription,
                                      bool available);

/* Blended score for a registered provider (1e9 if unknown/unavailable). */
double provider_pool_score(const char *name);

/* Of the given candidate providers, return the one with the best (lowest)
 * score that is currently healthy, or NULL if none are usable. Names are
 * canonicalized. Does not mutate the pool. */
const char *provider_pool_fastest_healthy(const char *const *names, int n);

/* Reorder a statically-built fallback model chain in place by each model's
 * provider's live score, best-first — making the fallback order dynamic
 * instead of a fixed priority list. No-op before the pool is initialized or
 * when all candidates score equally, so the static default stands until
 * runtime signal accumulates. Stable across equal scores. `n` is the number
 * of populated entries (max 16). */
void provider_pool_rerank_fallbacks(char out_models[][128], int n);

#endif /* DSCO_PROVIDER_POOL_H */
