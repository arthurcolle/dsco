#ifndef DSCO_GRAPHSUB_CLIENT_H
#define DSCO_GRAPHSUB_CLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ── GraphSub Client ─────────────────────────────────────────────────────
 * HTTP client for the GraphSub substrate. Connects dsco-cli (edge agent)
 * to GraphSub (graph state store) for:
 *   - Agent registration + heartbeat
 *   - Pheromone deposit/query/sweep
 *   - Graph traversal
 *   - Memory sync (episodic → semantic)
 *   - Swarm topology registration
 *   - Tool result logging
 *   - Bridge fleet registration
 *
 * Configuration:
 *   GRAPHSUB_HOST env var (default: http://localhost:7879)
 *   GRAPHSUB_TENANT_ID env var (default: local)
 *   GRAPHSUB_API_KEY env var (optional)
 *
 * All functions return malloc'd JSON strings (caller must free).
 * NULL return = transport error. ──────────────────────────────────────── */

/* Resolve GraphSub host from env or return default. */
const char *graphsub_host(void);
uint16_t   graphsub_port(void);
const char *graphsub_tenant_id(void);
bool       graphsub_is_available(void);

/* ── Agent Lifecycle ────────────────────────────────────────────────────── */

/* Register this agent on GraphSub. Returns response JSON (caller frees). */
char *graphsub_agent_register(const char *agent_id,
                               const char *model,
                               const char *capabilities_json, /* JSON array string */
                               const char *tools_json,        /* JSON array string */
                               const char *hostname,
                               const char *version,
                               const char *bridge_peer);      /* may be NULL */

/* Send heartbeat. Returns response JSON (caller frees). */
char *graphsub_agent_heartbeat(const char *agent_id,
                                const char *status,
                                double load,
                                int memory_used_mb,
                                int active_agents,
                                int uptime_seconds);

/* Deregister agent. Returns response JSON (caller frees). */
char *graphsub_agent_deregister(const char *agent_id);

/* ── Pheromones ─────────────────────────────────────────────────────────── */

/* Deposit a pheromone signal. Returns response JSON (caller frees). */
char *graphsub_pheromone_deposit(const char *agent_id,
                                  const char *target_type,   /* task|agent|path|resource */
                                  const char *target_id,
                                  const char *signal_type,    /* progress|attraction|warning|success|help_needed|capacity */
                                  double intensity,           /* 0.0 - 1.0 */
                                  int ttl_seconds,
                                  const char *metadata_json); /* may be NULL */

/* Query pheromones for a target. Returns response JSON (caller frees). */
char *graphsub_pheromone_query(const char *target_id,
                                const char *signal_type,    /* may be NULL */
                                double min_intensity);      /* 0.0 = all */

/* Sweep (batch decay + cleanup). Returns response JSON (caller frees). */
char *graphsub_pheromone_sweep(const char *target_type,    /* may be NULL = all */
                                const char *signal_type);   /* may be NULL = all */

/* ── Graph Operations ───────────────────────────────────────────────────── */

/* Traverse the graph from a start node. Returns response JSON (caller frees). */
char *graphsub_graph_traverse(const char *start_node,
                              int depth,
                              const char *edge_filter_json, /* JSON array, may be NULL */
                              int limit);

/* ── Memory ─────────────────────────────────────────────────────────────── */

/* Sync episodic memory to GraphSub. Returns response JSON (caller frees). */
char *graphsub_memory_sync(const char *agent_id,
                            const char *episodes_json,    /* JSON array of episodes */
                            const char *type);            /* "episodic" or "semantic" */

/* Trigger memory consolidation. Returns response JSON (caller frees). */
char *graphsub_memory_consolidate(const char *agent_id,
                                   const char *from_ts,      /* ISO 8601 */
                                   const char *to_ts);      /* ISO 8601 */

/* ── Swarm ───────────────────────────────────────────────────────────────── */

/* Register a swarm topology. Returns response JSON (caller frees). */
char *graphsub_swarm_register(const char *swarm_id,
                              const char *topology,         /* fanout|fanin|debate|pipeline|tournament|stigmergic */
                              const char *agents_json,     /* JSON array of {agent_id, role, host} */
                              const char *task_desc);

/* ── Tool Results ────────────────────────────────────────────────────────── */

/* Log a tool execution result to GraphSub. Returns response JSON (caller frees). */
char *graphsub_tool_result(const char *agent_id,
                            const char *tool_name,
                            const char *tool_params_json,
                            const char *result_json,
                            int duration_ms,
                            bool success,
                            const char *executed_on,
                            const char *swarm_id);          /* may be NULL */

/* ── Bridge Fleet ────────────────────────────────────────────────────────── */

/* Register bridge fleet peers on GraphSub. Returns response JSON (caller frees). */
char *graphsub_bridge_fleet(const char *peers_json);   /* JSON array of {name, host, addr, status, agent_id} */

/* ── Low-level ───────────────────────────────────────────────────────────── */

/* Low-level POST to GraphSub. Returns malloc'd response body or NULL. */
char *graphsub_post(const char *path, const char *json_body);

/* Low-level GET from GraphSub. Returns malloc'd response body or NULL. */
char *graphsub_get(const char *path);

#endif /* DSCO_GRAPHSUB_CLIENT_H */
