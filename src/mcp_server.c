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
#include "tool_content.h"
#include "gov_experiment.h"
#include "../vendor/yyjson.h"

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
    {"core", "bash,read_file,write_file,edit_file,list_directory,find_files,"
             "grep_files,run_command,dsco-python-3x,python,jq,http_request,persistent_directive,standing_directive,value_ledger"},
    {"terminal", "surface,kitty_remote,kitten,pty_session,buffer,buffer_view,native_window,ui_trace"},
    {"buffers", "buffer,buffer_view"},
    {"desktop", "desktop,computer,view_image,ui_render,native_window"},
    {"browser", "browser,browser_session"},
    {"ast", "ast_*,code_index,code_search,call_graph,dependency_graph,"
            "symbol_def,symbol_refs,inspect_file,api_outline"},
    {"swarm", "agent,swarm,topology_*,agent_wait"},
    {"market", "kalshi,polymarket,prediction,alpha_vantage,contract_*"},
    {"crypto", "sha256,md5,hmac,hkdf,uuid,jwt_decode,file_hash"},
    {"all", "*"},
};
static const int k_toolset_count = (int)(sizeof(k_toolsets) / sizeof(k_toolsets[0]));

static char g_enabled_sets[256] = "core";
/* External MCP callers default to the untrusted governance tier; operators
 * can raise via --tier trusted when they own both ends of the pipe. */
static char g_tier[64] = "untrusted";

static bool name_matches(const char *pattern, const char *name) {
    size_t plen = strlen(pattern);
    if (plen == 0)
        return false;
    if (pattern[plen - 1] == '*')
        return strncmp(pattern, name, plen - 1) == 0;
    return strcmp(pattern, name) == 0;
}

static bool set_allows(const char *names_csv, const char *tool) {
    char buf[1024];
    snprintf(buf, sizeof(buf), "%s", names_csv);
    char *save = NULL;
    for (char *tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        while (*tok == ' ')
            tok++;
        if (strcmp(tok, "*") == 0 || name_matches(tok, tool))
            return true;
    }
    return false;
}

static bool tool_exposed(const char *tool) {
    char sets[256];
    snprintf(sets, sizeof(sets), "%s", g_enabled_sets);
    char *save = NULL;
    for (char *tok = strtok_r(sets, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        while (*tok == ' ')
            tok++;
        for (int i = 0; i < k_toolset_count; i++) {
            if (strcmp(k_toolsets[i].set, tok) == 0 && set_allows(k_toolsets[i].names, tool))
                return true;
        }
    }
    return false;
}

/* Structured parsing uses the yyjson implementation already linked by json_fast.c.
 * Protocol names are C strings, so never silently accept an embedded NUL. */
static const char *protocol_string(yyjson_val *value) {
    const char *s = yyjson_get_str(value);
    return s && strlen(s) == yyjson_get_len(value) ? s : NULL;
}

/* Read bounded bytes, not strlen(fgets(...)): a literal NUL is invalid JSON,
 * not an opportunity to execute only the prefix of an invalid request. */
static int read_request_line(char *line, size_t capacity, size_t *length) {
    size_t used = 0;
    bool oversized = false;
    int c;
    flockfile(stdin);
    while ((c = getc_unlocked(stdin)) != EOF) {
        if (used + 1 < capacity) line[used++] = (char)c;
        else oversized = true;
        if (c == '\n') break;
    }
    funlockfile(stdin);
    line[used] = '\0';
    *length = used;
    return oversized ? -1 : (used ? 1 : 0);
}

/* Reject ambiguous reserved members instead of inheriting first/last-wins
 * behavior from different peers' JSON implementations. */
static bool unique_protocol_members(yyjson_val *root) {
    unsigned seen = 0;
    size_t index, count;
    yyjson_val *key, *value;
    yyjson_obj_foreach(root, index, count, key, value) {
        (void)value;
        unsigned bit = yyjson_equals_str(key, "jsonrpc") ? 1u :
                       yyjson_equals_str(key, "method") ? 2u :
                       yyjson_equals_str(key, "id") ? 4u :
                       yyjson_equals_str(key, "params") ? 8u : 0u;
        if (seen & bit) return false;
        seen |= bit;
    }
    return true;
}

static void json_escape_into(FILE *f, const char *s) {
    const char *run = s;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c >= 0x20 && c != '"' && c != '\\')
            continue;
        if (s > run) fwrite(run, 1, (size_t)(s - run), f);
        switch (c) {
            case '"':
                fputs("\\\"", f);
                break;
            case '\\':
                fputs("\\\\", f);
                break;
            case '\n':
                fputs("\\n", f);
                break;
            case '\r':
                fputs("\\r", f);
                break;
            case '\t':
                fputs("\\t", f);
                break;
            default:
                fprintf(f, "\\u%04x", c);
        }
        run = s + 1;
    }
    if (s > run) fwrite(run, 1, (size_t)(s - run), f);
}

/* ── responses ─────────────────────────────────────────────────────── */

static void send_result_prefix(const char *id_raw) {
    printf("{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":", id_raw[0] ? id_raw : "null");
}

static void send_error(const char *id_raw, int code, const char *msg) {
    if (!id_raw) return; /* valid notifications never receive a response */
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
        if (!defs[i].name || !tool_exposed(defs[i].name))
            continue;
        if (!first)
            printf(",");
        first = false;
        printf("{\"name\":\"");
        json_escape_into(stdout, defs[i].name);
        printf("\",\"description\":\"");
        json_escape_into(stdout, defs[i].description ? defs[i].description : "");
        printf("\",\"inputSchema\":%s}", defs[i].input_schema_json && defs[i].input_schema_json[0]
                                             ? defs[i].input_schema_json
                                             : "{\"type\":\"object\"}");
    }
    printf("]}}\n");
    fflush(stdout);
}

static void handle_tools_call(const char *id_raw, yyjson_val *params) {
    const char *name = protocol_string(yyjson_obj_get(params, "name"));
    if (!name || !name[0]) {
        send_error(id_raw, -32602, "missing or invalid tool name");
        return;
    }
    if (!tool_exposed(name)) {
        send_error(id_raw, -32602, "tool not exposed by enabled toolsets");
        return;
    }
    yyjson_val *arguments = yyjson_obj_get(params, "arguments");
    if (arguments && !yyjson_is_obj(arguments)) {
        send_error(id_raw, -32602, "arguments must be an object");
        return;
    }

    /* Immune gate: identical governance path as internal callers. */
    char reason[256];
    if (!tools_is_allowed_for_tier(name, g_tier, reason, sizeof(reason))) {
        send_error(id_raw, -32000, reason[0] ? reason : "governance_block");
        return;
    }
    size_t args_length = 2;
    char *args = arguments ? yyjson_val_write(arguments, 0, &args_length) : strdup("{}");
    if (args && args_length >= MCP_LINE_MAX) {
        free(args);
        send_error(id_raw, -32602, "arguments too large");
        return;
    }
    if (!args) {
        send_error(id_raw, -32603, "cannot serialize tool arguments");
        return;
    }
    static char result[MCP_RESULT_MAX];
    result[0] = '\0';
    tool_content_clear();
    bool ok = tools_execute_for_tier(name, args, g_tier, result, sizeof(result));
    free(args);
    if (!id_raw) {
        tool_content_clear();
        return;
    }

    send_result_prefix(id_raw);
    printf("{\"content\":[{\"type\":\"text\",\"text\":\"");
    json_escape_into(stdout, result);
    printf("\"}");
    tool_content_t *images = tool_content_take();
    for (tool_content_t *im = images; im; im = im->next) {
        printf(",{\"type\":\"image\",\"mimeType\":\"");
        json_escape_into(stdout, im->mime_type);
        printf("\",\"data\":\"");
        json_escape_into(stdout, im->data);
        printf("\"}");
    }
    tool_content_free(images);
    printf("],\"isError\":%s}}\n", ok ? "false" : "true");
    fflush(stdout);
}

/* ── main loop ─────────────────────────────────────────────────────── */

int mcp_server_run(const char *toolsets_csv, const char *tier) {
    /* Boundary hardening: the external MCP surface must have a deterministic
     * governance posture. Saved-env loading (~/.dsco/env profiles) and other
     * subsystems mutate DSCO_GOV_* mid-process, so without an explicit pin an
     * untrusted-tier server could silently come up ungoverned. Operators can
     * still opt out with --gov-model. */
    if (!getenv("DSCO_MCP_GOV_PINNED")) {
        setenv("DSCO_GOV_BYPASS", "0", 1);
        setenv("DSCO_GOV_MODEL", "standard", 1);
        setenv("DSCO_MCP_GOV_PINNED", "1", 1);
        /* Re-resolve: the lazy cache may already hold a stale decision. */
        gov_experiment_reset_cache();
    }
    if (toolsets_csv && toolsets_csv[0])
        snprintf(g_enabled_sets, sizeof(g_enabled_sets), "%s", toolsets_csv);
    if (tier && tier[0])
        snprintf(g_tier, sizeof(g_tier), "%s", tier);

    tools_init_local_only();

    static char line[MCP_LINE_MAX];
    size_t len;
    int line_status;
    while ((line_status = read_request_line(line, sizeof(line), &len)) != 0) {
        if (line_status < 0) {
            send_error("", -32600, "request line too large");
            continue;
        }
        if (len == 1 && line[0] == '\n') continue;
        /* Raw numbers preserve request ids and argument integers without a
         * double round-trip; default strict parsing consumes the whole line. */
        yyjson_doc *doc = yyjson_read(line, len, YYJSON_READ_NUMBER_AS_RAW);
        if (!doc) {
            send_error("", -32700, "parse error");
            continue;
        }
        yyjson_val *root = yyjson_doc_get_root(doc);
        if (!yyjson_is_obj(root) || !unique_protocol_members(root)) {
            send_error("", -32600, "invalid or ambiguous request object");
            yyjson_doc_free(doc);
            continue;
        }
        yyjson_val *id = yyjson_obj_get(root, "id");
        const char *version = protocol_string(yyjson_obj_get(root, "jsonrpc"));
        const char *method = protocol_string(yyjson_obj_get(root, "method"));
        if (!version || strcmp(version, "2.0") || !method ||
            (id && !yyjson_is_str(id) && !yyjson_is_raw(id) && !yyjson_is_null(id))) {
            send_error("", -32600, "invalid request");
            yyjson_doc_free(doc);
            continue;
        }
        char *id_raw = id ? yyjson_val_write(id, 0, NULL) : NULL;
        if (id && !id_raw) {
            send_error("", -32603, "cannot serialize request id");
            yyjson_doc_free(doc);
            continue;
        }
        yyjson_val *params = yyjson_obj_get(root, "params");
        if (params && !yyjson_is_obj(params)) {
            send_error(id_raw, -32602, "params must be an object");
        } else if (strcmp(method, "initialize") == 0) {
            if (id_raw) handle_initialize(id_raw);
        } else if (strcmp(method, "ping") == 0) {
            if (id_raw) {
                send_result_prefix(id_raw);
                printf("{}}\n");
                fflush(stdout);
            }
        } else if (strcmp(method, "tools/list") == 0) {
            if (id_raw) handle_tools_list(id_raw);
        } else if (strcmp(method, "tools/call") == 0) {
            if (params) handle_tools_call(id_raw, params);
            else send_error(id_raw, -32602, "missing params");
        } else if (id_raw) {
            send_error(id_raw, -32601, "method not found");
        }
        free(id_raw);
        yyjson_doc_free(doc);
    }
    return 0;
}
