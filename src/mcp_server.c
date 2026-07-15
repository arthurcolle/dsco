/* mcp_server.c — dsco as an MCP *server* (Plan 05, harness-parity)
 *
 * Exposes dsco's tool registry over MCP stdio JSON-RPC 2.0 so Claude Code,
 * Codex, opencode, goose, etc. can call dsco tools directly:
 *
 *   claude mcp add dsco -- dsco mcp serve --toolsets core,ast
 *
 * Protocol: initialize / notifications/initialized / tools/list / tools/call / ping.
 * Every tools/call routes through tools_execute_for_tier() so the Immune
 * System governance gate applies identically to external callers.
 *
 * Wiring: add to Makefile objects; in main.c dispatch
 *   `dsco mcp serve [--toolsets a,b] [--tier agent]` -> mcp_server_run(...)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <ctype.h>

#include "tools.h"

#define MCP_PROTOCOL_VERSION "2025-06-18"
#define MCP_SERVER_NAME "dsco"
#define MCP_SERVER_VERSION "2.1"
#define MCP_RESULT_MAX (1024 * 1024)
#define MCP_LINE_MAX (4 * 1024 * 1024)

/* ── toolset curation ──────────────────────────────────────────────────
 * Flag-gated toolsets (github-mcp-server pattern). "core" is the default.
 * A tool is exposed if it matches any enabled set's prefix/name list.   */
typedef struct {
    const char *set;
    const char *names; /* comma list; entries ending in '*' are prefixes */
} mcp_toolset_t;

static const mcp_toolset_t k_toolsets[] = {
    {"core",  "bash,read_file,write_file,edit_file,list_directory,find_files,"
              "grep_files,run_command,python,jq,http_request"},
    {"ast",   "ast_*,code_index,code_search,call_graph,dependency_graph,"
              "symbol_def,symbol_refs,inspect_file,api_outline"},
    {"swarm", "agent,swarm,topology_*,agent_wait"},
    {"market","kalshi,polymarket,prediction,alpha_vantage,contract_*"},
    {"crypto","sha256,md5,hmac,hkdf,uuid,jwt_decode,file_hash"},
    {"all",   "*"},
};
static const int k_toolset_count = (int)(sizeof(k_toolsets) / sizeof(k_toolsets[0]));

static char g_enabled_sets[256] = "core";
/* External MCP callers default to the untrusted governance tier; operators
 * can raise via --tier trusted when they own both ends of the pipe. */
static char g_tier[64] = "untrusted";

static bool name_matches(const char *pattern, const char *name) {
    size_t plen = strlen(pattern);
    if (plen == 0) return false;
    if (pattern[plen - 1] == '*') return strncmp(pattern, name, plen - 1) == 0;
    return strcmp(pattern, name) == 0;
}

static bool set_allows(const char *names_csv, const char *tool) {
    char buf[1024];
    snprintf(buf, sizeof(buf), "%s", names_csv);
    char *save = NULL;
    for (char *tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        while (*tok == ' ') tok++;
        if (strcmp(tok, "*") == 0 || name_matches(tok, tool)) return true;
    }
    return false;
}

static bool tool_exposed(const char *tool) {
    char sets[256];
    snprintf(sets, sizeof(sets), "%s", g_enabled_sets);
    char *save = NULL;
    for (char *tok = strtok_r(sets, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        while (*tok == ' ') tok++;
        for (int i = 0; i < k_toolset_count; i++) {
            if (strcmp(k_toolsets[i].set, tok) == 0 && set_allows(k_toolsets[i].names, tool))
                return true;
        }
    }
    return false;
}

/* ── minimal JSON helpers (scaffold-grade; swap for shared json.c) ───── */

static bool json_find_string(const char *json, const char *key, char *out, size_t olen) {
    char pat[128];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return false;
    p = strchr(p + strlen(pat), ':');
    if (!p) return false;
    p++;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != '"') return false;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < olen) {
        if (*p == '\\' && p[1]) { p++; }
        out[i++] = *p++;
    }
    out[i] = '\0';
    return true;
}

/* Extract raw JSON value (object/number/string) for key into out. */
static bool json_find_raw(const char *json, const char *key, char *out, size_t olen) {
    char pat[128];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return false;
    p = strchr(p + strlen(pat), ':');
    if (!p) return false;
    p++;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p == '{' || *p == '[') {
        char open = *p, close = (*p == '{') ? '}' : ']';
        int depth = 0; bool instr = false;
        const char *start = p;
        while (*p) {
            if (instr) {
                if (*p == '\\' && p[1]) p++;
                else if (*p == '"') instr = false;
            } else {
                if (*p == '"') instr = true;
                else if (*p == open) depth++;
                else if (*p == close && --depth == 0) { p++; break; }
            }
            p++;
        }
        size_t n = (size_t)(p - start);
        if (n + 1 > olen) return false;
        memcpy(out, start, n);
        out[n] = '\0';
        return true;
    }
    /* scalar */
    size_t i = 0;
    while (*p && *p != ',' && *p != '}' && *p != '\n' && i + 1 < olen) out[i++] = *p++;
    out[i] = '\0';
    while (i > 0 && isspace((unsigned char)out[i - 1])) out[--i] = '\0';
    return i > 0;
}

static void json_escape_into(FILE *f, const char *s) {
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        switch (c) {
        case '"': fputs("\\\"", f); break;
        case '\\': fputs("\\\\", f); break;
        case '\n': fputs("\\n", f); break;
        case '\r': fputs("\\r", f); break;
        case '\t': fputs("\\t", f); break;
        default:
            if (c < 0x20) fprintf(f, "\\u%04x", c);
            else fputc(c, f);
        }
    }
}

/* ── responses ─────────────────────────────────────────────────────── */

static void send_result_prefix(const char *id_raw) {
    printf("{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":", id_raw[0] ? id_raw : "null");
}

static void send_error(const char *id_raw, int code, const char *msg) {
    printf("{\"jsonrpc\":\"2.0\",\"id\":%s,\"error\":{\"code\":%d,\"message\":\"",
           id_raw[0] ? id_raw : "null", code);
    json_escape_into(stdout, msg);
    printf("\"}}\n");
    fflush(stdout);
}

static void handle_initialize(const char *id_raw) {
    send_result_prefix(id_raw);
    printf("{\"protocolVersion\":\"%s\",\"capabilities\":{\"tools\":{\"listChanged\":false}},"
           "\"serverInfo\":{\"name\":\"%s\",\"version\":\"%s\"}}}\n",
           MCP_PROTOCOL_VERSION, MCP_SERVER_NAME, MCP_SERVER_VERSION);
    fflush(stdout);
}

static void handle_tools_list(const char *id_raw) {
    int count = 0;
    const tool_def_t *defs = tools_get_all(&count);
    send_result_prefix(id_raw);
    printf("{\"tools\":[");
    bool first = true;
    for (int i = 0; i < count; i++) {
        if (!defs[i].name || !tool_exposed(defs[i].name)) continue;
        if (!first) printf(",");
        first = false;
        printf("{\"name\":\"");
        json_escape_into(stdout, defs[i].name);
        printf("\",\"description\":\"");
        json_escape_into(stdout, defs[i].description ? defs[i].description : "");
        printf("\",\"inputSchema\":%s}",
               defs[i].input_schema_json && defs[i].input_schema_json[0]
                   ? defs[i].input_schema_json
                   : "{\"type\":\"object\"}");
    }
    printf("]}}\n");
    fflush(stdout);
}

static void handle_tools_call(const char *id_raw, const char *params) {
    char name[256] = {0};
    if (!json_find_string(params, "name", name, sizeof(name))) {
        send_error(id_raw, -32602, "missing tool name");
        return;
    }
    if (!tool_exposed(name)) {
        send_error(id_raw, -32602, "tool not exposed by enabled toolsets");
        return;
    }
    static char args[MCP_LINE_MAX];
    if (!json_find_raw(params, "arguments", args, sizeof(args)))
        snprintf(args, sizeof(args), "{}");

    /* Immune gate: identical governance path as internal callers. */
    char reason[256];
    if (!tools_is_allowed_for_tier(name, g_tier, reason, sizeof(reason))) {
        send_error(id_raw, -32000, reason[0] ? reason : "governance_block");
        return;
    }
    size_t arglen = strnlen(args, sizeof(args));
    if (arglen >= sizeof(args) - 1) {
        send_error(id_raw, -32602, "arguments too large");
        return;
    }
    static char result[MCP_RESULT_MAX];
    result[0] = '\0';
    bool ok = tools_execute_for_tier(name, args, g_tier, result, sizeof(result));

    send_result_prefix(id_raw);
    printf("{\"content\":[{\"type\":\"text\",\"text\":\"");
    json_escape_into(stdout, result);
    printf("\"}],\"isError\":%s}}\n", ok ? "false" : "true");
    fflush(stdout);
}

/* ── main loop ─────────────────────────────────────────────────────── */

int mcp_server_run(const char *toolsets_csv, const char *tier) {
    if (toolsets_csv && toolsets_csv[0])
        snprintf(g_enabled_sets, sizeof(g_enabled_sets), "%s", toolsets_csv);
    if (tier && tier[0])
        snprintf(g_tier, sizeof(g_tier), "%s", tier);

    tools_init_local_only();

    static char line[MCP_LINE_MAX];
    while (fgets(line, sizeof(line), stdin)) {
        if (line[0] == '\n' || line[0] == '\0') continue;

        char method[128] = {0}, id_raw[64] = {0};
        json_find_string(line, "method", method, sizeof(method));
        json_find_raw(line, "id", id_raw, sizeof(id_raw));

        if (strcmp(method, "initialize") == 0) {
            handle_initialize(id_raw);
        } else if (strcmp(method, "notifications/initialized") == 0 ||
                   strncmp(method, "notifications/", 14) == 0) {
            /* notifications: no response */
        } else if (strcmp(method, "ping") == 0) {
            send_result_prefix(id_raw);
            printf("{}}\n");
            fflush(stdout);
        } else if (strcmp(method, "tools/list") == 0) {
            handle_tools_list(id_raw);
        } else if (strcmp(method, "tools/call") == 0) {
            static char params[MCP_LINE_MAX];
            if (json_find_raw(line, "params", params, sizeof(params)))
                handle_tools_call(id_raw, params);
            else
                send_error(id_raw, -32602, "missing params");
        } else if (id_raw[0]) {
            send_error(id_raw, -32601, "method not found");
        }
    }
    return 0;
}
