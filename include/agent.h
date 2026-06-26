#ifndef DSCO_AGENT_H
#define DSCO_AGENT_H

#include <stdbool.h>

/* Capture the parsed launch argv so in-process /restart can exec the same
 * invocation without dropping flags such as -m, --profile, or --provider. */
void agent_set_launch_argv(int argc, char **argv);

/* Run the agent loop: read user input, send to Claude with tools,
 * execute tools in a loop until end_turn, print response.
 * provider_override: if non-NULL, forces this provider instead of auto-detect. */
bool agent_run(const char *api_key, const char *model,
               const char *topology_name, bool topology_auto,
               const char *provider_override);

/* Orchestrator mode: Haiku routes to domain-filtered workers + topologies.
 * See include/orchestrator.h for full documentation. */
bool agent_run_orchestrated(const char *api_key,
                             const char *chat_model,
                             const char *worker_model,
                             const char *provider_override);

#endif
