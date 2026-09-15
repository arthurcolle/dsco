#ifndef DSCO_SURFACE_CLI_H
#define DSCO_SURFACE_CLI_H

/* Full main argv: dsco surface ACTION [JSON_OBJECT]. Emits one JSON line.
 * Returns 0 on success/help, 1 on tool/output failure, 2 on invalid arguments. */
int surface_cli(int argc, char **argv);

#endif
