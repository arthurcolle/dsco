#ifndef DSCO_CONFIG_H
#define DSCO_CONFIG_H

#include <stdbool.h>
#include <string.h>
#include <stddef.h>

#define DSCO_VERSION "0.8.0"

/* Build info — set via Makefile CFLAGS */
#ifndef BUILD_DATE
#define BUILD_DATE "unknown"
#endif
#ifndef GIT_HASH
#define GIT_HASH "unknown"
#endif

/* Buffer sizes */
#define MAX_REQUEST_SIZE    (512 * 1024)
#define MAX_RESPONSE_SIZE   (512 * 1024)
#define MAX_TOOL_RESULT     (128 * 1024)
#define MAX_MSG_CONTENT     (64 * 1024)
#define MAX_MESSAGES        128
#define MAX_TOOLS           512
#define MAX_INPUT_LINE      65536

/* API defaults */
#define DEFAULT_MODEL       "claude-opus-4-6"
#define API_URL_ANTHROPIC   "https://api.anthropic.com/v1/messages"
#define API_URL_COUNT_TOKENS "https://api.anthropic.com/v1/messages/count_tokens"
#define ANTHROPIC_VERSION   "2023-06-01"
#define ANTHROPIC_BETAS     "interleaved-thinking-2025-05-14,code-execution-2025-05-22,advanced-tool-use-2025-11-20"
#define MAX_TOKENS          16384

/* Tool limits */
#define MAX_FILE_PAGE_SIZE  4096
#define MAX_EXEC_OUTPUT     (64 * 1024)

/* Agent loop */
#define MAX_AGENT_TURNS     50

/* Context window / token budget */
#define CONTEXT_WINDOW_TOKENS  200000  /* claude-opus-4-6 */
#define TOKEN_BUDGET_WARN      0.85    /* warn at 85% usage */
#define TOKEN_BUDGET_COMPACT   0.80    /* auto-compact at 80% */

/* Session storage */
#define DSCO_SESSION_DIR     "~/.dsco/sessions"
#define DSCO_DEBUG_DIR       "~/.dsco/debug"
#define DSCO_MCP_CONFIG      "~/.dsco/mcp.json"
#define DSCO_SYSTEM_PROMPT   "~/.dsco/system_prompt.txt"
#define DSCO_PLUGINS_DIR     "~/.dsco/plugins"

/* Max content blocks in a single response */
#define MAX_CONTENT_BLOCKS  128

/* ── Model registry ────────────────────────────────────────────────────── */

typedef struct {
    const char *alias;          /* short name: "opus", "sonnet", "haiku" */
    const char *model_id;       /* full API model ID */
    int         context_window; /* tokens */
    int         max_output;     /* max output tokens */
    double      input_price;    /* $ per 1M input tokens */
    double      output_price;   /* $ per 1M output tokens */
    double      cache_read_price;  /* $ per 1M cache read tokens */
    double      cache_write_price; /* $ per 1M cache write tokens */
    int         supports_thinking; /* 1 = adaptive thinking */
} model_info_t;

static const model_info_t MODEL_REGISTRY[] = {
    /* Anthropic */
    { "opus",   "claude-opus-4-6",             200000, 32000,  15.0,  75.0,  1.50, 18.75, 1 },
    { "sonnet", "claude-sonnet-4-6",           200000, 16000,   3.0,  15.0,  0.30,  3.75, 1 },
    { "haiku",  "claude-haiku-4-5-20251001",   200000,  8192,   0.80,  4.0,  0.08,  1.00, 0 },
    /* OpenAI */
    { "gpt4o",      "gpt-4o",                 128000, 16384,   2.50, 10.0,  0, 0, 0 },
    { "gpt4o-mini", "gpt-4o-mini",            128000, 16384,   0.15,  0.60, 0, 0, 0 },
    { "o1",         "o1",                      200000, 100000, 15.0,  60.0,  0, 0, 1 },
    { "o3-mini",    "o3-mini",                 200000, 100000,  1.10,  4.40, 0, 0, 1 },
    /* Groq (fast inference) */
    { "llama70b",   "llama-3.3-70b-versatile", 128000, 32768,   0.59,  0.79, 0, 0, 0 },
    { "llama8b",    "llama-3.1-8b-instant",    128000, 8192,    0.05,  0.08, 0, 0, 0 },
    { "mixtral",    "mixtral-8x7b-32768",       32768, 32768,   0.24,  0.24, 0, 0, 0 },
    /* DeepSeek */
    { "deepseek",   "deepseek-chat",           128000, 8192,    0.14,  0.28, 0, 0, 1 },
    { "deepseek-r", "deepseek-reasoner",       128000, 8192,    0.55,  2.19, 0, 0, 1 },
    /* Mistral */
    { "mistral-l",  "mistral-large-latest",    128000, 8192,    2.0,   6.0,  0, 0, 0 },
    { "mistral-s",  "mistral-small-latest",    128000, 8192,    0.1,   0.3,  0, 0, 0 },
    { "codestral",  "codestral-latest",        256000, 8192,    0.3,   0.9,  0, 0, 0 },
    /* Together */
    { "qwen72b",    "Qwen/Qwen2.5-72B-Instruct-Turbo", 128000, 8192, 0.6, 0.6, 0, 0, 0 },
    /* Cohere */
    { "command-r",  "command-r-plus",          128000, 4096,    2.5,  10.0,  0, 0, 0 },
    /* Cerebras */
    { "cerebras",   "llama3.1-70b",            128000, 8192,    0.60,  0.60, 0, 0, 0 },
    /* xAI */
    { "grok",       "grok-2",                  128000, 8192,    2.0,  10.0,  0, 0, 0 },
    /* Perplexity */
    { "pplx",       "sonar-pro",               200000, 8192,    3.0,  15.0,  0, 0, 0 },
    { "pplx-small", "sonar",                   128000, 8192,    1.0,   1.0,  0, 0, 0 },
    { NULL, NULL, 0, 0, 0, 0, 0, 0, 0 }
};

static inline const model_info_t *model_lookup(const char *name) {
    for (int i = 0; MODEL_REGISTRY[i].alias; i++) {
        if (strcmp(name, MODEL_REGISTRY[i].alias) == 0 ||
            strcmp(name, MODEL_REGISTRY[i].model_id) == 0)
            return &MODEL_REGISTRY[i];
    }
    return NULL;
}

static inline const char *model_resolve_alias(const char *name) {
    const model_info_t *m = model_lookup(name);
    return m ? m->model_id : name;
}

static inline int model_context_window(const char *name) {
    const model_info_t *m = model_lookup(name);
    return m ? m->context_window : CONTEXT_WINDOW_TOKENS;
}

/* ── Effort levels ─────────────────────────────────────────────────────── */

#define EFFORT_LOW    "low"
#define EFFORT_MEDIUM "medium"
#define EFFORT_HIGH   "high"

/* System prompt */
#define SYSTEM_PROMPT \
    "You are dsco, an agentic CLI with self-introspection, swarm, crypto, pipeline, " \
    "and plugin capabilities. You have 110+ tools including file I/O, compilation, " \
    "shell, git, network, and these special capabilities:\n" \
    "1) AST SELF-INTROSPECTION: Use self_inspect, inspect_file, call_graph, and " \
    "dependency_graph to understand any C codebase at the AST level — including " \
    "your own source code. Analyze functions, complexity, dependencies.\n" \
    "2) HIERARCHICAL SWARMS: Use spawn_agent to launch sub-dsco processes that " \
    "can themselves spawn sub-agents (up to depth 4). Use create_swarm for " \
    "parallel agent groups. Sub-agents inherit all tools and API access. " \
    "Monitor with agent_status, collect with swarm_collect. " \
    "For complex tasks, decompose into hierarchies: a coordinator spawns " \
    "specialist agents, each of which can spawn workers.\n" \
    "3) CRYPTO TOOLKIT: Pure C SHA-256, MD5, HMAC-SHA256, HKDF, base64, UUID v4, " \
    "random bytes, JWT decode. Use sha256, md5, hmac, uuid, random_bytes, " \
    "base64_tool, jwt_decode, hkdf.\n" \
    "4) PIPELINE ENGINE: Chain data transforms using coroutines (Tatham's technique). " \
    "Use pipeline with spec like 'filter:error|sort|uniq|head:20'. 30+ stages " \
    "including filter, sort, map, regex, json_extract, csv_column, stats.\n" \
    "5) EXPRESSION EVALUATOR: Use eval for math expressions with variables, " \
    "functions (sin/cos/log/sqrt/gcd/fib), hex/oct/bin literals, factorial. " \
    "Use big_factorial for exact large factorials.\n" \
    "6) PLUGIN SYSTEM: Dynamic .dylib/.so plugins from ~/.dsco/plugins/. " \
    "Use plugin_list, plugin_reload, plugin_load.\n" \
    "7) BASH: Use the 'bash' tool for all shell commands, scripts, pipes, and " \
    "multi-line operations. Supports cwd parameter for directory context. " \
    "Default 120s timeout. Preferred over run_command, but do not use bash when a dedicated tool exists.\n" \
    "8) MARKET DATA: Use market_quote for live ticker/stock/crypto/forex prices.\n" \
    "You operate in a streaming loop. Be concise. Prefer action over explanation. " \
    "When tasks are parallelizable, use swarms. When you need code understanding, " \
    "use AST tools before editing. You can call many tools in a single response — " \
    "use parallel tool calls aggressively for independent operations."

/* ── TUI Feature Flags ─────────────────────────────────────────────────── */

typedef struct {
    bool token_heatmap;         /* F1:  word-level hue by length */
    bool typing_cadence;        /* F2:  buffered streaming at steady rate */
    bool inline_diff;           /* F3:  red/green diff rendering */
    bool collapsible_thinking;  /* F4:  summarize thinking as one-liner */
    bool live_word_count;       /* F5:  right-aligned word/char counter */
    bool paragraph_fade;        /* F6:  fade-in new paragraphs */
    bool citation_footnotes;    /* F7:  tool→footnote mapping */
    bool flame_timeline;        /* F8:  horizontal flame chart after tools */
    bool live_stdout_tee;       /* F9:  dim live tool output */
    bool tool_dep_graph;        /* F10: ASCII DAG for tool chains */
    bool retry_pulse;           /* F11: pulsing retry animation */
    bool result_sparkline;      /* F12: sparkline for numeric outputs */
    bool tool_cost;             /* F13: per-tool cost annotation */
    bool cached_badge;          /* F14: green CACHED badge */
    bool context_gauge;         /* F15: context pressure bar */
    bool conv_minimap;          /* F16: conversation minimap */
    bool compact_flash;         /* F17: compaction notification */
    bool session_diff;          /* F18: session load summary */
    bool branch_indicator;      /* F19: branch detection indicator */
    bool multiline_highlight;   /* F20: syntax highlight pasted code */
    bool ghost_suggestions;     /* F21: ghost command suggestions */
    bool prompt_tokens;         /* F22: prompt token counter */
    bool drag_drop_preview;     /* F23: image drop preview badge */
    bool command_palette;       /* F24: slash command palette */
    bool agent_topology;        /* F25: agent tree visualization */
    bool ipc_message_line;      /* F26: dim IPC log lines */
    bool agent_rollup;          /* F27: agent progress rollup */
    bool swarm_cost;            /* F28: per-agent cost table */
    bool adaptive_theme;        /* F29: auto-detect light/dark theme */
    bool section_dividers;      /* F30: turn dividers with context */
    bool status_clock;          /* F31: clock in status bar */
    bool error_severity;        /* F32: typed error messages */
    bool smooth_scroll;         /* F33: paginated code blocks */
    bool notify_bell;           /* F34: notification bell */
    bool ascii_charts;          /* F35: inline bar charts */
    bool table_sort;            /* F36: sort indicators in tables */
    bool json_tree;             /* F37: JSON tree view */
    bool diff_code_blocks;      /* F38: diff-aware code blocks */
    bool throughput_graph;      /* F39: streaming throughput sparkline */
    bool latency_waterfall;     /* F40: cURL latency breakdown */
} tui_features_t;

static inline void tui_features_init(tui_features_t *f) {
    memset(f, 0, sizeof(*f)); /* start clean */

    /* Safe features that don't use cursor manipulation during streaming */
    f->inline_diff           = true;  /* F3:  red/green diff rendering */
    f->collapsible_thinking  = true;  /* F4:  summarize thinking as one-liner */
    f->cached_badge          = true;  /* F14: green CACHED badge */
    f->context_gauge         = true;  /* F15: context pressure bar */
    f->compact_flash         = true;  /* F17: compaction notification */
    f->error_severity        = true;  /* F32: typed error messages */
    f->notify_bell           = true;  /* F34: notification bell */
    f->drag_drop_preview     = true;  /* F23: image drop preview badge */
    f->section_dividers      = true;  /* F30: turn dividers with inline stats */
    f->tool_dep_graph        = true;  /* F10: compact tool chain display */
    f->citation_footnotes    = true;  /* F7:  tool→footnote mapping (post-stream) */
    f->flame_timeline        = true;  /* F8:  flame chart after multi-tool turns */
    f->tool_cost             = true;  /* F13: per-tool cost annotation */
    f->branch_indicator      = true;  /* F19: git branch detection */
    f->agent_rollup          = true;  /* F27: swarm summary on completion */
    f->ascii_charts          = true;  /* F35: inline bar charts */
    f->table_sort            = true;  /* F36: sort indicators in tables */
    f->throughput_graph       = true;  /* F39: streaming throughput sparkline */

    /* Disabled by default — these cause rendering corruption or noise:
     * F1  token_heatmap       — modifies inline text rendering
     * F2  typing_cadence      — buffers stdout, causes partial writes
     * F5  live_word_count     — cursor save/restore to row 1 during streaming
     * F6  paragraph_fade      — interferes with streaming text
     * F9  live_stdout_tee     — tees tool output during execution
     * F12 result_sparkline    — tries to detect numbers in output
     * F16 conv_minimap        — cursor manipulation
     * F21 ghost_suggestions   — ghost command suggestions
     * F22 prompt_tokens       — prompt token counter
     * F25 agent_topology      — agent tree visualization
     * F26 ipc_message_line    — dim IPC log lines
     * F28 swarm_cost          — per-agent cost table
     * F29 adaptive_theme      — auto-detect (safe but leave off for simplicity)
     * F31 status_clock        — clock in status bar
     * F33 smooth_scroll       — paginated code blocks
     * F37 json_tree           — JSON tree view
     * F38 diff_code_blocks    — diff-aware code blocks
     * F40 latency_waterfall   — cURL latency breakdown
     */
}

static inline const char *tui_feature_name(int idx) {
    static const char *names[] = {
        "token_heatmap", "typing_cadence", "inline_diff", "collapsible_thinking",
        "live_word_count", "paragraph_fade", "citation_footnotes", "flame_timeline",
        "live_stdout_tee", "tool_dep_graph", "retry_pulse", "result_sparkline",
        "tool_cost", "cached_badge", "context_gauge", "conv_minimap",
        "compact_flash", "session_diff", "branch_indicator", "multiline_highlight",
        "ghost_suggestions", "prompt_tokens", "drag_drop_preview", "command_palette",
        "agent_topology", "ipc_message_line", "agent_rollup", "swarm_cost",
        "adaptive_theme", "section_dividers", "status_clock", "error_severity",
        "smooth_scroll", "notify_bell", "ascii_charts", "table_sort",
        "json_tree", "diff_code_blocks", "throughput_graph", "latency_waterfall",
    };
    if (idx >= 0 && idx < 40) return names[idx];
    return "unknown";
}

#define TUI_FEATURE_COUNT 40

#endif
