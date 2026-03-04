#include "mcp.h"
#include "json_util.h"
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/time.h>

/* ── JSON-RPC helpers ──────────────────────────────────────────────────── */

static double mcp_now_sec(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec / 1e6;
}

static char *rpc_build(int id, const char *method, const char *params) {
    jbuf_t b;
    jbuf_init(&b, 1024);
    jbuf_append(&b, "{\"jsonrpc\":\"2.0\",\"id\":");
    jbuf_append_int(&b, id);
    jbuf_append(&b, ",\"method\":");
    jbuf_append_json_str(&b, method);
    if (params) {
        jbuf_append(&b, ",\"params\":");
        jbuf_append(&b, params);
    }
    jbuf_append(&b, "}\n");
    return b.data;
}

/* Read a single JSON-RPC response line from fd (blocking with timeout) */
static char *rpc_read_response(int fd, int timeout_ms) {
    struct pollfd pfd = { .fd = fd, .events = POLLIN };

    jbuf_t line;
    jbuf_init(&line, 4096);

    if (timeout_ms <= 0) timeout_ms = 10000;
    double deadline = mcp_now_sec() + (timeout_ms / 1000.0);

    while (1) {
        double now = mcp_now_sec();
        int remain_ms = (int)((deadline - now) * 1000.0);
        if (remain_ms <= 0) { jbuf_free(&line); return NULL; }
        int rc = poll(&pfd, 1, remain_ms);
        if (rc < 0) {
            if (errno == EINTR) continue;
            jbuf_free(&line);
            return NULL;
        }
        if (rc == 0) { jbuf_free(&line); return NULL; }

        char buf[4096];
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            jbuf_free(&line);
            return NULL;
        }
        if (n == 0) { jbuf_free(&line); return NULL; }
        buf[n] = '\0';

        /* Look for newline — each message is one JSON line */
        for (ssize_t i = 0; i < n; i++) {
            if (buf[i] == '\n') {
                if (line.len > 0 &&
                    line.data[0] == '{' &&
                    line.data[line.len - 1] == '}') {
                    return line.data;
                }
                jbuf_reset(&line);  /* ignore non-JSON or partial lines */
                continue;
            }
            jbuf_append_char(&line, buf[i]);
        }

        /* If we accumulated a complete JSON object without newline */
        if (line.len > 2 && line.data[0] == '{' && line.data[line.len - 1] == '}') {
            /* Quick check: does it end with } ? */
            return line.data;
        }
    }
}

/* ── Server lifecycle ──────────────────────────────────────────────────── */

static bool spawn_server(mcp_server_t *srv) {
    int in_pipe[2], out_pipe[2];
    if (pipe(in_pipe) < 0 || pipe(out_pipe) < 0) return false;

    pid_t pid = fork();
    if (pid < 0) {
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        return false;
    }

    if (pid == 0) {
        /* Child: stdin from in_pipe[0], stdout to out_pipe[1] */
        dup2(in_pipe[0], STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);

        /* Build argv */
        char *argv[16];
        argv[0] = srv->command;
        for (int i = 0; i < srv->argc && i < 14; i++)
            argv[i + 1] = srv->args[i];
        argv[srv->argc + 1] = NULL;

        execvp(srv->command, argv);
        _exit(127);
    }

    /* Parent */
    close(in_pipe[0]);
    close(out_pipe[1]);
    srv->pid = pid;
    srv->stdin_fd = in_pipe[1];
    srv->stdout_fd = out_pipe[0];

    /* Set stdout_fd non-blocking for reads */
    int flags = fcntl(srv->stdout_fd, F_GETFL, 0);
    if (flags >= 0) fcntl(srv->stdout_fd, F_SETFL, flags | O_NONBLOCK);

    return true;
}

static bool initialize_server(mcp_server_t *srv) {
    srv->rpc_id = 1;

    /* Send initialize request */
    char *init_req = rpc_build(srv->rpc_id++, "initialize",
        "{\"protocolVersion\":\"2024-11-05\","
        "\"capabilities\":{\"tools\":{}},"
        "\"clientInfo\":{\"name\":\"dsco\",\"version\":\"" DSCO_VERSION "\"}}");

    ssize_t w = write(srv->stdin_fd, init_req, strlen(init_req));
    free(init_req);
    if (w <= 0) return false;

    /* Read initialize response */
    char *resp = rpc_read_response(srv->stdout_fd, 10000);
    if (!resp) return false;
    free(resp);

    /* Send initialized notification */
    const char *notif = "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}\n";
    write(srv->stdin_fd, notif, strlen(notif));

    srv->initialized = true;
    return true;
}

static void stop_server(mcp_server_t *srv) {
    if (srv->pid <= 0) return;

    if (srv->stdin_fd >= 0) {
        close(srv->stdin_fd);
        srv->stdin_fd = -1;
    }
    if (srv->stdout_fd >= 0) {
        close(srv->stdout_fd);
        srv->stdout_fd = -1;
    }

    kill(srv->pid, SIGTERM);
    int status;
    waitpid(srv->pid, &status, WNOHANG);
    srv->pid = 0;
    srv->initialized = false;
}

/* ── Tool discovery ────────────────────────────────────────────────────── */

typedef struct {
    mcp_registry_t *reg;
    int server_idx;
} tools_list_ctx_t;

static void parse_tool_entry(const char *json, void *ctx) {
    tools_list_ctx_t *tlc = (tools_list_ctx_t *)ctx;
    mcp_registry_t *reg = tlc->reg;
    if (reg->tool_count >= MCP_MAX_TOOLS) return;

    mcp_tool_t *tool = &reg->tools[reg->tool_count];
    memset(tool, 0, sizeof(*tool));

    char *name = json_get_str(json, "name");
    char *desc = json_get_str(json, "description");
    char *schema = json_get_raw(json, "inputSchema");

    if (name) {
        snprintf(tool->name, sizeof(tool->name), "mcp_%s_%s",
                 reg->servers[tlc->server_idx].name, name);
        free(name);
    }
    if (desc) {
        snprintf(tool->description, sizeof(tool->description), "%s", desc);
        free(desc);
    }
    if (schema) {
        snprintf(tool->input_schema, sizeof(tool->input_schema), "%s", schema);
        free(schema);
    } else {
        snprintf(tool->input_schema, sizeof(tool->input_schema),
                 "{\"type\":\"object\",\"properties\":{}}");
    }
    tool->server_idx = tlc->server_idx;
    reg->tool_count++;
}

static int discover_tools(mcp_registry_t *reg, int server_idx) {
    mcp_server_t *srv = &reg->servers[server_idx];
    if (!srv->initialized) return 0;

    char *req = rpc_build(srv->rpc_id++, "tools/list", "{}");
    ssize_t w = write(srv->stdin_fd, req, strlen(req));
    free(req);
    if (w <= 0) return 0;

    char *resp = rpc_read_response(srv->stdout_fd, 10000);
    if (!resp) return 0;

    /* Parse result.tools array */
    char *result = json_get_raw(resp, "result");
    if (!result) { free(resp); return 0; }

    tools_list_ctx_t ctx = { .reg = reg, .server_idx = server_idx };
    int before = reg->tool_count;
    json_array_foreach(result, "tools", parse_tool_entry, &ctx);
    int discovered = reg->tool_count - before;

    free(result);
    free(resp);
    return discovered;
}

/* ── Public API ────────────────────────────────────────────────────────── */

int mcp_init(mcp_registry_t *reg) {
    memset(reg, 0, sizeof(*reg));

    char config_path[512];
    const char *home = getenv("HOME");
    if (!home) return 0;
    snprintf(config_path, sizeof(config_path), "%s/.dsco/mcp.json", home);

    FILE *f = fopen(config_path, "r");
    if (!f) return 0;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz <= 0 || sz > 64 * 1024) { fclose(f); return 0; }
    fseek(f, 0, SEEK_SET);

    char *data = safe_malloc((size_t)sz + 1);
    size_t nr = fread(data, 1, (size_t)sz, f);
    data[nr] = '\0';
    fclose(f);

    /* Parse servers object */
    char *servers_raw = json_get_raw(data, "servers");
    if (!servers_raw && json_get_raw(data, "mcpServers")) {
        free(servers_raw);
        servers_raw = json_get_raw(data, "mcpServers");
    }
    free(data);
    if (!servers_raw) return 0;

    /* Simple parsing: iterate top-level keys of servers object */
    const char *p = servers_raw;
    while (*p && *p != '{') p++;
    if (*p == '{') p++;

    while (*p && reg->server_count < MCP_MAX_SERVERS) {
        while (*p && *p != '"' && *p != '}') p++;
        if (*p != '"') break;

        /* parse the key */
        char *key = NULL;
        {
            jbuf_t kb;
            jbuf_init(&kb, 128);
            p++; /* skip opening " */
            while (*p && *p != '"') {
                if (*p == '\\') { p++; if (*p) jbuf_append_char(&kb, *p); }
                else jbuf_append_char(&kb, *p);
                p++;
            }
            if (*p == '"') p++;
            key = kb.data;
        }

        /* skip to colon */
        while (*p && *p != ':') p++;
        if (*p == ':') p++;
        while (*p && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) p++;

        if (*p != '{') { free(key); continue; }

        /* Extract the server config object */
        const char *obj_start = p;
        int depth = 1;
        p++;
        while (*p && depth > 0) {
            if (*p == '{') depth++;
            else if (*p == '}') depth--;
            else if (*p == '"') {
                p++;
                while (*p && *p != '"') { if (*p == '\\') p++; p++; }
            }
            p++;
        }
        size_t obj_len = (size_t)(p - obj_start);
        char *obj = safe_malloc(obj_len + 1);
        memcpy(obj, obj_start, obj_len);
        obj[obj_len] = '\0';

        mcp_server_t *srv = &reg->servers[reg->server_count];
        memset(srv, 0, sizeof(*srv));
        snprintf(srv->name, sizeof(srv->name), "%s", key);

        char *cmd = json_get_str(obj, "command");
        if (cmd) {
            snprintf(srv->command, sizeof(srv->command), "%s", cmd);
            free(cmd);
        }

        /* Args are optional — skip for now (most MCP servers take no args) */
        srv->argc = 0;

        free(obj);
        free(key);

        if (srv->command[0]) {
            fprintf(stderr, "  \033[2mmcp: spawning %s (%s)\033[0m\n", srv->name, srv->command);
            if (spawn_server(srv) && initialize_server(srv)) {
                int n = discover_tools(reg, reg->server_count);
                fprintf(stderr, "  \033[2mmcp: %s: %d tools discovered\033[0m\n", srv->name, n);
                reg->server_count++;
            } else {
                fprintf(stderr, "  \033[31mmcp: failed to start %s\033[0m\n", srv->name);
                stop_server(srv);
            }
        }

        /* skip comma */
        while (*p && (*p == ',' || *p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) p++;
    }

    free(servers_raw);
    reg->loaded = true;
    return reg->tool_count;
}

void mcp_shutdown(mcp_registry_t *reg) {
    for (int i = 0; i < reg->server_count; i++) {
        stop_server(&reg->servers[i]);
    }
    reg->server_count = 0;
    reg->tool_count = 0;
}

const mcp_tool_t *mcp_get_tools(mcp_registry_t *reg, int *count) {
    *count = reg->tool_count;
    return reg->tools;
}

char *mcp_call_tool(mcp_registry_t *reg, const char *tool_name,
                     const char *arguments_json) {
    /* Find the tool */
    int tool_idx = -1;
    for (int i = 0; i < reg->tool_count; i++) {
        if (strcmp(reg->tools[i].name, tool_name) == 0) {
            tool_idx = i;
            break;
        }
    }
    if (tool_idx < 0) return NULL;

    mcp_tool_t *tool = &reg->tools[tool_idx];
    mcp_server_t *srv = &reg->servers[tool->server_idx];
    if (!srv->initialized) return NULL;

    /* Strip mcp_<server>_ prefix to get original tool name */
    const char *orig_name = tool_name;
    char prefix[256];
    snprintf(prefix, sizeof(prefix), "mcp_%s_", srv->name);
    if (strncmp(tool_name, prefix, strlen(prefix)) == 0) {
        orig_name = tool_name + strlen(prefix);
    }

    /* Build tools/call request */
    jbuf_t params;
    jbuf_init(&params, 1024);
    jbuf_append(&params, "{\"name\":");
    jbuf_append_json_str(&params, orig_name);
    jbuf_append(&params, ",\"arguments\":");
    jbuf_append(&params, arguments_json ? arguments_json : "{}");
    jbuf_append(&params, "}");

    char *req = rpc_build(srv->rpc_id++, "tools/call", params.data);
    jbuf_free(&params);

    ssize_t w = write(srv->stdin_fd, req, strlen(req));
    free(req);
    if (w <= 0) return NULL;

    /* Read response */
    char *resp = rpc_read_response(srv->stdout_fd, 30000);
    if (!resp) return NULL;

    /* Extract result.content[0].text */
    char *result = json_get_raw(resp, "result");
    if (!result) { free(resp); return safe_strdup("{\"error\":\"no result\"}"); }

    /* Try to get text content from result */
    char *content_text = NULL;
    /* result might have content array */
    char *content_raw = json_get_raw(result, "content");
    if (content_raw) {
        /* First element text */
        content_text = json_get_str(content_raw, "text");
        if (!content_text) {
            /* Maybe it's an array — just return the raw content */
            content_text = safe_strdup(content_raw);
        }
        free(content_raw);
    }

    if (!content_text) content_text = safe_strdup(result);

    free(result);
    free(resp);
    return content_text;
}
