#ifndef DSCO_FLEET_BRIDGE_H
#define DSCO_FLEET_BRIDGE_H
#include <stdbool.h>
#include <stddef.h>
/* Structured fleet-mesh adapter. Caller must enter through the tool gate. */
bool fleet_bridge_execute(const char *input, char *result, size_t result_len);
#endif
