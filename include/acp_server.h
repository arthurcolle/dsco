#ifndef DSCO_ACP_SERVER_H
#define DSCO_ACP_SERVER_H

/* dsco as an Agent Client Protocol (ACP) agent over stdio JSON-RPC.
 *
 * This adapter is intentionally separate from the interactive agent loop. It
 * is launched by an ACP harness such as Buzz, maintains router state for that
 * adapter process, and delegates each completed ACP prompt to a governed
 * headless dsco child process.
 */

int acp_server_run(const char *dsco_argv0);

#endif /* DSCO_ACP_SERVER_H */
