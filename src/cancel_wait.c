#define _POSIX_C_SOURCE 200809L
#include "cancel_wait.h"
#include <pthread.h>
#include <poll.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
struct cancel_wait {
    const volatile int *local, *global;
    int read_fd, write_fd;
    struct cancel_wait *next;
};
static pthread_mutex_t registry_lock = PTHREAD_MUTEX_INITIALIZER;
static cancel_wait_t *registry;
static int64_t now_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
bool cancel_wait_requested(const cancel_wait_t *w) {
    return w && ((w->local && __atomic_load_n(w->local, __ATOMIC_ACQUIRE)) ||
                 (w->global && __atomic_load_n(w->global, __ATOMIC_ACQUIRE)));
}
cancel_wait_t *cancel_wait_open(const volatile int *local, const volatile int *global) {
    cancel_wait_t *w = calloc(1, sizeof(*w));
    if (!w) return NULL;
    int p[2];
    if (pipe(p) < 0) { free(w); return NULL; }
    for (int i = 0; i < 2; ++i) {
        int flags = fcntl(p[i], F_GETFL);
        if (flags < 0 || fcntl(p[i], F_SETFL, flags | O_NONBLOCK) < 0 ||
            fcntl(p[i], F_SETFD, FD_CLOEXEC) < 0) {
            int saved = errno; close(p[0]); close(p[1]); free(w); errno = saved; return NULL;
        }
    }
    w->local = local; w->global = global; w->read_fd = p[0]; w->write_fd = p[1];
    pthread_mutex_lock(&registry_lock);
    w->next = registry; registry = w;
    pthread_mutex_unlock(&registry_lock);
    return w;
}
void cancel_wait_close(cancel_wait_t *w) {
    if (!w) return;
    pthread_mutex_lock(&registry_lock);
    cancel_wait_t **link = &registry;
    while (*link && *link != w) link = &(*link)->next;
    if (*link) *link = w->next;
    /* Signal holds this lock: no publisher can touch a closed/reused fd. */
    close(w->read_fd); close(w->write_fd);
    pthread_mutex_unlock(&registry_lock);
    free(w);
}
int cancel_wait_signal(volatile int *flag) {
    if (!flag) { errno = EINVAL; return -1; }
    int error = 0;
    pthread_mutex_lock(&registry_lock);
    __atomic_store_n(flag, 1, __ATOMIC_RELEASE);
    for (cancel_wait_t *w = registry; w; w = w->next) {
        if (w->local != flag && w->global != flag) continue;
        char byte = 1; ssize_t n;
        do { n = write(w->write_fd, &byte, 1); } while (n < 0 && errno == EINTR);
        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) error = errno;
    }
    pthread_mutex_unlock(&registry_lock);
    if (error) { errno = error; return -1; }
    return 0;
}
int cancel_wait_poll(cancel_wait_t *w, int fd, short events, int timeout_ms) {
    if (!w) { errno = EINVAL; return -1; }
    int64_t deadline = timeout_ms < 0 ? 0 : now_ms() + timeout_ms;
    for (;;) {
        if (cancel_wait_requested(w)) { errno = ECANCELED; return -1; }
        int wait = -1;
        if (timeout_ms >= 0) {
            int64_t remaining = deadline - now_ms();
            wait = remaining <= 0 ? 0 : remaining > INT_MAX ? INT_MAX : (int)remaining;
        }
        struct pollfd fds[2] = {{fd, events, 0}, {w->read_fd, POLLIN, 0}};
        int rc = poll(fds, 2, wait);
        if (rc < 0) { if (errno == EINTR) continue; return -1; }
        if (cancel_wait_requested(w)) { errno = ECANCELED; return -1; }
        if (!rc) return 0;
        if (fds[0].revents) return 1;
        char bytes[256];
        while (read(w->read_fd, bytes, sizeof(bytes)) > 0) {}
    }
}
