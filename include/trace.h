#ifndef DSCO_TRACE_H
#define DSCO_TRACE_H

#include <stdbool.h>
#include <stddef.h>

/*
 * Structured file-based trace logger for debugging execution paths.
 *
 * Writes JSON Lines to ~/.dsco/debug/trace-<pid>.jsonl
 * Each line is a self-contained JSON object with:
 *   ts, level, func, file, line, event, msg, [key-value data]
 *
 * Enable at runtime:
 *   DSCO_TRACE=1          enable tracing (levels: info, warn, error)
 *   DSCO_TRACE=debug      enable all levels including debug
 *   DSCO_TRACE=<path>     write to a specific file
 *   DSCO_TRACE_STDERR=1   also mirror trace lines to stderr
 *
 * Usage in code:
 *   TRACE_INIT();                         // call once at startup
 *   TRACE_DEBUG("parsing chunk len=%zu", n);
 *   TRACE_INFO("streaming started model=%s", model);
 *   TRACE_WARN("rate limited, retry in %ds", wait);
 *   TRACE_ERROR("curl failed: %s", errmsg);
 *   TRACE_ENTER();                        // log function entry
 *   TRACE_LEAVE();                        // log function exit
 *   TRACE_KV("event", "key1", "val1", "key2", "val2", NULL);
 *   TRACE_SHUTDOWN();                     // flush and close
 */

typedef enum {
    TRACE_LVL_DEBUG = 0,
    TRACE_LVL_INFO  = 1,
    TRACE_LVL_WARN  = 2,
    TRACE_LVL_ERROR = 3,
    TRACE_LVL_OFF   = 4,
} trace_level_t;

/* Initialize trace system. Safe to call multiple times (no-op after first).
 * Reads DSCO_TRACE and DSCO_TRACE_STDERR env vars. */
void trace_init(void);

/* Flush and close the trace log. */
void trace_shutdown(void);

/* Check if tracing is active at a given level. */
bool trace_enabled(trace_level_t lvl);

/* Core logging function — use the macros below instead. */
void trace_log(trace_level_t lvl, const char *func, const char *file,
               int line, const char *fmt, ...)
    __attribute__((format(printf, 5, 6)));

/* Log with arbitrary key-value pairs (NULL-terminated varargs of key, value).
 * Values are always strings. */
void trace_log_kv(trace_level_t lvl, const char *func, const char *file,
                  int line, const char *event, ...);

/* ── Convenience macros ──────────────────────────────────────────────── */

#define TRACE_INIT()     trace_init()
#define TRACE_SHUTDOWN() trace_shutdown()

#define TRACE_DEBUG(...) \
    do { if (trace_enabled(TRACE_LVL_DEBUG)) \
        trace_log(TRACE_LVL_DEBUG, __func__, __FILE__, __LINE__, __VA_ARGS__); \
    } while(0)

#define TRACE_INFO(...) \
    do { if (trace_enabled(TRACE_LVL_INFO)) \
        trace_log(TRACE_LVL_INFO, __func__, __FILE__, __LINE__, __VA_ARGS__); \
    } while(0)

#define TRACE_WARN(...) \
    do { if (trace_enabled(TRACE_LVL_WARN)) \
        trace_log(TRACE_LVL_WARN, __func__, __FILE__, __LINE__, __VA_ARGS__); \
    } while(0)

#define TRACE_ERROR(...) \
    do { if (trace_enabled(TRACE_LVL_ERROR)) \
        trace_log(TRACE_LVL_ERROR, __func__, __FILE__, __LINE__, __VA_ARGS__); \
    } while(0)

#define TRACE_ENTER() \
    TRACE_DEBUG("enter")

#define TRACE_LEAVE() \
    TRACE_DEBUG("leave")

/* Log structured key-value event. Args: event_name, key, val, ..., NULL */
#define TRACE_KV(event, ...) \
    do { if (trace_enabled(TRACE_LVL_INFO)) \
        trace_log_kv(TRACE_LVL_INFO, __func__, __FILE__, __LINE__, event, __VA_ARGS__); \
    } while(0)

#endif
