#ifndef DSCO_AGENT_H
#define DSCO_AGENT_H

#include <stdbool.h>

/* Run the agent loop: read user input, send to Claude with tools,
 * execute tools in a loop until end_turn, print response. */
void agent_run(const char *api_key, const char *model,
               const char *topology_name, bool topology_auto);

#endif
