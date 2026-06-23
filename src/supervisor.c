/* src/supervisor.c — Higher-order process supervisor for dsco
 *
 * Runs the real dsco as a managed child process. On abnormal death:
 *   1. Classifies the exit (clean / nonzero / fatal signal / OOM-kill)
 *   2. Captures diagnostics from /tmp/dsco_crash.log + per-pid backtrace
 *   3. Restarts with exponential backoff + crash-loop circuit breaker
 *
 * The child inherits the controlling terminal directly (no pipe), so the
 * interactive TUI works unchanged. Signals are forwarded.
 *
 * See include/supervisor.h for the full design doc and tunables. */

#define _POSIX_C_SOURCE 200809L
/* macOS hides proc_pid_rusage(), rusage_info_v2 and the full sysctl surface
 * behind the Darwin namespace; _POSIX_C_SOURCE alone masks them. */
#ifdef __APPLE__
#define _DARWIN_C_SOURCE 1
#endif

#include "supervisor.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/sysctl.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <time.h>
#include <stdarg.h>
#include <limits.h>
#ifdef __APPLE__
#include <libproc.h>
#endif

/* ── Defaults ─────────────────────────────────────────────────────────── */
#define DEFAULT_MAX_RESTARTS    20
#define DEFAULT_WINDOW_S        120
#define DEFAULT_STABLE_S        30
#define DEFAULT_BACKOFF_MS      250
#define DEFAULT_BACKOFF_MAX_MS  15000

/* Memory watchdog: FRIENDLY TENANT.
 * >= 4GB floor/session. Dynamic fleet budget. Pre-empt only at CRITICAL pressure. */
#define DEFAULT_POLL_MS         500
#define DEFAULT_MEM_SOFT_PCT    80
#define DEFAULT_TERM_GRACE_MS   8000
#define DEFAULT_METRICS_S       10

#define DSCO_FLEET_LOCK_PATH    "/tmp/dsco_fleet.lock"
#define DSCO_FLEET_JSON_PATH    "/tmp/dsco_fleet.json"
#define DSCO_POOL_BASE_MB           2048ULL  /* 2 GB starting pool    */
#define DSCO_POOL_STEP_MB           1024ULL  /* +1 GB per step        */
#define DSCO_PREEMPT_PRESSURE_LEVEL 4

#define INCIDENT_SNIPPET_BYTES  8192

static int env_int(const char *name, int def, int min_v, int max_v) {
    const char *v = getenv(name);
    if (!v || !v[0]) return def;
    int n = atoi(v);
    if (n < min_v) return min_v;
    if (n > max_v) return max_v;
    return n;
}

static double now_monotonic(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void sleep_ms(int ms) {
    if (ms <= 0) return;
    struct timespec ts = {
        .tv_sec  = ms / 1000,
        .tv_nsec = (long)(ms % 1000) * 1000000L,
    };
    nanosleep(&ts, NULL);
}

static void supervisor_dir(char *dir, size_t len) {
    const char *home = getenv("HOME");
    if (home && home[0]) snprintf(dir, len, "%s/.dsco", home);
    else snprintf(dir, len, "/tmp/.dsco");
    if (mkdir(dir, 0700) != 0 && errno != EEXIST) {
        /* best effort */
    }
}

static void supervisor_log(const char *fmt, ...) {
    char dir[512];
    supervisor_dir(dir, sizeof(dir));
    if (mkdir(dir, 0700) != 0 && errno != EEXIST) {
        return;
    }

    char path[640];
    snprintf(path, sizeof(path), "%s/supervisor.log", dir);
    FILE *f = fopen(path, "a");
    if (!f) return;

    time_t t = time(NULL);
    struct tm tmv;
    localtime_r(&t, &tmv);
    char ts[64];
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S%z", &tmv);
    fprintf(f, "%s pid=%d ", ts, (int)getpid());

    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);

    fputc('\n', f);
    fclose(f);
}

static void json_escape_text(const char *s, char *out, size_t cap) {
    if (!out || cap == 0) return;
    if (!s) s = "";
    size_t j = 0;
    for (size_t i = 0; s[i] && j + 2 < cap; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') {
            if (j + 2 >= cap) break;
            out[j++] = '\\';
            out[j++] = (char)c;
        } else if (c == '\n') {
            if (j + 2 >= cap) break;
            out[j++] = '\\';
            out[j++] = 'n';
        } else if (c == '\r') {
            if (j + 2 >= cap) break;
            out[j++] = '\\';
            out[j++] = 'r';
        } else if (c == '\t') {
            if (j + 2 >= cap) break;
            out[j++] = '\\';
            out[j++] = 't';
        } else if (c >= 32 && c < 127) {
            out[j++] = (char)c;
        }
    }
    out[j] = '\0';
}

static bool json_object_like(const char *s) {
    if (!s) return false;
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
    if (*s != '{') return false;
    const char *end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' ||
                       end[-1] == '\n' || end[-1] == '\r')) end--;
    return end > s && end[-1] == '}';
}

static bool json_find_i64(const char *json, const char *key, long long *out) {
    if (!json || !key || !out) return false;
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char *p = strstr(json, needle);
    if (!p) return false;
    p += strlen(needle);
    while (*p == ' ' || *p == '\t') p++;
    char *end = NULL;
    long long v = strtoll(p, &end, 10);
    if (end == p) return false;
    *out = v;
    return true;
}

static bool read_file_limited(const char *path, char *buf, size_t cap) {
    if (!buf || cap == 0) return false;
    buf[0] = '\0';
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    ssize_t n = read(fd, buf, cap - 1);
    close(fd);
    if (n <= 0) return false;
    buf[n] = '\0';
    return true;
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
        dst[used++] = (c == ' ') ? '_' : (char)c;
        dst[used] = '\0';
    }
}

static void format_cmdline(int argc, char **argv, char *out, size_t cap) {
    if (!out || cap == 0) return;
    out[0] = '\0';
    bool redact_next = false;
    for (int i = 0; i < argc && argv && argv[i]; i++) {
        bool redact = redact_next || argv_token_sensitive(argv[i]);
        append_cmd_arg(out, cap, argv[i], redact);
        redact_next = argv_token_sensitive(argv[i]) &&
                      strchr(argv[i], '=') == NULL &&
                      argv[i][0] == '-';
    }
}

static void child_metrics_path(pid_t child, char *path, size_t len) {
    char dir[512];
    supervisor_dir(dir, sizeof(dir));
    snprintf(path, len, "%s/child-metrics-%d.jsonl", dir, (int)child);
}

static void append_child_metric(const char *path, pid_t child, double uptime,
                                uint64_t rss, uint64_t peak_rss, int pressure) {
    FILE *f = fopen(path, "a");
    if (!f) return;
    fprintf(f,
            "{\"ts\":%lld,\"supervisor_pid\":%d,\"child_pid\":%d,"
            "\"uptime_s\":%.3f,\"rss_mb\":%llu,\"peak_rss_mb\":%llu,"
            "\"mem_pressure\":%d}\n",
            (long long)time(NULL), (int)getpid(), (int)child, uptime,
            (unsigned long long)(rss >> 20),
            (unsigned long long)(peak_rss >> 20),
            pressure);
    fclose(f);
}

static const char *signal_name(int sig) {
    switch (sig) {
        case 0:       return "none";
        case SIGKILL: return "SIGKILL";
        case SIGTERM: return "SIGTERM";
        case SIGINT:  return "SIGINT";
        case SIGSEGV: return "SIGSEGV";
        case SIGBUS:  return "SIGBUS";
        case SIGABRT: return "SIGABRT";
        case SIGFPE:  return "SIGFPE";
        case SIGILL:  return "SIGILL";
        case SIGHUP:  return "SIGHUP";
        default:      return "UNKNOWN";
    }
}

/* ── Memory introspection ─────────────────────────────────────────────── */

/* Total physical RAM in bytes (0 = unknown). */
static uint64_t system_mem_bytes(void) {
    uint64_t mem = 0;
#ifdef __APPLE__
    size_t len = sizeof(mem);
    if (sysctlbyname("hw.memsize", &mem, &len, NULL, 0) != 0) return 0;
#else
    long pages = sysconf(_SC_PHYS_PAGES);
    long psz   = sysconf(_SC_PAGESIZE);
    if (pages > 0 && psz > 0) mem = (uint64_t)pages * (uint64_t)psz;
#endif
    return mem;
}

/* Resident-set / physical footprint of a live pid in bytes (0 = unknown). */
static uint64_t child_rss_bytes(pid_t pid) {
#ifdef __APPLE__
    struct rusage_info_v2 ri;
    if (proc_pid_rusage((int)pid, RUSAGE_INFO_V2, (rusage_info_t *)&ri) == 0)
        return ri.ri_resident_size;
    return 0;
#else
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/statm", (int)pid);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    unsigned long size_pages = 0, rss_pages = 0;
    int got = fscanf(f, "%lu %lu", &size_pages, &rss_pages);
    fclose(f);
    if (got != 2) return 0;
    long psz = sysconf(_SC_PAGESIZE);
    return (uint64_t)rss_pages * (uint64_t)(psz > 0 ? psz : 4096);
#endif
}

/* macOS VM pressure level: 1=normal, 2=warn, 4=critical (0 = unknown). */
static int system_mem_pressure(void) {
#ifdef __APPLE__
    int level = 0;
    size_t len = sizeof(level);
    if (sysctlbyname("kern.memorystatus_vm_pressure_level", &level, &len, NULL, 0) != 0)
        return 0;
    return level;
#else
    return 0;
#endif
}


/* ============================================================
 * Fleet registry: cross-session dynamic memory budget
 * ============================================================ */
static uint64_t fleet_total_rss_mb(void) {
    int fd = open(DSCO_FLEET_JSON_PATH, O_RDONLY); if (fd < 0) return 0;
    char buf[16384]; buf[0] = '\0';
    ssize_t n = read(fd, buf, sizeof(buf)-1); close(fd);
    if (n <= 0) return 0; buf[n] = '\0';
    uint64_t total = 0; const char *p = buf;
    while ((p = strstr(p, "\"rss_mb\":")) != NULL)
        { p += 10; total += (uint64_t)strtoull(p, NULL, 10); }
    return total;
}

static void fleet_update(pid_t child, uint64_t rss_bytes, uint64_t budget_mb) {
    int lfd = open(DSCO_FLEET_LOCK_PATH, O_RDWR|O_CREAT, 0600);
    if (lfd < 0) return;
    if (flock(lfd, LOCK_EX|LOCK_NB) != 0) { close(lfd); return; }
    char buf[16384]; buf[0]='\0';
    int jfd = open(DSCO_FLEET_JSON_PATH, O_RDONLY);
    if (jfd>=0) { ssize_t n=read(jfd,buf,sizeof(buf)-1); if(n>0) buf[n]='\0'; close(jfd); }
    char mykey[32]; snprintf(mykey,sizeof(mykey),"\"sup%d\"",(int)getpid());
    char out[16384]; int op=0; out[op++]='{';
    int w=snprintf(out+op,sizeof(out)-op,
        "%s:{\"sup\":%d,\"child\":%d,\"rss_mb\":%llu,\"budget_mb\":%llu,\"ts\":%lld}",
        mykey,(int)getpid(),(int)child,
        (unsigned long long)(rss_bytes>>20),(unsigned long long)budget_mb,(long long)time(NULL));
    if(w>0 && w<(int)sizeof(out)-op-2) op+=w;
    if (buf[0]=='{') {
        const char *p=buf+1;
        while(*p && op<(int)sizeof(out)-256) {
            const char *q=strchr(p,'"'); if(!q) break;
            if(strncmp(q,mykey,strlen(mykey))==0){
                const char *ob=strchr(q,'{'); const char *cb=ob?strchr(ob,'}'):NULL;
                p=cb?cb+1:q+strlen(q); if(*p==',')p++; continue;}
            const char *ob=strchr(q,'{'); const char *cb=ob?strchr(ob,'}'):NULL;
            if(!ob||!cb) break;
            const char *tsp=strstr(ob,"\"ts\":"); long long ts=0;
            if(tsp) ts=strtoll(tsp+6,NULL,10);
            if(ts>0 && (long long)time(NULL)-ts>60){p=cb+1;if(*p==',')p++;continue;}
            out[op++]=',';
            int span=(int)(cb-q)+1;
            if(op+span<(int)sizeof(out)-2){memcpy(out+op,q,span);op+=span;}
            p=cb+1; if(*p==',')p++;
        }
    }
    if(op<(int)sizeof(out)-1) out[op++]='}'; out[op]='\0';
    jfd=open(DSCO_FLEET_JSON_PATH,O_WRONLY|O_CREAT|O_TRUNC,0644);
    if(jfd>=0){(void)write(jfd,out,op);close(jfd);}
    flock(lfd,LOCK_UN); close(lfd);
}

static int fleet_session_count(void) {
    int fd=open(DSCO_FLEET_JSON_PATH,O_RDONLY); if(fd<0) return 1;
    char buf[16384]; buf[0]='\0';
    ssize_t n=read(fd,buf,sizeof(buf)-1); close(fd);
    if(n<=0) return 1; buf[n]='\0';
    int c=0; const char *p=buf;
    while((p=strstr(p,"\"ts\":"))!=NULL){c++;p+=6;}
    return c>0?c:1;
}

/* Dynamic budget: max(4GB, (avail_ram + our_rss) / n_sessions), cap 90% RAM */
/* Shared-pool model, no doublings:
 *   pool = 2GB + k*1GB where k = steps until pool >= fleet_total_rss
 *   budget = pool / n_sessions
 * 1 session  @   0MB: pool=2GB budget=2GB
 * 1 session  @ 2.1GB: pool=3GB budget=3GB
 * 2 sessions @   0MB: pool=2GB budget=1GB each
 * 2 sessions @ 2.1GB: pool=3GB budget=1.5GB each
 * 2 sessions @ 3.1GB: pool=4GB budget=2GB each  */
static uint64_t fleet_compute_budget(uint64_t our_rss_bytes) {
    (void)our_rss_bytes;
    const char *em = getenv("DSCO_SUPERVISE_MEM_LIMIT_MB");
    if (em && em[0] && atol(em) > 0) return (uint64_t)atol(em) << 20;

    uint64_t sys = system_mem_bytes();
    uint64_t sys_ceil_mb = sys > 0 ? (sys >> 20) * 90ULL / 100ULL : 43008ULL;

    uint64_t fleet_rss = fleet_total_rss_mb();
    uint64_t pool_mb = DSCO_POOL_BASE_MB;
    while (pool_mb < fleet_rss && pool_mb < sys_ceil_mb)
        pool_mb += DSCO_POOL_STEP_MB;
    if (pool_mb > sys_ceil_mb) pool_mb = sys_ceil_mb;

    int sessions = fleet_session_count();
    uint64_t budget_mb = sessions > 0 ? pool_mb / (uint64_t)sessions : pool_mb;
    if (budget_mb < 1) budget_mb = 1;
    return budget_mb << 20;
}

/* Startup budget: delegate to fleet_compute_budget.
 * The poll loop refreshes dynamically every tick. */
static uint64_t resolve_mem_hard_limit(void) {
    return fleet_compute_budget(0);
}

/* ── Crash classification ─────────────────────────────────────────────── */
typedef enum {
    EXIT_CLEAN = 0,       /* WIFEXITED, status 0 */
    EXIT_NONZERO,         /* WIFEXITED, status != 0 */
    EXIT_SIGNAL,          /* WIFSIGNALED — SIGSEGV, SIGBUS, SIGABRT, etc. */
    EXIT_OOM_KILL,        /* WIFSIGNALED, SIGKILL — likely jetsam/OOM */
} crash_class_t;

static const char *crash_class_name(crash_class_t c) {
    switch (c) {
        case EXIT_CLEAN:     return "clean";
        case EXIT_NONZERO:   return "nonzero";
        case EXIT_SIGNAL:    return "signal";
        case EXIT_OOM_KILL:  return "oom_kill";
    }
    return "unknown";
}

static crash_class_t classify_exit(int status) {
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status) == 0 ? EXIT_CLEAN : EXIT_NONZERO;
    }
    if (WIFSIGNALED(status)) {
        int sig = WTERMSIG(status);
        if (sig == SIGKILL) return EXIT_OOM_KILL;
        return EXIT_SIGNAL;
    }
    return EXIT_NONZERO;
}

typedef struct incident_report {
    pid_t child_pid;
    crash_class_t cls;
    int sig;
    int exit_code;
    double uptime;
    uint64_t peak_rss;
    uint64_t mem_hard;
    uint64_t mem_soft;
    int pressure;
    int restart_count;
    int next_delay_ms;
    int poll_ms;
    bool preempted;
    bool tracer_reaped;
    const char *action;
    const char *child_cmdline;
    const char *metrics_path;
} incident_report_t;

static void write_incident_json(FILE *f, const incident_report_t *r,
                                const char *incident_path) {
    char dir[512], hb_path[640], runtime_path[640], supervisor_path[640];
    supervisor_dir(dir, sizeof(dir));
    snprintf(hb_path, sizeof(hb_path), "%s/last_heartbeat.json", dir);
    snprintf(runtime_path, sizeof(runtime_path), "%s/runtime.log", dir);
    snprintf(supervisor_path, sizeof(supervisor_path), "%s/supervisor.log", dir);

    char last_hb[INCIDENT_SNIPPET_BYTES + 1];
    char crash_log[INCIDENT_SNIPPET_BYTES + 1];
    char bt_log[INCIDENT_SNIPPET_BYTES + 1];
    char bt_path[128];
    snprintf(bt_path, sizeof(bt_path), "/tmp/dsco_bt_%d.txt", (int)r->child_pid);

    bool have_hb = read_file_limited(hb_path, last_hb, sizeof(last_hb));
    bool have_crash = read_file_limited("/tmp/dsco_crash.log", crash_log, sizeof(crash_log));
    bool have_bt = read_file_limited(bt_path, bt_log, sizeof(bt_log));
    long long hb_pid = -1, hb_ts = 0;
    bool have_hb_pid = have_hb && json_find_i64(last_hb, "pid", &hb_pid);
    bool have_hb_ts = have_hb && json_find_i64(last_hb, "ts", &hb_ts);
    long long hb_age_s = have_hb_ts ? ((long long)time(NULL) - hb_ts) : -1;

    char action_esc[128], cmd_esc[768], metrics_esc[PATH_MAX + 64];
    char incident_esc[PATH_MAX + 64], hb_path_esc[PATH_MAX + 64];
    char runtime_esc[PATH_MAX + 64], supervisor_esc[PATH_MAX + 64];
    char crash_esc[(INCIDENT_SNIPPET_BYTES * 2) + 1];
    char bt_esc[(INCIDENT_SNIPPET_BYTES * 2) + 1];
    char hb_esc[(INCIDENT_SNIPPET_BYTES * 2) + 1];
    json_escape_text(r->action ? r->action : "", action_esc, sizeof(action_esc));
    json_escape_text(r->child_cmdline ? r->child_cmdline : "", cmd_esc, sizeof(cmd_esc));
    json_escape_text(r->metrics_path ? r->metrics_path : "", metrics_esc, sizeof(metrics_esc));
    json_escape_text(incident_path ? incident_path : "", incident_esc, sizeof(incident_esc));
    json_escape_text(hb_path, hb_path_esc, sizeof(hb_path_esc));
    json_escape_text(runtime_path, runtime_esc, sizeof(runtime_esc));
    json_escape_text(supervisor_path, supervisor_esc, sizeof(supervisor_esc));
    json_escape_text(have_crash ? crash_log : "", crash_esc, sizeof(crash_esc));
    json_escape_text(have_bt ? bt_log : "", bt_esc, sizeof(bt_esc));
    json_escape_text(have_hb ? last_hb : "", hb_esc, sizeof(hb_esc));

    fprintf(f,
            "{\n"
            "  \"event\":\"supervisor_incident\",\n"
            "  \"ts\":%lld,\n"
            "  \"supervisor_pid\":%d,\n"
            "  \"child_pid\":%d,\n"
            "  \"class\":\"%s\",\n"
            "  \"signal\":\"%s\",\n"
            "  \"signal_num\":%d,\n"
            "  \"exit_code\":%d,\n"
            "  \"action\":\"%s\",\n"
            "  \"restart_count\":%d,\n"
            "  \"next_delay_ms\":%d,\n"
            "  \"uptime_s\":%.3f,\n"
            "  \"peak_rss_mb\":%llu,\n"
            "  \"mem_budget_mb\":%llu,\n"
            "  \"mem_soft_mb\":%llu,\n"
            "  \"mem_pressure\":%d,\n"
            "  \"poll_ms\":%d,\n"
            "  \"preempted\":%s,\n"
            "  \"tracer_reaped\":%s,\n"
            "  \"resume_after_crash\":%s,\n"
            "  \"mem_restart\":%s,\n"
            "  \"last_heartbeat_pid\":%lld,\n"
            "  \"last_heartbeat_age_s\":%lld,\n"
            "  \"last_heartbeat_pid_matches_child\":%s,\n"
            "  \"child_cmdline\":\"%s\",\n",
            (long long)time(NULL),
            (int)getpid(),
            (int)r->child_pid,
            crash_class_name(r->cls),
            signal_name(r->sig),
            r->sig,
            r->exit_code,
            action_esc,
            r->restart_count,
            r->next_delay_ms,
            r->uptime,
            (unsigned long long)(r->peak_rss >> 20),
            (unsigned long long)(r->mem_hard >> 20),
            (unsigned long long)(r->mem_soft >> 20),
            r->pressure,
            r->poll_ms,
            r->preempted ? "true" : "false",
            r->tracer_reaped ? "true" : "false",
            getenv("DSCO_RESUME_AFTER_CRASH") ? "true" : "false",
            getenv("DSCO_MEM_PRESSURE") ? "true" : "false",
            have_hb_pid ? hb_pid : -1,
            hb_age_s,
            (have_hb_pid && hb_pid == (long long)r->child_pid) ? "true" : "false",
            cmd_esc);

    fprintf(f,
            "  \"paths\":{"
            "\"incident\":\"%s\","
            "\"last_heartbeat\":\"%s\","
            "\"runtime_log\":\"%s\","
            "\"supervisor_log\":\"%s\","
            "\"metrics\":\"%s\","
            "\"crash_log\":\"/tmp/dsco_crash.log\","
            "\"debugger_backtrace\":\"%s\"},\n",
            incident_esc, hb_path_esc, runtime_esc, supervisor_esc,
            metrics_esc, bt_path);

    fprintf(f, "  \"last_heartbeat\":");
    if (have_hb && json_object_like(last_hb)) fputs(last_hb, f);
    else fprintf(f, "\"%s\"", hb_esc);
    fprintf(f, ",\n");

    fprintf(f,
            "  \"crash_log_present\":%s,\n"
            "  \"debugger_backtrace_present\":%s,\n"
            "  \"crash_log_excerpt\":\"%s\",\n"
            "  \"debugger_backtrace_excerpt\":\"%s\"\n"
            "}\n",
            have_crash ? "true" : "false",
            have_bt ? "true" : "false",
            crash_esc,
            bt_esc);
}

static bool write_incident_report(const incident_report_t *r,
                                  char *out_path, size_t out_path_len) {
    char dir[512], incident_dir[640];
    supervisor_dir(dir, sizeof(dir));
    snprintf(incident_dir, sizeof(incident_dir), "%s/incidents", dir);
    if (mkdir(incident_dir, 0700) != 0 && errno != EEXIST) return false;

    time_t t = time(NULL);
    struct tm tmv;
    localtime_r(&t, &tmv);
    char stamp[64];
    strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tmv);
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/incident-%s-pid%d.json",
             incident_dir, stamp, (int)r->child_pid);

    FILE *f = fopen(path, "w");
    if (!f) return false;
    write_incident_json(f, r, path);
    fclose(f);

    char last_path[PATH_MAX];
    snprintf(last_path, sizeof(last_path), "%s/last_incident.json", dir);
    f = fopen(last_path, "w");
    if (f) {
        write_incident_json(f, r, path);
        fclose(f);
    }

    if (out_path && out_path_len > 0) snprintf(out_path, out_path_len, "%s", path);
    return true;
}

/* ── Diagnostic capture ───────────────────────────────────────────────── */
static void capture_diagnostics(pid_t child_pid, crash_class_t cls, int sig) {
    /* Read the child's crash log (written by crash_handler in main.c). */
    char path[64];
    snprintf(path, sizeof(path), "/tmp/dsco_crash.log");
    FILE *f = fopen(path, "r");
    if (f) {
        char line[512];
        fprintf(stderr, "\n[supervisor] ── crash log (/tmp/dsco_crash.log) ──\n");
        while (fgets(line, sizeof(line), f)) {
            fputs(line, stderr);
        }
        fclose(f);
        fprintf(stderr, "[supervisor] ── end crash log ──\n\n");
    } else {
        fprintf(stderr,
                "[supervisor] no /tmp/dsco_crash.log for pid=%d (%s/%s).\n",
                (int)child_pid, crash_class_name(cls), signal_name(sig));
        if (cls == EXIT_OOM_KILL || sig == SIGKILL) {
            fprintf(stderr,
                    "[supervisor] SIGKILL cannot be caught by the child; "
                    "check ~/.dsco/last_heartbeat.json and ~/.dsco/runtime.log "
                    "for the last recorded RSS/memory-pressure state.\n");
        }
    }

    /* Check for per-pid backtrace from the debugger. */
    snprintf(path, sizeof(path), "/tmp/dsco_bt_%d.txt", (int)child_pid);
    f = fopen(path, "r");
    if (f) {
        char line[512];
        fprintf(stderr, "[supervisor] ── debugger backtrace (%s) ──\n", path);
        while (fgets(line, sizeof(line), f)) {
            fputs(line, stderr);
        }
        fclose(f);
        fprintf(stderr, "[supervisor] ── end backtrace ──\n\n");
    } else if (cls == EXIT_SIGNAL) {
        fprintf(stderr, "[supervisor] no debugger backtrace at %s.\n", path);
    }
}

/* ── Signal forwarding ────────────────────────────────────────────────── */
/* We want signals delivered to the supervisor to be forwarded to the child.
 * Since the child inherits the controlling terminal, terminal signals (SIGINT,
 * SIGTSTP, SIGQUIT) go directly to the child's process group. But SIGTERM
 * sent explicitly to the supervisor needs forwarding. */
static volatile sig_atomic_t g_forward_pid = -1;

static void forward_signal(int sig) {
    pid_t pid = g_forward_pid;
    if (pid > 0) kill(pid, sig);
    /* Re-install for System V semantics */
    signal(sig, forward_signal);
}

int supervisor_run(int child_argc, char **child_argv) {
    int max_restarts   = env_int("DSCO_SUPERVISE_MAX_RESTARTS", DEFAULT_MAX_RESTARTS, 1, 100);
    int window_s       = env_int("DSCO_SUPERVISE_WINDOW_S",     DEFAULT_WINDOW_S,     5, 3600);
    int stable_s       = env_int("DSCO_SUPERVISE_STABLE_S",      DEFAULT_STABLE_S,    5, 3600);
    int backoff_ms     = env_int("DSCO_SUPERVISE_BACKOFF_MS",    DEFAULT_BACKOFF_MS,  10, 60000);
    int backoff_max_ms = env_int("DSCO_SUPERVISE_BACKOFF_MAX_MS", DEFAULT_BACKOFF_MAX_MS, 100, 300000);

    /* ── Active memory watchdog tunables ──────────────────────────────── */
    int poll_ms        = env_int("DSCO_SUPERVISE_POLL_MS",       DEFAULT_POLL_MS,     20, 5000);
    int term_grace_ms  = env_int("DSCO_SUPERVISE_TERM_GRACE_MS", DEFAULT_TERM_GRACE_MS, 100, 60000);
    int soft_pct       = env_int("DSCO_SUPERVISE_MEM_SOFT_PCT",  DEFAULT_MEM_SOFT_PCT, 10, 99);
    int metrics_s      = env_int("DSCO_SUPERVISE_METRICS_SECS",  DEFAULT_METRICS_S,   0, 3600);
    uint64_t mem_hard_bytes = resolve_mem_hard_limit();
    uint64_t mem_soft_bytes = mem_hard_bytes / 100ULL * (uint64_t)soft_pct;
    char child_cmdline[512];
    format_cmdline(child_argc, child_argv, child_cmdline, sizeof(child_cmdline));
    {
        uint64_t sys = system_mem_bytes();
        fprintf(stderr,
            "[supervisor] memory watchdog: budget %lluMB (soft %lluMB) of "
            "%lluMB RAM, sampling every %dms.\n",
            (unsigned long long)(mem_hard_bytes >> 20),
            (unsigned long long)(mem_soft_bytes >> 20),
            (unsigned long long)(sys >> 20),
            poll_ms);
        supervisor_log("event=start child_cmdline=%s mem_budget_mb=%llu soft_mb=%llu ram_mb=%llu poll_ms=%d metrics_s=%d",
                       child_cmdline[0] ? child_cmdline : "(null)",
                       (unsigned long long)(mem_hard_bytes >> 20),
                       (unsigned long long)(mem_soft_bytes >> 20),
                       (unsigned long long)(sys >> 20),
                       poll_ms, metrics_s);
    }

    /* OTP restart type (erlang supervisor child_spec Restart):
     *   transient (default) — restart only on abnormal exit; a clean exit 0
     *                         (e.g. the user typed /quit) is honoured.
     *   permanent           — always restart, even on a clean exit. Use for a
     *                         daemon that must never stay down.
     *   temporary           — never restart; just report and propagate.
     * This is the genuinely useful piece of BEAM/OTP for "never get killed":
     * supervision semantics, not the bytecode emulator (dsco already has a
     * stack VM in vm.c + a reduction-style scheduler in scheduler.c). */
    const char *rt = getenv("DSCO_SUPERVISE_RESTART");
    enum { RT_TRANSIENT, RT_PERMANENT, RT_TEMPORARY } restart_type =
        (rt && strcmp(rt, "permanent") == 0) ? RT_PERMANENT :
        (rt && strcmp(rt, "temporary") == 0) ? RT_TEMPORARY : RT_TRANSIENT;

    /* Build the child argv: self-path + DSCO_NO_SUPERVISE=1 + passed args. */
    /* child_argv[0] is the original argv[0] (the dsco binary path). */
    /* The args after "supervise" were already stripped by main.c. */

    /* Set env so the child knows it's supervised and enables crash handlers. */
    setenv("DSCO_NO_SUPERVISE", "1", 1);
    setenv("DSCO_CRASH_DEBUGGER", "1", 1);

    /* Install signal forwarders. Terminal signals (SIGINT, SIGTSTP, SIGQUIT)
     * go directly to the child's process group since it owns the tty, but
     * SIGTERM and SIGHUP sent to the supervisor need forwarding. */
    signal(SIGTERM, forward_signal);
    signal(SIGHUP,  forward_signal);

    int restart_count = 0;
    double first_crash_time = 0;
    double last_start_time = 0;
    int current_backoff = backoff_ms;

    for (;;) {
        /* Reset the crash log so we don't re-read stale data. */
        unlink("/tmp/dsco_crash.log");

        char supervise_level[16];
        snprintf(supervise_level, sizeof(supervise_level), "%d", restart_count + 1);
        setenv("DSCO_SUPERVISED", supervise_level, 1);

        last_start_time = now_monotonic();

        pid_t child = fork();
        if (child < 0) {
            fprintf(stderr, "[supervisor] fork failed: %s\n", strerror(errno));
            supervisor_log("event=fork_failed errno=%d error=%s", errno, strerror(errno));
            return 1;
        }

        if (child == 0) {
            /* Child: restore default signal handlers, then exec. */
            signal(SIGTERM, SIG_DFL);
            signal(SIGHUP,  SIG_DFL);
            execvp(child_argv[0], child_argv);
            /* If exec fails, the child exits with an error. */
            fprintf(stderr, "[supervisor] exec failed: %s: %s\n",
                    child_argv[0], strerror(errno));
            _exit(127);
        }

        supervisor_log("event=child_start child_pid=%d generation=%d",
                       (int)child, restart_count + 1);

        /* Parent: actively monitor the child instead of blocking on it.
         * Sampling the child's RSS lets us pre-empt the kernel's uncatchable
         * SIGKILL: when the footprint approaches the memory budget we trigger
         * a graceful, resumable restart of our own — an event we control and
         * can checkpoint — rather than waiting for jetsam to reap the process
         * (and possibly the supervisor) with no warning. */
        g_forward_pid = child;
        int status = 0;
        pid_t wr = 0;
        uint64_t peak_rss = 0;
        bool soft_warned = false;
        bool preempted = false;   /* we asked the child to restart for memory */
        bool tracer_reaped = false; /* child reaped out from under us (lldb/gdb) */
        char metrics_path[PATH_MAX];
        child_metrics_path(child, metrics_path, sizeof(metrics_path));
        double last_metrics_at = 0.0;

        for (;;) {
            wr = waitpid(child, &status, WNOHANG);
            if (wr == child) break;             /* child exited on its own */
            if (wr < 0) {
                if (errno == EINTR) continue;
                if (errno == ECHILD) {
                    /* The child was reaped out from under us. This happens on
                     * the crash path: crash_handler ptrace-attaches lldb/gdb,
                     * and the traced child dies while the debugger owns the
                     * wait status. Only crashes attach a debugger, so we know
                     * it died abnormally — let the restart policy + diagnostics
                     * run rather than aborting supervision. */
                    tracer_reaped = true;
                    wr = child;
                    status = 0;
                    break;
                }
                fprintf(stderr, "[supervisor] waitpid failed: %s\n", strerror(errno));
                return 1;
            }

            /* wr == 0: child still alive — sample its memory. */
            uint64_t rss = child_rss_bytes(child);
            if (rss > peak_rss) peak_rss = rss;
            double sample_now = now_monotonic();

            /* Dynamic budget refresh + fleet registration every tick */
            mem_hard_bytes = fleet_compute_budget(rss);
            mem_soft_bytes = mem_hard_bytes / 100ULL * (uint64_t)soft_pct;
            fleet_update(child, rss, mem_hard_bytes >> 20);

            if (metrics_s > 0 &&
                (last_metrics_at == 0.0 || sample_now - last_metrics_at >= (double)metrics_s)) {
                append_child_metric(metrics_path, child, sample_now - last_start_time, rss,
                                    peak_rss, system_mem_pressure());
                last_metrics_at = sample_now;
            }

            /* Friendly tenant: only pre-empt on CRITICAL system pressure.
             * Level 1=normal, 2=warn (log+continue), 4=critical (act). */
            if (rss > 0 && rss >= mem_hard_bytes) {
                int pressure = system_mem_pressure();
                if (pressure < DSCO_PREEMPT_PRESSURE_LEVEL) {
                    if (!soft_warned) {
                        soft_warned = true;
                        fprintf(stderr,
                            "[supervisor] pid=%d RSS %lluMB > budget %lluMB"
                            " pressure=%d (need %d) — growing freely.\n",
                            (int)child,(unsigned long long)(rss>>20),
                            (unsigned long long)(mem_hard_bytes>>20),
                            pressure, DSCO_PREEMPT_PRESSURE_LEVEL);
                        supervisor_log("event=mem_grow child_pid=%d rss_mb=%llu budget_mb=%llu pressure=%d",
                            (int)child,(unsigned long long)(rss>>20),
                            (unsigned long long)(mem_hard_bytes>>20),pressure);
                    }
                    sleep_ms(poll_ms); continue;
                }
                fprintf(stderr,"\n[supervisor] pid=%d RSS %lluMB CRITICAL pressure=%d — restart.\n",
                    (int)child,(unsigned long long)(rss>>20),pressure);
                supervisor_log("event=mem_preempt child_pid=%d rss_mb=%llu budget_mb=%llu pressure=%d",
                    (int)child,(unsigned long long)(rss>>20),
                    (unsigned long long)(mem_hard_bytes>>20),pressure);
                preempted = true;

                /* Hand the child a chance to checkpoint and exit cleanly. */
                kill(child, SIGTERM);
                int waited_ms = 0;
                while (waited_ms < term_grace_ms) {
                    pid_t g = waitpid(child, &status, WNOHANG);
                    if (g == child) { wr = child; break; }
                    if (g < 0 && errno != EINTR) break;
                    sleep_ms(50);
                    waited_ms += 50;
                }
                /* Still alive after the grace window — force it down so the
                 * restart can proceed (better us than jetsam: we resume). */
                if (wr != child) {
                    kill(child, SIGKILL);
                    do { wr = waitpid(child, &status, 0); }
                    while (wr < 0 && errno == EINTR);
                }
                break;
            }

            if (rss > 0 && rss >= mem_soft_bytes && !soft_warned) {
                soft_warned = true;
                fprintf(stderr,
                    "[supervisor] child RSS %lluMB crossed soft watermark "
                    "%lluMB (budget %lluMB) — watching closely.\n",
                    (unsigned long long)(rss >> 20),
                    (unsigned long long)(mem_soft_bytes >> 20),
                    (unsigned long long)(mem_hard_bytes >> 20));
                supervisor_log("event=mem_soft child_pid=%d rss_mb=%llu soft_mb=%llu budget_mb=%llu",
                               (int)child,
                               (unsigned long long)(rss >> 20),
                               (unsigned long long)(mem_soft_bytes >> 20),
                               (unsigned long long)(mem_hard_bytes >> 20));
            }

            sleep_ms(poll_ms);
        }

        g_forward_pid = -1;

        if (wr < 0) {
            fprintf(stderr, "[supervisor] waitpid failed: %s\n", strerror(errno));
            return 1;
        }

        if (peak_rss > 0) {
            fprintf(stderr, "[supervisor] child pid=%d peak RSS: %lluMB.\n",
                    (int)child, (unsigned long long)(peak_rss >> 20));
        }

        /* A memory pre-emption is an abnormal exit we manufactured: classify
         * it as an OOM event so the restart path engages and the child is told
         * to come back leaner and resume from its last checkpoint. */
        crash_class_t cls = preempted     ? EXIT_OOM_KILL :
                            tracer_reaped  ? EXIT_SIGNAL   : classify_exit(status);

        /* Tell the next incarnation why it is restarting so it can resume the
         * session and (on memory events) start in a more conservative state. */
        if (cls == EXIT_OOM_KILL) {
            setenv("DSCO_MEM_PRESSURE", "1", 1);
            setenv("DSCO_RESUME_AFTER_CRASH", "1", 1);
        } else if (cls == EXIT_SIGNAL || cls == EXIT_NONZERO) {
            setenv("DSCO_RESUME_AFTER_CRASH", "1", 1);
        } else {
            unsetenv("DSCO_MEM_PRESSURE");
            unsetenv("DSCO_RESUME_AFTER_CRASH");
        }

        /* Clean exit — honour OTP restart semantics. transient/temporary stop;
         * only permanent keeps a cleanly-exited child alive. */
        if (cls == EXIT_CLEAN) {
            if (restart_type != RT_PERMANENT) {
                if (restart_count > 0)
                    fprintf(stderr, "[supervisor] child exited cleanly after %d restart(s).\n",
                            restart_count);
                supervisor_log("event=child_exit child_pid=%d class=%s uptime_s=%.1f peak_rss_mb=%llu",
                               (int)child, crash_class_name(cls),
                               now_monotonic() - last_start_time,
                               (unsigned long long)(peak_rss >> 20));
                return 0;
            }
            fprintf(stderr, "[supervisor] child exited cleanly but restart=permanent — relaunching.\n");
            supervisor_log("event=child_exit child_pid=%d class=%s restart=permanent peak_rss_mb=%llu",
                           (int)child, crash_class_name(cls),
                           (unsigned long long)(peak_rss >> 20));
        }

        /* temporary children are never restarted, abnormal exit or not. */
        if (restart_type == RT_TEMPORARY) {
            int sig0 = WIFSIGNALED(status) ? WTERMSIG(status) : 0;
            fprintf(stderr, "\n[supervisor] child died (%s) and restart=temporary — not restarting.\n",
                    crash_class_name(cls));
            supervisor_log("event=child_exit child_pid=%d class=%s signal=%s exit_code=%d restart=temporary peak_rss_mb=%llu",
                           (int)child, crash_class_name(cls), signal_name(sig0),
                           WIFEXITED(status) ? WEXITSTATUS(status) : -1,
                           (unsigned long long)(peak_rss >> 20));
            incident_report_t report = {
                .child_pid = child,
                .cls = cls,
                .sig = sig0,
                .exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1,
                .uptime = now_monotonic() - last_start_time,
                .peak_rss = peak_rss,
                .mem_hard = mem_hard_bytes,
                .mem_soft = mem_soft_bytes,
                .pressure = system_mem_pressure(),
                .restart_count = restart_count,
                .next_delay_ms = 0,
                .poll_ms = poll_ms,
                .preempted = preempted,
                .tracer_reaped = tracer_reaped,
                .action = "stop_temporary",
                .child_cmdline = child_cmdline,
                .metrics_path = metrics_path,
            };
            char incident_path[PATH_MAX];
            if (write_incident_report(&report, incident_path, sizeof(incident_path)))
                fprintf(stderr, "[supervisor] incident report: %s\n", incident_path);
            capture_diagnostics(child, cls, sig0);
            return sig0 > 0 ? 128 + sig0 : (WIFEXITED(status) ? WEXITSTATUS(status) : 1);
        }

        /* Abnormal exit — classify and decide whether to restart. */
        int sig = WIFSIGNALED(status) ? WTERMSIG(status) : 0;
        double now = now_monotonic();
        double uptime = now - last_start_time;

        fprintf(stderr, "\n[supervisor] child pid=%d died: %s",
                (int)child, crash_class_name(cls));
        if (sig > 0) fprintf(stderr, " (%s/%d)", signal_name(sig), sig);
        fprintf(stderr, " after %.1fs\n", uptime);
        supervisor_log("event=child_exit child_pid=%d class=%s signal=%s signal_num=%d exit_code=%d uptime_s=%.1f peak_rss_mb=%llu preempted=%d tracer_reaped=%d pressure=%d",
                       (int)child, crash_class_name(cls), signal_name(sig), sig,
                       WIFEXITED(status) ? WEXITSTATUS(status) : -1,
                       uptime, (unsigned long long)(peak_rss >> 20),
                       preempted ? 1 : 0, tracer_reaped ? 1 : 0,
                       system_mem_pressure());

        incident_report_t report = {
            .child_pid = child,
            .cls = cls,
            .sig = sig,
            .exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1,
            .uptime = uptime,
            .peak_rss = peak_rss,
            .mem_hard = mem_hard_bytes,
            .mem_soft = mem_soft_bytes,
            .pressure = system_mem_pressure(),
            .restart_count = restart_count,
            .next_delay_ms = current_backoff,
            .poll_ms = poll_ms,
            .preempted = preempted,
            .tracer_reaped = tracer_reaped,
            .action = "restart_pending",
            .child_cmdline = child_cmdline,
            .metrics_path = metrics_path,
        };
        char incident_path[PATH_MAX];
        if (write_incident_report(&report, incident_path, sizeof(incident_path)))
            fprintf(stderr, "[supervisor] incident report: %s\n", incident_path);

        /* Capture diagnostics. */
        capture_diagnostics(child, cls, sig);

        /* ── Circuit breaker logic ─────────────────────────────────────── */
        /* If the child was stable for >= stable_s, reset the restart counter
         * and backoff — this was a "good run" that happened to crash. */
        if (uptime >= (double)stable_s) {
            if (restart_count > 0) {
                fprintf(stderr, "[supervisor] child was stable for %.0fs — resetting crash counter.\n",
                        uptime);
            }
            restart_count = 0;
            first_crash_time = 0;
            current_backoff = backoff_ms;
        }

        /* Track rapid restarts. */
        if (first_crash_time == 0) {
            first_crash_time = now;
        }
        restart_count++;

        /* Check if we're in a crash loop. */
        double window_elapsed = now - first_crash_time;
        if (restart_count > max_restarts && window_elapsed < (double)window_s) {
            fprintf(stderr, "[supervisor] ── CRASH LOOP ──\n");
            fprintf(stderr, "[supervisor] %d restarts in %.0fs (limit: %d in %ds).\n",
                    restart_count, window_elapsed, max_restarts, window_s);
            fprintf(stderr, "[supervisor] Circuit breaker tripped. Giving up.\n");
            fprintf(stderr, "[supervisor] Last exit: %s", crash_class_name(cls));
            if (sig > 0) fprintf(stderr, " (%s/%d)", signal_name(sig), sig);
            fprintf(stderr, ".\n");
            fprintf(stderr, "[supervisor] Check /tmp/dsco_crash.log for details.\n");
            supervisor_log("event=crash_loop restarts=%d window_s=%.0f limit=%d last_class=%s last_signal=%s",
                           restart_count, window_elapsed, max_restarts,
                           crash_class_name(cls), signal_name(sig));
            report.action = "give_up_crash_loop";
            report.restart_count = restart_count;
            report.next_delay_ms = 0;
            if (write_incident_report(&report, incident_path, sizeof(incident_path)))
                fprintf(stderr, "[supervisor] crash-loop incident report: %s\n", incident_path);
            return sig > 0 ? 128 + sig : 1;
        }

        /* If we exceeded the window but still have restarts, slide the window. */
        if (window_elapsed >= (double)window_s) {
            restart_count = 1;
            first_crash_time = now;
            current_backoff = backoff_ms;
        }

        /* Exponential backoff. */
        fprintf(stderr, "[supervisor] restarting in %dms (attempt %d)...\n",
                current_backoff, restart_count + 1);
        supervisor_log("event=restart delay_ms=%d next_attempt=%d", current_backoff, restart_count + 1);
        sleep_ms(current_backoff);
        current_backoff *= 2;
        if (current_backoff > backoff_max_ms) current_backoff = backoff_max_ms;
    }
}
