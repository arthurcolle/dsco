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

#endif
