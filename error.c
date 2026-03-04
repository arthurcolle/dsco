#include "error.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

/* ── Thread-local error state (C11) ────────────────────────────────────── */

static _Thread_local dsco_error_t tl_error = {0};

const dsco_error_t *dsco_err_last(void) {
    return &tl_error;
}

dsco_err_code_t dsco_err_code(void) {
    return tl_error.code;
}

const char *dsco_err_msg(void) {
    return tl_error.msg;
}

void dsco_err_clear(void) {
    /* Free cause chain */
    dsco_error_t *c = tl_error.cause;
    while (c) {
        dsco_error_t *next = c->cause;
        free(c);
        c = next;
    }
    memset(&tl_error, 0, sizeof(tl_error));
}

void dsco_err_set(dsco_err_code_t code, const char *file, int line,
                  const char *fmt, ...) {
    dsco_err_clear();
    tl_error.code = code;
    tl_error.file = file;
    tl_error.line = line;

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tl_error.msg, sizeof(tl_error.msg), fmt, ap);
    va_end(ap);
}

void dsco_err_wrap(dsco_err_code_t code, const char *file, int line,
                   const char *fmt, ...) {
    /* Save current error as cause */
    dsco_error_t *prev = malloc(sizeof(dsco_error_t));
    if (prev) {
        *prev = tl_error;
        prev->cause = tl_error.cause;
    }

    tl_error.code = code;
    tl_error.file = file;
    tl_error.line = line;
    tl_error.cause = prev;

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tl_error.msg, sizeof(tl_error.msg), fmt, ap);
    va_end(ap);
}

const char *dsco_err_code_str(dsco_err_code_t code) {
    switch (code) {
        case DSCO_ERR_OK:        return "OK";
        case DSCO_ERR_PARSE:     return "PARSE";
        case DSCO_ERR_NET:       return "NET";
        case DSCO_ERR_TOOL:      return "TOOL";
        case DSCO_ERR_OOM:       return "OOM";
        case DSCO_ERR_IO:        return "IO";
        case DSCO_ERR_TIMEOUT:   return "TIMEOUT";
        case DSCO_ERR_MCP:       return "MCP";
        case DSCO_ERR_PROVIDER:  return "PROVIDER";
        case DSCO_ERR_BUDGET:    return "BUDGET";
        case DSCO_ERR_INJECTION: return "INJECTION";
        case DSCO_ERR_INTERNAL:  return "INTERNAL";
    }
    return "UNKNOWN";
}
