/* graphsub_client.c — GraphSub HTTP client for dsco-cli
 *
 * Connects the local edge agent to the GraphSub substrate for:
 *   agent registration, heartbeat, pheromone coordination, graph traversal,
 *   memory sync, swarm topology, tool result logging, fleet registration.
 *
 * Uses netsrv_client_post for TLS-capable HTTP. Falls back to plain HTTP
 * for localhost connections.
 *
 * Config: GRAPHSUB_HOST (default http://localhost:7879)
 *         GRAPHSUB_TENANT_ID (default "local")
 *         GRAPHSUB_API_KEY (optional)
 */

#include "graphsub_client.h"
#include "net_server.h"
#include "json_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <errno.h>

#if defined(HAVE_MBEDTLS) && defined(HAVE_LIBSODIUM)
#  include "net_server.h"
#endif

/* ── Config resolution ──────────────────────────────────────────────────── */

#define DEFAULT_HOST "localhost"
#define DEFAULT_PORT 7879
#define MAX_URL 512

static char  s_host[128] = {0};
static uint16_t s_port = 0;
static char  s_tenant[64] = "local";

static void resolve_config(void) {
    if (s_host[0]) return;  /* already resolved */

    const char *env = getenv("GRAPHSUB_HOST");
    if (env && *env) {
        /* Parse http://host:port or host:port or just host */
        const char *p = env;
        if (strncmp(p, "http://", 7) == 0) p += 7;
        else if (strncmp(p, "https://", 8) == 0) p += 8;

        /* Extract host (up to : or / or end) */
        const char *colon = strchr(p, ':');
        const char *slash = strchr(p, '/');
        if (colon && (!slash || colon < slash)) {
            size_t hlen = (size_t)(colon - p);
            if (hlen >= sizeof(s_host)) hlen = sizeof(s_host) - 1;
            memcpy(s_host, p, hlen);
            s_host[hlen] = '\0';
            s_port = (uint16_t)atoi(colon + 1);
        } else {
            size_t hlen = (slash ? (size_t)(slash - p) : strlen(p));
            if (hlen >= sizeof(s_host)) hlen = sizeof(s_host) - 1;
            memcpy(s_host, p, hlen);
            s_host[hlen] = '\0';
            s_port = DEFAULT_PORT;
        }
    }

    if (!s_host[0]) {
        strncpy(s_host, DEFAULT_HOST, sizeof(s_host) - 1);
        s_port = DEFAULT_PORT;
    }

    const char *tenant = getenv("GRAPHSUB_TENANT_ID");
    if (tenant && *tenant) {
        strncpy(s_tenant, tenant, sizeof(s_tenant) - 1);
    }
}

const char *graphsub_host(void) { resolve_config(); return s_host; }
uint16_t   graphsub_port(void) { resolve_config(); return s_port; }
const char *graphsub_tenant_id(void) { resolve_config(); return s_tenant; }

bool graphsub_is_available(void) {
    resolve_config();
#if defined(HAVE_MBEDTLS) && defined(HAVE_LIBSODIUM)
    char *resp = netsrv_client_post(s_host, s_port, "/health", "{}", NULL, 0, false);
    if (resp) { free(resp); return true; }
    return false;
#else
    /* Without TLS libs, try via curl */
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "curl -sk --max-time 2 http://%s:%u/health >/dev/null 2>&1",
             s_host, s_port);
    return system(cmd) == 0;
#endif
}

/* ── Low-level HTTP ──────────────────────────────────────────────────────── */

char *graphsub_post(const char *path, const char *json_body) {
    resolve_config();
    if (!json_body) json_body = "{}";

#if defined(HAVE_MBEDTLS) && defined(HAVE_LIBSODIUM)
    /* Check if localhost — use plain HTTP */
    bool use_tls = (strcmp(s_host, "localhost") != 0 && strcmp(s_host, "127.0.0.1") != 0);
    return netsrv_client_post(s_host, s_port, path, json_body, NULL, 0, use_tls);
#else
    /* Fallback: use curl via popen */
    char cmd[1024];
    char *escaped_body = NULL;

    /* Write body to temp file to avoid shell escaping issues */
    char tmpl[] = "/tmp/dsco-gs-XXXXXX";
    int fd = mkstemp(tmpl);
    if (fd < 0) return NULL;
    if (json_body) {
        size_t blen = strlen(json_body);
        write(fd, json_body, blen);
    }
    close(fd);

    snprintf(cmd, sizeof(cmd),
        "curl -sk --max-time 10 -X POST http://%s:%u%s -H 'Content-Type: application/json' -d @%s 2>/dev/null",
        s_host, s_port, path, mtmpl);
    FILE *fp = popen(cmd, "r");
    if (!fp) { unlink(mtmpl); return NULL; }

    /* Read response */
    size_t cap = 4096, len = 0;
    char *buf = malloc(cap);
    if (!buf) { pclose(fp); unlink(mtmpl); return NULL; }
    size_t n;
    while ((n = fread(buf + len, 1, cap - len, fp)) > 0) {
        len += n;
        if (len >= cap) {
            cap *= 2;
            char *nb = realloc(buf, cap);
            if (!nb) { free(buf); pclose(fp); unlink(mtmpl); return NULL; }
            buf = nb;
        }
    }
    buf[len] = '\0';
    pclose(fp);
    unlink(mtmpl);

    if (len == 0) { free(buf); return NULL; }
    return buf;
#endif
}

char *graphsub_get(const char *path) {
    resolve_config();

#if defined(HAVE_MBEDTLS) && defined(HAVE_LIBSODIUM)
    /* For GET, we can use POST with empty body as workaround */
    return netsrv_client_post(s_host, s_port, path, "", NULL, 0, false);
#else
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
        "curl -sk --max-time 10 http://%s:%u%s 2>/dev/null",
        s_host, s_port, path);
    FILE *fp = popen(cmd, "r");
    if (!fp) return NULL;
    size_t cap = 4096, len = 0;
    char *buf = malloc(cap);
    if (!buf) { pclose(fp); return NULL; }
    size_t n;
    while ((n = fread(buf + len, 1, cap - len, fp)) > 0) {
        len += n;
        if (len >= cap) {
            cap *= 2;
            char *nb = realloc(buf, cap);
            if (!nb) { free(buf); pclose(fp); return NULL; }
            buf = nb;
        }
    }
    buf[len] = '\0';
    pclose(fp);
    if (len == 0) { free(buf); return NULL; }
    return buf;
#endif
}

/* ── Agent Lifecycle ────────────────────────────────────────────────────── */

char *graphsub_agent_register(const char *agent_id,
                               const char *model,
                               const char *capabilities_json,
                               const char *tools_json,
                               const char *hostname,
                               const char *version,
                               const char *bridge_peer) {
    char body[4096];
    int n = snprintf(body, sizeof(body),
        "{\"id\":\"%s\","
        "\"model\":\"%s\","
        "\"capabilities\":%s,"
        "\"tools\":%s,"
        "\"host\":\"%s\","
        "\"version\":\"%s\","
        "\"tenant_id\":\"%s\""
        "%s%s%s}",
        agent_id ? agent_id : "",
        model ? model : "",
        capabilities_json ? capabilities_json : "[]",
        tools_json ? tools_json : "[]",
        hostname ? hostname : "",
        version ? version : "",
        s_tenant,
        bridge_peer ? ",\"bridge_peer\":\"" : "",
        bridge_peer ? bridge_peer : "",
        bridge_peer ? "\"" : "");
    if (n < 0 || (size_t)n >= sizeof(body)) return NULL;
    return graphsub_post("/agent/register", body);
}

char *graphsub_agent_heartbeat(const char *agent_id,
                                const char *status,
                                double load,
                                int memory_used_mb,
                                int active_agents,
                                int uptime_seconds) {
    char body[512];
    snprintf(body, sizeof(body),
        "{\"id\":\"%s\","
        "\"status\":\"%s\","
        "\"load\":%.2f,"
        "\"memory_used_mb\":%d,"
        "\"active_agents\":%d,"
        "\"uptime_seconds\":%d}",
        agent_id ? agent_id : "",
        status ? status : "idle",
        load,
        memory_used_mb,
        active_agents,
        uptime_seconds);
    return graphsub_post("/agent/heartbeat", body);
}

char *graphsub_agent_deregister(const char *agent_id) {
    char body[256];
    snprintf(body, sizeof(body), "{\"id\":\"%s\"}", agent_id ? agent_id : "");
    return graphsub_post("/agent/deregister", body);
}

/* ── Pheromones ─────────────────────────────────────────────────────────── */

char *graphsub_pheromone_deposit(const char *agent_id,
                                  const char *target_type,
                                  const char *target_id,
                                  const char *signal_type,
                                  double intensity,
                                  int ttl_seconds,
                                  const char *metadata_json) {
    char body[1024];
    snprintf(body, sizeof(body),
        "{\"agent_id\":\"%s\","
        "\"target_type\":\"%s\","
        "\"target_id\":\"%s\","
        "\"signal_type\":\"%s\","
        "\"intensity\":%.3f,"
        "\"ttl_seconds\":%d"
        "%s%s%s}",
        agent_id ? agent_id : "",
        target_type ? target_type : "task",
        target_id ? target_id : "",
        signal_type ? signal_type : "progress",
        intensity,
        ttl_seconds,
        metadata_json ? ",\"metadata\":" : "",
        metadata_json ? metadata_json : "",
        metadata_json ? "" : "");
    return graphsub_post("/pheromone/deposit", body);
}

char *graphsub_pheromone_query(const char *target_id,
                                const char *signal_type,
                                double min_intensity) {
    char path[512];
    if (signal_type) {
        snprintf(path, sizeof(path),
            "/pheromone/query?target_id=%s&signal_type=%s&min_intensity=%.2f",
            target_id ? target_id : "", signal_type, min_intensity);
    } else {
        snprintf(path, sizeof(path),
            "/pheromone/query?target_id=%s&min_intensity=%.2f",
            target_id ? target_id : "", min_intensity);
    }
    return graphsub_get(path);
}

char *graphsub_pheromone_sweep(const char *target_type,
                                const char *signal_type) {
    char body[256];
    snprintf(body, sizeof(body),
        "{\"target_type\":%s,\"signal_type\":%s}",
        target_type ? target_type : "null",
        signal_type ? signal_type : "null");
    return graphsub_post("/pheromone/sweep", body);
}

/* ── Graph Operations ───────────────────────────────────────────────────── */

char *graphsub_graph_traverse(const char *start_node,
                              int depth,
                              const char *edge_filter_json,
                              int limit) {
    char body[1024];
    snprintf(body, sizeof(body),
        "{\"start\":\"%s\","
        "\"depth\":%d,"
        "%s%s%s"
        "\"limit\":%d}",
        start_node ? start_node : "",
        depth > 0 ? depth : 3,
        edge_filter_json ? "\"edge_filter\":" : "",
        edge_filter_json ? edge_filter_json : "",
        edge_filter_json ? "," : "",
        limit > 0 ? limit : 100);
    return graphsub_post("/graph/traverse", body);
}

/* ── Memory ─────────────────────────────────────────────────────────────── */

char *graphsub_memory_sync(const char *agent_id,
                            const char *episodes_json,
                            const char *type) {
    char *body = NULL;
    size_t body_len = 0;
    FILE *mem = open_memstream(&body, &body_len);
    if (!mem) return NULL;

    fprintf(mem,
        "{\"agent_id\":\"%s\","
        "\"type\":\"%s\","
        "\"episodes\":%s}",
        agent_id ? agent_id : "",
        type ? type : "episodic",
        episodes_json ? episodes_json : "[]");

    fclose(mem);
    char *result = graphsub_post("/memory/sync", body);
    free(body);
    return result;
}

char *graphsub_memory_consolidate(const char *agent_id,
                                   const char *from_ts,
                                   const char *to_ts) {
    char body[512];
    snprintf(body, sizeof(body),
        "{\"agent_id\":\"%s\","
        "\"time_range\":{\"from\":\"%s\",\"to\":\"%s\"}}",
        agent_id ? agent_id : "",
        from_ts ? from_ts : "",
        to_ts ? to_ts : "");
    return graphsub_post("/memory/consolidate", body);
}

/* ── Swarm ───────────────────────────────────────────────────────────────── */

char *graphsub_swarm_register(const char *swarm_id,
                              const char *topology,
                              const char *agents_json,
                              const char *task_desc) {
    char *body = NULL;
    size_t body_len = 0;
    FILE *mem = open_memstream(&body, &body_len);
    if (!mem) return NULL;

    fprintf(mem,
        "{\"swarm_id\":\"%s\","
        "\"topology\":\"%s\","
        "\"agents\":%s,"
        "\"task\":\"%s\"}",
        swarm_id ? swarm_id : "",
        topology ? topology : "fanout",
        agents_json ? agents_json : "[]",
        task_desc ? task_desc : "");

    fclose(mem);
    char *result = graphsub_post("/swarm/topology", body);
    free(body);
    return result;
}

/* ── Tool Results ────────────────────────────────────────────────────────── */

char *graphsub_tool_result(const char *agent_id,
                            const char *tool_name,
                            const char *tool_params_json,
                            const char *result_json,
                            int duration_ms,
                            bool success,
                            const char *executed_on,
                            const char *swarm_id) {
    char *body = NULL;
    size_t body_len = 0;
    FILE *mem = open_memstream(&body, &body_len);
    if (!mem) return NULL;

    fprintf(mem,
        "{\"agent_id\":\"%s\","
        "\"tool_name\":\"%s\","
        "\"tool_params\":%s,"
        "\"result\":%s,"
        "\"duration_ms\":%d,"
        "\"success\":%s,"
        "\"executed_on\":\"%s\""
        "%s%s%s}",
        agent_id ? agent_id : "",
        tool_name ? tool_name : "",
        tool_params_json ? tool_params_json : "{}",
        result_json ? result_json : "{}",
        duration_ms,
        success ? "true" : "false",
        executed_on ? executed_on : "",
        swarm_id ? ",\"swarm_id\":\"" : "",
        swarm_id ? swarm_id : "",
        swarm_id ? "\"" : "");

    fclose(mem);
    char *result = graphsub_post("/tool/result", body);
    free(body);
    return result;
}

/* ── Bridge Fleet ────────────────────────────────────────────────────────── */

char *graphsub_bridge_fleet(const char *peers_json) {
    char body[4096];
    snprintf(body, sizeof(body),
        "{\"peers\":%s}",
        peers_json ? peers_json : "[]");
    return graphsub_post("/bridge/fleet", body);
}
