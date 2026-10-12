#ifndef DSCO_SWARM_HIERARCHY_H
#define DSCO_SWARM_HIERARCHY_H
#include <stdbool.h>
#include <stddef.h>

/* Process-owned logical dependency trees. All worker operations re-enter the
 * normal tier gate; logical parentage does not grant worker authority. */
bool swarm_hierarchy_execute(const char *input, const char *tier, int physical_limit,
                             int depth_limit, char *result, size_t result_len);
#endif
