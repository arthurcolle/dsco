#define _DARWIN_C_SOURCE 1
#define _GNU_SOURCE 1
#include "shell_job_events.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#ifdef __APPLE__
#include <sys/event.h>
#elif defined(__linux__)
#include <poll.h>
#include <sys/inotify.h>
#include <sys/syscall.h>
#endif

struct shell_job_events {
    int queue, directory, process;
    bool reconcile;
    uint64_t waits, wakes;
};

const char *shell_job_events_backend(void) {
#ifdef __APPLE__
    return "kqueue";
#elif defined(__linux__)
    return "inotify+pidfd";
#else
    return "unsupported";
#endif
}

void shell_job_events_close(shell_job_events_t *e) {
    if (!e) return;
    if (e->process >= 0) close(e->process);
    if (e->directory >= 0) close(e->directory);
    if (e->queue >= 0) close(e->queue);
    free(e);
}

#ifdef __APPLE__
static int watch(int queue, uintptr_t id, int16_t filter, uint32_t notes) {
    struct kevent change;
    EV_SET(&change, id, filter, EV_ADD | EV_CLEAR, notes, 0, NULL);
    int rc;
    do { rc = kevent(queue, &change, 1, NULL, 0, NULL); } while (rc < 0 && errno == EINTR);
    return rc;
}
#endif

shell_job_events_t *shell_job_events_open(const char *path, pid_t pid, bool signals) {
    shell_job_events_t *e = calloc(1, sizeof(*e));
    if (!e) return NULL;
    e->queue = e->directory = e->process = -1;
#ifdef __APPLE__
    e->queue = kqueue();
    if (e->queue < 0 || fcntl(e->queue, F_SETFD, FD_CLOEXEC) < 0) goto fail;
    e->directory = open(path, O_EVTONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (e->directory < 0 || watch(e->queue, e->directory, EVFILT_VNODE,
                                 NOTE_WRITE | NOTE_RENAME | NOTE_DELETE | NOTE_REVOKE) < 0) goto fail;
    if (pid > 0 && watch(e->queue, pid, EVFILT_PROC, NOTE_EXIT) < 0) {
        if (errno != ESRCH) goto fail;
        e->reconcile = true; /* Exit raced registration; owner checks state. */
    }
    if (signals && (watch(e->queue, SIGTERM, EVFILT_SIGNAL, 0) < 0 ||
                    watch(e->queue, SIGINT, EVFILT_SIGNAL, 0) < 0)) goto fail;
#elif defined(__linux__)
    (void)signals; /* Installed signal handlers interrupt poll; no signal masking. */
    e->directory = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (e->directory < 0 || inotify_add_watch(e->directory, path,
            IN_CREATE | IN_MOVED_TO | IN_DELETE | IN_DELETE_SELF | IN_MOVE_SELF) < 0) goto fail;
    if (pid > 0) {
#ifdef SYS_pidfd_open
        e->process = (int)syscall(SYS_pidfd_open, pid, 0);
        if (e->process < 0) {
            if (errno != ESRCH) goto fail;
            e->reconcile = true;
        }
#else
        errno = ENOTSUP; goto fail;
#endif
    }
#else
    (void)path; (void)pid; (void)signals;
    errno = ENOTSUP; goto fail;
#endif
    return e;
fail:;
    int saved = errno; shell_job_events_close(e); errno = saved; return NULL;
}

int shell_job_events_wait(shell_job_events_t *e, int timeout_ms) {
    if (!e) { errno = EINVAL; return -1; }
    if (e->reconcile) { e->reconcile = false; return 1; }
    ++e->waits;
#ifdef __APPLE__
    struct timespec delay = {0, 0};
    if (timeout_ms >= 0) {
        delay.tv_sec = timeout_ms / 1000; delay.tv_nsec = (timeout_ms % 1000) * 1000000L;
    }
    struct kevent event;
    int rc = kevent(e->queue, NULL, 0, &event, 1, timeout_ms < 0 ? NULL : &delay);
    if (rc > 0 && (event.flags & EV_ERROR)) { errno = (int)event.data; return -1; }
    if (rc > 0 && event.filter == EVFILT_VNODE &&
        (event.fflags & (NOTE_DELETE | NOTE_RENAME | NOTE_REVOKE))) {
        errno = ESTALE; return -1;
    }
#elif defined(__linux__)
    struct pollfd p[2] = {{.fd = e->directory, .events = POLLIN},
                          {.fd = e->process, .events = POLLIN}};
    int rc = poll(p, 2, timeout_ms);
    if (rc > 0 && p[0].revents) {
        char events[8192]; ssize_t n = read(e->directory, events, sizeof(events));
        if (n < 0 && errno != EAGAIN && errno != EINTR) return -1;
        for (size_t offset = 0; n > 0 && offset + sizeof(struct inotify_event) <= (size_t)n;) {
            struct inotify_event event;
            __builtin_memcpy(&event, events + offset, sizeof(event));
            if (event.mask & (IN_Q_OVERFLOW | IN_IGNORED | IN_DELETE_SELF | IN_MOVE_SELF)) {
                errno = ESTALE; return -1;
            }
            offset += sizeof(event) + event.len;
        }
    }
    if (rc > 0 && ((p[0].revents | p[1].revents) & (POLLERR | POLLNVAL))) {
        errno = EIO; return -1;
    }
#else
    (void)timeout_ms; errno = ENOTSUP; return -1;
#endif
#if defined(__APPLE__) || defined(__linux__)
    if (rc < 0 && errno == EINTR) return 1; /* Owner rechecks cancellation. */
    if (rc > 0) ++e->wakes;
    return rc;
#endif
}

uint64_t shell_job_events_wait_count(const shell_job_events_t *e) { return e ? e->waits : 0; }
uint64_t shell_job_events_wake_count(const shell_job_events_t *e) { return e ? e->wakes : 0; }
