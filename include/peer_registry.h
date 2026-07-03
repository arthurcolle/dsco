#ifndef DSCO_PEER_REGISTRY_H
#define DSCO_PEER_REGISTRY_H

#include <stdbool.h>
#include <stddef.h>

/* ── Fleet peer registry for latency-aware tool offload ─────────────────────
 *
 * Discovers dsco fleet nodes (from ~/bridge/fleet/<name>.host ADDR= lines) and
 * tracks per-peer health with the same EWMA-scored model as provider_pool:
 * latency EWMA, success-rate EWMA, in-flight load, and a circuit breaker.
 * peer_registry_pick() hands the concurrent tool executor the best (lowest-
 * scoring) healthy peer to run an offload-safe tool on, load-spreading via the
 * in-flight counter. Unlike provider_pool this is touched from many concurrent
 * tool threads plus the heartbeat thread, so all state is mutex-guarded.
 *
 * Gated by DSCO_OFFLOAD — with it unset the registry stays empty and pick()
 * always returns NULL, so behavior is identical to local-only execution. */

/* True if offload is enabled (DSCO_OFFLOAD set). Cheap; no lock. */
bool peer_registry_enabled(void);

/* Discover fleet peers into the registry. Idempotent; safe to call at startup.
 * Excludes the local node by hostname. No-op if offload is disabled. */
void peer_registry_init(void);

/* Start the detached liveness/latency heartbeat (POST /health probe every
 * ~15s, DSCO_PEER_HEARTBEAT_SECS override), keeping scores fresh so pick()
 * avoids dead peers. Idempotent; no-op if offload is disabled. */
void peer_registry_start_heartbeat(void);

/* Select the best healthy peer to offload to and atomically bump its in-flight
 * count. Writes host into addr[alen] and port into *port, returns the peer's
 * stable name (do not free), or NULL if none are usable / offload is off.
 * Every non-NULL pick MUST be paired with peer_registry_done(). */
const char *peer_registry_pick(char *addr, size_t alen, int *port);

/* Report the outcome of an offloaded call: feeds the latency + success EWMAs
 * and the circuit breaker, and decrements the in-flight count. */
void peer_registry_done(const char *name, bool ok, double rtt_ms);

/* Pure scoring core (no state) — exposed for unit testing and reuse. Lower is
 * better. rtt_ewma_ms<=0 uses a neutral LAN prior; success clamped to [0,1];
 * `available` false returns the disqualifying 1e9. */
double peer_registry_score_components(double rtt_ewma_ms, double success_ewma, int inflight,
                                      bool available);

/* UCB1-style exploration adjustment (pure). Selection minimizes a cost score;
 * this subtracts an exploration bonus that is large for rarely-observed arms
 * and shrinks as an arm accrues observations — so a backend that looked slow
 * once still gets re-probed occasionally, and recovery is detected instead of
 * the greedy policy avoiding it forever. explore_c<=0 disables (returns
 * base_score unchanged). Never drives an available arm's score to negative
 * infinity — the bonus is bounded by explore_c * sqrt(ln(total+1)). */
double bandit_ucb_adjust(double base_score, long arm_pulls, long total_pulls, double explore_c);

/* Structural trust gate for an offloaded tool result (pure). Returns false if
 * the peer's response is untrustworthy on its face — empty, all-whitespace, or
 * an error envelope (e.g. the 403 "tool not permitted" body from a peer running
 * a mismatched/locked-down binary). Deliberately determinism-safe: it does NOT
 * compare content against a local run (offloaded web searches legitimately
 * differ across machines), so there are no false positives on real results. An
 * implausible result is discarded and the turn falls back to local execution,
 * closing the "trust a peer's result blindly" injection surface. */
bool peer_result_plausible(const char *result, size_t len);

/* Speculative-hedge decision rule (pure; no state). Given the done/ok state of
 * a racing local (worker 0) and remote (worker 1) execution, returns:
 *   -1  keep waiting (no success yet and not both finished)
 *    0  take local  (local succeeded — don't wait on remote)
 *    1  take remote (remote succeeded and local hasn't yet)
 *    2  both finished and both failed
 * Returning as soon as either side succeeds is what makes hedged latency
 * min(local, remote). Exposed for unit testing. */
int peer_hedge_decide(bool local_done, bool local_ok, bool remote_done, bool remote_ok);

/* Render a human-readable table (for the /peers command). */
void peer_registry_render(char *out, size_t out_len);

/* Number of discovered peers (for status). */
int peer_registry_count(void);

#endif /* DSCO_PEER_REGISTRY_H */
