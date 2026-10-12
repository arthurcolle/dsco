#include "timeout_policy.h"
#include "tools.h"
#include "json_util.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <limits.h>

int dsco_job_timeout_s(const char *input) {
    char *raw = json_get_raw(input ? input : "{}", "timeout_seconds");
    if (!raw) raw = json_get_raw(input ? input : "{}", "timeout");
    const char *value = raw ? raw : getenv("DSCO_COMMAND_TIMEOUT_S");
    if (!value || !*value) { free(raw); return 0; }
    char *end; errno = 0;
    long seconds = strtol(value, &end, 10);
    bool valid = !errno && end != value && !*end && seconds >= 0 && seconds <= DSCO_EXECUTION_MAX_S;
    free(raw);
    return valid ? (int)seconds : -1;
}

int dsco_execution_default_s(void) {
    return dsco_env_int("DSCO_COMMAND_TIMEOUT_S", DSCO_EXECUTION_DEFAULT_S, 1, DSCO_EXECUTION_MAX_S);
}

int dsco_execution_timeout_s(const char *tool, const char *input) {
    int seconds = json_get_int(input ? input : "{}", "timeout_seconds", 0);
    if (seconds <= 0) {
        seconds = json_get_int(input ? input : "{}", "timeout", 0);
        /* Preserve the existing capital-Bash millisecond compatibility.
         * timeout_seconds is unambiguous for multi-hour calls. */
        if (tool && !strcmp(tool, "Bash") && seconds > 1000)
            seconds = seconds / 1000 + (seconds % 1000 != 0);
    }
    if (seconds <= 0) seconds = dsco_execution_default_s();
    if (seconds > DSCO_EXECUTION_MAX_S) seconds = DSCO_EXECUTION_MAX_S;
    return seconds;
}

int dsco_mcp_call_timeout_ms(void) {
    const char *specific = getenv("DSCO_MCP_CALL_TIMEOUT_MS");
    if (specific && *specific)
        return dsco_env_int("DSCO_MCP_CALL_TIMEOUT_MS", DSCO_EXECUTION_DEFAULT_S * 1000,
                            250, DSCO_EXECUTION_MAX_S * 1000);
    return dsco_env_int("DSCO_MCP_TIMEOUT_MS", DSCO_EXECUTION_DEFAULT_S * 1000,
                        250, DSCO_EXECUTION_MAX_S * 1000);
}

/* Per-tool timeout overrides (tools that naturally take longer) */
static const tool_timeout_cfg_t s_timeout_overrides[] = {
    /* Interactive: blocks on the human, so the watchdog must not fire — a 30s
     * timeout makes the agent re-issue the dialog in a loop, ballooning context.
     * Effectively "wait for the user" (24h); Esc/Ctrl-C still cancels. */
    {"AskUserQuestion", 86400},
    {"bash", 600},
    {"run_command", 600},
    {"sandbox_run", 600},
    {"dsco-python-3x", 600},
    {"node", 600},
    {"compile", 600},
    {"spawn_agent", 300},
    {"agent_wait", 21600},   /* up to 6h: long-blocking agent joins */
    {"agent_race", 3660},
    /* Read-only local search/inspection tools previously inherited the 30s
     * default and were killed on large trees/corpora ("grep_files exceeded
     * 30s"). They are I/O-bound and cooperatively cancellable, so give them a
     * generous ceiling. Override per-tool via DSCO_TOOL_TIMEOUT_<NAME>. */
    {"grep_files", 600},
    {"Grep", 600},
    {"find_files", 600},
    {"read_file", 300},
    {"list_directory", 300},
    {"ast_search", 900},
    {"ast_index", 3600},
    {"ast_insights", 900},
    /* The consolidated `swarm` tool dispatches long-blocking actions (create,
     * map_reduce, collect, provider_fabric, topology_run/solve) that run whole
     * model instances to completion. Without an entry it inherited the 30s
     * default (+5s grace) and real swarms were killed at ~35s. Give it the same
     * ceiling as swarm_collect/agent_wait; fast sub-actions (status/inspect/
     * budget) return early, and Esc/Ctrl-C + swarm budget still bound runaways.
     * Runtime override: DSCO_TOOL_TIMEOUT_SWARM. */
    {"swarm", 21600},
    /* Direct native HTTP batch work must not inherit the generic 30s tool
     * deadline; it is bounded by per-request curl timeouts and concurrency. */
    {"weather_batch", 300},
    {"create_swarm", 300},
    {"swarm_collect", 21600},
    {"spawn_executor", 300},
    {"spawn_provider", 300},
    {"create_executor_swarm", 300},
    {"executor_status", 30},
    {"swarm_budget", 30},
    {"edit_file", 120}, /* local file ops: generous margin for big files */
    {"Edit", 120},
    {"write_file", 120},
    {"Write", 120},
    {"http_request", 600},
    {"curl", 600},
    {"market_quote", 30},
    {"topology_run", 1800},
    {NULL, 0}};

int tool_timeout_for(const char *name) {
    if (!name) return dsco_tool_default_timeout_s();
    /* 1. Per-tool env override: DSCO_TOOL_TIMEOUT_BASH, etc. */
    char env_key[128];
    snprintf(env_key, sizeof(env_key), "DSCO_TOOL_TIMEOUT_%s", name);
    for (char *p = env_key + strlen("DSCO_TOOL_TIMEOUT_"); *p; p++)
        *p = (*p >= 'a' && *p <= 'z') ? *p - 32 : *p;
    int env_timeout = dsco_env_int(env_key, -1, 1, TOOL_TIMEOUT_MAX_S);
    if (env_timeout > 0)
        return env_timeout;
    /* 2. Static table */
    for (int i = 0; s_timeout_overrides[i].name; i++) {
        if (strcmp(s_timeout_overrides[i].name, name) == 0)
            return s_timeout_overrides[i].timeout_s;
    }
    /* 3. Global default override */
    return dsco_tool_default_timeout_s();
}


int tool_timeout_for_input(const char *name, const char *input) {
    int base = tool_timeout_for(name);
    if (!name) return base;
    /* An operator's explicit per-tool watchdog remains authoritative. */
    char key[128]; snprintf(key, sizeof(key), "DSCO_TOOL_TIMEOUT_%s", name);
    for (char *p = key + strlen("DSCO_TOOL_TIMEOUT_"); *p; ++p)
        if (*p >= 'a' && *p <= 'z') *p -= 32;
    if (dsco_env_int(key, -1, 1, TOOL_TIMEOUT_MAX_S) > 0) return base;
    int execution = 0;
    const char *commands[] = {"bash", "Bash", "run_command", "sandbox_run", "python",
        "python_exec", "dsco-python-3x", "node", "http_request", "browser_snapshot", "research_probe", NULL};
    for (int i = 0; commands[i]; ++i) if (!strcmp(name, commands[i])) {
        execution = dsco_execution_timeout_s(name, input); break;
    }
    if (!strncmp(name, "mcp__", 5)) {
        int ms = dsco_mcp_call_timeout_ms(); execution = ms / 1000 + (ms % 1000 != 0);
    }
    if (execution > 0) {
        int supervised = execution > TOOL_TIMEOUT_MAX_S - 5 ? TOOL_TIMEOUT_MAX_S : execution + 5;
        if (supervised > base) base = supervised;
    }
    return base;
}
