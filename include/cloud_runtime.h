#ifndef DSCO_CLOUD_RUNTIME_H
#define DSCO_CLOUD_RUNTIME_H

#include <stdbool.h>
#include <stddef.h>

/* Fail-closed runtime boundary for multi-tenant/BYOK deployments.  It is
 * deliberately separate from the interactive performance profile: cloud mode
 * is a security posture, selected only at process start. */
bool dsco_cloud_runtime_requested(int argc, char **argv);
bool dsco_cloud_runtime_init(int argc, char **argv, char *err, size_t err_len);
bool dsco_cloud_runtime_active(void);
const char *dsco_cloud_lease_id(void);

/* Install build-time RuntimeSpec ceilings over mutable operator environment.
 * This is exposed for the focused runtime boundary test; cloud startup calls
 * it only after a verified RuntimeSpec-bound lease. */
bool dsco_cloud_runtime_apply_compiled_ceilings(char *err, size_t err_len);

/* Host allowlist used by ToolManagement and capability dispatch.  In cloud
 * mode the list is mandatory and an absent or malformed target is denied. */
bool dsco_cloud_destination_allowed(const char *url_or_host);

#endif
