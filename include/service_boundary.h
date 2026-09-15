#ifndef DSCO_SERVICE_BOUNDARY_H
#define DSCO_SERVICE_BOUNDARY_H
#include <stdbool.h>
/* Enforce scoped network and cloud grants against the actual host-owned
 * destination. This supplements, and never replaces, governed tool dispatch. */
bool service_destination_allowed(const char *tool, const char *url);
bool service_action_destination_allowed(const char *tool, const char *action, const char *url);
#endif
