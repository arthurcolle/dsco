#ifndef DSCO_BUFFER_CLI_H
#define DSCO_BUFFER_CLI_H
#include <stdbool.h>
#include <stddef.h>

/* Full main argv: dsco buffer ACTION [JSON_OBJECT]. Emits one JSON line.
 * Returns 0 on success/help, 1 on tool/output failure, 2 on invalid arguments. */
int buffer_cli(int argc, char **argv);

/* Parse a /buffer command tail and execute at the caller's existing tier.
 * Does not initialize tools, change configuration, reset state, or print. */
bool buffer_command_execute(const char *arguments, const char *tier,
                            char *result, size_t cap);
#endif
