#ifndef DSCO_SURFACE_POLICY_H
#define DSCO_SURFACE_POLICY_H
#include <stdbool.h>
bool surface_policy_caps(const char *name, const char *json, unsigned *caps);
/* Internal scope, established only after the registry verifies exact ownership.
 * Never driven by a tool argument. Returns the previous thread-local state. */
bool surface_policy_owned_read_scope(bool enabled);
#endif
