#ifndef DSCO_SWARM_DAEMON_H
#define DSCO_SWARM_DAEMON_H

/* Durable detached swarm supervisor.
 *
 * Unlike the in-process swarm tool (whose worker table is owned by an active
 * chat/MCP process), this daemon owns a run directory and its worker process
 * groups. A caller can exit, time out, or restart without killing the swarm.
 * The daemon writes atomic state.json snapshots and append-only events.jsonl;
 * status/collect/abort are independent CLI invocations against that state.
 */

int swarm_daemon_cli(int argc, char **argv);

#endif /* DSCO_SWARM_DAEMON_H */
