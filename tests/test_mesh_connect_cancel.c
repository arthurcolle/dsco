/* Offline regression for teardown waiting on an unreachable mesh seed.
 * Only connect() is substituted for the stalled-connect case. The wait,
 * cancellation, descriptor flags, and successful loopback dial are real. */
#define _DARWIN_C_SOURCE 1
#include <sys/socket.h>
#include <errno.h>
#include <stdbool.h>

static bool force_pending;
static int test_connect(int fd, const struct sockaddr *addr, socklen_t len) {
    if (force_pending) {
        errno = EINPROGRESS;
        return -1;
    }
    return connect(fd, addr, len);
}
#define connect test_connect
#include "../src/mesh.c"
#undef connect

#include <assert.h>
#include <stdatomic.h>

static atomic_bool cancel_dial;
static bool cancelled(void *ctx) {
    (void)ctx;
    return atomic_load(&cancel_dial);
}
static void *cancel_soon(void *ctx) {
    (void)ctx;
    struct timespec delay = {.tv_nsec = 100000000};
    nanosleep(&delay, NULL);
    atomic_store(&cancel_dial, true);
    return NULL;
}
static double seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(void) {
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    assert(listener >= 0);
    struct sockaddr_in addr = {.sin_family = AF_INET,
                               .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    assert(bind(listener, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    assert(listen(listener, 1) == 0);
    socklen_t addrlen = sizeof(addr);
    assert(getsockname(listener, (struct sockaddr *)&addr, &addrlen) == 0);
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    int flags = fcntl(fd, F_GETFL);
    assert(mesh_connect_socket(fd, (struct sockaddr *)&addr, addrlen, cancelled, NULL));
    assert(fcntl(fd, F_GETFL) == flags);
    close(fd);
    close(listener);
    fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    assert(!mesh_connect_socket(fd, (struct sockaddr *)&addr, addrlen, cancelled, NULL));
    close(fd);

    /* A full socket send buffer gives a deterministic pending POLLOUT wait,
     * without relying on any external blackhole address or network route. */
    int pair[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    assert(fcntl(pair[0], F_SETFL, O_NONBLOCK) == 0);
    char bytes[4096] = {0};
    while (write(pair[0], bytes, sizeof(bytes)) > 0) {}
    assert(errno == EAGAIN || errno == EWOULDBLOCK);
    force_pending = true;
    pthread_t thread;
    assert(pthread_create(&thread, NULL, cancel_soon, NULL) == 0);
    double start = seconds();
    assert(!mesh_connect_socket(pair[0], (struct sockaddr *)&addr, addrlen, cancelled, NULL));
    double cancel_seconds = seconds() - start;
    assert(cancel_seconds < 0.5);
    assert(pthread_join(thread, NULL) == 0);

    atomic_store(&cancel_dial, false);
    start = seconds();
    assert(!mesh_connect_socket(pair[0], (struct sockaddr *)&addr, addrlen, cancelled, NULL));
    double timeout_seconds = seconds() - start;
    assert(timeout_seconds >= 2.9 && timeout_seconds < 3.5);
    close(pair[0]);
    close(pair[1]);
    printf("mesh connect: loopback succeeds; cancellation %.3fs; timeout %.3fs\n",
           cancel_seconds, timeout_seconds);
    return 0;
}
