#ifndef DSCO_PROVIDER_ADMISSION_H
#define DSCO_PROVIDER_ADMISSION_H
#include <stdbool.h>
#include <stdint.h>
/* Short-lived metadata coordination; never held during a provider stream.
 * A locked ticket identifies a live waiter without trusting PID reuse. */
typedef struct {
    int state_fd, ticket_fd;
    char directory[1400], ticket[1500];
    uint64_t session;
    bool foreground;
    bool locked;
} provider_admission_t;
/* EAGAIN means retry within the caller's cancellation/deadline budget. */
bool provider_admission_join(provider_admission_t *q, const char *base);
/* 1: caller is in the next fair admission cohort, metadata lock retained;
 * 0: wait; 2: short-lived metadata collision, retry quickly; -1: state error.
 * `window` should match the downstream permit capacity so a wide lane can fill
 * without one polling interval per permit. */
int provider_admission_choose(provider_admission_t *q, unsigned window);
void provider_admission_decide(provider_admission_t *q, bool admitted);
void provider_admission_leave(provider_admission_t *q);
/* Children retain the original session's share, but are background work. */
void provider_admission_inherit_session(void);
#endif
