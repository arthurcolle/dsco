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
#include <sys/wait.h>
#include <signal.h>
#include <dirent.h>
#include <time.h>
#include <errno.h>

#ifdef HAVE_LIBSODIUM
#include "mesh.h"
#include "peer_bootstrap.h"
#endif

#if defined(HAVE_MBEDTLS) && defined(HAVE_LIBSODIUM)
#include "net_server.h"
#include "cluster.h"
#endif

/* ── Forward decl: g_mesh_node and g_net_server are defined in main.c ──── */
#ifdef HAVE_LIBSODIUM
/* g_mesh_node defined in main.c; weak fallback for test builds (main.o excluded) */
mesh_node_t *g_mesh_node __attribute__((weak)) = NULL;
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
    if (!fp)
        return strdup("");
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
    int n =
        snprintf(p, rem, "{\"status\":\"running\",\"pubkey\":\"%s\",\"peer_count\":%d,\"peers\":[",
                 pk_hex, npeers);
    p += n;
    rem -= (size_t)n;
    for (int i = 0; i < npeers && rem > 4; i++) {
        char ppk[65];
        mesh_pubkey_to_hex(peers[i].pubkey, ppk);
        n = snprintf(p, rem, "%s{\"pubkey\":\"%s\",\"addr\":\"%s\",\"outbound\":%s}", i ? "," : "",
                     ppk, peers[i].addr, peers[i].outbound ? "true" : "false");
        p += n;
        rem -= (size_t)n;
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
    char *data = json_get_str(input, "data");
    if (!peer_pk_hex || !data) {
        free(peer_pk_hex);
        free(data);
        snprintf(result, rlen, "{\"error\":\"peer_pubkey and data required\"}");
        return false;
    }
    uint8_t pk[MESH_PUBKEY_LEN];
    if (!mesh_pubkey_from_hex(peer_pk_hex, pk)) {
        free(peer_pk_hex);
        free(data);
        snprintf(result, rlen, "{\"error\":\"invalid pubkey hex\"}");
        return false;
    }
    bool ok = mesh_node_send_to(g_mesh_node, pk, data, strlen(data));
    snprintf(result, rlen, "{\"sent\":%s}", ok ? "true" : "false");
    free(peer_pk_hex);
    free(data);
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
    int port = json_get_int(input, "port", 7337);
    if (!host) {
        snprintf(result, rlen, "{\"error\":\"host required\"}");
        return false;
    }
    bool ok = mesh_node_connect(g_mesh_node, host, (uint16_t)port);
    snprintf(result, rlen, "{\"connected\":%s,\"host\":\"%s\",\"port\":%d}", ok ? "true" : "false",
             host, port);
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
    char *host = json_get_str(input, "host");
    int port = json_get_int(input, "port", 7547);
    char *path = json_get_str(input, "path");
    char *body = json_get_str(input, "body");
    bool tls = json_get_bool(input, "tls", true);

    if (!host) {
        free(host);
        free(path);
        free(body);
        snprintf(result, rlen, "{\"error\":\"host required\"}");
        return false;
    }

    char *resp = netsrv_client_post(host, (uint16_t)port, path ? path : "/tool", body ? body : "{}",
                                    NULL, 0, tls);

    if (resp) {
        snprintf(result, rlen, "%s", resp);
        free(resp);
    } else {
        snprintf(result, rlen, "{\"error\":\"request failed\"}");
    }

    free(host);
    free(path);
    free(body);
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
    p += n;
    rem -= (size_t)n;

    struct dirent *ent;
    bool first = true;
    while ((ent = readdir(d)) != NULL) {
        size_t nl = strlen(ent->d_name);
        if (nl < 6 || strcmp(ent->d_name + nl - 5, ".host") != 0)
            continue;

        char fpath[1024];
        snprintf(fpath, sizeof(fpath), "%s/%s", fleet_dir, ent->d_name);

        FILE *f = fopen(fpath, "r");
        if (!f)
            continue;

        /* Parse key="value" lines */
        char name[64] = "", user[64] = "", addr[64] = "", arch[32] = "", cpu[128] = "",
             ram[16] = "", os[32] = "", roles[128] = "", seen[32] = "";
        char line[512];
        while (fgets(line, sizeof(line), f)) {
            /* Strip comments and newlines */
            char *hash = strchr(line, '#');
            if (hash)
                *hash = '\0';
            char *nl2 = strchr(line, '\n');
            if (nl2)
                *nl2 = '\0';
            /* Parse KEY="VALUE" */
            char key[64], val[256];
            if (sscanf(line, "%63[^=]=\"%255[^\"]\"", key, val) == 2) {
                if (strcmp(key, "NAME") == 0)
                    snprintf(name, sizeof(name), "%s", val);
                else if (strcmp(key, "USER") == 0)
                    snprintf(user, sizeof(user), "%s", val);
                else if (strcmp(key, "ADDR") == 0)
                    snprintf(addr, sizeof(addr), "%s", val);
                else if (strcmp(key, "ARCH") == 0)
                    snprintf(arch, sizeof(arch), "%s", val);
                else if (strcmp(key, "CPU") == 0)
                    snprintf(cpu, sizeof(cpu), "%s", val);
                else if (strcmp(key, "RAM_GB") == 0)
                    snprintf(ram, sizeof(ram), "%s", val);
                else if (strcmp(key, "OS") == 0)
                    snprintf(os, sizeof(os), "%s", val);
                else if (strcmp(key, "ROLES") == 0)
                    snprintf(roles, sizeof(roles), "%s", val);
                else if (strcmp(key, "LAST_SEEN") == 0)
                    snprintf(seen, sizeof(seen), "%s", val);
            }
        }
        fclose(f);

        if (!name[0])
            continue;
        n = snprintf(p, rem,
                     "%s{\"name\":\"%s\",\"user\":\"%s\",\"addr\":\"%s\","
                     "\"arch\":\"%s\",\"cpu\":\"%s\",\"ram_gb\":\"%s\","
                     "\"os\":\"%s\",\"roles\":\"%s\",\"last_seen\":\"%s\"}",
                     first ? "" : ",", name, user, addr, arch, cpu, ram, os, roles, seen);
        p += n;
        rem -= (size_t)n;
        first = false;
    }
    closedir(d);

    snprintf(p, rem, "]}");
    return true;
}

/* Execute command on bridge peer via connect.sh exec */
static bool net_bridge_exec(const char *input, char *result, size_t rlen) {
    char *peer = json_get_str(input, "peer");
    char *cmd = json_get_str(input, "cmd");
    if (!peer || !cmd) {
        free(peer);
        free(cmd);
        snprintf(result, rlen, "{\"error\":\"peer and cmd required\"}");
        return false;
    }

    /* Use fleet.sh on <peer> */
    char sh[2048];
    snprintf(sh, sizeof(sh), "%s/bridge/plugins/fleet.sh on %s %s 2>&1", home_dir(), peer, cmd);

    char *out = shell_capture(sh);
    snprintf(result, rlen, "{\"peer\":\"%s\",\"cmd\":\"%s\",\"output\":%s}", peer, cmd,
             out && out[0] ? "\"see raw\"" : "\"\"");

    if (out && strlen(out) + 64 < rlen) {
        /* Escape and embed */
        char esc[4096] = {0};
        size_t ei = 0;
        for (size_t i = 0; out[i] && ei < sizeof(esc) - 4; i++) {
            if (out[i] == '"') {
                esc[ei++] = '\\';
                esc[ei++] = '"';
            } else if (out[i] == '\\') {
                esc[ei++] = '\\';
                esc[ei++] = '\\';
            } else if (out[i] == '\n') {
                esc[ei++] = '\\';
                esc[ei++] = 'n';
            } else if (out[i] == '\r') {
            } else
                esc[ei++] = out[i];
        }
        snprintf(result, rlen, "{\"peer\":\"%s\",\"cmd\":\"%s\",\"output\":\"%s\"}", peer, cmd,
                 esc);
    }
    free(out);
    free(peer);
    free(cmd);
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
        free(kind);
        free(body);
        snprintf(result, rlen, "{\"error\":\"kind required\"}");
        return false;
    }
    char sh[2048];
    snprintf(sh, sizeof(sh), "%s/bridge/plugins/bus.py put %s %s 2>&1", home_dir(), kind,
             body ? body : "");
    char *out = shell_capture(sh);
    snprintf(result, rlen, "{\"seq\":%s}", out && out[0] ? out : "null");
    free(out);
    free(kind);
    free(body);
    return true;
}

/* Read from bus.py JSONL log */
static bool net_bridge_bus_get(const char *input, char *result, size_t rlen) {
    int since = json_get_int(input, "since", 0);
    char *kind = json_get_str(input, "kind");
    int limit = json_get_int(input, "limit", 20);

    char sh[2048];
    snprintf(sh, sizeof(sh), "%s/bridge/plugins/bus.py get --since %d %s --limit %d 2>&1",
             home_dir(), since, kind ? kind : "", limit);
    char *out = shell_capture(sh);
    snprintf(result, rlen, "%s", out && out[0] ? out : "[]");
    free(out);
    free(kind);
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
        free(peer);
        free(tool);
        free(params);
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
            if (sscanf(line, "%63[^=]=\"%127[^\"]\"", key, val) == 2 && strcmp(key, "ADDR") == 0) {
                snprintf(addr, sizeof(addr), "%s", val);
                break;
            }
        }
        fclose(f);
    }

    /* Build JSON body: {"tool": "...", "params": {...}} */
    char body[4096];
    snprintf(body, sizeof(body), "{\"tool\":\"%s\",\"params\":%s}", tool, params ? params : "{}");

    char *resp = netsrv_client_post(addr, (uint16_t)port, "/tool", body, NULL, 0, true);
    if (resp) {
        snprintf(result, rlen, "%s", resp);
        free(resp);
    } else {
        snprintf(result, rlen, "{\"error\":\"remote call failed\",\"peer\":\"%s\",\"addr\":\"%s\"}",
                 peer, addr);
    }
    free(peer);
    free(tool);
    free(params);
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
    if (strcmp(action, "mesh/status") == 0)
        ok = net_mesh_status(input, result, rlen);
    else if (strcmp(action, "mesh/peers") == 0)
        ok = net_mesh_peers(input, result, rlen);
    else if (strcmp(action, "mesh/send") == 0)
        ok = net_mesh_send(input, result, rlen);
    else if (strcmp(action, "mesh/broadcast") == 0)
        ok = net_mesh_broadcast(input, result, rlen);
    else if (strcmp(action, "mesh/connect") == 0)
        ok = net_mesh_connect(input, result, rlen);
    else if (strcmp(action, "http/post") == 0)
        ok = net_http_post(input, result, rlen);
    else if (strcmp(action, "http/status") == 0)
        ok = net_http_status(input, result, rlen);
    else if (strcmp(action, "bridge/fleet") == 0)
        ok = net_bridge_fleet(input, result, rlen);
    else if (strcmp(action, "bridge/exec") == 0)
        ok = net_bridge_exec(input, result, rlen);
    else if (strcmp(action, "bridge/send") == 0)
        ok = net_bridge_send(input, result, rlen);
    else if (strcmp(action, "bridge/bus_put") == 0)
        ok = net_bridge_bus_put(input, result, rlen);
    else if (strcmp(action, "bridge/bus_get") == 0)
        ok = net_bridge_bus_get(input, result, rlen);
    else if (strcmp(action, "remote") == 0)
        ok = net_remote_tool(input, result, rlen);
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
    (void)req;
    (void)ctx;
    char buf[512];
    char pk_hex[65] = "disabled";
    int npeers = 0;
    if (g_mesh_node) {
        mesh_pubkey_to_hex(mesh_node_pubkey(g_mesh_node), pk_hex);
        mesh_peer_info_t peers[MESH_MAX_PEERS];
        npeers = mesh_node_peers(g_mesh_node, peers, MESH_MAX_PEERS);
    }
    snprintf(buf, sizeof(buf),
             "{\"status\":\"ok\",\"node\":\"dsco\",\"mesh_pubkey\":\"%s\",\"peers\":%d}", pk_hex,
             npeers);
    return (netsrv_response_t){.status = 200, .body = strdup(buf), .heap_body = true};
}

/* Minimal remote tool invocation: POST /tool {"tool":"name","params":{...}} */
static netsrv_response_t route_tool(const netsrv_request_t *req, void *ctx) {
    (void)ctx;
    if (!req->body || req->body_len == 0) {
        return (netsrv_response_t){
            .status = 400, .body = (char *)"{\"error\":\"empty body\"}", .heap_body = false};
    }

    /* Extract tool name and params from body JSON */
    char *tool_name = json_get_str(req->body, "tool");
    char *params_raw = json_get_str(req->body, "params");
    if (!tool_name) {
        free(params_raw);
        return (netsrv_response_t){
            .status = 400, .body = (char *)"{\"error\":\"tool required\"}", .heap_body = false};
    }

    /* Look up tool in global registry */
    char result_buf[128 * 1024];
    result_buf[0] = '\0';
    bool ok = tools_invoke_by_name(tool_name, params_raw ? params_raw : "{}", result_buf,
                                   sizeof(result_buf));
    (void)ok;

    free(tool_name);
    free(params_raw);

    char *body = strdup(result_buf[0] ? result_buf : "{\"ok\":true}");
    return (netsrv_response_t){.status = 200, .body = body, .heap_body = true};
}

static netsrv_response_t route_mesh_peers(const netsrv_request_t *req, void *ctx) {
    (void)req;
    (void)ctx;
    char buf[8192];
    net_mesh_status(NULL, buf, sizeof(buf));
    return (netsrv_response_t){.status = 200, .body = strdup(buf), .heap_body = true};
}

static int spawn_pipe(const char *cmd, pid_t *pid) {
    int pfd[2];
    if (pipe(pfd) != 0)
        return -1;
    pid_t c = fork();
    if (c < 0) {
        close(pfd[0]);
        close(pfd[1]);
        return -1;
    }
    if (c == 0) {
        setpgid(0, 0); /* new process group → group-killable */
        dup2(pfd[1], STDOUT_FILENO);
        close(pfd[0]);
        close(pfd[1]);
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    setpgid(c, c); /* also set in parent to close the exec/setpgid race */
    close(pfd[1]);
    *pid = c;
    return pfd[0];
}

static char *chat_extract_prompt(const char *body) {
    char *p = json_get_str(body, "prompt");
    if (p && p[0])
        return p;
    free(p);
    /* Find the LAST "content": "…" and extract its (escape-decoded) value —
     * json_get_str doesn't descend into messages[]. */
    const char *c = NULL, *s = body;
    while ((s = strstr(s, "\"content\"")) != NULL) {
        c = s;
        s += 9;
    }
    if (!c)
        return NULL;
    c = strchr(c, ':');
    if (!c)
        return NULL;
    c++;
    while (*c == ' ' || *c == '\t' || *c == '\n' || *c == '\r')
        c++;
    if (*c != '"')
        return NULL;
    c++;
    jbuf_t b;
    jbuf_init(&b, 128);
    for (; *c && *c != '"'; c++) {
        if (*c == '\\' && c[1]) {
            c++;
            switch (*c) {
                case 'n':
                    jbuf_append_char(&b, '\n');
                    break;
                case 't':
                    jbuf_append_char(&b, '\t');
                    break;
                case 'r':
                    jbuf_append_char(&b, '\r');
                    break;
                case '"':
                    jbuf_append_char(&b, '"');
                    break;
                case '\\':
                    jbuf_append_char(&b, '\\');
                    break;
                case '/':
                    jbuf_append_char(&b, '/');
                    break;
                default:
                    jbuf_append_char(&b, *c);
                    break;
            }
        } else {
            jbuf_append_char(&b, *c);
        }
    }
    return b.data;
}

/* Rough token estimate (~4 chars/token) — we shell out so exact counts aren't
 * available; this matches the heuristic most OpenAI-compatible proxies use. */
static int est_tokens(const char *s) {
    size_t n = s ? strlen(s) : 0;
    return (int)((n + 3) / 4);
}

#define MAX_STOPS 4
#define STOP_LEN 128

/* Parse the OpenAI `stop` field (a string or array of up to MAX_STOPS strings)
 * into `stops`, unescaping \n \t \r \" \\.  Returns the count. */
static int parse_stops(const char *body, char stops[][STOP_LEN]) {
    char *raw = json_get_raw(body, "stop");
    if (!raw)
        return 0;
    char *p = raw;
    while (*p == ' ' || *p == '\t')
        p++;
    int n = 0;
    int in_array = (*p == '[');
    if (in_array)
        p++;
    while (n < MAX_STOPS) {
        while (*p && *p != '"' && *p != ']')
            p++;
        if (*p != '"')
            break;
        p++; /* opening quote */
        int k = 0;
        while (*p && *p != '"' && k < STOP_LEN - 1) {
            if (*p == '\\' && p[1]) {
                p++;
                stops[n][k++] = (*p == 'n') ? '\n' : (*p == 't') ? '\t' : (*p == 'r') ? '\r' : *p;
            } else {
                stops[n][k++] = *p;
            }
            p++;
        }
        stops[n][k] = '\0';
        if (k > 0)
            n++;
        if (*p == '"')
            p++;
        if (!in_array)
            break;
    }
    free(raw);
    return n;
}

/* Earliest position in buf[from,to) where any stop string fully matches.
 * Returns 1 and sets *pos, else 0. */
static int find_stop(const char *buf, size_t from, size_t to, char stops[][STOP_LEN], int nstops,
                     size_t *pos) {
    size_t best = to;
    int found = 0;
    for (int i = 0; i < nstops; i++) {
        size_t sl = strlen(stops[i]);
        if (!sl || sl > to - from)
            continue;
        for (size_t q = from; q + sl <= to; q++) {
            if (memcmp(buf + q, stops[i], sl) == 0) {
                if (q < best) {
                    best = q;
                    found = 1;
                }
                break;
            }
        }
    }
    *pos = best;
    return found;
}

/* Build the llama-completion command for a chat request.  On success returns
 * 200, writes the prompt to tempfile `pf` (caller unlinks after running), fills
 * `cmd` + `*model_out` (points into env, do not free) + `*prompt_toks`.  On
 * failure returns an HTTP status and points `*errjson` at a static error body. */
static int chat_prepare(const netsrv_request_t *req, char *cmd, size_t cmdsz, char *pf, size_t pfsz,
                        const char **model_out, int *prompt_toks, const char **errjson) {
    if (!req->body || req->body_len == 0) {
        *errjson = "{\"error\":\"empty body\"}";
        return 400;
    }
    const char *model = getenv("DSCO_SERVE_MODEL");
    if (!model || !model[0]) {
        *errjson = "{\"error\":\"set DSCO_SERVE_MODEL=<path.gguf>\"}";
        return 503;
    }
    char *prompt = chat_extract_prompt(req->body);
    if (!prompt || !prompt[0]) {
        free(prompt);
        *errjson = "{\"error\":\"no prompt or messages[].content\"}";
        return 400;
    }
    int nmax = json_get_int(req->body, "max_tokens", 128);
    if (nmax < 1)
        nmax = 1;
    else if (nmax > 2048)
        nmax = 2048;

    /* Sampling params → llama-completion flags (only when the client sets them,
     * so llama.cpp's own defaults apply otherwise). */
    char samp[256] = "";
    size_t sl = 0;
    double temp = json_get_double(req->body, "temperature", -1);
    double top_p = json_get_double(req->body, "top_p", -1);
    int top_k = json_get_int(req->body, "top_k", -1);
    int seed = json_get_int(req->body, "seed", -1);
    if (temp >= 0)
        sl += (size_t)snprintf(samp + sl, sizeof(samp) - sl, "--temp %g ", temp);
    if (top_p >= 0)
        sl += (size_t)snprintf(samp + sl, sizeof(samp) - sl, "--top-p %g ", top_p);
    if (top_k >= 0)
        sl += (size_t)snprintf(samp + sl, sizeof(samp) - sl, "--top-k %d ", top_k);
    if (seed >= 0)
        sl += (size_t)snprintf(samp + sl, sizeof(samp) - sl, "--seed %d ", seed);

    if (prompt_toks)
        *prompt_toks = est_tokens(prompt);
    snprintf(pf, pfsz, "/tmp/dsco_serve_XXXXXX");
    int fd = mkstemp(pf);
    if (fd >= 0) {
        ssize_t wr = write(fd, prompt, strlen(prompt));
        (void)wr;
        close(fd);
    }
    free(prompt);

    const char *lb = getenv("DSCO_LLAMACPP_DIR");
    char lbdef[512];
    if (!lb || !lb[0]) {
        const char *home = getenv("HOME");
        snprintf(lbdef, sizeof(lbdef), "%s/native_tools/llama.cpp/build/bin", home ? home : ".");
        lb = lbdef;
    }
    /* Distributed: if DSCO_SERVE_RPC names peers, front the split across them
     * (resolve + ensure rpc-server on each, then offload with --rpc + -ngl). */
    char rpc_arg[1200] = "";
    const char *serve_rpc = getenv("DSCO_SERVE_RPC");
    if (serve_rpc && serve_rpc[0]) {
        char eps[1024] = "";
        if (dsco_cluster_rpc_endpoints(serve_rpc, eps, sizeof(eps), 1) > 0)
            snprintf(rpc_arg, sizeof(rpc_arg), "--rpc %s -ngl 99 ", eps);
    }
    snprintf(
        cmd, cmdsz,
        "DYLD_LIBRARY_PATH='%s' LD_LIBRARY_PATH='%s' '%s/llama-completion' -m '%s' %s%s-f '%s' "
        "-n %d --no-warmup --no-display-prompt 2>/dev/null",
        lb, lb, lb, model, rpc_arg, samp, pf, nmax);
    *model_out = model;
    return 200;
}

/* Strip llama.cpp's trailing "> EOF by user" marker + trailing whitespace. */
static void chat_strip_tail(char *out) {
    size_t n = strlen(out);
    /* trim trailing whitespace */
    while (n &&
           (out[n - 1] == '\n' || out[n - 1] == '\r' || out[n - 1] == ' ' || out[n - 1] == '\t'))
        out[--n] = '\0';
    /* Remove a TRAILING "> EOF by user" marker only.  strstr would truncate at
     * the FIRST occurrence, mangling content that legitimately contains the
     * phrase mid-text; the marker is only ever llama.cpp's end-of-stream notice. */
    static const char MARK[] = "> EOF by user";
    size_t ml = sizeof(MARK) - 1;
    if (n >= ml && memcmp(out + n - ml, MARK, ml) == 0) {
        n -= ml;
        out[n] = '\0';
        while (n && (out[n - 1] == '\n' || out[n - 1] == '\r' || out[n - 1] == ' ' ||
                     out[n - 1] == '\t'))
            out[--n] = '\0';
    }
}

/* Largest k <= len such that buf[0..k) ends on a complete UTF-8 character — so
 * a streamed delta never splits a multibyte sequence (which would make the
 * frame's JSON string invalid UTF-8 for strict clients). */
static size_t utf8_trunc(const char *buf, size_t len) {
    if (len == 0)
        return 0;
    size_t start = len;
    while (start > 0 && ((unsigned char)buf[start - 1] & 0xC0) == 0x80)
        start--; /* skip conts */
    if (start == 0)
        return len; /* no lead byte in range; nothing to hold back */
    start--;        /* index of the last character's lead byte */
    unsigned char lead = (unsigned char)buf[start];
    size_t need = lead < 0x80             ? 1
                  : (lead & 0xE0) == 0xC0 ? 2
                  : (lead & 0xF0) == 0xE0 ? 3
                  : (lead & 0xF8) == 0xF0 ? 4
                                          : 1;
    return (start + need <= len) ? len : start; /* complete → keep all; else cut before it */
}

/* Build the full non-streaming chat.completion JSON body (with usage). */
static void chat_build_json(jbuf_t *b, const char *model, const char *content, int prompt_toks) {
    int ct = est_tokens(content);
    jbuf_init(b, strlen(content) + 320);
    jbuf_append(b, "{\"id\":\"chatcmpl-dsco\",\"object\":\"chat.completion\",\"model\":");
    jbuf_append_json_str(b, model);
    jbuf_append(b, ",\"choices\":[{\"index\":0,\"message\":{\"role\":\"assistant\",\"content\":");
    jbuf_append_json_str(b, content);
    jbuf_append(b, "},\"finish_reason\":\"stop\"}],\"usage\":{\"prompt_tokens\":");
    jbuf_append_int(b, prompt_toks);
    jbuf_append(b, ",\"completion_tokens\":");
    jbuf_append_int(b, ct);
    jbuf_append(b, ",\"total_tokens\":");
    jbuf_append_int(b, prompt_toks + ct);
    jbuf_append(b, "}}");
}

/* Send one SSE frame: `data: <json>\n\n`. */
static int sse_send(netsrv_stream_t *s, const char *json) {
    if (netsrv_stream_send(s, "data: ", 6) != 0)
        return -1;
    if (netsrv_stream_send(s, json, strlen(json)) != 0)
        return -1;
    return netsrv_stream_send(s, "\n\n", 2);
}

/* Send a chat.completion.chunk content delta for `len` bytes of `text`.
 * Returns 0 on success, -1 if the client connection is gone. */
static int sse_content(netsrv_stream_t *s, const char *model, const char *text, size_t len) {
    char *tmp = strndup(text, len);
    jbuf_t b;
    jbuf_init(&b, len + 160);
    jbuf_append(&b, "{\"id\":\"chatcmpl-dsco\",\"object\":\"chat.completion.chunk\",\"model\":");
    jbuf_append_json_str(&b, model);
    jbuf_append(&b, ",\"choices\":[{\"index\":0,\"delta\":{\"content\":");
    jbuf_append_json_str(&b, tmp ? tmp : "");
    jbuf_append(&b, "},\"finish_reason\":null}]}");
    int rc = sse_send(s, b.data);
    jbuf_free(&b);
    free(tmp);
    return rc;
}

/* POST /v1/chat/completions — streaming-aware front for route_chat.  If the body
 * has "stream": true, emit text/event-stream token deltas; else send one
 * buffered JSON response.  This handler owns the socket (writes status+headers). */
static void route_chat_stream(const netsrv_request_t *req, netsrv_stream_t *s, void *ctx) {
    (void)ctx;
    char cmd[1700], pf[64];
    const char *model = NULL, *errjson = NULL;
    int ptoks = 0;
    int st = chat_prepare(req, cmd, sizeof(cmd), pf, sizeof(pf), &model, &ptoks, &errjson);
    if (st != 200) {
        char hdr[256];
        int n = snprintf(hdr, sizeof(hdr),
                         "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\n"
                         "Content-Length: %zu\r\nConnection: close\r\n\r\n",
                         st, st == 503 ? "Service Unavailable" : "Bad Request", strlen(errjson));
        netsrv_stream_send(s, hdr, (size_t)n);
        netsrv_stream_send(s, errjson, strlen(errjson));
        return;
    }
    char stops[MAX_STOPS][STOP_LEN];
    int nstops = parse_stops(req->body, stops);
    size_t maxstop = 0;
    for (int i = 0; i < nstops; i++) {
        size_t l = strlen(stops[i]);
        if (l > maxstop)
            maxstop = l;
    }

    if (!json_get_bool(req->body, "stream", false)) {
        /* Buffered: run to completion, send a single JSON response ourselves. */
        char *out = shell_capture(cmd);
        unlink(pf);
        if (!out)
            out = strdup("");
        chat_strip_tail(out);
        size_t sp;
        if (nstops && find_stop(out, 0, strlen(out), stops, nstops, &sp))
            out[sp] = '\0';
        jbuf_t b;
        chat_build_json(&b, model, out, ptoks);
        free(out);
        char hdr[256];
        int n = snprintf(hdr, sizeof(hdr),
                         "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                         "Content-Length: %zu\r\nConnection: close\r\n\r\n",
                         b.len);
        netsrv_stream_send(s, hdr, (size_t)n);
        netsrv_stream_send(s, b.data, b.len);
        jbuf_free(&b);
        return;
    }

    /* SSE: headers, initial role delta, token deltas, stop delta, [DONE]. */
    const char *sse_hdr = "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
                          "Cache-Control: no-cache\r\nConnection: close\r\n\r\n";
    netsrv_stream_send(s, sse_hdr, strlen(sse_hdr));
    {
        jbuf_t b;
        jbuf_init(&b, 160);
        jbuf_append(&b,
                    "{\"id\":\"chatcmpl-dsco\",\"object\":\"chat.completion.chunk\",\"model\":");
        jbuf_append_json_str(&b, model);
        jbuf_append(&b, ",\"choices\":[{\"index\":0,\"delta\":{\"role\":\"assistant\"},"
                        "\"finish_reason\":null}]}");
        sse_send(s, b.data);
        jbuf_free(&b);
    }

    /* NB: do NOT unlink(pf) before the child reads it — the child opens the
     * prompt file after fork/exec; deleting it early feeds an empty prompt. */
    pid_t child = -1;
    int cfd = spawn_pipe(cmd, &child);
    size_t comp_bytes = 0; /* content bytes emitted, for usage accounting */
    if (cfd >= 0) {
        /* Accumulate output; emit a growing "safe" prefix as deltas.  Hold back
         * `guard` bytes so we never (a) split a multibyte UTF-8 char, (b) leak the
         * trailing "> EOF by user" marker, or (c) emit past a stop sequence that
         * only completes in a later read.  read() (not fread) flushes per token;
         * break on client disconnect. */
        enum { HOLD = 32 };
        size_t guard = HOLD > maxstop ? HOLD : maxstop;
        char *acc = NULL;
        size_t acclen = 0, acccap = 0, sent = 0;
        char buf[512];
        ssize_t r;
        int broken = 0, done = 0;
        while (!broken && !done && (r = read(cfd, buf, sizeof(buf))) > 0) {
            if (acclen + (size_t)r + 1 > acccap) {
                acccap = (acclen + (size_t)r + 1) * 2;
                char *na = realloc(acc, acccap);
                if (!na) {
                    broken = 1;
                    break;
                }
                acc = na;
            }
            memcpy(acc + acclen, buf, (size_t)r);
            acclen += (size_t)r;
            size_t sp;
            if (nstops && find_stop(acc, sent, acclen, stops, nstops, &sp)) {
                if (sp > sent) {
                    if (sse_content(s, model, acc + sent, sp - sent) != 0)
                        broken = 1;
                    else
                        comp_bytes += sp - sent;
                }
                sent = sp;
                done = 1;
                break;
            }
            if (acclen > sent + guard) {
                size_t emit = utf8_trunc(acc + sent, acclen - guard - sent);
                if (emit > 0) {
                    if (sse_content(s, model, acc + sent, emit) != 0)
                        broken = 1;
                    else {
                        sent += emit;
                        comp_bytes += emit;
                    }
                }
            }
        }
        if (!broken && !done) {
            /* natural EOF: strip trailing marker, honor a stop in the held tail,
             * then flush whatever remains. */
            if (!acc)
                acc = calloc(1, 1);
            if (acc) {
                acc[acclen] = '\0';
                chat_strip_tail(acc);
                acclen = strlen(acc);
                size_t sp;
                if (nstops && find_stop(acc, sent, acclen, stops, nstops, &sp))
                    acclen = sp;
                if (acclen > sent) {
                    sse_content(s, model, acc + sent, acclen - sent);
                    comp_bytes += acclen - sent;
                }
            }
        }
        free(acc);
        close(cfd);
        /* client disconnect OR stop-sequence hit → abandon the rest of the
         * generation now instead of burning compute on output nobody reads. */
        if (broken || done)
            kill(-child, SIGKILL);
        waitpid(child, NULL, 0);
    }
    unlink(pf);

    /* final chunk: finish_reason + usage (approx token counts, ~4 chars/token) */
    {
        int ctoks = (int)((comp_bytes + 3) / 4);
        jbuf_t b;
        jbuf_init(&b, 240);
        jbuf_append(&b,
                    "{\"id\":\"chatcmpl-dsco\",\"object\":\"chat.completion.chunk\",\"model\":");
        jbuf_append_json_str(&b, model);
        jbuf_append(&b, ",\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"stop\"}],"
                        "\"usage\":{\"prompt_tokens\":");
        jbuf_append_int(&b, ptoks);
        jbuf_append(&b, ",\"completion_tokens\":");
        jbuf_append_int(&b, ctoks);
        jbuf_append(&b, ",\"total_tokens\":");
        jbuf_append_int(&b, ptoks + ctoks);
        jbuf_append(&b, "}}");
        sse_send(s, b.data);
        jbuf_free(&b);
    }
    netsrv_stream_send(s, "data: [DONE]\n\n", 14);
}

/* GET /v1/models — OpenAI model listing.  Reports DSCO_SERVE_MODEL plus any
 * *.gguf under DSCO_SERVE_MODELS_DIR, so SDKs that probe /v1/models on init
 * (many do) see a valid catalog. */
static netsrv_response_t route_models(const netsrv_request_t *req, void *ctx) {
    (void)req;
    (void)ctx;
    jbuf_t b;
    jbuf_init(&b, 256);
    jbuf_append(&b, "{\"object\":\"list\",\"data\":[");
    int n = 0;
    const char *sm = getenv("DSCO_SERVE_MODEL");
    if (sm && sm[0]) {
        const char *base = strrchr(sm, '/');
        base = base ? base + 1 : sm;
        jbuf_append(&b, "{\"id\":");
        jbuf_append_json_str(&b, base);
        jbuf_append(&b, ",\"object\":\"model\",\"created\":0,\"owned_by\":\"dsco\"}");
        n++;
    }
    const char *md = getenv("DSCO_SERVE_MODELS_DIR");
    if (md && md[0]) {
        DIR *d = opendir(md);
        if (d) {
            struct dirent *e;
            while ((e = readdir(d))) {
                size_t l = strlen(e->d_name);
                if (l < 6 || strcmp(e->d_name + l - 5, ".gguf") != 0)
                    continue;
                if (n)
                    jbuf_append(&b, ",");
                jbuf_append(&b, "{\"id\":");
                jbuf_append_json_str(&b, e->d_name);
                jbuf_append(&b, ",\"object\":\"model\",\"created\":0,\"owned_by\":\"dsco\"}");
                n++;
            }
            closedir(d);
        }
    }
    jbuf_append(&b, "]}");
    return (netsrv_response_t){.status = 200, .body = b.data, .heap_body = true};
}

#endif /* HAVE_MBEDTLS && HAVE_LIBSODIUM */

void dsco_net_routes_register(void *srv_opaque) {
#if defined(HAVE_MBEDTLS) && defined(HAVE_LIBSODIUM)
    dsco_net_server_t *srv = (dsco_net_server_t *)srv_opaque;
    netsrv_route(srv, "GET", "/health", route_health, NULL);
    netsrv_route(srv, "POST", "/tool", route_tool, NULL);
    netsrv_route_stream(srv, "POST", "/v1/chat/completions", route_chat_stream, NULL);
    netsrv_route(srv, "GET", "/v1/models", route_models, NULL);
    netsrv_route(srv, "GET", "/mesh/peers", route_mesh_peers, NULL);
#else
    (void)srv_opaque;
#endif
}
