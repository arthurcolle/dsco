#include "topology.h"
#include "config.h"
#include "env_config.h"
#include "json_util.h"
#include "provider.h"
#include "provider_profiles.h"
#include "scheduler.h"
#include "swarm.h"
#include "tui.h"
#include "task_profile.h"
#include "cost_model.h"
#include "plan_cache.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define TOPO_STAGE_MARKER_BEGIN "<<DSCO_NODE_OUTPUT>>"
#define TOPO_STAGE_MARKER_END "<<END_DSCO_NODE_OUTPUT>>"

static topology_t g_topologies[TOPOLOGY_COUNT];
static bool g_topologies_inited = false;

/* ── Cooperative scheduler integration ──────────────────────────────── */
static scheduler_t *g_topo_sched = NULL;

void topology_set_scheduler(scheduler_t *sched) {
    g_topo_sched = sched;
}

static bool strcasestr_simple(const char *haystack, const char *needle);
static void build_dynamic_plan(const topology_task_profile_t *profile, const char *task,
                               topology_plan_t *plan);

static void topo_init_entry(topology_t *t, int id, const char *name, const char *description,
                            topo_category_t category, exec_strategy_t strategy, int max_iterations,
                            double latency_mult) {
    memset(t, 0, sizeof(*t));
    t->id = id;
    snprintf(t->name, sizeof(t->name), "%s", name);
    snprintf(t->description, sizeof(t->description), "%s", description);
    t->category = category;
    t->strategy = strategy;
    t->max_iterations = max_iterations > 0 ? max_iterations : 1;
    t->est_latency_mult = latency_mult;
}

static int topo_add_node(topology_t *t, model_tier_t tier, node_role_t role, const char *tag,
                         int replicas) {
    if (!t || t->node_count >= TOPO_MAX_NODES)
        return -1;
    topo_node_t *n = &t->nodes[t->node_count];
    memset(n, 0, sizeof(*n));
    n->id = t->node_count;
    n->tier = tier;
    n->role = role;
    n->replicas = replicas > 0 ? replicas : 1;
    snprintf(n->tag, sizeof(n->tag), "%s", tag);
    t->total_agents += n->replicas;
    return t->node_count++;
}

static void topo_add_edge(topology_t *t, int from, int to, edge_type_t type) {
    if (!t || t->edge_count >= TOPO_MAX_EDGES)
        return;
    t->edges[t->edge_count].from = from;
    t->edges[t->edge_count].to = to;
    t->edges[t->edge_count].type = type;
    t->edge_count++;
}

static double tier_unit_cost(model_tier_t tier) {
    switch (tier) {
        case TIER_HAIKU:
            return 0.0048;
        case TIER_SONNET:
            return 0.0180;
        case TIER_OPUS:
            return 0.0900;
    }
    return 0.0180;
}

static void topo_finalize(topology_t *t) {
    double unit = 0.0;
    for (int i = 0; i < t->node_count; i++) {
        unit += tier_unit_cost(t->nodes[i].tier) * t->nodes[i].replicas;
    }
    if (unit <= 0.0)
        unit = 0.0180;
    t->est_cost_1k = unit;
    if (t->total_agents <= 0)
        t->total_agents = t->node_count;
}

#define TI(idx, name, desc, cat, strat, iters, lat)                                                \
    topo_init_entry(&g_topologies[(idx) - 1], (idx), name, desc, cat, strat, iters, lat)
#define TN(idx, tier, role, tag, reps)                                                             \
    topo_add_node(&g_topologies[(idx) - 1], tier, role, tag, reps)
#define TE(idx, from, to, type) topo_add_edge(&g_topologies[(idx) - 1], from, to, type)
#define TF(idx) topo_finalize(&g_topologies[(idx) - 1])

static void init_topology_registry_linear(void) {
    TI(1, "sentinel", "Escalation chain: triage, analyze, decide", CAT_CHAIN, EXEC_LINEAR, 1, 3.0);
    TN(1, TIER_HAIKU, ROLE_CLASSIFIER, "triage", 1);
    TN(1, TIER_SONNET, ROLE_WORKER, "analyze", 1);
    TN(1, TIER_OPUS, ROLE_JUDGE, "decide", 1);
    TE(1, 0, 1, EDGE_SEQUENCE);
    TE(1, 1, 2, EDGE_SEQUENCE);
    TF(1);

    TI(2, "refinery", "Cheap preprocessing before a stronger final pass", CAT_CHAIN, EXEC_LINEAR, 1,
       3.0);
    TN(2, TIER_HAIKU, ROLE_SCOUT, "scan", 1);
    TN(2, TIER_HAIKU, ROLE_REDUCER, "refine", 1);
    TN(2, TIER_SONNET, ROLE_SYNTHESIZER, "summarize", 1);
    TE(2, 0, 1, EDGE_SEQUENCE);
    TE(2, 1, 2, EDGE_SEQUENCE);
    TF(2);

    TI(3, "deepdive", "Investigate, synthesize, and report", CAT_CHAIN, EXEC_LINEAR, 1, 3.0);
    TN(3, TIER_SONNET, ROLE_WORKER, "investigate", 1);
    TN(3, TIER_OPUS, ROLE_SYNTHESIZER, "synthesize", 1);
    TN(3, TIER_SONNET, ROLE_WORKER, "report", 1);
    TE(3, 0, 1, EDGE_SEQUENCE);
    TE(3, 1, 2, EDGE_SEQUENCE);
    TF(3);

    TI(4, "cascade", "Plan, execute, and package", CAT_CHAIN, EXEC_LINEAR, 1, 3.0);
    TN(4, TIER_OPUS, ROLE_COORDINATOR, "plan", 1);
    TN(4, TIER_SONNET, ROLE_WORKER, "execute", 1);
    TN(4, TIER_HAIKU, ROLE_VALIDATOR, "package", 1);
    TE(4, 0, 1, EDGE_SEQUENCE);
    TE(4, 1, 2, EDGE_SEQUENCE);
    TF(4);

    TI(5, "echo", "Multi-pass same-tier refinement", CAT_CHAIN, EXEC_LINEAR, 1, 3.0);
    TN(5, TIER_SONNET, ROLE_GENERATOR, "draft", 1);
    TN(5, TIER_SONNET, ROLE_CRITIC, "review", 1);
    TN(5, TIER_SONNET, ROLE_SYNTHESIZER, "finalize", 1);
    TE(5, 0, 1, EDGE_SEQUENCE);
    TE(5, 1, 2, EDGE_SEQUENCE);
    TF(5);

    TI(6, "distillery", "Haiku-heavy extraction chain into a Sonnet finish", CAT_CHAIN, EXEC_LINEAR,
       1, 4.0);
    TN(6, TIER_HAIKU, ROLE_SCOUT, "scan", 1);
    TN(6, TIER_HAIKU, ROLE_REDUCER, "filter", 1);
    TN(6, TIER_HAIKU, ROLE_VALIDATOR, "extract", 1);
    TN(6, TIER_SONNET, ROLE_SYNTHESIZER, "summarize", 1);
    TE(6, 0, 1, EDGE_SEQUENCE);
    TE(6, 1, 2, EDGE_SEQUENCE);
    TE(6, 2, 3, EDGE_SEQUENCE);
    TF(6);

    TI(7, "telescope", "Zoom out and back in across tiers", CAT_CHAIN, EXEC_LINEAR, 1, 5.0);
    TN(7, TIER_HAIKU, ROLE_CLASSIFIER, "scope", 1);
    TN(7, TIER_SONNET, ROLE_WORKER, "investigate", 1);
    TN(7, TIER_OPUS, ROLE_SYNTHESIZER, "synthesize", 1);
    TN(7, TIER_SONNET, ROLE_WORKER, "refine", 1);
    TN(7, TIER_HAIKU, ROLE_VALIDATOR, "deliver", 1);
    TE(7, 0, 1, EDGE_SEQUENCE);
    TE(7, 1, 2, EDGE_SEQUENCE);
    TE(7, 2, 3, EDGE_SEQUENCE);
    TE(7, 3, 4, EDGE_SEQUENCE);
    TF(7);

    TI(8, "gauntlet", "Extended chain with strategic middle and cheap verification", CAT_CHAIN,
       EXEC_LINEAR, 1, 6.0);
    TN(8, TIER_HAIKU, ROLE_CLASSIFIER, "triage", 1);
    TN(8, TIER_SONNET, ROLE_WORKER, "analyze", 1);
    TN(8, TIER_OPUS, ROLE_JUDGE, "decide", 1);
    TN(8, TIER_SONNET, ROLE_WORKER, "implement", 1);
    TN(8, TIER_HAIKU, ROLE_VALIDATOR, "verify", 1);
    TN(8, TIER_HAIKU, ROLE_REDUCER, "package", 1);
    TE(8, 0, 1, EDGE_SEQUENCE);
    TE(8, 1, 2, EDGE_SEQUENCE);
    TE(8, 2, 3, EDGE_SEQUENCE);
    TE(8, 3, 4, EDGE_SEQUENCE);
    TE(8, 4, 5, EDGE_SEQUENCE);
    TF(8);
}

static void init_topology_registry_fanout(void) {
    TI(9, "starburst", "Opus coordinator fans out to Sonnet workers and merges", CAT_FANOUT,
       EXEC_PARALLEL_STAGES, 1, 3.0);
    TN(9, TIER_OPUS, ROLE_COORDINATOR, "coord", 1);
    TN(9, TIER_SONNET, ROLE_WORKER, "worker1", 1);
    TN(9, TIER_SONNET, ROLE_WORKER, "worker2", 1);
    TN(9, TIER_SONNET, ROLE_WORKER, "worker3", 1);
    TN(9, TIER_SONNET, ROLE_WORKER, "worker4", 1);
    TN(9, TIER_OPUS, ROLE_SYNTHESIZER, "merge", 1);
    for (int i = 1; i <= 4; i++) {
        TE(9, 0, i, EDGE_FANOUT);
        TE(9, i, 5, EDGE_FANIN);
    }
    TF(9);

    TI(10, "scatter_gather", "Sonnet plans, Haiku gathers, Sonnet merges", CAT_FANOUT,
       EXEC_PARALLEL_STAGES, 1, 3.0);
    TN(10, TIER_SONNET, ROLE_DELEGATOR, "plan", 1);
    for (int i = 0; i < 6; i++)
        TN(10, TIER_HAIKU, ROLE_SCOUT,
           i == 0   ? "scout1"
           : i == 1 ? "scout2"
           : i == 2 ? "scout3"
           : i == 3 ? "scout4"
           : i == 4 ? "scout5"
                    : "scout6",
           1);
    TN(10, TIER_SONNET, ROLE_SYNTHESIZER, "merge", 1);
    for (int i = 1; i <= 6; i++) {
        TE(10, 0, i, EDGE_FANOUT);
        TE(10, i, 7, EDGE_FANIN);
    }
    TF(10);

    TI(11, "mapreduce", "Map shards in parallel then reduce", CAT_FANOUT, EXEC_PARALLEL_STAGES, 1,
       3.0);
    TN(11, TIER_SONNET, ROLE_DELEGATOR, "map", 1);
    for (int i = 0; i < 8; i++)
        TN(11, TIER_HAIKU, ROLE_WORKER,
           i == 0   ? "shard1"
           : i == 1 ? "shard2"
           : i == 2 ? "shard3"
           : i == 3 ? "shard4"
           : i == 4 ? "shard5"
           : i == 5 ? "shard6"
           : i == 6 ? "shard7"
                    : "shard8",
           1);
    TN(11, TIER_SONNET, ROLE_REDUCER, "reduce", 1);
    for (int i = 1; i <= 8; i++) {
        TE(11, 0, i, EDGE_FANOUT);
        TE(11, i, 9, EDGE_FANIN);
    }
    TF(11);

    TI(12, "trident", "Three Sonnet prongs under Opus control", CAT_FANOUT, EXEC_PARALLEL_STAGES, 1,
       3.0);
    TN(12, TIER_OPUS, ROLE_COORDINATOR, "coord", 1);
    TN(12, TIER_SONNET, ROLE_SPECIALIST, "prong1", 1);
    TN(12, TIER_SONNET, ROLE_SPECIALIST, "prong2", 1);
    TN(12, TIER_SONNET, ROLE_SPECIALIST, "prong3", 1);
    TN(12, TIER_OPUS, ROLE_SYNTHESIZER, "merge", 1);
    for (int i = 1; i <= 3; i++) {
        TE(12, 0, i, EDGE_FANOUT);
        TE(12, i, 4, EDGE_FANIN);
    }
    TF(12);

    TI(13, "constellation", "Five expert rays synthesized by Opus", CAT_FANOUT,
       EXEC_PARALLEL_STAGES, 1, 3.0);
    TN(13, TIER_OPUS, ROLE_COORDINATOR, "coord", 1);
    for (int i = 0; i < 5; i++)
        TN(13, TIER_SONNET, ROLE_SPECIALIST,
           i == 0   ? "expert1"
           : i == 1 ? "expert2"
           : i == 2 ? "expert3"
           : i == 3 ? "expert4"
                    : "expert5",
           1);
    TN(13, TIER_OPUS, ROLE_SYNTHESIZER, "merge", 1);
    for (int i = 1; i <= 5; i++) {
        TE(13, 0, i, EDGE_FANOUT);
        TE(13, i, 6, EDGE_FANIN);
    }
    TF(13);

    TI(14, "hydra", "Branched multistage fanout with Haiku executors", CAT_FANOUT,
       EXEC_PARALLEL_STAGES, 1, 4.0);
    TN(14, TIER_SONNET, ROLE_DELEGATOR, "plan", 1);
    TN(14, TIER_HAIKU, ROLE_WORKER, "branch_a1", 1);
    TN(14, TIER_HAIKU, ROLE_WORKER, "branch_b1", 1);
    TN(14, TIER_HAIKU, ROLE_WORKER, "branch_a2", 1);
    TN(14, TIER_HAIKU, ROLE_WORKER, "branch_b2", 1);
    TN(14, TIER_SONNET, ROLE_SYNTHESIZER, "merge", 1);
    TE(14, 0, 1, EDGE_FANOUT);
    TE(14, 0, 2, EDGE_FANOUT);
    TE(14, 1, 3, EDGE_SEQUENCE);
    TE(14, 2, 4, EDGE_SEQUENCE);
    TE(14, 3, 5, EDGE_FANIN);
    TE(14, 4, 5, EDGE_FANIN);
    TF(14);

    TI(15, "dandelion", "Classifier chooses one branch from a fanout", CAT_FANOUT,
       EXEC_PARALLEL_STAGES, 1, 3.0);
    TN(15, TIER_HAIKU, ROLE_CLASSIFIER, "route", 1);
    TN(15, TIER_SONNET, ROLE_SPECIALIST, "option1", 1);
    TN(15, TIER_SONNET, ROLE_SPECIALIST, "option2", 1);
    TN(15, TIER_SONNET, ROLE_SPECIALIST, "option3", 1);
    TN(15, TIER_SONNET, ROLE_SPECIALIST, "option4", 1);
    TN(15, TIER_OPUS, ROLE_JUDGE, "finalize", 1);
    for (int i = 1; i <= 4; i++) {
        TE(15, 0, i, EDGE_CONDITIONAL);
        TE(15, i, 5, EDGE_FANIN);
    }
    TF(15);

    TI(16, "nova", "Wide Haiku exploration with Sonnet synthesis and Opus finish", CAT_FANOUT,
       EXEC_PARALLEL_STAGES, 1, 4.0);
    TN(16, TIER_OPUS, ROLE_COORDINATOR, "coord", 1);
    for (int i = 0; i < 8; i++)
        TN(16, TIER_HAIKU, ROLE_SCOUT,
           i == 0   ? "probe1"
           : i == 1 ? "probe2"
           : i == 2 ? "probe3"
           : i == 3 ? "probe4"
           : i == 4 ? "probe5"
           : i == 5 ? "probe6"
           : i == 6 ? "probe7"
                    : "probe8",
           1);
    TN(16, TIER_SONNET, ROLE_SYNTHESIZER, "synthesize", 1);
    TN(16, TIER_OPUS, ROLE_JUDGE, "finalize", 1);
    for (int i = 1; i <= 8; i++) {
        TE(16, 0, i, EDGE_FANOUT);
        TE(16, i, 9, EDGE_FANIN);
    }
    TE(16, 9, 10, EDGE_SEQUENCE);
    TF(16);

    TI(17, "fireworks", "Haiku sparks cluster into Sonnet lanes then Opus merge", CAT_FANOUT,
       EXEC_PARALLEL_STAGES, 1, 4.0);
    TN(17, TIER_SONNET, ROLE_DELEGATOR, "plan", 1);
    for (int i = 0; i < 6; i++)
        TN(17, TIER_HAIKU, ROLE_SCOUT,
           i == 0   ? "spark1"
           : i == 1 ? "spark2"
           : i == 2 ? "spark3"
           : i == 3 ? "spark4"
           : i == 4 ? "spark5"
                    : "spark6",
           1);
    TN(17, TIER_SONNET, ROLE_SYNTHESIZER, "cluster1", 1);
    TN(17, TIER_SONNET, ROLE_SYNTHESIZER, "cluster2", 1);
    TN(17, TIER_SONNET, ROLE_SYNTHESIZER, "cluster3", 1);
    TN(17, TIER_OPUS, ROLE_JUDGE, "merge", 1);
    for (int i = 1; i <= 6; i++)
        TE(17, 0, i, EDGE_FANOUT);
    TE(17, 1, 7, EDGE_FANIN);
    TE(17, 2, 7, EDGE_FANIN);
    TE(17, 3, 8, EDGE_FANIN);
    TE(17, 4, 8, EDGE_FANIN);
    TE(17, 5, 9, EDGE_FANIN);
    TE(17, 6, 9, EDGE_FANIN);
    TE(17, 7, 10, EDGE_FANIN);
    TE(17, 8, 10, EDGE_FANIN);
    TE(17, 9, 10, EDGE_FANIN);
    TF(17);

    TI(18, "prism", "Seven Haiku facets into one Sonnet interpretation", CAT_FANOUT,
       EXEC_PARALLEL_STAGES, 1, 3.0);
    TN(18, TIER_SONNET, ROLE_DELEGATOR, "plan", 1);
    for (int i = 0; i < 7; i++)
        TN(18, TIER_HAIKU, ROLE_SCOUT,
           i == 0   ? "facet1"
           : i == 1 ? "facet2"
           : i == 2 ? "facet3"
           : i == 3 ? "facet4"
           : i == 4 ? "facet5"
           : i == 5 ? "facet6"
                    : "facet7",
           1);
    TN(18, TIER_SONNET, ROLE_SYNTHESIZER, "merge", 1);
    for (int i = 1; i <= 7; i++) {
        TE(18, 0, i, EDGE_FANOUT);
        TE(18, i, 8, EDGE_FANIN);
    }
    TF(18);
}

static void init_topology_registry_hierarchy(void) {
    TI(19, "military", "General to captains to soldiers", CAT_HIERARCHY, EXEC_PARALLEL_STAGES, 1,
       3.0);
    TN(19, TIER_OPUS, ROLE_COORDINATOR, "general", 1);
    TN(19, TIER_SONNET, ROLE_DELEGATOR, "captain1", 1);
    TN(19, TIER_SONNET, ROLE_DELEGATOR, "captain2", 1);
    for (int i = 0; i < 6; i++)
        TN(19, TIER_HAIKU, ROLE_WORKER,
           i == 0   ? "soldier1"
           : i == 1 ? "soldier2"
           : i == 2 ? "soldier3"
           : i == 3 ? "soldier4"
           : i == 4 ? "soldier5"
                    : "soldier6",
           1);
    TE(19, 0, 1, EDGE_SEQUENCE);
    TE(19, 0, 2, EDGE_SEQUENCE);
    TE(19, 1, 3, EDGE_FANOUT);
    TE(19, 1, 4, EDGE_FANOUT);
    TE(19, 1, 5, EDGE_FANOUT);
    TE(19, 2, 6, EDGE_FANOUT);
    TE(19, 2, 7, EDGE_FANOUT);
    TE(19, 2, 8, EDGE_FANOUT);
    TF(19);

    TI(20, "corporate", "Executive tree with managers and staff", CAT_HIERARCHY,
       EXEC_PARALLEL_STAGES, 1, 3.0);
    TN(20, TIER_OPUS, ROLE_COORDINATOR, "ceo", 1);
    for (int i = 0; i < 3; i++)
        TN(20, TIER_SONNET, ROLE_DELEGATOR, i == 0 ? "mgr1" : i == 1 ? "mgr2" : "mgr3", 1);
    for (int i = 0; i < 9; i++)
        TN(20, TIER_HAIKU, ROLE_WORKER,
           i == 0   ? "staff1"
           : i == 1 ? "staff2"
           : i == 2 ? "staff3"
           : i == 3 ? "staff4"
           : i == 4 ? "staff5"
           : i == 5 ? "staff6"
           : i == 6 ? "staff7"
           : i == 7 ? "staff8"
                    : "staff9",
           1);
    for (int i = 1; i <= 3; i++)
        TE(20, 0, i, EDGE_SEQUENCE);
    TE(20, 1, 4, EDGE_FANOUT);
    TE(20, 1, 5, EDGE_FANOUT);
    TE(20, 1, 6, EDGE_FANOUT);
    TE(20, 2, 7, EDGE_FANOUT);
    TE(20, 2, 8, EDGE_FANOUT);
    TE(20, 2, 9, EDGE_FANOUT);
    TE(20, 3, 10, EDGE_FANOUT);
    TE(20, 3, 11, EDGE_FANOUT);
    TE(20, 3, 12, EDGE_FANOUT);
    TF(20);

    TI(21, "binary_tree", "Balanced binary delegation tree", CAT_HIERARCHY, EXEC_PARALLEL_STAGES, 1,
       3.0);
    TN(21, TIER_OPUS, ROLE_COORDINATOR, "root", 1);
    TN(21, TIER_SONNET, ROLE_DELEGATOR, "left", 1);
    TN(21, TIER_SONNET, ROLE_DELEGATOR, "right", 1);
    TN(21, TIER_HAIKU, ROLE_WORKER, "leaf1", 1);
    TN(21, TIER_HAIKU, ROLE_WORKER, "leaf2", 1);
    TN(21, TIER_HAIKU, ROLE_WORKER, "leaf3", 1);
    TN(21, TIER_HAIKU, ROLE_WORKER, "leaf4", 1);
    TE(21, 0, 1, EDGE_SEQUENCE);
    TE(21, 0, 2, EDGE_SEQUENCE);
    TE(21, 1, 3, EDGE_FANOUT);
    TE(21, 1, 4, EDGE_FANOUT);
    TE(21, 2, 5, EDGE_FANOUT);
    TE(21, 2, 6, EDGE_FANOUT);
    TF(21);

    TI(22, "asymmetric", "Asymmetric tree with a deep left branch", CAT_HIERARCHY,
       EXEC_PARALLEL_STAGES, 1, 4.0);
    TN(22, TIER_OPUS, ROLE_COORDINATOR, "root", 1);
    TN(22, TIER_SONNET, ROLE_DELEGATOR, "left_mgr", 1);
    TN(22, TIER_SONNET, ROLE_DELEGATOR, "left_submgr", 1);
    TN(22, TIER_HAIKU, ROLE_WORKER, "left_leaf", 1);
    TN(22, TIER_HAIKU, ROLE_WORKER, "right_leaf1", 1);
    TN(22, TIER_HAIKU, ROLE_WORKER, "right_leaf2", 1);
    TN(22, TIER_HAIKU, ROLE_WORKER, "right_leaf3", 1);
    TE(22, 0, 1, EDGE_SEQUENCE);
    TE(22, 1, 2, EDGE_SEQUENCE);
    TE(22, 2, 3, EDGE_SEQUENCE);
    TE(22, 0, 4, EDGE_FANOUT);
    TE(22, 0, 5, EDGE_FANOUT);
    TE(22, 0, 6, EDGE_FANOUT);
    TF(22);

    TI(23, "fractal", "Recursive delegation with symmetric worker clusters", CAT_HIERARCHY,
       EXEC_PARALLEL_STAGES, 1, 3.0);
    TN(23, TIER_OPUS, ROLE_COORDINATOR, "root", 1);
    TN(23, TIER_SONNET, ROLE_DELEGATOR, "branch1", 1);
    TN(23, TIER_SONNET, ROLE_DELEGATOR, "branch2", 1);
    for (int i = 0; i < 6; i++)
        TN(23, TIER_HAIKU, ROLE_WORKER,
           i == 0   ? "leaf1"
           : i == 1 ? "leaf2"
           : i == 2 ? "leaf3"
           : i == 3 ? "leaf4"
           : i == 4 ? "leaf5"
                    : "leaf6",
           1);
    TE(23, 0, 1, EDGE_SEQUENCE);
    TE(23, 0, 2, EDGE_SEQUENCE);
    TE(23, 1, 3, EDGE_FANOUT);
    TE(23, 1, 4, EDGE_FANOUT);
    TE(23, 1, 5, EDGE_FANOUT);
    TE(23, 2, 6, EDGE_FANOUT);
    TE(23, 2, 7, EDGE_FANOUT);
    TE(23, 2, 8, EDGE_FANOUT);
    TF(23);

    TI(24, "canopy", "Layered hierarchy with scouts feeding each manager layer", CAT_HIERARCHY,
       EXEC_PARALLEL_STAGES, 1, 4.0);
    TN(24, TIER_OPUS, ROLE_COORDINATOR, "root", 1);
    TN(24, TIER_HAIKU, ROLE_SCOUT, "root_scout1", 1);
    TN(24, TIER_HAIKU, ROLE_SCOUT, "root_scout2", 1);
    TN(24, TIER_SONNET, ROLE_DELEGATOR, "mid1", 1);
    TN(24, TIER_HAIKU, ROLE_SCOUT, "mid1_scout1", 1);
    TN(24, TIER_HAIKU, ROLE_SCOUT, "mid1_scout2", 1);
    TN(24, TIER_SONNET, ROLE_DELEGATOR, "mid2", 1);
    TN(24, TIER_HAIKU, ROLE_SCOUT, "mid2_scout1", 1);
    TN(24, TIER_HAIKU, ROLE_SCOUT, "mid2_scout2", 1);
    TN(24, TIER_HAIKU, ROLE_WORKER, "leaf", 1);
    TE(24, 0, 1, EDGE_FANOUT);
    TE(24, 0, 2, EDGE_FANOUT);
    TE(24, 1, 3, EDGE_FANIN);
    TE(24, 2, 3, EDGE_FANIN);
    TE(24, 3, 4, EDGE_FANOUT);
    TE(24, 3, 5, EDGE_FANOUT);
    TE(24, 4, 6, EDGE_FANIN);
    TE(24, 5, 6, EDGE_FANIN);
    TE(24, 6, 7, EDGE_FANOUT);
    TE(24, 6, 8, EDGE_FANOUT);
    TE(24, 7, 9, EDGE_FANIN);
    TE(24, 8, 9, EDGE_FANIN);
    TF(24);

    TI(25, "pyramid", "Wide base hierarchy with a single apex", CAT_HIERARCHY, EXEC_PARALLEL_STAGES,
       1, 3.0);
    TN(25, TIER_OPUS, ROLE_COORDINATOR, "apex", 1);
    for (int i = 0; i < 3; i++)
        TN(25, TIER_SONNET, ROLE_DELEGATOR, i == 0 ? "mid1" : i == 1 ? "mid2" : "mid3", 1);
    for (int i = 0; i < 9; i++)
        TN(25, TIER_HAIKU, ROLE_WORKER,
           i == 0   ? "base1"
           : i == 1 ? "base2"
           : i == 2 ? "base3"
           : i == 3 ? "base4"
           : i == 4 ? "base5"
           : i == 5 ? "base6"
           : i == 6 ? "base7"
           : i == 7 ? "base8"
                    : "base9",
           1);
    for (int i = 1; i <= 3; i++)
        TE(25, 0, i, EDGE_SEQUENCE);
    TE(25, 1, 4, EDGE_FANOUT);
    TE(25, 1, 5, EDGE_FANOUT);
    TE(25, 1, 6, EDGE_FANOUT);
    TE(25, 2, 7, EDGE_FANOUT);
    TE(25, 2, 8, EDGE_FANOUT);
    TE(25, 2, 9, EDGE_FANOUT);
    TE(25, 3, 10, EDGE_FANOUT);
    TE(25, 3, 11, EDGE_FANOUT);
    TE(25, 3, 12, EDGE_FANOUT);
    TF(25);

    TI(26, "inverted_pyramid", "Wide scout base narrowing toward one decision", CAT_HIERARCHY,
       EXEC_PARALLEL_STAGES, 1, 4.0);
    for (int i = 0; i < 8; i++)
        TN(26, TIER_HAIKU, ROLE_SCOUT,
           i == 0   ? "base1"
           : i == 1 ? "base2"
           : i == 2 ? "base3"
           : i == 3 ? "base4"
           : i == 4 ? "base5"
           : i == 5 ? "base6"
           : i == 6 ? "base7"
                    : "base8",
           1);
    for (int i = 0; i < 4; i++)
        TN(26, TIER_SONNET, ROLE_REDUCER,
           i == 0   ? "mid1"
           : i == 1 ? "mid2"
           : i == 2 ? "mid3"
                    : "mid4",
           1);
    TN(26, TIER_SONNET, ROLE_SYNTHESIZER, "upper1", 1);
    TN(26, TIER_SONNET, ROLE_SYNTHESIZER, "upper2", 1);
    TN(26, TIER_OPUS, ROLE_JUDGE, "finalize", 1);
    TE(26, 0, 8, EDGE_FANIN);
    TE(26, 1, 8, EDGE_FANIN);
    TE(26, 2, 9, EDGE_FANIN);
    TE(26, 3, 9, EDGE_FANIN);
    TE(26, 4, 10, EDGE_FANIN);
    TE(26, 5, 10, EDGE_FANIN);
    TE(26, 6, 11, EDGE_FANIN);
    TE(26, 7, 11, EDGE_FANIN);
    TE(26, 8, 12, EDGE_FANIN);
    TE(26, 9, 12, EDGE_FANIN);
    TE(26, 10, 13, EDGE_FANIN);
    TE(26, 11, 13, EDGE_FANIN);
    TE(26, 12, 14, EDGE_FANIN);
    TE(26, 13, 14, EDGE_FANIN);
    TF(26);
}

static void init_topology_registry_mesh(void) {
    TI(27, "tribunal", "Three judges deliberate and Opus synthesizes", CAT_MESH, EXEC_CONSENSUS, 1,
       5.0);
    TN(27, TIER_SONNET, ROLE_SPECIALIST, "judge1", 1);
    TN(27, TIER_SONNET, ROLE_SPECIALIST, "judge2", 1);
    TN(27, TIER_SONNET, ROLE_SPECIALIST, "judge3", 1);
    TN(27, TIER_OPUS, ROLE_JUDGE, "verdict", 1);
    TE(27, 0, 3, EDGE_FANIN);
    TE(27, 1, 3, EDGE_FANIN);
    TE(27, 2, 3, EDGE_FANIN);
    TF(27);

    TI(28, "senate", "Parallel vote then Opus decision", CAT_MESH, EXEC_CONSENSUS, 1, 2.0);
    for (int i = 0; i < 5; i++)
        TN(28, TIER_SONNET, ROLE_SPECIALIST,
           i == 0   ? "senator1"
           : i == 1 ? "senator2"
           : i == 2 ? "senator3"
           : i == 3 ? "senator4"
                    : "senator5",
           1);
    TN(28, TIER_OPUS, ROLE_JUDGE, "decision", 1);
    for (int i = 0; i < 5; i++)
        TE(28, i, 5, EDGE_FANIN);
    TF(28);

    TI(29, "gossip", "Peer gossip distilled by a Sonnet bridge", CAT_MESH, EXEC_PARALLEL_STAGES, 1,
       4.0);
    for (int i = 0; i < 4; i++)
        TN(29, TIER_HAIKU, ROLE_SCOUT,
           i == 0   ? "peer1"
           : i == 1 ? "peer2"
           : i == 2 ? "peer3"
                    : "peer4",
           1);
    TN(29, TIER_SONNET, ROLE_SYNTHESIZER, "bridge", 1);
    for (int i = 0; i < 4; i++)
        TE(29, i, 4, EDGE_FANIN);
    TF(29);

    TI(30, "ring", "Sequential ring passing with final Opus synthesis", CAT_MESH, EXEC_LINEAR, 1,
       5.0);
    TN(30, TIER_SONNET, ROLE_WORKER, "ring1", 1);
    TN(30, TIER_SONNET, ROLE_WORKER, "ring2", 1);
    TN(30, TIER_SONNET, ROLE_WORKER, "ring3", 1);
    TN(30, TIER_SONNET, ROLE_WORKER, "ring4", 1);
    TN(30, TIER_OPUS, ROLE_SYNTHESIZER, "merge", 1);
    TE(30, 0, 1, EDGE_SEQUENCE);
    TE(30, 1, 2, EDGE_SEQUENCE);
    TE(30, 2, 3, EDGE_SEQUENCE);
    TE(30, 3, 4, EDGE_SEQUENCE);
    TF(30);

    TI(31, "full_mesh", "Multiple peer opinions merged centrally", CAT_MESH, EXEC_CONSENSUS, 1,
       5.0);
    TN(31, TIER_SONNET, ROLE_SPECIALIST, "peer1", 1);
    TN(31, TIER_SONNET, ROLE_SPECIALIST, "peer2", 1);
    TN(31, TIER_SONNET, ROLE_SPECIALIST, "peer3", 1);
    TN(31, TIER_OPUS, ROLE_SYNTHESIZER, "merge", 1);
    TE(31, 0, 3, EDGE_FANIN);
    TE(31, 1, 3, EDGE_FANIN);
    TE(31, 2, 3, EDGE_FANIN);
    TF(31);

    TI(32, "small_world", "Clustered local scouts with a bridge and final synthesis", CAT_MESH,
       EXEC_PARALLEL_STAGES, 1, 4.0);
    for (int i = 0; i < 6; i++)
        TN(32, TIER_HAIKU, ROLE_SCOUT,
           i == 0   ? "cluster1_a"
           : i == 1 ? "cluster1_b"
           : i == 2 ? "cluster1_c"
           : i == 3 ? "cluster2_a"
           : i == 4 ? "cluster2_b"
                    : "cluster2_c",
           1);
    TN(32, TIER_SONNET, ROLE_SYNTHESIZER, "bridge", 1);
    TN(32, TIER_OPUS, ROLE_JUDGE, "finalize", 1);
    for (int i = 0; i < 6; i++)
        TE(32, i, 6, EDGE_FANIN);
    TE(32, 6, 7, EDGE_SEQUENCE);
    TF(32);
}

static void init_topology_registry_specialist(void) {
    TI(33, "switchboard", "Haiku router chooses one or two specialists then Opus integrates",
       CAT_SPECIALIST, EXEC_PARALLEL_STAGES, 1, 3.0);
    TN(33, TIER_HAIKU, ROLE_CLASSIFIER, "router", 1);
    TN(33, TIER_SONNET, ROLE_SPECIALIST, "api", 1);
    TN(33, TIER_SONNET, ROLE_SPECIALIST, "db", 1);
    TN(33, TIER_SONNET, ROLE_SPECIALIST, "frontend", 1);
    TN(33, TIER_SONNET, ROLE_SPECIALIST, "infra", 1);
    TN(33, TIER_SONNET, ROLE_SPECIALIST, "security", 1);
    TN(33, TIER_SONNET, ROLE_SPECIALIST, "docs", 1);
    TN(33, TIER_OPUS, ROLE_SYNTHESIZER, "integrate", 1);
    for (int i = 1; i <= 6; i++) {
        TE(33, 0, i, EDGE_CONDITIONAL);
        TE(33, i, 7, EDGE_FANIN);
    }
    TF(33);

    TI(34, "triage", "Classifier routes work to one specialized downstream node", CAT_SPECIALIST,
       EXEC_PARALLEL_STAGES, 1, 2.0);
    TN(34, TIER_HAIKU, ROLE_CLASSIFIER, "classify", 1);
    TN(34, TIER_SONNET, ROLE_SPECIALIST, "code", 1);
    TN(34, TIER_SONNET, ROLE_SPECIALIST, "data", 1);
    TN(34, TIER_SONNET, ROLE_SPECIALIST, "writing", 1);
    TN(34, TIER_OPUS, ROLE_SPECIALIST, "strategy", 1);
    TE(34, 0, 1, EDGE_CONDITIONAL);
    TE(34, 0, 2, EDGE_CONDITIONAL);
    TE(34, 0, 3, EDGE_CONDITIONAL);
    TE(34, 0, 4, EDGE_CONDITIONAL);
    TF(34);

    TI(35, "expert_panel", "Five domain experts feed one Opus synthesis", CAT_SPECIALIST,
       EXEC_PARALLEL_STAGES, 1, 2.0);
    TN(35, TIER_SONNET, ROLE_SPECIALIST, "security", 1);
    TN(35, TIER_SONNET, ROLE_SPECIALIST, "perf", 1);
    TN(35, TIER_SONNET, ROLE_SPECIALIST, "ux", 1);
    TN(35, TIER_SONNET, ROLE_SPECIALIST, "arch", 1);
    TN(35, TIER_SONNET, ROLE_SPECIALIST, "ops", 1);
    TN(35, TIER_OPUS, ROLE_SYNTHESIZER, "synthesize", 1);
    for (int i = 0; i < 5; i++)
        TE(35, i, 5, EDGE_FANIN);
    TF(35);

    TI(36, "clinic", "Intake, diagnose, plan treatment, implement", CAT_SPECIALIST, EXEC_LINEAR, 1,
       4.0);
    TN(36, TIER_HAIKU, ROLE_CLASSIFIER, "intake", 1);
    TN(36, TIER_SONNET, ROLE_WORKER, "diagnose", 1);
    TN(36, TIER_OPUS, ROLE_COORDINATOR, "treatment", 1);
    TN(36, TIER_SONNET, ROLE_WORKER, "implement", 1);
    TE(36, 0, 1, EDGE_SEQUENCE);
    TE(36, 1, 2, EDGE_SEQUENCE);
    TE(36, 2, 3, EDGE_SEQUENCE);
    TF(36);

    TI(37, "assembly_line", "Parser, transform, validate, format, QA", CAT_SPECIALIST, EXEC_LINEAR,
       1, 5.0);
    TN(37, TIER_HAIKU, ROLE_CLASSIFIER, "parse", 1);
    TN(37, TIER_SONNET, ROLE_WORKER, "transform", 1);
    TN(37, TIER_SONNET, ROLE_VALIDATOR, "validate", 1);
    TN(37, TIER_HAIKU, ROLE_REDUCER, "format", 1);
    TN(37, TIER_OPUS, ROLE_JUDGE, "qa", 1);
    TE(37, 0, 1, EDGE_SEQUENCE);
    TE(37, 1, 2, EDGE_SEQUENCE);
    TE(37, 2, 3, EDGE_SEQUENCE);
    TE(37, 3, 4, EDGE_SEQUENCE);
    TF(37);

    TI(38, "newsroom", "Reporters gather, editor composes, chief approves", CAT_SPECIALIST,
       EXEC_PARALLEL_STAGES, 1, 3.0);
    for (int i = 0; i < 4; i++)
        TN(38, TIER_HAIKU, ROLE_SCOUT,
           i == 0   ? "reporter1"
           : i == 1 ? "reporter2"
           : i == 2 ? "reporter3"
                    : "reporter4",
           1);
    TN(38, TIER_SONNET, ROLE_SYNTHESIZER, "editor", 1);
    TN(38, TIER_OPUS, ROLE_JUDGE, "chief", 1);
    for (int i = 0; i < 4; i++)
        TE(38, i, 4, EDGE_FANIN);
    TE(38, 4, 5, EDGE_SEQUENCE);
    TF(38);

    TI(39, "orchestra", "Conductor sets direction, sections work in parallel, conductor merges",
       CAT_SPECIALIST, EXEC_PARALLEL_STAGES, 1, 2.0);
    TN(39, TIER_OPUS, ROLE_COORDINATOR, "conductor", 1);
    TN(39, TIER_SONNET, ROLE_SPECIALIST, "strings", 1);
    TN(39, TIER_SONNET, ROLE_SPECIALIST, "winds", 1);
    TN(39, TIER_SONNET, ROLE_SPECIALIST, "brass", 1);
    TN(39, TIER_HAIKU, ROLE_SPECIALIST, "percussion", 1);
    TN(39, TIER_OPUS, ROLE_SYNTHESIZER, "merge", 1);
    for (int i = 1; i <= 4; i++) {
        TE(39, 0, i, EDGE_FANOUT);
        TE(39, i, 5, EDGE_FANIN);
    }
    TF(39);

    TI(40, "kitchen_brigade", "Head chef plans, sous-chef delegates, line executes", CAT_SPECIALIST,
       EXEC_PARALLEL_STAGES, 1, 3.0);
    TN(40, TIER_OPUS, ROLE_COORDINATOR, "head_chef", 1);
    TN(40, TIER_SONNET, ROLE_DELEGATOR, "sous_chef", 1);
    TN(40, TIER_HAIKU, ROLE_WORKER, "prep1", 1);
    TN(40, TIER_HAIKU, ROLE_WORKER, "prep2", 1);
    TN(40, TIER_SONNET, ROLE_SPECIALIST, "saucier", 1);
    TN(40, TIER_HAIKU, ROLE_SPECIALIST, "pastry", 1);
    TN(40, TIER_OPUS, ROLE_JUDGE, "plate", 1);
    TE(40, 0, 1, EDGE_SEQUENCE);
    for (int i = 2; i <= 5; i++) {
        TE(40, 1, i, EDGE_FANOUT);
        TE(40, i, 6, EDGE_FANIN);
    }
    TF(40);
}

static void init_topology_registry_feedback(void) {
    TI(41, "critic_loop", "Generate, critique, refine with up to three rounds", CAT_FEEDBACK,
       EXEC_ITERATIVE, 3, 6.0);
    TN(41, TIER_SONNET, ROLE_GENERATOR, "generate", 1);
    TN(41, TIER_OPUS, ROLE_CRITIC, "critique", 1);
    TN(41, TIER_SONNET, ROLE_WORKER, "refine", 1);
    TE(41, 0, 1, EDGE_SEQUENCE);
    TE(41, 1, 2, EDGE_SEQUENCE);
    TE(41, 1, 0, EDGE_FEEDBACK);
    TF(41);

    TI(42, "polish", "Draft, review, fix, review cycle", CAT_FEEDBACK, EXEC_ITERATIVE, 2, 4.0);
    TN(42, TIER_HAIKU, ROLE_GENERATOR, "draft", 1);
    TN(42, TIER_SONNET, ROLE_CRITIC, "review1", 1);
    TN(42, TIER_HAIKU, ROLE_WORKER, "fix", 1);
    TN(42, TIER_SONNET, ROLE_VALIDATOR, "review2", 1);
    TE(42, 0, 1, EDGE_SEQUENCE);
    TE(42, 1, 2, EDGE_SEQUENCE);
    TE(42, 2, 3, EDGE_SEQUENCE);
    TE(42, 3, 0, EDGE_FEEDBACK);
    TF(42);

    TI(43, "adversarial", "Red team versus blue team with a judge", CAT_FEEDBACK, EXEC_LINEAR, 1,
       4.0);
    TN(43, TIER_SONNET, ROLE_GENERATOR, "red_generate", 1);
    TN(43, TIER_SONNET, ROLE_CRITIC, "blue_attack", 1);
    TN(43, TIER_SONNET, ROLE_WORKER, "red_defend", 1);
    TN(43, TIER_OPUS, ROLE_JUDGE, "judge", 1);
    TE(43, 0, 1, EDGE_SEQUENCE);
    TE(43, 1, 2, EDGE_SEQUENCE);
    TE(43, 2, 3, EDGE_SEQUENCE);
    TF(43);

    TI(44, "evolution", "Generate, select, mutate, select, finalize", CAT_FEEDBACK,
       EXEC_PARALLEL_STAGES, 1, 5.0);
    for (int i = 0; i < 4; i++)
        TN(44, TIER_HAIKU, ROLE_GENERATOR,
           i == 0   ? "gen1"
           : i == 1 ? "gen2"
           : i == 2 ? "gen3"
                    : "gen4",
           1);
    TN(44, TIER_SONNET, ROLE_JUDGE, "select1", 1);
    TN(44, TIER_HAIKU, ROLE_WORKER, "mutate1", 1);
    TN(44, TIER_HAIKU, ROLE_WORKER, "mutate2", 1);
    TN(44, TIER_HAIKU, ROLE_WORKER, "mutate3", 1);
    TN(44, TIER_SONNET, ROLE_JUDGE, "select2", 1);
    TN(44, TIER_OPUS, ROLE_JUDGE, "final", 1);
    for (int i = 0; i < 4; i++)
        TE(44, i, 4, EDGE_FANIN);
    TE(44, 4, 5, EDGE_FANOUT);
    TE(44, 4, 6, EDGE_FANOUT);
    TE(44, 4, 7, EDGE_FANOUT);
    TE(44, 5, 8, EDGE_FANIN);
    TE(44, 6, 8, EDGE_FANIN);
    TE(44, 7, 8, EDGE_FANIN);
    TE(44, 8, 9, EDGE_SEQUENCE);
    TF(44);

    TI(45, "debate", "Structured debate with rebuttal and final decision", CAT_FEEDBACK,
       EXEC_LINEAR, 1, 5.0);
    TN(45, TIER_SONNET, ROLE_SPECIALIST, "pro", 1);
    TN(45, TIER_SONNET, ROLE_SPECIALIST, "con", 1);
    TN(45, TIER_OPUS, ROLE_COORDINATOR, "moderate", 1);
    TN(45, TIER_SONNET, ROLE_WORKER, "rebuttal", 1);
    TN(45, TIER_OPUS, ROLE_JUDGE, "decide", 1);
    TE(45, 0, 1, EDGE_SEQUENCE);
    TE(45, 1, 2, EDGE_SEQUENCE);
    TE(45, 2, 3, EDGE_SEQUENCE);
    TE(45, 3, 4, EDGE_SEQUENCE);
    TF(45);

    TI(46, "annealing", "Plan, implement, test, adjust, retest with feedback loop", CAT_FEEDBACK,
       EXEC_ITERATIVE, 3, 5.0);
    TN(46, TIER_OPUS, ROLE_COORDINATOR, "plan", 1);
    TN(46, TIER_SONNET, ROLE_WORKER, "implement", 1);
    TN(46, TIER_HAIKU, ROLE_VALIDATOR, "test", 1);
    TN(46, TIER_SONNET, ROLE_WORKER, "adjust", 1);
    TN(46, TIER_HAIKU, ROLE_VALIDATOR, "retest", 1);
    TE(46, 0, 1, EDGE_SEQUENCE);
    TE(46, 1, 2, EDGE_SEQUENCE);
    TE(46, 2, 3, EDGE_SEQUENCE);
    TE(46, 3, 4, EDGE_SEQUENCE);
    TE(46, 4, 1, EDGE_FEEDBACK);
    TF(46);

    TI(47, "ratchet", "Incremental progress with checkpoints", CAT_FEEDBACK, EXEC_LINEAR, 1, 5.0);
    TN(47, TIER_HAIKU, ROLE_WORKER, "step1", 1);
    TN(47, TIER_SONNET, ROLE_VALIDATOR, "checkpoint1", 1);
    TN(47, TIER_HAIKU, ROLE_WORKER, "step2", 1);
    TN(47, TIER_SONNET, ROLE_VALIDATOR, "checkpoint2", 1);
    TN(47, TIER_OPUS, ROLE_JUDGE, "final", 1);
    TE(47, 0, 1, EDGE_SEQUENCE);
    TE(47, 1, 2, EDGE_SEQUENCE);
    TE(47, 2, 3, EDGE_SEQUENCE);
    TE(47, 3, 4, EDGE_SEQUENCE);
    TF(47);

    TI(48, "mirror", "Generate, simplify, compare, decide", CAT_FEEDBACK, EXEC_LINEAR, 1, 4.0);
    TN(48, TIER_SONNET, ROLE_GENERATOR, "generate", 1);
    TN(48, TIER_HAIKU, ROLE_REDUCER, "simplify", 1);
    TN(48, TIER_SONNET, ROLE_VALIDATOR, "compare", 1);
    TN(48, TIER_OPUS, ROLE_JUDGE, "decide", 1);
    TE(48, 0, 1, EDGE_SEQUENCE);
    TE(48, 1, 2, EDGE_SEQUENCE);
    TE(48, 0, 2, EDGE_FANIN);
    TE(48, 2, 3, EDGE_SEQUENCE);
    TF(48);
}

static void init_topology_registry_competitive(void) {
    TI(49, "tournament", "Multiple contestants, one judge", CAT_COMPETITIVE, EXEC_TOURNAMENT, 1,
       2.0);
    for (int i = 0; i < 4; i++)
        TN(49, TIER_SONNET, ROLE_GENERATOR,
           i == 0   ? "contestant1"
           : i == 1 ? "contestant2"
           : i == 2 ? "contestant3"
                    : "contestant4",
           1);
    TN(49, TIER_OPUS, ROLE_JUDGE, "judge", 1);
    for (int i = 0; i < 4; i++)
        TE(49, i, 4, EDGE_FANIN);
    TF(49);

    TI(50, "auction", "Confidence auction promotes two bids to a higher tier", CAT_COMPETITIVE,
       EXEC_TOURNAMENT, 1, 4.0);
    for (int i = 0; i < 6; i++)
        TN(50, TIER_HAIKU, ROLE_GENERATOR,
           i == 0   ? "bid1"
           : i == 1 ? "bid2"
           : i == 2 ? "bid3"
           : i == 3 ? "bid4"
           : i == 4 ? "bid5"
                    : "bid6",
           1);
    TN(50, TIER_SONNET, ROLE_JUDGE, "auctioneer", 1);
    TN(50, TIER_SONNET, ROLE_WORKER, "promoted1", 1);
    TN(50, TIER_SONNET, ROLE_WORKER, "promoted2", 1);
    TN(50, TIER_OPUS, ROLE_JUDGE, "winner", 1);
    for (int i = 0; i < 6; i++)
        TE(50, i, 6, EDGE_FANIN);
    TE(50, 6, 7, EDGE_FANOUT);
    TE(50, 6, 8, EDGE_FANOUT);
    TE(50, 7, 9, EDGE_FANIN);
    TE(50, 8, 9, EDGE_FANIN);
    TF(50);

    TI(51, "ensemble", "Diverse model ensemble merged by Opus", CAT_COMPETITIVE, EXEC_CONSENSUS, 1,
       2.0);
    TN(51, TIER_SONNET, ROLE_GENERATOR, "model_a", 1);
    TN(51, TIER_SONNET, ROLE_GENERATOR, "model_b", 1);
    TN(51, TIER_SONNET, ROLE_GENERATOR, "model_c", 1);
    TN(51, TIER_OPUS, ROLE_SYNTHESIZER, "weighted_merge", 1);
    TE(51, 0, 3, EDGE_FANIN);
    TE(51, 1, 3, EDGE_FANIN);
    TE(51, 2, 3, EDGE_FANIN);
    TF(51);

    TI(52, "gladiator", "Two fighters, two scorers, one decision", CAT_COMPETITIVE, EXEC_TOURNAMENT,
       1, 3.0);
    TN(52, TIER_SONNET, ROLE_GENERATOR, "fighter1", 1);
    TN(52, TIER_SONNET, ROLE_GENERATOR, "fighter2", 1);
    TN(52, TIER_HAIKU, ROLE_VALIDATOR, "score1", 1);
    TN(52, TIER_HAIKU, ROLE_VALIDATOR, "score2", 1);
    TN(52, TIER_OPUS, ROLE_JUDGE, "declare", 1);
    TE(52, 0, 2, EDGE_FANIN);
    TE(52, 1, 2, EDGE_FANIN);
    TE(52, 0, 3, EDGE_FANIN);
    TE(52, 1, 3, EDGE_FANIN);
    TE(52, 2, 4, EDGE_FANIN);
    TE(52, 3, 4, EDGE_FANIN);
    TF(52);

    TI(53, "monte_carlo", "Broad random exploration followed by identification and refinement",
       CAT_COMPETITIVE, EXEC_PARALLEL_STAGES, 1, 3.0);
    for (int i = 0; i < 8; i++)
        TN(53, TIER_HAIKU, ROLE_GENERATOR,
           i == 0   ? "explore1"
           : i == 1 ? "explore2"
           : i == 2 ? "explore3"
           : i == 3 ? "explore4"
           : i == 4 ? "explore5"
           : i == 5 ? "explore6"
           : i == 6 ? "explore7"
                    : "explore8",
           1);
    TN(53, TIER_SONNET, ROLE_JUDGE, "identify", 1);
    TN(53, TIER_OPUS, ROLE_WORKER, "refine_best", 1);
    for (int i = 0; i < 8; i++)
        TE(53, i, 8, EDGE_FANIN);
    TE(53, 8, 9, EDGE_SEQUENCE);
    TF(53);

    TI(54, "hedge",
       "Optimistic, pessimistic, balanced perspectives merged into a risk-adjusted result",
       CAT_COMPETITIVE, EXEC_CONSENSUS, 1, 2.0);
    TN(54, TIER_SONNET, ROLE_SPECIALIST, "optimistic", 1);
    TN(54, TIER_SONNET, ROLE_SPECIALIST, "pessimistic", 1);
    TN(54, TIER_SONNET, ROLE_SPECIALIST, "balanced", 1);
    TN(54, TIER_OPUS, ROLE_JUDGE, "risk_adjust", 1);
    TE(54, 0, 3, EDGE_FANIN);
    TE(54, 1, 3, EDGE_FANIN);
    TE(54, 2, 3, EDGE_FANIN);
    TF(54);
}

static void init_topology_registry_domain(void) {
    TI(55, "code_review", "Lint, logic review, security review, verdict", CAT_DOMAIN, EXEC_LINEAR,
       1, 4.0);
    TN(55, TIER_HAIKU, ROLE_VALIDATOR, "lint", 1);
    TN(55, TIER_SONNET, ROLE_SPECIALIST, "logic", 1);
    TN(55, TIER_SONNET, ROLE_SPECIALIST, "security", 1);
    TN(55, TIER_OPUS, ROLE_JUDGE, "verdict", 1);
    TE(55, 0, 1, EDGE_SEQUENCE);
    TE(55, 1, 2, EDGE_SEQUENCE);
    TE(55, 2, 3, EDGE_SEQUENCE);
    TF(55);

    TI(56, "research", "Parallel gathering, dual analysis, final synthesis", CAT_DOMAIN,
       EXEC_PARALLEL_STAGES, 1, 3.0);
    for (int i = 0; i < 4; i++)
        TN(56, TIER_HAIKU, ROLE_SCOUT,
           i == 0   ? "gather1"
           : i == 1 ? "gather2"
           : i == 2 ? "gather3"
                    : "gather4",
           1);
    TN(56, TIER_SONNET, ROLE_SPECIALIST, "analyze1", 1);
    TN(56, TIER_SONNET, ROLE_SPECIALIST, "analyze2", 1);
    TN(56, TIER_OPUS, ROLE_SYNTHESIZER, "synthesize", 1);
    TE(56, 0, 4, EDGE_FANIN);
    TE(56, 1, 4, EDGE_FANIN);
    TE(56, 2, 5, EDGE_FANIN);
    TE(56, 3, 5, EDGE_FANIN);
    TE(56, 4, 6, EDGE_FANIN);
    TE(56, 5, 6, EDGE_FANIN);
    TF(56);

    TI(57, "incident", "Incident triage through verification", CAT_DOMAIN, EXEC_LINEAR, 1, 5.0);
    TN(57, TIER_HAIKU, ROLE_CLASSIFIER, "triage", 1);
    TN(57, TIER_SONNET, ROLE_WORKER, "diagnose", 1);
    TN(57, TIER_OPUS, ROLE_COORDINATOR, "plan", 1);
    TN(57, TIER_SONNET, ROLE_WORKER, "fix", 1);
    TN(57, TIER_HAIKU, ROLE_VALIDATOR, "verify", 1);
    TE(57, 0, 1, EDGE_SEQUENCE);
    TE(57, 1, 2, EDGE_SEQUENCE);
    TE(57, 2, 3, EDGE_SEQUENCE);
    TE(57, 3, 4, EDGE_SEQUENCE);
    TF(57);

    TI(58, "data_pipeline", "Ingest, clean, transform, validate, sign off", CAT_DOMAIN, EXEC_LINEAR,
       1, 5.0);
    TN(58, TIER_HAIKU, ROLE_SCOUT, "ingest", 1);
    TN(58, TIER_HAIKU, ROLE_REDUCER, "clean", 1);
    TN(58, TIER_SONNET, ROLE_WORKER, "transform", 1);
    TN(58, TIER_SONNET, ROLE_VALIDATOR, "validate", 1);
    TN(58, TIER_OPUS, ROLE_JUDGE, "signoff", 1);
    TE(58, 0, 1, EDGE_SEQUENCE);
    TE(58, 1, 2, EDGE_SEQUENCE);
    TE(58, 2, 3, EDGE_SEQUENCE);
    TE(58, 3, 4, EDGE_SEQUENCE);
    TF(58);

    TI(59, "security_audit", "Parallel scan, audit, risk review, remediation", CAT_DOMAIN,
       EXEC_PARALLEL_STAGES, 1, 4.0);
    TN(59, TIER_HAIKU, ROLE_SCOUT, "scan1", 1);
    TN(59, TIER_HAIKU, ROLE_SCOUT, "scan2", 1);
    TN(59, TIER_HAIKU, ROLE_SCOUT, "scan3", 1);
    TN(59, TIER_SONNET, ROLE_SPECIALIST, "audit", 1);
    TN(59, TIER_OPUS, ROLE_JUDGE, "risk", 1);
    TN(59, TIER_SONNET, ROLE_WORKER, "remediate", 1);
    TE(59, 0, 3, EDGE_FANIN);
    TE(59, 1, 3, EDGE_FANIN);
    TE(59, 2, 3, EDGE_FANIN);
    TE(59, 3, 4, EDGE_SEQUENCE);
    TE(59, 4, 5, EDGE_SEQUENCE);
    TF(59);

    TI(60, "creative", "Brainstorm, curate, direct, polish, package", CAT_DOMAIN,
       EXEC_PARALLEL_STAGES, 1, 5.0);
    for (int i = 0; i < 6; i++)
        TN(60, TIER_HAIKU, ROLE_GENERATOR,
           i == 0   ? "brainstorm1"
           : i == 1 ? "brainstorm2"
           : i == 2 ? "brainstorm3"
           : i == 3 ? "brainstorm4"
           : i == 4 ? "brainstorm5"
                    : "brainstorm6",
           1);
    TN(60, TIER_SONNET, ROLE_JUDGE, "curate", 1);
    TN(60, TIER_OPUS, ROLE_COORDINATOR, "direct", 1);
    TN(60, TIER_SONNET, ROLE_WORKER, "polish", 1);
    TN(60, TIER_HAIKU, ROLE_REDUCER, "package", 1);
    for (int i = 0; i < 6; i++)
        TE(60, i, 6, EDGE_FANIN);
    TE(60, 6, 7, EDGE_SEQUENCE);
    TE(60, 7, 8, EDGE_SEQUENCE);
    TE(60, 8, 9, EDGE_SEQUENCE);
    TF(60);
}

void topology_registry_init(void) {
    if (g_topologies_inited)
        return;
    memset(g_topologies, 0, sizeof(g_topologies));
    init_topology_registry_linear();
    init_topology_registry_fanout();
    init_topology_registry_hierarchy();
    init_topology_registry_mesh();
    init_topology_registry_specialist();
    init_topology_registry_feedback();
    init_topology_registry_competitive();
    init_topology_registry_domain();
    g_topologies_inited = true;
}

const topology_t *topology_get(int id) {
    topology_registry_init();
    if (id < 1 || id > TOPOLOGY_COUNT)
        return NULL;
    return &g_topologies[id - 1];
}

const topology_t *topology_find(const char *name) {
    topology_registry_init();
    if (!name || !name[0])
        return NULL;
    for (int i = 0; i < TOPOLOGY_COUNT; i++) {
        if (strcasecmp(name, g_topologies[i].name) == 0)
            return &g_topologies[i];
    }
    return NULL;
}

const topology_t *topology_registry(int *count) {
    topology_registry_init();
    if (count)
        *count = TOPOLOGY_COUNT;
    return g_topologies;
}

const topology_t *topology_auto_select(const char *task) {
    topology_registry_init();
    if (!task || !task[0])
        return topology_find("triage");

    /* Phase 1: scored semantic profiling via task_profile() */
    task_profile_t *tp = task_profile(task, NULL);
    if (tp) {
        const topology_t *best = task_profile_best_topology(tp);
        task_profile_free(tp);
        if (best)
            return best;
    }

    /* Fallback: enum-based keyword matching */
    topology_task_profile_t profile;
    if (!topology_profile_task(task, &profile))
        return topology_find("triage");

    switch (profile.kind) {
        case TOPO_TASK_CODE:
            return topology_find("clinic");
        case TOPO_TASK_RESEARCH:
            return topology_find("research");
        case TOPO_TASK_REVIEW:
            return topology_find("code_review");
        case TOPO_TASK_CREATIVE:
            return topology_find("creative");
        case TOPO_TASK_INCIDENT:
            return topology_find("incident");
        case TOPO_TASK_GENERAL:
        default:
            return topology_find("triage");
    }
}

static bool is_dynamic_topology_name(const char *name);

bool topology_plan_build(const char *preferred_topology, bool auto_mode, const char *task,
                         topology_plan_t *plan) {
    if (!plan)
        return false;
    memset(plan, 0, sizeof(*plan));

    if (preferred_topology && preferred_topology[0] &&
        !is_dynamic_topology_name(preferred_topology)) {
        const topology_t *static_topo = topology_find(preferred_topology);
        if (!static_topo)
            return false;
        plan->topology = *static_topo;
        snprintf(plan->rationale, sizeof(plan->rationale), "explicit topology: %s",
                 static_topo->name);
        return true;
    }

    if (!auto_mode && !is_dynamic_topology_name(preferred_topology))
        return false;

    topology_profile_task(task, &plan->profile);
    build_dynamic_plan(&plan->profile, task, plan);
    return true;
}

bool topology_is_runnable(const topology_t *t) {
    return t && t->node_count > 0;
}

static __attribute__((unused)) const char *edge_type_label(edge_type_t type) {
    switch (type) {
        case EDGE_SEQUENCE:
            return "->";
        case EDGE_FANOUT:
            return "=>";
        case EDGE_FANIN:
            return "<=";
        case EDGE_FEEDBACK:
            return "~>";
        case EDGE_CONDITIONAL:
            return "?>";
        case EDGE_COMPETE:
            return "vs";
    }
    return "->";
}

/* ── Tree-form topology rendering ─────────────────────────────────── */

/* Compute topological depth (longest path from a root) for each node.
 * Nodes at the same depth form a "stage" and render on the same row. */
static void topo_compute_depth(const topology_t *t, int *depth) {
    for (int i = 0; i < t->node_count; i++)
        depth[i] = -1;

    /* Find roots: nodes with no non-feedback incoming edges */
    bool changed = true;
    for (int i = 0; i < t->node_count; i++) {
        bool has_incoming = false;
        for (int e = 0; e < t->edge_count; e++) {
            if (t->edges[e].to == i && t->edges[e].type != EDGE_FEEDBACK) {
                has_incoming = true;
                break;
            }
        }
        if (!has_incoming)
            depth[i] = 0;
    }

    /* BFS-like propagation: depth[child] = max(depth[parent] + 1) */
    while (changed) {
        changed = false;
        for (int e = 0; e < t->edge_count; e++) {
            if (t->edges[e].type == EDGE_FEEDBACK)
                continue;
            int from = t->edges[e].from;
            int to = t->edges[e].to;
            if (depth[from] < 0)
                continue;
            int d = depth[from] + 1;
            if (d > depth[to]) {
                depth[to] = d;
                changed = true;
            }
        }
    }
    /* Unreachable nodes get depth 0 */
    for (int i = 0; i < t->node_count; i++)
        if (depth[i] < 0)
            depth[i] = 0;
}

/* Render a node label: [H:tag] or [S:tag x3] */
static int render_node_label(const topo_node_t *n, char *out, size_t len) {
    if (n->replicas > 1)
        return snprintf(out, len, "[%s:%s x%d]", tier_label(n->tier), n->tag, n->replicas);
    return snprintf(out, len, "[%s:%s]", tier_label(n->tier), n->tag);
}

/* Edge glyph for vertical connectors */
static const char *edge_glyph(edge_type_t type) {
    switch (type) {
        case EDGE_SEQUENCE:
            return "|";
        case EDGE_FANOUT:
            return "|";
        case EDGE_FANIN:
            return "|";
        case EDGE_FEEDBACK:
            return "~";
        case EDGE_CONDITIONAL:
            return "?";
        case EDGE_COMPETE:
            return "!";
    }
    return "|";
}

int topology_render_ascii(const topology_t *t, char *buf, size_t buflen) {
    if (!buf || buflen == 0)
        return -1;
    buf[0] = '\0';
    if (!t || t->node_count == 0)
        return -1;

    int depth[TOPO_MAX_NODES];
    topo_compute_depth(t, depth);

    /* Find max depth */
    int max_depth = 0;
    for (int i = 0; i < t->node_count; i++)
        if (depth[i] > max_depth)
            max_depth = depth[i];

    jbuf_t b;
    jbuf_init(&b, 2048);

    /* Header */
    jbuf_appendf(&b, "%s  [%s/%d agents/%.1fx]\n", t->name, topo_category_label(t->category),
                 t->total_agents, t->est_latency_mult);

    /* Render stage by stage (depth 0, 1, 2, ...) */
    for (int d = 0; d <= max_depth; d++) {

        /* Collect nodes at this depth */
        int stage_nodes[TOPO_MAX_NODES];
        int stage_count = 0;
        for (int i = 0; i < t->node_count; i++)
            if (depth[i] == d)
                stage_nodes[stage_count++] = i;

        if (stage_count == 0)
            continue;

        /* Render connector lines from previous stage */
        if (d > 0) {
            /* Determine the edge type(s) connecting to this stage */
            edge_type_t predominant = EDGE_SEQUENCE;
            for (int e = 0; e < t->edge_count; e++) {
                if (t->edges[e].type == EDGE_FEEDBACK)
                    continue;
                if (depth[t->edges[e].to] == d && depth[t->edges[e].from] == d - 1) {
                    predominant = t->edges[e].type;
                    break;
                }
            }
            const char *glyph = edge_glyph(predominant);

            if (stage_count == 1) {
                /* Single child: vertical line */
                jbuf_appendf(&b, "    %s\n", glyph);
                jbuf_appendf(&b, "    v\n");
            } else {
                /* Multiple children: fan-out tree branches */
                jbuf_append(&b, "   ");
                for (int i = 0; i < stage_count; i++) {
                    if (i == 0)
                        jbuf_append(&b, " /");
                    else if (i == stage_count - 1)
                        jbuf_appendf(&b, "%s\\", glyph);
                    else
                        jbuf_appendf(&b, "%s", glyph);
                    if (i < stage_count - 1)
                        jbuf_append(&b, "---");
                }
                jbuf_append(&b, "\n");
            }
        }

        /* Render the node labels for this stage */
        if (stage_count == 1) {
            char label[80];
            render_node_label(&t->nodes[stage_nodes[0]], label, sizeof(label));
            jbuf_appendf(&b, "  %s\n", label);
        } else {
            /* Multiple nodes side by side */
            jbuf_append(&b, "  ");
            for (int i = 0; i < stage_count; i++) {
                char label[80];
                render_node_label(&t->nodes[stage_nodes[i]], label, sizeof(label));
                if (i > 0)
                    jbuf_append(&b, "  ");
                jbuf_append(&b, label);
            }
            jbuf_append(&b, "\n");
        }
    }

    /* Render feedback loops at the bottom */
    for (int e = 0; e < t->edge_count; e++) {
        if (t->edges[e].type == EDGE_FEEDBACK) {
            jbuf_appendf(&b, "    ~> feedback: %s -> %s\n", t->nodes[t->edges[e].from].tag,
                         t->nodes[t->edges[e].to].tag);
        }
    }

    int written = (int)(b.len < buflen - 1 ? b.len : buflen - 1);
    memcpy(buf, b.data, (size_t)written);
    buf[written] = '\0';
    jbuf_free(&b);
    return written;
}

double topology_estimate_cost(const topology_t *t, int input_tokens, int output_tokens) {
    if (!t)
        return 0.0;
    double units = (input_tokens + output_tokens) / 1000.0;
    if (units < 1.0)
        units = 1.0;
    return t->est_cost_1k * units;
}

bool topology_plan_run(const topology_plan_t *plan, const char *api_key,
                       const char *coordinator_model, const char *task, char *result, size_t rlen,
                       topology_run_stats_t *stats) {
    if (!plan)
        return false;
    return topology_run(&plan->topology, api_key, coordinator_model, task, result, rlen, stats);
}

/* OpenRouter swarm tier defaults. The tier enum names are legacy, but the
 * default models follow the current policy:
 *   HAIKU  → z-ai/glm-5.2
 *   SONNET → moonshotai/kimi-k2.7-code
 *   OPUS   → z-ai/glm-5.2
 *
 * Override per-tier at runtime:
 *   DSCO_SWARM_HAIKU=z-ai/glm-5.2
 *   DSCO_SWARM_SONNET=moonshotai/kimi-k2.7-code
 *   DSCO_SWARM_OPUS=z-ai/glm-5.2
 */
static const char *or_tier_model(model_tier_t tier) {
    switch (tier) {
        case TIER_HAIKU: {
            const char *e = getenv("DSCO_SWARM_HAIKU");
            return (e && e[0]) ? e : "openrouter/z-ai/glm-5.2";
        }
        case TIER_SONNET: {
            const char *e = getenv("DSCO_SWARM_SONNET");
            return (e && e[0]) ? e : "openrouter/moonshotai/kimi-k2.7-code";
        }
        case TIER_OPUS: {
            const char *e = getenv("DSCO_SWARM_OPUS");
            return (e && e[0]) ? e : "openrouter/z-ai/glm-5.2";
        }
    }
    return "openrouter/z-ai/glm-5.2";
}

/* ── Heterogeneous tier pool ──────────────────────────────────────────────
 * When DSCO_TOPO_HETERO is set, every topology node resolves its tier to a
 * genuinely cross-provider model instead of three variants of one family,
 * turning any of the 60 topologies into a multi-model graph:
 *   HAIKU  → fast/cheap classify·scout·validate  (stepfun/step-3.7-flash)
 *   SONNET → coder worker·implement·analyze       (moonshotai/kimi-k2.7-code)
 *   OPUS   → long-ctx coordinate·synthesize·judge (z-ai/glm-5.2)
 * Each tier is still overridable via DSCO_SWARM_{HAIKU,SONNET,OPUS}. */
bool topology_hetero_enabled(void) {
    const char *e = getenv("DSCO_TOPO_HETERO");
    return e && e[0] && e[0] != '0';
}

bool topology_throughput_enabled(void) {
    return dsco_env_bool("DSCO_TOPO_THROUGHPUT", false) ||
           dsco_env_bool("DSCO_MAX_THROUGHPUT", false) ||
           dsco_env_bool("DSCO_PROVIDER_THROUGHPUT", false);
}

static const char *hetero_tier_model(model_tier_t tier) {
    switch (tier) {
        case TIER_HAIKU: {
            const char *e = getenv("DSCO_SWARM_HAIKU");
            return (e && e[0]) ? e : "openrouter/stepfun/step-3.7-flash";
        }
        case TIER_SONNET: {
            const char *e = getenv("DSCO_SWARM_SONNET");
            return (e && e[0]) ? e : "openrouter/moonshotai/kimi-k2.7-code";
        }
        case TIER_OPUS: {
            const char *e = getenv("DSCO_SWARM_OPUS");
            return (e && e[0]) ? e : "openrouter/z-ai/glm-5.2";
        }
    }
    return "openrouter/z-ai/glm-5.2";
}

static bool topology_model_has_openrouter_prefix(const char *model) {
    return model &&
           (strncmp(model, "openrouter/", 11) == 0 || strncmp(model, "openrouter:", 11) == 0);
}

static const char *topology_resolve_openrouter_model(const char *model, char *buf, size_t buflen) {
    const char *resolved = topology_model_has_openrouter_prefix(model)
                               ? model
                               : model_resolve_alias(model ? model : "auto");
    if (!resolved || !resolved[0])
        resolved = "auto";

    if (!buf || buflen == 0)
        return resolved;

    if (topology_model_has_openrouter_prefix(resolved)) {
        snprintf(buf, buflen, "%s", resolved);
    } else {
        snprintf(buf, buflen, "openrouter/%s", resolved);
    }
    return buf;
}

const char *topology_resolve_model_for_tier(const char *coordinator_model, const char *api_key,
                                            model_tier_t tier, char *buf, size_t buflen) {
    /* Heterogeneous pool short-circuits provider detection entirely. */
    if (topology_hetero_enabled()) {
        return topology_resolve_openrouter_model(hetero_tier_model(tier), buf, buflen);
    }

    const char *model = coordinator_model && coordinator_model[0]
                            ? model_resolve_alias(coordinator_model)
                            : DEFAULT_MODEL;
    const char *provider = provider_detect(model, api_key);
    const char *resolved = NULL;

    if (strcmp(provider, "anthropic") == 0) {
        resolved = tier_model_id(tier);
    } else if (strcmp(provider, "openrouter") == 0) {
        return topology_resolve_openrouter_model(or_tier_model(tier), buf, buflen);
    } else if (strcmp(provider, "openai") == 0) {
        resolved = (tier == TIER_OPUS) ? "o1" : (tier == TIER_SONNET ? "gpt-4o" : "gpt-4o-mini");
    } else if (strcmp(provider, "groq") == 0) {
        resolved = (tier == TIER_HAIKU) ? "llama-3.1-8b-instant" : "llama-3.3-70b-versatile";
    } else if (strcmp(provider, "deepseek") == 0) {
        resolved = (tier == TIER_OPUS) ? "deepseek-reasoner" : "deepseek-chat";
    } else if (strcmp(provider, "mistral") == 0) {
        resolved = (tier == TIER_HAIKU)
                       ? "mistral-small-latest"
                       : (tier == TIER_SONNET ? "codestral-latest" : "mistral-large-latest");
    } else if (strcmp(provider, "perplexity") == 0) {
        resolved = (tier == TIER_HAIKU) ? "sonar" : "sonar-pro";
    } else {
        resolved = model;
    }

    if (buf && buflen > 0)
        snprintf(buf, buflen, "%s", model_resolve_alias(resolved));
    return buf ? buf : model_resolve_alias(resolved);
}

typedef struct {
    char provider[64];
    char model[128];
} topo_throughput_lane_t;

static bool topology_provider_is_local_lane(const char *provider) {
    if (!provider || !provider[0])
        return false;
    const char *p = provider_profile_canonical_name(provider);
    return strcmp(p, "ollama") == 0 || strcmp(p, "lmstudio") == 0 ||
           strcmp(p, "mlx") == 0 || strcmp(p, "local") == 0;
}

static bool topology_provider_supported_for_lane(const char *provider) {
    const provider_profile_t *profile = provider_profile_find(provider);
    if (!profile)
        return false;
    return provider_profile_transport_supported(profile) ||
           profile->transport == PROVIDER_TRANSPORT_CODEX_RESPONSES;
}

static bool topology_lane_provider_exists(const topo_throughput_lane_t lanes[], int count,
                                          const char *provider) {
    for (int i = 0; i < count; i++) {
        if (strcmp(lanes[i].provider, provider) == 0)
            return true;
    }
    return false;
}

static bool topology_add_throughput_lane(topo_throughput_lane_t lanes[], int *count, int max,
                                         const char *provider, const char *model,
                                         const char *api_key) {
    if (!lanes || !count || *count >= max || !provider || !provider[0] || !model || !model[0])
        return false;

    const char *canonical = provider_profile_canonical_name(provider);
    if (!topology_provider_supported_for_lane(canonical))
        return false;
    if (strcmp(canonical, "openai-codex") == 0 &&
        dsco_env_bool("DSCO_DISABLE_CODEX_OAUTH_DISCOVERY", false))
        return false;
    if (topology_provider_is_local_lane(canonical) &&
        !dsco_env_bool("DSCO_TOPO_THROUGHPUT_INCLUDE_LOCAL", false))
        return false;
    /* Fugu PAYG keys are metered overflow capacity, not default throughput
     * lanes. Only subscription-class Sakana keys should preempt slot 0. */
    if (strcmp(canonical, "sakana") == 0 && !provider_sakana_current_key_is_subscription())
        return false;
    if (!provider_has_usable_key(canonical, api_key))
        return false;

    for (int i = 0; i < *count; i++) {
        if (strcmp(lanes[i].provider, canonical) == 0 && strcmp(lanes[i].model, model) == 0)
            return false;
    }

    snprintf(lanes[*count].provider, sizeof(lanes[*count].provider), "%s", canonical);
    snprintf(lanes[*count].model, sizeof(lanes[*count].model), "%s", model);
    (*count)++;
    return true;
}

static const char *topology_native_lane_model(const provider_profile_t *profile,
                                              model_tier_t tier) {
    if (!profile || !profile->name)
        return NULL;
    const char *p = profile->name;

    if (strcmp(p, "sakana") == 0)
        return tier == TIER_OPUS ? "fugu-ultra" : "fugu";
    if (strcmp(p, "anthropic") == 0)
        return tier == TIER_HAIKU   ? "claude-haiku-4-5-20251001"
               : tier == TIER_OPUS ? "claude-opus-4-8"
                                    : "claude-sonnet-4-6";
    if (strcmp(p, "openai-codex") == 0)
        return "gpt-5.5";
    if (strcmp(p, "openai") == 0)
        return tier == TIER_HAIKU   ? "gpt-4.1-mini"
               : tier == TIER_OPUS ? "gpt-5.4"
                                    : "gpt-4.1";
    if (strcmp(p, "xai") == 0)
        return tier == TIER_OPUS ? "grok-4" : "grok-4-fast";
    if (strcmp(p, "moonshot") == 0)
        return "kimi-k2.7-code-highspeed";
    if (strcmp(p, "google") == 0)
        return tier == TIER_OPUS ? "gemini-2.5-pro" : "gemini-2.5-flash";
    if (strcmp(p, "groq") == 0)
        return tier == TIER_HAIKU ? "llama-3.1-8b-instant" : "llama-3.3-70b-versatile";
    if (strcmp(p, "deepseek") == 0)
        return tier == TIER_OPUS ? "deepseek-reasoner" : "deepseek-chat";
    if (strcmp(p, "mistral") == 0)
        return tier == TIER_HAIKU   ? "mistral-small-latest"
               : tier == TIER_OPUS ? "mistral-large-latest"
                                    : "codestral-latest";
    if (strcmp(p, "perplexity") == 0)
        return tier == TIER_HAIKU ? "sonar" : "sonar-pro";
    if (strcmp(p, "cerebras") == 0)
        return "qwen-3-235b-a22b-instruct-2507";
    if (strcmp(p, "cohere") == 0)
        return "command-a-03-2025";
    if (strcmp(p, "alibaba") == 0 || strcmp(p, "alibaba-coding-plan") == 0 ||
        strcmp(p, "qwen-oauth") == 0)
        return "qwen3-coder-plus";
    if (strcmp(p, "zai") == 0)
        return "glm-5.2";

    if (tier == TIER_HAIKU && profile->default_aux_model && profile->default_aux_model[0])
        return profile->default_aux_model;
    return profile->default_model;
}

static const char *topology_openrouter_lane_model(model_tier_t tier, int slot) {
    static const char *haiku[] = {
        "stepfun/step-3.7-flash",
        "qwen/qwen3.5-flash-02-23",
        "z-ai/glm-5.2",
    };
    static const char *sonnet[] = {
        "moonshotai/kimi-k2.7-code",
        "x-ai/grok-4.20-beta",
        "deepseek/deepseek-chat",
    };
    static const char *opus[] = {
        "z-ai/glm-5.2",
        "google/gemini-2.5-pro",
        "anthropic/claude-sonnet-4.6",
    };

    const char **models = sonnet;
    int count = (int)(sizeof(sonnet) / sizeof(sonnet[0]));
    if (tier == TIER_HAIKU) {
        models = haiku;
        count = (int)(sizeof(haiku) / sizeof(haiku[0]));
    } else if (tier == TIER_OPUS) {
        models = opus;
        count = (int)(sizeof(opus) / sizeof(opus[0]));
    }
    if (slot < 0)
        slot = 0;
    return models[slot % count];
}

static int topology_collect_throughput_lanes(const char *api_key, model_tier_t tier,
                                             topo_throughput_lane_t lanes[], int max) {
    int count = 0;
    static const char *preferred[] = {
        "sakana",       "anthropic", "openai-codex", "openai",   "xai",
        "moonshot",     "google",    "groq",         "cerebras", "deepseek",
        "mistral",      "together",  "cohere",       "perplexity",
        "alibaba",      "alibaba-coding-plan",       "qwen-oauth",
        "zai",          NULL,
    };

    for (int i = 0; preferred[i] && count < max; i++) {
        const provider_profile_t *profile = provider_profile_find(preferred[i]);
        const char *model = topology_native_lane_model(profile, tier);
        if (profile && model)
            topology_add_throughput_lane(lanes, &count, max, profile->name, model, api_key);
    }

    if (provider_has_usable_key("openrouter", api_key)) {
        for (int i = 0; i < 3 && count < max; i++) {
            topology_add_throughput_lane(lanes, &count, max, "openrouter",
                                         topology_openrouter_lane_model(tier, i), api_key);
        }
    }

    for (size_t i = 0; i < provider_profile_count() && count < max; i++) {
        const provider_profile_t *profile = provider_profile_at(i);
        if (!profile || !profile->name || strcmp(profile->name, "openrouter") == 0)
            continue;
        if (topology_lane_provider_exists(lanes, count, profile->name))
            continue;
        const char *model = topology_native_lane_model(profile, tier);
        if (model)
            topology_add_throughput_lane(lanes, &count, max, profile->name, model, api_key);
    }

    return count;
}

bool topology_resolve_throughput_lane_for_tier(const char *api_key, model_tier_t tier, int slot,
                                               char *provider_buf, size_t provider_len,
                                               char *model_buf, size_t model_len) {
    if (!provider_buf || provider_len == 0 || !model_buf || model_len == 0)
        return false;

    topo_throughput_lane_t lanes[64];
    int count = topology_collect_throughput_lanes(api_key, tier, lanes,
                                                  (int)(sizeof(lanes) / sizeof(lanes[0])));
    if (count <= 0) {
        provider_buf[0] = '\0';
        model_buf[0] = '\0';
        return false;
    }

    if (slot < 0)
        slot = 0;
    const topo_throughput_lane_t *lane = &lanes[slot % count];
    snprintf(provider_buf, provider_len, "%s", lane->provider);
    snprintf(model_buf, model_len, "%s", lane->model);
    return true;
}

static bool has_incoming_type(const topology_t *t, int node_id, edge_type_t type) {
    for (int i = 0; i < t->edge_count; i++) {
        if (t->edges[i].to == node_id && t->edges[i].type == type)
            return true;
    }
    return false;
}

static bool has_feedback_edges(const topology_t *t) {
    for (int i = 0; i < t->edge_count; i++) {
        if (t->edges[i].type == EDGE_FEEDBACK)
            return true;
    }
    return false;
}

static bool node_is_sink(const topology_t *t, int node_id) {
    for (int i = 0; i < t->edge_count; i++) {
        if (t->edges[i].from == node_id && t->edges[i].type != EDGE_FEEDBACK) {
            return false;
        }
    }
    return true;
}

static char *trim_copy(const char *s, size_t max_len) {
    if (!s)
        return safe_strdup("");
    size_t len = strlen(s);
    if (len <= max_len)
        return safe_strdup(s);
    const char *tail = s + (len - max_len);
    char *out = safe_malloc(max_len + 48);
    snprintf(out, max_len + 48, "[...trimmed %zu bytes...]\n%s", len - max_len, tail);
    return out;
}

static char *strip_ansi_copy(const char *src) {
    if (!src)
        return safe_strdup("");
    size_t len = strlen(src);
    char *out = safe_malloc(len + 1);
    size_t w = 0;
    for (size_t i = 0; i < len; i++) {
        if ((unsigned char)src[i] == 0x1b && i + 1 < len && src[i + 1] == '[') {
            i += 2;
            while (i < len && ((src[i] >= '0' && src[i] <= '9') || src[i] == ';' || src[i] == '?'))
                i++;
            continue;
        }
        out[w++] = src[i];
    }
    out[w] = '\0';
    return out;
}

static char *extract_marked_output(const char *raw) {
    if (!raw)
        return safe_strdup("");
    const char *start = strstr(raw, TOPO_STAGE_MARKER_BEGIN);
    if (!start)
        return NULL;
    start += strlen(TOPO_STAGE_MARKER_BEGIN);
    const char *end = strstr(start, TOPO_STAGE_MARKER_END);
    if (!end || end <= start)
        return NULL;
    size_t len = (size_t)(end - start);
    while (len > 0 && isspace((unsigned char)start[0])) {
        start++;
        len--;
    }
    while (len > 0 && isspace((unsigned char)start[len - 1]))
        len--;
    char *out = safe_malloc(len + 1);
    memcpy(out, start, len);
    out[len] = '\0';
    return out;
}

static char *normalize_node_output(const char *raw) {
    char *marked = extract_marked_output(raw);
    if (marked)
        return marked;
    char *plain = strip_ansi_copy(raw);
    char *trimmed = trim_copy(plain, 12000);
    free(plain);
    return trimmed;
}

static bool strcasestr_simple(const char *haystack, const char *needle) {
    if (!haystack || !needle || !needle[0])
        return false;
    size_t nlen = strlen(needle);
    for (const char *p = haystack; *p; p++) {
        if (strncasecmp(p, needle, nlen) == 0)
            return true;
    }
    return false;
}

static bool task_mentions_self_improvement(const char *task) {
    return strcasestr_simple(task, "self-improvement") ||
           strcasestr_simple(task, "self_improve") ||
           (strcasestr_simple(task, "self") && strcasestr_simple(task, "improvement")) ||
           (strcasestr_simple(task, "learn") && strcasestr_simple(task, "patterns"));
}

static bool task_mentions_large_scale_ai_work(const char *task) {
    bool scale = strcasestr_simple(task, "large-scale") ||
                 (strcasestr_simple(task, "large") && strcasestr_simple(task, "scale")) ||
                 strcasestr_simple(task, "broad") ||
                 strcasestr_simple(task, "wide");
    bool ai = strcasestr_simple(task, "ai") ||
              strcasestr_simple(task, "agent") ||
              strcasestr_simple(task, "model") ||
              strcasestr_simple(task, "dsco-cli");
    return scale && (ai || task_mentions_self_improvement(task));
}

static topology_task_kind_t map_task_profile_to_kind(const task_profile_t *tp, const char *task) {
    int score[TOPO_TASK_INCIDENT + 1] = {0};
    if (!tp)
        return TOPO_TASK_GENERAL;

    if (tp->patterns[PATTERN_ANALYSIS]) {
        score[TOPO_TASK_RESEARCH] += 2;
    }
    if (tp->patterns[PATTERN_CODING]) {
        score[TOPO_TASK_CODE] += 4;
    }
    if (tp->patterns[PATTERN_REVIEW]) {
        score[TOPO_TASK_REVIEW] += 4;
    }
    if (tp->patterns[PATTERN_PLANNING]) {
        score[TOPO_TASK_GENERAL] += 1;
        score[TOPO_TASK_CODE] += 1;
    }
    if (tp->patterns[PATTERN_SYNTHESIS]) {
        score[TOPO_TASK_RESEARCH] += 1;
        score[TOPO_TASK_REVIEW] += 1;
    }
    if (tp->patterns[PATTERN_REASONING]) {
        score[TOPO_TASK_CODE] += 1;
        score[TOPO_TASK_REVIEW] += 1;
    }
    if (tp->patterns[PATTERN_ITERATION]) {
        score[TOPO_TASK_REVIEW] += 1;
        score[TOPO_TASK_CODE] += 1;
    }
    if (tp->patterns[PATTERN_PARALLELISM]) {
        score[TOPO_TASK_RESEARCH] += 1;
    }
    if (tp->patterns[PATTERN_CONSENSUS]) {
        score[TOPO_TASK_REVIEW] += 2;
    }
    if (tp->patterns[PATTERN_SPECIALIST]) {
        score[TOPO_TASK_CODE] += 1;
    }

    if (task_mentions_self_improvement(task)) {
        score[TOPO_TASK_CODE] += 2;
        score[TOPO_TASK_REVIEW] += 1;
        score[TOPO_TASK_RESEARCH] += 1;
    }
    if (task_mentions_large_scale_ai_work(task)) {
        score[TOPO_TASK_CODE] += 2;
        score[TOPO_TASK_REVIEW] += 1;
    }

    if (strcasestr_simple(task, "code") || strcasestr_simple(task, "implement") ||
        strcasestr_simple(task, "bug") || strcasestr_simple(task, "refactor") ||
        strcasestr_simple(task, "build") || strcasestr_simple(task, "compile") ||
        strcasestr_simple(task, "deploy") || strcasestr_simple(task, "ci") ||
        strcasestr_simple(task, "cli") || strcasestr_simple(task, "repo") ||
        strcasestr_simple(task, "codebase")) {
        score[TOPO_TASK_CODE] += 2;
    }
    if (strcasestr_simple(task, "review") || strcasestr_simple(task, "audit") ||
        strcasestr_simple(task, "verify") || strcasestr_simple(task, "validation") ||
        strcasestr_simple(task, "qa")) {
        score[TOPO_TASK_REVIEW] += 2;
    }
    if (strcasestr_simple(task, "research") || strcasestr_simple(task, "investigate") ||
        strcasestr_simple(task, "compare") || strcasestr_simple(task, "gather") ||
        strcasestr_simple(task, "analyze") || strcasestr_simple(task, "analysis")) {
        score[TOPO_TASK_RESEARCH] += 2;
    }
    if (strcasestr_simple(task, "creative") || strcasestr_simple(task, "brainstorm") ||
        strcasestr_simple(task, "campaign") || strcasestr_simple(task, "story") ||
        strcasestr_simple(task, "concept") || strcasestr_simple(task, "design")) {
        score[TOPO_TASK_CREATIVE] += 3;
    }
    if (strcasestr_simple(task, "incident") || strcasestr_simple(task, "outage") ||
        strcasestr_simple(task, "broken") || strcasestr_simple(task, "failure") ||
        strcasestr_simple(task, "degraded") || strcasestr_simple(task, "regression") ||
        strcasestr_simple(task, "hotfix") || strcasestr_simple(task, "triage")) {
        score[TOPO_TASK_INCIDENT] += 3;
    }

    if (score[TOPO_TASK_CREATIVE] == 0 && score[TOPO_TASK_INCIDENT] == 0 &&
        score[TOPO_TASK_REVIEW] == 0 && score[TOPO_TASK_RESEARCH] == 0 &&
        score[TOPO_TASK_CODE] == 0) {
        return TOPO_TASK_GENERAL;
    }

    int best_kind = TOPO_TASK_GENERAL;
    int best_score = 0;
    for (int i = TOPO_TASK_CODE; i <= TOPO_TASK_INCIDENT; i++) {
        if (score[i] > best_score) {
            best_score = score[i];
            best_kind = i;
        }
    }

    return (topology_task_kind_t)best_kind;
}

static int clamp_int(int v, int lo, int hi) {
    if (v < lo)
        return lo;
    if (v > hi)
        return hi;
    return v;
}

static bool is_dynamic_topology_name(const char *name) {
    return name && name[0] &&
           (strcasecmp(name, "dynamic") == 0 || strcasecmp(name, "adaptive") == 0 ||
            strcasecmp(name, "auto_dynamic") == 0);
}

static bool task_mentions_provider_throughput(const char *task) {
    if (!task || !task[0])
        return false;

    bool provider_scope = strcasestr_simple(task, "all providers") ||
                          strcasestr_simple(task, "across providers") ||
                          strcasestr_simple(task, "across all providers") ||
                          strcasestr_simple(task, "provider") ||
                          strcasestr_simple(task, "providers");
    bool throughput_goal = strcasestr_simple(task, "throughput") ||
                           strcasestr_simple(task, "fanout") ||
                           strcasestr_simple(task, "fan-out") ||
                           strcasestr_simple(task, "parallel") ||
                           strcasestr_simple(task, "concurrency") ||
                           strcasestr_simple(task, "maximize") ||
                           strcasestr_simple(task, "maximise");

    return provider_scope && throughput_goal;
}

bool topology_profile_task(const char *task, topology_task_profile_t *profile) {
    if (!profile)
        return false;
    memset(profile, 0, sizeof(*profile));
    profile->kind = TOPO_TASK_GENERAL;
    profile->complexity = 1;
    profile->desired_parallelism = 1;
    if (!task || !task[0])
        return true;

    task_profile_t *tp = task_profile(task, NULL);
    if (!tp) {
        profile->complexity = 2;
        profile->desired_parallelism = 2;
        profile->needs_iteration =
            strcasestr_simple(task, "iterate") || strcasestr_simple(task, "refine") ||
            strcasestr_simple(task, "improve") || strcasestr_simple(task, "polish");
        return true;
    }

    profile->kind = map_task_profile_to_kind(tp, task);

    profile->complexity = 1 + (int)(tp->complexity_score * 4.0 + 0.5);
    if (tp->task_length > 180)
        profile->complexity++;
    if (tp->task_length > 480)
        profile->complexity++;
    if (tp->clause_count > 2)
        profile->complexity++;
    if (tp->keyword_match_count >= 3)
        profile->complexity++;
    profile->complexity = clamp_int(profile->complexity, 1, 5);

    profile->needs_iteration =
        tp->patterns[PATTERN_ITERATION] || tp->convergence_score >= 0.6 ||
        strcasestr_simple(task, "iterate") || strcasestr_simple(task, "refine") ||
        strcasestr_simple(task, "improve") || strcasestr_simple(task, "polish") ||
        profile->kind == TOPO_TASK_REVIEW;

    profile->needs_validation =
        profile->kind == TOPO_TASK_CODE || profile->kind == TOPO_TASK_REVIEW ||
        profile->kind == TOPO_TASK_INCIDENT || tp->patterns[PATTERN_REVIEW] ||
        tp->patterns[PATTERN_ANALYSIS] || strcasestr_simple(task, "verify") ||
        strcasestr_simple(task, "validate") || strcasestr_simple(task, "test");

    profile->prefers_breadth =
        tp->parallelism_score >= 0.45 || tp->patterns[PATTERN_PARALLELISM] ||
        tp->patterns[PATTERN_CONSENSUS] || strcasestr_simple(task, "compare") ||
        strcasestr_simple(task, "parallel") || strcasestr_simple(task, "many") ||
        strcasestr_simple(task, "multiple");

    profile->desired_parallelism = 1;
    if (profile->prefers_breadth)
        profile->desired_parallelism++;
    if (tp->parallelism_score >= 0.4)
        profile->desired_parallelism++;
    if (tp->parallelism_score >= 0.65)
        profile->desired_parallelism++;
    if (profile->complexity >= 3)
        profile->desired_parallelism++;
    if (profile->complexity >= 4)
        profile->desired_parallelism++;
    if (strcasestr_simple(task, "wide") || strcasestr_simple(task, "many")) {
        profile->desired_parallelism++;
    }
    if (profile->kind == TOPO_TASK_INCIDENT && profile->desired_parallelism < 2) {
        profile->desired_parallelism = 2;
    }
    if (profile->kind == TOPO_TASK_CREATIVE && profile->desired_parallelism < 3) {
        profile->desired_parallelism = 3;
    }
    if (task_mentions_self_improvement(task) || task_mentions_large_scale_ai_work(task)) {
        profile->complexity = clamp_int(profile->complexity < 4 ? 4 : profile->complexity, 1, 5);
        profile->needs_iteration = true;
        profile->needs_validation = true;
        profile->prefers_breadth = true;
        if (profile->desired_parallelism < 6)
            profile->desired_parallelism = 6;
    }
    if (task_mentions_provider_throughput(task)) {
        profile->complexity = clamp_int(profile->complexity < 4 ? 4 : profile->complexity, 1, 5);
        profile->needs_validation = true;
        profile->prefers_breadth = true;
        profile->desired_parallelism = 8;
    }
    profile->desired_parallelism = clamp_int(profile->desired_parallelism, 1, 8);

    task_profile_free(tp);

    return true;
}

static void build_dynamic_general(topology_plan_t *plan, const topology_task_profile_t *profile) {
    topology_t *t = &plan->topology;
    topo_init_entry(t, 0, "dynamic_general", "Adaptive delegator-worker-synthesizer plan",
                    CAT_FANOUT, profile->needs_iteration ? EXEC_ITERATIVE : EXEC_PARALLEL_STAGES,
                    profile->needs_iteration ? 2 : 1, 2.5 + (profile->desired_parallelism * 0.3));
    int triage = topo_add_node(t, TIER_HAIKU, ROLE_CLASSIFIER, "triage", 1);
    int plan_node = topo_add_node(t, profile->complexity >= 4 ? TIER_OPUS : TIER_SONNET,
                                  ROLE_DELEGATOR, "plan", 1);
    int work =
        topo_add_node(t, TIER_SONNET, profile->prefers_breadth ? ROLE_SCOUT : ROLE_WORKER,
                      profile->prefers_breadth ? "explore" : "work", profile->desired_parallelism);
    int merge = topo_add_node(t, TIER_SONNET, ROLE_SYNTHESIZER, "merge", 1);
    int final = topo_add_node(t, profile->complexity >= 4 ? TIER_OPUS : TIER_SONNET,
                              profile->needs_validation ? ROLE_VALIDATOR : ROLE_JUDGE,
                              profile->needs_validation ? "validate" : "decide", 1);
    topo_add_edge(t, triage, plan_node, EDGE_SEQUENCE);
    topo_add_edge(t, plan_node, work, EDGE_SEQUENCE);
    topo_add_edge(t, work, merge, EDGE_SEQUENCE);
    topo_add_edge(t, merge, final, EDGE_SEQUENCE);
    if (profile->needs_iteration)
        topo_add_edge(t, final, work, EDGE_FEEDBACK);
    topo_finalize(t);
}

static void build_dynamic_code(topology_plan_t *plan, const topology_task_profile_t *profile) {
    topology_t *t = &plan->topology;
    topo_init_entry(t, 0, "dynamic_code",
                    "Adaptive code swarm with planning, parallel implementation, and validation",
                    CAT_DOMAIN, profile->needs_iteration ? EXEC_ITERATIVE : EXEC_PARALLEL_STAGES,
                    profile->needs_iteration ? 2 : 1, 3.5 + (profile->desired_parallelism * 0.35));
    int triage = topo_add_node(t, TIER_HAIKU, ROLE_CLASSIFIER, "triage", 1);
    int plan_node = topo_add_node(t, profile->complexity >= 4 ? TIER_OPUS : TIER_SONNET,
                                  ROLE_COORDINATOR, "plan", 1);
    int implement =
        topo_add_node(t, TIER_SONNET, ROLE_WORKER, "implement", profile->desired_parallelism);
    int validate = topo_add_node(t, TIER_HAIKU, ROLE_VALIDATOR, "validate", 1);
    int judge = topo_add_node(t, profile->complexity >= 4 ? TIER_OPUS : TIER_SONNET, ROLE_JUDGE,
                              "decide", 1);
    topo_add_edge(t, triage, plan_node, EDGE_SEQUENCE);
    topo_add_edge(t, plan_node, implement, EDGE_SEQUENCE);
    topo_add_edge(t, implement, validate, EDGE_SEQUENCE);
    topo_add_edge(t, validate, judge, EDGE_SEQUENCE);
    if (profile->needs_iteration)
        topo_add_edge(t, validate, implement, EDGE_FEEDBACK);
    topo_finalize(t);
}

static void build_dynamic_research(topology_plan_t *plan, const topology_task_profile_t *profile) {
    topology_t *t = &plan->topology;
    topo_init_entry(t, 0, "dynamic_research",
                    "Adaptive research swarm with broad scouting and dual analysis", CAT_DOMAIN,
                    EXEC_PARALLEL_STAGES, 1, 2.8 + (profile->desired_parallelism * 0.25));
    int plan_node = topo_add_node(t, TIER_SONNET, ROLE_DELEGATOR, "plan", 1);
    int scout = topo_add_node(t, TIER_HAIKU, ROLE_SCOUT, "gather", profile->desired_parallelism);
    int analyze =
        topo_add_node(t, TIER_SONNET, ROLE_SPECIALIST, "analyze", profile->complexity >= 4 ? 2 : 1);
    int synth = topo_add_node(t, profile->complexity >= 4 ? TIER_OPUS : TIER_SONNET,
                              ROLE_SYNTHESIZER, "synthesize", 1);
    topo_add_edge(t, plan_node, scout, EDGE_SEQUENCE);
    topo_add_edge(t, scout, analyze, EDGE_SEQUENCE);
    topo_add_edge(t, analyze, synth, EDGE_SEQUENCE);
    topo_finalize(t);
}

static void build_dynamic_review(topology_plan_t *plan, const topology_task_profile_t *profile) {
    topology_t *t = &plan->topology;
    topo_init_entry(
        t, 0, "dynamic_review", "Adaptive review swarm with broad specialist coverage and verdict",
        CAT_SPECIALIST, EXEC_PARALLEL_STAGES, 1, 2.4 + (profile->desired_parallelism * 0.25));
    int triage = topo_add_node(t, TIER_HAIKU, ROLE_CLASSIFIER, "scope", 1);
    int reviewers =
        topo_add_node(t, TIER_SONNET, ROLE_SPECIALIST, "review", profile->desired_parallelism);
    int judge = topo_add_node(t, profile->complexity >= 4 ? TIER_OPUS : TIER_SONNET, ROLE_JUDGE,
                              "verdict", 1);
    topo_add_edge(t, triage, reviewers, EDGE_SEQUENCE);
    topo_add_edge(t, reviewers, judge, EDGE_SEQUENCE);
    topo_finalize(t);
}

static void build_dynamic_creative(topology_plan_t *plan, const topology_task_profile_t *profile) {
    topology_t *t = &plan->topology;
    topo_init_entry(t, 0, "dynamic_creative",
                    "Adaptive creative swarm with parallel generation, curation, and polish",
                    CAT_DOMAIN, EXEC_CONSENSUS, 1, 2.6 + (profile->desired_parallelism * 0.25));
    int brainstorm =
        topo_add_node(t, TIER_HAIKU, ROLE_GENERATOR, "brainstorm", profile->desired_parallelism);
    int curate = topo_add_node(t, TIER_SONNET, ROLE_JUDGE, "curate", 1);
    int direct = topo_add_node(t, TIER_OPUS, ROLE_COORDINATOR, "direct", 1);
    int polish = topo_add_node(t, TIER_SONNET, ROLE_WORKER, "polish", 1);
    topo_add_edge(t, brainstorm, curate, EDGE_SEQUENCE);
    topo_add_edge(t, curate, direct, EDGE_SEQUENCE);
    topo_add_edge(t, direct, polish, EDGE_SEQUENCE);
    topo_finalize(t);
}

static void build_dynamic_incident(topology_plan_t *plan, const topology_task_profile_t *profile) {
    topology_t *t = &plan->topology;
    topo_init_entry(t, 0, "dynamic_incident",
                    "Adaptive incident swarm with triage, diagnosis, remediation, and verification",
                    CAT_DOMAIN, profile->needs_iteration ? EXEC_ITERATIVE : EXEC_PARALLEL_STAGES,
                    profile->needs_iteration ? 2 : 1, 3.2 + (profile->desired_parallelism * 0.3));
    int triage = topo_add_node(t, TIER_HAIKU, ROLE_CLASSIFIER, "triage", 1);
    int diagnose =
        topo_add_node(t, TIER_SONNET, ROLE_WORKER, "diagnose", profile->desired_parallelism);
    int plan_node = topo_add_node(t, TIER_OPUS, ROLE_COORDINATOR, "plan", 1);
    int fix = topo_add_node(t, TIER_SONNET, ROLE_WORKER, "remediate", 1);
    int verify = topo_add_node(t, TIER_HAIKU, ROLE_VALIDATOR, "verify", 1);
    topo_add_edge(t, triage, diagnose, EDGE_SEQUENCE);
    topo_add_edge(t, diagnose, plan_node, EDGE_SEQUENCE);
    topo_add_edge(t, plan_node, fix, EDGE_SEQUENCE);
    topo_add_edge(t, fix, verify, EDGE_SEQUENCE);
    if (profile->needs_iteration)
        topo_add_edge(t, verify, diagnose, EDGE_FEEDBACK);
    topo_finalize(t);
}

static void build_dynamic_plan(const topology_task_profile_t *profile, const char *task,
                               topology_plan_t *plan) {
    memset(plan, 0, sizeof(*plan));
    plan->is_dynamic = true;
    plan->profile = *profile;

    switch (profile->kind) {
        case TOPO_TASK_CODE:
            build_dynamic_code(plan, profile);
            break;
        case TOPO_TASK_RESEARCH:
            build_dynamic_research(plan, profile);
            break;
        case TOPO_TASK_REVIEW:
            build_dynamic_review(plan, profile);
            break;
        case TOPO_TASK_CREATIVE:
            build_dynamic_creative(plan, profile);
            break;
        case TOPO_TASK_INCIDENT:
            build_dynamic_incident(plan, profile);
            break;
        case TOPO_TASK_GENERAL:
        default:
            build_dynamic_general(plan, profile);
            break;
    }

    snprintf(plan->rationale, sizeof(plan->rationale),
             "dynamic plan: kind=%d complexity=%d parallelism=%d%s%s%s task=\"%.80s\"",
             profile->kind, profile->complexity, profile->desired_parallelism,
             profile->needs_iteration ? " iterative" : "",
             profile->needs_validation ? " validation" : "",
             profile->prefers_breadth ? " breadth" : "", task ? task : "");
}

static bool output_approved(const char *text) {
    if (!text || !text[0])
        return false;
    if (strcasestr_simple(text, "APPROVED: yes"))
        return true;
    if (strcasestr_simple(text, "APPROVED=yes"))
        return true;
    if (strcasestr_simple(text, "APPROVED:true"))
        return true;
    if (strcasestr_simple(text, "approved") && !strcasestr_simple(text, "not approved") &&
        !strcasestr_simple(text, "APPROVED: no"))
        return true;
    return false;
}

static int choose_conditional_target(const topology_t *t, int from_node, const char *output) {
    int first_target = -1;
    if (output) {
        const char *route = NULL;
        for (const char *p = output; *p; p++) {
            if (strncasecmp(p, "ROUTE:", 6) == 0) {
                route = p;
                break;
            }
        }
        if (route) {
            route += 6;
            while (*route && isspace((unsigned char)*route))
                route++;
            char token[TOPO_MAX_TAG];
            size_t i = 0;
            while (route[i] && !isspace((unsigned char)route[i]) && route[i] != ',' &&
                   i + 1 < sizeof(token)) {
                token[i] = route[i];
                i++;
            }
            token[i] = '\0';
            if (token[0]) {
                for (int e = 0; e < t->edge_count; e++) {
                    if (t->edges[e].from != from_node || t->edges[e].type != EDGE_CONDITIONAL)
                        continue;
                    if (first_target < 0)
                        first_target = t->edges[e].to;
                    if (strcasecmp(t->nodes[t->edges[e].to].tag, token) == 0) {
                        return t->edges[e].to;
                    }
                }
            }
        }
    }
    for (int e = 0; e < t->edge_count; e++) {
        if (t->edges[e].from != from_node || t->edges[e].type != EDGE_CONDITIONAL)
            continue;
        if (first_target < 0)
            first_target = t->edges[e].to;
        if (output && strcasestr_simple(output, t->nodes[t->edges[e].to].tag)) {
            return t->edges[e].to;
        }
    }
    return first_target;
}

static void append_trimmed_section(jbuf_t *b, const char *title, const char *text, size_t max_len) {
    jbuf_append(b, title);
    jbuf_append(b, "\n");
    char *trimmed = trim_copy(text ? text : "", max_len);
    jbuf_append(b, trimmed);
    free(trimmed);
    jbuf_append(b, "\n");
}

static char *build_node_prompt(const topology_t *t, const topo_node_t *node, const char *task,
                               char *const current_outputs[], char *const prev_outputs[],
                               int iteration) {
    jbuf_t b;
    jbuf_init(&b, 4096);
    jbuf_append(&b, "You are executing one node inside the dsco topology runtime.\n\n");
    jbuf_append(&b, "Topology: ");
    jbuf_append(&b, t->name);
    jbuf_append(&b, "\nNode tag: ");
    jbuf_append(&b, node->tag);
    jbuf_append(&b, "\nRole: ");
    jbuf_append(&b, node_role_label(node->role));
    jbuf_append(&b, "\nTier: ");
    jbuf_append(&b, tier_label(node->tier));
    jbuf_append(&b, "\nIteration: ");
    jbuf_append_int(&b, iteration + 1);
    jbuf_append(&b, " / ");
    jbuf_append_int(&b, t->max_iterations > 0 ? t->max_iterations : 1);
    jbuf_append(&b, "\n\nOriginal user task:\n");
    append_trimmed_section(&b, "", task, 5000);

    bool wrote_inputs = false;
    for (int e = 0; e < t->edge_count; e++) {
        if (t->edges[e].to != node->id)
            continue;
        if (t->edges[e].type == EDGE_FEEDBACK) {
            if (prev_outputs[t->edges[e].from]) {
                if (!wrote_inputs) {
                    jbuf_append(&b, "Feedback from previous iteration:\n");
                    wrote_inputs = true;
                }
                jbuf_append(&b, "- from ");
                jbuf_append(&b, t->nodes[t->edges[e].from].tag);
                jbuf_append(&b, ":\n");
                append_trimmed_section(&b, "", prev_outputs[t->edges[e].from], 3000);
            }
            continue;
        }
        if (current_outputs[t->edges[e].from]) {
            if (!wrote_inputs) {
                jbuf_append(&b, "Upstream stage outputs:\n");
                wrote_inputs = true;
            }
            jbuf_append(&b, "- from ");
            jbuf_append(&b, t->nodes[t->edges[e].from].tag);
            jbuf_append(&b, ":\n");
            append_trimmed_section(&b, "", current_outputs[t->edges[e].from], 3000);
        }
    }
    if (!wrote_inputs) {
        jbuf_append(&b, "Upstream stage outputs:\n- none\n");
    }

    bool has_conditional_out = false;
    for (int e = 0; e < t->edge_count; e++) {
        if (t->edges[e].from == node->id && t->edges[e].type == EDGE_CONDITIONAL) {
            if (!has_conditional_out) {
                jbuf_append(&b, "\nThis node must route to exactly one downstream target.\nAllowed "
                                "ROUTE values: ");
                has_conditional_out = true;
            } else {
                jbuf_append(&b, ", ");
            }
            jbuf_append(&b, t->nodes[t->edges[e].to].tag);
        }
    }
    if (has_conditional_out) {
        jbuf_append(&b, "\nEnd your response with a line like: ROUTE: <tag>\n");
    }

    if (t->strategy == EXEC_ITERATIVE || node->role == ROLE_CRITIC ||
        node->role == ROLE_VALIDATOR || node->role == ROLE_JUDGE) {
        jbuf_append(&b, "If this stage is evaluating readiness, include a line: APPROVED: yes or "
                        "APPROVED: no\n");
    }

    jbuf_append(&b, "\nReturn only the stage output between the exact markers below.\n");
    jbuf_append(&b, TOPO_STAGE_MARKER_BEGIN);
    jbuf_append(&b, "\n<your stage output>\n");
    jbuf_append(&b, TOPO_STAGE_MARKER_END);
    jbuf_append(&b, "\n");
    return b.data;
}

typedef struct {
    int node_id;
    int replica;
    int child_id;
} stage_child_map_t;

static bool run_stage(const topology_t *t, const int ready_nodes[], int ready_count,
                      const char *api_key, const char *coordinator_model, const char *task,
                      char *current_outputs[], char *prev_outputs[], int iteration,
                      topology_run_stats_t *stats) {
    swarm_t sw;
    swarm_init(&sw, api_key, coordinator_model);

    stage_child_map_t child_map[TOPO_MAX_NODES * 4];
    int child_count = 0;
    bool throughput_mode = topology_throughput_enabled() || task_mentions_provider_throughput(task);

    for (int i = 0; i < ready_count; i++) {
        const topo_node_t *node = &t->nodes[ready_nodes[i]];
        char model_buf[128];
        const char *default_node_model = topology_resolve_model_for_tier(
            coordinator_model, api_key, node->tier, model_buf, sizeof(model_buf));
        for (int rep = 0; rep < node->replicas; rep++) {
            char *prompt =
                build_node_prompt(t, node, task, current_outputs, prev_outputs, iteration);
            if (node->replicas > 1) {
                jbuf_t extra;
                jbuf_init(&extra, strlen(prompt) + 128);
                jbuf_append(&extra, prompt);
                jbuf_append(&extra, "\nReplica: ");
                jbuf_append_int(&extra, rep + 1);
                jbuf_append(&extra, " / ");
                jbuf_append_int(&extra, node->replicas);
                free(prompt);
                prompt = extra.data;
            }

            char lane_provider[64];
            char lane_model[128];
            const char *node_model = default_node_model;
            const char *node_provider = NULL;
            if (throughput_mode &&
                topology_resolve_throughput_lane_for_tier(api_key, node->tier, child_count,
                                                          lane_provider, sizeof(lane_provider),
                                                          lane_model, sizeof(lane_model))) {
                node_provider = lane_provider;
                node_model = lane_model;
            }

            int child_id = node_provider ? swarm_spawn_provider(&sw, -1, prompt, node_model,
                                                                node_provider)
                                         : swarm_spawn(&sw, prompt, node_model);
            free(prompt);
            if (child_id < 0) {
                swarm_destroy(&sw);
                return false;
            }
            child_map[child_count].node_id = node->id;
            child_map[child_count].replica = rep;
            child_map[child_count].child_id = child_id;
            child_count++;
            if (stats)
                stats->agents_spawned++;
        }
    }

    while (swarm_active_count(&sw) > 0) {
        swarm_poll(&sw, 100);
    }

    for (int i = 0; i < ready_count; i++) {
        int node_id = ready_nodes[i];
        jbuf_t combined;
        jbuf_init(&combined, 2048);
        int matches = 0;
        for (int c = 0; c < child_count; c++) {
            if (child_map[c].node_id != node_id)
                continue;
            swarm_child_t *child = swarm_get(&sw, child_map[c].child_id);
            if (!child)
                continue;
            char *normalized = normalize_node_output(child->output ? child->output : "");
            if (matches > 0)
                jbuf_append(&combined, "\n\n");
            if (t->nodes[node_id].replicas > 1) {
                jbuf_append(&combined, "[replica ");
                jbuf_append_int(&combined, child_map[c].replica + 1);
                jbuf_append(&combined, "]\n");
            }
            jbuf_append(&combined, normalized);
            free(normalized);
            matches++;
        }
        free(current_outputs[node_id]);
        current_outputs[node_id] = combined.data;
        if (stats) {
            stats->nodes_executed++;
            snprintf(stats->final_node_tag, sizeof(stats->final_node_tag), "%s",
                     t->nodes[node_id].tag);
        }
    }

    swarm_destroy(&sw);
    return true;
}

bool topology_run(const topology_t *t, const char *api_key, const char *coordinator_model,
                  const char *task, char *result, size_t rlen, topology_run_stats_t *stats) {
    if (!result || rlen == 0)
        return false;
    result[0] = '\0';
    if (stats)
        memset(stats, 0, sizeof(*stats));
    if (!t || !topology_is_runnable(t) || !task || !task[0]) {
        snprintf(result, rlen, "error: invalid topology run request");
        return false;
    }

    char *prev_outputs[TOPO_MAX_NODES] = {0};
    char *current_outputs[TOPO_MAX_NODES] = {0};
    int max_iterations =
        (t->strategy == EXEC_ITERATIVE && t->max_iterations > 1) ? t->max_iterations : 1;
    bool ok = true;

    fprintf(stderr, "  %s⚡%s topology \"%s\" — %d nodes\n", TUI_BYELLOW, TUI_RESET, t->name,
            t->node_count);

    for (int iteration = 0; iteration < max_iterations && ok; iteration++) {
        bool completed[TOPO_MAX_NODES] = {0};
        bool activated[TOPO_MAX_NODES] = {0};
        int completed_count = 0;

        for (int i = 0; i < t->node_count; i++) {
            activated[i] = !has_incoming_type(t, i, EDGE_CONDITIONAL);
            free(current_outputs[i]);
            current_outputs[i] = NULL;
        }

        fprintf(stderr, "  %s├─%s iteration %d/%d\n", TUI_DIM, TUI_RESET, iteration + 1,
                max_iterations);

        while (completed_count < t->node_count) {
            int ready[TOPO_MAX_NODES];
            int ready_count = 0;

            for (int n = 0; n < t->node_count; n++) {
                if (completed[n] || !activated[n])
                    continue;
                bool deps_met = true;
                for (int e = 0; e < t->edge_count; e++) {
                    if (t->edges[e].to != n || t->edges[e].type == EDGE_FEEDBACK)
                        continue;
                    if (!completed[t->edges[e].from]) {
                        deps_met = false;
                        break;
                    }
                }
                if (deps_met)
                    ready[ready_count++] = n;
            }

            if (ready_count == 0)
                break;

            fprintf(stderr, "  %s│%s stage:", TUI_DIM, TUI_RESET);
            for (int i = 0; i < ready_count; i++) {
                fprintf(stderr, " %s%s%s", TUI_CYAN, t->nodes[ready[i]].tag, TUI_RESET);
            }
            fprintf(stderr, "\n");

            ok = run_stage(t, ready, ready_count, api_key, coordinator_model, task, current_outputs,
                           prev_outputs, iteration, stats);
            if (!ok)
                break;

            for (int i = 0; i < ready_count; i++) {
                int node_id = ready[i];
                completed[node_id] = true;
                completed_count++;

                int chosen = choose_conditional_target(t, node_id, current_outputs[node_id]);
                for (int e = 0; e < t->edge_count; e++) {
                    if (t->edges[e].from != node_id || t->edges[e].type != EDGE_CONDITIONAL)
                        continue;
                    activated[t->edges[e].to] = (t->edges[e].to == chosen);
                }
            }
        }

        if (stats)
            stats->iterations = iteration + 1;

        if (iteration + 1 < max_iterations && has_feedback_edges(t)) {
            bool approved = false;
            for (int i = 0; i < t->node_count; i++) {
                if (current_outputs[i] && output_approved(current_outputs[i])) {
                    approved = true;
                    break;
                }
            }
            if (approved)
                break;
        }

        for (int i = 0; i < t->node_count; i++) {
            free(prev_outputs[i]);
            prev_outputs[i] = current_outputs[i] ? safe_strdup(current_outputs[i]) : NULL;
        }
    }

    if (ok) {
        jbuf_t out;
        jbuf_init(&out, 8192);
        int sinks = 0;
        for (int i = 0; i < t->node_count; i++) {
            if (!node_is_sink(t, i) || !current_outputs[i])
                continue;
            if (sinks++ > 0)
                jbuf_append(&out, "\n\n");
            if (sinks > 1) {
                jbuf_append(&out, "[");
                jbuf_append(&out, t->nodes[i].tag);
                jbuf_append(&out, "]\n");
            }
            jbuf_append(&out, current_outputs[i]);
        }
        if (sinks == 0) {
            jbuf_append(&out, "topology finished without sink output");
        }
        int written = (int)(out.len < rlen - 1 ? out.len : rlen - 1);
        memcpy(result, out.data, (size_t)written);
        result[written] = '\0';
        if (stats)
            stats->est_cost_usd =
                topology_estimate_cost(t, (int)strlen(task) / 4, (int)strlen(result) / 4);
        /* Teach the cost model from this execution */
        if (stats && stats->est_cost_usd > 0) {
            int total_toks = (int)(strlen(task) / 4 + strlen(result) / 4);
            cost_model_learn(t->name, total_toks, stats->est_cost_usd, 0.0);
            /* Cache successful topology selection */
            plan_cache_store(task, t->name, "topology_run success", 0.85f);
        }

        jbuf_free(&out);
    } else {
        snprintf(result, rlen, "error: topology execution failed");
    }

    for (int i = 0; i < t->node_count; i++) {
        free(prev_outputs[i]);
        free(current_outputs[i]);
    }

    fprintf(stderr, "  %s└─%s topology \"%s\" %s\n", ok ? TUI_GREEN : TUI_BRED, TUI_RESET, t->name,
            ok ? "complete" : "failed");
    return ok;
}

/* ── Scheduler-integrated topology execution ────────────────────────── */

typedef struct {
    topology_plan_t *plan;
    int node_idx;
    const char *api_key;
    const char *model;
    const char *task;
    char **current_outputs;
    char **prev_outputs;
    int iteration;
    topology_run_stats_t *stats;
    char *output;
    size_t output_len;
    bool success;
} topo_task_ctx_t;

/*
 * Task adapter: executes a single topology node through run_stage.
 * Wraps the existing run_stage logic for a single-node ready set so
 * the scheduler can drive each node independently.
 * Returns 0 on completion (success or failure captured in ctx->success).
 */
static int topo_node_task(void *ctx) {
    topo_task_ctx_t *tc = ctx;
    const topology_t *t = &tc->plan->topology;
    int ready[1] = {tc->node_idx};

    tc->success = run_stage(t, ready, 1, tc->api_key, tc->model, tc->task, tc->current_outputs,
                            tc->prev_outputs, tc->iteration, tc->stats);

    /* Capture the output for the caller */
    if (tc->success && tc->current_outputs[tc->node_idx]) {
        size_t len = strlen(tc->current_outputs[tc->node_idx]);
        if (len > tc->output_len - 1)
            len = tc->output_len - 1;
        if (tc->output) {
            memcpy(tc->output, tc->current_outputs[tc->node_idx], len);
            tc->output[len] = '\0';
        }
    }

    return 0; /* 0 = completed to the scheduler */
}

/*
 * run_stage_scheduled — scheduler-driven version of a stage.
 * When the scheduler is available, each ready node is spawned as an
 * independent scheduler task so they interleave cooperatively.
 * Falls back to the original run_stage when no scheduler is set.
 */
static bool run_stage_scheduled(const topology_t *t, const int ready_nodes[], int ready_count,
                                const char *api_key, const char *coordinator_model,
                                const char *task, char *current_outputs[], char *prev_outputs[],
                                int iteration, topology_run_stats_t *stats, topology_plan_t *plan) {
    /* If only one ready node or no scheduler, use the original path */
    if (!g_topo_sched || ready_count <= 1) {
        return run_stage(t, ready_nodes, ready_count, api_key, coordinator_model, task,
                         current_outputs, prev_outputs, iteration, stats);
    }

    /* Spawn each ready node as a scheduler task */
    topo_task_ctx_t ctxs[TOPO_MAX_NODES];
    task_id_t ids[TOPO_MAX_NODES];
    char labels[TOPO_MAX_NODES][64];
    memset(ctxs, 0, sizeof(ctxs));

    for (int i = 0; i < ready_count; i++) {
        topo_task_ctx_t *tc = &ctxs[i];
        tc->plan = plan;
        tc->node_idx = ready_nodes[i];
        tc->api_key = api_key;
        tc->model = coordinator_model;
        tc->task = task;
        tc->current_outputs = current_outputs;
        tc->prev_outputs = prev_outputs;
        tc->iteration = iteration;
        tc->stats = stats;
        tc->output = NULL;
        tc->output_len = 0;
        tc->success = false;

        snprintf(labels[i], sizeof(labels[i]), "topo:%s/%s", t->name, t->nodes[ready_nodes[i]].tag);

        ids[i] = sched_spawn(g_topo_sched, topo_node_task, tc, labels[i], SCHED_PRIO_NORMAL);
        if (ids[i] == TASK_INVALID) {
            /* Could not spawn — cancel previously spawned and fall back */
            for (int j = 0; j < i; j++)
                sched_cancel(g_topo_sched, ids[j]);
            return run_stage(t, ready_nodes, ready_count, api_key, coordinator_model, task,
                             current_outputs, prev_outputs, iteration, stats);
        }
    }

    /* Drive the scheduler until all our tasks finish */
    bool all_done = false;
    while (!all_done) {
        sched_tick(g_topo_sched);
        all_done = true;
        for (int i = 0; i < ready_count; i++) {
            sched_task_t *st = sched_task_get(g_topo_sched, ids[i]);
            if (!st)
                continue;
            if (st->state != TASK_COMPLETED && st->state != TASK_FAILED &&
                st->state != TASK_CANCELLED) {
                all_done = false;
                break;
            }
        }
    }

    /* Check results */
    for (int i = 0; i < ready_count; i++) {
        if (!ctxs[i].success)
            return false;
    }
    return true;
}

/*
 * topology_run_scheduled — public entry point that uses the cooperative
 * scheduler for parallel node execution when available.
 *
 * Mirrors topology_run but replaces run_stage calls with
 * run_stage_scheduled so fan-out nodes execute as concurrent
 * scheduler tasks.
 */
bool topology_run_scheduled(const topology_t *t, const char *api_key, const char *coordinator_model,
                            const char *task, char *result, size_t rlen,
                            topology_run_stats_t *stats) {
    if (!result || rlen == 0)
        return false;
    result[0] = '\0';
    if (stats)
        memset(stats, 0, sizeof(*stats));
    if (!t || !topology_is_runnable(t) || !task || !task[0]) {
        snprintf(result, rlen, "error: invalid topology run request");
        return false;
    }

    /* If no scheduler is set, delegate to the original path */
    if (!g_topo_sched) {
        return topology_run(t, api_key, coordinator_model, task, result, rlen, stats);
    }

    /* Build a mutable plan wrapper so the task adapter can reference it */
    topology_plan_t plan;
    memset(&plan, 0, sizeof(plan));
    plan.topology = *t;

    char *prev_outputs[TOPO_MAX_NODES] = {0};
    char *current_outputs[TOPO_MAX_NODES] = {0};
    int max_iterations =
        (t->strategy == EXEC_ITERATIVE && t->max_iterations > 1) ? t->max_iterations : 1;
    bool ok = true;

    fprintf(stderr, "  %s⚡%s topology \"%s\" — %d nodes (scheduled)\n", TUI_BYELLOW, TUI_RESET,
            t->name, t->node_count);

    for (int iteration = 0; iteration < max_iterations && ok; iteration++) {
        bool completed[TOPO_MAX_NODES] = {0};
        bool activated[TOPO_MAX_NODES] = {0};
        int completed_count = 0;

        for (int i = 0; i < t->node_count; i++) {
            activated[i] = !has_incoming_type(t, i, EDGE_CONDITIONAL);
            free(current_outputs[i]);
            current_outputs[i] = NULL;
        }

        fprintf(stderr, "  %s├─%s iteration %d/%d\n", TUI_DIM, TUI_RESET, iteration + 1,
                max_iterations);

        while (completed_count < t->node_count) {
            int ready[TOPO_MAX_NODES];
            int ready_count = 0;

            for (int n = 0; n < t->node_count; n++) {
                if (completed[n] || !activated[n])
                    continue;
                bool deps_met = true;
                for (int e = 0; e < t->edge_count; e++) {
                    if (t->edges[e].to != n || t->edges[e].type == EDGE_FEEDBACK)
                        continue;
                    if (!completed[t->edges[e].from]) {
                        deps_met = false;
                        break;
                    }
                }
                if (deps_met)
                    ready[ready_count++] = n;
            }

            if (ready_count == 0)
                break;

            fprintf(stderr, "  %s│%s stage [sched]:", TUI_DIM, TUI_RESET);
            for (int i = 0; i < ready_count; i++) {
                fprintf(stderr, " %s%s%s", TUI_CYAN, t->nodes[ready[i]].tag, TUI_RESET);
            }
            fprintf(stderr, "\n");

            ok = run_stage_scheduled(t, ready, ready_count, api_key, coordinator_model, task,
                                     current_outputs, prev_outputs, iteration, stats, &plan);
            if (!ok)
                break;

            for (int i = 0; i < ready_count; i++) {
                int node_id = ready[i];
                completed[node_id] = true;
                completed_count++;

                int chosen = choose_conditional_target(t, node_id, current_outputs[node_id]);
                for (int e = 0; e < t->edge_count; e++) {
                    if (t->edges[e].from != node_id || t->edges[e].type != EDGE_CONDITIONAL)
                        continue;
                    activated[t->edges[e].to] = (t->edges[e].to == chosen);
                }
            }
        }

        if (stats)
            stats->iterations = iteration + 1;

        if (iteration + 1 < max_iterations && has_feedback_edges(t)) {
            bool approved = false;
            for (int i = 0; i < t->node_count; i++) {
                if (current_outputs[i] && output_approved(current_outputs[i])) {
                    approved = true;
                    break;
                }
            }
            if (approved)
                break;
        }

        for (int i = 0; i < t->node_count; i++) {
            free(prev_outputs[i]);
            prev_outputs[i] = current_outputs[i] ? safe_strdup(current_outputs[i]) : NULL;
        }
    }

    if (ok) {
        jbuf_t out;
        jbuf_init(&out, 8192);
        int sinks = 0;
        for (int i = 0; i < t->node_count; i++) {
            if (!node_is_sink(t, i) || !current_outputs[i])
                continue;
            if (sinks++ > 0)
                jbuf_append(&out, "\n\n");
            if (sinks > 1) {
                jbuf_append(&out, "[");
                jbuf_append(&out, t->nodes[i].tag);
                jbuf_append(&out, "]\n");
            }
            jbuf_append(&out, current_outputs[i]);
        }
        if (sinks == 0) {
            jbuf_append(&out, "topology finished without sink output");
        }
        int written = (int)(out.len < rlen - 1 ? out.len : rlen - 1);
        memcpy(result, out.data, (size_t)written);
        result[written] = '\0';
        if (stats)
            stats->est_cost_usd =
                topology_estimate_cost(t, (int)strlen(task) / 4, (int)strlen(result) / 4);
        /* Teach the cost model from this execution */
        if (stats && stats->est_cost_usd > 0) {
            int total_toks = (int)(strlen(task) / 4 + strlen(result) / 4);
            cost_model_learn(t->name, total_toks, stats->est_cost_usd, 0.0);
            /* Cache successful topology selection */
            plan_cache_store(task, t->name, "topology_run success", 0.85f);
        }

        jbuf_free(&out);
    } else {
        snprintf(result, rlen, "error: topology execution failed");
    }

    for (int i = 0; i < t->node_count; i++) {
        free(prev_outputs[i]);
        free(current_outputs[i]);
    }

    fprintf(stderr, "  %s└─%s topology \"%s\" %s\n", ok ? TUI_GREEN : TUI_BRED, TUI_RESET, t->name,
            ok ? "complete" : "failed");
    return ok;
}

bool topology_plan_run_scheduled(const topology_plan_t *plan, const char *api_key,
                                 const char *coordinator_model, const char *task, char *result,
                                 size_t rlen, topology_run_stats_t *stats) {
    if (!plan)
        return false;
    return topology_run_scheduled(&plan->topology, api_key, coordinator_model, task, result, rlen,
                                  stats);
}
