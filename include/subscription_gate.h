#ifndef DSCO_SUBSCRIPTION_GATE_H
#define DSCO_SUBSCRIPTION_GATE_H

#include <stdbool.h>

/* Cross-process gate for a shared subscription lane.
 *
 * The gate is keyed by a non-secret hash of `scope` (normally the provider
 * account id), and holds an advisory filesystem lock until release. This
 * keeps independent dsco processes, swarm children, and fabric workers from
 * opening concurrent streams against the same flat-rate account. A release
 * cooldown is persisted in the lock file so Retry-After learned by one
 * process is honored by all of them.
 *
 * Operational filesystem errors fail open. Cancellation returns false with
 * errno=EINTR; exceeding DSCO_CHATGPT_GATE_MAX_WAIT_MS across lock contention
 * and cooldown returns false with errno=ETIMEDOUT. waited_ms includes failed
 * waits and uses a monotonic clock. Set DSCO_CHATGPT_GLOBAL_GATE=0 to opt out. */
typedef struct {
    int fd;
    bool held;
} subscription_gate_t;

bool subscription_gate_acquire(subscription_gate_t *gate, const char *scope,
                               const volatile int *interrupted, long *waited_ms);

/* Release the lease and prevent the next request from starting for at least
 * cooldown_ms. Successful requests pass 0 and have no synthetic delay by
 * default. DSCO_CHATGPT_MIN_INTERVAL_MS can add operator-selected spacing. */
void subscription_gate_release(subscription_gate_t *gate, long cooldown_ms);

#endif /* DSCO_SUBSCRIPTION_GATE_H */
