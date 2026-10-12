#define _DARWIN_C_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#include "event_wakeup.h"
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#if defined(__APPLE__) && !defined(EV_FORCE_POLL)
#include <sys/event.h>
#endif

int ev_wakeup_init(ev_wakeup_t *w, int queue_fd) {
    *w = (ev_wakeup_t){queue_fd, -1, -1};
#if defined(__APPLE__) && !defined(EV_FORCE_POLL)
    struct kevent change;
    EV_SET(&change, 1, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, NULL);
    return kevent(queue_fd, &change, 1, NULL, 0, NULL);
#else
    int p[2];
    if (pipe(p) < 0) return -1;
    for (int i = 0; i < 2; ++i) {
        int flags = fcntl(p[i], F_GETFL);
        if (flags < 0 || fcntl(p[i], F_SETFL, flags | O_NONBLOCK) < 0 ||
            fcntl(p[i], F_SETFD, FD_CLOEXEC) < 0) {
            int saved = errno; close(p[0]); close(p[1]); errno = saved;
            return -1;
        }
    }
    w->read_fd = p[0]; w->write_fd = p[1];
    return 0;
#endif
}

int ev_wakeup_signal(const ev_wakeup_t *w) {
#if defined(__APPLE__) && !defined(EV_FORCE_POLL)
    struct kevent change;
    EV_SET(&change, 1, EVFILT_USER, 0, NOTE_TRIGGER, 0, NULL);
    int rc;
    do { rc = kevent(w->queue_fd, &change, 1, NULL, 0, NULL); }
    while (rc < 0 && errno == EINTR);
    return rc;
#else
    char byte = 1; ssize_t rc;
    do { rc = write(w->write_fd, &byte, 1); } while (rc < 0 && errno == EINTR);
    /* A full pipe already holds a wake: notifications may coalesce. */
    return rc == 1 || (rc < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) ? 0 : -1;
#endif
}

void ev_wakeup_drain(const ev_wakeup_t *w) {
    if (w->read_fd < 0) return;
    char bytes[256];
    /* A hot producer must not keep the owner inside the drain forever. */
    for (int i = 0; i < 256; ++i) {
        ssize_t n = read(w->read_fd, bytes, sizeof(bytes));
        if (n > 0 || (n < 0 && errno == EINTR)) continue;
        break;
    }
}

void ev_wakeup_close(ev_wakeup_t *w) {
    if (w->read_fd >= 0) close(w->read_fd);
    if (w->write_fd >= 0) close(w->write_fd);
    w->read_fd = w->write_fd = -1;
}
