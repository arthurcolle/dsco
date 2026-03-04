#include "trace.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <unistd.h>
#include <sys/time.h>
#include <pthread.h>
#include <errno.h>
#include <sys/stat.h>

/* ── State ────────────────────────────────────────────────────────────── */

static FILE          *s_trace_fp     = NULL;
static bool           s_trace_stderr = false;
static trace_level_t  s_min_level    = TRACE_LVL_OFF;
static bool           s_initialized  = false;
static pthread_mutex_t s_trace_mu    = PTHREAD_MUTEX_INITIALIZER;

static const char *level_str(trace_level_t lvl) {
    switch (lvl) {
    case TRACE_LVL_DEBUG: return "debug";
    case TRACE_LVL_INFO:  return "info";
    case TRACE_LVL_WARN:  return "warn";
    case TRACE_LVL_ERROR: return "error";
    default:              return "?";
    }
}

/* JSON-escape a string into buf. Returns bytes written (excluding NUL). */
static size_t json_escape(char *buf, size_t cap, const char *s) {
    size_t w = 0;
    if (!s) { if (cap > 0) buf[0] = '\0'; return 0; }
    for (; *s && w + 6 < cap; s++) {
        switch (*s) {
        case '"':  buf[w++] = '\\'; buf[w++] = '"';  break;
        case '\\': buf[w++] = '\\'; buf[w++] = '\\'; break;
        case '\n': buf[w++] = '\\'; buf[w++] = 'n';  break;
        case '\r': buf[w++] = '\\'; buf[w++] = 'r';  break;
        case '\t': buf[w++] = '\\'; buf[w++] = 't';  break;
        default:
            if ((unsigned char)*s < 0x20) {
                w += (size_t)snprintf(buf + w, cap - w, "\\u%04x", (unsigned char)*s);
            } else {
                buf[w++] = *s;
            }
        }
    }
    if (w < cap) buf[w] = '\0';
    return w;
}

/* Get microsecond timestamp string: "2026-03-04T12:34:56.789012Z" */
static void timestamp(char *buf, size_t cap) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm tm;
    gmtime_r(&tv.tv_sec, &tm);
    int n = (int)strftime(buf, cap, "%Y-%m-%dT%H:%M:%S", &tm);
    if (n > 0 && (size_t)n + 9 < cap)
        snprintf(buf + n, cap - (size_t)n, ".%06dZ", (int)tv.tv_usec);
}

/* Strip path to just filename */
static const char *basename_only(const char *path) {
    const char *p = strrchr(path, '/');
    return p ? p + 1 : path;
}

/* ── Public API ───────────────────────────────────────────────────────── */

void trace_init(void) {
    pthread_mutex_lock(&s_trace_mu);
    if (s_initialized) { pthread_mutex_unlock(&s_trace_mu); return; }
    s_initialized = true;

    const char *env = getenv("DSCO_TRACE");
    if (!env || env[0] == '\0' || strcmp(env, "0") == 0) {
        s_min_level = TRACE_LVL_OFF;
        pthread_mutex_unlock(&s_trace_mu);
        return;
    }

    /* Determine min level */
    if (strcmp(env, "debug") == 0) {
        s_min_level = TRACE_LVL_DEBUG;
    } else {
        s_min_level = TRACE_LVL_INFO;  /* "1", "true", or a path */
    }

    /* Determine output file */
    const char *trace_stderr = getenv("DSCO_TRACE_STDERR");
    if (trace_stderr && (strcmp(trace_stderr, "1") == 0 || strcmp(trace_stderr, "true") == 0))
        s_trace_stderr = true;

    if (strcmp(env, "1") == 0 || strcmp(env, "true") == 0 ||
        strcmp(env, "debug") == 0 || strcmp(env, "info") == 0) {
        /* Default path: ~/.dsco/debug/trace-<pid>.jsonl */
        const char *home = getenv("HOME");
        if (!home) home = "/tmp";
        char dir[512], path[600];
        snprintf(dir, sizeof(dir), "%s/.dsco/debug", home);
        mkdir(dir, 0755);  /* ignore error if exists */
        snprintf(path, sizeof(path), "%s/trace-%d.jsonl", dir, (int)getpid());
        s_trace_fp = fopen(path, "a");
        if (!s_trace_fp) {
            fprintf(stderr, "dsco: trace: cannot open %s: %s\n", path, strerror(errno));
            s_min_level = TRACE_LVL_OFF;
        } else {
            fprintf(stderr, "\033[2mdsco: trace log → %s\033[0m\n", path);
        }
    } else {
        /* env is a file path */
        s_trace_fp = fopen(env, "a");
        if (!s_trace_fp) {
            fprintf(stderr, "dsco: trace: cannot open %s: %s\n", env, strerror(errno));
            s_min_level = TRACE_LVL_OFF;
        } else {
            fprintf(stderr, "\033[2mdsco: trace log → %s\033[0m\n", env);
        }
    }

    pthread_mutex_unlock(&s_trace_mu);
}

void trace_shutdown(void) {
    pthread_mutex_lock(&s_trace_mu);
    if (s_trace_fp) {
        fflush(s_trace_fp);
        fclose(s_trace_fp);
        s_trace_fp = NULL;
    }
    s_min_level = TRACE_LVL_OFF;
    s_initialized = false;
    pthread_mutex_unlock(&s_trace_mu);
}

bool trace_enabled(trace_level_t lvl) {
    return lvl >= s_min_level;
}

/* Write a single JSON line to the trace file (and optionally stderr). */
static void emit_line(const char *line) {
    pthread_mutex_lock(&s_trace_mu);
    if (s_trace_fp) {
        fputs(line, s_trace_fp);
        fputc('\n', s_trace_fp);
        fflush(s_trace_fp);
    }
    if (s_trace_stderr) {
        fputs(line, stderr);
        fputc('\n', stderr);
    }
    pthread_mutex_unlock(&s_trace_mu);
}

void trace_log(trace_level_t lvl, const char *func, const char *file,
               int line, const char *fmt, ...) {
    if (lvl < s_min_level) return;

    char ts[40];
    timestamp(ts, sizeof(ts));

    char msg[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    char esc_msg[4096];
    json_escape(esc_msg, sizeof(esc_msg), msg);

    char esc_func[256];
    json_escape(esc_func, sizeof(esc_func), func);

    char buf[8192];
    snprintf(buf, sizeof(buf),
        "{\"ts\":\"%s\",\"level\":\"%s\",\"pid\":%d,\"tid\":%lu,"
        "\"func\":\"%s\",\"file\":\"%s\",\"line\":%d,"
        "\"msg\":\"%s\"}",
        ts, level_str(lvl), (int)getpid(),
        (unsigned long)pthread_self(),
        esc_func, basename_only(file), line,
        esc_msg);

    emit_line(buf);
}

void trace_log_kv(trace_level_t lvl, const char *func, const char *file,
                  int line, const char *event, ...) {
    if (lvl < s_min_level) return;

    char ts[40];
    timestamp(ts, sizeof(ts));

    char esc_event[256];
    json_escape(esc_event, sizeof(esc_event), event);

    char esc_func[256];
    json_escape(esc_func, sizeof(esc_func), func);

    /* Build the JSON line with key-value pairs */
    char buf[8192];
    int pos = snprintf(buf, sizeof(buf),
        "{\"ts\":\"%s\",\"level\":\"%s\",\"pid\":%d,\"tid\":%lu,"
        "\"func\":\"%s\",\"file\":\"%s\",\"line\":%d,"
        "\"event\":\"%s\"",
        ts, level_str(lvl), (int)getpid(),
        (unsigned long)pthread_self(),
        esc_func, basename_only(file), line,
        esc_event);

    va_list ap;
    va_start(ap, event);
    for (;;) {
        const char *key = va_arg(ap, const char *);
        if (!key) break;
        const char *val = va_arg(ap, const char *);
        if (!val) val = "(null)";

        char esc_key[256], esc_val[2048];
        json_escape(esc_key, sizeof(esc_key), key);
        json_escape(esc_val, sizeof(esc_val), val);

        int wrote = snprintf(buf + pos, sizeof(buf) - (size_t)pos,
                             ",\"%s\":\"%s\"", esc_key, esc_val);
        if (wrote > 0 && (size_t)(pos + wrote) < sizeof(buf))
            pos += wrote;
        else
            break;
    }
    va_end(ap);

    if ((size_t)pos + 2 < sizeof(buf)) {
        buf[pos++] = '}';
        buf[pos] = '\0';
    }

    emit_line(buf);
}
