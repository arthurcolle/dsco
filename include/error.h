#ifndef DSCO_ERROR_H
#define DSCO_ERROR_H

#include <stdbool.h>
#include <stddef.h>

/* ── Structured error codes ────────────────────────────────────────────── */

typedef enum {
    DSCO_ERR_OK       = 0,
    DSCO_ERR_PARSE    = 1,
    DSCO_ERR_NET      = 2,
    DSCO_ERR_TOOL     = 3,
    DSCO_ERR_OOM      = 4,
    DSCO_ERR_IO       = 5,
    DSCO_ERR_TIMEOUT  = 6,
    DSCO_ERR_MCP      = 7,
    DSCO_ERR_PROVIDER = 8,
    DSCO_ERR_BUDGET   = 9,
    DSCO_ERR_INJECTION = 10,
    DSCO_ERR_INTERNAL = 11,
} dsco_err_code_t;

/* ── Structured error with optional cause chain ────────────────────────── */

typedef struct dsco_error {
    dsco_err_code_t    code;
    const char        *file;
    int                line;
    char               msg[256];
    struct dsco_error *cause;    /* owned pointer, freed on clear */
} dsco_error_t;

/* Thread-local error state — safe for parallel tool workers (C11) */
const dsco_error_t *dsco_err_last(void);
dsco_err_code_t     dsco_err_code(void);
const char         *dsco_err_msg(void);
void                dsco_err_clear(void);

/* Set error with printf-style message */
void dsco_err_set(dsco_err_code_t code, const char *file, int line,
                  const char *fmt, ...);

/* Push current error as cause, set new error on top */
void dsco_err_wrap(dsco_err_code_t code, const char *file, int line,
                   const char *fmt, ...);

/* Human-readable code name */
const char *dsco_err_code_str(dsco_err_code_t code);

/* ── Convenience macros ────────────────────────────────────────────────── */

#define DSCO_SET_ERR(code, ...) \
    dsco_err_set((code), __FILE__, __LINE__, __VA_ARGS__)

#define DSCO_WRAP_ERR(code, ...) \
    dsco_err_wrap((code), __FILE__, __LINE__, __VA_ARGS__)

/* Evaluate expr; if false, set error and return false */
#define DSCO_TRY(expr) do { \
    if (!(expr)) { \
        DSCO_SET_ERR(DSCO_ERR_INTERNAL, "%s", #expr); \
        return false; \
    } \
} while(0)

/* Evaluate expr; if false, set specific error and return false */
#define DSCO_TRY_MSG(expr, code, ...) do { \
    if (!(expr)) { \
        DSCO_SET_ERR(code, __VA_ARGS__); \
        return false; \
    } \
} while(0)

/* ── Runtime invariant guards (Skeletal/Immune: defensive contracts) ─────
 *
 * Unlike <assert.h>, these are PRODUCTION-SAFE: they never abort(). A failed
 * invariant records a structured error and returns a caller-chosen value, so a
 * violated precondition degrades gracefully instead of crashing the runtime.
 * This is the INTEGRITY doctrine in C: state the contract, fail closed.
 *
 *   DSCO_REQUIRE(expr)            — precondition; on fail, return false
 *   DSCO_REQUIRE_RET(expr, ret)   — precondition; on fail, return `ret`
 *   DSCO_REQUIRE_VOID(expr)       — precondition in void fn; on fail, return
 *   DSCO_CHECK(expr)              — soft invariant; on fail, log + continue
 *   DSCO_ASSERT(expr)            — alias of DSCO_REQUIRE (audit-discoverable)
 *
 * All record file/line via the thread-local error state, so a violated
 * contract is observable through dsco_err_last() and the audit trail.
 */
#define DSCO_REQUIRE(expr) do { \
    if (!(expr)) { \
        DSCO_SET_ERR(DSCO_ERR_INTERNAL, "invariant violated: %s", #expr); \
        return false; \
    } \
} while(0)

#define DSCO_REQUIRE_RET(expr, ret) do { \
    if (!(expr)) { \
        DSCO_SET_ERR(DSCO_ERR_INTERNAL, "invariant violated: %s", #expr); \
        return (ret); \
    } \
} while(0)

#define DSCO_REQUIRE_VOID(expr) do { \
    if (!(expr)) { \
        DSCO_SET_ERR(DSCO_ERR_INTERNAL, "invariant violated: %s", #expr); \
        return; \
    } \
} while(0)

#define DSCO_CHECK(expr) do { \
    if (!(expr)) { \
        DSCO_SET_ERR(DSCO_ERR_INTERNAL, "soft invariant: %s", #expr); \
    } \
} while(0)

/* Audit-discoverable alias; same fail-closed semantics as DSCO_REQUIRE. */
#define DSCO_ASSERT(expr) DSCO_REQUIRE(expr)

#endif
