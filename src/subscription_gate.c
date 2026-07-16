#ifdef __APPLE__
#define _DARWIN_C_SOURCE 1
#endif

#include "subscription_gate.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define SUBSCRIPTION_GATE_POLL_MS 100L
#define SUBSCRIPTION_GATE_DEFAULT_INTERVAL_MS 1000L
#define SUBSCRIPTION_GATE_DEFAULT_MAX_WAIT_MS 900000L

static bool gate_env_false(const char *value) {
    return value && value[0] &&
           (strcmp(value, "0") == 0 || strcasecmp(value, "false") == 0 ||
            strcasecmp(value, "off") == 0 || strcasecmp(value, "no") == 0);
}

static long gate_env_long(const char *name, long fallback, long min_value, long max_value) {
    const char *raw = getenv(name);
    if (!raw || !raw[0])
        return fallback;
    char *end = NULL;
    errno = 0;
    long value = strtol(raw, &end, 10);
    if (errno != 0 || !end || end == raw || *end != '\0')
        return fallback;
    if (value < min_value)
        return min_value;
    if (value > max_value)
        return max_value;
    return value;
}

static long long gate_now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000LL + tv.tv_usec / 1000LL;
}

static uint64_t gate_scope_hash(const char *scope) {
    const unsigned char *p = (const unsigned char *)(scope && scope[0] ? scope : "default");
    uint64_t hash = UINT64_C(1469598103934665603);
    while (*p) {
        hash ^= *p++;
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static bool gate_path(const char *scope, char *out, size_t out_len) {
    if (!out || out_len == 0)
        return false;
    const char *home = getenv("HOME");
    const char *root = home && home[0] ? home : "/tmp";
    char dir[1024];
    int n = snprintf(dir, sizeof(dir), "%s/.dsco", root);
    if (n <= 0 || (size_t)n >= sizeof(dir))
        return false;
    if (mkdir(dir, 0700) != 0 && errno != EEXIST)
        return false;
    n = snprintf(out, out_len, "%s/openai-codex-gate-%016llx", dir,
                 (unsigned long long)gate_scope_hash(scope));
    return n > 0 && (size_t)n < out_len;
}

static long long gate_read_deadline_ms(int fd) {
    char buf[64];
    if (lseek(fd, 0, SEEK_SET) < 0)
        return 0;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    if (n <= 0)
        return 0;
    buf[n] = '\0';
    char *end = NULL;
    errno = 0;
    long long deadline = strtoll(buf, &end, 10);
    return errno == 0 && end && end != buf && deadline > 0 ? deadline : 0;
}

static void gate_write_deadline_ms(int fd, long long deadline_ms) {
    char buf[64];
    int n = snprintf(buf, sizeof(buf), "%lld\n", deadline_ms);
    if (n <= 0 || (size_t)n >= sizeof(buf))
        return;
    if (ftruncate(fd, 0) != 0 || lseek(fd, 0, SEEK_SET) < 0)
        return;
    ssize_t ignored = write(fd, buf, (size_t)n);
    (void)ignored;
}

static bool gate_sleep_ms(long duration_ms, const volatile int *interrupted) {
    while (duration_ms > 0) {
        if (interrupted && *interrupted)
            return false;
        long chunk =
            duration_ms > SUBSCRIPTION_GATE_POLL_MS ? SUBSCRIPTION_GATE_POLL_MS : duration_ms;
        struct timespec ts = {.tv_sec = chunk / 1000, .tv_nsec = (chunk % 1000) * 1000000L};
        while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
            if (interrupted && *interrupted)
                return false;
        }
        duration_ms -= chunk;
    }
    return !(interrupted && *interrupted);
}

static void gate_close(subscription_gate_t *gate) {
    if (!gate)
        return;
    if (gate->held && gate->fd >= 0)
        flock(gate->fd, LOCK_UN);
    if (gate->fd >= 0)
        close(gate->fd);
    gate->fd = -1;
    gate->held = false;
}

bool subscription_gate_acquire(subscription_gate_t *gate, const char *scope,
                               const volatile int *interrupted, long *waited_ms) {
    if (!gate)
        return true;
    gate->fd = -1;
    gate->held = false;
    if (waited_ms)
        *waited_ms = 0;
    if (gate_env_false(getenv("DSCO_CHATGPT_GLOBAL_GATE")))
        return true;

    char path[1200];
    if (!gate_path(scope, path, sizeof(path)))
        return true; /* preserve provider availability if local state is unavailable */
    int fd = open(path, O_RDWR | O_CREAT, 0600);
    if (fd < 0)
        return true;
    fchmod(fd, 0600);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    gate->fd = fd;

    long long started_ms = gate_now_ms();
    for (;;) {
        if (flock(fd, LOCK_EX | LOCK_NB) == 0) {
            gate->held = true;
            break;
        }
        if (errno != EWOULDBLOCK && errno != EAGAIN && errno != EINTR) {
            gate_close(gate);
            return true; /* fail open on unsupported/advisory-lock errors */
        }
        if (!gate_sleep_ms(SUBSCRIPTION_GATE_POLL_MS, interrupted)) {
            gate_close(gate);
            return false;
        }
    }

    long max_wait_ms = gate_env_long("DSCO_CHATGPT_GATE_MAX_WAIT_MS",
                                     SUBSCRIPTION_GATE_DEFAULT_MAX_WAIT_MS, 1000L, 3600000L);
    for (;;) {
        long long now_ms = gate_now_ms();
        long long deadline_ms = gate_read_deadline_ms(fd);
        if (deadline_ms <= now_ms)
            break;
        long long remaining = deadline_ms - now_ms;
        if (remaining > max_wait_ms) {
            remaining = max_wait_ms;
            /* A corrupt or stale state file must not block the lane forever. */
            gate_write_deadline_ms(fd, now_ms + remaining);
        }
        if (!gate_sleep_ms((long)remaining, interrupted)) {
            gate_close(gate);
            return false;
        }
    }

    if (waited_ms) {
        long long elapsed = gate_now_ms() - started_ms;
        *waited_ms = elapsed > 0 && elapsed < 2147483647LL ? (long)elapsed : 0;
    }
    return true;
}

void subscription_gate_release(subscription_gate_t *gate, long cooldown_ms) {
    if (!gate || !gate->held || gate->fd < 0) {
        gate_close(gate);
        return;
    }
    long min_interval_ms = gate_env_long("DSCO_CHATGPT_MIN_INTERVAL_MS",
                                         SUBSCRIPTION_GATE_DEFAULT_INTERVAL_MS, 0L, 60000L);
    long max_wait_ms = gate_env_long("DSCO_CHATGPT_GATE_MAX_WAIT_MS",
                                     SUBSCRIPTION_GATE_DEFAULT_MAX_WAIT_MS, 1000L, 3600000L);
    long delay_ms = cooldown_ms > min_interval_ms ? cooldown_ms : min_interval_ms;
    if (delay_ms > max_wait_ms)
        delay_ms = max_wait_ms;
    gate_write_deadline_ms(gate->fd, gate_now_ms() + delay_ms);
    gate_close(gate);
}
