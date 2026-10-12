#ifndef DSCO_SWARM_REACTOR_H
#define DSCO_SWARM_REACTOR_H

#include <poll.h>
#include <stddef.h>
#include <sys/types.h>

/* Wait for fd readiness or an exit of any positive pid (nonpositive pids and
 * negative fds are ignored). Returns the number of ready pollfd entries, or 1
 * for a child-only wake; 0 on timeout/no interests, -1 on error with errno set.
 * Inspect revents and reap children separately: this API never consumes status.
 * Negative timeout waits indefinitely, zero probes, positive values are a
 * monotonic deadline across EINTR. No fds with events and no positive pids means
 * an immediate return, even for an indefinite timeout. EOF is a ready event.
 * No signal dispositions are changed. On macOS exits use kqueue NOTE_EXIT;
 * the portable no-pidfd fallback checks children at bounded intervals internally.
 */
int swarm_reactor_wait_children(struct pollfd *fds, nfds_t count,
                               const pid_t *pids, size_t pid_count,
                               int timeout_ms);

/* Fd-only compatibility wrapper, with the same (unclamped) timeout semantics. */
int swarm_reactor_wait(struct pollfd *fds, nfds_t count, int timeout_ms);
#endif
