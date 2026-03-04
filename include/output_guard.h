#ifndef DSCO_OUTPUT_GUARD_H
#define DSCO_OUTPUT_GUARD_H

#include <stdbool.h>

/* Enable process-wide output flood guard for stdout/stderr.
 * Returns true when guard is active, false when disabled or unavailable. */
bool output_guard_init(void);

#endif
