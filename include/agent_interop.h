#ifndef DSCO_AGENT_INTEROP_H
#define DSCO_AGENT_INTEROP_H

#include <stdbool.h>
#include <stddef.h>

#define AGENT_INTEROP_ARGV_MAX 64

typedef enum {
    AGENT_INTEROP_CLI = 1u << 0,
    AGENT_INTEROP_ACP = 1u << 1,
    AGENT_INTEROP_MCP_CLIENT = 1u << 2,
    AGENT_INTEROP_MCP_SERVER = 1u << 3,
    AGENT_INTEROP_HTTP = 1u << 4,
} agent_interop_cap_t;

typedef struct {
    const char *id;
    const char *label;
    const char *binary;
    const char *aliases;
    unsigned capabilities;
    const char *model_flag;
    const char *cwd_flag;
    const char *prompt_flag;
    const char *const *prefix;
    const char *const *acp_args;
} agent_interop_adapter_t;

typedef struct {
    char *argv[AGENT_INTEROP_ARGV_MAX];
    int argc;
    bool prompt_on_stdin;
} agent_interop_argv_t;

size_t agent_interop_adapter_count(void);
const agent_interop_adapter_t *agent_interop_adapter_at(size_t index);
const agent_interop_adapter_t *agent_interop_find(const char *name);

bool agent_interop_resolve_binary(const char *binary, char *out, size_t out_len);
bool agent_interop_build_argv(const agent_interop_adapter_t *adapter,
                              const char *resolved_binary, const char *prompt,
                              const char *model, const char *cwd,
                              agent_interop_argv_t *out, char *error,
                              size_t error_len);
void agent_interop_argv_free(agent_interop_argv_t *argv);

bool agent_interop_manifest_json(const char *dsco_binary, char *out, size_t out_len);
bool agent_interop_status_json(const char *dsco_binary, char *out, size_t out_len);

/* `dsco interop ...` entry point. The surface is operator-invoked; autonomous
 * model delegation continues to use native DSCO workers and the governed
 * swarm tool. */
int agent_interop_cli(int argc, char **argv, const char *dsco_binary);

#endif
