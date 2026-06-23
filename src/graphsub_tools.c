/* graphsub_tools.c — Tool dispatch for GraphSub operations
 *
 * Registers these tools in the dsco-cli tool table:
 *   graphsub_query      — traverse the graph
 *   graphsub_register    — register agent on GraphSub
 *   graphsub_pheromone   — deposit a pheromone signal
 *   graphsub_pheromone_query — read pheromones for a target
 *   graphsub_memory_sync  — sync episodic memory to GraphSub
 *   graphsub_swarm       — register a swarm topology
 *   graphsub_fleet       — register bridge fleet peers
 *   graphsub_status      — check GraphSub connection + health
 *
 * All tools accept JSON input and return JSON results.
 */

#include "tools.h"
#include "graphsub_client.h"
#include "json_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

/* ── Helper: get agent ID ───────────────────────────────────────────────── */

static void get_agent_id(char *buf, size_t buflen) {
    const char *env_id = getenv("DSCO_AGENT_ID");
    if (env_id && *env_id) {
        strncpy(buf, env_id, buflen - 1);
        buf[buflen - 1] = '\0';
        return;
    }
    char hostname[128] = {0};
    gethostname(hostname, sizeof(hostname) - 1);
    snprintf(buf, buflen, "dsco-%s-%d", hostname, (int)getpid());
}

/* ── Tool: graphsub_status ───────────────────────────────────────────────── */

static bool tool_graphsub_status(const char *input, char *result, size_t rlen) {
    (void)input;
    const char *host = graphsub_host();
    uint16_t port = graphsub_port();

    bool avail = graphsub_is_available();
    char agent_id[256];
    get_agent_id(agent_id, sizeof(agent_id));

    snprintf(result, rlen,
             "{\"host\":\"%s\",\"port\":%u,\"available\":%s,"
             "\"agent_id\":\"%s\",\"tenant\":\"%s\"}",
             host, port, avail ? "true" : "false", agent_id, graphsub_tenant_id());
    return avail;
}

/* ── Tool: graphsub_register ─────────────────────────────────────────────── */

static bool tool_graphsub_register(const char *input, char *result, size_t rlen) {
    char *model = json_get_str(input, "model");
    char *capabilities = json_get_str(input, "capabilities");
    char *tools_list = json_get_str(input, "tools");
    char *hostname = json_get_str(input, "host");
    char *bridge_peer = json_get_str(input, "bridge_peer");
    char agent_id[256];
    get_agent_id(agent_id, sizeof(agent_id));

    char *resp =
        graphsub_agent_register(agent_id, model ? model : "unknown",
                                capabilities ? capabilities : "[\"code\",\"bash\",\"file\"]",
                                tools_list ? tools_list : "[\"bash\",\"read_file\",\"write_file\"]",
                                hostname ? hostname : "localhost", "2.0", bridge_peer);

    if (resp) {
        snprintf(result, rlen, "%s", resp);
        free(resp);
    } else {
        snprintf(result, rlen, "{\"error\":\"failed to register on GraphSub\"}");
    }

    free(model);
    free(capabilities);
    free(tools_list);
    free(hostname);
    free(bridge_peer);
    return resp != NULL;
}

/* ── Tool: graphsub_pheromone ────────────────────────────────────────────── */

static bool tool_graphsub_pheromone(const char *input, char *result, size_t rlen) {
    char *action = json_get_str(input, "action"); /* deposit or query, default deposit */
    if (!action)
        action = strdup("deposit");

    char agent_id[256];
    get_agent_id(agent_id, sizeof(agent_id));

    if (strcmp(action, "deposit") == 0) {
        char *target_type = json_get_str(input, "target_type");
        char *target_id = json_get_str(input, "target_id");
        char *signal_type = json_get_str(input, "signal_type");
        double intensity = json_get_double(input, "intensity", 0.5);
        int ttl = json_get_int(input, "ttl_seconds", 300);
        char *metadata = json_get_str(input, "metadata");

        char *resp = graphsub_pheromone_deposit(
            agent_id, target_type ? target_type : "task", target_id ? target_id : "",
            signal_type ? signal_type : "progress", intensity, ttl, metadata);

        if (resp) {
            snprintf(result, rlen, "%s", resp);
            free(resp);
        } else
            snprintf(result, rlen, "{\"error\":\"deposit failed\"}");

        free(target_type);
        free(target_id);
        free(signal_type);
        free(metadata);
    } else if (strcmp(action, "query") == 0) {
        char *target_id = json_get_str(input, "target_id");
        char *signal_type = json_get_str(input, "signal_type");
        double min_intensity = json_get_double(input, "min_intensity", 0.0);

        char *resp =
            graphsub_pheromone_query(target_id ? target_id : "", signal_type, /* may be NULL */
                                     min_intensity);

        if (resp) {
            snprintf(result, rlen, "%s", resp);
            free(resp);
        } else
            snprintf(result, rlen, "{\"error\":\"query failed\"}");

        free(target_id);
        free(signal_type);
    } else if (strcmp(action, "sweep") == 0) {
        char *target_type = json_get_str(input, "target_type");
        char *signal_type = json_get_str(input, "signal_type");

        char *resp = graphsub_pheromone_sweep(target_type,  /* may be NULL */
                                              signal_type); /* may be NULL */

        if (resp) {
            snprintf(result, rlen, "%s", resp);
            free(resp);
        } else
            snprintf(result, rlen, "{\"error\":\"sweep failed\"}");

        free(target_type);
        free(signal_type);
    } else {
        snprintf(result, rlen, "{\"error\":\"unknown action: %s (use deposit|query|sweep)\"}",
                 action);
    }

    free(action);
    return true;
}

/* ── Tool: graphsub_query (graph traversal) ──────────────────────────────── */

static bool tool_graphsub_query(const char *input, char *result, size_t rlen) {
    char *start = json_get_str(input, "start");
    int depth = json_get_int(input, "depth", 3);
    char *edge_filter = json_get_str(input, "edge_filter");
    int limit = json_get_int(input, "limit", 100);

    char *resp = graphsub_graph_traverse(start ? start : "", depth, edge_filter, /* may be NULL */
                                         limit);

    if (resp) {
        snprintf(result, rlen, "%s", resp);
        free(resp);
        return true;
    } else {
        snprintf(result, rlen, "{\"error\":\"traverse failed\"}");
        return false;
    }

    free(start);
    free(edge_filter);
}

/* ── Tool: graphsub_memory_sync ──────────────────────────────────────────── */

static bool tool_graphsub_memory_sync(const char *input, char *result, size_t rlen) {
    char *episodes = json_get_str(input, "episodes");
    char *type = json_get_str(input, "type");
    char agent_id[256];
    get_agent_id(agent_id, sizeof(agent_id));

    char *resp =
        graphsub_memory_sync(agent_id, episodes ? episodes : "[]", type ? type : "episodic");

    if (resp) {
        snprintf(result, rlen, "%s", resp);
        free(resp);
        return true;
    } else {
        snprintf(result, rlen, "{\"error\":\"memory sync failed\"}");
        return false;
    }

    free(episodes);
    free(type);
}

/* ── Tool: graphsub_swarm ────────────────────────────────────────────────── */

static bool tool_graphsub_swarm(const char *input, char *result, size_t rlen) {
    char *swarm_id = json_get_str(input, "swarm_id");
    char *topology = json_get_str(input, "topology");
    char *agents = json_get_str(input, "agents");
    char *task = json_get_str(input, "task");

    /* Auto-generate swarm_id if not provided */
    char auto_id[64];
    if (!swarm_id) {
        snprintf(auto_id, sizeof(auto_id), "swarm-%ld", (long)time(NULL));
        swarm_id = auto_id;
    }

    char *resp = graphsub_swarm_register(swarm_id, topology ? topology : "fanout",
                                         agents ? agents : "[]", task ? task : "");

    if (resp) {
        snprintf(result, rlen, "%s", resp);
        free(resp);
        return true;
    } else {
        snprintf(result, rlen, "{\"error\":\"swarm register failed\"}");
        return false;
    }

    if (swarm_id != auto_id)
        free(swarm_id);
    free(topology);
    free(agents);
    free(task);
}

/* ── Tool: graphsub_fleet ────────────────────────────────────────────────── */

static bool tool_graphsub_fleet(const char *input, char *result, size_t rlen) {
    char *peers = json_get_str(input, "peers");

    char *resp = graphsub_bridge_fleet(peers ? peers : "[]");

    if (resp) {
        snprintf(result, rlen, "%s", resp);
        free(resp);
        return true;
    } else {
        snprintf(result, rlen, "{\"error\":\"fleet registration failed\"}");
        return false;
    }

    free(peers);
}

/* ── Tool registration ───────────────────────────────────────────────────── */

/* This function is called from tools_init() to add GraphSub tools.
 * Returns the number of tools added. */
int graphsub_tools_register(void) {
    /* These tools are registered dynamically by appending to the tool table.
     * In the current architecture, tools are static in tools.c's s_tools[].
     * For now, we expose them via the "graphsub" action dispatch tool. */
    return 8; /* number of available graphsub operations */
}

/* ── Unified "graphsub" dispatch tool ────────────────────────────────────── */
/* This provides a single entry point: { "action": "register|pheromone|query|..." } */

bool tool_graphsub_dispatch(const char *input, char *result, size_t rlen) {
    char *action = json_get_str(input, "action");
    if (!action) {
        snprintf(result, rlen,
                 "{\"error\":\"action required: "
                 "register|pheromone|query|memory_sync|swarm|fleet|status\"}");
        return false;
    }

    bool ok = false;
    if (strcmp(action, "status") == 0) {
        ok = tool_graphsub_status(input, result, rlen);
    } else if (strcmp(action, "register") == 0) {
        ok = tool_graphsub_register(input, result, rlen);
    } else if (strcmp(action, "pheromone") == 0) {
        ok = tool_graphsub_pheromone(input, result, rlen);
    } else if (strcmp(action, "query") == 0) {
        ok = tool_graphsub_query(input, result, rlen);
    } else if (strcmp(action, "memory_sync") == 0) {
        ok = tool_graphsub_memory_sync(input, result, rlen);
    } else if (strcmp(action, "swarm") == 0) {
        ok = tool_graphsub_swarm(input, result, rlen);
    } else if (strcmp(action, "fleet") == 0) {
        ok = tool_graphsub_fleet(input, result, rlen);
    } else {
        snprintf(result, rlen, "{\"error\":\"unknown action: %s\"}", action);
    }

    free(action);
    return ok;
}
