#include "swarm_reactor.h"
#include <errno.h>
#include <time.h>

static long long monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int swarm_reactor_wait(struct pollfd *fds, nfds_t count, int timeout_ms) {
    /* Even silent workers need reaping/accounting every 100ms. Negative
     * timeouts mean indefinite caller interest, not indefinite maintenance. */
    int bounded = timeout_ms < 0 || timeout_ms > 100 ? 100 : timeout_ms;
    long long deadline = monotonic_ms() + bounded;
    for (;;) {
        int result = poll(fds, count, bounded);
        if (result >= 0 || errno != EINTR)
            return result;
        long long remaining = deadline - monotonic_ms();
        if (remaining <= 0)
            return 0;
        bounded = (int)remaining;
    }
}
