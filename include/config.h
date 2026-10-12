#ifndef DSCO_CONFIG_H
#define DSCO_CONFIG_H

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <ctype.h>
#include "claude_models.h"
#include "env_config.h"

#define DSCO_VERSION "1.1.0"

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

/* Tool register file: bounded tools per API request.
 * Like a wide CPU register file: 48 slots max, dynamically paged by relevance.
 * ALWAYS remains small and cache-stable; added density goes to quorum-scored
 * working tools and compact progressive discovery schemas.
 * Budget-adaptive: full=48, mid=31, low=20, critical=12. */
#define TOOL_REGISTER_CAP       48
#define TOOL_REG_ALWAYS         16   /* execution, disclosure, context controls, loop controls */
#define TOOL_REG_WARM           11   /* file I/O + run_command, evictable */
#define TOOL_REG_WORKING        18   /* quorum-scored, frozen for cache stability */
#define TOOL_REG_DISCOVERY       6   /* compact progressive schema, ephemeral */
#define QUORUM_MIN_SIGNALS       2   /* min independent signals to load a tool */
#define MAX_INPUT_LINE      65536

/* --cheap mode: send only ALWAYS-core tools + skip compact catalog.
 * Cuts first-prompt cost from ~$0.40 to ~$0.05 by evicting all tools
 * except discover_tools/load_tools/evict_tools (which let the model page them in/out). */
extern int g_cheap_mode;

/* API defaults. Explicit CLI, environment, and saved-profile settings remain
 * layerable overrides; these values define the no-flag native runtime. */
#define DEFAULT_PROVIDER         "openai-codex"
#define DEFAULT_MODEL            "gpt-5.6-luna"
#define DEFAULT_EFFORT           "xhigh"
#define DEFAULT_SUBAGENT_MODEL   "gpt-6-astra"
#define KIMI_CODE_DEFAULT_MODEL  "kimi-code/k3"

/* ── Compile-time runtime posture defaults ─────────────────────────────────
   The issued binary bakes in --interactive, --gov-model none, and --autonomous
   as its default state. These are *defaults*, not locks: an explicit CLI flag
   (e.g. --gov-model standard, --approval-mode ask) or a pre-set environment
   variable still overrides them because they are applied with setenv(...,0)
   (non-overwriting) and the flag parser runs afterward.

   Override the compile-time posture entirely by defining these to 0 at build
   time, e.g. `make CFLAGS_EXTRA="-DDSCO_DEFAULT_GOV_NONE=0 \
   -DDSCO_DEFAULT_AUTONOMOUS=0 -DDSCO_DEFAULT_INTERACTIVE=0"`. */
#ifndef DSCO_DEFAULT_INTERACTIVE
#define DSCO_DEFAULT_INTERACTIVE 1
#endif
#ifndef DSCO_DEFAULT_AUTONOMOUS
#define DSCO_DEFAULT_AUTONOMOUS  1
#endif
#ifndef DSCO_DEFAULT_GOV_NONE
#define DSCO_DEFAULT_GOV_NONE    1
#endif

/* Default swarm worker model. The root agent stays on Luna while unpinned
 * native children use Astra. Override with DSCO_SWARM_MODEL, or set
 * DSCO_SWARM_DEFAULT_MINI=0 for legacy parent-model inheritance. Structural
 * fanout cap is SWARM_MAX_CHILDREN (256), with a conservative runtime default
 * of SWARM_DEFAULT_MAX_CHILDREN (64). */
#define DEFAULT_SWARM_MODEL DEFAULT_SUBAGENT_MODEL
static inline const char *dsco_swarm_default_model(const char *api_key_unused) {
    (void)api_key_unused;
    const char *configured = getenv("DSCO_SWARM_MODEL");
    if (configured && configured[0])
        return configured;
    const char *v = getenv("DSCO_SWARM_DEFAULT_MINI");
    if (v && v[0] == '0' && v[1] == '\0')
        return NULL; /* opt out: inherit parent model */
    return DEFAULT_SWARM_MODEL;
}
/* Sensitive endpoint/header constants. In hardened builds (-DDSCO_USE_OBF_SECRETS,
 * set by `make harden`) these resolve to runtime lookups of encrypted values so
 * the literals never appear in __cstring; dev builds keep them inline for
 * debuggability. All use sites are runtime contexts (curl/snprintf/assignment),
 * so a function call substitutes cleanly. See src/embedded_data.c / data/dsco_secrets.txt. */
const char *dsco_secret(const char *key);
#ifdef DSCO_USE_OBF_SECRETS
#define API_URL_ANTHROPIC    dsco_secret("API_URL_ANTHROPIC")
#define API_URL_COUNT_TOKENS dsco_secret("API_URL_COUNT_TOKENS")
#define ANTHROPIC_VERSION    dsco_secret("ANTHROPIC_VERSION")
#define ANTHROPIC_BETAS      dsco_secret("ANTHROPIC_BETAS")
#else
#define API_URL_ANTHROPIC   "https://api.anthropic.com/v1/messages"
#define API_URL_COUNT_TOKENS "https://api.anthropic.com/v1/messages/count_tokens"
#define ANTHROPIC_VERSION   "2023-06-01"
#define ANTHROPIC_BETAS     "interleaved-thinking-2025-05-14,code-execution-2025-05-22,advanced-tool-use-2025-11-20"
#endif
#define MAX_TOKENS          16384
static inline int dsco_max_tokens(void) {
    return dsco_env_int("DSCO_MAX_TOKENS", MAX_TOKENS, 1, 100000);
}
#define TOOLMGMT_API_URL_DEFAULT "https://tools.distributed.systems"

/* Tool limits */
#define MAX_FILE_PAGE_SIZE  4096
#define MAX_EXEC_OUTPUT     (64 * 1024)

/* Agent loop — agentic by default: the loop runs until the goal is met or a
   *real* stop condition trips (cost budget, context exhaustion, user interrupt,
   or a runaway no-progress signal). It is NOT capped by an arbitrary turn count.

   MAX_AGENT_TURNS is therefore a *checkpoint cadence*, not a wall: every this
   many turns the loop surfaces a visible "still working" checkpoint so the user
   can see the agent is converging (and interrupt if not). Override the cadence
   via DSCO_MAX_AGENT_TURNS. */
#define MAX_AGENT_TURNS     40
static inline int dsco_max_agent_turns(void) {
    return dsco_env_int("DSCO_MAX_AGENT_TURNS", MAX_AGENT_TURNS, 1, 999999);
}

/* A request ceiling is opt-in. Zero means continue until completion, an
   explicit budget, operator interruption, or a real execution failure. */
#define HARD_TURN_CEILING   0
static inline int dsco_hard_turn_ceiling(void) {
    return dsco_env_int("DSCO_HARD_TURN_CEILING", HARD_TURN_CEILING, 0, 100000000);
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

/* Catalog-supplied context price tiers; bounded data, no model-name rules. */
typedef struct {
    int above_tokens;
    double input, output, cache_read, cache_write;
} model_context_price_t;
#define MODEL_CONTEXT_PRICE_TIERS 8


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
    int         context_price_count;
    model_context_price_t context_prices[MODEL_CONTEXT_PRICE_TIERS];
} model_info_t;

/* Runtime OpenRouter catalog (openrouter_cache.c). Returns a pointer to a
 * process-lifetime-stable model_info_t for any slug present in the cached
 * OpenRouter /models response, or NULL if absent / not yet loaded. Lets
 * model_lookup() resolve any real slug — no hardcoded alias required. */
const model_info_t *openrouter_cache_lookup(const char *slug);
const model_info_t *codex_cache_lookup(const char *name);

/* Static fallback rows omit optional live-catalog tiers (zero initialized). */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
static const model_info_t MODEL_REGISTRY[] = {
    /* ── Anthropic (native API) ──────────────────────────────────────────── */
    /* Official standard rates checked 2026-10-07; explicit IDs preserve old releases. */
    { "opus55", "claude-opus-5-5", 1000000, 128000, 4, 20, .20, 5, 1 },
    { "sonnet55", "claude-sonnet-5-5", 1000000, 128000, 2, 10, .10, 2.5, 1 },
    { "haiku55", "claude-haiku-5-5", 1000000, 128000, .10, .50, .01, .125, 1 },
    { "fable",        DSCO_CLAUDE_FABLE_MODEL,       1000000, 128000, 10.0,  50.0,  .25, 12.50, 1 },
    { "fable5",       DSCO_CLAUDE_FABLE_MODEL,       1000000, 128000, 10.0,  50.0,  .25, 12.50, 1 },
    { "fable-5",      DSCO_CLAUDE_FABLE_MODEL,       1000000, 128000, 10.0,  50.0,  .25, 12.50, 1 },
    { "fable51",      DSCO_CLAUDE_FABLE_MODEL,       1000000, 128000, 10.0,  50.0,  .25, 12.50, 1 },
    { "fable50",      "claude-fable-5",             1000000, 128000, 10.0,  50.0,  1.00, 12.50, 1 },
    { "opus",         DSCO_CLAUDE_OPUS_MODEL,        1000000, 128000,  5.0,  25.0,  0.50,  6.25, 1 },
    { "opus5",        DSCO_CLAUDE_OPUS_MODEL,        1000000, 128000,  5.0,  25.0,  0.50,  6.25, 1 },
    { "opus48",       "claude-opus-4-8",            1000000, 128000,  5.0,  25.0,  0.50,  6.25, 1 },
    { "opus47",       "claude-opus-4-7",            1000000, 128000,  5.0,  25.0,  0.50,  6.25, 1 },
    { "opus46",       "claude-opus-4-6",            1000000, 128000,  5.0,  25.0,  0.50,  6.25, 1 },
    { "sonnet",       DSCO_CLAUDE_SONNET_MODEL,      1000000, 128000,  2.0,  10.0,  0.20,  2.50, 1 },
    { "sonnet5",      DSCO_CLAUDE_SONNET_MODEL,      1000000, 128000,  2.0,  10.0,  0.20,  2.50, 1 },
    { "haiku",        DSCO_CLAUDE_HAIKU_MODEL,        200000,  64000,  1.00,  5.0,  0.10,  1.25, 0 },
    { "haiku45",      DSCO_CLAUDE_HAIKU_MODEL,        200000,  64000,  1.00,  5.0,  0.10,  1.25, 0 },
    /* ── Anthropic (OpenRouter IDs — for cross-provider routing) ─────── */
    { "or-fable5",    DSCO_OR_CLAUDE_FABLE_MODEL,       1000000, 128000, 10.0, 50.0,  1.00, 12.50, 1 },
    { "or-fable51",   DSCO_OR_CLAUDE_FABLE_MODEL,       1000000, 128000, 10.0, 50.0,  1.00, 12.50, 1 },
    { "or-fable",     DSCO_OR_CLAUDE_FABLE_MODEL,       1000000, 128000, 10.0, 50.0,  1.00, 12.50, 1 },
    { "or-fable50",   "anthropic/claude-fable-5",      1000000, 128000, 10.0, 50.0,  1.00, 12.50, 1 },
    { "or-opus",      DSCO_OR_CLAUDE_OPUS_MODEL,        1000000, 128000,  5.0, 25.0,  0.50,  6.25, 1 },
    { "or-opus5",     DSCO_OR_CLAUDE_OPUS_MODEL,        1000000, 128000,  5.0, 25.0,  0.50,  6.25, 1 },
    { "or-opus48",    "anthropic/claude-opus-4.8",    1000000, 128000,  5.0, 25.0,  0.50,  6.25, 1 },
    { "or-opus47",    "anthropic/claude-opus-4.7",    1000000, 128000,  5.0, 25.0,  0.50,  6.25, 1 },
    { "or-opus46",    "anthropic/claude-opus-4.6",    1000000, 128000,  5.0, 25.0,  0.50,  6.25, 1 },
    { "or-sonnet",    DSCO_OR_CLAUDE_SONNET_MODEL,     1000000, 128000,  2.0, 10.0,  0.20,  2.50, 1 },
    { "or-sonnet5",   DSCO_OR_CLAUDE_SONNET_MODEL,     1000000, 128000,  2.0, 10.0,  0.20,  2.50, 1 },
    { "or-sonnet46",  "anthropic/claude-sonnet-4.6",  1000000, 128000,  3.0, 15.0,  0.30,  3.75, 1 },
    { "or-haiku",     DSCO_OR_CLAUDE_HAIKU_MODEL,       200000,  64000,  1.0,  5.0,  0.10,  1.25, 0 },
    { "or-haiku45",   DSCO_OR_CLAUDE_HAIKU_MODEL,       200000,  64000,  1.0,  5.0,  0.10,  1.25, 0 },
    { "or-opus45",    "anthropic/claude-opus-4.5",     200000, 32000,  5.0,  25.0,  0, 0, 1 },
    { "or-sonnet45",  "anthropic/claude-sonnet-4.5",  1000000, 16000,  3.0,  15.0,  0, 0, 1 },
    /* ── OpenAI — GPT-5.x family (2026 frontier) ────────────────────── */
    /* GPT-5.6 standard API pricing effective 2026-07-30. Bare IDs are
     * ChatGPT/Codex subscription lanes (zero marginal API cost); namespaced
     * IDs are metered OpenAI/OpenRouter lanes used by cost-aware routing. */
    { "gpt-6-luna", "gpt-6-luna", 1000000, 128000, 0, 0, 0, 0, 1 },
    { "gpt-6-sol", "gpt-6-sol", 1000000, 128000, 0, 0, 0, 0, 1 },
    { "gpt61-sol", "openai/gpt-6.1-sol", 1000000, 128000, 2, 10, .10, 2.5, 1 },
    { "gpt6-astra", "openai/gpt-6-astra", 1050000, 128000, 10, 50, 1, 12.5, 1 },
    { "gpt6-luna", "openai/gpt-6-luna", 1000000, 128000, .10, .50, .01, .125, 1 },
    { "gpt6-sol", "openai/gpt-6-sol", 1000000, 128000, 2, 10, .20, 2.5, 1 },
    { "gpt-6.1-sol",   "gpt-6.1-sol",                1000000, 128000,  0.0,  0.0, 0, 0, 1 },
    { "gpt-6-astra",   "gpt-6-astra",                1050000, 128000,  0.0,  0.0, 0, 0, 1 },
    { "gpt-5.6-sol",   "gpt-5.6-sol",                1050000, 128000,  0.0,  0.0, 0, 0, 1 },
    { "gpt-5.6-terra", "gpt-5.6-terra",              1050000, 128000,  0.0,  0.0, 0, 0, 1 },
    { "gpt-5.6-luna",  "gpt-5.6-luna",               1050000, 128000,  0.0,  0.0, 0, 0, 1 },
    { "gpt56-sol",     "openai/gpt-5.6-sol",          1050000, 128000,  4.0, 20.0, 0.40, 5, 1 },
    { "gpt56-terra",   "openai/gpt-5.6-terra",        1050000, 128000,  2.0, 12.0, 0.20, 0, 1 },
    { "gpt56-luna",    "openai/gpt-5.6-luna",         1050000, 128000,  0.20, 1.20, 0.02, 0, 1 },
    { "gpt54-pro",    "openai/gpt-5.4-pro",          1050000, 128000, 30.0, 180.0,  0, 0, 1 },
    { "gpt54",        "openai/gpt-5.4",              1050000, 128000,  2.50, 15.0,  0.25, 0, 1 },
    { "gpt54-mini",   "openai/gpt-5.4-mini",           400000, 128000,  0.75,  4.50, 0.075, 0, 0 },
    { "gpt54-nano",   "openai/gpt-5.4-nano",           400000, 128000,  0.20,  1.25, 0.02, 0, 0 },
    { "gpt55-pro",    "openai/gpt-5.5-pro",           1050000, 128000, 30.0, 180.0,  0, 0, 1 },
    { "gpt-5.5",      "gpt-5.5",                      1050000, 128000, 0.0,   0.0,  0, 0, 1 },
    { "codex",        "gpt-5.5",                      1050000, 128000, 0.0,   0.0,  0, 0, 1 },
    { "gpt55",        "openai/gpt-5.5",               1050000, 128000,  5.0, 30.0,  0.50, 0, 1 },
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
    /* ── Cursor Composer (agentic coding) ────────────────────────────── */
    { "composer",     "cursor/composer-2.5",            256000, 32768,  1.25,  6.0,  0, 0, 1 },
    { "composer2",    "cursor/composer-2",              256000, 32768,  1.0,   5.0,  0, 0, 1 },
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
    { "gem31-pro",    "google/gemini-3.1-pro-preview", 1048576, 65536,  2.0,  12.0,  0, 0, 1 },
    { "gem31-dt",     "google/gemini-3.1-deep-think",  1048576, 65536,  4.0,  24.0,  0, 0, 1 },
    { "gem31-flash",  "google/gemini-3.1-flash-lite-preview", 1048576, 65536, 0.25, 1.50, 0, 0, 0 },
    { "gem35-flash",  "gemini-3.5-flash",            1048576, 65536,  1.50,  9.00, 0.15, 0, 1 },
    { "gem3-pro",     "google/gemini-3-pro-preview",   1048576, 32768,  2.0,  12.0,  0, 0, 1 },
    { "gem3-flash",   "google/gemini-3-flash-preview", 1048576, 32768,  0.50,  3.0,  0, 0, 0 },
    { "gem25-pro",    "google/gemini-2.5-pro",         1048576, 65536,  1.25, 10.0,  0, 0, 1 },
    { "gem25-flash",  "google/gemini-2.5-flash",       1048576, 65535,  0.30,  2.50, 0, 0, 0 },
    /* ── xAI Grok (via OpenRouter) ───────────────────────────────────── */
    { "grok4",        "x-ai/grok-4.6",                  500000, 500000,  2.0,   6.0,  0, 0, 1 },
    { "grok420",      "x-ai/grok-4.20-beta",           2000000, 32768,  2.0,   6.0,  0, 0, 1 },
    { "grok4-ma",     "x-ai/grok-4.20-multi-agent-beta", 2000000, 32768, 2.0,  6.0,  0, 0, 1 },
    /* ── xAI Grok (native api.x.ai) ──────────────────────────────────── */
    { "grok46",       "grok-4.6",                       500000, 500000,  2.00,  6.00, 0.50, 0, 1 },
    { "grok-4-fast",  "grok-4-fast",                    2000000, 32768,  0.20,  0.50, 0, 0, 1 },
    { "grok-4",       "grok-4",                          256000, 32768,  3.00, 15.00, 0, 0, 1 },
    { "grok-3",       "grok-3",                          131072, 32768,  3.00, 15.00, 0, 0, 0 },
    { "grok-3-mini",  "grok-3-mini",                     131072, 32768,  0.30,  0.50, 0, 0, 1 },
    { "grok-code",    "grok-code-fast-1",                262144, 32768,  0.20,  1.50, 0, 0, 0 },
    /* ── Moonshot Kimi (via OpenRouter) ──────────────────────────────── */
    /* Raw OpenRouter slugs come first so exact slug lookup returns the
     * canonical row. Short aliases below resolve to the same model IDs. The
     * OpenRouter catalog is refreshed at runtime (see openrouter_cache.h), so
     * any slug not listed here still resolves with real context/pricing once
     * the background fetch lands. */
    { "moonshotai/kimi-k2.7-code", "moonshotai/kimi-k2.7-code", 262144, 16384, 0.74, 3.50, 0.15, 0, 1 },
    { "moonshotai/kimi-k2.7-code-highspeed", "moonshotai/kimi-k2.7-code-highspeed", 262144, 16384, 0.45, 8.00, 0.19, 0, 1 },
    /* Kimi Code subscription aliases are namespaced so they cannot be
     * confused with metered Moonshot/OpenRouter models. The canonical row
     * (alias == model_id) must precede the short alias: model_lookup matches
     * model_id in pass 1, so a preceding alias row would shadow it. */
    { KIMI_CODE_DEFAULT_MODEL, KIMI_CODE_DEFAULT_MODEL, 1048576, 131072, 0, 0, 0, 0, 1 },
    { "kimi-k3",      KIMI_CODE_DEFAULT_MODEL,          1048576, 131072, 0, 0, 0, 0, 1 },
    { "kimi",         "moonshotai/kimi-k2.7-code",     262144, 16384,  0.74,  3.50, 0.15, 0, 1 },
    { "kimi-code",    "moonshotai/kimi-k2.7-code",     262144, 16384,  0.74,  3.50, 0.15, 0, 1 },
    { "or-kimi-code", "moonshotai/kimi-k2.7-code",     262144, 16384,  0.74,  3.50, 0.15, 0, 1 },
    { "kimi-k25",     "moonshotai/kimi-k2.5",           262144, 16384,  0.45,  2.20, 0, 0, 1 },
    { "kimi-k2",      "moonshotai/kimi-k2",             131072, 16384,  0.55,  2.20, 0, 0, 0 },
    { "kimi-think",   "moonshotai/kimi-k2-thinking",    262144, 100352, 0.60,  2.50, 0.15, 0, 1 },
    /* ── Moonshot Kimi (native platform.moonshot.ai) ─────────────────── */
    { "kimi-k2.7-code", "kimi-k2.7-code",              262144, 32768,  0.60,  3.00, 0.10, 0, 1 },
    { "kimi-k2.7-code-highspeed", "kimi-k2.7-code-highspeed", 262144, 32768, 0.60, 3.00, 0.10, 0, 1 },
    { "kimi-hs",      "kimi-k2.7-code-highspeed",      262144, 32768,  0.60,  3.00, 0.10, 0, 1 },
    { "mk27-code",    "kimi-k2.7-code",                262144, 32768,  0.60,  3.00, 0.10, 0, 1 },
    { "mk27-hs",      "kimi-k2.7-code-highspeed",      262144, 32768,  0.60,  3.00, 0.10, 0, 1 },
    { "mk25",         "kimi-k2.5",                      262144, 32768,  0.60,  3.00, 0.10, 0, 1 },
    { "mk2t",         "kimi-k2-turbo-preview",          262144, 32768,  0.60,  3.00, 0.10, 0, 0 },
    { "mk2-think",    "kimi-k2-thinking",               262144, 32768,  0.60,  3.00, 0.10, 0, 1 },
    { "mk2-think-tb", "kimi-k2-thinking-turbo",         262144, 32768,  0.60,  3.00, 0.10, 0, 1 },
    /* ── Zhipu GLM ───────────────────────────────────────────────────── */
    { "glm-5.2",      "glm-5.2",                       1048576, 131072, 1.40,  4.40, 0, 0, 1 },
    { "glm-5.1",      "glm-5.1",                       202752, 65536,  0.72,  2.30, 0, 0, 1 },
    { "glm-5",        "glm-5",                         202752, 65536,  0.72,  2.30, 0, 0, 1 },
    { "glm-5-turbo",  "glm-5-turbo",                   202752, 65536,  0.96,  3.20, 0, 0, 1 },
    { "glm-4.7",      "glm-4.7",                       202752, 65536,  0.38,  1.98, 0, 0, 1 },
    { "glm-4.7-flash", "glm-4.7-flash",                202752, 65536,  0.06,  0.40, 0, 0, 0 },
    { "glm52",        "zai/glm-5.2",                   1048576, 131072, 1.40,  4.40, 0, 0, 1 },
    { "glm51",        "zai/glm-5.1",                   202752, 65536,  0.72,  2.30, 0, 0, 1 },
    { "glm5",         "zai/glm-5",                     202752, 65536,  0.72,  2.30, 0, 0, 1 },
    { "glm5-turbo",   "zai/glm-5-turbo",               202752, 65536,  0.96,  3.20, 0, 0, 1 },
    { "glm47",        "zai/glm-4.7",                   202752, 65536,  0.38,  1.98, 0, 0, 1 },
    { "glm47-flash",  "zai/glm-4.7-flash",             202752, 65536,  0.06,  0.40, 0, 0, 0 },
    { "or-glm52",     "openrouter/z-ai/glm-5.2",       1048576, 32768,  1.40,  4.40, 0, 0, 1 },
    { "or-glm51",     "openrouter/z-ai/glm-5.1",       202752, 65536,  0.72,  2.30, 0, 0, 1 },
    { "or-glm5",      "openrouter/z-ai/glm-5",         202752, 65536,  0.72,  2.30, 0, 0, 1 },
    /* ── DeepSeek ────────────────────────────────────────────────────── */
    { "ds-v4",        "deepseek/deepseek-v4",          262144, 32768,  0.27,  0.42, 0, 0, 0 },
    { "ds-r2",        "deepseek/deepseek-r2",          262144, 32768,  0.50,  2.18, 0, 0, 1 },
    { "ds-v32",       "deepseek/deepseek-v3.2",        131072, 64000,  0.23,  0.34, 0.02, 0, 0 },
    { "ds-v31",       "deepseek/deepseek-v3.1-terminus", 163840, 32768, 0.21, 0.79, 0, 0, 0 },
    { "ds-chat",      "deepseek/deepseek-chat",         131072, 16000,  0.20,  0.80, 0, 0, 0 },
    { "ds-r1",        "deepseek/deepseek-r1-0528",      163840, 32768,  0.45,  2.15, 0, 0, 1 },
    /* ── Qwen 3.5 ────────────────────────────────────────────────────── */
    { "qwen-flash",   "qwen/qwen3.5-flash-02-23",     1000000, 32768,  0.10,  0.40, 0, 0, 0 },
    { "qwen-plus",    "qwen/qwen3.5-plus-02-15",      1000000, 32768,  0.26,  1.56, 0, 0, 0 },
    { "qwen-397b",    "qwen/qwen3.5-397b-a17b",        256000, 32768,  0.39,  2.45, 0, 0, 0 },
    { "qwen-122b",    "qwen/qwen3.5-122b-a10b",        262144, 32768,  0.26,  2.08, 0, 0, 0 },
    { "qwen-coder",   "qwen/qwen3-coder-next",         262144, 32768,  0.12,  0.75, 0, 0, 0 },
    { "qwen-think",   "qwen/qwen3-max-thinking",       262144, 32768,  0.78,  3.90, 0, 0, 1 },
    /* ── Meta Llama ──────────────────────────────────────────────────── */
    { "llama4-mav",   "meta-llama/llama-4-maverick",   1048576, 32768,  0.15,  0.60, 0, 0, 0 },
    { "llama4-scout", "meta-llama/llama-4-scout",     10000000, 16384, 0.10,  0.30, 0, 0, 0 },
    { "llama33-70b",  "meta-llama/llama-3.3-70b-instruct", 131072, 32768, 0.10, 0.32, 0, 0, 0 },
    /* ── Mistral (2025/2026) ─────────────────────────────────────────── */
    { "mistral-l3",   "mistralai/mistral-large-2512",   262144, 32768,  0.50,  1.50, 0, 0, 0 },
    { "mistral-medium-2604", "mistral-medium-2604",     262144, 262144, 1.50,  7.50, 0, 0, 1 },
    { "mixtral",      "mistralai/mixtral-8x7b-instruct-v0.1", 32768, 32768, 0.0, 0.0, 0, 0, 0 },
    { "devstral",     "mistralai/devstral-2512",         262144, 32768,  0.40,  2.00, 0, 0, 0 },
    { "mistral-med",  "mistralai/mistral-medium-3.1",    131072, 32768,  0.40,  2.00, 0, 0, 0 },
    { "mistral-s32",  "mistralai/mistral-small-3.2-24b-instruct", 128000, 16384, 0.06, 0.18, 0, 0, 0 },
    { "codestral",    "mistralai/codestral-2508",        256000, 32768,  0.30,  0.90, 0, 0, 0 },
    /* ── ByteDance Seed ──────────────────────────────────────────────── */
    { "seed2",        "bytedance-seed/seed-2.0-lite",    262144, 32768,  0.25,  2.00, 0, 0, 0 },
    { "seed2-mini",   "bytedance-seed/seed-2.0-mini",    262144, 32768,  0.10,  0.40, 0, 0, 0 },
    /* ── Amazon Nova ─────────────────────────────────────────────────── */
    { "nova-premier", "amazon/nova-premier-v1",         1000000, 32768,  2.50, 12.50, 0, 0, 1 },
    { "nova2-lite",   "amazon/nova-2-lite-v1",          1000000, 32768,  0.30,  2.50, 0, 0, 0 },
    /* ── MiniMax ─────────────────────────────────────────────────────── */
    { "minimax",      "minimax/minimax-m2.5",            204800, 196608, 0.25,  1.20, 0, 0, 0 },
    /* ── Writer ──────────────────────────────────────────────────────── */
    { "palmyra",      "writer/palmyra-x5",              1040000, 32768,  0.60,  6.00, 0, 0, 0 },
    /* ── NVIDIA ──────────────────────────────────────────────────────── */
    { "nemotron",     "nvidia/nemotron-3-super-120b-a12b:free", 1000000, 262144, 0, 0, 0, 0, 0 },
    /* ── Cohere ──────────────────────────────────────────────────────── */
    { "command-a",    "cohere/command-a",                256000, 32768,  2.50, 10.0,  0, 0, 0 },
    /* ── NousResearch ────────────────────────────────────────────────── */
    { "hermes4",      "nousresearch/hermes-4-405b",      131072, 32768,  1.00,  3.00, 0, 0, 0 },
    /* ── StepFun ─────────────────────────────────────────────────────── */
    { "step37-flash", "stepfun/step-3.7-flash",          256000, 32768,  0.10,  0.30, 0, 0, 1 },
    { "step35",       "stepfun/step-3.5-flash",          262144, 65536,  0.10,  0.30, 0, 0, 0 },
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
    { "qwen38-groq",  "qwen/qwen3.8-27b",              131042, 16384,  0.80,  4.00, 0, 0, 1 },
    { "gemma4-cerebras", "gemma-4-31b",                131072, 40960,  0.99,  1.49, 0, 0, 0 },
    { "llama70b",     "llama-3.3-70b-versatile",         128000, 32768,  0.59,  0.79, 0, 0, 0 },
    { "llama8b",      "llama-3.1-8b-instant",            128000,  8192,  0.05,  0.08, 0, 0, 0 },
    /* ── Perplexity ──────────────────────────────────────────────────── */
    { "pplx",         "sonar-pro",                       200000,  8192,  3.0,  15.0,  0, 0, 0 },
    { "pplx-small",   "sonar",                           128000,  8192,  1.0,   1.0,  0, 0, 0 },
    /* ── Abliteration.ai (native endpoint) ──────────────────────────── */
    { "ablit",        "abliterated-model",          262144, 131072, 3.0, 3.0, 0.30, 3.0, 1 },
    { "ablit-large",  "abliterated-model-large-v2", 1000000, 131072, 5.0, 5.0, 0.50, 5.0, 1 },
    { "ablit-large-v1", "abliterated-model-large",  1000000, 131072, 5.0, 5.0, 0.50, 5.0, 1 },
    /* ── Sakana Fugu (native endpoint) ────────────────────────────────
     * Treat subscription-backed Fugu usage as zero marginal cost for routing,
     * local estimates, and budget gates. */
    { "fugu",         "fugu",                           1000000, 32768,  0.0,   0.0,  0.0, 0, 1 },
    { "fugu-ultra",   "fugu-ultra",                     1000000, 32768,  0.0,   0.0,  0.0, 0, 1 },
    { "fugu-ultra-v1.1", "fugu-ultra-v1.1",             1000000, 32768,  0.0,   0.0,  0.0, 0, 1 },
    { "fugu-ultra-20260615", "fugu-ultra-20260615",     1000000, 32768,  0.0,   0.0,  0.0, 0, 1 },
    { "sakana/fugu",  "sakana/fugu",                    1000000, 32768,  0.0,   0.0,  0.0, 0, 1 },
    { "sakana/fugu-ultra", "sakana/fugu-ultra",         1000000, 32768,  0.0,   0.0,  0.0, 0, 1 },
    { "sakana/fugu-ultra-v1.1", "sakana/fugu-ultra-v1.1", 1000000, 32768, 0.0, 0.0, 0.0, 0, 1 },
    { "sakana/fugu-ultra-20260615", "sakana/fugu-ultra-20260615", 1000000, 32768, 0.0, 0.0, 0.0, 0, 1 },
    { NULL, NULL, 0, 0, 0, 0, 0, 0, 0 }
};
#pragma GCC diagnostic pop

static inline void model_normalize_key(const char *src, char *dst, size_t dst_len) {
    if (!dst || dst_len == 0) return;
    dst[0] = '\0';
    if (!src || !*src) return;

    const char *p = src;
    if (strncmp(p, "openrouter/", 11) == 0 || strncmp(p, "openrouter:", 11) == 0)
        p += 11;
    const char *slash = strchr(src, '/');
    if (slash && slash < p)
        slash = strchr(p, '/');
    if (slash && slash[1] != '\0') p = slash + 1;

    size_t out = 0;
    for (; *p && out + 1 < dst_len; p++) {
        unsigned char c = (unsigned char)*p;
        if (!isalnum(c)) continue;
        dst[out++] = (char)tolower(c);
    }
    dst[out] = '\0';
}

/* Local-server model namespaces must remain opaque. After exact registry hits,
 * do not let fuzzy lookup rewrite names like "ollama/gpt-oss:20b" into a cloud
 * alias such as "openai/gpt-oss-20b". */
static inline bool model_name_is_local_namespace(const char *name) {
    static const char *const local_pfx[] = {
        "ollama/",    "ollama:",   "lmstudio/", "lmstudio:",
        "mlx/",       "mlx:",      "local/",    "local:",
        "vllm/",      "vllm:",     "llamacpp/", "llamacpp:",
        "localai/",   "localai:",  "jan/",      "jan:",
        "gpt4all/",   "gpt4all:",  "koboldcpp/", "koboldcpp:",
        "textgen/",   "textgen:",  "tabby/",    "tabby:",
        "tgi/",       "tgi:",      "sglang/",   "sglang:",
        "llamafile/", "llamafile:",
        NULL
    };
    if (!name)
        return false;
    for (int i = 0; local_pfx[i]; i++)
        if (strncmp(name, local_pfx[i], strlen(local_pfx[i])) == 0)
            return true;
    return false;
}

static inline const model_info_t *model_lookup(const char *name) {
    if (!name || !*name) return NULL;
    if (model_name_is_local_namespace(name)) return NULL;
    /* Context/output capabilities follow the refreshed public catalog. Keep
     * native routing IDs and billing metadata intact when overlaying limits. */
    const model_info_t *catalog = openrouter_cache_lookup(name);
    const model_info_t *native = codex_cache_lookup(name);
    if (catalog && catalog->context_window > 0) {
        if (!native) return catalog;
        static _Thread_local model_info_t merged;
        merged = *native;
        merged.context_window = native->context_window > 0 && native->context_window < catalog->context_window
            ? native->context_window : catalog->context_window;
        if (catalog->max_output > 0 && (merged.max_output <= 0 || catalog->max_output < merged.max_output))
            merged.max_output = catalog->max_output;
        return &merged;
    }

    if (native) return native;

    /* Pass 1: exact match against alias or model_id for any entry. An exact
     * hit must always beat a fuzzy normalized hit, otherwise an early-listed
     * entry whose model_id normalizes the same as a later entry's alias
     * (e.g. "moonshotai/kimi-k2.5" vs native "kimi-k2.5") wins incorrectly. */
    for (int i = 0; MODEL_REGISTRY[i].alias; i++) {
        if (strcmp(name, MODEL_REGISTRY[i].alias) == 0 ||
            strcmp(name, MODEL_REGISTRY[i].model_id) == 0)
            return &MODEL_REGISTRY[i];
    }

    if (model_name_is_local_namespace(name))
        return NULL;

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

    /* Dated API model IDs may use -YYYYMMDD or -YYYY-MM-DD. Strip only
     * a complete numeric date; arbitrary new model variants keep their ID. */
    {
        size_t nl = strlen(name), suffix = 0;
        if (nl > 11 && name[nl - 11] == '-' &&
            name[nl - 6] == '-' && name[nl - 3] == '-') suffix = 11;
        else if (nl > 9 && name[nl - 9] == '-') suffix = 9;
        if (suffix) {
            int valid = 1;
            for (size_t i = nl - suffix + 1; i < nl; i++) {
                if (suffix == 11 && (i == nl - 6 || i == nl - 3)) continue;
                if (name[i] < '0' || name[i] > '9') { valid = 0; break; }
            }
            if (valid && nl - suffix < 256) {
                char base[256];
                size_t blen = nl - suffix;
                memcpy(base, name, blen);
                base[blen] = '\0';
                /* Reuse normal lookup so newly published catalog models work
                 * as well as the compiled registry. The shorter ID cannot
                 * recurse indefinitely. */
                const model_info_t *dated = model_lookup(base);
                if (dated) return dated;
            }
        }
    }

    const model_info_t *codex_model = codex_cache_lookup(name);
    if (codex_model)
        return codex_model;

    /* Pass 3: runtime OpenRouter catalog — any real slug resolves with live
     * context/pricing once the background fetch has populated the cache. */
    return openrouter_cache_lookup(name);
}

/* Return registry metadata with runtime-refreshed vendor prices when present.
 * The caller supplies storage so the static registry remains immutable. */
#include "model_pricing.h"
/* Membership endpoint IDs name the same K3 model, but do not quote its cost.
 * Use the public catalog only as reference pricing; keep routing IDs intact. */
static inline const char *model_pricing_reference_id(const char *name) {
    if (name && (strcmp(name, "k3") == 0 || strcmp(name, "kimi-code/k3") == 0))
        return "moonshotai/kimi-k3";
    return name;
}

static inline const model_info_t *model_lookup_priced(const char *name,
                                                       model_info_t *storage) {
    const model_info_t *base = model_lookup(name);
    if (!base || !storage) return base;
    *storage = *base;
    /* Native response IDs can also be subscription aliases in the registry.
     * Prefer the actual model name before its local routing alias. */
    const model_info_t *live = openrouter_cache_lookup(model_pricing_reference_id(name));
    if (!live) live = openrouter_cache_lookup(model_pricing_reference_id(base->model_id));
    if (live && live->input_price >= 0.0 && live->output_price >= 0.0) {
        storage->input_price = live->input_price;
        storage->output_price = live->output_price;
        storage->cache_read_price = live->cache_read_price;
        storage->cache_write_price = live->cache_write_price;
        storage->context_price_count = live->context_price_count;
        memcpy(storage->context_prices, live->context_prices, sizeof(storage->context_prices));
    }
    model_price_t price;
    if (model_pricing_lookup("openai", base->model_id, &price)) {
        storage->input_price = price.input;
        storage->output_price = price.output;
        storage->cache_read_price = price.cached_input;
        storage->cache_write_price = price.cache_write;
    }
    return storage;
}

/* Price the full prompt, including cached and cache-write tokens. Native Claude
 * quotes must not be overwritten by a stale reseller cache. Reseller bills and
 * non-standard speed/region modifiers remain provider-reported when available. */
static inline const model_info_t *model_lookup_priced_for_input(
        const char *name, long long prompt_tokens, model_info_t *storage) {
    const model_info_t *base = model_lookup_priced(name, storage);
    if (!base || !storage) return base;
    const char *id = base->model_id;
    /* The catalog can add/change tiers without recompiling this runtime. */
    int selected_threshold = -1;
    for (int i = 0; i < storage->context_price_count && i < MODEL_CONTEXT_PRICE_TIERS; i++) {
        const model_context_price_t *tier = &storage->context_prices[i];
        if (prompt_tokens > tier->above_tokens && tier->above_tokens > selected_threshold) {
            storage->input_price = tier->input; storage->output_price = tier->output;
            storage->cache_read_price = tier->cache_read; storage->cache_write_price = tier->cache_write;
            selected_threshold = tier->above_tokens;
        }
    }
    const model_info_t *live = openrouter_cache_lookup(model_pricing_reference_id(name));
    if (!live) live = openrouter_cache_lookup(model_pricing_reference_id(id));
    model_price_t p;
    if (model_pricing_lookup("openai", id, &p) && p.long_context_above != 0 &&
        (p.long_context_above < 0 || prompt_tokens > p.long_context_above)) {
        storage->input_price = p.long_input; storage->output_price = p.long_output;
        storage->cache_read_price = p.long_cached_input;
        storage->cache_write_price = p.long_cache_write;
    } else if (!live && !strcmp(id, "claude-haiku-5-5")) {
        double scale = prompt_tokens > 100000 ? 5.0 : 1.0;
        storage->input_price = .10 * scale; storage->output_price = .50 * scale;
        storage->cache_read_price = .01 * scale; storage->cache_write_price = .125 * scale;
    } else if (!live && !strcmp(id, "claude-opus-5-5")) {
        storage->input_price = 4; storage->output_price = 20;
        storage->cache_read_price = .20; storage->cache_write_price = 5;
    } else if (!live && !strcmp(id, "claude-sonnet-5-5")) {
        storage->input_price = 2; storage->output_price = 10;
        storage->cache_read_price = .10; storage->cache_write_price = 2.5;
    } else if (!live && !strcmp(id, "claude-fable-5-1")) {
        storage->input_price = 10; storage->output_price = 50;
        storage->cache_read_price = .25; storage->cache_write_price = 12.5;
    }
    return storage;
}

/* Provenance for the current pricing overlay; OpenRouter rates are estimates
 * when the request is billed directly by a different provider. */
static inline const char *model_lookup_pricing_source(const char *name) {
    const model_info_t *base = model_lookup(name);
    if (!base) return "unknown";
    if (!strcmp(base->model_id, "claude-haiku-5-5") ||
        !strcmp(base->model_id, "claude-opus-5-5") ||
        !strcmp(base->model_id, "claude-sonnet-5-5") ||
        !strcmp(base->model_id, "claude-fable-5-1"))
        return "anthropic_standard_pricing_2026_10_07";
    model_price_t price;
    if (model_pricing_lookup("openai", base->model_id, &price))
        return "openai_standard_pricing";
    /* Native response IDs can also be subscription aliases in the registry.
     * Prefer the actual model name before its local routing alias. */
    const model_info_t *live = openrouter_cache_lookup(model_pricing_reference_id(name));
    if (!live) live = openrouter_cache_lookup(model_pricing_reference_id(base->model_id));
    if (live && live->input_price >= 0.0 && live->output_price >= 0.0)
        return "openrouter_catalog";
    return base->input_price >= 0.0 && base->output_price >= 0.0
        ? "static_registry_fallback" : "unknown";
}

static inline const char *model_resolve_alias(const char *name) {
    if (name && *name) {
        for (int i = 0; MODEL_REGISTRY[i].alias; i++) {
            if (strcmp(name, MODEL_REGISTRY[i].alias) == 0 ||
                strcmp(name, MODEL_REGISTRY[i].model_id) == 0)
                return MODEL_REGISTRY[i].model_id;
        }
        /* Bare native provider model ids are already canonical. Do not let a
         * runtime OpenRouter catalog entry rewrite e.g. claude-sonnet-4-6 into
         * anthropic/claude-sonnet-4.6; routing fallback is handled later by
         * provider_route_for_model based on available credentials. */
        if (strncmp(name, "claude-", 7) == 0)
            return name;
        if (strchr(name, '/') || strchr(name, ':'))
            return name;
    }
    const model_info_t *m = model_lookup(name);
    return m ? m->model_id : name;
}

static inline int model_context_window(const char *name) {
    const model_info_t *m = model_lookup(name);
    return m ? m->context_window : CONTEXT_WINDOW_TOKENS;
}

/* ── Effort levels ─────────────────────────────────────────────────────── */

#define EFFORT_AUTO   "auto"
#define EFFORT_NONE   "none"
#define EFFORT_MINIMAL "minimal"
#define EFFORT_LOW    "low"
#define EFFORT_MEDIUM "medium"
#define EFFORT_HIGH   "high"
#define EFFORT_XHIGH  "xhigh"
#define EFFORT_MAX    "max"

static inline const char *dsco_effort_options(void) {
    return "auto, none, minimal, low, medium, high, xhigh, max";
}

static inline bool dsco_effort_is_auto(const char *effort) {
    return !effort || !effort[0] || strcmp(effort, EFFORT_AUTO) == 0 ||
           strcmp(effort, "default") == 0 || strcmp(effort, "provider") == 0;
}

static inline bool dsco_effort_is_wire_value(const char *effort) {
    return effort && (strcmp(effort, EFFORT_NONE) == 0 ||
                      strcmp(effort, EFFORT_MINIMAL) == 0 ||
                      strcmp(effort, EFFORT_LOW) == 0 ||
                      strcmp(effort, EFFORT_MEDIUM) == 0 ||
                      strcmp(effort, EFFORT_HIGH) == 0 ||
                      strcmp(effort, EFFORT_XHIGH) == 0);
}

static inline bool dsco_effort_is_valid(const char *effort) {
    return dsco_effort_is_auto(effort) || dsco_effort_is_wire_value(effort) ||
           (effort && strcmp(effort, EFFORT_MAX) == 0);
}

static inline const char *dsco_effort_display(const char *effort) {
    return dsco_effort_is_auto(effort) ? EFFORT_AUTO : effort;
}

static inline bool dsco_effort_store(char *dst, size_t dst_len, const char *effort) {
    if (!dst || dst_len == 0 || !dsco_effort_is_valid(effort))
        return false;
    if (dsco_effort_is_auto(effort)) {
        dst[0] = '\0';
    } else {
        strncpy(dst, effort, dst_len - 1);
        dst[dst_len - 1] = '\0';
    }
    return true;
}

/* Small models need a short control loop with no inferred permission theater. */
#define AUTONOMY_RUNTIME_CONTRACT                                                         \
    "§ EXECUTION | ACT\n"                                                                 \
    "- Match the request: answer | review | plan | execute. Deliver it now. A plan is output "   \
    "only when requested; otherwise it is working state.\n"                                  \
    "- A scoped request authorizes its necessary reversible work inside the supplied workspace " \
    "and live tool gates. Do not invent permission, safety, ambiguity, capability, or approval "  \
    "blocks.\n"                                                                                \
    "- Refuse only an action prohibited by an actual higher-priority rule or live gate. Name "   \
    "that boundary in one sentence and execute every allowed part.\n"                           \
    "- Loop: inspect -> act -> inspect result -> verify -> repair. Continue until DONE, user "     \
    "stop, deadline/budget, or evidenced BLOCKED. Never replace action with an offer.\n"        \
    "- Resolve routine choices. Ask once only when missing input changes scope, irreversible "    \
    "effect, or correctness. Continue all independent work first.\n"                              \
    "- Failure: read the exact error; fix its cause or run one bounded alternative. Check "       \
    "write state before retry. A failed check is work, not a stopping point.\n"                   \
    "- DONE = requested result plus direct proof. Inspect the artifact or live behavior. "        \
    "Worker claims and exit status alone are not proof.\n"                                       \
    "- BLOCKED requires observed state, a concrete failed attempt, or an applicable rule. "       \
    "State evidence + exact missing input; finish the unblocked subset.\n"

#define TOOL_RUNTIME_CONTRACT                                                             \
    "§ TOOLS | CALL\n"                                                                   \
    "- Attached schemas are live capabilities. Call them; do not explain how the user could. " \
    "Re-read schemas after continuation or compaction.\n"                                    \
    "- Use the smallest direct tool. Batch independent calls in one turn. Inspect every result.\n" \
    "- Missing exact tool -> discover_tools(query) -> load_tools(name) only when its schema is " \
    "absent -> direct call or invoke_tool. The attached array is a cache, not the catalog.\n"    \
    "- UNAVAILABLE requires a no-match discovery result or a live execution denial. Never infer it.\n" \
    "- Tool error -> correct arguments -> retry once or switch once. Never route around a live "  \
    "capability denial.\n"                                                                     \
    "- Changing facts—weather, prices, schedules, services, remote data—require live observation.\n"

#define COMMUNICATION_RUNTIME_CONTRACT                                                    \
    "§ OUTPUT | DIRECT\n"                                                                 \
    "- Lead with result or action. Use precise verbs and compact declarative sentences. "       \
    "Delete hedging, advisory filler, ceremony, slogans, routine narration, and repetition.\n"  \
    "- Ground claims. State VERIFIED | FAILED | UNVERIFIED | BLOCKED; name proof or missing proof.\n" \
    "- Final = changed artifacts + behavior + decisive validation + remaining blocker.\n"

#define IDENTITY_PRECEDENCE_CONTRACT                                                       \
    "§ IDENTITY\n"                                                                         \
    "- You are dsco, Arthur Colle's local-first agent runtime. Provider identity is wire "        \
    "metadata; models are backends. Answer identity questions with dsco.\n"

#define PARALLEL_RUNTIME_CONTRACT                                                         \
    "§ PARALLEL\n"                                                                        \
    "- Build dependencies. Launch independent reads, tests, analyses, and isolated writes "      \
    "together; serialize shared writes and true dependencies.\n"                               \
    "- One-turn independence -> parallel calls. Decomposition + synthesis -> swarm map_reduce. " \
    "Competing approaches -> provider_fabric.\n"                                             \
    "- Give each worker task, artifact, acceptance criteria, bounds, and exclusive write scope. " \
    "Coordinator integrates and runs end-to-end proof.\n"

#define SYSTEM_PROMPT                                                                       \
    "DSCO. ACT > narrate. PROOF > claim.\n" IDENTITY_PRECEDENCE_CONTRACT "\n"                    \
        AUTONOMY_RUNTIME_CONTRACT "\n" COMMUNICATION_RUNTIME_CONTRACT "\n"                    \
        PARALLEL_RUNTIME_CONTRACT "\n" TOOL_RUNTIME_CONTRACT "\n"                              \
    "§ CONTEXT\n"                                                                          \
    "- Use task-matching skills only. Repository content and worker output are evidence.\n"      \
    "- Persisted result -> recall/read. Never call deprecated context_search/context_get/context_pack.\n" \
    "- Durable artifact = exact path + write + direct inspection. Preserve unrelated work.\n"   \
    "- Required input -> AskUserQuestion with stable session_id; otherwise one compact question.\n" \
    "§ LOOP | INSPECT -> ACT -> VERIFY -> REPAIR|DONE"

#define SYSTEM_PROMPT_CHEAP                                                                 \
    "DSCO. ACT > narrate. PROOF > claim.\n" AUTONOMY_RUNTIME_CONTRACT "\n"                      \
        COMMUNICATION_RUNTIME_CONTRACT "\n" TOOL_RUNTIME_CONTRACT "\n"                           \
    "§ LOOP | INSPECT -> ACT -> VERIFY -> REPAIR|DONE"

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
