#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE 1
#endif

#include "agent_interop.h"

#include "json_util.h"
#include "process_capture.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

static const char *const k_codex_prefix[] = {
    "exec", "--json", "--approve-for-me", "--ephemeral",
    "--skip-git-repo-check", NULL,
};
static const char *const k_claude_prefix[] = {
    "-p", "--output-format", "stream-json", "--permission-mode", "dontAsk",
    "--no-session-persistence", "--verbose", NULL,
};
static const char *const k_opencode_prefix[] = {"run", "--format", "json", NULL};
static const char *const k_omp_prefix[] = {
    "-p", "--mode", "json", "--no-session", "--approval-mode", "write", NULL,
};
static const char *const k_hermes_prefix[] = {"chat", NULL};

static const char *const k_opencode_acp[] = {"acp", NULL};
static const char *const k_omp_acp[] = {"acp", NULL};

static const agent_interop_adapter_t k_adapters[] = {
    {.id = "codex",
     .label = "OpenAI Codex",
     .binary = "codex",
     .aliases = "openai-codex,codex-cli",
     .capabilities = AGENT_INTEROP_CLI | AGENT_INTEROP_MCP_CLIENT,
     .model_flag = "-m",
     .cwd_flag = "-C",
     .prefix = k_codex_prefix},
    {.id = "claude-code",
     .label = "Anthropic Claude Code",
     .binary = "claude",
     .aliases = "claude,anthropic-claude-code",
     .capabilities = AGENT_INTEROP_CLI | AGENT_INTEROP_MCP_CLIENT,
     .model_flag = "--model",
     .prefix = k_claude_prefix},
    {.id = "opencode",
     .label = "OpenCode",
     .binary = "opencode",
     .aliases = "anomalyco-opencode",
     .capabilities = AGENT_INTEROP_CLI | AGENT_INTEROP_ACP |
                     AGENT_INTEROP_MCP_CLIENT | AGENT_INTEROP_HTTP,
     .model_flag = "-m",
     .cwd_flag = "--dir",
     .prefix = k_opencode_prefix,
     .acp_args = k_opencode_acp},
    {.id = "omp",
     .label = "oh-my-pi",
     .binary = "omp",
     .aliases = "oh-my-pi,pi",
     .capabilities = AGENT_INTEROP_CLI | AGENT_INTEROP_ACP |
                     AGENT_INTEROP_MCP_CLIENT,
     .model_flag = "--model",
     .cwd_flag = "--cwd",
     .prefix = k_omp_prefix,
     .acp_args = k_omp_acp},
    {.id = "hermes",
     .label = "Nous Hermes Agent",
     .binary = "hermes",
     .aliases = "hermes-agent,nous-hermes",
     .capabilities = AGENT_INTEROP_CLI | AGENT_INTEROP_MCP_CLIENT,
     .model_flag = "-m",
     .prompt_flag = "-q",
     .prefix = k_hermes_prefix},
};

static bool alias_matches(const char *aliases, const char *name) {
    if (!aliases || !name || !name[0])
        return false;
    size_t wanted = strlen(name);
    for (const char *p = aliases; *p;) {
        const char *comma = strchr(p, ',');
        size_t n = comma ? (size_t)(comma - p) : strlen(p);
        if (n == wanted && strncasecmp(p, name, n) == 0)
            return true;
        if (!comma)
            break;
        p = comma + 1;
    }
    return false;
}

size_t agent_interop_adapter_count(void) {
    return sizeof(k_adapters) / sizeof(k_adapters[0]);
}

const agent_interop_adapter_t *agent_interop_adapter_at(size_t index) {
    return index < agent_interop_adapter_count() ? &k_adapters[index] : NULL;
}

const agent_interop_adapter_t *agent_interop_find(const char *name) {
    if (!name || !name[0])
        return NULL;
    for (size_t i = 0; i < agent_interop_adapter_count(); i++) {
        const agent_interop_adapter_t *adapter = &k_adapters[i];
        if (strcasecmp(adapter->id, name) == 0 ||
            strcasecmp(adapter->binary, name) == 0 ||
            alias_matches(adapter->aliases, name))
            return adapter;
    }
    return NULL;
}

static bool executable_at(const char *dir, size_t dir_len, const char *binary,
                          char *out, size_t out_len) {
    if (!dir || !dir_len || !binary || !out || !out_len || dir_len > PATH_MAX - 2)
        return false;
    char path[PATH_MAX];
    int n = snprintf(path, sizeof(path), "%.*s/%s", (int)dir_len, dir, binary);
    if (n < 0 || (size_t)n >= sizeof(path) || access(path, X_OK) != 0)
        return false;
    if ((size_t)n >= out_len)
        return false;
    snprintf(out, out_len, "%s", path);
    return true;
}

bool agent_interop_resolve_binary(const char *binary, char *out, size_t out_len) {
    if (!binary || !binary[0] || !out || out_len == 0)
        return false;
    out[0] = '\0';
    if (strchr(binary, '/')) {
        if (access(binary, X_OK) != 0)
            return false;
        if (strlen(binary) >= out_len)
            return false;
        snprintf(out, out_len, "%s", binary);
        return true;
    }

    const char *path = getenv("PATH");
    for (const char *p = path ? path : ""; *p;) {
        const char *colon = strchr(p, ':');
        size_t n = colon ? (size_t)(colon - p) : strlen(p);
        if (n && executable_at(p, n, binary, out, out_len))
            return true;
        if (!colon)
            break;
        p = colon + 1;
    }

    const char *home = getenv("HOME");
    if (home && home[0]) {
        char local_bin[PATH_MAX];
        int n = snprintf(local_bin, sizeof(local_bin), "%s/.local/bin", home);
        if (n > 0 && (size_t)n < sizeof(local_bin) &&
            executable_at(local_bin, (size_t)n, binary, out, out_len))
            return true;
    }
    static const char *const common[] = {
        "/opt/homebrew/bin", "/usr/local/bin", "/usr/bin", NULL,
    };
    for (size_t i = 0; common[i]; i++)
        if (executable_at(common[i], strlen(common[i]), binary, out, out_len))
            return true;
    return false;
}

static bool argv_push(agent_interop_argv_t *out, const char *value) {
    if (!out || !value || out->argc + 1 >= AGENT_INTEROP_ARGV_MAX)
        return false;
    out->argv[out->argc] = strdup(value);
    if (!out->argv[out->argc])
        return false;
    out->argc++;
    out->argv[out->argc] = NULL;
    return true;
}

void agent_interop_argv_free(agent_interop_argv_t *argv) {
    if (!argv)
        return;
    for (int i = 0; i < argv->argc; i++)
        free(argv->argv[i]);
    memset(argv, 0, sizeof(*argv));
}

static bool build_fail(agent_interop_argv_t *out, char *error, size_t error_len,
                       const char *message) {
    agent_interop_argv_free(out);
    if (error && error_len)
        snprintf(error, error_len, "%s", message);
    return false;
}

bool agent_interop_build_argv(const agent_interop_adapter_t *adapter,
                              const char *resolved_binary, const char *prompt,
                              const char *model, const char *cwd,
                              agent_interop_argv_t *out, char *error,
                              size_t error_len) {
    if (!out)
        return false;
    memset(out, 0, sizeof(*out));
    if (!adapter || !resolved_binary || !resolved_binary[0] || !prompt || !prompt[0])
        return build_fail(out, error, error_len, "adapter, binary, and prompt are required");
    if (!argv_push(out, resolved_binary))
        return build_fail(out, error, error_len, "argument capacity exhausted");
    for (size_t i = 0; adapter->prefix && adapter->prefix[i]; i++)
        if (!argv_push(out, adapter->prefix[i]))
            return build_fail(out, error, error_len, "argument capacity exhausted");
    if (cwd && cwd[0] && adapter->cwd_flag) {
        if (!argv_push(out, adapter->cwd_flag) || !argv_push(out, cwd))
            return build_fail(out, error, error_len, "argument capacity exhausted");
    }
    if (model && model[0] && adapter->model_flag) {
        if (!argv_push(out, adapter->model_flag) || !argv_push(out, model))
            return build_fail(out, error, error_len, "argument capacity exhausted");
    }
    if (adapter->prompt_flag && !argv_push(out, adapter->prompt_flag))
        return build_fail(out, error, error_len, "argument capacity exhausted");
    if (!argv_push(out, prompt))
        return build_fail(out, error, error_len, "argument capacity exhausted");
    return true;
}

static void append_capabilities(jbuf_t *b, unsigned caps) {
    struct cap_name { unsigned bit; const char *name; };
    static const struct cap_name names[] = {
        {AGENT_INTEROP_CLI, "cli"}, {AGENT_INTEROP_ACP, "acp"},
        {AGENT_INTEROP_MCP_CLIENT, "mcp_client"},
        {AGENT_INTEROP_MCP_SERVER, "mcp_server"}, {AGENT_INTEROP_HTTP, "http"},
    };
    jbuf_append(b, "[");
    bool comma = false;
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (!(caps & names[i].bit))
            continue;
        if (comma)
            jbuf_append(b, ",");
        jbuf_append_json_str(b, names[i].name);
        comma = true;
    }
    jbuf_append(b, "]");
}

static void append_command(jbuf_t *b, const char *binary,
                           const char *const *args) {
    jbuf_append(b, "[");
    jbuf_append_json_str(b, binary);
    for (size_t i = 0; args && args[i]; i++) {
        jbuf_append(b, ",");
        jbuf_append_json_str(b, args[i]);
    }
    jbuf_append(b, "]");
}

bool agent_interop_manifest_json(const char *dsco_binary, char *out, size_t out_len) {
    if (!out || out_len == 0)
        return false;
    const char *self = dsco_binary && dsco_binary[0] ? dsco_binary : "dsco";
    static const char *const mcp[] = {"mcp", "serve", NULL};
    static const char *const acp[] = {"acp", "serve", NULL};
    static const char *const prompt[] = {"--prompt", "<text>", NULL};
    static const char *const generic[] = {
        "interop", "run", "generic", "--prompt", "<text>", "--stdin", "--",
        "<command>", NULL,
    };
    jbuf_t b;
    jbuf_init(&b, 2048);
    jbuf_append(&b, "{\"schema\":\"dsco.agent.interop/v1\",\"agent\":\"dsco\",");
    jbuf_append(&b, "\"inbound\":[{\"protocol\":\"mcp\",\"transport\":\"stdio\",\"command\":");
    append_command(&b, self, mcp);
    jbuf_append(&b, "},{\"protocol\":\"acp\",\"transport\":\"stdio-jsonrpc\",\"command\":");
    append_command(&b, self, acp);
    jbuf_append(&b, "},{\"protocol\":\"headless\",\"transport\":\"process\",\"command\":");
    append_command(&b, self, prompt);
    jbuf_append(&b, "}],\"outbound\":[{\"protocol\":\"named-cli\",\"adapter_count\":");
    jbuf_append_int(&b, (long long)agent_interop_adapter_count());
    jbuf_append(&b, "},{\"protocol\":\"generic-argv-stdio\",\"command\":");
    append_command(&b, self, generic);
    jbuf_append(&b, "}],\"authority\":{\"operator_invoked\":true,");
    jbuf_append(&b, "\"autonomous_workers\":\"dsco-native\",\"shell_interpolation\":false},");
    jbuf_append(&b, "\"content\":{\"mcp_compatible_blocks\":true,\"unknown_fields\":\"tolerated\"}}\n");
    bool ok = b.data && b.len < out_len;
    if (ok)
        memcpy(out, b.data, b.len + 1);
    else
        snprintf(out, out_len, "{\"error\":\"manifest buffer too small\"}");
    jbuf_free(&b);
    return ok;
}

bool agent_interop_status_json(const char *dsco_binary, char *out, size_t out_len) {
    if (!out || out_len == 0)
        return false;
    jbuf_t b;
    jbuf_init(&b, 4096);
    jbuf_append(&b, "{\"schema\":\"dsco.agent.interop/v1\",\"adapters\":[");
    for (size_t i = 0; i < agent_interop_adapter_count(); i++) {
        const agent_interop_adapter_t *adapter = &k_adapters[i];
        char path[PATH_MAX];
        bool available = agent_interop_resolve_binary(adapter->binary, path, sizeof(path));
        if (i)
            jbuf_append(&b, ",");
        jbuf_append(&b, "{\"id\":");
        jbuf_append_json_str(&b, adapter->id);
        jbuf_append(&b, ",\"label\":");
        jbuf_append_json_str(&b, adapter->label);
        jbuf_appendf(&b, ",\"available\":%s,\"path\":", available ? "true" : "false");
        jbuf_append_json_str(&b, available ? path : "");
        jbuf_append(&b, ",\"capabilities\":");
        append_capabilities(&b, adapter->capabilities);
        jbuf_append(&b, "}");
    }
    jbuf_append(&b, "],\"generic\":{\"available\":true,\"prompt_transports\":[\"argv\",\"stdin\"]},\"dsco_binary\":");
    jbuf_append_json_str(&b, dsco_binary && dsco_binary[0] ? dsco_binary : "dsco");
    jbuf_append(&b, "}\n");
    bool ok = b.data && b.len < out_len;
    if (ok)
        memcpy(out, b.data, b.len + 1);
    else
        snprintf(out, out_len, "{\"error\":\"status buffer too small\"}");
    jbuf_free(&b);
    return ok;
}

static void usage(FILE *out, const char *self) {
    fprintf(out,
            "Usage:\n"
            "  %s interop status [--json]\n"
            "  %s interop manifest\n"
            "  %s interop mcp-config [json|toml] [--tier TIER]\n"
            "  %s interop acp-command [AGENT]\n"
            "  %s interop run AGENT --prompt TEXT [--model MODEL] [--cwd DIR] [--json]\n"
            "      [--timeout-ms N] [--max-output BYTES]\n"
            "  %s interop run generic --prompt TEXT [--stdin] [--json] -- COMMAND [ARGS...]\n",
            self, self, self, self, self, self);
}

static int print_mcp_config(const char *self, const char *format, const char *tier) {
    if (strcmp(tier, "standard") != 0 && strcmp(tier, "trusted") != 0 &&
        strcmp(tier, "untrusted") != 0) {
        fprintf(stderr, "dsco interop: MCP tier must be standard, trusted, or untrusted\n");
        return 2;
    }
    if (format && strcmp(format, "toml") == 0) {
        printf("[mcp_servers.dsco]\ncommand = \"");
        for (const char *p = self; *p; p++) {
            if (*p == '\\' || *p == '"') putchar('\\');
            putchar(*p);
        }
        printf("\"\nargs = [\"mcp\", \"serve\", \"--tier\", \"%s\"]\n", tier);
        return 0;
    }
    jbuf_t b;
    jbuf_init(&b, 512);
    jbuf_append(&b, "{\"mcpServers\":{\"dsco\":{\"command\":");
    jbuf_append_json_str(&b, self);
    jbuf_append(&b, ",\"args\":[\"mcp\",\"serve\",\"--tier\",");
    jbuf_append_json_str(&b, tier);
    jbuf_append(&b, "]}}}\n");
    fputs(b.data, stdout);
    jbuf_free(&b);
    return 0;
}

static int print_acp_command(const char *self, const char *name) {
    if (!name || !name[0] || strcmp(name, "dsco") == 0) {
        jbuf_t b;
        jbuf_init(&b, 256);
        static const char *const args[] = {"acp", "serve", NULL};
        append_command(&b, self, args);
        jbuf_append(&b, "\n");
        fputs(b.data, stdout);
        jbuf_free(&b);
        return 0;
    }
    const agent_interop_adapter_t *adapter = agent_interop_find(name);
    if (!adapter || !adapter->acp_args) {
        fprintf(stderr, "dsco interop: '%s' has no native ACP command; use its ACP adapter or generic bridge\n", name);
        return 2;
    }
    char path[PATH_MAX];
    if (!agent_interop_resolve_binary(adapter->binary, path, sizeof(path))) {
        fprintf(stderr, "dsco interop: executable '%s' not found\n", adapter->binary);
        return 127;
    }
    jbuf_t b;
    jbuf_init(&b, 256);
    append_command(&b, path, adapter->acp_args);
    jbuf_append(&b, "\n");
    fputs(b.data, stdout);
    jbuf_free(&b);
    return 0;
}

static bool replace_token(const char *arg, const char *prompt, const char *model,
                          const char *cwd, const char **replacement) {
    if (strcmp(arg, "{prompt}") == 0) {
        *replacement = prompt;
        return true;
    }
    if (strcmp(arg, "{model}") == 0) {
        *replacement = model ? model : "";
        return true;
    }
    if (strcmp(arg, "{cwd}") == 0) {
        *replacement = cwd ? cwd : "";
        return true;
    }
    *replacement = arg;
    return false;
}

static bool parse_int_range(const char *value, int minimum, int maximum,
                            int *out) {
    if (!value || !value[0] || !out)
        return false;
    errno = 0;
    char *end = NULL;
    long parsed = strtol(value, &end, 10);
    if (errno || !end || *end || parsed < minimum || parsed > maximum)
        return false;
    *out = (int)parsed;
    return true;
}

static bool parse_size_range(const char *value, size_t minimum, size_t maximum,
                             size_t *out) {
    if (!value || !value[0] || value[0] == '-' || !out)
        return false;
    errno = 0;
    char *end = NULL;
    unsigned long long parsed = strtoull(value, &end, 10);
    if (errno || !end || *end || parsed < minimum || parsed > maximum)
        return false;
    *out = (size_t)parsed;
    return true;
}

static int print_capture(const char *adapter, const process_capture_t *capture,
                         bool ok, bool json) {
    if (!json) {
        if (capture->output && capture->length)
            fwrite(capture->output, 1, capture->length, stdout);
        if (capture->length && capture->output[capture->length - 1] != '\n')
            putchar('\n');
        if (capture->spawn_error)
            fprintf(stderr, "dsco interop: %s\n", strerror(capture->spawn_error));
        else if (capture->timed_out)
            fprintf(stderr, "dsco interop: agent timed out\n");
        return ok ? 0 : (capture->exit_code >= 0 ? capture->exit_code : 1);
    }
    jbuf_t b;
    jbuf_init(&b, capture->length + 512);
    jbuf_append(&b, "{\"schema\":\"dsco.agent.interop/v1\",\"event\":\"run.completed\",\"adapter\":");
    jbuf_append_json_str(&b, adapter);
    jbuf_appendf(&b, ",\"ok\":%s,\"exit_code\":%d,\"timed_out\":%s,\"truncated\":%s,\"output\":",
                 ok ? "true" : "false", capture->exit_code,
                 capture->timed_out ? "true" : "false",
                 capture->truncated ? "true" : "false");
    process_capture_append_json_output(&b, capture);
    if (capture->spawn_error) {
        jbuf_append(&b, ",\"error\":");
        jbuf_append_json_str(&b, strerror(capture->spawn_error));
    }
    jbuf_append(&b, "}\n");
    fputs(b.data, stdout);
    jbuf_free(&b);
    return ok ? 0 : (capture->exit_code >= 0 ? capture->exit_code : 1);
}

static int run_agent(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "dsco interop: run requires an agent name\n");
        return 2;
    }
    const char *name = argv[3];
    const char *prompt = NULL, *model = NULL, *cwd = NULL;
    bool json = false, stdin_prompt = false;
    int timeout_ms = 600000;
    size_t max_output = 8 * 1024 * 1024;
    int passthrough = -1;
    for (int i = 4; i < argc; i++) {
        if (strcmp(argv[i], "--") == 0) {
            passthrough = i + 1;
            break;
        }
        if ((strcmp(argv[i], "--prompt") == 0 || strcmp(argv[i], "-p") == 0) && i + 1 < argc)
            prompt = argv[++i];
        else if ((strcmp(argv[i], "--model") == 0 || strcmp(argv[i], "-m") == 0) && i + 1 < argc)
            model = argv[++i];
        else if (strcmp(argv[i], "--cwd") == 0 && i + 1 < argc)
            cwd = argv[++i];
        else if (strcmp(argv[i], "--timeout-ms") == 0 && i + 1 < argc) {
            if (!parse_int_range(argv[++i], 1, 24 * 60 * 60 * 1000,
                                 &timeout_ms)) {
                fprintf(stderr, "dsco interop: --timeout-ms is invalid\n");
                return 2;
            }
        } else if (strcmp(argv[i], "--max-output") == 0 && i + 1 < argc) {
            if (!parse_size_range(argv[++i], 1, 16 * 1024 * 1024,
                                  &max_output)) {
                fprintf(stderr, "dsco interop: --max-output is invalid\n");
                return 2;
            }
        }
        else if (strcmp(argv[i], "--json") == 0)
            json = true;
        else if (strcmp(argv[i], "--stdin") == 0)
            stdin_prompt = true;
        else {
            fprintf(stderr, "dsco interop: unknown run option '%s'\n", argv[i]);
            return 2;
        }
    }
    if (!prompt || !prompt[0]) {
        fprintf(stderr, "dsco interop: --prompt is required\n");
        return 2;
    }
    if (cwd && chdir(cwd) != 0) {
        fprintf(stderr, "dsco interop: cannot enter '%s': %s\n", cwd, strerror(errno));
        return 2;
    }

    agent_interop_argv_t command;
    memset(&command, 0, sizeof(command));
    const agent_interop_adapter_t *adapter = NULL;
    char binary[PATH_MAX], error[160];
    if (strcmp(name, "generic") == 0) {
        if (passthrough < 0 || passthrough >= argc) {
            fprintf(stderr, "dsco interop: generic run requires -- COMMAND [ARGS...]\n");
            return 2;
        }
        if (!agent_interop_resolve_binary(argv[passthrough], binary, sizeof(binary))) {
            fprintf(stderr, "dsco interop: executable '%s' not found\n", argv[passthrough]);
            return 127;
        }
        if (!argv_push(&command, binary))
            return 1;
        bool inserted_prompt = false;
        for (int i = passthrough + 1; i < argc; i++) {
            const char *value = NULL;
            bool replaced = replace_token(argv[i], prompt, model, cwd, &value);
            if (replaced && strcmp(argv[i], "{prompt}") == 0)
                inserted_prompt = true;
            if (!argv_push(&command, value)) {
                agent_interop_argv_free(&command);
                return 1;
            }
        }
        if (!stdin_prompt && !inserted_prompt && !argv_push(&command, prompt)) {
            agent_interop_argv_free(&command);
            return 1;
        }
        command.prompt_on_stdin = stdin_prompt;
    } else {
        if (stdin_prompt) {
            fprintf(stderr, "dsco interop: --stdin is supported by the generic adapter\n");
            return 2;
        }
        if (passthrough >= 0) {
            fprintf(stderr, "dsco interop: passthrough argv is available through the generic adapter\n");
            return 2;
        }
        adapter = agent_interop_find(name);
        if (!adapter) {
            fprintf(stderr, "dsco interop: unknown adapter '%s'; use 'generic' for any executable\n", name);
            return 2;
        }
        if (!agent_interop_resolve_binary(adapter->binary, binary, sizeof(binary))) {
            fprintf(stderr, "dsco interop: executable '%s' not found\n", adapter->binary);
            return 127;
        }
        if (!agent_interop_build_argv(adapter, binary, prompt, model, cwd, &command,
                                      error, sizeof(error))) {
            fprintf(stderr, "dsco interop: %s\n", error);
            return 2;
        }
    }

    process_capture_t capture;
    bool ok = command.prompt_on_stdin
                  ? process_capture_input(binary, command.argv, prompt, strlen(prompt),
                                          timeout_ms, max_output, &capture)
                  : process_capture(binary, command.argv, timeout_ms, max_output, &capture);
    int rc = print_capture(adapter ? adapter->id : "generic", &capture, ok, json);
    process_capture_free(&capture);
    agent_interop_argv_free(&command);
    return rc;
}

int agent_interop_cli(int argc, char **argv, const char *dsco_binary) {
    const char *self = dsco_binary && dsco_binary[0] ? dsco_binary : "dsco";
    if (argc < 3 || strcmp(argv[2], "help") == 0 || strcmp(argv[2], "--help") == 0) {
        usage(argc < 3 ? stderr : stdout, self);
        return argc < 3 ? 2 : 0;
    }
    if (strcmp(argv[2], "manifest") == 0) {
        if (argc != 3) {
            fprintf(stderr, "dsco interop: manifest takes no arguments\n");
            return 2;
        }
        char out[8192];
        if (!agent_interop_manifest_json(self, out, sizeof(out)))
            return 1;
        fputs(out, stdout);
        return 0;
    }
    if (strcmp(argv[2], "status") == 0 || strcmp(argv[2], "list") == 0) {
        if (argc > 4 || (argc == 4 && strcmp(argv[3], "--json") != 0)) {
            fprintf(stderr, "dsco interop: status accepts only --json\n");
            return 2;
        }
        char out[16384];
        if (!agent_interop_status_json(self, out, sizeof(out)))
            return 1;
        fputs(out, stdout);
        return 0;
    }
    if (strcmp(argv[2], "mcp-config") == 0) {
        const char *format = "json", *tier = "trusted";
        for (int i = 3; i < argc; i++) {
            if ((strcmp(argv[i], "json") == 0 || strcmp(argv[i], "toml") == 0))
                format = argv[i];
            else if (strcmp(argv[i], "--tier") == 0 && i + 1 < argc)
                tier = argv[++i];
            else {
                fprintf(stderr, "dsco interop: invalid mcp-config option '%s'\n", argv[i]);
                return 2;
            }
        }
        return print_mcp_config(self, format, tier);
    }
    if (strcmp(argv[2], "acp-command") == 0) {
        if (argc > 4) {
            fprintf(stderr, "dsco interop: acp-command accepts at most one agent\n");
            return 2;
        }
        return print_acp_command(self, argc > 3 ? argv[3] : NULL);
    }
    if (strcmp(argv[2], "run") == 0)
        return run_agent(argc, argv);
    usage(stderr, self);
    return 2;
}
