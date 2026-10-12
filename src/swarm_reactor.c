#define _POSIX_C_SOURCE 200809L
#include "swarm_reactor.h"
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>
#ifdef __APPLE__
#include <fcntl.h>
#include <sys/event.h>
#endif

static long long monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int remaining_ms(int timeout_ms, long long deadline) {
    if (timeout_ms < 0)
        return -1;
    long long remaining = deadline - monotonic_ms();
    return remaining > 0 ? (int)remaining : 0;
}

static int poll_until(struct pollfd *fds, nfds_t count, int timeout_ms,
                      long long deadline) {
    for (;;) {
        int result = poll(fds, count, remaining_ms(timeout_ms, deadline));
        if (result >= 0) {
            /* Darwin coalesces duplicate descriptors, including revents.
             * Restore poll's per-entry contract before counting readiness. */
            for (nfds_t i = 0; i < count; ++i) {
                if (fds[i].fd < 0) continue;
                short observed = 0;
                for (nfds_t j = 0; j < count; ++j)
                    if (fds[j].fd == fds[i].fd) observed |= fds[j].revents;
                fds[i].revents |= observed &
                    (fds[i].events | POLLERR | POLLHUP | POLLNVAL);
            }
            int ready = 0;
            for (nfds_t i = 0; i < count; ++i)
                if (fds[i].revents) ++ready;
            return ready;
        }
        if (errno != EINTR)
            return result;
        if (timeout_ms >= 0 && remaining_ms(timeout_ms, deadline) == 0)
            return 0;
    }
}

#ifdef __APPLE__
static int watch_event(int queue, uintptr_t ident, int16_t filter,
                       uint32_t flags) {
    struct kevent change;
    EV_SET(&change, ident, filter, EV_ADD | EV_CLEAR, flags, 0, NULL);
    /* With no event list this only installs the filter, never waits. */
    int result;
    do {
        result = kevent(queue, &change, 1, NULL, 0, NULL);
    } while (result < 0 && errno == EINTR);
    return result;
}

static int wait_kqueue(struct pollfd *fds, nfds_t count, const pid_t *pids,
                       size_t pid_count, int timeout_ms, long long deadline) {
    int queue = kqueue();
    if (queue < 0)
        return -1;
    int result = -1;
    if (fcntl(queue, F_SETFD, FD_CLOEXEC) < 0)
        goto done;
    for (nfds_t i = 0; i < count; ++i) {
        if (fds[i].fd < 0)
            continue;
        if ((fds[i].events & (POLLIN | POLLRDNORM)) &&
            watch_event(queue, (uintptr_t)fds[i].fd, EVFILT_READ, 0) < 0)
            goto done;
        if ((fds[i].events & (POLLOUT | POLLWRNORM)) &&
            watch_event(queue, (uintptr_t)fds[i].fd, EVFILT_WRITE, 0) < 0)
            goto done;
        if ((fds[i].events & (POLLPRI | POLLRDBAND)) &&
            watch_event(queue, (uintptr_t)fds[i].fd, EVFILT_EXCEPT, NOTE_OOB) < 0)
            goto done;
    }
    for (size_t i = 0; i < pid_count; ++i) {
        if (pids[i] <= 0)
            continue;
        if (watch_event(queue, (uintptr_t)pids[i], EVFILT_PROC, NOTE_EXIT) < 0) {
            /* Includes the race where a child exits/is reaped before EV_ADD.
             * Wake the caller to reconcile its children; never reap here. */
            if (errno == ESRCH)
                result = 1;
            goto done;
        }
    }
    for (;;) {
        int remaining = remaining_ms(timeout_ms, deadline);
        struct timespec delay = {0, 0};
        if (remaining >= 0) {
            delay.tv_sec = remaining / 1000;
            delay.tv_nsec = (remaining % 1000) * 1000000L;
        }
        struct kevent event;
        int n = kevent(queue, NULL, 0, &event, 1,
                       remaining < 0 ? NULL : &delay);
        if (n < 0) {
            if (errno != EINTR)
                goto done;
            if (timeout_ms >= 0 && remaining_ms(timeout_ms, deadline) == 0) {
                result = 0;
                goto done;
            }
            continue;
        }
        if (n == 0) {
            result = 0;
            goto done;
        }
        if (event.flags & EV_ERROR) {
            errno = (int)event.data;
            goto done;
        }
        /* poll supplies its native revents/count (including EOF, errors and
         * duplicate entries); kqueue is only the combined wakeup mechanism. */
        result = poll_until(fds, count, 0, monotonic_ms());
        if (result != 0)
            goto done;
        if (event.filter == EVFILT_PROC && (event.fflags & NOTE_EXIT)) {
            result = 1;
            goto done;
        }
        if (timeout_ms >= 0 && remaining_ms(timeout_ms, deadline) == 0)
            goto done;
    }
done:;
    int saved_errno = errno;
    close(queue);
    errno = saved_errno;
    return result;
}
#else
static int child_exited(const pid_t *pids, size_t pid_count) {
    for (size_t i = 0; i < pid_count; ++i) {
        if (pids[i] <= 0)
            continue;
        siginfo_t info = {0};
        if (waitid(P_PID, (id_t)pids[i], &info, WEXITED | WNOHANG | WNOWAIT) == 0) {
            if (info.si_pid != 0)
                return 1;
        } else if (errno == ECHILD) {
            if (kill(pids[i], 0) < 0 && errno == ESRCH)
                return 1;
        } else if (errno != EINTR) {
            return -1;
        }
    }
    return 0;
}

static int wait_portable(struct pollfd *fds, nfds_t count, const pid_t *pids,
                         size_t pid_count, int timeout_ms, long long deadline) {
    /* No pidfd implementation in this portability path (including Linux).
     * Bounded 100ms maintenance checks detect exits without reaping or changing
     * SIGCHLD. They are internal only: never truncate the caller's timeout or
     * return a spurious wake. Fd-only waits bypass this fallback entirely. */
    for (;;) {
        int result = child_exited(pids, pid_count);
        if (result != 0)
            return result;
        int remaining = remaining_ms(timeout_ms, deadline);
        if (remaining == 0)
            return 0;
        int interval = remaining < 0 || remaining > 100 ? 100 : remaining;
        result = poll(fds, count, interval);
        if (result > 0 || (result < 0 && errno != EINTR))
            return result;
    }
}
#endif

int swarm_reactor_wait_children(struct pollfd *fds, nfds_t count,
                               const pid_t *pids, size_t pid_count,
                               int timeout_ms) {
    if ((!fds && count) || (!pids && pid_count)) {
        errno = EINVAL;
        return -1;
    }
    long long deadline = timeout_ms < 0 ? 0 : monotonic_ms() + timeout_ms;
    int fd_interest = 0, child_interest = 0;
    for (nfds_t i = 0; i < count; ++i) {
        fds[i].revents = 0;
        if (fds[i].fd >= 0 && fds[i].events != 0)
            fd_interest = 1;
    }
    for (size_t i = 0; i < pid_count; ++i)
        if (pids[i] > 0)
            child_interest = 1;
    int result = poll_until(fds, count, 0, monotonic_ms());
    if (result != 0 || (!fd_interest && !child_interest))
        return result;
    if (!child_interest)
        return poll_until(fds, count, timeout_ms, deadline);
#ifdef __APPLE__
    return wait_kqueue(fds, count, pids, pid_count, timeout_ms, deadline);
#else
    return wait_portable(fds, count, pids, pid_count, timeout_ms, deadline);
#endif
}

int swarm_reactor_wait(struct pollfd *fds, nfds_t count, int timeout_ms) {
    return swarm_reactor_wait_children(fds, count, NULL, 0, timeout_ms);
}
