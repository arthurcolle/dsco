#ifdef __APPLE__
#define _DARWIN_C_SOURCE 1
#endif

#include "heartbeat.h"
#include "watchdog.h"
#include "sealed_store.h"
#include "audit_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>
#include <stdatomic.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/resource.h>

#ifdef __APPLE__
#  include <libproc.h>
#  include <sys/proc_info.h>
#  include <sys/sysctl.h>
#endif

#ifdef HAVE_LIBSODIUM
#  include <sodium.h>
#endif

#ifdef HAVE_MBEDTLS
#  include "net_server.h"   /* netsrv_client_post */
#endif

/* ── state ──────────────────────────────────────────────────────────────── */

static pthread_t  s_thread;
static atomic_int s_running  = 0;
static atomic_int s_poke     = 0;
static uint64_t   s_seq      = 0;
static time_t     s_start_ts = 0;
static pthread_mutex_t s_emit_lock = PTHREAD_MUTEX_INITIALIZER;
static char       s_cmdline[512] = "";
static char       s_cwd[PATH_MAX] = "";
static char       s_exe[PATH_MAX] = "";
static char       s_phase[96] = "startup";
static long       s_last_rss_mb = -1;
static long       s_peak_rss_mb = -1;

typedef struct runtime_sample {
    long rss_mb;
    long maxrss_mb;
    long user_ms;
    long sys_ms;
    long minor_faults;
    long major_faults;
    long vol_ctx_switches;
    long invol_ctx_switches;
    int  fd_count;
    int  thread_count;
    int  mem_pressure;
} runtime_sample_t;

/* ── helpers ────────────────────────────────────────────────────────────── */

static void dsco_dir(char *buf, size_t len) {
    const char *home = getenv("HOME");
    if (home && home[0]) snprintf(buf, len, "%s/.dsco", home);
    else snprintf(buf, len, "/tmp/.dsco");
    if (mkdir(buf, 0700) != 0 && errno != EEXIST) {
        /* Best effort only. Later open() calls will surface any real problem. */
    }
}

static void get_node_id(char *buf, size_t len) {
    if (sealed_store_get("DSCO_NODE_ID", buf, len) > 0 && buf[0]) return;
    if (gethostname(buf, len) != 0) snprintf(buf, len, "unknown");
}

static int get_interval(void) {
    char buf[16];
    if (sealed_store_get("DSCO_BEACON_SECS", buf, sizeof(buf)) > 0 && buf[0])
        return atoi(buf);
    const char *e = getenv("DSCO_BEACON_SECS");
    return (e && e[0]) ? atoi(e) : 60;
}

static void sign_beacon(const char *node, int64_t ts, uint64_t seq,
                         char *sig_hex, size_t sig_hex_len) {
#ifdef HAVE_LIBSODIUM
    uint8_t key[32] = {0};
    char keybuf[512] = {0};
    sealed_store_get("DSCO_NET_AUTH_KEY", keybuf, sizeof(keybuf));
    if (!keybuf[0]) sealed_store_get("DSCO_MESH_SECRET", keybuf, sizeof(keybuf));
    if (keybuf[0])
        crypto_generichash(key, sizeof(key),
                           (const uint8_t *)keybuf, strlen(keybuf), NULL, 0);

    uint8_t hmac[32];
    crypto_auth_hmacsha256_state st;
    crypto_auth_hmacsha256_init(&st, key, sizeof(key));
    crypto_auth_hmacsha256_update(&st, (const uint8_t *)&ts,  sizeof(ts));
    crypto_auth_hmacsha256_update(&st, (const uint8_t *)&seq, sizeof(seq));
    crypto_auth_hmacsha256_update(&st, (const uint8_t *)node, strlen(node));
    crypto_auth_hmacsha256_final(&st, hmac);

    /* hex-encode first 16 bytes for a compact sig */
    for (int i = 0; i < 16 && (size_t)(i*2+3) < sig_hex_len; i++)
        snprintf(sig_hex + i*2, 3, "%02x", hmac[i]);
    sodium_memzero(key, sizeof(key));
    sodium_memzero(keybuf, sizeof(keybuf));
#else
    (void)node; (void)ts; (void)seq;
    snprintf(sig_hex, sig_hex_len, "00000000000000000000000000000000");
#endif
}

static long current_rss_mb(void) {
#ifdef __APPLE__
    struct rusage_info_v2 ri;
    if (proc_pid_rusage((int)getpid(), RUSAGE_INFO_V2, (rusage_info_t *)&ri) == 0)
        return (long)(ri.ri_resident_size >> 20);
#elif defined(__linux__)
    FILE *f = fopen("/proc/self/statm", "r");
    if (f) {
        unsigned long size_pages = 0, rss_pages = 0;
        int got = fscanf(f, "%lu %lu", &size_pages, &rss_pages);
        fclose(f);
        if (got == 2) {
            long psz = sysconf(_SC_PAGESIZE);
            return (long)(((uint64_t)rss_pages * (uint64_t)(psz > 0 ? psz : 4096)) >> 20);
        }
    }
#endif
    struct rusage ru;
    if (getrusage(RUSAGE_SELF, &ru) == 0) {
#ifdef __APPLE__
        return (long)(ru.ru_maxrss >> 20);       /* bytes on macOS */
#else
        return (long)(ru.ru_maxrss / 1024);      /* KiB on Linux/BSD */
#endif
    }
    return -1;
}

static int current_mem_pressure(void) {
#ifdef __APPLE__
    int level = 0;
    size_t len = sizeof(level);
    if (sysctlbyname("kern.memorystatus_vm_pressure_level", &level, &len, NULL, 0) == 0)
        return level;
#endif
    return 0;
}

static int count_dir_entries(const char *path) {
    DIR *d = opendir(path);
    if (!d) return -1;
    int n = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
        n++;
    }
    closedir(d);
    return n;
}

static int current_fd_count(void) {
#if defined(__linux__)
    int n = count_dir_entries("/proc/self/fd");
    if (n >= 0) return n;
#endif
    return count_dir_entries("/dev/fd");
}

static int current_thread_count(void) {
#ifdef __APPLE__
    struct proc_taskinfo ti;
    memset(&ti, 0, sizeof(ti));
    int n = proc_pidinfo((int)getpid(), PROC_PIDTASKINFO, 0, &ti, sizeof(ti));
    if (n == (int)sizeof(ti)) return (int)ti.pti_threadnum;
#elif defined(__linux__)
    return count_dir_entries("/proc/self/task");
#endif
    return -1;
}

static long maxrss_mb_from_rusage(const struct rusage *ru) {
#ifdef __APPLE__
    return (long)(ru->ru_maxrss >> 20);      /* bytes on macOS */
#else
    return (long)(ru->ru_maxrss / 1024);     /* KiB on Linux/BSD */
#endif
}

static void collect_runtime_sample(runtime_sample_t *s) {
    memset(s, 0, sizeof(*s));
    s->rss_mb = current_rss_mb();
    s->fd_count = current_fd_count();
    s->thread_count = current_thread_count();
    s->mem_pressure = current_mem_pressure();

    struct rusage ru;
    if (getrusage(RUSAGE_SELF, &ru) == 0) {
        s->maxrss_mb = maxrss_mb_from_rusage(&ru);
        s->user_ms = (long)ru.ru_utime.tv_sec * 1000L + (long)ru.ru_utime.tv_usec / 1000L;
        s->sys_ms  = (long)ru.ru_stime.tv_sec * 1000L + (long)ru.ru_stime.tv_usec / 1000L;
        s->minor_faults = ru.ru_minflt;
        s->major_faults = ru.ru_majflt;
        s->vol_ctx_switches = ru.ru_nvcsw;
        s->invol_ctx_switches = ru.ru_nivcsw;
    } else {
        s->maxrss_mb = -1;
        s->user_ms = s->sys_ms = -1;
        s->minor_faults = s->major_faults = -1;
        s->vol_ctx_switches = s->invol_ctx_switches = -1;
    }
}

static void persist_runtime_state(const char *json) {
    char dir[512];
    dsco_dir(dir, sizeof(dir));

    char path[640], tmp[672], live[672], log_path[640];
    snprintf(path, sizeof(path), "%s/last_heartbeat.json", dir);
    snprintf(tmp, sizeof(tmp), "%s/last_heartbeat.%d.tmp", dir, (int)getpid());
    snprintf(live, sizeof(live), "%s/runtime-%d.json", dir, (int)getpid());
    snprintf(log_path, sizeof(log_path), "%s/runtime.log", dir);

    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) {
        (void)!write(fd, json, strlen(json));
        (void)!write(fd, "\n", 1);
        close(fd);
        rename(tmp, path);
    }

    fd = open(live, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) {
        (void)!write(fd, json, strlen(json));
        (void)!write(fd, "\n", 1);
        close(fd);
    }

    fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd >= 0) {
        (void)!write(fd, json, strlen(json));
        (void)!write(fd, "\n", 1);
        close(fd);
    }
}

static void json_escape_small(const char *s, char *out, size_t len) {
    if (!out || len == 0) return;
    size_t j = 0;
    if (!s) s = "";
    for (size_t i = 0; s[i] && j + 2 < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') {
            if (j + 2 >= len) break;
            out[j++] = '\\';
            out[j++] = (char)c;
        } else if (c >= 32 && c < 127) {
            out[j++] = (char)c;
        }
    }
    out[j] = '\0';
}

static bool argv_token_sensitive(const char *s) {
    if (!s) return false;
    char lower[160];
    size_t n = strlen(s);
    if (n >= sizeof(lower)) n = sizeof(lower) - 1;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        lower[i] = (char)((c >= 'A' && c <= 'Z') ? c + 32 : c);
    }
    lower[n] = '\0';
    return strstr(lower, "key") ||
           strstr(lower, "token") ||
           strstr(lower, "secret") ||
           strstr(lower, "password") ||
           strstr(lower, "passwd") ||
           strstr(lower, "bearer") ||
           strstr(lower, "auth");
}

static void append_cmd_arg(char *dst, size_t cap, const char *arg, bool redact) {
    if (!dst || cap == 0 || !arg) return;
    size_t used = strlen(dst);
    if (used + 2 >= cap) return;
    if (used > 0) dst[used++] = ' ';
    dst[used] = '\0';

    const char *src = redact ? "[redacted]" : arg;
    for (size_t i = 0; src[i] && used + 2 < cap; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c < 32 || c >= 127) continue;
        if (c == '"' || c == '\\') {
            if (used + 2 >= cap) break;
            dst[used++] = '\\';
            dst[used++] = (char)c;
        } else if (c == ' ') {
            dst[used++] = '_';
        } else {
            dst[used++] = (char)c;
        }
        dst[used] = '\0';
    }
}

static void refresh_exec_path(void) {
#ifdef __APPLE__
    if (proc_pidpath((int)getpid(), s_exe, (uint32_t)sizeof(s_exe)) <= 0)
        s_exe[0] = '\0';
#elif defined(__linux__)
    ssize_t n = readlink("/proc/self/exe", s_exe, sizeof(s_exe) - 1);
    if (n > 0) s_exe[n] = '\0';
    else s_exe[0] = '\0';
#else
    s_exe[0] = '\0';
#endif
}

static void emit_beacon_event(const char *event, const char *detail) {
    pthread_mutex_lock(&s_emit_lock);
    watchdog_ping();

    char node[256];
    get_node_id(node, sizeof(node));
    int64_t ts      = (int64_t)time(NULL);
    uint64_t seq    = s_seq++;
    long uptime     = (long)(ts - s_start_ts);
    runtime_sample_t sample;
    collect_runtime_sample(&sample);
    long rss_delta_mb = (s_last_rss_mb >= 0 && sample.rss_mb >= 0) ?
                        sample.rss_mb - s_last_rss_mb : 0;
    if (sample.rss_mb >= 0) {
        s_last_rss_mb = sample.rss_mb;
        if (sample.rss_mb > s_peak_rss_mb) s_peak_rss_mb = sample.rss_mb;
    }
    const char *supervised = getenv("DSCO_SUPERVISED");
    const char *mem_restart = getenv("DSCO_MEM_PRESSURE");

    char sig[64] = {0};
    sign_beacon(node, ts, seq, sig, sizeof(sig));

    char event_buf[64], detail_buf[192], node_buf[256];
    char phase_buf[128], cmd_buf[640], cwd_buf[PATH_MAX + 64], exe_buf[PATH_MAX + 64];
    json_escape_small(event ? event : "heartbeat", event_buf, sizeof(event_buf));
    json_escape_small(detail ? detail : "", detail_buf, sizeof(detail_buf));
    json_escape_small(node, node_buf, sizeof(node_buf));
    json_escape_small(s_phase, phase_buf, sizeof(phase_buf));
    json_escape_small(s_cmdline, cmd_buf, sizeof(cmd_buf));
    json_escape_small(s_cwd, cwd_buf, sizeof(cwd_buf));
    json_escape_small(s_exe, exe_buf, sizeof(exe_buf));

    char json[4096];
    snprintf(json, sizeof(json),
        "{\"event\":\"%s\",\"node\":\"%s\",\"pid\":%d,\"ppid\":%d,"
        "\"ts\":%lld,\"seq\":%llu,\"uptime_s\":%ld,"
        "\"rss_mb\":%ld,\"rss_delta_mb\":%ld,\"peak_rss_mb\":%ld,"
        "\"maxrss_mb\":%ld,\"fd_count\":%d,\"thread_count\":%d,"
        "\"cpu_user_ms\":%ld,\"cpu_sys_ms\":%ld,"
        "\"minor_faults\":%ld,\"major_faults\":%ld,"
        "\"ctx_switches_vol\":%ld,\"ctx_switches_invol\":%ld,"
        "\"mem_pressure\":%d,\"phase\":\"%s\","
        "\"supervised\":\"%s\",\"mem_restart\":%d,"
        "\"detail\":\"%s\",\"cmdline\":\"%s\",\"cwd\":\"%s\","
        "\"exe\":\"%s\",\"sig\":\"%s\"}",
        event_buf, node_buf, (int)getpid(), (int)getppid(),
        (long long)ts, (unsigned long long)seq, uptime,
        sample.rss_mb, rss_delta_mb, s_peak_rss_mb,
        sample.maxrss_mb, sample.fd_count, sample.thread_count,
        sample.user_ms, sample.sys_ms,
        sample.minor_faults, sample.major_faults,
        sample.vol_ctx_switches, sample.invol_ctx_switches,
        sample.mem_pressure, phase_buf,
        supervised ? supervised : "",
        mem_restart && mem_restart[0] ? 1 : 0,
        detail_buf, cmd_buf, cwd_buf, exe_buf, sig);

    audit_log("heartbeat", json);
    persist_runtime_state(json);
    pthread_mutex_unlock(&s_emit_lock);

    /* phone home if URL configured */
    char url[512] = {0};
    sealed_store_get("DSCO_BEACON_URL", url, sizeof(url));
    if (!url[0]) {
        const char *e = getenv("DSCO_BEACON_URL");
        if (e) snprintf(url, sizeof(url), "%s", e);
    }

#if defined(HAVE_MBEDTLS) && defined(HAVE_LIBSODIUM)
    if (url[0]) {
        /* parse host / port / path from URL */
        char host[256] = {0}, path[256] = "/beacon";
        int port = 443;
        int use_tls = 1;
        const char *p = url;
        if (strncmp(p, "https://", 8) == 0) { p += 8; use_tls = 1; port = 443; }
        else if (strncmp(p, "http://", 7) == 0) { p += 7; use_tls = 0; port = 80; }

        const char *slash = strchr(p, '/');
        const char *colon = strchr(p, ':');
        if (colon && (!slash || colon < slash)) {
            snprintf(host, (size_t)(colon - p + 1), "%s", p);
            port = atoi(colon + 1);
        } else if (slash) {
            snprintf(host, (size_t)(slash - p + 1), "%s", p);
        } else {
            snprintf(host, sizeof(host), "%s", p);
        }
        if (slash) snprintf(path, sizeof(path), "%s", slash);

        uint8_t auth_key[32] = {0};
        char akbuf[512] = {0};
        sealed_store_get("DSCO_NET_AUTH_KEY", akbuf, sizeof(akbuf));
        if (akbuf[0])
            crypto_generichash(auth_key, sizeof(auth_key),
                               (const uint8_t *)akbuf, strlen(akbuf), NULL, 0);

        char *resp = netsrv_client_post(host, (uint16_t)port, path, json,
                                        auth_key, sizeof(auth_key), use_tls);
        free(resp);
    }
#endif
}

static void emit_beacon(void) {
    emit_beacon_event("heartbeat", "");
}

/* ── thread ─────────────────────────────────────────────────────────────── */

static void *beacon_thread(void *arg) {
    (void)arg;
    while (atomic_load(&s_running)) {
        emit_beacon();
        int secs = get_interval();
        for (int i = 0; i < secs && atomic_load(&s_running); i++) {
            if (atomic_exchange(&s_poke, 0)) break;
            sleep(1);
        }
    }
    return NULL;
}

/* ── public API ─────────────────────────────────────────────────────────── */

void heartbeat_start(void) {
    if (atomic_load(&s_running)) return;
    s_start_ts = time(NULL);
    s_seq      = 0;
    s_last_rss_mb = -1;
    s_peak_rss_mb = -1;
    atomic_store(&s_running, 1);
    emit_beacon_event("start", "");
    pthread_create(&s_thread, NULL, beacon_thread, NULL);
}

void heartbeat_set_context(int argc, char **argv) {
    pthread_mutex_lock(&s_emit_lock);
    s_cmdline[0] = '\0';
    bool redact_next = false;
    for (int i = 0; i < argc && argv && argv[i]; i++) {
        bool redact = redact_next || argv_token_sensitive(argv[i]);
        append_cmd_arg(s_cmdline, sizeof(s_cmdline), argv[i], redact);
        redact_next = argv_token_sensitive(argv[i]) &&
                      strchr(argv[i], '=') == NULL &&
                      argv[i][0] == '-';
    }
    if (!getcwd(s_cwd, sizeof(s_cwd))) s_cwd[0] = '\0';
    refresh_exec_path();
    pthread_mutex_unlock(&s_emit_lock);
}

void heartbeat_set_phase(const char *phase) {
    pthread_mutex_lock(&s_emit_lock);
    if (phase && phase[0]) {
        snprintf(s_phase, sizeof(s_phase), "%s", phase);
    }
    pthread_mutex_unlock(&s_emit_lock);
    if (atomic_load(&s_running)) heartbeat_note_event("phase", phase ? phase : "");
}

void heartbeat_stop(void) {
    if (!atomic_load(&s_running)) return;
    emit_beacon_event("clean_exit", "heartbeat_stop");
    atomic_store(&s_running, 0);
    atomic_store(&s_poke, 1);
    pthread_join(s_thread, NULL);
}

bool heartbeat_running(void) {
    return atomic_load(&s_running) != 0;
}

void heartbeat_poke(void) {
    atomic_store(&s_poke, 1);
}

void heartbeat_note_event(const char *event, const char *detail) {
    if (s_start_ts == 0) s_start_ts = time(NULL);
    emit_beacon_event(event ? event : "event", detail ? detail : "");
}
