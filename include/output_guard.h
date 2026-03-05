#ifndef DSCO_OUTPUT_GUARD_H
#define DSCO_OUTPUT_GUARD_H

#include <stdbool.h>

/* Enable process-wide output flood guard for stdout/stderr.
 * Returns true when guard is active, false when disabled or unavailable. */
bool output_guard_init(void);

/* Reset the output guard after a trip (e.g. between conversation turns).
 * Clears the tripped state and repeat counters so output flows again. */
void output_guard_reset(void);

#endif
