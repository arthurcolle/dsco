#include "chimera_scale.h"

#include <limits.h>
#include <stddef.h>

bool chimera_scale_plan(uint64_t logical_agents, uint64_t hosts,
                        uint64_t slots_per_host, uint64_t fanout,
                        chimera_scale_plan_t *out) {
    if (!out || logical_agents == 0 || logical_agents > 1000000ULL ||
        hosts == 0 || slots_per_host == 0 || fanout < 2 ||
        hosts > UINT64_MAX / slots_per_host)
        return false;

    const uint64_t capacity = hosts * slots_per_host;
    const uint64_t active = logical_agents < capacity ? logical_agents : capacity;
    if (active == 0) return false;

    out->logical_agents = logical_agents;
    out->hosts = hosts;
    out->slots_per_host = slots_per_host;
    out->fanout = fanout;
    out->active_slots = active;
    out->waves = (logical_agents + active - 1) / active;
    out->leaf_shards = active;
    out->hierarchy_depth = 0;
    out->hierarchy_reducers = 0;
    out->hierarchy_messages = 0;

    uint64_t frontier = active;
    while (frontier > 1) {
        const uint64_t reducers = frontier / fanout + (frontier % fanout != 0);
        out->hierarchy_messages += frontier;
        out->hierarchy_reducers += reducers;
        out->hierarchy_depth++;
        frontier = reducers;
    }
    out->all_to_all_messages = logical_agents * (logical_agents - 1);
    return true;
}

bool chimera_scale_shard_range(const chimera_scale_plan_t *plan,
                               uint64_t shard, uint64_t *begin,
                               uint64_t *end) {
    if (!plan || !begin || !end || plan->logical_agents == 0 ||
        plan->leaf_shards == 0 || shard >= plan->leaf_shards)
        return false;
    *begin = (shard * plan->logical_agents) / plan->leaf_shards;
    *end = ((shard + 1) * plan->logical_agents) / plan->leaf_shards;
    return true;
}
