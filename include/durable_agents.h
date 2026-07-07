#ifndef DSCO_DURABLE_AGENTS_H
#define DSCO_DURABLE_AGENTS_H

#include <stddef.h>

void durable_agents_default_db_path(char *out, size_t len);
int durable_agents_cli(int argc, char **argv);

#endif
