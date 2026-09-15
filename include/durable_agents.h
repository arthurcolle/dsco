#ifndef DSCO_DURABLE_AGENTS_H
#define DSCO_DURABLE_AGENTS_H

#include <stdbool.h>
#include <stddef.h>

void durable_agents_default_db_path(char *out, size_t len);
/* Spawn a detached, cost-safe activation for an already-persisted targeted task. */
bool durable_agents_wake(const char *program, const char *agent_id,
                         int boot_task_id, const char *boot_task);
int durable_agents_cli(int argc, char **argv);

#endif
