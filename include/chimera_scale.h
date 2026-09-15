#ifndef DSCO_CHIMERA_SCALE_H
#define DSCO_CHIMERA_SCALE_H

#include <stdbool.h>
#include <stdint.h>

/* A lazy execution plan for a logical Chimera population.
 * No per-agent or per-shard storage is allocated; ranges are derived on demand.
 */
typedef struct {
    uint64_t logical_agents;
    uint64_t hosts;
    uint64_t slots_per_host;
    uint64_t fanout;
    uint64_t active_slots;
    uint64_t waves;
    uint64_t leaf_shards;
    uint64_t hierarchy_depth;
    uint64_t hierarchy_reducers;
    uint64_t hierarchy_messages;
    uint64_t all_to_all_messages;
} chimera_scale_plan_t;

/* Return false for an invalid configuration (agents must be 1..1,000,000 and
 * hosts, slots_per_host, and fanout must be non-zero, with fanout >= 2). */
bool chimera_scale_plan(uint64_t logical_agents, uint64_t hosts,
                        uint64_t slots_per_host, uint64_t fanout,
                        chimera_scale_plan_t *out);

/* Resolve shard to its exact contiguous half-open logical-agent range. */
bool chimera_scale_shard_range(const chimera_scale_plan_t *plan,
                               uint64_t shard, uint64_t *begin,
                               uint64_t *end);

#endif
