#ifndef DSCO_SWARM_TELEMETRY_H
#define DSCO_SWARM_TELEMETRY_H
#include "swarm.h"
/* Snapshot only; caller may poll the owned swarm before rendering. */
bool swarm_health_json(const swarm_t *s, const char *input, char *out, size_t len);
#endif
