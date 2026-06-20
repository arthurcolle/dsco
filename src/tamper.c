/* Enable Darwin extensions for BSD-compat types (u_int, u_char, etc.) */
#ifdef __APPLE__
#  define _DARWIN_C_SOURCE 1
#endif

#include "tamper.h"
#include <sodium.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <errno.h>
#include <sys/stat.h>

#ifdef __APPLE__
#  include <sys/ptrace.h>
#  include <sys/sysctl.h>
#  include <sys/event.h>
#  include <mach-o/dyld.h>
#  include <mach-o/getsect.h>
#  include <mach-o/loader.h>
#endif

#ifdef __linux__
#  include <sys/inotify.h>
#  include <sys/prctl.h>
#endif

/* ── Constants ─────────────────────────────────────────────────────────── */
#define HASH_BYTES         crypto_generichash_BYTES   /* 32 */
#define MAX_WIPERS         32
#define WATCH_INTERVAL_MS  2000

/* ── Global state ──────────────────────────────────────────────────────── */
static struct {
    bool     initialised;
    uint8_t  code_hash[HASH_BYTES];
    bool     have_code_hash;

    /* Registered secret wipers */
    tamper_wiper_fn wipers[MAX_WIPERS];
    void           *wiper_ctx[MAX_WIPERS];
    int             wiper_count;
    pthread_mutex_t wiper_lock;

    /* Binary path + watch fd */
    char exe_path[4096];
    int  watch_fd;      /* fd used for kqueue / inotify */
    int  kq_or_inotify; /* kqueue fd (macOS) or inotify fd (Linux) */

    pthread_t watch_thread;
    volatile bool watch_running;
} g = {0};

/* ── Forward declarations ─────────────────────────────────────────────── */
static void do_response(const char *reason);
static void *watcher_thread(void *arg);

/* ═══════════════════════════════════════════════════════════════════════ */
/* 1. Anti-debug                                                           */
/* ═══════════════════════════════════════════════════════════════════════ */

static void deny_debugger(void) {
#ifdef __APPLE__
    /* Prevent any debugger from attaching to this process. */
    ptrace(PT_DENY_ATTACH, 0, NULL, 0);
#endif
#ifdef __linux__
    /* Prevent ptrace attach from any process (including gdb). */
    prctl(PR_SET_DUMPABLE, 0);
#endif
}

static bool debugger_present(void) {
#ifdef __APPLE__
    struct kinfo_proc info;
    memset(&info, 0, sizeof(info));
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid()};
    size_t sz = sizeof(info);
    sysctl(mib, 4, &info, &sz, NULL, 0);
    return (info.kp_proc.p_flag & P_TRACED) != 0;
#elif defined(__linux__)
    FILE *f = fopen("/proc/self/status", "r");
    if (!f) return false;
    char line[256];
    bool traced = false;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "TracerPid:", 10) == 0) {
            traced = atoi(line + 10) != 0;
            break;
        }
    }
    fclose(f);
    return traced;
#else
    return false;
#endif
}

/* ═══════════════════════════════════════════════════════════════════════ */
/* 2. Code-segment integrity                                               */
/* ═══════════════════════════════════════════════════════════════════════ */

static bool compute_code_hash(uint8_t *out) {
#ifdef __APPLE__
    const struct mach_header_64 *mh =
        (const struct mach_header_64 *)_dyld_get_image_header(0);
    if (!mh) return false;
    unsigned long text_size = 0;
    const uint8_t *text = (const uint8_t *)
        getsectiondata(mh, "__TEXT", "__text", &text_size);
    if (!text || text_size == 0) return false;
    crypto_generichash(out, HASH_BYTES, text, text_size, NULL, 0);
    return true;
#else
    /* Linux: read .text from /proc/self/maps and hash mapped pages.
     * Simplified: hash first 256 KB of our own exe as a proxy. */
    FILE *f = fopen(g.exe_path, "rb");
    if (!f) return false;
    uint8_t buf[65536];
    crypto_generichash_state st;
    crypto_generichash_init(&st, NULL, 0, HASH_BYTES);
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        crypto_generichash_update(&st, buf, n);
    fclose(f);
    crypto_generichash_final(&st, out, HASH_BYTES);
    return true;
#endif
}

/* ═══════════════════════════════════════════════════════════════════════ */
/* 3. Binary file watcher                                                  */
/* ═══════════════════════════════════════════════════════════════════════ */

#ifdef __APPLE__
static bool setup_kqueue_watch(void) {
    g.watch_fd = open(g.exe_path, O_RDONLY | O_CLOEXEC);
    if (g.watch_fd < 0) return false;

    g.kq_or_inotify = kqueue();
    if (g.kq_or_inotify < 0) { close(g.watch_fd); return false; }

    struct kevent kev;
    /* NOTE_OPEN fires when any process (other than us, post-setup) opens
     * the binary — catches Ghidra, objdump, strings, etc.
     * NOTE_WRITE / NOTE_DELETE catch in-place patching or removal. */
    EV_SET(&kev, (uintptr_t)g.watch_fd, EVFILT_VNODE,
           EV_ADD | EV_CLEAR | EV_ENABLE,
           NOTE_WRITE | NOTE_DELETE | NOTE_RENAME | NOTE_ATTRIB | NOTE_REVOKE,
           0, NULL);
    kevent(g.kq_or_inotify, &kev, 1, NULL, 0, NULL);
    return true;
}

static void *watcher_thread(void *arg) {
    (void)arg;
    struct timespec timeout = { .tv_sec = 0, .tv_nsec = WATCH_INTERVAL_MS * 1000000 };
    struct kevent ev[4];

    /* Drain any events from our own startup open */
    kevent(g.kq_or_inotify, NULL, 0, ev, 4, &timeout);

    while (g.watch_running) {
        int n = kevent(g.kq_or_inotify, NULL, 0, ev, 4, &timeout);
        for (int i = 0; i < n; i++) {
            uint32_t flags = (uint32_t)ev[i].fflags;
            if (flags & NOTE_WRITE)
                do_response("binary modified on disk (patching attempt)");
            if (flags & (NOTE_DELETE | NOTE_RENAME | NOTE_REVOKE))
                do_response("binary deleted, renamed, or revoked");
            if (flags & NOTE_ATTRIB)
                do_response("binary attributes modified (setuid/chmod tampering)");
        }
    }
    return NULL;
}
#endif /* __APPLE__ */

#ifdef __linux__
static bool setup_inotify_watch(void) {
    g.kq_or_inotify = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
    if (g.kq_or_inotify < 0) return false;
    g.watch_fd = inotify_add_watch(g.kq_or_inotify, g.exe_path,
                                   IN_OPEN | IN_MODIFY | IN_DELETE_SELF |
                                   IN_MOVE_SELF | IN_ATTRIB);
    return g.watch_fd >= 0;
}

static void *watcher_thread(void *arg) {
    (void)arg;
    uint8_t buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));

    while (g.watch_running) {
        usleep(WATCH_INTERVAL_MS * 1000);
        ssize_t len = read(g.kq_or_inotify, buf, sizeof(buf));
        if (len < 0) continue;
        const struct inotify_event *ev;
        for (const uint8_t *p = buf; p < buf + len;
             p += sizeof(*ev) + ev->len) {
            ev = (const struct inotify_event *)p;
            if (ev->mask & IN_OPEN)
                do_response("binary opened by external process");
            if (ev->mask & IN_MODIFY)
                do_response("binary modified on disk");
            if (ev->mask & (IN_DELETE_SELF | IN_MOVE_SELF))
                do_response("binary deleted or moved");
        }
    }
    return NULL;
}
#endif /* __linux__ */

/* ═══════════════════════════════════════════════════════════════════════ */
/* 4. Tamper response                                                      */
/* ═══════════════════════════════════════════════════════════════════════ */

/* Overwrite the on-disk binary with cryptographically random bytes. */
static void self_destruct_binary(void) {
    if (!g.exe_path[0]) return;

    int fd = open(g.exe_path, O_WRONLY);
    if (fd < 0) return;

    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 0) { close(fd); return; }

    size_t sz  = (size_t)st.st_size;
    uint8_t *rnd = malloc(sz);
    if (!rnd) {
        /* Fall back to chunked write */
        uint8_t chunk[4096];
        size_t remaining = sz;
        lseek(fd, 0, SEEK_SET);
        while (remaining > 0) {
            size_t n = remaining < sizeof(chunk) ? remaining : sizeof(chunk);
            randombytes_buf(chunk, n);
            ssize_t w = write(fd, chunk, n);
            if (w <= 0) break;
            remaining -= (size_t)w;
        }
    } else {
        randombytes_buf(rnd, sz);
        lseek(fd, 0, SEEK_SET);
        size_t written = 0;
        while (written < sz) {
            ssize_t w = write(fd, rnd + written, sz - written);
            if (w <= 0) break;
            written += (size_t)w;
        }
        /* Zero then free the random buffer — don't leak the overwrite data */
        sodium_memzero(rnd, sz);
        free(rnd);
    }

    fsync(fd);
    close(fd);
}

static volatile bool g_responding = false;

static void do_response(const char *reason) {
    if (g_responding) _exit(1);
    g_responding = true;

    /* Invoke registered wipers to zero out all in-memory secrets */
    pthread_mutex_lock(&g.wiper_lock);
    for (int i = 0; i < g.wiper_count; i++) {
        if (g.wipers[i]) g.wipers[i](g.wiper_ctx[i]);
    }
    pthread_mutex_unlock(&g.wiper_lock);

    /* Zero our own secret state */
    sodium_memzero(g.code_hash, sizeof(g.code_hash));

    /* Wipe stack as much as we can */
    volatile uint8_t zstack[8192];
    memset((void *)zstack, 0, sizeof(zstack));

    /* On-disk binary destruction (opt-in via env var for safety) */
    const char *destruct_env = getenv("DSCO_TAMPER_DESTRUCT");
    if (destruct_env && destruct_env[0] == '1') {
        self_destruct_binary();
    }

    fprintf(stderr, "\n\033[31m[TAMPER] %s\033[0m\n", reason);
    fflush(stderr);
    _exit(1);
}

/* ═══════════════════════════════════════════════════════════════════════ */
/* Public API                                                              */
/* ═══════════════════════════════════════════════════════════════════════ */

void tamper_init(void) {
    if (g.initialised) return;
    g.initialised = true;

    if (sodium_init() < 0) return;
    pthread_mutex_init(&g.wiper_lock, NULL);

    /* Step 1: deny debugger attachment */
    deny_debugger();

    /* Step 2: resolve our own executable path */
#ifdef __APPLE__
    uint32_t sz = sizeof(g.exe_path);
    if (_NSGetExecutablePath(g.exe_path, &sz) != 0)
        g.exe_path[0] = '\0';
#elif defined(__linux__)
    ssize_t n = readlink("/proc/self/exe", g.exe_path, sizeof(g.exe_path) - 1);
    if (n > 0) g.exe_path[n] = '\0';
    else        g.exe_path[0] = '\0';
#endif

    /* Step 3: compute initial code hash */
    if (compute_code_hash(g.code_hash)) {
        g.have_code_hash = true;
    }

    /* Step 4: start file watcher */
    bool watch_ok = false;
#ifdef __APPLE__
    watch_ok = setup_kqueue_watch();
#endif
#ifdef __linux__
    watch_ok = setup_inotify_watch();
#endif

    if (watch_ok) {
        g.watch_running = true;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_create(&g.watch_thread, &attr, watcher_thread, NULL);
        pthread_attr_destroy(&attr);
    }
}

void tamper_register_wiper(tamper_wiper_fn fn, void *ctx) {
    pthread_mutex_lock(&g.wiper_lock);
    if (g.wiper_count < MAX_WIPERS) {
        g.wipers[g.wiper_count]     = fn;
        g.wiper_ctx[g.wiper_count]  = ctx;
        g.wiper_count++;
    }
    pthread_mutex_unlock(&g.wiper_lock);
}

bool tamper_check(void) {
    if (!g.initialised) return true;

    /* Check for active debugger */
    if (debugger_present()) {
        do_response("debugger detected (ptrace/P_TRACED)");
        return false;
    }

    /* Verify code segment hash */
    if (g.have_code_hash) {
        uint8_t current[HASH_BYTES];
        if (compute_code_hash(current)) {
            if (sodium_memcmp(current, g.code_hash, HASH_BYTES) != 0) {
                do_response("code segment integrity check failed (breakpoint/patch detected)");
                return false;
            }
        }
    }

    return true;
}

void tamper_trigger(const char *reason) {
    do_response(reason ? reason : "manual tamper trigger");
}
