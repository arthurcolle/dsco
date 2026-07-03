#ifndef DSCO_CLUSTER_H
#define DSCO_CLUSTER_H

/* ── Distributed-inference cluster primitives (strict superset of exo core) ──
 * First-class, reimplemented-from-algorithm (no upstream code):
 *   - cluster inventory: realtime topology + per-node resources (cores, memory,
 *     accelerator) across the local node + ~/bridge/fleet peers, probed live.
 *   - memory-weighted ring partitioning: split a model's layers across the
 *     heterogeneous nodes proportional to available memory, in pipeline/ring
 *     order (exo's "ring memory-weighted partitioning").
 *   - placement preview: per-node layer range + memory delta + feasibility,
 *     superset of exo's /instance/previews memory_delta_by_node.
 *
 * CLI:
 *   dsco cluster                      inventory table
 *   dsco cluster plan [--model N | --bytes GB --layers L] [--quant 4|8|16]
 */
int dsco_cluster_cli(int argc, char **argv);

/* Resolve a peers CSV (names from ~/bridge/fleet, or literal host:port) to a
 * comma-separated "host:port,host:port" RPC endpoint list. When ensure!=0,
 * launch ggml-rpc-server on each peer if not already running. Returns the
 * endpoint count. Used by `dsco serve` so /v1/chat can front the distributed
 * split instead of running local-only. */
#include <stddef.h>
int dsco_cluster_rpc_endpoints(const char *peers_csv, char *out, size_t outlen, int ensure);

#endif /* DSCO_CLUSTER_H */
