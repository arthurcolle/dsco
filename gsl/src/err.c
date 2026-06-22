/* err.c — minimal error handling */
#include "gsl/gsl_errno.h"
#include <stdio.h>
#include <stdlib.h>

static gsl_error_handler_t *handler = NULL;

void gsl_error(const char *reason, const char *file, int line, int gsl_errno) {
    if (handler) {
        (*handler)(reason, file, line, gsl_errno);
    } else {
        fprintf(stderr, "GSL ERROR: %s:%d: %s (errno=%d)\n", file, line, reason, gsl_errno);
        abort();
    }
}

void gsl_stream_printf(const char *label, const char *file, int line, const char *reason) {
    fprintf(stderr, "%s:%d: %s: %s\n", file, line, label, reason);
}

gsl_error_handler_t *gsl_set_error_handler(gsl_error_handler_t *new_handler) {
    gsl_error_handler_t *old = handler;
    handler = new_handler;
    return old;
}

gsl_error_handler_t *gsl_set_error_handler_off(void) {
    return gsl_set_error_handler(NULL);
}