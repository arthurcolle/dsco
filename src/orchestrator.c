#include "orchestrator.h"
#include "agent.h"
#include "tools.h"
#include "llm.h"
#include "provider.h"
#include "json_util.h"
#include "config.h"
#include "topology.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Default models ─────────────────────────────────────────────────── */

#define ORCH_CHAT_MODEL_DEFAULT "z-ai/glm-5.2"
#define ORCH_WORKER_MODEL_DEFAULT "kimi-k2.7-code"

/* Max LLM turns per worker task before forcing stop */
#define ORCH_WORKER_MAX_TURNS 24

/* Max tools per worker (the register file will page within this) */
#define ORCH_WORKER_MAX_TOOLS 32

/* Max bytes in worker result buffer */
#define ORCH_RESULT_BUF (256 * 1024)

/* ── Domain enum + metadata ─────────────────────────────────────────── */

typedef enum {
    ORCH_DOMAIN_FILE = 0,
    ORCH_DOMAIN_GIT,
    ORCH_DOMAIN_SYSTEM,
    ORCH_DOMAIN_CODE,
    ORCH_DOMAIN_WEB,
    ORCH_DOMAIN_TRADING,
    ORCH_DOMAIN_MARKET,
    ORCH_DOMAIN_WINGS,
    ORCH_DOMAIN_TEXT,
    ORCH_DOMAIN_GENERAL,
    ORCH_DOMAIN_COUNT,
} orch_domain_t;

static const char *s_domain_names[ORCH_DOMAIN_COUNT] = {
    "file", "git", "system", "code", "web", "trading", "market", "wings", "text", "general",
};

static const char *s_domain_desc[ORCH_DOMAIN_COUNT] = {
    "File read/write/edit/search/find operations (~20 tools)",
    "Git: status, diff, commit, push, pull, branch, log (~10 tools)",
    "Shell commands, processes, environment, system info (~15 tools)",
    "Code compilation, execution, analysis, debugging (~8 tools)",
    "Web requests, HTTP, browser, research, DNS, port scan (~15 tools)",
    "Kalshi/Polymarket order execution and position management (~22 tools)",
    "Market data, prediction prices, arbitrage scanning (~27 tools)",
    "Wings/Talons: governance, OODA, memory, pheromones, immune (~22 tools)",
    "Text processing: sed, awk, jq, CSV, regex, zip, url (~14 tools)",
    "General purpose — all tools via register-file paging (up to 32)",
};

/* ── Always-on pinned tools (available in every domain) ─────────────── */

static const char *s_pinned[] = {"bash", "read_file", "discover_tools", "context_recall", NULL};

/* ── Per-worker state (set before each dispatch, single-threaded) ───── */

static orch_domain_t g_worker_domain = ORCH_DOMAIN_GENERAL;
static char g_worker_model[128] = ORCH_WORKER_MODEL_DEFAULT;

/* ── Domain classification by prefix/name ──────────────────────────── */

static orch_domain_t classify_tool(const char *name) {
    /* Git */
    if (strncmp(name, "git_", 4) == 0)
        return ORCH_DOMAIN_GIT;

    /* Trading execution */
    if (strncmp(name, "kalshi_", 7) == 0 || strncmp(name, "polymarket_", 11) == 0 ||
        strncmp(name, "arb_", 4) == 0)
        return ORCH_DOMAIN_TRADING;

    /* Market data */
    if (strncmp(name, "prediction_", 11) == 0 || strncmp(name, "market_", 7) == 0)
        return ORCH_DOMAIN_MARKET;

    /* Wings / governance / immune */
    if (strncmp(name, "pheromone_", 10) == 0 || strncmp(name, "ooda_", 5) == 0 ||
        strncmp(name, "killswitch_", 11) == 0 || strncmp(name, "governance_", 11) == 0 ||
        strncmp(name, "memory_", 7) == 0 || strncmp(name, "talons_", 7) == 0 ||
        strncmp(name, "legion_", 7) == 0)
        return ORCH_DOMAIN_WINGS;

    /* Web / network */
    if (strcmp(name, "web_extract") == 0 || strcmp(name, "http_request") == 0 ||
        strcmp(name, "json_api") == 0 || strcmp(name, "curl_raw") == 0 ||
        strcmp(name, "dns_lookup") == 0 || strcmp(name, "port_scan") == 0 ||
        strcmp(name, "port_check") == 0 || strcmp(name, "ping") == 0 ||
        strcmp(name, "web_search") == 0 || strcmp(name, "web_screenshot") == 0 ||
        strncmp(name, "browser_", 8) == 0 || strncmp(name, "research_", 9) == 0)
        return ORCH_DOMAIN_WEB;

    /* Code / execution */
    if (strcmp(name, "compile") == 0 || strcmp(name, "run_command") == 0 ||
        strcmp(name, "run_background") == 0 || strcmp(name, "sandbox_run") == 0 ||
        strncmp(name, "code_", 5) == 0 || strcmp(name, "debug_trace") == 0)
        return ORCH_DOMAIN_CODE;

    /* System */
    if (strcmp(name, "ps") == 0 || strcmp(name, "kill_process") == 0 ||
        strncmp(name, "env_", 4) == 0 || strcmp(name, "sysinfo") == 0 ||
        strcmp(name, "disk_usage") == 0 || strcmp(name, "which") == 0 || strcmp(name, "cwd") == 0 ||
        strcmp(name, "whoami") == 0 || strcmp(name, "uptime") == 0 ||
        strcmp(name, "open_url") == 0 || strncmp(name, "clipboard_", 10) == 0 ||
        strcmp(name, "sleep_ms") == 0)
        return ORCH_DOMAIN_SYSTEM;

    /* File I/O */
    if (strncmp(name, "file_", 5) == 0 || strcmp(name, "write_file") == 0 ||
        strcmp(name, "edit_file") == 0 || strcmp(name, "find_files") == 0 ||
        strcmp(name, "grep_files") == 0 || strcmp(name, "list_dir") == 0 ||
        strcmp(name, "find_and_replace") == 0 || strcmp(name, "diff") == 0 ||
        strcmp(name, "patch") == 0 || strcmp(name, "copy_file") == 0 ||
        strcmp(name, "move_file") == 0 || strcmp(name, "delete_file") == 0)
        return ORCH_DOMAIN_FILE;

    /* Text processing */
    if (strcmp(name, "sed") == 0 || strcmp(name, "awk") == 0 || strcmp(name, "sort_uniq") == 0 ||
        strcmp(name, "jq") == 0 || strcmp(name, "tar") == 0 || strcmp(name, "zip") == 0 ||
        strncmp(name, "csv_", 4) == 0 || strcmp(name, "regex_match") == 0 ||
        strcmp(name, "json_format") == 0 || strcmp(name, "sed_transform") == 0 ||
        strcmp(name, "awk_process") == 0 || strncmp(name, "url_", 4) == 0 ||
        strcmp(name, "semver_compare") == 0 || strcmp(name, "cron_parse") == 0 ||
        strcmp(name, "mapbox_geocode") == 0)
        return ORCH_DOMAIN_TEXT;

    return ORCH_DOMAIN_GENERAL;
}

/* ── Profile filters ────────────────────────────────────────────────── */

/* Orchestrator chat filter: only the orchestrator meta-tools */
static bool orch_chat_filter(const char *name, const char *group_hint) {
    (void)group_hint;
    return strcmp(name, "dispatch_agent") == 0 || strcmp(name, "dispatch_topology") == 0 ||
           strcmp(name, "dispatch_tools") == 0 || strcmp(name, "list_domains") == 0;
}

/* Worker filter: domain-specific tools + always-on pinned tools */
static bool orch_worker_filter(const char *name, const char *group_hint) {
    (void)group_hint;
    if (g_worker_domain == ORCH_DOMAIN_GENERAL)
        return true;
    for (int i = 0; s_pinned[i]; i++)
        if (strcmp(name, s_pinned[i]) == 0)
            return true;
    return classify_tool(name) == g_worker_domain;
}

/* ── Lightweight worker task runner ─────────────────────────────────── */

static char *run_worker_task(const char *task, const char *model) {
    const char *api_key = tools_runtime_api_key();
    if (!api_key || !api_key[0]) {
        return safe_strdup("[error: no API key available for worker]");
    }

    /* Resolve API key for the model's provider */
    const char *pname = provider_route_for_model(model, api_key, NULL);
    const char *key = provider_resolve_request_api_key(pname, api_key);
    if (!key || !key[0])
        key = api_key;
    provider_debug_log_request(pname, model, key);

    /* Session */
    session_state_t session;
    session_state_init(&session, model);
    tools_set_context_window(session.context_window);

    /* Conversation: single user turn with the task */
    conversation_t conv;
    conv_init(&conv);
    conv_add_user_text(&conv, task);

    char *result = NULL;

    for (int turn = 0; turn < ORCH_WORKER_MAX_TURNS; turn++) {
        char *req = llm_build_request_ex_for_credential(&conv, &session, 8192, key);
        if (!req)
            break;

        /* Stream silently — worker output is returned as tool result */
        stream_result_t sr = llm_stream(key, req, NULL, NULL, NULL, NULL);
        free(req);

        if (!sr.ok) {
            /* Capture error info if available */
            if (!result) {
                char errbuf[256];
                snprintf(errbuf, sizeof(errbuf), "[worker error: HTTP %d, stop=%s]", sr.http_status,
                         sr.parsed.stop_reason ? sr.parsed.stop_reason : "unknown");
                result = safe_strdup(errbuf);
            }
            json_free_response(&sr.parsed);
            break;
        }

        /* Track tokens for budget_ratio updates */
        session.total_input_tokens += sr.usage.input_tokens;
        session.total_output_tokens += sr.usage.output_tokens;
        session.turn_count++;
        if (session.context_window > 0)
            session.tool_budget_ratio =
                1.0f - (float)session.total_input_tokens / (float)session.context_window;

        conv_add_assistant_raw(&conv, &sr.parsed);

        /* Execute tool calls */
        bool has_tools = false;
        for (int i = 0; i < sr.parsed.count; i++) {
            content_block_t *blk = &sr.parsed.blocks[i];
            if (!blk->type || strcmp(blk->type, "tool_use") != 0)
                continue;
            has_tools = true;

            fprintf(stderr, "  \033[2m[worker] tool: %s\033[0m\n",
                    blk->tool_name ? blk->tool_name : "?");

            char *tr = safe_malloc(ORCH_RESULT_BUF);
            tr[0] = '\0';
            const char *tier = "standard";

            bool ok = tools_is_allowed_for_tier(blk->tool_name, tier, tr, ORCH_RESULT_BUF);
            if (ok)
                ok = tools_execute_for_tier(blk->tool_name, blk->tool_input, tier, tr,
                                            ORCH_RESULT_BUF);
            conv_add_tool_result_named(&conv, blk->tool_id, blk->tool_name, tr, !ok);
            free(tr);
        }

        bool done =
            !has_tools || (sr.parsed.stop_reason && strcmp(sr.parsed.stop_reason, "end_turn") == 0);

        /* Capture final assistant text */
        if (done) {
            for (int i = 0; i < sr.parsed.count; i++) {
                content_block_t *blk = &sr.parsed.blocks[i];
                if (blk->type && strcmp(blk->type, "text") == 0 && blk->text) {
                    free(result);
                    result = safe_strdup(blk->text);
                }
            }
            json_free_response(&sr.parsed);
            break;
        }

        json_free_response(&sr.parsed);
    }

    fprintf(stderr, "  \033[2m[worker] %d turns, %d in + %d out tokens\033[0m\n",
            session.turn_count, session.total_input_tokens, session.total_output_tokens);

    conv_free(&conv);
    return result ? result : safe_strdup("[worker completed with no text output]");
}

/* ── Virtual Tool Wrapping: every tool as a Haiku micro-agent ───────── */
/*
 * Instead of sending 30 tools to one agent, wrap each tool in its own
 * Haiku instance. The coordinator (Sonnet/Opus) describes WHAT to do,
 * the Haiku wrapper figures out HOW to call its single tool.
 *
 * Benefits:
 *   - Zero tool selection confusion (1 tool per agent)
 *   - Haiku is $0.25/MTok — cheaper than wasting Sonnet context on 30 schemas
 *   - Natural parallelism via topology fan-out
 *   - Error isolation: one tool failure doesn't corrupt the whole session
 *
 * Cost: ~$0.001-0.003 per tool invocation (Haiku turn + tool execution)
 */

/* Run a single tool via a Haiku wrapper agent.
 * task: natural language description of what the tool should do.
 * tool_name: specific tool to invoke.
 * Returns: tool result interpreted by Haiku, or raw result on failure. */
static char *run_virtual_tool(const char *task, const char *tool_name) {
    const char *api_key = tools_runtime_api_key();
    if (!api_key || !api_key[0])
        return safe_strdup("[error: no API key]");

    const char *key = provider_resolve_request_api_key(
        provider_route_for_model("claude-haiku-4-5-20251001", api_key, NULL), api_key);
    if (!key || !key[0])
        key = api_key;

    /* Build a micro-session: Haiku, 1 tool */
    session_state_t session;
    session_state_init(&session, "claude-haiku-4-5-20251001");

    /* We can't nest profile filters safely, so we use direct tool execution.
     * Instead of going through the LLM → tool loop, build a prompt that
     * makes Haiku call exactly this tool, then execute directly. */

    /* Look up the tool definition for its schema */
    int tool_idx = tool_map_lookup(&g_tool_map, tool_name);
    if (tool_idx < 0 && tool_idx > -(10000)) {
        /* Not found — try direct execution with the task as input */
        char *tr = safe_malloc(ORCH_RESULT_BUF);
        tr[0] = '\0';
        bool ok = tools_execute(tool_name, task, tr, ORCH_RESULT_BUF);
        if (!ok && tr[0] == '\0')
            snprintf(tr, ORCH_RESULT_BUF, "[tool %s not found]", tool_name);
        return tr;
    }

    /* Build a Haiku conversation that forces it to call this specific tool */
    conversation_t conv;
    conv_init(&conv);

    /* System context: you are a single-tool executor */
    char user_msg[4096];
    snprintf(user_msg, sizeof(user_msg),
             "You have exactly one tool available: %s\n"
             "Call it to accomplish this task:\n\n%s\n\n"
             "Call the tool now with the correct parameters. "
             "After getting the result, return it with a brief summary.",
             tool_name, task);
    conv_add_user_text(&conv, user_msg);

    /* Force tool_choice to this specific tool */
    snprintf(session.tool_choice, sizeof(session.tool_choice), "tool:%s", tool_name);

    /* Set a single-tool profile filter */
    tools_set_profile_filter(NULL); /* clear any existing */

    char *result = NULL;

    /* 2 turns max: call tool, then summarize */
    for (int turn = 0; turn < 3; turn++) {
        char *req = llm_build_request_ex_for_credential(&conv, &session, 4096, key);
        if (!req)
            break;

        stream_result_t sr = llm_stream(key, req, NULL, NULL, NULL, NULL);
        free(req);

        if (!sr.ok) {
            json_free_response(&sr.parsed);
            break;
        }

        session.total_input_tokens += sr.usage.input_tokens;
        session.total_output_tokens += sr.usage.output_tokens;
        session.turn_count++;

        conv_add_assistant_raw(&conv, &sr.parsed);

        bool has_tools = false;
        for (int i = 0; i < sr.parsed.count; i++) {
            content_block_t *blk = &sr.parsed.blocks[i];
            if (!blk->type || strcmp(blk->type, "tool_use") != 0)
                continue;
            has_tools = true;

            char *tr = safe_malloc(ORCH_RESULT_BUF);
            tr[0] = '\0';
            bool ok = tools_execute(blk->tool_name, blk->tool_input, tr, ORCH_RESULT_BUF);
            conv_add_tool_result_named(&conv, blk->tool_id, blk->tool_name, tr, !ok);
            free(tr);
        }

        /* After first tool call, switch to auto tool_choice for summary */
        session.tool_choice[0] = '\0';

        bool done =
            !has_tools || (sr.parsed.stop_reason && strcmp(sr.parsed.stop_reason, "end_turn") == 0);

        if (done) {
            for (int i = 0; i < sr.parsed.count; i++) {
                content_block_t *blk = &sr.parsed.blocks[i];
                if (blk->type && strcmp(blk->type, "text") == 0 && blk->text) {
                    free(result);
                    result = safe_strdup(blk->text);
                }
            }
            json_free_response(&sr.parsed);
            break;
        }

        json_free_response(&sr.parsed);
    }

    conv_free(&conv);
    return result ? result : safe_strdup("[virtual tool returned no output]");
}

/* Dispatch N virtual tool calls in sequence (parallel later via swarm).
 * tools_json: JSON array of {tool, task} objects.
 * Returns: combined results as JSON array string. */
static char *dispatch_virtual_tools_cb(const char *name, const char *input_json, void *ctx) {
    (void)name;
    (void)ctx;

    /* Parse the array of tool calls */
    char *calls_raw = json_get_raw(input_json, "calls");
    if (!calls_raw || calls_raw[0] != '[') {
        free(calls_raw);
        return safe_strdup("error: 'calls' must be a JSON array of {tool, task} objects");
    }

    /* Simple JSON array iteration — find each {tool, task} */
    jbuf_t result;
    jbuf_init(&result, 4096);
    jbuf_append(&result, "[");

    int call_count = 0;
    const char *p = calls_raw + 1; /* skip [ */
    while (*p) {
        /* Find next { */
        while (*p && *p != '{')
            p++;
        if (!*p)
            break;

        /* Find matching } — simple brace counting */
        int depth = 0;
        const char *obj_start = p;
        do {
            if (*p == '{')
                depth++;
            else if (*p == '}')
                depth--;
            p++;
        } while (*p && depth > 0);

        /* Extract the object */
        size_t obj_len = (size_t)(p - obj_start);
        char *obj = safe_malloc(obj_len + 1);
        memcpy(obj, obj_start, obj_len);
        obj[obj_len] = '\0';

        char *tool = json_get_str(obj, "tool");
        char *task = json_get_str(obj, "task");

        if (tool && task && tool[0] && task[0]) {
            fprintf(stderr, "  \033[35m[vtool]\033[0m %s\n", tool);

            /* Clear filter for this tool's execution */
            tools_set_profile_filter(NULL);

            char *tool_result = run_virtual_tool(task, tool);

            if (call_count > 0)
                jbuf_append(&result, ",");
            jbuf_append(&result, "{\"tool\":");
            jbuf_append_json_str(&result, tool);
            jbuf_append(&result, ",\"result\":");
            jbuf_append_json_str(&result, tool_result ? tool_result : "");
            jbuf_append(&result, "}");

            free(tool_result);
            call_count++;
        }

        free(tool);
        free(task);
        free(obj);
    }

    jbuf_append(&result, "]");
    free(calls_raw);

    /* Restore orchestrator filter */
    tools_set_profile_filter(orch_chat_filter);

    fprintf(stderr, "  \033[35m[vtool]\033[0m %d tools executed\n", call_count);

    char *out = safe_strdup(result.data);
    jbuf_free(&result);
    return out;
}

/* ── Orchestrator tool: list_domains ────────────────────────────────── */

static char *list_domains_cb(const char *name, const char *input_json, void *ctx) {
    (void)name;
    (void)input_json;
    (void)ctx;

    jbuf_t b;
    jbuf_init(&b, 4096);

    jbuf_append(&b, "## Worker Domains\n\n");
    for (int i = 0; i < ORCH_DOMAIN_COUNT; i++) {
        jbuf_appendf(&b, "  %-10s  %s\n", s_domain_names[i], s_domain_desc[i]);
    }

    jbuf_appendf(&b,
                 "\n## Topology Patterns (for complex multi-step tasks)\n\n"
                 "  %-16s  %s\n"
                 "  %-16s  %s\n"
                 "  %-16s  %s\n"
                 "  %-16s  %s\n"
                 "  %-16s  %s\n"
                 "  %-16s  %s\n"
                 "  %-16s  %s\n"
                 "  %-16s  %s\n",
                 "clinic", "Intake→Plan→Implement→Validate→Judge (code tasks)", "research",
                 "Plan→Gather(scouts)→Analyze→Synthesize (info gathering)", "code_review",
                 "Lint→Logic→Security→Verdict (review tasks)", "sentinel",
                 "Triage(H)→Analyze(S)→Decide(O) (quick decisions)", "switchboard",
                 "Classify→Route to specialists→Integrate (routing)", "critic_loop",
                 "Generate→Critique→Refine, repeat 3x (quality)", "tournament",
                 "N candidates compete, judge picks (best-of-N)", "starburst",
                 "Coordinator fans out to parallel workers→merge");

    jbuf_appendf(&b,
                 "\n## Models\n\n"
                 "  Default worker: %s\n"
                 "  Override per-call: glm (general) | kimi/code (worker) | full model ID\n"
                 "\n## Always Available\n\n"
                 "  Pinned in all domains: bash, read_file, discover_tools, context_recall\n",
                 g_worker_model);

    char *out = safe_strdup(b.data);
    jbuf_free(&b);
    return out;
}

/* ── Orchestrator tool: dispatch_agent ──────────────────────────────── */

static orch_domain_t domain_from_str(const char *s) {
    if (!s || !s[0])
        return ORCH_DOMAIN_GENERAL;
    for (int i = 0; i < ORCH_DOMAIN_COUNT; i++)
        if (strcmp(s, s_domain_names[i]) == 0)
            return (orch_domain_t)i;
    return ORCH_DOMAIN_GENERAL;
}

static const char *resolve_model_alias(const char *alias) {
    if (!alias || !alias[0])
        return g_worker_model;
    if (strcmp(alias, "glm") == 0)
        return provider_select_default_primary_model(false);
    if (strcmp(alias, "kimi") == 0)
        return provider_select_default_primary_model(true);
    if (strcmp(alias, "code") == 0)
        return provider_select_default_primary_model(true);
    if (strcmp(alias, "haiku") == 0)
        return provider_select_default_primary_model(false);
    if (strcmp(alias, "sonnet") == 0)
        return provider_select_default_primary_model(true);
    if (strcmp(alias, "opus") == 0)
        return "claude-opus-4-6";
    return alias; /* pass through full model IDs */
}

static char *dispatch_agent_cb(const char *name, const char *input_json, void *ctx) {
    (void)name;
    (void)ctx;

    char *domain_str = json_get_str(input_json, "domain");
    char *task = json_get_str(input_json, "task");
    char *model_alias = json_get_str(input_json, "model");

    if (!task || !task[0]) {
        free(domain_str);
        free(task);
        free(model_alias);
        return safe_strdup("error: 'task' field is required");
    }

    orch_domain_t domain = domain_from_str(domain_str);
    const char *wmodel = resolve_model_alias(model_alias);
    const char *dname = s_domain_names[domain];

    fprintf(stderr, "  \033[36m[orch]\033[0m dispatch → %s worker (%s)\n", dname, wmodel);

    /* Switch to domain-filtered tool set */
    g_worker_domain = domain;
    tools_set_profile_filter(orch_worker_filter);

    char *result = run_worker_task(task, wmodel);

    /* Restore orchestrator filter */
    g_worker_domain = ORCH_DOMAIN_GENERAL;
    tools_set_profile_filter(orch_chat_filter);

    fprintf(stderr, "  \033[36m[orch]\033[0m %s worker done (%zu chars)\n", dname,
            result ? strlen(result) : 0);

    free(domain_str);
    free(task);
    free(model_alias);
    return result;
}

/* ── Orchestrator tool: dispatch_topology ───────────────────────────── */

static char *dispatch_topology_cb(const char *name, const char *input_json, void *ctx) {
    (void)name;
    (void)ctx;

    char *topo_name = json_get_str(input_json, "topology");
    char *task = json_get_str(input_json, "task");
    bool auto_mode = json_get_bool(input_json, "auto", false);

    if (!task || !task[0]) {
        free(topo_name);
        free(task);
        return safe_strdup("error: 'task' field is required");
    }

    const char *api_key = tools_runtime_api_key();
    if (!api_key || !api_key[0]) {
        free(topo_name);
        free(task);
        return safe_strdup("error: no API key available");
    }

    /* Clear the orchestrator filter so topology sub-agents get full toolset */
    tools_set_profile_filter(NULL);

    /* Build execution plan */
    topology_plan_t plan;
    bool have_plan =
        topology_plan_build(topo_name, auto_mode || (!topo_name || !topo_name[0]), task, &plan);

    if (!have_plan) {
        tools_set_profile_filter(orch_chat_filter);
        free(topo_name);
        free(task);
        return safe_strdup("error: failed to build topology plan");
    }

    fprintf(stderr, "  \033[36m[orch]\033[0m topology → %s (%d nodes, %s%s)\n", plan.topology.name,
            plan.topology.node_count, plan.is_dynamic ? "dynamic" : "static",
            plan.rationale[0] ? ", " : "");
    if (plan.rationale[0])
        fprintf(stderr, "  \033[2m%s\033[0m\n", plan.rationale);

    /* Execute the topology */
    char *result_buf = safe_malloc(ORCH_RESULT_BUF);
    result_buf[0] = '\0';
    topology_run_stats_t stats = {0};

    bool ok = topology_plan_run(&plan, api_key, g_worker_model, task, result_buf, ORCH_RESULT_BUF,
                                &stats);

    /* Restore orchestrator filter */
    tools_set_profile_filter(orch_chat_filter);

    if (!ok) {
        free(topo_name);
        free(task);
        char errbuf[512];
        snprintf(errbuf, sizeof(errbuf),
                 "Topology '%s' execution failed. "
                 "Nodes executed: %d, agents spawned: %d",
                 plan.topology.name, stats.nodes_executed, stats.agents_spawned);
        free(result_buf);
        return safe_strdup(errbuf);
    }

    fprintf(stderr,
            "  \033[36m[orch]\033[0m topology done: %d iterations, "
            "%d nodes, %d agents, ~$%.4f\n",
            stats.iterations, stats.nodes_executed, stats.agents_spawned, stats.est_cost_usd);

    free(topo_name);
    free(task);

    /* Transfer ownership of result_buf */
    if (result_buf[0] == '\0') {
        free(result_buf);
        return safe_strdup("[topology completed with no output]");
    }
    return result_buf;
}

/* ── Tool schemas ───────────────────────────────────────────────────── */

static const char s_dispatch_schema[] =
    "{"
    "\"type\":\"object\","
    "\"properties\":{"
    "\"domain\":{"
    "\"type\":\"string\","
    "\"description\":\"Specialist domain for the worker agent.\","
    "\"enum\":[\"file\",\"git\",\"system\",\"code\",\"web\","
    "\"trading\",\"market\",\"wings\",\"text\",\"general\"]"
    "},"
    "\"task\":{"
    "\"type\":\"string\","
    "\"description\":\"Complete, self-contained task description. Include ALL context the worker "
    "needs — it has no access to our conversation history.\""
    "},"
    "\"model\":{"
    "\"type\":\"string\","
    "\"description\":\"Worker model alias or full model ID. Default is kimi-k2.7-code; use glm for "
    "GLM or kimi/code for Kimi K2.7 Code.\""
    "}"
    "},"
    "\"required\":[\"domain\",\"task\"]"
    "}";

static const char s_topology_schema[] =
    "{"
    "\"type\":\"object\","
    "\"properties\":{"
    "\"topology\":{"
    "\"type\":\"string\","
    "\"description\":\"Topology name: clinic, research, code_review, sentinel, switchboard, "
    "critic_loop, tournament, starburst, or empty for auto-select.\""
    "},"
    "\"task\":{"
    "\"type\":\"string\","
    "\"description\":\"Complete task description for multi-agent execution.\""
    "},"
    "\"auto\":{"
    "\"type\":\"boolean\","
    "\"description\":\"Auto-select best topology for the task (default: true if no topology "
    "specified).\""
    "}"
    "},"
    "\"required\":[\"task\"]"
    "}";

static const char s_vtool_schema[] = "{"
                                     "\"type\":\"object\","
                                     "\"properties\":{"
                                     "\"calls\":{"
                                     "\"type\":\"array\","
                                     "\"description\":\"Array of tool invocations. Each is a Haiku "
                                     "micro-agent with exactly 1 tool.\","
                                     "\"items\":{"
                                     "\"type\":\"object\","
                                     "\"properties\":{"
                                     "\"tool\":{\"type\":\"string\",\"description\":\"Exact tool "
                                     "name to invoke (e.g. read_file, git_status, compile)\"},"
                                     "\"task\":{\"type\":\"string\",\"description\":\"Natural "
                                     "language description of what to do with this tool.\"}"
                                     "},"
                                     "\"required\":[\"tool\",\"task\"]"
                                     "}"
                                     "}"
                                     "},"
                                     "\"required\":[\"calls\"]"
                                     "}";

static const char s_list_domains_schema[] = "{\"type\":\"object\",\"properties\":{}}";

/* ── Public entry point ─────────────────────────────────────────────── */

bool agent_run_orchestrated(const char *api_key, const char *chat_model, const char *worker_model,
                            const char *provider_override) {
    /* Apply defaults */
    if (!chat_model || !chat_model[0])
        chat_model = provider_select_default_primary_model(false);

    /* Worker model: arg > env > default */
    const char *env_worker = getenv("DSCO_WORKER_MODEL");
    if (worker_model && worker_model[0])
        snprintf(g_worker_model, sizeof(g_worker_model), "%s", worker_model);
    else if (env_worker && env_worker[0])
        snprintf(g_worker_model, sizeof(g_worker_model), "%s", env_worker);
    else
        snprintf(g_worker_model, sizeof(g_worker_model), "%s",
                 provider_select_default_primary_model(true));

    fprintf(stderr,
            "\n  \033[1;36morchestrator mode\033[0m\n"
            "  chat:   %s  (routing + synthesis)\n"
            "  worker: %s  (execution)\n"
            "  max tools/worker: %d\n"
            "  override worker: -M MODEL or DSCO_WORKER_MODEL\n\n",
            chat_model, g_worker_model, ORCH_WORKER_MAX_TOOLS);

    /* Register orchestrator meta-tools as external tools.
     * Must happen before tools_init() (called inside agent_run) so
     * tool_map_rebuild() includes them in the hash map. */
    tools_register_external(
        "dispatch_agent",
        "Dispatch a specialist worker agent with a domain-filtered toolkit. "
        "Worker runs autonomously (15-32 tools) and returns its output. "
        "Use for: single-domain tasks like file edits, git ops, code execution, "
        "web requests, trading, market queries, system commands.",
        s_dispatch_schema, dispatch_agent_cb, NULL);

    tools_register_external("dispatch_topology",
                            "Dispatch a multi-agent topology for complex tasks requiring multiple "
                            "specialist agents coordinated in a DAG. Topologies use 3-12 agents "
                            "across Haiku/Sonnet/Opus tiers. Use for: code generation (clinic), "
                            "research (research), code review (code_review), quality refinement "
                            "(critic_loop), competitive selection (tournament).",
                            s_topology_schema, dispatch_topology_cb, NULL);

    tools_register_external(
        "dispatch_tools",
        "Execute specific tools via Haiku micro-agents (1 tool per agent). "
        "Each tool call is wrapped in its own cheap Haiku instance that handles "
        "parameter construction and result interpretation. Use when you know EXACTLY "
        "which tools to call. Much cheaper than dispatch_agent for targeted operations. "
        "Example: [{tool:\"read_file\",task:\"read /etc/hosts\"}, "
        "{tool:\"git_status\",task:\"show current git status\"}]",
        s_vtool_schema, dispatch_virtual_tools_cb, NULL);

    tools_register_external(
        "list_domains", "List available worker domains, topology patterns, models, and pricing.",
        s_list_domains_schema, list_domains_cb, NULL);

    /* Restrict the orchestrator to only its 4 meta-tools */
    tools_set_profile_filter(orch_chat_filter);
    g_worker_domain = ORCH_DOMAIN_GENERAL;

    /* Hand off to the full interactive agent loop (TUI + readline + history).
     * Haiku sees only dispatch_agent, dispatch_topology, list_domains.
     * It routes user requests to domain-filtered workers or multi-agent
     * topologies as needed. */
    bool user_exit_requested = agent_run(api_key, chat_model, NULL, false, provider_override);

    /* Clean up on exit */
    tools_set_profile_filter(NULL);
    return user_exit_requested;
}
