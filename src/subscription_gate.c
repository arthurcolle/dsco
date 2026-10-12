#ifdef __APPLE__
#define _DARWIN_C_SOURCE 1
#endif

#include "subscription_gate.h"
#include "provider_admission.h"

static void (*wait_hook)(const char *);
void subscription_gate_set_wait_hook(void (*hook)(const char *)) { wait_hook = hook; }

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
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

#define SUBSCRIPTION_GATE_POLL_MS 20L
#define SUBSCRIPTION_GATE_DEFAULT_INTERVAL_MS 0L
#define SUBSCRIPTION_GATE_DEFAULT_MAX_WAIT_MS 900000L
#define SUBSCRIPTION_GATE_DEFAULT_CONCURRENCY 4L
#define SUBSCRIPTION_GATE_MAX_CONCURRENCY 512L

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

static long long gate_monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
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

static bool gate_path_for_hash(uint64_t scope_hash, int slot, char *out, size_t out_len) {
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
    if (slot < 0)
        n = snprintf(out, out_len, "%s/openai-codex-gate-%016llx", dir,
                     (unsigned long long)scope_hash);
    else
        n = snprintf(out, out_len, "%s/openai-codex-gate-%016llx-slot-%02d", dir,
                     (unsigned long long)scope_hash, slot);
    return n > 0 && (size_t)n < out_len;
}

/* -2 is corrupt/unavailable state; -1 is transient metadata contention. */
static long long gate_read_deadline_ms(int fd) {
    char buf[64]; struct stat st;
    if(fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_uid!=geteuid()||st.st_size>63){errno=EIO;return -2;}
    ssize_t n=pread(fd,buf,sizeof(buf)-1,0);
    if(n<0)return -2;
    if(!n)return 0;
    buf[n]='\0';char *end=NULL;errno=0;
    long long deadline=strtoll(buf,&end,10);
    if(errno||end==buf||deadline<0){errno=EPROTO;return -2;}
    while(*end==' '||*end=='\n'||*end=='\r'||*end=='\t')end++;
    if(*end){errno=EPROTO;return -2;}
    return deadline;
}
static bool gate_write_deadline_ms(int fd,long long deadline) {
    char buf[64];int n=snprintf(buf,sizeof(buf),"%lld\n",deadline);
    return n>0 && n<(int)sizeof(buf) && pwrite(fd,buf,(size_t)n,0)==n &&
           ftruncate(fd,n)==0 && fsync(fd)==0;
}
static long long gate_shared_deadline_ms(uint64_t scope_hash) {
    char path[1200], failed[1220];
    if(!gate_path_for_hash(scope_hash,-1,path,sizeof(path))){errno=EIO;return -2;}
    snprintf(failed,sizeof(failed),"%s.failed",path);struct stat st;
    if(lstat(failed,&st)==0 || errno!=ENOENT){errno=EIO;return -2;}
    int fd=open(path,O_RDWR|O_CREAT|O_CLOEXEC|O_NOFOLLOW,0600);
    if(fd<0)return -2;
    long long deadline;
    if(flock(fd,LOCK_SH|LOCK_NB)==0){deadline=gate_read_deadline_ms(fd);flock(fd,LOCK_UN);}
    else deadline=(errno==EWOULDBLOCK||errno==EAGAIN)?-1:-2;
    close(fd);return deadline;
}
static void gate_extend_shared_deadline_ms(uint64_t scope_hash,long long deadline_ms) {
    char path[1200];bool ok=false;
    if(!gate_path_for_hash(scope_hash,-1,path,sizeof(path))) {
        fprintf(stderr,"error: subscription cooldown path unavailable; admission state must be repaired\n");return;
    }
    int fd=open(path,O_RDWR|O_CREAT|O_CLOEXEC|O_NOFOLLOW,0600);
    if(fd>=0) {
        if(flock(fd,LOCK_EX|LOCK_NB)==0) {
            long long current=gate_read_deadline_ms(fd);
            ok=current>=0 && (deadline_ms<=current || gate_write_deadline_ms(fd,deadline_ms));
            flock(fd,LOCK_UN);
        }
        close(fd);
    }
    if(!ok) {
        /* An effect already happened: never replay it because persistence
         * failed. Mark the lane unhealthy for subsequent admissions. */
        char failed[1220];snprintf(failed,sizeof(failed),"%s.failed",path);
        fd=open(failed,O_WRONLY|O_CREAT|O_CLOEXEC|O_NOFOLLOW,0600);
        if(fd>=0){(void)fsync(fd);close(fd);}
        fprintf(stderr,"error: subscription cooldown could not be persisted; lane requires state repair\n");
    }
}

static bool gate_sleep_ms(long duration_ms, const volatile int *interrupted) {
    long long until_ms = gate_monotonic_ms() + duration_ms;
    for (;;) {
        if (wait_hook) wait_hook("subscription_queue");
        if (interrupted && *interrupted)
            return false;
        long long remaining_ms = until_ms - gate_monotonic_ms();
        if (remaining_ms <= 0)
            break;
        long chunk =
            remaining_ms > SUBSCRIPTION_GATE_POLL_MS ? SUBSCRIPTION_GATE_POLL_MS : (long)remaining_ms;
        struct timespec ts = {.tv_sec = chunk / 1000, .tv_nsec = (chunk % 1000) * 1000000L};
        while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
            if (interrupted && *interrupted)
                return false;
        }
    }
    return !(interrupted && *interrupted);
}

static void gate_close(subscription_gate_t *gate) {
    if (!gate)
        return;
    if (gate->legacy_held) { close(gate->legacy_fd); gate->legacy_held=false; gate->legacy_fd=-1; }
    if (gate->held && gate->fd >= 0)
        flock(gate->fd, LOCK_UN);
    if (gate->fd >= 0)
        close(gate->fd);
    gate->fd = -1;
    gate->held = false;
    gate->slot = 0;
}

static void gate_set_waited(long long started_ms, long *waited_ms) {
    if (waited_ms) {
        long long elapsed = gate_monotonic_ms() - started_ms;
        *waited_ms = elapsed > LONG_MAX ? LONG_MAX : elapsed > 0 ? (long)elapsed : 0;
    }
}

static bool gate_wait_failed(subscription_gate_t *gate, long long started_ms,
                             long *waited_ms, int reason) {
    gate_close(gate);
    gate_set_waited(started_ms, waited_ms);
    errno = reason; /* close/unlock must not overwrite the acquisition reason */
    return false;
}

bool subscription_gate_acquire(subscription_gate_t *gate, const char *scope,
                               const volatile int *interrupted, long *waited_ms) {
    if (!gate)
        return true;
    gate->fd = -1;
    gate->legacy_fd = -1;
    gate->legacy_held = false;
    gate->held = false;
    gate->scope_hash = gate_scope_hash(scope);
    gate->slot = 0;
    gate->capacity = 0;
    if (waited_ms)
        *waited_ms = 0;
    long long started_ms = gate_monotonic_ms();
    if (interrupted && *interrupted)
        return gate_wait_failed(gate, started_ms, waited_ms, EINTR);
    if (gate_env_false(getenv("DSCO_CHATGPT_GLOBAL_GATE")))
        return true;
    long max_wait_ms = gate_env_long("DSCO_CHATGPT_GATE_MAX_WAIT_MS",
                                     SUBSCRIPTION_GATE_DEFAULT_MAX_WAIT_MS, 1000L, 3600000L);
    long capacity = gate_env_long("DSCO_CHATGPT_LANE_CONCURRENCY",
                                  SUBSCRIPTION_GATE_DEFAULT_CONCURRENCY, 1L,
                                  SUBSCRIPTION_GATE_MAX_CONCURRENCY);
    gate->capacity = (unsigned)capacity;
    long long expires_ms = started_ms + max_wait_ms;
    provider_admission_t admission = {.state_fd=-1, .ticket_fd=-1};
    char admission_path[1200];
    if (!gate_path_for_hash(gate->scope_hash, -1, admission_path, sizeof(admission_path)))
        return gate_wait_failed(gate, started_ms, waited_ms, EIO);
#define ADMISSION_FAIL(error_) do { int saved_error=(error_); provider_admission_leave(&admission); return gate_wait_failed(gate, started_ms, waited_ms, saved_error); } while (0)


    for (;;) {
        bool fast_retry = false;
        if (interrupted && *interrupted)
            ADMISSION_FAIL(EINTR);
        long long budget_ms = expires_ms - gate_monotonic_ms();
        if (budget_ms <= 0)
            ADMISSION_FAIL(ETIMEDOUT);
        /* Compatibility bridge: old binaries lock the account file exclusively.
         * New streams hold shared leases, so new/new streams overlap while
         * old/new clients still coordinate. Poll rather than blocking so the
         * original cancellation/deadline budget remains authoritative. */
        long long legacy_deadline = 0;
        const char *separator = scope ? strrchr(scope, '|') : NULL;
        if (separator && !gate->legacy_held) {
            char account[512], path[1200]; size_t n=(size_t)(separator-scope);
            if (n >= sizeof(account)) ADMISSION_FAIL(ENAMETOOLONG);
            memcpy(account,scope,n);account[n]='\0';
            if (!gate_path_for_hash(gate_scope_hash(account),-1,path,sizeof(path))) ADMISSION_FAIL(EIO);
            int fd=open(path,O_RDWR|O_CREAT|O_CLOEXEC|O_NOFOLLOW,0600);
            if(fd<0) ADMISSION_FAIL(errno);
            if(flock(fd,LOCK_SH|LOCK_NB)) {
                int error=errno;close(fd);
                if(error!=EAGAIN && error!=EWOULDBLOCK && error!=EINTR) ADMISSION_FAIL(error);
                goto pause_admission;
            }
            gate->legacy_fd=fd;gate->legacy_held=true;
        }
        if(gate->legacy_held) { legacy_deadline=gate_read_deadline_ms(gate->legacy_fd); if(legacy_deadline<0) ADMISSION_FAIL(EIO); }
        long long now_ms = gate_now_ms();
        long long deadline_ms = gate_shared_deadline_ms(gate->scope_hash);
        if(deadline_ms==-2) ADMISSION_FAIL(errno ? errno : EIO);
        if(deadline_ms==-1) goto pause_admission;
        if(legacy_deadline>deadline_ms) deadline_ms=legacy_deadline;
        if (deadline_ms > now_ms) {
            long long remaining = deadline_ms - now_ms;
            if (remaining > max_wait_ms)
                remaining = max_wait_ms; /* corrupt state cannot block forever */
            if (remaining > budget_ms)
                remaining = budget_ms;
            if (!gate_sleep_ms((long)remaining, interrupted))
                ADMISSION_FAIL(EINTR);
            continue;
        }

        if (admission.ticket_fd < 0 && !provider_admission_join(&admission, admission_path)) {
            if (errno != EWOULDBLOCK && errno != EAGAIN && errno != EINTR) ADMISSION_FAIL(errno);
            fast_retry = true;
            goto pause_admission;
        }
        int chosen = provider_admission_choose(&admission, gate->capacity);
        if (chosen < 0) ADMISSION_FAIL(errno);
        if (chosen == 2) { fast_retry = true; goto pause_admission; }
        if (!chosen) goto pause_admission;

        /* Rotate the first slot by process/time so concurrent callers do not
         * all hammer slot zero. A successful slot lock is the only object held
         * across provider transport. */
        unsigned first = (unsigned)(((uint64_t)getpid() ^ (uint64_t)started_ms) %
                                    (uint64_t)gate->capacity);
        for (unsigned offset = 0; offset < gate->capacity; offset++) {
            unsigned slot = (first + offset) % gate->capacity;
            char path[1200];
            if (!gate_path_for_hash(gate->scope_hash, (int)slot, path, sizeof(path)))
                ADMISSION_FAIL(EIO);
            int fd = open(path, O_RDWR | O_CREAT, 0600);
            if (fd < 0)
                ADMISSION_FAIL(errno);
            fchmod(fd, 0600);
            fcntl(fd, F_SETFD, FD_CLOEXEC);
            if (flock(fd, LOCK_EX | LOCK_NB) == 0) {
                gate->fd = fd;
                gate->held = true;
                gate->slot = slot;
                /* A sibling may have published a cooldown while we scanned. */
                long long checked_deadline=gate_shared_deadline_ms(gate->scope_hash);
                if (checked_deadline == -2) ADMISSION_FAIL(errno ? errno : EIO);
                if (checked_deadline >= 0 && checked_deadline <= gate_now_ms())
                    goto acquired;
                gate_close(gate);
                break;
            }
            int lock_error = errno;
            close(fd);
            if (lock_error != EWOULDBLOCK && lock_error != EAGAIN && lock_error != EINTR)
                ADMISSION_FAIL(lock_error);
        }
        provider_admission_decide(&admission, false);
pause_admission:;
        long poll_ms = fast_retry ? 1L : SUBSCRIPTION_GATE_POLL_MS;
        long pause_ms = budget_ms < poll_ms ? (long)budget_ms : poll_ms;
        if (!gate_sleep_ms(pause_ms, interrupted))
            ADMISSION_FAIL(EINTR);
    }

acquired:
    if (interrupted && *interrupted)
        ADMISSION_FAIL(EINTR);
    if (gate_monotonic_ms() >= expires_ms)
        ADMISSION_FAIL(ETIMEDOUT);
    provider_admission_decide(&admission, true);
    gate_set_waited(started_ms, waited_ms);
#undef ADMISSION_FAIL
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
    if (delay_ms > 0)
        gate_extend_shared_deadline_ms(gate->scope_hash, gate_now_ms() + delay_ms);
    gate_close(gate);
}
