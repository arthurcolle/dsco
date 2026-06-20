#ifndef DSCO_CONFIG_H
#define DSCO_CONFIG_H

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <ctype.h>

#define DSCO_VERSION "1.0.0"

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

/* Tool register file: hard cap on tools per API request.
 * Like CPU registers — 32 slots max, tools evicted/loaded dynamically.
 * bash+python always core; everything else loaded via load_tools/hints.
 * Budget-adaptive: full=32, mid=24, low=13, critical=5 */
#define TOOL_REGISTER_CAP       32
#define TOOL_REG_ALWAYS          7   /* R0-R6:   bash,python,discover,load,exit,loop */
#define TOOL_REG_WARM           11   /* file I/O + run_command, evictable */
#define TOOL_REG_WORKING        10   /* quorum-scored, turn-volatile */
#define TOOL_REG_DISCOVERY       4   /* R28-R31: progressive schema, ephemeral */
#define QUORUM_MIN_SIGNALS       2   /* min independent signals to load a tool */
#define MAX_INPUT_LINE      65536

/* --cheap mode: send only ALWAYS-core tools (5) + skip compact catalog.
 * Cuts first-prompt cost from ~$0.40 to ~$0.05 by evicting all tools
 * except discover_tools/load_tools (which let the model page them in). */
extern int g_cheap_mode;

/* API defaults */
#define DEFAULT_MODEL       "moonshotai/kimi-k2.7-code"
#define API_URL_ANTHROPIC   "https://api.anthropic.com/v1/messages"
#define API_URL_COUNT_TOKENS "https://api.anthropic.com/v1/messages/count_tokens"
#define ANTHROPIC_VERSION   "2023-06-01"
#define ANTHROPIC_BETAS     "interleaved-thinking-2025-05-14,code-execution-2025-05-22,advanced-tool-use-2025-11-20"
#define MAX_TOKENS          16384
#define TOOLMGMT_API_URL_DEFAULT "https://tools.distributed.systems"

/* Tool limits */
#define MAX_FILE_PAGE_SIZE  4096
#define MAX_EXEC_OUTPUT     (64 * 1024)

/* Agent loop — 40 turns default balances utility vs cost.
   Research (W&D 2026): most tasks complete in <20 turns;
   >40 indicates runaway loops or redundant retrieval. */
#define MAX_AGENT_TURNS     40
/* Override at runtime via DSCO_MAX_AGENT_TURNS */
static inline int dsco_max_agent_turns(void) {
    const char *e = getenv("DSCO_MAX_AGENT_TURNS");
    if (e && e[0]) {
        int v = atoi(e);
        if (v >= 1 && v <= 999999) return v;
    }
    return MAX_AGENT_TURNS;
}

/* Context window / token budget */
#define CONTEXT_WINDOW_TOKENS  2000000  /* x-ai/grok-4.20-beta */
#define TOKEN_BUDGET_WARN      0.85    /* warn at 85% usage */
#define TOKEN_BUDGET_COMPACT   0.80    /* auto-compact at 80% */

/* Output-aware context management (inspired by Claude Code):
 * Effective window = context_window - max_output_reserve
 * Compact threshold = effective_window - buffer_tokens
 * This prevents compacting too late when output reservation is large. */
#define AUTOCOMPACT_BUFFER_TOKENS  13000  /* safety margin below effective window */
#define SNIP_KEEP_HEAD             3      /* rounds to preserve at start */
#define SNIP_KEEP_TAIL             6      /* rounds to preserve at end */
#define COMPACT_CIRCUIT_BREAKER    3      /* max consecutive compact failures */

/* Session storage */
#define DSCO_SESSION_DIR     "~/.dsco/sessions"
#define DSCO_DEBUG_DIR       "~/.dsco/debug"
#define DSCO_MCP_CONFIG      "~/.dsco/mcp.json"
#define DSCO_SYSTEM_PROMPT   "~/.dsco/system_prompt.txt"
#define DSCO_PLUGINS_DIR     "~/.dsco/plugins"
#define DSCO_WORKSPACE_DIR   "~/.dsco/workspace"
#define DSCO_WORKSPACE_SKILLS_DIR "~/.dsco/workspace/skills"

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

/* Runtime OpenRouter catalog (openrouter_cache.c). Returns a pointer to a
 * process-lifetime-stable model_info_t for any slug present in the cached
 * OpenRouter /models response, or NULL if absent / not yet loaded. Lets
 * model_lookup() resolve any real slug — no hardcoded alias required. */
const model_info_t *openrouter_cache_lookup(const char *slug);

static const model_info_t MODEL_REGISTRY[] = {
    /* ── Anthropic (native API) ──────────────────────────────────────────── */
    { "opus",         "claude-opus-4-7",             200000, 32000,  15.0,  75.0,  1.50, 18.75, 1 },
    { "opus46",       "claude-opus-4-6",             200000, 32000,  15.0,  75.0,  1.50, 18.75, 1 },
    { "sonnet",       "claude-sonnet-4-6",           200000, 16000,   3.0,  15.0,  0.30,  3.75, 1 },
    { "haiku",        "claude-haiku-4-5-20251001",   200000,  8192,   0.80,  4.0,  0.08,  1.00, 0 },
    /* ── Anthropic (OpenRouter IDs — for cross-provider routing) ─────── */
    { "or-opus47",    "anthropic/claude-opus-4.7",    1000000, 32000, 15.0,  75.0,  0, 0, 1 },
    { "or-opus46",    "anthropic/claude-opus-4.6",    1000000, 32000,  5.0,  25.0,  0, 0, 1 },
    { "or-sonnet46",  "anthropic/claude-sonnet-4.6",  1000000, 16000,  3.0,  15.0,  0, 0, 1 },
    { "or-opus45",    "anthropic/claude-opus-4.5",     200000, 32000,  5.0,  25.0,  0, 0, 1 },
    { "or-sonnet45",  "anthropic/claude-sonnet-4.5",  1000000, 16000,  3.0,  15.0,  0, 0, 1 },
    /* ── OpenAI — GPT-5.x family (2026 frontier) ────────────────────── */
    { "gpt54-pro",    "openai/gpt-5.4-pro",          1050000, 32768, 30.0, 180.0,  0, 0, 1 },
    { "gpt54",        "openai/gpt-5.4",              1050000, 32768,  2.50, 15.0,  0, 0, 1 },
    { "gpt54-mini",   "openai/gpt-5.4-mini",           400000, 32768,  0.75,  4.50, 0, 0, 0 },
    { "gpt54-nano",   "openai/gpt-5.4-nano",           400000, 32768,  0.20,  1.25, 0, 0, 0 },
    { "gpt55-pro",    "openai/gpt-5.5-pro",            400000, 32768,  6.0,  24.0,  0, 0, 1 },
    { "gpt55",        "openai/gpt-5.5",                400000, 32768,  1.75, 14.0,  0, 0, 1 },
    { "gpt53-codex",      "openai/gpt-5.3-codex",           400000, 32768,  1.75, 14.0,  0, 0, 1 },
    { "gpt52-pro",        "openai/gpt-5.2-pro",              400000, 32768, 21.0, 168.0,  0, 0, 1 },
    { "gpt52",            "openai/gpt-5.2",                  400000, 32768,  1.75, 14.0,  0, 0, 1 },
    { "gpt52-codex",      "openai/gpt-5.2-codex",            400000, 32768,  1.75, 14.0,  0, 0, 1 },
    { "gpt51",            "openai/gpt-5.1",                  400000, 32768,  1.25, 10.0,  0, 0, 1 },
    { "gpt51-codex",      "openai/gpt-5.1-codex",            400000, 32768,  1.25, 10.0,  0, 0, 1 },
    { "gpt51-codex-mini", "openai/gpt-5.1-codex-mini",       400000, 32768,  0.50,  4.0,  0, 0, 0 },
    { "gpt51-codex-max",  "openai/gpt-5.1-codex-max",        400000, 32768,  3.0,  24.0,  0, 0, 1 },
    { "gpt5",         "openai/gpt-5",                   400000, 32768,  1.25, 10.0,  0, 0, 1 },
    { "gpt5-pro",     "openai/gpt-5-pro",               400000, 32768, 15.0, 120.0,  0, 0, 1 },
    { "gpt5-codex",   "openai/gpt-5-codex",             400000, 32768,  1.25, 10.0,  0, 0, 1 },
    { "gpt5-mini",    "openai/gpt-5-mini",              400000, 32768,  0.25,  2.0,  0, 0, 0 },
    { "gpt5-nano",    "openai/gpt-5-nano",              400000, 32768,  0.05,  0.40, 0, 0, 0 },
    /* ── OpenAI — GPT-4.x / 4o family ───────────────────────────────── */
    { "gpt41",        "openai/gpt-4.1",               1047576, 32768,  2.0,   8.0,  0, 0, 0 },
    { "gpt41-mini",   "openai/gpt-4.1-mini",          1047576, 32768,  0.40,  1.60, 0, 0, 0 },
    { "gpt41-nano",   "openai/gpt-4.1-nano",          1047576, 32768,  0.10,  0.40, 0, 0, 0 },
    { "gpt4o",        "openai/gpt-4o",                 128000, 16384,  2.50, 10.0,  0, 0, 0 },
    { "gpt4o-mini",   "openai/gpt-4o-mini",            128000, 16384,  0.15,  0.60, 0, 0, 0 },
    /* ── OpenAI — o-series reasoning ─────────────────────────────────── */
    { "o4-mini",      "openai/o4-mini",                200000, 100000,  1.10,  4.40, 0, 0, 1 },
    { "o4-mini-hi",   "openai/o4-mini-high",           200000, 100000,  1.10,  4.40, 0, 0, 1 },
    { "o3",           "openai/o3",                     200000, 100000,  2.0,   8.0,  0, 0, 1 },
    { "o3-pro",       "openai/o3-pro",                 200000, 100000, 20.0,  80.0,  0, 0, 1 },
    { "o3-mini",      "openai/o3-mini",                200000, 100000,  1.10,  4.40, 0, 0, 1 },
    { "o1",           "openai/o1",                     200000, 100000, 15.0,  60.0,  0, 0, 1 },
    /* ── OpenAI — open-source models ─────────────────────────────────── */
    { "gpt-oss",      "openai/gpt-oss-120b",           131072, 32768,  0.04,  0.19, 0, 0, 0 },
    /* ── Google Gemini ───────────────────────────────────────────────── */
    { "gem31-pro",    "google/gemini-3.1-pro-preview", 1048576, 32768,  2.0,  12.0,  0, 0, 1 },
    { "gem31-flash",  "google/gemini-3.1-flash-lite-preview", 1048576, 32768, 0.25, 1.50, 0, 0, 0 },
    { "gem3-pro",     "google/gemini-3-pro-preview",   1048576, 32768,  2.0,  12.0,  0, 0, 1 },
    { "gem3-flash",   "google/gemini-3-flash-preview", 1048576, 32768,  0.50,  3.0,  0, 0, 0 },
    { "gem25-pro",    "google/gemini-2.5-pro",         1048576, 32768,  1.25, 10.0,  0, 0, 1 },
    { "gem25-flash",  "google/gemini-2.5-flash",       1048576, 32768,  0.30,  2.50, 0, 0, 0 },
    /* ── xAI Grok (via OpenRouter) ───────────────────────────────────── */
    { "grok4",        "x-ai/grok-4.20-beta",           2000000, 32768,  2.0,   6.0,  0, 0, 1 },
    { "grok4-ma",     "x-ai/grok-4.20-multi-agent-beta", 2000000, 32768, 2.0,  6.0,  0, 0, 1 },
    /* ── xAI Grok (native api.x.ai) ──────────────────────────────────── */
    { "grok-4-fast",  "grok-4-fast",                    2000000, 32768,  0.20,  0.50, 0, 0, 1 },
    { "grok-4",       "grok-4",                          256000, 32768,  3.00, 15.00, 0, 0, 1 },
    { "grok-3",       "grok-3",                          131072, 32768,  3.00, 15.00, 0, 0, 0 },
    { "grok-3-mini",  "grok-3-mini",                     131072, 32768,  0.30,  0.50, 0, 0, 1 },
    { "grok-code",    "grok-code-fast-1",                262144, 32768,  0.20,  1.50, 0, 0, 0 },
    /* ── Moonshot Kimi (via OpenRouter) ──────────────────────────────── */
    /* Raw OpenRouter slugs only — alias == model_id so `-m <slug>` is the
     * canonical, lookup-able name. The OpenRouter catalog is refreshed at
     * runtime (see openrouter_cache.h), so any slug not listed here still
     * resolves with real context/pricing once the background fetch lands. */
    { "moonshotai/kimi-k2.7-code", "moonshotai/kimi-k2.7-code", 262144, 16384, 0.74, 3.50, 0.15, 0, 1 },
    { "kimi",         "moonshotai/kimi-k2.5",           262144, 16384,  0.45,  2.20, 0, 0, 1 },
    { "kimi-k2",      "moonshotai/kimi-k2",             131000, 16384,  0.55,  2.20, 0, 0, 0 },
    { "kimi-think",   "moonshotai/kimi-k2-thinking",    131072, 16384,  0.47,  2.00, 0, 0, 1 },
    /* ── Moonshot Kimi (native platform.moonshot.ai) ─────────────────── */
    { "mk25",         "kimi-k2.5",                      262144, 32768,  0.60,  3.00, 0.10, 0, 1 },
    { "mk2t",         "kimi-k2-turbo-preview",          262144, 32768,  0.60,  3.00, 0.10, 0, 0 },
    { "mk2-think",    "kimi-k2-thinking",               262144, 32768,  0.60,  3.00, 0.10, 0, 1 },
    { "mk2-think-tb", "kimi-k2-thinking-turbo",         262144, 32768,  0.60,  3.00, 0.10, 0, 1 },
    /* ── Zhipu GLM ───────────────────────────────────────────────────── */
    { "glm52",        "z-ai/glm-5.2",                  1048576, 262144, 1.40,  4.40, 0, 0, 1 },
    { "glm51",        "z-ai/glm-5.1",                  202752, 65536,  0.72,  2.30, 0, 0, 1 },
    { "glm5",         "z-ai/glm-5",                    202752, 65536,  0.72,  2.30, 0, 0, 1 },
    { "glm5-turbo",   "z-ai/glm-5-turbo",              202752, 65536,  0.96,  3.20, 0, 0, 1 },
    { "glm47",        "z-ai/glm-4.7",                  202752, 65536,  0.38,  1.98, 0, 0, 1 },
    { "glm47-flash",  "z-ai/glm-4.7-flash",            202752, 65536,  0.06,  0.40, 0, 0, 0 },
    /* ── DeepSeek ────────────────────────────────────────────────────── */
    { "ds-v32",       "deepseek/deepseek-v3.2",        163840, 32768,  0.26,  0.38, 0, 0, 0 },
    { "ds-v31",       "deepseek/deepseek-v3.1-terminus", 163840, 32768, 0.21, 0.79, 0, 0, 0 },
    { "ds-chat",      "deepseek/deepseek-chat",         163840, 32768,  0.32,  0.89, 0, 0, 0 },
    { "ds-r1",        "deepseek/deepseek-r1-0528",      163840, 32768,  0.45,  2.15, 0, 0, 1 },
    /* ── Qwen 3.5 ────────────────────────────────────────────────────── */
    { "qwen-flash",   "qwen/qwen3.5-flash-02-23",     1000000, 32768,  0.10,  0.40, 0, 0, 0 },
    { "qwen-plus",    "qwen/qwen3.5-plus-02-15",      1000000, 32768,  0.26,  1.56, 0, 0, 0 },
    { "qwen-397b",    "qwen/qwen3.5-397b-a17b",        262144, 32768,  0.39,  2.34, 0, 0, 0 },
    { "qwen-122b",    "qwen/qwen3.5-122b-a10b",        262144, 32768,  0.26,  2.08, 0, 0, 0 },
    { "qwen-coder",   "qwen/qwen3-coder-next",         262144, 32768,  0.12,  0.75, 0, 0, 0 },
    { "qwen-think",   "qwen/qwen3-max-thinking",       262144, 32768,  0.78,  3.90, 0, 0, 1 },
    /* ── Meta Llama ──────────────────────────────────────────────────── */
    { "llama4-mav",   "meta-llama/llama-4-maverick",   1048576, 32768,  0.15,  0.60, 0, 0, 0 },
    { "llama4-scout", "meta-llama/llama-4-scout",       327680, 32768,  0.08,  0.30, 0, 0, 0 },
    { "llama33-70b",  "meta-llama/llama-3.3-70b-instruct", 131072, 32768, 0.10, 0.32, 0, 0, 0 },
    /* ── Mistral (2025/2026) ─────────────────────────────────────────── */
    { "mistral-l3",   "mistralai/mistral-large-2512",   262144, 32768,  0.50,  1.50, 0, 0, 0 },
    { "mixtral",      "mistralai/mixtral-8x7b-instruct-v0.1", 32768, 32768, 0.0, 0.0, 0, 0, 0 },
    { "devstral",     "mistralai/devstral-2512",         262144, 32768,  0.40,  2.00, 0, 0, 0 },
    { "mistral-med",  "mistralai/mistral-medium-3.1",    131072, 32768,  0.40,  2.00, 0, 0, 0 },
    { "mistral-s32",  "mistralai/mistral-small-3.2-24b-instruct", 131072, 32768, 0.06, 0.18, 0, 0, 0 },
    { "codestral",    "mistralai/codestral-2508",        256000, 32768,  0.30,  0.90, 0, 0, 0 },
    /* ── ByteDance Seed ──────────────────────────────────────────────── */
    { "seed2",        "bytedance-seed/seed-2.0-lite",    262144, 32768,  0.25,  2.00, 0, 0, 0 },
    { "seed2-mini",   "bytedance-seed/seed-2.0-mini",    262144, 32768,  0.10,  0.40, 0, 0, 0 },
    /* ── Amazon Nova ─────────────────────────────────────────────────── */
    { "nova-premier", "amazon/nova-premier-v1",         1000000, 32768,  2.50, 12.50, 0, 0, 1 },
    { "nova2-lite",   "amazon/nova-2-lite-v1",          1000000, 32768,  0.30,  2.50, 0, 0, 0 },
    /* ── MiniMax ─────────────────────────────────────────────────────── */
    { "minimax",      "minimax/minimax-m2.5",            196608, 32768,  0.25,  1.20, 0, 0, 0 },
    /* ── Writer ──────────────────────────────────────────────────────── */
    { "palmyra",      "writer/palmyra-x5",              1040000, 32768,  0.60,  6.00, 0, 0, 0 },
    /* ── NVIDIA ──────────────────────────────────────────────────────── */
    { "nemotron",     "nvidia/nemotron-3-super-120b-a12b:free", 262144, 32768, 0, 0, 0, 0, 0 },
    /* ── Cohere ──────────────────────────────────────────────────────── */
    { "command-a",    "cohere/command-a",                256000, 32768,  2.50, 10.0,  0, 0, 0 },
    /* ── NousResearch ────────────────────────────────────────────────── */
    { "hermes4",      "nousresearch/hermes-4-405b",      131072, 32768,  1.00,  3.00, 0, 0, 0 },
    /* ── StepFun ─────────────────────────────────────────────────────── */
    { "step35",       "stepfun/step-3.5-flash",          256000, 32768,  0.10,  0.30, 0, 0, 0 },
    /* ── Inception Mercury ───────────────────────────────────────────── */
    { "mercury",      "inception/mercury-2",             128000, 32768,  0.25,  0.75, 0, 0, 0 },
    /* ── Baidu ERNIE ─────────────────────────────────────────────────── */
    { "ernie45",      "baidu/ernie-4.5-300b-a47b",      123000, 32768,  0.28,  1.10, 0, 0, 0 },
    /* ── Arcee AI ────────────────────────────────────────────────────── */
    { "arcee-reason", "arcee-ai/maestro-reasoning",      131072, 32768,  0.90,  3.30, 0, 0, 1 },
    /* ── Xiaomi ──────────────────────────────────────────────────────── */
    { "mimo",         "xiaomi/mimo-v2-flash",            262144, 32768,  0.09,  0.29, 0, 0, 0 },
    /* ── Aion Labs ───────────────────────────────────────────────────── */
    { "aion",         "aion-labs/aion-2.0",              131072, 32768,  0.80,  1.60, 0, 0, 0 },
    /* ── KwaiPilot ───────────────────────────────────────────────────── */
    { "kat-coder",    "kwaipilot/kat-coder-pro",         256000, 32768,  0.21,  0.83, 0, 0, 1 },
    /* ── Groq (fast native inference, not OpenRouter) ────────────────── */
    { "llama70b",     "llama-3.3-70b-versatile",         128000, 32768,  0.59,  0.79, 0, 0, 0 },
    { "llama8b",      "llama-3.1-8b-instant",            128000,  8192,  0.05,  0.08, 0, 0, 0 },
    /* ── Perplexity ──────────────────────────────────────────────────── */
    { "pplx",         "sonar-pro",                       200000,  8192,  3.0,  15.0,  0, 0, 0 },
    { "pplx-small",   "sonar",                           128000,  8192,  1.0,   1.0,  0, 0, 0 },
    { NULL, NULL, 0, 0, 0, 0, 0, 0, 0 }
};

static inline void model_normalize_key(const char *src, char *dst, size_t dst_len) {
    if (!dst || dst_len == 0) return;
    dst[0] = '\0';
    if (!src || !*src) return;

    const char *p = src;
    const char *slash = strchr(src, '/');
    if (slash && slash[1] != '\0') p = slash + 1;

    size_t out = 0;
    for (; *p && out + 1 < dst_len; p++) {
        unsigned char c = (unsigned char)*p;
        if (!isalnum(c)) continue;
        dst[out++] = (char)tolower(c);
    }
    dst[out] = '\0';
}

static inline const model_info_t *model_lookup(const char *name) {
    if (!name || !*name) return NULL;

    /* Pass 1: exact match against alias or model_id for any entry. An exact
     * hit must always beat a fuzzy normalized hit, otherwise an early-listed
     * entry whose model_id normalizes the same as a later entry's alias
     * (e.g. "moonshotai/kimi-k2.5" vs native "kimi-k2.5") wins incorrectly. */
    for (int i = 0; MODEL_REGISTRY[i].alias; i++) {
        if (strcmp(name, MODEL_REGISTRY[i].alias) == 0 ||
            strcmp(name, MODEL_REGISTRY[i].model_id) == 0)
            return &MODEL_REGISTRY[i];
    }

    /* Pass 2: normalized (case/punctuation-insensitive) match. */
    char want_norm[256];
    model_normalize_key(name, want_norm, sizeof(want_norm));
    if (want_norm[0] == '\0') return NULL;

    for (int i = 0; MODEL_REGISTRY[i].alias; i++) {
        char alias_norm[256];
        char model_norm[256];
        model_normalize_key(MODEL_REGISTRY[i].alias, alias_norm, sizeof(alias_norm));
        model_normalize_key(MODEL_REGISTRY[i].model_id, model_norm, sizeof(model_norm));
        if (strcmp(want_norm, alias_norm) == 0 || strcmp(want_norm, model_norm) == 0)
            return &MODEL_REGISTRY[i];
    }

    /* Pass 3: runtime OpenRouter catalog — any real slug resolves with live
     * context/pricing once the background fetch has populated the cache. */
    return openrouter_cache_lookup(name);
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
    "You are dsco, an agentic CLI built on the Overmind Soul architecture.\n" \
    "Three-layer design: Wings (soar) + Talons (win) + Immune System (survive).\n\n" \
    "WINGS (Autonomy & Emergence):\n" \
    "- PHEROMONE COORDINATION: Stigmergic signals (PROGRESS/ATTRACTION/WARNING/SUCCESS/" \
    "HELP_NEEDED/CAPACITY) with exponential decay. Coordinate without central planning.\n" \
    "- THREE-TIER MEMORY: Working (60s), Episodic (1h), Semantic (permanent). " \
    "Auto-consolidation promotes important memories upward.\n" \
    "- HIERARCHICAL SWARMS: Sub-agent hierarchies (depth 6). " \
    "topology_run for fanout/fanin, debate, competition.\n" \
    "- CAPABILITY MATCHING: EXPERT/PROFICIENT/COMPETENT/NOVICE. " \
    "Self-assess and delegate when outmatched.\n\n" \
    "TALONS (Competitive Execution — the ability to WIN):\n" \
    "- GOAL PURSUIT: Track goals through hunt states: nascent -> stalking -> " \
    "striking -> gripping -> captured (win) or escaped (fail). " \
    "Use talons_goal to create, talons_advance to progress.\n" \
    "- GRIP STRENGTH: tentative (1 retry), holding (3), locked (7), death_grip (20). " \
    "Failed goals auto-retry based on grip. Escalate grip for critical objectives.\n" \
    "- TOURNAMENT SELECTION: Race N strategies in parallel, pick the winner. " \
    "Use talons_tournament to begin/add/result/decide. Scored by quality/speed/cost.\n" \
    "- STRATEGY ENGINE: direct, flanking, tournament, escalation, divide, ambush. " \
    "talons_recommend learns from win/loss history to suggest best approach.\n" \
    "- ADAPTIVE: Strategy success rates updated from every hunt. The system gets " \
    "better at winning over time.\n\n" \
    "IMMUNE SYSTEM (Guardrails & Safety):\n" \
    "- OODA DISCIPLINE: Observe->Orient->Decide->Act for non-trivial decisions.\n" \
    "- KILL SWITCHES: 5 granularities (agent/workflow/service/pheromone/system).\n" \
    "- GSU BUDGETS: Resource accounting with hard limits — no overdraft.\n" \
    "- PRINCIPAL TIERS: Tier 0 (Founder) > Tier 1 (Operator) > Tier 2 (Agent) > Tier 3 (User).\n" \
    "- HARDCODED BEHAVIORS: Non-bypassable rules (must-always/must-never).\n" \
    "- GOVERNANCE CHECKPOINT: hardcoded -> budget -> killswitch -> authorize -> audit.\n\n" \
    "TOOLS: 364+ tools across file I/O, git, network, shell, crypto, pipeline, " \
    "math, AST, plugins, market data, prediction markets, soul evolution.\n" \
    "The TOOL CATALOG below lists every tool with its signature (param* = required).\n" \
    "Call any tool directly — the catalog has all the parameter info you need.\n" \
    "MULTI-EXECUTOR SWARMS: dsco (fork self), claude (Claude Code CLI), codex (OpenAI Codex).\n\n" \
    "TOKEN EFFICIENCY:\n" \
    "- Issue 3+ parallel tool calls per step when gathering information (36% cheaper, 41% faster).\n" \
    "- For external parallelism, you may use bash to launch local dsc or dsco worker processes when swarm/executor tools are not the best fit.\n" \
    "- Large tool results are truncated inline. Full results persist in VFS — use context_recall to retrieve.\n" \
    "- Durable artifacts require proof: prefer write_file/append_file; if bash creates files, declare verify_path/verify_paths with optional size/content/hash checks.\n" \
    "- Do not use context_search/context_get/context_pack — they are deprecated.\n" \
    "- Be concise. Prefer action over explanation.\n" \
    "Create goals for complex tasks. Use tournaments when multiple approaches exist."

/* Cheap-mode system prompt: minimal, no catalog reference, teaches discover/load */
#define SYSTEM_PROMPT_CHEAP \
    "You are dsco, an agentic CLI with 364+ tools.\n" \
    "You are running in CHEAP MODE — only 5 core tools are loaded to minimize cost.\n\n" \
    "YOUR ACTIVE TOOLS:\n" \
    "  bash          — run shell commands\n" \
    "  python        — execute Python code\n" \
    "  discover_tools — browse all 364+ tools by category\n" \
    "  load_tools    — page in tools you need (they become callable immediately)\n" \
    "  self_exit     — end session\n\n" \
    "WORKFLOW: For any task beyond bash/python, call discover_tools to find " \
    "relevant tools, then load_tools to activate them. Loaded tools persist " \
    "for the session. Categories: file_io, git, network, shell, code, crypto, " \
    "swarm, ast, pipeline, math, search, general, finance, prediction, memory.\n\n" \
    "EFFICIENCY:\n" \
    "- Only load tools you actually need — each adds ~200 tokens per turn.\n" \
    "- Prefer bash/python for simple tasks over loading specialized tools.\n" \
    "- Durable artifacts require proof: prefer write_file/append_file; if bash creates files, declare verify_path/verify_paths with optional size/content/hash checks.\n" \
    "- Issue parallel tool calls when gathering information.\n" \
    "- For external parallelism, bash may launch local dsc or dsco workers when that is simpler than loading swarm tools.\n" \
    "- Be concise. Prefer action over explanation."

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
