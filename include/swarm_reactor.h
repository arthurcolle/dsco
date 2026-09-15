#ifndef DSCO_SWARM_REACTOR_H
#define DSCO_SWARM_REACTOR_H
#include <poll.h>
/* Readiness wait bounded by maintenance deadline; retries EINTR without
 * extending the caller's deadline. No tool execution or background threads. */
int swarm_reactor_wait(struct pollfd *fds, nfds_t count, int timeout_ms);
#endif
