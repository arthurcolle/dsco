/* net_tool.c — "net" tool dispatch + HTTP route registration for dsco
 *
 * Tool actions:
 *   mesh/status   mesh/peers   mesh/send   mesh/broadcast
 *   http/post
 *   bridge/fleet  bridge/exec  bridge/send
 *   bridge/bus_put bridge/bus_get
 *
 * HTTP routes (registered via dsco_net_routes_register):
 *   GET  /health       → 200 JSON status
 *   POST /tool         → invoke named dsco tool remotely
 *   GET  /mesh/peers   → list mesh peers
 */

#include "tools.h"
#include "json_util.h"
#include "audit_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <time.h>
#include <errno.h>

#ifdef HAVE_LIBSODIUM
#  include "mesh.h"
#  include "peer_bootstrap.h"
#endif

#if defined(HAVE_MBEDTLS) && defined(HAVE_LIBSODIUM)
#  include "net_server.h"
#endif

/* ── Forward decl: g_mesh_node and g_net_server are defined in main.c ──── */
#ifdef HAVE_LIBSODIUM
/* g_mesh_node defined in main.c; weak fallback for test builds (main.o excluded) */
mesh_node_t       *g_mesh_node  __attribute__((weak)) = NULL;
#endif
#if defined(HAVE_MBEDTLS) && defined(HAVE_LIBSODIUM)
dsco_net_server_t *g_net_server __attribute__((weak)) = NULL;
#endif

/* ── Helpers ───────────────────────────────────────────────────────────── */

static const char *home_dir(void) {
    const char *h = getenv("HOME");
    return h ? h : "/tmp";
}

/* Run a shell command, capture stdout+stderr, return malloc'd string */
static char *shell_capture(const char *cmd) {
    FILE *fp = popen(cmd, "r");
    if (!fp) return strdup("");
    char *out = NULL;
    size_t cap = 0, len = 0;
    char buf[4096];
    while (fgets(buf, sizeof(buf), fp)) {
        size_t n = strlen(buf);
        if (len + n + 1 > cap) {
            cap = (len + n + 1) * 2 + 1024;
            out = realloc(out, cap);
        }
        memcpy(out + len, buf, n);
        len += n;
        out[len] = '\0';
    }
    pclose(fp);
    return out ? out : strdup("");
}

/* ══════════════════════════════════════════════════════════════════════════
 * MESH ACTIONS
 * ══════════════════════════════════════════════════════════════════════════ */

static bool net_mesh_status(const char *input, char *result, size_t rlen) {
#ifndef HAVE_LIBSODIUM
    (void)input;
    snprintf(result, rlen, "{\"error\":\"libsodium not compiled in\"}");
    return false;
#else
    (void)input;
    if (!g_mesh_node) {
        snprintf(result, rlen, "{\"status\":\"disabled\",\"error\":\"mesh not started\"}");
        return true;
    }
    char pk_hex[65] = {0};
    mesh_pubkey_to_hex(mesh_node_pubkey(g_mesh_node), pk_hex);

    mesh_peer_info_t peers[MESH_MAX_PEERS];
    int npeers = mesh_node_peers(g_mesh_node, peers, MESH_MAX_PEERS);

    char *p = result;
    size_t rem = rlen;
    int n = snprintf(p, rem,
        "{\"status\":\"running\",\"pubkey\":\"%s\",\"peer_count\":%d,\"peers\":[",
        pk_hex, npeers);
    p += n; rem -= (size_t)n;
    for (int i = 0; i < npeers && rem > 4; i++) {
        char ppk[65];
        mesh_pubkey_to_hex(peers[i].pubkey, ppk);
        n = snprintf(p, rem, "%s{\"pubkey\":\"%s\",\"addr\":\"%s\",\"outbound\":%s}",
            i ? "," : "", ppk, peers[i].addr, peers[i].outbound ? "true" : "false");
        p += n; rem -= (size_t)n;
    }
    snprintf(p, rem, "]}");
    return true;
#endif
}

static bool net_mesh_peers(const char *input, char *result, size_t rlen) {
    return net_mesh_status(input, result, rlen); /* same info */
}

static bool net_mesh_send(const char *input, char *result, size_t rlen) {
#ifndef HAVE_LIBSODIUM
    (void)input;
    snprintf(result, rlen, "{\"error\":\"libsodium not compiled in\"}");
    return false;
#else
    if (!g_mesh_node) {
        snprintf(result, rlen, "{\"error\":\"mesh not started\"}");
        return false;
    }
    char *peer_pk_hex = json_get_str(input, "peer_pubkey");
    char *data        = json_get_str(input, "data");
    if (!peer_pk_hex || !data) {
        free(peer_pk_hex); free(data);
        snprintf(result, rlen, "{\"error\":\"peer_pubkey and data required\"}");
        return false;
    }
    uint8_t pk[MESH_PUBKEY_LEN];
    if (!mesh_pubkey_from_hex(peer_pk_hex, pk)) {
        free(peer_pk_hex); free(data);
        snprintf(result, rlen, "{\"error\":\"invalid pubkey hex\"}");
        return false;
    }
    bool ok = mesh_node_send_to(g_mesh_node, pk, data, strlen(data));
    snprintf(result, rlen, "{\"sent\":%s}", ok ? "true" : "false");
    free(peer_pk_hex); free(data);
    return ok;
#endif
}

static bool net_mesh_broadcast(const char *input, char *result, size_t rlen) {
#ifndef HAVE_LIBSODIUM
    (void)input;
    snprintf(result, rlen, "{\"error\":\"libsodium not compiled in\"}");
    return false;
#else
    if (!g_mesh_node) {
        snprintf(result, rlen, "{\"error\":\"mesh not started\"}");
        return false;
    }
    char *data = json_get_str(input, "data");
    if (!data) {
        snprintf(result, rlen, "{\"error\":\"data required\"}");
        return false;
    }
    int sent = mesh_node_broadcast(g_mesh_node, data, strlen(data));
    snprintf(result, rlen, "{\"broadcast_count\":%d}", sent);
    free(data);
    return true;
#endif
}

static bool net_mesh_connect(const char *input, char *result, size_t rlen) {
#ifndef HAVE_LIBSODIUM
    (void)input;
    snprintf(result, rlen, "{\"error\":\"libsodium not compiled in\"}");
    return false;
#else
    if (!g_mesh_node) {
        snprintf(result, rlen, "{\"error\":\"mesh not started\"}");
        return false;
    }
    char *host = json_get_str(input, "host");
    int port   = json_get_int(input, "port", 7337);
    if (!host) {
        snprintf(result, rlen, "{\"error\":\"host required\"}");
        return false;
    }
    bool ok = mesh_node_connect(g_mesh_node, host, (uint16_t)port);
    snprintf(result, rlen, "{\"connected\":%s,\"host\":\"%s\",\"port\":%d}",
             ok ? "true" : "false", host, port);
    free(host);
    return ok;
#endif
}

/* ══════════════════════════════════════════════════════════════════════════
 * HTTP ACTIONS
 * ══════════════════════════════════════════════════════════════════════════ */

static bool net_http_post(const char *input, char *result, size_t rlen) {
#if !defined(HAVE_MBEDTLS) || !defined(HAVE_LIBSODIUM)
    (void)input;
    snprintf(result, rlen, "{\"error\":\"mbedTLS+libsodium not compiled in\"}");
    return false;
#else
    char *host  = json_get_str(input, "host");
    int   port  = json_get_int(input, "port", 7547);
    char *path  = json_get_str(input, "path");
    char *body  = json_get_str(input, "body");
    bool  tls   = json_get_bool(input, "tls", true);

    if (!host) {
        free(host); free(path); free(body);
        snprintf(result, rlen, "{\"error\":\"host required\"}");
        return false;
    }

    char *resp = netsrv_client_post(
        host, (uint16_t)port,
        path ? path : "/tool",
        body ? body : "{}",
        NULL, 0, tls
    );

    if (resp) {
        snprintf(result, rlen, "%s", resp);
        free(resp);
    } else {
        snprintf(result, rlen, "{\"error\":\"request failed\"}");
    }

    free(host); free(path); free(body);
    return resp != NULL;
#endif
}

static bool net_http_status(const char *input, char *result, size_t rlen) {
    (void)input;
#if defined(HAVE_MBEDTLS) && defined(HAVE_LIBSODIUM)
    if (g_net_server) {
        snprintf(result, rlen, "{\"status\":\"running\",\"port\":%u}",
                 (unsigned)netsrv_port(g_net_server));
    } else {
        snprintf(result, rlen, "{\"status\":\"stopped\"}");
    }
#else
    snprintf(result, rlen, "{\"status\":\"disabled\",\"error\":\"mbedTLS not compiled in\"}");
#endif
    return true;
}

/* ══════════════════════════════════════════════════════════════════════════
 * BRIDGE ACTIONS
 * ══════════════════════════════════════════════════════════════════════════ */

/* Parse fleet directory .host files into JSON array */
static bool net_bridge_fleet(const char *input, char *result, size_t rlen) {
    (void)input;
    char fleet_dir[512];
    snprintf(fleet_dir, sizeof(fleet_dir), "%s/bridge/fleet", home_dir());

    DIR *d = opendir(fleet_dir);
    if (!d) {
        snprintf(result, rlen, "{\"error\":\"fleet dir not found: %s\"}", fleet_dir);
        return false;
    }

    char *p = result;
    size_t rem = rlen;
    int n = snprintf(p, rem, "{\"fleet\":[");
    p += n; rem -= (size_t)n;

    struct dirent *ent;
    bool first = true;
    while ((ent = readdir(d)) != NULL) {
        size_t nl = strlen(ent->d_name);
        if (nl < 6 || strcmp(ent->d_name + nl - 5, ".host") != 0) continue;

        char fpath[1024];
        snprintf(fpath, sizeof(fpath), "%s/%s", fleet_dir, ent->d_name);

        FILE *f = fopen(fpath, "r");
        if (!f) continue;

        /* Parse key="value" lines */
        char name[64]="", user[64]="", addr[64]="", arch[32]="",
             cpu[128]="", ram[16]="", os[32]="", roles[128]="", seen[32]="";
        char line[512];
        while (fgets(line, sizeof(line), f)) {
            /* Strip comments and newlines */
            char *hash = strchr(line, '#');
            if (hash) *hash = '\0';
            char *nl2 = strchr(line, '\n');
            if (nl2) *nl2 = '\0';
            /* Parse KEY="VALUE" */
            char key[64], val[256];
            if (sscanf(line, "%63[^=]=\"%255[^\"]\"", key, val) == 2) {
                if      (strcmp(key,"NAME")==0)      snprintf(name,  sizeof(name),  "%s", val);
                else if (strcmp(key,"USER")==0)      snprintf(user,  sizeof(user),  "%s", val);
                else if (strcmp(key,"ADDR")==0)      snprintf(addr,  sizeof(addr),  "%s", val);
                else if (strcmp(key,"ARCH")==0)      snprintf(arch,  sizeof(arch),  "%s", val);
                else if (strcmp(key,"CPU")==0)       snprintf(cpu,   sizeof(cpu),   "%s", val);
                else if (strcmp(key,"RAM_GB")==0)    snprintf(ram,   sizeof(ram),   "%s", val);
                else if (strcmp(key,"OS")==0)        snprintf(os,    sizeof(os),    "%s", val);
                else if (strcmp(key,"ROLES")==0)     snprintf(roles, sizeof(roles), "%s", val);
                else if (strcmp(key,"LAST_SEEN")==0) snprintf(seen,  sizeof(seen),  "%s", val);
            }
        }
        fclose(f);

        if (!name[0]) continue;
        n = snprintf(p, rem,
            "%s{\"name\":\"%s\",\"user\":\"%s\",\"addr\":\"%s\","
            "\"arch\":\"%s\",\"cpu\":\"%s\",\"ram_gb\":\"%s\","
            "\"os\":\"%s\",\"roles\":\"%s\",\"last_seen\":\"%s\"}",
            first ? "" : ",", name, user, addr, arch, cpu, ram, os, roles, seen);
        p += n; rem -= (size_t)n;
        first = false;
    }
    closedir(d);

    snprintf(p, rem, "]}");
    return true;
}

/* Execute command on bridge peer via connect.sh exec */
static bool net_bridge_exec(const char *input, char *result, size_t rlen) {
    char *peer = json_get_str(input, "peer");
    char *cmd  = json_get_str(input, "cmd");
    if (!peer || !cmd) {
        free(peer); free(cmd);
        snprintf(result, rlen, "{\"error\":\"peer and cmd required\"}");
        return false;
    }

    /* Use fleet.sh on <peer> */
    char sh[2048];
    snprintf(sh, sizeof(sh),
        "%s/bridge/plugins/fleet.sh on %s %s 2>&1",
        home_dir(), peer, cmd);

    char *out = shell_capture(sh);
    snprintf(result, rlen, "{\"peer\":\"%s\",\"cmd\":\"%s\",\"output\":%s}",
             peer, cmd, out && out[0] ? "\"see raw\"" : "\"\"");
    
    if (out && strlen(out) + 64 < rlen) {
        /* Escape and embed */
        char esc[4096] = {0};
        size_t ei = 0;
        for (size_t i = 0; out[i] && ei < sizeof(esc)-4; i++) {
            if      (out[i]=='"')  { esc[ei++]='\\'; esc[ei++]='"'; }
            else if (out[i]=='\\') { esc[ei++]='\\'; esc[ei++]='\\'; }
            else if (out[i]=='\n') { esc[ei++]='\\'; esc[ei++]='n'; }
            else if (out[i]=='\r') { }
            else esc[ei++] = out[i];
        }
        snprintf(result, rlen,
            "{\"peer\":\"%s\",\"cmd\":\"%s\",\"output\":\"%s\"}",
            peer, cmd, esc);
    }
    free(out); free(peer); free(cmd);
    return true;
}

/* Drop a .msg file into ~/bridge/outbox */
static bool net_bridge_send(const char *input, char *result, size_t rlen) {
    char *msg = json_get_str(input, "message");
    if (!msg) {
        snprintf(result, rlen, "{\"error\":\"message required\"}");
        return false;
    }
    char outbox[512];
    snprintf(outbox, sizeof(outbox), "%s/bridge/outbox", home_dir());
    char fname[640];
    snprintf(fname, sizeof(fname), "%s/%ld-%d.msg", outbox, (long)time(NULL), getpid());

    FILE *f = fopen(fname, "w");
    if (!f) {
        free(msg);
        snprintf(result, rlen, "{\"error\":\"cannot write to outbox: %s\"}", strerror(errno));
        return false;
    }
    fputs(msg, f);
    fclose(f);
    snprintf(result, rlen, "{\"queued\":\"%s\"}", fname);
    free(msg);
    return true;
}

/* Write to bus.py JSONL log */
static bool net_bridge_bus_put(const char *input, char *result, size_t rlen) {
    char *kind = json_get_str(input, "kind");
    char *body = json_get_str(input, "body");
    if (!kind) {
        free(kind); free(body);
        snprintf(result, rlen, "{\"error\":\"kind required\"}");
        return false;
    }
    char sh[2048];
    snprintf(sh, sizeof(sh),
        "%s/bridge/plugins/bus.py put %s %s 2>&1",
        home_dir(), kind, body ? body : "");
    char *out = shell_capture(sh);
    snprintf(result, rlen, "{\"seq\":%s}", out && out[0] ? out : "null");
    free(out); free(kind); free(body);
    return true;
}

/* Read from bus.py JSONL log */
static bool net_bridge_bus_get(const char *input, char *result, size_t rlen) {
    int since  = json_get_int(input, "since", 0);
    char *kind = json_get_str(input, "kind");
    int limit  = json_get_int(input, "limit", 20);

    char sh[2048];
    snprintf(sh, sizeof(sh),
        "%s/bridge/plugins/bus.py get --since %d %s --limit %d 2>&1",
        home_dir(), since,
        kind ? kind : "",
        limit);
    char *out = shell_capture(sh);
    snprintf(result, rlen, "%s", out && out[0] ? out : "[]");
    free(out); free(kind);
    return true;
}

/* Remote tool invocation via HTTP */
static bool net_remote_tool(const char *input, char *result, size_t rlen) {
#if !defined(HAVE_MBEDTLS) || !defined(HAVE_LIBSODIUM)
    (void)input;
    snprintf(result, rlen, "{\"error\":\"mbedTLS+libsodium not compiled in\"}");
    return false;
#else
    /* peer name or IP */
    char *peer = json_get_str(input, "peer");
    char *tool = json_get_str(input, "tool");
    char *params = json_get_str(input, "params");
    int port = json_get_int(input, "port", 7547);

    if (!peer || !tool) {
        free(peer); free(tool); free(params);
        snprintf(result, rlen, "{\"error\":\"peer and tool required\"}");
        return false;
    }

    /* Resolve peer addr from fleet if it's a name not an IP */
    char addr[128];
    snprintf(addr, sizeof(addr), "%s", peer);

    /* Try fleet lookup */
    char fleet_path[512];
    snprintf(fleet_path, sizeof(fleet_path), "%s/bridge/fleet/%s.host", home_dir(), peer);
    FILE *f = fopen(fleet_path, "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            char key[64], val[128];
            if (sscanf(line, "%63[^=]=\"%127[^\"]\"", key, val) == 2 &&
                strcmp(key, "ADDR") == 0) {
                snprintf(addr, sizeof(addr), "%s", val);
                break;
            }
        }
        fclose(f);
    }

    /* Build JSON body: {"tool": "...", "params": {...}} */
    char body[4096];
    snprintf(body, sizeof(body), "{\"tool\":\"%s\",\"params\":%s}",
             tool, params ? params : "{}");

    char *resp = netsrv_client_post(addr, (uint16_t)port, "/tool", body, NULL, 0, true);
    if (resp) {
        snprintf(result, rlen, "%s", resp);
        free(resp);
    } else {
        snprintf(result, rlen, "{\"error\":\"remote call failed\",\"peer\":\"%s\",\"addr\":\"%s\"}", peer, addr);
    }
    free(peer); free(tool); free(params);
    return resp != NULL;
#endif
}

/* ══════════════════════════════════════════════════════════════════════════
 * TOP-LEVEL TOOL DISPATCH
 * ══════════════════════════════════════════════════════════════════════════ */

bool tool_net_dispatch(const char *input, char *result, size_t rlen) {
    char *action = json_get_str(input, "action");
    if (!action || !action[0]) {
        free(action);
        snprintf(result, rlen,
            "{\"error\":\"action required\","
            "\"actions\":["
            "\"mesh/status\",\"mesh/peers\",\"mesh/send\",\"mesh/broadcast\",\"mesh/connect\","
            "\"http/post\",\"http/status\","
            "\"bridge/fleet\",\"bridge/exec\",\"bridge/send\","
            "\"bridge/bus_put\",\"bridge/bus_get\","
            "\"remote\""
            "]}");
        return false;
    }

    bool ok = false;
    if      (strcmp(action, "mesh/status")    == 0) ok = net_mesh_status(input, result, rlen);
    else if (strcmp(action, "mesh/peers")     == 0) ok = net_mesh_peers(input, result, rlen);
    else if (strcmp(action, "mesh/send")      == 0) ok = net_mesh_send(input, result, rlen);
    else if (strcmp(action, "mesh/broadcast") == 0) ok = net_mesh_broadcast(input, result, rlen);
    else if (strcmp(action, "mesh/connect")   == 0) ok = net_mesh_connect(input, result, rlen);
    else if (strcmp(action, "http/post")      == 0) ok = net_http_post(input, result, rlen);
    else if (strcmp(action, "http/status")    == 0) ok = net_http_status(input, result, rlen);
    else if (strcmp(action, "bridge/fleet")   == 0) ok = net_bridge_fleet(input, result, rlen);
    else if (strcmp(action, "bridge/exec")    == 0) ok = net_bridge_exec(input, result, rlen);
    else if (strcmp(action, "bridge/send")    == 0) ok = net_bridge_send(input, result, rlen);
    else if (strcmp(action, "bridge/bus_put") == 0) ok = net_bridge_bus_put(input, result, rlen);
    else if (strcmp(action, "bridge/bus_get") == 0) ok = net_bridge_bus_get(input, result, rlen);
    else if (strcmp(action, "remote")         == 0) ok = net_remote_tool(input, result, rlen);
    else {
        snprintf(result, rlen, "{\"error\":\"unknown action: %s\"}", action);
    }

    free(action);
    return ok;
}

/* ══════════════════════════════════════════════════════════════════════════
 * HTTP SERVER ROUTES  (called from main.c after netsrv_create)
 * ══════════════════════════════════════════════════════════════════════════ */

#if defined(HAVE_MBEDTLS) && defined(HAVE_LIBSODIUM)

static netsrv_response_t route_health(const netsrv_request_t *req, void *ctx) {
    (void)req; (void)ctx;
    char buf[512];
    char pk_hex[65] = "disabled";
    int npeers = 0;
    if (g_mesh_node) {
        mesh_pubkey_to_hex(mesh_node_pubkey(g_mesh_node), pk_hex);
        mesh_peer_info_t peers[MESH_MAX_PEERS];
        npeers = mesh_node_peers(g_mesh_node, peers, MESH_MAX_PEERS);
    }
    snprintf(buf, sizeof(buf),
        "{\"status\":\"ok\",\"node\":\"dsco\",\"mesh_pubkey\":\"%s\",\"peers\":%d}",
        pk_hex, npeers);
    return (netsrv_response_t){ .status = 200, .body = strdup(buf), .heap_body = true };
}

/* Minimal remote tool invocation: POST /tool {"tool":"name","params":{...}} */
static netsrv_response_t route_tool(const netsrv_request_t *req, void *ctx) {
    (void)ctx;
    if (!req->body || req->body_len == 0) {
        return (netsrv_response_t){ .status = 400,
            .body = (char*)"{\"error\":\"empty body\"}", .heap_body = false };
    }

    /* Extract tool name and params from body JSON */
    char *tool_name = json_get_str(req->body, "tool");
    char *params_raw = json_get_str(req->body, "params");
    if (!tool_name) {
        free(params_raw);
        return (netsrv_response_t){ .status = 400,
            .body = (char*)"{\"error\":\"tool required\"}", .heap_body = false };
    }

    /* Look up tool in global registry */
    char result_buf[128 * 1024];
    result_buf[0] = '\0';
    bool ok = tools_invoke_by_name(tool_name, params_raw ? params_raw : "{}", result_buf, sizeof(result_buf));
    (void)ok;

    free(tool_name);
    free(params_raw);

    char *body = strdup(result_buf[0] ? result_buf : "{\"ok\":true}");
    return (netsrv_response_t){ .status = 200, .body = body, .heap_body = true };
}

static netsrv_response_t route_mesh_peers(const netsrv_request_t *req, void *ctx) {
    (void)req; (void)ctx;
    char buf[8192];
    net_mesh_status(NULL, buf, sizeof(buf));
    return (netsrv_response_t){ .status = 200, .body = strdup(buf), .heap_body = true };
}

#endif /* HAVE_MBEDTLS && HAVE_LIBSODIUM */

void dsco_net_routes_register(void *srv_opaque) {
#if defined(HAVE_MBEDTLS) && defined(HAVE_LIBSODIUM)
    dsco_net_server_t *srv = (dsco_net_server_t *)srv_opaque;
    netsrv_route(srv, "GET",  "/health",     route_health,     NULL);
    netsrv_route(srv, "POST", "/tool",       route_tool,       NULL);
    netsrv_route(srv, "GET",  "/mesh/peers", route_mesh_peers, NULL);
#else
    (void)srv_opaque;
#endif
}
