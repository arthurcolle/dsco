#ifndef DSCO_REMOTE_CLI_H
#define DSCO_REMOTE_CLI_H

/* Direct fleet control over plain SSH — bypasses the ~/bridge spool, the
 * launchd daemon, the signed-exec files, and the Python bus entirely.
 *
 *   dsco remote <peer> <cmd...>   run a command on a fleet peer, output passthrough
 *   dsco remote <peer>            open an interactive shell on the peer
 *   dsco fleet                    list fleet peers with live reachability
 *
 * Peers are read from ~/bridge/fleet/<peer>.host (NAME/USER/ADDR/...). That
 * flat host registry is the one good, simple part of the bridge; everything
 * else is skipped. */
int dsco_remote_cli(int argc, char **argv);
int dsco_fleet_cli(int argc, char **argv);

/* Resolve a fleet peer name → user/addr from ~/bridge/fleet/<peer>.host.
 * Returns true if the host file exists (addr may be empty if malformed).
 * Shared with the compute fabric's "fleet" backend. */
#include <stddef.h>
#include <stdbool.h>
bool dsco_fleet_resolve(const char *peer, char *user, size_t ul, char *addr, size_t al);

#endif /* DSCO_REMOTE_CLI_H */
