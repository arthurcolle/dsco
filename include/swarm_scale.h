#ifndef DSCO_SWARM_SCALE_H
#define DSCO_SWARM_SCALE_H
#include <stdbool.h>
#include <stddef.h>
/* Request-local exact-work sharing, opt-in for pure interchangeable tasks.
 * Expected linear hash planning for bounded descriptors; validation scans each
 * object's <=128 key pairs. No cross-run cache or total-cost promise.
 * Execution re-enters tools_execute_for_tier("swarm", action=create). */
bool swarm_scale_execute(const char *input, const char *tier, int physical_limit,
                         char *result, size_t result_len);
#endif
