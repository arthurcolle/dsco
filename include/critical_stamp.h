#ifndef DSCO_CRITICAL_STAMP_H
#define DSCO_CRITICAL_STAMP_H

#include <stdio.h>
#include <stddef.h>

/* Terminal-visible stop marker. Only call after recovery is exhausted and the
 * current task will not complete. Never pass raw secrets or provider bodies. */
void critical_stamp_format(char *out, size_t cap, const char *reason,
                           const char *impact, const char *next_step);
void critical_stamp_print(FILE *out, const char *reason, const char *impact,
                          const char *next_step);

#endif
