#ifndef DSCO_CANCEL_WAIT_H
#define DSCO_CANCEL_WAIT_H
#include <stdbool.h>
typedef struct cancel_wait cancel_wait_t;
/* Flags outlive registrations. Register before checking the predicate. Signal
 * sets the sticky flag and wakes attached operations. Reset only after their
 * completion/join. Not async-signal-safe. Registrations have one owner. */
cancel_wait_t *cancel_wait_open(const volatile int *local, const volatile int *global);
void cancel_wait_close(cancel_wait_t *wait);
int cancel_wait_signal(volatile int *flag);
bool cancel_wait_requested(const cancel_wait_t *wait);
/* Kernel wait for data, cancellation or explicit observation deadline.
 * Negative timeout is indefinite; EINTR does not restart the budget.
 * Returns 1 ready, 0 deadline, -1 error. ECANCELED wins over simultaneous data. */
int cancel_wait_poll(cancel_wait_t *wait, int fd, short events, int timeout_ms);
#endif
