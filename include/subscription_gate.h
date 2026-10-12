#ifndef DSCO_SUBSCRIPTION_GATE_H
#define DSCO_SUBSCRIPTION_GATE_H

#include <stdbool.h>
#include <stdint.h>

void subscription_gate_set_wait_hook(void (*hook)(const char *));

/* Cross-process admission gate for one subscription swimlane.
 *
 * The gate is keyed by a non-secret hash of `scope` (normally account + model
 * swimlane). Each acquired request holds one advisory-lock permit until
 * release. DSCO_CHATGPT_LANE_CONCURRENCY controls permits per swimlane
 * (default 4, range 1..512), so independent dsco processes, swarm children and
 * fabric workers can make bounded progress without one account-global mutex.
 * A separate swimlane cooldown is persisted so Retry-After learned by one
 * process is honored by every permit in that swimlane.
 *
 * Admission/permit filesystem errors fail closed. Pending requests are session-fair
 * with bounded interactive preference and aging; metadata locks are short-lived. Cancellation returns false with
 * errno=EINTR; exceeding DSCO_CHATGPT_GATE_MAX_WAIT_MS across lock contention
 * and cooldown returns false with errno=ETIMEDOUT. waited_ms includes failed
 * waits and uses a monotonic clock. Set DSCO_CHATGPT_GLOBAL_GATE=0 to opt out. */
typedef struct {
    int fd;
    int legacy_fd;
    bool legacy_held;
    bool held;
    uint64_t scope_hash;
    unsigned slot;
    unsigned capacity;
} subscription_gate_t;

bool subscription_gate_acquire(subscription_gate_t *gate, const char *scope,
                               const volatile int *interrupted, long *waited_ms);

/* Release the lease and prevent the next request from starting for at least
 * cooldown_ms. Successful requests pass 0 and have no synthetic delay by
 * default. DSCO_CHATGPT_MIN_INTERVAL_MS can add operator-selected spacing. */
void subscription_gate_release(subscription_gate_t *gate, long cooldown_ms);

#endif /* DSCO_SUBSCRIPTION_GATE_H */
