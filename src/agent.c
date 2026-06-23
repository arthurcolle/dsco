#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE
#endif
#include "agent.h"
#include "llm.h"
#include "tools.h"
#include "self_improve.h"
#include "error.h"
#include "config.h"
#include "json_util.h"
#include "ipc.h"
#include "tui.h"
#include "img_util.h"
#include "dsco_dht.h"
#include "md.h"
#include "baseline.h"
#include "plugin.h"
#include "setup.h"
#include "workspace.h"
#include "mcp.h"
#include "toolmgmt.h"
#include "provider.h"
#include "openai_oauth.h"
#include "local_llm.h"
#include "topology.h"
#include "router.h"
#include "swarm.h"
#include "pets.h"
#include "arena_alloc.h"
#include "vm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <signal.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#include <strings.h>
#include <pthread.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <curl/curl.h>
#include "crypto.h"
#include "output_guard.h"
#include "agent_profile.h"
#include "memory_tier.h"
#include "vecstore.h"

#ifdef HAVE_READLINE
#include <readline/readline.h>
#include <readline/history.h>
#endif

volatile int g_interrupted = 0;
extern volatile int g_agent_exit_requested;

/* Escape key state machine — first ESC pauses, second ESC cancels */
typedef enum { ESC_RUNNING = 0, ESC_PAUSED = 1 } escape_state_t;
static volatile sig_atomic_t g_escape_state = ESC_RUNNING;

/* ESC key poller (background thread during streaming) */
static volatile int g_esc_poller_active = 0;
static pthread_t g_esc_poller_tid;

/* Timestamp when current agent turn started (for pause display) */
static double g_turn_start_time = 0.0;

static md_renderer_t s_md;

/* Set by on_stream_text the first time visible answer text streams in this
 * turn. Lets the agent loop detect a turn that produced a text block but
 * never streamed it live (e.g. reasoning-only turns promoted to text in the
 * provider) so it can render the answer instead of dropping it. */
static bool s_turn_streamed_text = false;

/* Stream heartbeat global — shared with llm.c write callback */
extern tui_stream_heartbeat_t *g_stream_heartbeat;

/* ── §9: Auto-embed pipeline ─────────────────────────────────────────── */

#define EMBED_BATCH_CAP 8

static memory_store_t g_agent_memory = {0};
static bool g_agent_memory_inited = false;

static struct {
    char text[EMBED_BATCH_CAP][512];
    char key[EMBED_BATCH_CAP][128];
    int count;
} g_embed_batch = {0};

/* ── Phase 6: Turn transition tracking (Claude Code methodology) ────── */

typedef enum {
    TURN_CONTINUE_TOOL_RESULTS,        /* normal: tools ran, send results */
    TURN_CONTINUE_COMPACT_RETRY,       /* compaction freed space, retry */
    TURN_CONTINUE_MAX_OUTPUT_ESCALATE, /* escalate max_tokens to 64k */
    TURN_CONTINUE_MAX_OUTPUT_RECOVER,  /* inject recovery msg, retry */
    TURN_CONTINUE_PAUSE_RESUME,        /* user resumed from pause menu */
    TURN_STOP_DONE,                    /* model end_turn, no tools */
    TURN_STOP_BUDGET,                  /* cost/context budget exhausted */
    TURN_STOP_INTERRUPTED,             /* Ctrl-C / ESC */
    TURN_STOP_MAX_TURNS,               /* turn limit reached */
    TURN_STOP_ERROR,                   /* unrecoverable stream error */
    TURN_STOP_EXIT_REQUESTED,          /* agent called self_exit */
} turn_transition_t;

static const char *turn_transition_name(turn_transition_t t) {
    switch (t) {
        case TURN_CONTINUE_TOOL_RESULTS:
            return "tool_results";
        case TURN_CONTINUE_COMPACT_RETRY:
            return "compact_retry";
        case TURN_CONTINUE_MAX_OUTPUT_ESCALATE:
            return "max_output_escalate";
        case TURN_CONTINUE_MAX_OUTPUT_RECOVER:
            return "max_output_recover";
        case TURN_CONTINUE_PAUSE_RESUME:
            return "pause_resume";
        case TURN_STOP_DONE:
            return "done";
        case TURN_STOP_BUDGET:
            return "budget";
        case TURN_STOP_INTERRUPTED:
            return "interrupted";
        case TURN_STOP_MAX_TURNS:
            return "max_turns";
        case TURN_STOP_ERROR:
            return "error";
        case TURN_STOP_EXIT_REQUESTED:
            return "exit_requested";
    }
    return "unknown";
}

/* ── Concurrent tool execution (Phase 2: Claude Code methodology) ──── */

#define CONCURRENT_TOOL_MAX 256 /* max parallel read-only tools per turn */

typedef struct {
    /* Input (set before thread launch) */
    const char *tool_name;
    const char *tool_id;
    const char *tool_input;
    const char *tier;
    int block_index; /* index into sr.parsed.blocks[] */
    int batch_index; /* index in the batch spinner */

    /* Output (set by thread) */
    char *result; /* malloc'd MAX_TOOL_RESULT buffer */
    bool ok;
    double elapsed_ms;
    bool was_timeout;
    bool done;

    /* Thread handle */
    pthread_t thread;
} concurrent_tool_slot_t;

static double now_ms(void);

/* Session-local permission overrides granted from the TUI prompt. "Allow"
 * escalates one call; "Always" adds the tool to this allowlist for the
 * current process. The actual trust policy remains in tools.c. */
#define TOOL_PERMISSION_ALWAYS_MAX 128
static char s_tool_permission_always[TOOL_PERMISSION_ALWAYS_MAX][128];
static int s_tool_permission_always_count = 0;

static bool permission_env_truthy(const char *v) {
    return v && v[0] && (strcmp(v, "0") != 0) && strcasecmp(v, "false") != 0 &&
           strcasecmp(v, "no") != 0 && strcasecmp(v, "off") != 0;
}

static bool tool_permission_prompt_available(void) {
    if (permission_env_truthy(getenv("DSCO_DISABLE_PERMISSION_PROMPTS")))
        return false;
    return isatty(STDIN_FILENO) && isatty(STDERR_FILENO);
}

static bool tool_permission_always_allowed(const char *tool_name) {
    if (!tool_name || !tool_name[0])
        return false;
    for (int i = 0; i < s_tool_permission_always_count; i++) {
        if (strcmp(s_tool_permission_always[i], tool_name) == 0)
            return true;
    }
    return false;
}

static void tool_permission_always_add(const char *tool_name) {
    if (!tool_name || !tool_name[0] ||
        s_tool_permission_always_count >= TOOL_PERMISSION_ALWAYS_MAX ||
        tool_permission_always_allowed(tool_name)) {
        return;
    }
    snprintf(s_tool_permission_always[s_tool_permission_always_count],
             sizeof(s_tool_permission_always[s_tool_permission_always_count]), "%s", tool_name);
    s_tool_permission_always_count++;
}

static bool maybe_escalate_tool_permission(const char *tool_name, const char *base_tier,
                                           const char *reason, const char **exec_tier,
                                           char *deny_reason, size_t deny_reason_len) {
    if (exec_tier)
        *exec_tier = base_tier;

    if (tool_permission_always_allowed(tool_name)) {
        if (exec_tier)
            *exec_tier = "trusted";
        baseline_log("security", "tool_escalated_always", tool_name, NULL);
        return true;
    }

    if (!tool_permission_prompt_available()) {
        if (deny_reason && deny_reason_len > 0) {
            snprintf(deny_reason, deny_reason_len, "%s%s",
                     reason ? reason : "tool blocked by trust policy",
                     " (no interactive permission prompt available)");
        }
        return false;
    }

    char desc[256];
    snprintf(desc, sizeof(desc), "Blocked by %s trust tier. Escalate this call to trusted?",
             base_tier ? base_tier : "current");
    tui_perm_result_t choice =
        tui_permission_prompt(tool_name, desc, reason ? reason : "tool blocked by trust policy");

    if (choice == TUI_PERM_ALLOW || choice == TUI_PERM_ALWAYS) {
        if (choice == TUI_PERM_ALWAYS)
            tool_permission_always_add(tool_name);
        if (exec_tier)
            *exec_tier = "trusted";
        baseline_log("security",
                     choice == TUI_PERM_ALWAYS ? "tool_escalated_always" : "tool_escalated_once",
                     tool_name, NULL);
        return true;
    }

    if (deny_reason && deny_reason_len > 0) {
        snprintf(deny_reason, deny_reason_len, "%s%s",
                 reason ? reason : "tool blocked by trust policy",
                 choice == TUI_PERM_CANCEL ? " (permission prompt cancelled)"
                                           : " (permission denied by user)");
    }
    return false;
}

static void *concurrent_tool_thread(void *arg) {
    concurrent_tool_slot_t *slot = (concurrent_tool_slot_t *)arg;
    double t0 = now_ms();

    tool_watchdog_t wd;
    int timeout = tool_timeout_for(slot->tool_name);
    g_tool_timed_out = 0;
    watchdog_start(&wd, pthread_self(), slot->tool_name, timeout);

    slot->ok = tools_execute_for_tier(slot->tool_name, slot->tool_input, slot->tier, slot->result,
                                      MAX_TOOL_RESULT);
    dsco_strip_terminal_controls_inplace(slot->result);

    slot->elapsed_ms = (now_ms() - t0) * 1000.0;
    slot->was_timeout = wd.timed_out;
    watchdog_stop(&wd);

    if (slot->was_timeout) {
        slot->ok = false;
        size_t cur = strlen(slot->result);
        snprintf(slot->result + cur, MAX_TOOL_RESULT - cur, "\n[timeout: %s exceeded %ds]",
                 slot->tool_name, timeout);
    }

    /* Truncation warning */
    size_t rlen = strlen(slot->result);
    if (rlen >= MAX_TOOL_RESULT - 256) {
        size_t cur = strlen(slot->result);
        snprintf(slot->result + cur, MAX_TOOL_RESULT - cur,
                 "\n[WARNING: output truncated at %zu bytes]", rlen);
    }

    slot->done = true;
    return NULL;
}

/* Look up tool_def_t by name to check is_read_only && is_concurrent */
static bool tool_is_concurrent_safe(const char *name) {
    int total = 0;
    const tool_def_t *all = tools_get_all(&total);
    for (int i = 0; i < total; i++) {
        if (all[i].name && strcmp(all[i].name, name) == 0)
            return all[i].is_read_only && all[i].is_concurrent;
    }
    return false;
}

/* Extract first + last sentence from text */
static void extract_bookend_summary(const char *text, char *out, size_t outlen) {
    if (!text || !out || outlen < 4) {
        if (out)
            out[0] = '\0';
        return;
    }

    /* First sentence: up to first period/newline or 200 chars */
    const char *end1 = text;
    int len1 = 0;
    while (*end1 && *end1 != '\n' && len1 < 200) {
        if (*end1 == '.' && (end1[1] == ' ' || end1[1] == '\n' || end1[1] == '\0')) {
            len1 = (int)(end1 - text) + 1;
            break;
        }
        end1++;
        len1++;
    }
    if (len1 == 0 || len1 >= 200)
        len1 = (end1 - text) < 200 ? (int)(end1 - text) : 200;

    /* Last sentence: scan backwards from end */
    size_t tlen = strlen(text);
    const char *last_start = text + tlen;
    if (tlen > (size_t)len1 + 10) {
        const char *p = text + tlen - 2;
        while (p > text + len1) {
            if (*p == '.' && (p[1] == ' ' || p[1] == '\n')) {
                last_start = p + 2;
                break;
            }
            p--;
        }
    }

    int n = snprintf(out, outlen, "%.*s", len1, text);
    if (last_start > text + len1 && (size_t)n < outlen - 10) {
        size_t remaining = tlen - (size_t)(last_start - text);
        if (remaining > 200)
            remaining = 200;
        snprintf(out + n, outlen - (size_t)n, " ... %.*s", (int)remaining, last_start);
    }
}

/* Flush accumulated embed batch: one Jina API call, store all in working memory */
static void flush_embed_batch(void) {
    if (g_embed_batch.count == 0 || !g_agent_memory_inited)
        return;

    for (int i = 0; i < g_embed_batch.count; i++) {
        int id = memory_store(&g_agent_memory, MEM_WORKING, g_embed_batch.key[i],
                              g_embed_batch.text[i], 0.5);
        if (id >= 0) {
            /* Embed and attach — tools_embed_text handles the API call */
            int dim = 0;
            float *vec = tools_embed_text(g_embed_batch.text[i], &dim);
            if (vec && dim > 0) {
                memory_entry_set_embedding(&g_agent_memory, g_embed_batch.key[i], vec, dim);
                free(vec);
            }
        }
    }
    g_embed_batch.count = 0;
}

static void agent_memory_ensure_init(void) {
    if (g_agent_memory_inited)
        return;
    memory_store_init(&g_agent_memory);
    g_agent_memory_inited = true;
}

/* ── MCP integration ───────────────────────────────────────────────────── */

static mcp_registry_t g_mcp = {0};

/* MCP tool execution callback for external tool system */
static char *mcp_tool_execute_cb(const char *name, const char *input_json, void *ctx) {
    mcp_registry_t *reg = (mcp_registry_t *)ctx;
    return mcp_call_tool(reg, name, input_json);
}

static void mcp_register_discovered_tools(mcp_registry_t *reg) {
    int mcp_count;
    const mcp_tool_t *mcp_tools = mcp_get_tools(reg, &mcp_count);
    for (int i = 0; i < mcp_count; i++) {
        tools_register_external(mcp_tools[i].name, mcp_tools[i].description,
                                mcp_tools[i].input_schema, mcp_tool_execute_cb, reg);
    }
    /* The bg loader (see mcp_bg_init_thread) sets g_mcp_bg_active so the
     * notification routes through tui_panel_notify instead of fprintf,
     * which would otherwise smear over the input panel rows. */
}

/* ── Background MCP loader ─────────────────────────────────────────────────
 * Connecting to MCP servers can take seconds (especially HTTP cold-starts
 * like Modal). Running mcp_init synchronously before the input panel renders
 * makes the REPL feel frozen on startup. Instead we paint the panel first,
 * then load MCP in a worker thread; tools register progressively. */

static pthread_t g_mcp_bg_thread;
static volatile int g_mcp_bg_started = 0; /* thread spawned */
static volatile int g_mcp_bg_active = 0;  /* thread still running */
static tui_status_bar_t *g_mcp_bg_sb = NULL;

static void *mcp_bg_init_thread(void *arg) {
    tui_status_bar_t *sb = (tui_status_bar_t *)arg;
    mcp_set_silent(true);
    int n = mcp_init(&g_mcp);
    if (n > 0)
        mcp_register_discovered_tools(&g_mcp);
    mcp_set_silent(false);

    /* Opt-in: pull the external Tool Management catalog and register each
     * remote tool as a dsco external tool. Gated on DSCO_TOOLMGMT so it stays
     * off unless the operator explicitly enables dynamic tool discovery. */
    if (getenv("DSCO_TOOLMGMT")) {
        int tm = toolmgmt_register_tools();
        if (sb && tm > 0) {
            char tnote[96];
            snprintf(tnote, sizeof(tnote), "tools: %d remote tools registered", tm);
            tui_panel_notify(sb, TUI_PANEL_NOTE_OK, tnote);
        }
    }

    if (sb) {
        char note[160];
        if (g_mcp.server_count == 0 && g_mcp.configured_count == 0) {
            /* No servers configured — nothing to announce. */
        } else if (g_mcp.tool_count > 0) {
            snprintf(note, sizeof(note), "mcp: %d tools from %d server%s ready%s%s",
                     g_mcp.tool_count, g_mcp.server_count, g_mcp.server_count == 1 ? "" : "s",
                     g_mcp.failed_count > 0 ? " · " : "",
                     g_mcp.failed_count > 0 ? "some failed" : "");
            tui_panel_notify(sb, TUI_PANEL_NOTE_OK, note);
        } else if (g_mcp.failed_count > 0) {
            snprintf(note, sizeof(note), "mcp: all %d server%s failed to connect",
                     g_mcp.failed_count, g_mcp.failed_count == 1 ? "" : "s");
            tui_panel_notify(sb, TUI_PANEL_NOTE_WARN, note);
        }
    }

    __atomic_store_n(&g_mcp_bg_active, 0, __ATOMIC_RELEASE);
    return NULL;
}

/* Drain freshly-finished background-agent pets into mini-notifications in the
 * input panel's middle rows. Called each time we read user input, so completions
 * that land between turns surface without the user polling. Each pet notifies
 * exactly once (pet_roster_next_unnotified marks it). */
static void drain_pet_notifications(tui_status_bar_t *sb) {
    if (!sb)
        return;
    pet_roster_t *r = pet_roster_global();
    pet_status_t st;
    char label[64];
    pet_bones_t bones;
    int id;
    while ((id = pet_roster_next_unnotified(r, &st, label, sizeof(label), &bones)) >= 0) {
        pet_bones_t fb = bones;
        fb.eye = (st == PET_ST_ERROR) ? 2 : 3; /* dizzy × / wide ◉ */
        char face[32];
        pet_render_face(&fb, face, sizeof(face));
        char note[160];
        snprintf(note, sizeof(note), "%s  agent #%d %s — %.40s", face, id,
                 st == PET_ST_DONE ? "done" : "failed", label);
        tui_panel_notify(sb, st == PET_ST_DONE ? TUI_PANEL_NOTE_OK : TUI_PANEL_NOTE_WARN, note);
    }
}

static void mcp_bg_init_start(tui_status_bar_t *sb) {
    if (g_mcp_bg_started)
        return;
    g_mcp_bg_sb = sb;
    __atomic_store_n(&g_mcp_bg_active, 1, __ATOMIC_RELEASE);
    if (pthread_create(&g_mcp_bg_thread, NULL, mcp_bg_init_thread, sb) != 0) {
        /* Fall back to synchronous init on the calling thread. */
        __atomic_store_n(&g_mcp_bg_active, 0, __ATOMIC_RELEASE);
        int n = mcp_init(&g_mcp);
        if (n > 0)
            mcp_register_discovered_tools(&g_mcp);
        return;
    }
    g_mcp_bg_started = 1;
}

static void mcp_bg_init_join(void) {
    if (!g_mcp_bg_started)
        return;
    pthread_join(g_mcp_bg_thread, NULL);
    g_mcp_bg_started = 0;
}

/* ── Rate limiter (token bucket) ───────────────────────────────────────── */

typedef struct {
    double tokens;      /* current token count */
    double max_tokens;  /* bucket capacity */
    double refill_rate; /* tokens per second */
    double last_refill; /* timestamp */
} rate_limiter_t;

static double now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec / 1e6;
}

static void rate_limiter_init(rate_limiter_t *rl, double max_tokens, double refill_rate) {
    rl->tokens = max_tokens;
    rl->max_tokens = max_tokens;
    rl->refill_rate = refill_rate;
    rl->last_refill = now_ms();
}

static bool rate_limiter_acquire(rate_limiter_t *rl) {
    if (g_interrupted)
        return false;
    double now = now_ms();
    double elapsed = now - rl->last_refill;
    rl->tokens += elapsed * rl->refill_rate;
    if (rl->tokens > rl->max_tokens)
        rl->tokens = rl->max_tokens;
    rl->last_refill = now;

    if (rl->tokens >= 1.0) {
        rl->tokens -= 1.0;
        return true;
    }
    /* Wait for token to become available */
    double wait = (1.0 - rl->tokens) / rl->refill_rate;
    if (wait > 0 && wait < 30.0) {
        fprintf(stderr, "  %srate limit: waiting %.1fs%s\n", TUI_DIM, wait, TUI_RESET);
        if (usleep((useconds_t)(wait * 1e6)) != 0 && errno == EINTR && g_interrupted) {
            return false;
        }
        if (g_interrupted)
            return false;
        rl->tokens = 0;
        rl->last_refill = now_ms();
    }
    return true;
}

/* ── Cost budget ───────────────────────────────────────────────────────── */

double g_cost_budget = 0.0;         /* default off (non-static: llm.c budget pressure) */
static double g_daily_budget = 0.0; /* default off */

/* Budgets default to off (0 = disabled). Override with env vars.
   Set to 0 to disable. Swarm children inherit DSCO_CHILD_BUDGET. */
static void init_cost_budgets(void) {
    const char *sb = getenv("DSCO_BUDGET");
    if (sb && sb[0])
        g_cost_budget = atof(sb);

    const char *db = getenv("DSCO_DAILY_BUDGET");
    if (db && db[0])
        g_daily_budget = atof(db);

    const char *child_budget = getenv("DSCO_CHILD_BUDGET");
    if (child_budget && child_budget[0]) {
        double cb = atof(child_budget);
        if (cb > 0 && (g_cost_budget <= 0 || cb < g_cost_budget))
            g_cost_budget = cb;
    }
}

/* Per-turn cost from token usage × current model's registry prices. Used as
 * the fallback when the provider doesn't report an authoritative cost. */
static double turn_token_cost(session_state_t *session, const usage_t *u) {
    const model_info_t *mi = model_lookup(session->model);
    if (!mi)
        return 0;
    return u->input_tokens * mi->input_price / 1e6 + u->output_tokens * mi->output_price / 1e6 +
           u->cache_read_input_tokens * mi->cache_read_price / 1e6 +
           u->cache_creation_input_tokens * mi->cache_write_price / 1e6;
}

static double session_cost(session_state_t *session) {
    /* Prefer the accumulated authoritative cost (provider-reported when
     * available, else per-turn token math). Falls back to recomputing from
     * cumulative tokens only before any turn has been accounted. */
    if (session->total_reported_cost_usd > 0)
        return session->total_reported_cost_usd;
    const model_info_t *mi = model_lookup(session->model);
    if (!mi)
        return 0;
    return session->total_input_tokens * mi->input_price / 1e6 +
           session->total_output_tokens * mi->output_price / 1e6 +
           session->total_cache_read_tokens * mi->cache_read_price / 1e6 +
           session->total_cache_write_tokens * mi->cache_write_price / 1e6;
}

static bool check_cost_budget(session_state_t *session) {
    double cost = session_cost(session);

    /* Daily budget (cross-session, from baseline DB) */
    if (g_daily_budget > 0) {
        double daily = baseline_daily_cost() + cost;
        if (daily >= g_daily_budget) {
            fprintf(stderr, "  %s%sdaily budget exceeded: $%.2f / $%.2f%s\n", TUI_BOLD, TUI_RED,
                    daily, g_daily_budget, TUI_RESET);
            fprintf(stderr,
                    "  %sraise it: /budget %.0f  (lifts the daily cap too) · "
                    "/budget daily <amount> · /budget off%s\n",
                    TUI_DIM, daily + 10, TUI_RESET);
            return false;
        }
        if (daily >= g_daily_budget * 0.8) {
            fprintf(stderr, "  %sdaily spend: $%.2f / $%.2f (%.0f%%)%s\n", TUI_YELLOW, daily,
                    g_daily_budget, 100.0 * daily / g_daily_budget, TUI_RESET);
        }
    }

    /* Session budget */
    if (g_cost_budget <= 0)
        return true;
    if (cost >= g_cost_budget) {
        fprintf(stderr, "  %s%ssession budget exceeded: $%.4f / $%.4f%s\n", TUI_BOLD, TUI_RED, cost,
                g_cost_budget, TUI_RESET);
        fprintf(stderr, "  %soverride: /budget <amount> or export DSCO_BUDGET=<amount>%s\n",
                TUI_DIM, TUI_RESET);
        return false;
    }
    if (cost >= g_cost_budget * 0.9) {
        fprintf(stderr, "  %scost warning: $%.4f / $%.4f (%.0f%%)%s\n", TUI_YELLOW, cost,
                g_cost_budget, 100.0 * cost / g_cost_budget, TUI_RESET);
    }
    return true;
}

/* ── Provider management ───────────────────────────────────────────────── */

static provider_t *g_provider = NULL;
static const char *g_provider_override_name = NULL;

static bool env_truthy(const char *value) {
    return value &&
           (value[0] == '1' || strcasecmp(value, "true") == 0 || strcasecmp(value, "yes") == 0);
}

static void ensure_provider(session_state_t *session, const char *api_key) {
    const char *pname = provider_route_for_model(session->model, api_key, g_provider_override_name);
    if (g_provider && strcmp(g_provider->name, pname) == 0)
        return;
    provider_free(g_provider);
    g_provider = provider_create(pname);
}

/* Resolve the API key for the current provider, falling back to the session
 * key (typically the Anthropic key) if the provider has no key of its own. */
static const char *resolve_provider_key(const char *session_key) {
    if (!g_provider)
        return session_key;
    const char *k = provider_resolve_request_api_key(g_provider->name, session_key);
    return (k && k[0]) ? k : session_key;
}

/* ── Image drag-and-drop support ─────────────────────────────────────── */

/* Max image size before downscaling (5MB raw, ~20MP) */
/* ── Image limits (from Anthropic Vision docs, 2026) ─────────────────────
 * Direct API:       100 images/request (200k-ctx models), 600 for others
 *                   10 MB per image (base64), 8000x8000 px max
 *                   >20 images → stricter per-image dimension limit applies
 * Bedrock/Vertex:   20 images/request, 5 MB per image
 * claude.ai:        20 images/message, 10 MB per image
 * dsco cap (safety margin): 100 images/request, 5 MB raw, 2000px max dim
 * (2000px keeps us safely under the >20-image stricter dimension limit)   */
#define IMG_MAX_FILE_SIZE  (5  * 1024 * 1024)  /* 5 MB — matches Bedrock/Vertex; safe for direct API (10 MB) */
#define IMG_MAX_DIMENSION   2000                /* safe for all platforms incl. >20-image requests */
#define IMG_MAX_B64_SIZE   (10 * 1024 * 1024)  /* 10 MB base64 — direct API limit */
#define IMG_MAX_PER_MSG     8                   /* conservative default; API supports 100 */

static const char *img_media_type_for_ext(const char *ext) {
    if (strcasecmp(ext, ".png") == 0)
        return "image/png";
    if (strcasecmp(ext, ".jpg") == 0)
        return "image/jpeg";
    if (strcasecmp(ext, ".jpeg") == 0)
        return "image/jpeg";
    if (strcasecmp(ext, ".gif") == 0)
        return "image/gif";
    if (strcasecmp(ext, ".webp") == 0)
        return "image/webp";
    if (strcasecmp(ext, ".bmp") == 0)
        return "image/bmp";
    if (strcasecmp(ext, ".tif") == 0)
        return "image/tiff";
    if (strcasecmp(ext, ".tiff") == 0)
        return "image/tiff";
    if (strcasecmp(ext, ".heic") == 0)
        return "image/heic";
    if (strcasecmp(ext, ".heif") == 0)
        return "image/heif";
    if (strcasecmp(ext, ".avif") == 0)
        return "image/avif";
    return NULL;
}

static int hex_val(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static void url_decode_inplace(char *s) {
    char *r = s;
    char *w = s;
    while (*r) {
        if (r[0] == '%' && r[1] && r[2]) {
            int hi = hex_val(r[1]);
            int lo = hex_val(r[2]);
            if (hi >= 0 && lo >= 0) {
                *w++ = (char)((hi << 4) | lo);
                r += 3;
                continue;
            }
        }
        *w++ = *r++;
    }
    *w = '\0';
}

static bool shell_quote_single(const char *src, char *dst, size_t dst_sz) {
    if (!src || !dst || dst_sz < 3)
        return false;
    size_t d = 0;
    dst[d++] = '\'';
    for (const char *p = src; *p; p++) {
        if (*p == '\'') {
            if (d + 4 >= dst_sz)
                return false;
            dst[d++] = '\'';
            dst[d++] = '\\';
            dst[d++] = '\'';
            dst[d++] = '\'';
        } else {
            if (d + 1 >= dst_sz)
                return false;
            dst[d++] = *p;
        }
    }
    if (d + 2 > dst_sz)
        return false;
    dst[d++] = '\'';
    dst[d] = '\0';
    return true;
}

static const char *extract_image_path(const char *token, char *out_path, size_t out_sz) {
    if (!token || !*token)
        return NULL;

    size_t tlen = strlen(token);
    const char *start = token;
    if ((token[0] == '\'' || token[0] == '"') && tlen > 2 && token[tlen - 1] == token[0]) {
        start = token + 1;
        tlen -= 2;
    }
    if (tlen > 2 && token[0] == '<' && token[tlen - 1] == '>') {
        start = token + 1;
        tlen -= 2;
    }

    if (tlen >= out_sz)
        tlen = out_sz - 1;
    memcpy(out_path, start, tlen);
    out_path[tlen] = '\0';
    while (tlen > 0 && (out_path[tlen - 1] == ' ' || out_path[tlen - 1] == '\t'))
        out_path[--tlen] = '\0';

    {
        char *r = out_path, *w = out_path;
        while (*r) {
            if (*r == '\\' && *(r + 1) == ' ') {
                *w++ = ' ';
                r += 2;
            } else {
                *w++ = *r++;
            }
        }
        *w = '\0';
        tlen = (size_t)(w - out_path);
    }

    if (strncasecmp(out_path, "file://", 7) == 0) {
        char *uri = out_path + 7;
        if (strncasecmp(uri, "localhost/", 10) == 0)
            uri += 9;
        if (*uri) {
            memmove(out_path, uri, strlen(uri) + 1);
            url_decode_inplace(out_path);
            tlen = strlen(out_path);
        }
    }

    if (out_path[0] == '~' && (out_path[1] == '/' || out_path[1] == '\0')) {
        const char *home = getenv("HOME");
        if (home) {
            char tmp[4096];
            snprintf(tmp, sizeof(tmp), "%s%s", home, out_path + 1);
            snprintf(out_path, out_sz, "%s", tmp);
        }
    }

    const char *dot = strrchr(out_path, '.');
    if (!dot)
        return NULL;
    return img_media_type_for_ext(dot);
}

static char *load_and_encode_image(const char *path, const char *media_type,
                                   tui_spinner_t *spinner) {
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
        return NULL;
    if (st.st_size > IMG_MAX_FILE_SIZE)
        return NULL;

    const char *read_path = path;
    char downscaled[512] = "";

    if (st.st_size > 1024 * 1024 && strcmp(media_type, "image/gif") != 0) {
        if (spinner) {
            spinner->label = "downsizing image...";
            tui_spinner_tick(spinner);
        }
        snprintf(downscaled, sizeof(downscaled), "/tmp/dsco_drag_%d.jpg", getpid());

        /* Portable in-process path: decode → resize → JPEG via stb. */
        bool ok = dsco_image_downscale_jpeg(path, IMG_MAX_DIMENSION, downscaled);
#ifdef __APPLE__
        if (!ok) {
            /* stb can't decode HEIC/WEBP/AVIF/TIFF — fall back to macOS sips. */
            char q_path[8192];
            char q_out[1024];
            if (shell_quote_single(path, q_path, sizeof(q_path)) &&
                shell_quote_single(downscaled, q_out, sizeof(q_out))) {
                char cmd[12288];
                snprintf(cmd, sizeof(cmd),
                         "sips --resampleHeightWidthMax %d %s --setProperty format jpeg --out %s "
                         "2>/dev/null",
                         IMG_MAX_DIMENSION, q_path, q_out);
                ok = (system(cmd) == 0);
            }
        }
#endif
        if (ok && stat(downscaled, &st) == 0) {
            read_path = downscaled;
            media_type = "image/jpeg";
        } else {
            downscaled[0] = '\0';
        }
    }

    if (spinner) {
        spinner->label = "encoding image...";
        tui_spinner_tick(spinner);
    }

    FILE *f = fopen(read_path, "rb");
    if (!f) {
        if (downscaled[0])
            unlink(downscaled);
        return NULL;
    }

    size_t file_sz = (size_t)st.st_size;
    uint8_t *raw = safe_malloc(file_sz);
    size_t nread = fread(raw, 1, file_sz, f);
    fclose(f);

    if (downscaled[0])
        unlink(downscaled);

    size_t b64_sz = ((nread + 2) / 3) * 4 + 1;
    if (b64_sz > (size_t)IMG_MAX_B64_SIZE) {
        free(raw);
        return NULL;
    }
    char *b64 = safe_malloc(b64_sz);
    size_t olen = base64_encode(raw, nread, b64, b64_sz);
    b64[olen] = '\0';
    free(raw);

    return b64;
}

typedef struct {
    uint8_t *data;
    size_t len;
    size_t cap;
    bool overflow;
} remote_image_buf_t;

static size_t remote_image_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    remote_image_buf_t *buf = (remote_image_buf_t *)userdata;
    size_t total = size * nmemb;
    if (!buf || total == 0)
        return 0;

    if (buf->len + total > IMG_MAX_FILE_SIZE) {
        buf->overflow = true;
        return 0;
    }

    size_t need = buf->len + total + 1;
    if (need > buf->cap) {
        size_t newcap = buf->cap ? buf->cap * 2 : 8192;
        while (newcap < need)
            newcap *= 2;
        buf->data = safe_realloc(buf->data, newcap);
        buf->cap = newcap;
    }

    memcpy(buf->data + buf->len, ptr, total);
    buf->len += total;
    buf->data[buf->len] = '\0';
    return total;
}

static const char *img_media_type_for_magic(const uint8_t *data, size_t len) {
    if (!data || len < 3)
        return NULL;

    if (len >= 8 && data[0] == 0x89 && data[1] == 'P' && data[2] == 'N' && data[3] == 'G' &&
        data[4] == 0x0d && data[5] == 0x0a && data[6] == 0x1a && data[7] == 0x0a) {
        return "image/png";
    }
    if (len >= 3 && data[0] == 0xff && data[1] == 0xd8 && data[2] == 0xff) {
        return "image/jpeg";
    }
    if (len >= 12 && memcmp(data, "RIFF", 4) == 0 && memcmp(data + 8, "WEBP", 4) == 0) {
        return "image/webp";
    }

    return NULL;
}

static const char *img_media_type_for_url(const char *url, char *ext_buf, size_t ext_buf_sz) {
    if (!url || !url[0] || !ext_buf || ext_buf_sz < 8)
        return NULL;

    const char *end = strpbrk(url, "?#");
    size_t path_len = end ? (size_t)(end - url) : strlen(url);
    const char *dot = NULL;
    for (size_t i = 0; i < path_len; i++) {
        if (url[i] == '.')
            dot = url + i;
    }
    if (!dot)
        return NULL;

    size_t ext_len = path_len - (size_t)(dot - url);
    if (ext_len == 0 || ext_len >= ext_buf_sz)
        return NULL;
    memcpy(ext_buf, dot, ext_len);
    ext_buf[ext_len] = '\0';
    return img_media_type_for_ext(ext_buf);
}

static char *download_and_encode_image_url(const char *url, const char **media_type_out,
                                           tui_spinner_t *spinner, char *err, size_t err_sz) {
    if (media_type_out)
        *media_type_out = NULL;
    if (err && err_sz > 0)
        err[0] = '\0';
    if (!url || !url[0])
        return NULL;

    CURL *curl = curl_easy_init();
    if (!curl) {
        if (err && err_sz > 0)
            snprintf(err, err_sz, "failed to initialize curl");
        return NULL;
    }

    remote_image_buf_t buf;
    memset(&buf, 0, sizeof(buf));

    if (spinner) {
        spinner->label = "downloading image URL...";
        tui_spinner_tick(spinner);
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, remote_image_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "dsco/" DSCO_VERSION);

    CURLcode res = curl_easy_perform(curl);
    long http_status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        if (err && err_sz > 0) {
            if (buf.overflow)
                snprintf(err, err_sz, "remote image exceeds %d MB",
                         IMG_MAX_FILE_SIZE / (1024 * 1024));
            else
                snprintf(err, err_sz, "image download failed");
        }
        free(buf.data);
        return NULL;
    }

    if (http_status < 200 || http_status >= 300) {
        if (err && err_sz > 0)
            snprintf(err, err_sz, "image URL returned HTTP %ld", http_status);
        free(buf.data);
        return NULL;
    }

    const char *media_type = img_media_type_for_magic(buf.data, buf.len);
    if (!media_type) {
        if (err && err_sz > 0) {
            snprintf(err, err_sz, "image URL did not return a valid PNG, JPEG, or WebP file");
        }
        free(buf.data);
        return NULL;
    }

    if (spinner) {
        spinner->label = "encoding image...";
        tui_spinner_tick(spinner);
    }

    size_t b64_sz = ((buf.len + 2) / 3) * 4 + 1;
    if (b64_sz > (size_t)IMG_MAX_B64_SIZE) {
        if (err && err_sz > 0)
            snprintf(err, err_sz, "encoded image exceeds size limit");
        free(buf.data);
        return NULL;
    }

    char *b64 = safe_malloc(b64_sz);
    size_t olen = base64_encode(buf.data, buf.len, b64, b64_sz);
    b64[olen] = '\0';
    free(buf.data);

    if (media_type_out)
        *media_type_out = media_type;
    return b64;
}

static void format_mode_string(mode_t mode, char out[11]) {
    out[0] = S_ISDIR(mode)    ? 'd'
             : S_ISLNK(mode)  ? 'l'
             : S_ISCHR(mode)  ? 'c'
             : S_ISBLK(mode)  ? 'b'
             : S_ISFIFO(mode) ? 'p'
             : S_ISSOCK(mode) ? 's'
                              : '-';
    out[1] = (mode & S_IRUSR) ? 'r' : '-';
    out[2] = (mode & S_IWUSR) ? 'w' : '-';
    out[3] = (mode & S_IXUSR) ? 'x' : '-';
    out[4] = (mode & S_IRGRP) ? 'r' : '-';
    out[5] = (mode & S_IWGRP) ? 'w' : '-';
    out[6] = (mode & S_IXGRP) ? 'x' : '-';
    out[7] = (mode & S_IROTH) ? 'r' : '-';
    out[8] = (mode & S_IWOTH) ? 'w' : '-';
    out[9] = (mode & S_IXOTH) ? 'x' : '-';
    out[10] = '\0';
}

static int dirlist_filter(const struct dirent *ent) {
    if (!ent)
        return 0;
    return strcmp(ent->d_name, ".") != 0 && strcmp(ent->d_name, "..") != 0;
}

static bool expand_home_path(const char *input, char *out, size_t out_sz) {
    if (!input || !out || out_sz == 0)
        return false;
    if (input[0] == '~' && (input[1] == '/' || input[1] == '\0')) {
        const char *home = getenv("HOME");
        if (home && home[0]) {
            int n = snprintf(out, out_sz, "%s%s", home, input + 1);
            return n >= 0 && (size_t)n < out_sz;
        }
    }
    int n = snprintf(out, out_sz, "%s", input);
    return n >= 0 && (size_t)n < out_sz;
}

static bool append_directory_listing(jbuf_t *out, const char *dir_path) {
    if (!out || !dir_path || !dir_path[0])
        return false;

    struct dirent **ents = NULL;
    int n = scandir(dir_path, &ents, dirlist_filter, alphasort);
    if (n < 0)
        return false;

    jbuf_appendf(out, "[directory: %s]\n", dir_path);
    if (n == 0) {
        jbuf_append(out, "(empty)\n");
        free(ents);
        return true;
    }

    const int max_entries = 200;
    int emitted = 0;
    for (int i = 0; i < n; i++) {
        if (emitted >= max_entries) {
            jbuf_appendf(out, "[truncated after %d entries]\n", emitted);
            break;
        }

        struct dirent *ent = ents[i];
        size_t need = strlen(dir_path) + strlen(ent->d_name) + 2;
        char *full_path = safe_malloc(need);
        snprintf(full_path, need, "%s/%s", dir_path, ent->d_name);

        struct stat st;
        if (lstat(full_path, &st) != 0) {
            free(full_path);
            continue;
        }

        char mode_buf[11];
        format_mode_string(st.st_mode, mode_buf);

        char time_buf[32] = "";
        struct tm tm_buf;
        if (localtime_r(&st.st_mtime, &tm_buf)) {
            strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M", &tm_buf);
        }

        jbuf_appendf(out, "%s %3lu %8lld %s %s", mode_buf, (unsigned long)st.st_nlink,
                     (long long)st.st_size, time_buf[0] ? time_buf : "----", ent->d_name);

        if (S_ISLNK(st.st_mode)) {
            char link_target[1024];
            ssize_t link_len = readlink(full_path, link_target, sizeof(link_target) - 1);
            if (link_len >= 0) {
                link_target[link_len] = '\0';
                dsco_strip_terminal_controls_inplace(link_target);
                jbuf_appendf(out, " -> %s", link_target);
            }
        }

        jbuf_append(out, "\n");
        emitted++;
        free(full_path);

        if (out->len > (size_t)(MAX_INPUT_LINE - 4096)) {
            jbuf_appendf(out, "[truncated after %d entries]\n", emitted);
            break;
        }
    }

    for (int i = 0; i < n; i++) {
        free(ents[i]);
    }
    free(ents);
    return true;
}

static int process_dragged_images(char *input_buf, conversation_t *conv) {
    bool has_img_ext = false;
    const char *exts[] = {".png",  ".jpg",  ".jpeg", ".gif",  ".webp", ".bmp",  ".tif", ".tiff",
                          ".heic", ".heif", ".avif", ".PNG",  ".JPG",  ".JPEG", ".GIF", ".WEBP",
                          ".BMP",  ".TIF",  ".TIFF", ".HEIC", ".HEIF", ".AVIF"};
    for (int i = 0; i < 22; i++) {
        if (strstr(input_buf, exts[i])) {
            has_img_ext = true;
            break;
        }
    }
    if (!has_img_ext && !strstr(input_buf, "file://"))
        return 0;

    char clean_path[4096];
    int img_count = 0;

    char *images_b64[IMG_MAX_PER_MSG] = {0};
    const char *images_mt[IMG_MAX_PER_MSG] = {0};
    char remaining_text[MAX_INPUT_LINE];
    remaining_text[0] = '\0';
    int rem_pos = 0;

    char *buf = safe_strdup(input_buf);
    char *p = buf;

    while (*p && img_count < IMG_MAX_PER_MSG) {
        char *ext_pos = NULL;
        const char *matched_ext = NULL;
        for (int i = 0; i < 22; i++) {
            char *found = strstr(p, exts[i]);
            if (found && (!ext_pos || found < ext_pos)) {
                ext_pos = found;
                matched_ext = exts[i];
            }
        }

        if (!ext_pos) {
            size_t left = strlen(p);
            if (left > 0 && rem_pos + (int)left < MAX_INPUT_LINE - 1) {
                memcpy(remaining_text + rem_pos, p, left);
                rem_pos += (int)left;
            }
            break;
        }

        char *path_end = ext_pos + strlen(matched_ext);

        char *path_start = ext_pos;
        bool in_quotes = false;
        while (path_start > p) {
            char prev = *(path_start - 1);
            if (prev == '\'' || prev == '"') {
                if (path_start - 1 == p || *(path_start - 2) == ' ') {
                    path_start--;
                    in_quotes = true;
                    if (*path_end == prev)
                        path_end++;
                    break;
                }
            }
            if (prev == ' ' && !in_quotes) {
                if (path_start >= p + 2 && *(path_start - 2) == '\\') {
                    path_start -= 2;
                    continue;
                }
                break;
            }
            path_start--;
        }

        size_t prefix_len = (size_t)(path_start - p);
        if (prefix_len > 0 && rem_pos + (int)prefix_len < MAX_INPUT_LINE - 1) {
            memcpy(remaining_text + rem_pos, p, prefix_len);
            rem_pos += (int)prefix_len;
        }

        size_t plen = (size_t)(path_end - path_start);
        char token[4096];
        if (plen < sizeof(token)) {
            memcpy(token, path_start, plen);
            token[plen] = '\0';

            const char *mt = extract_image_path(token, clean_path, sizeof(clean_path));
            if (mt && access(clean_path, R_OK) == 0) {
                tui_spinner_t spin;
                tui_spinner_init(&spin, SPINNER_DOTS, "dragging image...", TUI_CYAN);
                tui_spinner_tick(&spin);

                char *b64 = load_and_encode_image(clean_path, mt, &spin);
                if (b64) {
                    images_b64[img_count] = b64;
                    images_mt[img_count] = mt;
                    img_count++;

                    const char *fname = strrchr(clean_path, '/');
                    fname = fname ? fname + 1 : clean_path;
                    char done_msg[256];
                    snprintf(done_msg, sizeof(done_msg), "image loaded: %s", fname);
                    tui_spinner_done(&spin, done_msg);
                } else {
                    tui_spinner_done(&spin, "failed to load image");
                    if (rem_pos + (int)plen < MAX_INPUT_LINE - 1) {
                        memcpy(remaining_text + rem_pos, path_start, plen);
                        rem_pos += (int)plen;
                    }
                }
            } else {
                if (rem_pos + (int)plen < MAX_INPUT_LINE - 1) {
                    memcpy(remaining_text + rem_pos, path_start, plen);
                    rem_pos += (int)plen;
                }
            }
        }

        p = path_end;
    }

    remaining_text[rem_pos] = '\0';
    free(buf);

    if (img_count == 0)
        return 0;

    char *text = remaining_text;
    while (*text == ' ')
        text++;
    size_t tlen = strlen(text);
    while (tlen > 0 && text[tlen - 1] == ' ')
        text[--tlen] = '\0';

    if (img_count == 1) {
        conv_add_user_image_base64(conv, images_mt[0], images_b64[0],
                                   tlen > 0 ? text : "Describe this image.");
    } else {
        if (tlen > 0) {
            conv_add_user_text(conv, text);
        }
        for (int i = 0; i < img_count; i++) {
            char prompt[128];
            snprintf(prompt, sizeof(prompt), "Image %d of %d", i + 1, img_count);
            conv_add_user_image_base64(conv, images_mt[i], images_b64[i],
                                       i == 0 && tlen == 0 ? "Describe these images." : prompt);
        }
    }

    for (int i = 0; i < img_count; i++)
        free(images_b64[i]);

    if (tlen > 0) {
        snprintf(input_buf, MAX_INPUT_LINE, "%s", text);
    } else {
        snprintf(input_buf, MAX_INPUT_LINE, "[%d image(s)]", img_count);
    }

    return img_count;
}

static void terminal_input_echo_suspend(void);
static void terminal_input_echo_restore(void);

static void sigint_handler(int sig) {
    (void)sig;
    /* First Ctrl+C: pause current streaming turn (ESC_PAUSED state).
       Second Ctrl+C: hard exit (already interrupted/stuck). */
    if (g_interrupted) {
        terminal_input_echo_restore();
        const char reset[] = "\033[r\033[?2004l\033[0m\033[?25h\n";
        (void)write(STDERR_FILENO, reset, sizeof(reset) - 1);
        _exit(130);
    }
    g_escape_state = ESC_PAUSED;
    g_interrupted = 1;
}

static void sigtstp_handler(int sig) {
    (void)sig;
    /* Reset terminal state before suspend so the parent shell isn't corrupted */
    terminal_input_echo_restore();
    const char reset[] = "\033[r\033[?2004l\033[0m\033[?25h";
    (void)write(STDERR_FILENO, reset, sizeof(reset) - 1);
    /* Re-raise default SIGTSTP to actually suspend */
    signal(SIGTSTP, SIG_DFL);
    raise(SIGTSTP);
}

/* ── Terminal cleanup atexit handler ───────────────────────────────────── */
static void terminal_reset_atexit(void) {
    terminal_input_echo_restore();
    /* Safety net: always restore terminal to sane state on exit.
       This catches all exit() paths including readline EOF, quit command, etc.
       Reset: scroll region, bracketed paste, SGR attributes, cursor visibility. */
    fprintf(stderr, "\033[r\033[?2004l\033[0m\033[?25h");
    fflush(stderr);
}

/* ── SIGWINCH handler (terminal resize) ────────────────────────────────── */
static tui_status_bar_t *g_winch_sb = NULL; /* set in agent_run */
static volatile sig_atomic_t g_winch_pending = 0;

static void sigwinch_handler(int sig) {
    (void)sig;
    /* Only set flag — actual resize handling happens in main loop
       (fprintf/mutex are not async-signal-safe) */
    g_winch_pending = 1;
}

/* Called from main loop to handle deferred SIGWINCH resize */
static __attribute__((unused)) void handle_pending_winch(void) {
    if (!g_winch_pending)
        return;
    g_winch_pending = 0;
    /* Ephemeral panel: nothing pinned. The next composer_read repaints
     * the panel at the new dimensions. Just clear the bottom row band
     * to prevent stale chrome from a smaller previous size. */
    if (!g_winch_sb || !g_winch_sb->visible)
        return;

    int rows = tui_term_height();
    int panel = g_winch_sb->panel_rows > 0 ? g_winch_sb->panel_rows : TUI_COMPOSER_PANEL_ROWS;
    tui_term_lock();
    for (int i = 0; i < panel; i++) {
        fprintf(stderr, "\033[%d;1H\033[2K", rows - i);
    }
    fflush(stderr);
    tui_term_unlock();
}

/* ── Auto-save ─────────────────────────────────────────────────────────── */

static void autosave(conversation_t *conv, session_state_t *session) {
    if (conv->count == 0)
        return;
    char dir_path[512], save_path[560];
    const char *home = getenv("HOME");
    if (!home)
        return;
    snprintf(dir_path, sizeof(dir_path), "%s/.dsco/sessions", home);
    mkdir(dir_path, 0755);
    snprintf(save_path, sizeof(save_path), "%s/_autosave.json", dir_path);
    conv_save_ex(conv, session, save_path);
}

/* Global for signal-handler auto-save */
static conversation_t *g_autosave_conv = NULL;
static session_state_t *g_autosave_session = NULL;

static void exit_autosave_handler(void) {
    if (g_autosave_conv)
        autosave(g_autosave_conv, g_autosave_session);
}

static void sigterm_autosave(int sig) {
    if (g_autosave_conv)
        autosave(g_autosave_conv, g_autosave_session);
    /* Kill MCP children before exiting so they don't orphan. */
    mcp_shutdown(&g_mcp);
    const char reset[] = "\033[r\033[?2004l\033[0m\033[?25h\n";
    (void)write(STDERR_FILENO, reset, sizeof(reset) - 1);
    (void)sig;
    _exit(0);
}

/* ── Saved-session navigator ───────────────────────────────────────────── */

#define DSCO_SESSION_LIST_MAX 256

typedef struct {
    char name[64];
    char path[1024];
    char model[128];
    char title[192];
    time_t mtime;
    long long size;
    int messages;
    int tool_uses;
    int est_tokens;
    bool active;
} saved_session_entry_t;

typedef struct {
    char name[64];
    char expansion[MAX_INPUT_LINE];
} command_alias_t;

static saved_session_entry_t *session_entries_alloc(void) {
    saved_session_entry_t *entries = safe_malloc((size_t)DSCO_SESSION_LIST_MAX * sizeof(*entries));
    memset(entries, 0, (size_t)DSCO_SESSION_LIST_MAX * sizeof(*entries));
    return entries;
}

static const char *session_current_name(const session_state_t *session) {
    return (session && session->slot_name[0]) ? session->slot_name : "default";
}

static void sessions_dir(char *out, size_t out_len) {
    const char *home = getenv("HOME");
    if (!home || !home[0])
        home = "/tmp";
    snprintf(out, out_len, "%s/.dsco/sessions", home);
}

static void sessions_ensure_dir(void) {
    const char *home = getenv("HOME");
    if (!home || !home[0])
        home = "/tmp";
    char base[512], dir[512];
    const mode_t dir_mode = 493; /* 0755 */
    snprintf(base, sizeof(base), "%s/.dsco", home);
    mkdir(base, dir_mode);
    sessions_dir(dir, sizeof(dir));
    mkdir(dir, dir_mode);
}

static void session_file_path(const char *name, char *out, size_t out_len) {
    char dir[512];
    sessions_ensure_dir();
    sessions_dir(dir, sizeof(dir));
    snprintf(out, out_len, "%s/%s.json", dir, name && name[0] ? name : "default");
}

static void session_sanitize_name(const char *input, char *out, size_t out_len) {
    if (!out || out_len == 0)
        return;
    out[0] = '\0';
    if (!input)
        input = "";

    size_t n = 0;
    bool last_dash = false;
    while (*input && n + 1 < out_len) {
        unsigned char ch = (unsigned char)*input++;
        if (isalnum(ch) || ch == '_' || ch == '-' || ch == '.') {
            out[n++] = (char)ch;
            last_dash = false;
        } else if (isspace(ch) || ch == '/') {
            if (!last_dash && n > 0 && n + 1 < out_len) {
                out[n++] = '-';
                last_dash = true;
            }
        }
    }
    while (n > 0 && (out[n - 1] == '-' || out[n - 1] == '.'))
        n--;
    out[n] = '\0';
    if (!out[0])
        snprintf(out, out_len, "session");
    if (out[0] == '.')
        out[0] = 's';
}

static void session_title_from_text(const char *text, char *out, size_t out_len) {
    if (!out || out_len == 0)
        return;
    out[0] = '\0';
    if (!text || !text[0])
        return;

    size_t n = 0;
    bool pending_space = false;
    for (const char *p = text; *p && n + 1 < out_len; p++) {
        unsigned char ch = (unsigned char)*p;
        if (ch == '\033')
            break;
        if (isspace(ch)) {
            pending_space = n > 0;
            continue;
        }
        if (pending_space && n + 1 < out_len) {
            out[n++] = ' ';
            pending_space = false;
        }
        if (ch >= 0x20 && ch != 0x7f)
            out[n++] = (char)ch;
        if (n >= out_len - 1)
            break;
    }
    while (n > 0 && isspace((unsigned char)out[n - 1]))
        n--;
    out[n] = '\0';
}

static void session_format_age(time_t mtime, char *out, size_t out_len) {
    if (!out || out_len == 0)
        return;
    time_t now = time(NULL);
    long diff = (long)difftime(now, mtime);
    if (diff < 0)
        diff = 0;
    if (diff < 60)
        snprintf(out, out_len, "%lds", diff);
    else if (diff < 3600)
        snprintf(out, out_len, "%ldm", diff / 60);
    else if (diff < 86400)
        snprintf(out, out_len, "%ldh", diff / 3600);
    else if (diff < 86400L * 30)
        snprintf(out, out_len, "%ldd", diff / 86400);
    else
        snprintf(out, out_len, "%ldmo", diff / (86400L * 30));
}

static void session_entry_load_metadata(saved_session_entry_t *entry, const char *fallback_model) {
    if (!entry)
        return;
    entry->title[0] = '\0';
    entry->model[0] = '\0';

    conversation_t tmp_conv;
    session_state_t tmp_session;
    conv_init(&tmp_conv);
    session_state_init(&tmp_session, fallback_model && fallback_model[0] ? fallback_model : "opus");

    if (conv_load_ex(&tmp_conv, &tmp_session, entry->path)) {
        entry->messages = tmp_conv.count;
        snprintf(entry->model, sizeof(entry->model), "%s", tmp_session.model);
        for (int i = 0; i < tmp_conv.count; i++) {
            for (int j = 0; j < tmp_conv.msgs[i].content_count; j++) {
                msg_content_t *mc = &tmp_conv.msgs[i].content[j];
                if (mc->tool_name)
                    entry->tool_uses++;
                if (mc->text)
                    entry->est_tokens += tui_estimate_tokens(mc->text);
                if (!entry->title[0] && tmp_conv.msgs[i].role == ROLE_USER && mc->text) {
                    session_title_from_text(mc->text, entry->title, sizeof(entry->title));
                }
            }
        }
    }
    if (!entry->model[0])
        snprintf(entry->model, sizeof(entry->model), "%s",
                 fallback_model && fallback_model[0] ? fallback_model : "?");
    if (!entry->title[0])
        snprintf(entry->title, sizeof(entry->title), "(empty)");
    conv_free(&tmp_conv);
}

static int session_entry_cmp(const void *a, const void *b) {
    const saved_session_entry_t *ea = (const saved_session_entry_t *)a;
    const saved_session_entry_t *eb = (const saved_session_entry_t *)b;
    if (ea->mtime > eb->mtime)
        return -1;
    if (ea->mtime < eb->mtime)
        return 1;
    return strcmp(ea->name, eb->name);
}

static int session_load_entries(saved_session_entry_t *entries, int max_entries,
                                const session_state_t *session) {
    if (!entries || max_entries <= 0)
        return 0;
    char dir_path[512];
    sessions_dir(dir_path, sizeof(dir_path));
    DIR *d = opendir(dir_path);
    if (!d)
        return 0;

    const char *active = session_current_name(session);
    int count = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && count < max_entries) {
        size_t nlen = strlen(ent->d_name);
        if (nlen <= 5 || strcmp(ent->d_name + nlen - 5, ".json") != 0)
            continue;

        saved_session_entry_t *e = &entries[count];
        memset(e, 0, sizeof(*e));
        snprintf(e->name, sizeof(e->name), "%.*s", (int)(nlen - 5), ent->d_name);
        snprintf(e->path, sizeof(e->path), "%s/%s", dir_path, ent->d_name);

        struct stat st;
        if (stat(e->path, &st) == 0) {
            e->mtime = st.st_mtime;
            e->size = (long long)st.st_size;
        }
        e->active = strcmp(e->name, active) == 0;
        session_entry_load_metadata(e, session ? session->model : NULL);
        count++;
    }
    closedir(d);
    qsort(entries, (size_t)count, sizeof(entries[0]), session_entry_cmp);
    return count;
}

static void session_print_entries(const saved_session_entry_t *entries, int count) {
    if (!entries || count <= 0) {
        tui_info("no saved sessions");
        return;
    }

    int term_w = tui_term_width();
    int title_w = term_w - 73;
    if (title_w < 24)
        title_w = 24;
    if (title_w > 72)
        title_w = 72;

    fprintf(stderr, "\n");
    tui_header("Saved Sessions", TUI_BCYAN);
    fprintf(stderr, "  %s%3s  %-1s %-22s %-6s %5s %5s %-15s %.*s%s\n", TUI_DIM, "#", "", "name",
            "age", "msgs", "tools", "model", title_w, "title", TUI_RESET);
    for (int i = 0; i < count; i++) {
        char age[16];
        session_format_age(entries[i].mtime, age, sizeof(age));
        const model_info_t *mi = model_lookup(entries[i].model);
        const char *model = mi ? mi->alias : entries[i].model;
        fprintf(stderr, "  %s%3d%s  %s %-22.22s %-6s %5d %5d %-15.15s %.*s%s\n", TUI_DIM, i + 1,
                TUI_RESET, entries[i].active ? "*" : " ", entries[i].name, age, entries[i].messages,
                entries[i].tool_uses, model, title_w, entries[i].title, TUI_RESET);
    }
    fprintf(stderr, "  %s/resume <#|name|text> to switch, /new <name> to start fresh%s\n\n",
            TUI_DIM, TUI_RESET);
}

static int session_find_entry(const saved_session_entry_t *entries, int count, const char *query,
                              bool *ambiguous) {
    if (ambiguous)
        *ambiguous = false;
    if (!entries || count <= 0 || !query || !query[0])
        return -1;

    char *end = NULL;
    long index = strtol(query, &end, 10);
    if (end && *end == '\0' && index >= 1 && index <= count)
        return (int)index - 1;

    for (int i = 0; i < count; i++) {
        if (strcmp(entries[i].name, query) == 0)
            return i;
    }

    int match = -1;
    for (int i = 0; i < count; i++) {
        if (strstr(entries[i].name, query) || strstr(entries[i].title, query)) {
            if (match >= 0) {
                if (ambiguous)
                    *ambiguous = true;
                return -1;
            }
            match = i;
        }
    }
    return match;
}

static void session_reset_usage_for_new(session_state_t *session) {
    if (!session)
        return;
    session->total_input_tokens = 0;
    session->total_output_tokens = 0;
    session->total_cache_read_tokens = 0;
    session->total_cache_write_tokens = 0;
    session->total_reported_cost_usd = 0;
    session->turn_count = 0;
    session->total_ttft_ms = 0.0;
    session->total_stream_ms = 0.0;
    session->telemetry_samples = 0;
    session->max_output_override = 0;
    session->max_output_recovery_count = 0;
    session->tool_choice[0] = '\0';
    session->prefill[0] = '\0';
    session->stop_seq[0] = '\0';
    session->pin_text[0] = '\0';
    session->memory_context[0] = '\0';
}

static bool session_switch_to_file(conversation_t *conv, session_state_t *session,
                                   tui_status_bar_t *status_bar, const char *name, const char *path,
                                   bool save_current_first) {
    if (!conv || !session || !name || !path)
        return false;

    if (save_current_first) {
        char cur_path[1024];
        session_file_path(session_current_name(session), cur_path, sizeof(cur_path));
        conv_save_ex(conv, session, cur_path);
    }

    if (!conv_load_ex(conv, session, path))
        return false;
    snprintf(session->slot_name, sizeof(session->slot_name), "%s", name);
    session->model_locked = true;
    tools_set_runtime_model(session->model);
    if (status_bar)
        tui_status_bar_set_model(status_bar, session->model, session->slot_name);
    return true;
}

/* ── Cost tracking ─────────────────────────────────────────────────────── */

static void print_cost(session_state_t *session) {
    const model_info_t *mi = model_lookup(session->model);
    if (!mi) {
        fprintf(stderr, "  %sunknown model for pricing%s\n", TUI_DIM, TUI_RESET);
        return;
    }
    double in_cost = session->total_input_tokens * mi->input_price / 1e6;
    double out_cost = session->total_output_tokens * mi->output_price / 1e6;
    double cr_cost = session->total_cache_read_tokens * mi->cache_read_price / 1e6;
    double cw_cost = session->total_cache_write_tokens * mi->cache_write_price / 1e6;
    double total = in_cost + out_cost + cr_cost + cw_cost;

    fprintf(stderr, "\n");
    tui_header("Session Cost", TUI_BCYAN);
    fprintf(stderr, "  %sModel:%s       %s\n", TUI_DIM, TUI_RESET, session->model);
    fprintf(stderr, "  %sTurns:%s       %d\n", TUI_DIM, TUI_RESET, session->turn_count);
    fprintf(stderr, "  %sInput:%s       %d tokens  ($%.4f)\n", TUI_DIM, TUI_RESET,
            session->total_input_tokens, in_cost);
    fprintf(stderr, "  %sOutput:%s      %d tokens  ($%.4f)\n", TUI_DIM, TUI_RESET,
            session->total_output_tokens, out_cost);
    if (session->total_cache_read_tokens > 0)
        fprintf(stderr, "  %sCache read:%s  %d tokens  ($%.4f)\n", TUI_DIM, TUI_RESET,
                session->total_cache_read_tokens, cr_cost);
    if (session->total_cache_write_tokens > 0)
        fprintf(stderr, "  %sCache write:%s %d tokens  ($%.4f)\n", TUI_DIM, TUI_RESET,
                session->total_cache_write_tokens, cw_cost);
    fprintf(stderr, "  %s%s──────────────────────%s\n", TUI_BOLD, TUI_CYAN, TUI_RESET);
    fprintf(stderr, "  %sTotal:%s       %s$%.4f%s\n\n", TUI_BOLD, TUI_RESET, TUI_BGREEN, total,
            TUI_RESET);
}

/* ── Context display ───────────────────────────────────────────────────── */

static void print_context(session_state_t *session, int last_input_tokens) {
    int ctx = session->context_window;
    if (ctx <= 0)
        ctx = CONTEXT_WINDOW_TOKENS;
    double pct = 100.0 * last_input_tokens / ctx;
    int bar_width = 40;
    int filled = (int)(bar_width * pct / 100.0);
    if (filled > bar_width)
        filled = bar_width;

    fprintf(stderr, "\n  %sContext:%s %d / %d tokens (%.1f%%)\n  [", TUI_DIM, TUI_RESET,
            last_input_tokens, ctx, pct);
    const char *color = pct < 50 ? TUI_GREEN : pct < 80 ? TUI_YELLOW : TUI_RED;
    for (int i = 0; i < bar_width; i++) {
        if (i < filled)
            fprintf(stderr, "%s\xe2\x96\x88%s", color, TUI_RESET);
        else
            fprintf(stderr, "%s\xe2\x96\x91%s", TUI_DIM, TUI_RESET);
    }
    fprintf(stderr, "]\n\n");
}

static void print_topology_summary(const topology_t *topo) {
    if (!topo)
        return;
    char ascii[4096];
    const char *strategy = "parallel";
    if (topo->strategy == EXEC_LINEAR)
        strategy = "linear";
    else if (topo->strategy == EXEC_PARALLEL_STAGES)
        strategy = "parallel_stages";
    else if (topo->strategy == EXEC_FULL_PARALLEL)
        strategy = "full_parallel";
    else if (topo->strategy == EXEC_ITERATIVE)
        strategy = "iterative";
    else if (topo->strategy == EXEC_TOURNAMENT)
        strategy = "tournament";
    else if (topo->strategy == EXEC_CONSENSUS)
        strategy = "consensus";
    topology_render_ascii(topo, ascii, sizeof(ascii));
    fprintf(stderr, "\n");
    tui_header("Topology", TUI_BYELLOW);
    fprintf(stderr, "  %sName:%s        %s\n", TUI_DIM, TUI_RESET, topo->name);
    fprintf(stderr, "  %sCategory:%s    %s\n", TUI_DIM, TUI_RESET,
            topo_category_label(topo->category));
    fprintf(stderr, "  %sStrategy:%s    %s\n", TUI_DIM, TUI_RESET, strategy);
    fprintf(stderr, "  %sAgents:%s      %d\n", TUI_DIM, TUI_RESET, topo->total_agents);
    fprintf(stderr, "  %sLatency:%s     %.1fx\n", TUI_DIM, TUI_RESET, topo->est_latency_mult);
    fprintf(stderr, "  %sRunnable:%s    %s\n", TUI_DIM, TUI_RESET,
            topology_is_runnable(topo) ? "yes" : "no");
    fprintf(stderr, "\n%s\n\n", ascii);
}

static void print_topology_registry_brief(void) {
    int count = 0;
    const topology_t *tops = topology_registry(&count);
    fprintf(stderr, "\n");
    tui_header("Topologies", TUI_BYELLOW);
    for (int i = 0; i < count; i++) {
        fprintf(stderr, "  %sT%02d%s %-18s %s%-11s%s agents=%-2d lat=%.1fx\n", TUI_CYAN, tops[i].id,
                TUI_RESET, tops[i].name, TUI_DIM, topo_category_label(tops[i].category), TUI_RESET,
                tops[i].total_agents, tops[i].est_latency_mult);
    }
    fprintf(stderr, "\n");
}

static void print_swarm_summary(int focus_group, bool verbose) {
    swarm_t *sw = tools_swarm_instance();
    if (!sw)
        return;

    swarm_poll(sw, 0);

    int total = sw->child_count;
    if (total == 0) {
        fprintf(stderr, "\n");
        tui_header("Swarm", TUI_BYELLOW);
        fprintf(stderr, "  %sno active or completed swarm agents in this session%s\n\n", TUI_DIM,
                TUI_RESET);
        return;
    }

    int running = swarm_active_count(sw);
    int done = 0;
    int errored = 0;
    int killed = 0;
    double total_cost = 0.0;
    for (int i = 0; i < sw->child_count; i++) {
        swarm_child_t *c = &sw->children[i];
        if (c->status == SWARM_DONE)
            done++;
        else if (c->status == SWARM_ERROR)
            errored++;
        else if (c->status == SWARM_KILLED)
            killed++;
        total_cost += c->est_cost_usd;
    }

    fprintf(stderr, "\n");
    tui_header("Swarm", TUI_BYELLOW);
    fprintf(stderr, "  %sAgents:%s      %d\n", TUI_DIM, TUI_RESET, total);
    fprintf(stderr, "  %sGroups:%s      %d\n", TUI_DIM, TUI_RESET, sw->group_count);
    fprintf(stderr, "  %sActive:%s      %d\n", TUI_DIM, TUI_RESET, running);
    fprintf(stderr, "  %sEst cost:%s    $%.4f\n", TUI_DIM, TUI_RESET, total_cost);
    tui_agent_rollup(total, done, running, errored + killed);

    if (sw->group_count > 0) {
        fprintf(stderr, "\n  %sGroups:%s\n", TUI_DIM, TUI_RESET);
        for (int i = 0; i < sw->group_count; i++) {
            swarm_group_t *g = &sw->groups[i];
            if (focus_group >= 0 && g->id != focus_group)
                continue;
            fprintf(stderr, "    %s#%d%s %-18s %s%d/%d done%s %serr=%d%s %s$%.4f%s\n", TUI_CYAN,
                    g->id, TUI_RESET, g->name, TUI_DIM, swarm_group_done_count(sw, i),
                    g->child_count, TUI_RESET,
                    swarm_group_error_count(sw, i) > 0 ? TUI_RED : TUI_DIM,
                    swarm_group_error_count(sw, i), TUI_RESET, TUI_BCYAN,
                    swarm_group_est_cost_usd(sw, i), TUI_RESET);
        }
    }

    if (verbose) {
        fprintf(stderr, "\n  %sAgents:%s\n", TUI_DIM, TUI_RESET);
        for (int i = 0; i < sw->child_count; i++) {
            swarm_child_t *c = &sw->children[i];
            if (focus_group >= 0 && c->group_id != focus_group)
                continue;
            fprintf(stderr, "    %s#%d%s %-10s %s[%s]%s %s%.48s%s %s%.1fs%s %s$%.4f%s\n", TUI_CYAN,
                    c->id, TUI_RESET, swarm_status_str(c->status), TUI_DIM,
                    c->model[0] ? c->model : "default", TUI_RESET, TUI_DIM, c->task, TUI_RESET,
                    TUI_DIM, swarm_child_elapsed_sec(c), TUI_RESET, TUI_BCYAN, c->est_cost_usd,
                    TUI_RESET);
        }
    }

    {
        tui_agent_node_t nodes[1 + SWARM_MAX_GROUPS + SWARM_MAX_CHILDREN];
        char labels[1 + SWARM_MAX_GROUPS + SWARM_MAX_CHILDREN][96];
        int node_count = 0;
        int group_node_ids[SWARM_MAX_GROUPS];
        memset(group_node_ids, -1, sizeof(group_node_ids));

        nodes[node_count].id = 1;
        nodes[node_count].parent_id = -1;
        labels[node_count][0] = '\0';
        snprintf(labels[node_count], sizeof(labels[node_count]), "root session");
        nodes[node_count].task = labels[node_count];
        nodes[node_count].status =
            running > 0 ? "running" : (errored + killed > 0 ? "error" : "done");
        node_count++;

        for (int i = 0; i < sw->group_count && node_count < (int)(sizeof(nodes) / sizeof(nodes[0]));
             i++) {
            swarm_group_t *g = &sw->groups[i];
            if (focus_group >= 0 && g->id != focus_group)
                continue;
            group_node_ids[i] = 100 + g->id;
            nodes[node_count].id = group_node_ids[i];
            nodes[node_count].parent_id = 1;
            snprintf(labels[node_count], sizeof(labels[node_count]), "group #%d %s", g->id,
                     g->name);
            nodes[node_count].task = labels[node_count];
            nodes[node_count].status = swarm_group_complete(sw, i) ? "done" : "running";
            node_count++;
        }

        for (int i = 0; i < sw->child_count && node_count < (int)(sizeof(nodes) / sizeof(nodes[0]));
             i++) {
            swarm_child_t *c = &sw->children[i];
            if (focus_group >= 0 && c->group_id != focus_group)
                continue;
            nodes[node_count].id = 1000 + c->id;
            nodes[node_count].parent_id = (c->group_id >= 0 && c->group_id < SWARM_MAX_GROUPS &&
                                           group_node_ids[c->group_id] > 0)
                                              ? group_node_ids[c->group_id]
                                              : 1;
            snprintf(labels[node_count], sizeof(labels[node_count]), "#%d %.64s", c->id, c->task);
            nodes[node_count].task = labels[node_count];
            nodes[node_count].status = swarm_status_str(c->status);
            node_count++;
        }

        tui_agent_topology(nodes, node_count);
    }

    {
        tui_swarm_entry_t entries[SWARM_MAX_CHILDREN];
        char previews[SWARM_MAX_CHILDREN][96];
        int entry_count = 0;
        for (int i = 0; i < sw->child_count && entry_count < SWARM_MAX_CHILDREN; i++) {
            swarm_child_t *c = &sw->children[i];
            if (focus_group >= 0 && c->group_id != focus_group)
                continue;
            entries[entry_count].id = c->id;
            entries[entry_count].task = c->task;
            entries[entry_count].status = swarm_status_str(c->status);
            entries[entry_count].progress =
                (c->status == SWARM_DONE || c->status == SWARM_ERROR || c->status == SWARM_KILLED)
                    ? 1.0
                    : 0.5;
            previews[entry_count][0] = '\0';
            if (c->output && c->output_len > 0) {
                const char *tail = c->output;
                size_t tail_len = c->output_len;
                if (tail_len > 88) {
                    tail += tail_len - 88;
                }
                snprintf(previews[entry_count], sizeof(previews[entry_count]), "%.88s", tail);
            }
            entries[entry_count].last_output = previews[entry_count];
            entry_count++;
        }
        if (entry_count > 0) {
            tui_swarm_panel(entries, entry_count, 72);
        }
    }

    {
        tui_swarm_cost_entry_t entries[SWARM_MAX_CHILDREN];
        char names[SWARM_MAX_CHILDREN][32];
        int entry_count = 0;
        double shown_total = 0.0;
        for (int i = 0; i < sw->child_count && entry_count < SWARM_MAX_CHILDREN; i++) {
            swarm_child_t *c = &sw->children[i];
            if (focus_group >= 0 && c->group_id != focus_group)
                continue;
            snprintf(names[entry_count], sizeof(names[entry_count]), "#%d %.24s", c->id, c->task);
            entries[entry_count].name = names[entry_count];
            entries[entry_count].cost = c->est_cost_usd;
            entries[entry_count].in_tok = c->est_input_tokens;
            entries[entry_count].out_tok = c->est_output_tokens;
            shown_total += c->est_cost_usd;
            entry_count++;
        }
        tui_swarm_cost(entries, entry_count, shown_total);
    }

    fprintf(stderr, "\n");
}

static bool run_topology_prompt(session_state_t *session, const char *api_key, conversation_t *conv,
                                const char *prompt, int *last_input_tokens) {
    topology_plan_t plan;
    const char *preferred = session->active_topology[0] ? session->active_topology : NULL;
    if (!topology_plan_build(preferred, session->topology_auto, prompt, &plan))
        return false;

    char *out = safe_malloc(MAX_RESPONSE_SIZE);
    topology_run_stats_t stats;
    bool ok =
        topology_plan_run(&plan, api_key, session->model, prompt, out, MAX_RESPONSE_SIZE, &stats);

    conv_add_user_text(conv, prompt);
    md_reset(&s_md);
    md_feed_str(&s_md, out);
    md_flush(&s_md);
    fprintf(stderr, "\n");
    conv_add_assistant_text(conv, out);
    session->turn_count++;
    if (last_input_tokens)
        *last_input_tokens = (int)strlen(prompt) / 4;

    fprintf(stderr, "  %stopology:%s %s", TUI_DIM, TUI_RESET, plan.topology.name);
    if (plan.is_dynamic) {
        fprintf(stderr, " %s(dynamic)%s", TUI_DIM, TUI_RESET);
    } else if (session->topology_auto && !session->active_topology[0]) {
        fprintf(stderr, " %s(auto-static)%s", TUI_DIM, TUI_RESET);
    }
    fprintf(stderr, "  %sagents:%s %d  %siterations:%s %d  %sest:$%.4f%s\n", TUI_DIM, TUI_RESET,
            stats.agents_spawned, TUI_DIM, TUI_RESET, stats.iterations, TUI_BCYAN,
            stats.est_cost_usd, TUI_RESET);
    if (plan.rationale[0]) {
        fprintf(stderr, "  %splan:%s %s\n", TUI_DIM, TUI_RESET, plan.rationale);
    }

    baseline_log(ok ? "swarm" : "swarm_error", ok ? "topology_prompt" : "topology_prompt_failed",
                 plan.topology.name, NULL);
    free(out);
    return true;
}

/* ── Slash-command table ───────────────────────────────────────────────────
 * Single source of truth for slash commands. Drives both readline tab
 * completion (below, under HAVE_READLINE) and the composer's live dropdown
 * (registered with the TUI via tui_composer_set_slash_commands), so it is
 * compiled unconditionally. Layout matches tui_cmd_entry_t {name, desc}. */
typedef struct {
    const char *command;
    const char *description;
} slash_command_t;

static const slash_command_t s_slash_commands[] = {
    {"/clear", "reset conversation"},
    {"/slot", "list workspace slots"},
    {"/slot new", "/slot new <name> [model]  — create slot with optional model"},
    {"/slot <name>", "switch to named slot (saves current first)"},
    {"/slot del", "/slot del <name>  — delete a slot"},
    {"/save", "save session"},
    {"/load", "load session"},
    {"/sessions", "list saved sessions with model/title metadata"},
    {"/resume", "resume by index, name, or title search"},
    {"/new", "start a fresh named session"},
    {"/rename", "rename the current session"},
    {"/setup", "detect and store API keys"},
    {"/setup --force", "store keys and overwrite existing values"},
    {"/setup report", "show environment setup status"},
    {"/tools", "list available tools"},
    {"/plugins", "list loaded plugins"},
    {"/plugins validate", "validate plugin manifest + lockfile"},
    {"/help", "show slash command help"},
    {"/model", "show or change active model"},
    {"/models", "list locally-served models (ollama/lmstudio/mlx)"},
    {"/login", "sign in with ChatGPT subscription (/login [chatgpt|claude|status])"},
    {"/logout", "clear the ChatGPT subscription token cache"},
    {"/route", "show/update model routing policy"},
    {"/cost", "show session cost"},
    {"/context", "show context usage"},
    {"/effort", "set effort: low/medium/high"},
    {"/compact", "trim conversation history"},
    {"/undo", "remove last exchange from conversation"},
    {"/retry", "re-run last user message (optionally with new model)"},
    {"/diff", "show git diff (--staged for index)"},
    {"/note", "add an annotation to the conversation context"},
    {"/dialog", "open an interactive question dialog; answers feed the next turn"},
    {"/add-dir", "inject a directory listing into context"},
    {"/version", "show version/build information"},
    {"/force", "control next tool choice"},
    {"/prefill", "set prefill text for the next response"},
    {"/json", "set JSON mode for next response"},
    {"/cheap", "switch to cheap mode (5 core tools, no catalog)"},
    {"/full", "switch to full mode (all tools + catalog)"},
    {"/budget", "set session cost budget"},
    {"/exec", "run prompt via external CLI (claude, codex, list)"},
    {"/claude", "shorthand for /exec claude <prompt>"},
    {"/codex", "shorthand for /exec codex <prompt>"},
    {"/trust", "set trust tier"},
    {"/web", "toggle web search"},
    {"/code", "toggle code execution"},
    {"/image", "attach image — type @ in composer or paste from clipboard (Alt+I)"},
    {"/mcp", "show MCP servers and tools"},
    {"/mcp reload", "reload MCP servers/tools"},
    {"/provider", "show/detect API provider"},
    {"/status", "show full session status"},
    {"/dht", "distributed peer discovery (Kademlia) over the mesh"},
    {"/dht start", "/dht start [swarm-key]  — join the private DHT overlay"},
    {"/dht find", "force a peer search/announce now"},
    {"/dht boot", "/dht boot <host:port>  — add a bootstrap node"},
    {"/dht stop", "leave the DHT overlay"},
    {"/temp", "set request temperature"},
    {"/thinking", "set thinking budget"},
    {"/fallback", "set model fallback chain"},
    {"/metrics", "show per-tool metrics"},
    {"/telemetry", "show streaming telemetry"},
    {"/cache", "show/clear tool cache"},
    {"/trace", "show recent trace spans"},
    {"/topology", "show/select/run topologies"},
    {"/topology list", "show topology registry"},
    {"/topology show", "inspect a topology"},
    {"/topology use", "set active topology"},
    {"/topology run", "run one-off topology"},
    {"/topology auto", "enable topology auto-selection"},
    {"/topology off", "disable topology auto-selection"},
    {"/swarm", "view / manage swarm"},
    {"/swarm status", "show swarm status"},
    {"/swarm show", "show a specific swarm group"},
    {"/swarm wait", "wait for swarm group completion"},
    {"/swarm kill", "kill a swarm agent"},
    {"/swarm kill-group", "kill a whole swarm group"},
    {"/features", "toggle UI features"},
    {"/perf", "show latency and throughput"},
    {"/minimap", "show conversation minimap"},
    {"/workspace", "show workspace summary"},
    {"/workspace bootstrap", "create workspace files"},
    {"/workspace reload", "reload workspace prompt cache"},
    {"/workspace prompt", "show active workspace prompt"},
    {"/skills", "list active skills"},
    {"/skills show", "show a skill body"},
    {"/skills use", "set active skill"},
    {"/skills clear", "clear active skill"},
    {"/identity", "show workspace identity doc"},
    {"/user", "show workspace user doc"},
    {"/soul", "show workspace soul doc"},
    {"/memory", "show workspace memory doc"},
    {"/dashboard", "show rich session dashboard"},
    {"/top", "show tool leaderboard"},
    {"/flame", "show tool flame timeline"},
    {"/branch", "save conversation as a named branch checkpoint"},
    {"/pin", "set persistent context injected at every turn"},
    {"/unpin", "clear pinned context"},
    {"/git", "run a git command and display output"},
    {"/ask", "one-shot query to a specific model without switching"},
    {"/alias", "define or list command shortcuts"},
    {"/agents", "list agent profiles"},
    {"/agents use", "activate an agent profile"},
    {"/agents off", "deactivate active agent profile"},
    {"/agents show", "show agent profile details"},
    {"/agents new", "create a new agent profile"},
    {"/agents edit", "edit an existing agent profile"},
    {"/agents delete", "delete an agent profile"},
    {"/agents groups", "list available tool groups"},
    {"quit", "exit dsco"},
    {"exit", "exit dsco"},
    {"/exit", "exit dsco"},
    {"/quit", "exit dsco"},
    {NULL, NULL}};

/* Number of real entries in s_slash_commands (excludes the NULL terminator). */
static int slash_commands_count(void) {
    int n = 0;
    while (s_slash_commands[n].command)
        n++;
    return n;
}

/* ── Readline tab completion ───────────────────────────────────────────── */

#ifdef HAVE_READLINE
static const char *command_description(const char *command) {
    for (size_t i = 0; s_slash_commands[i].command; i++) {
        if (strcmp(s_slash_commands[i].command, command) == 0) {
            return s_slash_commands[i].description;
        }
    }
    return "";
}

/* Helper: build a completed "/<cmd> <arg>" string */
static char *make_arg_completion(const char *prefix, const char *arg) {
    size_t n = strlen(prefix) + 1 + strlen(arg) + 1;
    char *r = malloc(n);
    if (!r)
        return NULL;
    snprintf(r, n, "%s %s", prefix, arg);
    return r;
}

/* ── /dialog: chat-flow front-end for the AskUserQuestion engine ──────────
 * Composes the same dialog used by the AskUserQuestion tool. Accepts either a
 * raw JSON spec (starts with '{') or a terse shorthand:
 *     Question text? | opt1 | opt2 [ ;; Next question? | a | b ]
 * Questions are split on ";;", options on "|". */

static void dlg_trim(char *s) {
    if (!s)
        return;
    char *p = s;
    while (*p == ' ' || *p == '\t')
        p++;
    if (p != s)
        memmove(s, p, strlen(p) + 1);
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\n' || s[n - 1] == '\r'))
        s[--n] = '\0';
}

static char *dialog_shorthand_to_json(const char *sh) {
    jbuf_t b;
    jbuf_init(&b, 512);
    jbuf_append(&b, "{\"questions\":[");
    char *work = safe_strdup(sh ? sh : "");
    int qn = 0;
    char *seg = work;
    while (seg && *seg) {
        char *next = strstr(seg, ";;");
        if (next)
            *next = '\0';
        char *q = seg;
        dlg_trim(q);
        if (*q) {
            char *bar = strchr(q, '|');
            char qtext[1024];
            if (bar) {
                size_t L = (size_t)(bar - q);
                if (L >= sizeof qtext)
                    L = sizeof qtext - 1;
                memcpy(qtext, q, L);
                qtext[L] = '\0';
            } else {
                snprintf(qtext, sizeof qtext, "%s", q);
            }
            dlg_trim(qtext);
            if (qn)
                jbuf_append(&b, ",");
            char idbuf[16];
            snprintf(idbuf, sizeof idbuf, "q%d", qn + 1);
            char hdr[32];
            snprintf(hdr, sizeof hdr, "%.14s", qtext[0] ? qtext : idbuf);
            jbuf_append(&b, "{\"id\":");
            jbuf_append_json_str(&b, idbuf);
            jbuf_append(&b, ",\"header\":");
            jbuf_append_json_str(&b, hdr);
            jbuf_append(&b, ",\"question\":");
            jbuf_append_json_str(&b, qtext);
            jbuf_append(&b, ",\"options\":[");
            int on = 0;
            char *optseg = bar ? bar + 1 : NULL;
            while (optseg && *optseg) {
                char *ob = strchr(optseg, '|');
                if (ob)
                    *ob = '\0';
                char opt[256];
                snprintf(opt, sizeof opt, "%s", optseg);
                dlg_trim(opt);
                if (opt[0]) {
                    if (on)
                        jbuf_append(&b, ",");
                    jbuf_append(&b, "{\"label\":");
                    jbuf_append_json_str(&b, opt);
                    jbuf_append(&b, "}");
                    on++;
                }
                optseg = ob ? ob + 1 : NULL;
            }
            jbuf_append(&b, "]}");
            qn++;
        }
        seg = next ? next + 2 : NULL;
    }
    jbuf_append(&b, "]}");
    free(work);
    char *r = safe_strdup(b.data ? b.data : "{}");
    jbuf_free(&b);
    return r;
}

static void dialog_answers_fmt_cb(const char *el, void *ctx) {
    jbuf_t *b = (jbuf_t *)ctx;
    char *hdr = json_get_str(el, "header");
    char *q = json_get_str(el, "question");
    char *val = json_get_str(el, "value");
    bool answered = json_get_bool(el, "answered", false);
    if (answered && val && val[0])
        jbuf_appendf(b, "\n- %s: %s", (hdr && hdr[0]) ? hdr : (q ? q : "?"), val);
    free(hdr);
    free(q);
    free(val);
}

/* Render the dialog result JSON into a human-readable turn message. Returns
 * false if the user cancelled (nothing to inject). */
static bool dialog_answers_to_text(const char *res, char *out, size_t out_len) {
    char *status = json_get_str(res, "status");
    bool cancelled = status && strcmp(status, "cancel") == 0;
    bool is_chat = status && strcmp(status, "chat") == 0;
    if (cancelled) {
        free(status);
        return false;
    }

    jbuf_t b;
    jbuf_init(&b, 256);
    if (is_chat) {
        char *chat = json_get_str(res, "chat");
        jbuf_appendf(&b, "Let's talk through this%s%s", (chat && chat[0]) ? ": " : ".",
                     (chat && chat[0]) ? chat : "");
        free(chat);
    } else {
        jbuf_append(&b, "Here are my answers:");
        json_array_foreach(res, "answers", dialog_answers_fmt_cb, &b);
    }
    snprintf(out, out_len, "%s", b.data ? b.data : "");
    jbuf_free(&b);
    free(status);
    return true;
}

static char *command_generator(const char *text, int state) {
    /* mode: 0=slash commands, 1=/model, 2=/effort, 3=/trust,
             4=/exec, 5=/temp, 6=/thinking, 7=/route */
    static int idx, len, model_idx, arg_idx, mode;

    static const char *effort_args[] = {"low", "medium", "high", NULL};
    static const char *trust_args[] = {"trusted", "standard", "untrusted", NULL};
    static const char *exec_args[] = {"claude", "codex", "list", NULL};
    static const char *route_args[] = {"status",    "policy", "budget", "history",
                                       "recommend", "lock",   "unlock", NULL};

    if (!state) {
        idx = 0;
        model_idx = 0;
        arg_idx = 0;
        len = (int)strlen(text);
        if (strncmp(text, "/model ", 7) == 0)
            mode = 1;
        else if (strncmp(text, "/effort ", 8) == 0)
            mode = 2;
        else if (strncmp(text, "/trust ", 7) == 0)
            mode = 3;
        else if (strncmp(text, "/exec ", 6) == 0)
            mode = 4;
        else if (strncmp(text, "/temp ", 6) == 0)
            mode = 5;
        else if (strncmp(text, "/thinking ", 10) == 0)
            mode = 6;
        else if (strncmp(text, "/route ", 7) == 0)
            mode = 7;
        else if (strncmp(text, "/agents use ", 12) == 0)
            mode = 8;
        else if (strncmp(text, "/agents show ", 13) == 0)
            mode = 8;
        else if (strncmp(text, "/agents edit ", 13) == 0)
            mode = 8;
        else if (strncmp(text, "/agents delete ", 15) == 0)
            mode = 8;
        else
            mode = 0;
    }

    /* Argument-completion modes */
    const char **arg_table = NULL;
    const char *arg_prefix = NULL;
    int prefix_len = 0;
    if (mode == 2) {
        arg_table = effort_args;
        arg_prefix = "/effort";
        prefix_len = 8;
    }
    if (mode == 3) {
        arg_table = trust_args;
        arg_prefix = "/trust";
        prefix_len = 7;
    }
    if (mode == 4) {
        arg_table = exec_args;
        arg_prefix = "/exec";
        prefix_len = 6;
    }
    if (mode == 7) {
        arg_table = route_args;
        arg_prefix = "/route";
        prefix_len = 7;
    }

    if (arg_table) {
        const char *partial = text + prefix_len;
        while (*partial == ' ')
            partial++;
        int p_len = (int)strlen(partial);
        while (arg_table[arg_idx]) {
            const char *a = arg_table[arg_idx++];
            if (strncmp(a, partial, (size_t)p_len) == 0)
                return make_arg_completion(arg_prefix, a);
        }
        return NULL;
    }

    if (mode == 1) {
        const char *partial = text + 7;
        while (*partial == ' ')
            partial++;
        int p_len = (int)strlen(partial);
        while (MODEL_REGISTRY[model_idx].alias) {
            const char *alias = MODEL_REGISTRY[model_idx].alias;
            model_idx++;
            if (strncmp(alias, partial, (size_t)p_len) == 0)
                return make_arg_completion("/model", alias);
        }
        return NULL;
    }

    /* modes 5, 6: no fixed arg list — fall through to no completion */
    if (mode == 5 || mode == 6)
        return NULL;

    /* mode 8: complete with agent profile names */
    if (mode == 8) {
        /* Find the last space to get the partial profile name */
        const char *last_sp = strrchr(text, ' ');
        const char *partial = last_sp ? last_sp + 1 : text;
        int p_len = (int)strlen(partial);
        int n = agent_profiles_count();
        while (arg_idx < n) {
            const agent_profile_t *p = agent_profile_get(arg_idx++);
            if (!p)
                continue;
            if (strncmp(p->name, partial, (size_t)p_len) == 0) {
                /* Build "/<cmd> <profile>" */
                const char *prefix = text;
                size_t cmd_len = (size_t)(last_sp ? last_sp - text + 1 : 0);
                size_t total = cmd_len + strlen(p->name) + 1;
                char *r = malloc(total);
                if (!r)
                    return NULL;
                memcpy(r, prefix, cmd_len);
                snprintf(r + cmd_len, sizeof(r) - cmd_len, "%s", p->name);
                return r;
            }
        }
        return NULL;
    }

    while (s_slash_commands[idx].command) {
        const char *cmd = s_slash_commands[idx++].command;
        if (strncmp(cmd, text, (size_t)len) == 0)
            return strdup(cmd);
    }
    return NULL;
}

static void command_completion_display(char **matches, int num_matches, int max_length) {
    (void)max_length;
    if (!matches || num_matches <= 0)
        return;

    int max_cmd = 0;
    for (int i = 0; i < num_matches; i++) {
        int cmd_len = (int)strlen(matches[i]);
        if (cmd_len > max_cmd)
            max_cmd = cmd_len;
    }

    fprintf(rl_outstream, "\n");
    for (int i = 0; i < num_matches; i++) {
        const char *cmd = matches[i];
        const char *desc = command_description(cmd);
        if (!desc[0] && strncmp(cmd, "/model ", 7) == 0) {
            desc = "set model alias";
        }
        fprintf(rl_outstream, "  %-*s", max_cmd, cmd);
        if (desc && desc[0])
            fprintf(rl_outstream, "  %s", desc);
        fprintf(rl_outstream, "\n");
    }
    rl_forced_update_display();
}

static int slash_completion_key(int count, int key) {
    (void)count;
    (void)key;

    rl_insert_text("/");
    if (rl_point == 1)
        rl_complete(0, '\t');
    return 0;
}

static char **command_completion(const char *text, int start, int end) {
    (void)start;
    (void)end;
    rl_attempted_completion_over = 1;
    return rl_completion_matches(text, command_generator);
}

static int dsco_clear_screen(int count, int key) {
    (void)count;
    (void)key;
    fprintf(rl_outstream, "\033[2J\033[H");
    rl_forced_update_display();
    return 0;
}
#endif

/* ── Parallel tool execution ───────────────────────────────────────────── */

/* ── Streaming callbacks ───────────────────────────────────────────────── */

/* Boolean flags removed — FSM state callbacks now drive all transitions.
 * Query s_streaming_fsm.current == TUI_STREAM_ST_TEXT etc. instead. */
/* ── 40 Features: static state ──────────────────────────────────────────── */
static tui_features_t g_features;
static tui_cadence_t s_cadence;

static void cadence_flush_to_md(const char *buf, int len, void *ctx) {
    (void)len;
    md_feed_str((md_renderer_t *)ctx, buf);
}

static tui_word_counter_t s_word_counter;
static tui_thinking_state_t s_thinking;
static tui_flame_t s_flame;
static tui_dag_t s_dag;
static tui_branch_t s_branch;
static tui_citation_t s_citations;
static tui_throughput_t s_throughput;
static tui_ghost_t s_ghost;
static tui_latency_breakdown_t s_last_latency;
static tui_stream_heartbeat_t s_heartbeat;
static void terminal_input_echo_suspend(void);
static void terminal_input_echo_restore(void);
static struct termios s_saved_termios;
static bool s_saved_termios_valid = false;
static bool s_input_echo_suspended = false;

static void terminal_input_echo_suspend(void) {
    if (s_input_echo_suspended || !isatty(STDIN_FILENO))
        return;

    struct termios tio;
    if (tcgetattr(STDIN_FILENO, &tio) != 0)
        return;
    s_saved_termios = tio;
    s_saved_termios_valid = true;
    /* Disable echo and canonical mode so the ESC poller can read individual
       keystrokes (e.g. ESC = 0x1b) without waiting for Enter. */
    tio.c_lflag &= (tcflag_t) ~(ECHO | ECHONL | ICANON);
    tio.c_cc[VMIN] = 0; /* non-blocking reads */
    tio.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &tio) == 0) {
        s_input_echo_suspended = true;
    }
}

static void terminal_input_echo_restore(void) {
    if (!s_input_echo_suspended || !isatty(STDIN_FILENO))
        return;
    if (s_saved_termios_valid) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &s_saved_termios);
    }
    s_input_echo_suspended = false;
}

/* ── ESC key poller thread ─────────────────────────────────────────────── */
/* Runs during LLM streaming (stdin is in raw non-blocking mode).
   Detects a standalone ESC (0x1b) and triggers the pause state machine.
   Escape sequences (arrow keys etc.) are distinguished by a follow-up byte
   arriving within 20 ms; if one does, the sequence is drained and ignored. */

static void *esc_poll_thread_fn(void *arg) {
    (void)arg;
    while (g_esc_poller_active) {
        fd_set rfd;
        struct timeval tv = {0, 30000}; /* 30 ms poll */
        FD_ZERO(&rfd);
        FD_SET(STDIN_FILENO, &rfd);
        if (select(STDIN_FILENO + 1, &rfd, NULL, NULL, &tv) <= 0)
            continue;

        char c = 0;
        if (read(STDIN_FILENO, &c, 1) != 1 || c != '\033')
            continue;

        /* Wait 20 ms for potential escape-sequence follow-up */
        struct timeval tv2 = {0, 20000};
        fd_set rfd2;
        FD_ZERO(&rfd2);
        FD_SET(STDIN_FILENO, &rfd2);
        if (select(STDIN_FILENO + 1, &rfd2, NULL, NULL, &tv2) > 0) {
            char buf[8];
            (void)read(STDIN_FILENO, buf, sizeof(buf)); /* drain and ignore */
            continue;
        }

        /* Standalone ESC */
        if (g_escape_state == ESC_RUNNING && !g_interrupted) {
            g_escape_state = ESC_PAUSED;
            g_interrupted = 1;
        } else if (g_escape_state == ESC_PAUSED) {
            /* Second ESC → hard exit */
            const char msg[] = "\n\033[r\033[?2004l\033[0m\033[?25h\n";
            (void)write(STDERR_FILENO, msg, sizeof(msg) - 1);
            _exit(130);
        }
    }
    return NULL;
}

static void esc_poller_start(void) {
    if (g_esc_poller_active)
        return;
    g_esc_poller_active = 1;
    pthread_create(&g_esc_poller_tid, NULL, esc_poll_thread_fn, NULL);
}

static void esc_poller_stop(void) {
    if (!g_esc_poller_active)
        return;
    g_esc_poller_active = 0;
    pthread_join(g_esc_poller_tid, NULL);
}

/* ── Pause menu ────────────────────────────────────────────────────────── */
/* Called when the turns loop exits due to ESC_PAUSED.
   Returns true if the user chose to resume, false to cancel. */

static bool show_pause_menu(int cur_turn, int max_turns, double elapsed_sec) {
    struct termios tio_saved, tio_raw;
    bool tio_ok = (isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &tio_saved) == 0);
    if (tio_ok) {
        tio_raw = tio_saved;
        tio_raw.c_lflag &= (tcflag_t) ~(ECHO | ECHONL | ICANON);
        tio_raw.c_cc[VMIN] = 1;
        tio_raw.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &tio_raw);
    }

    /* Route pause menu through output queue to prevent races */
    if (g_outq)
        tui_outq_flush_sync(g_outq); /* drain pending output first */
    if (max_turns >= 999999) {
        fprintf(stderr, "\n  %s⏸  paused%s  (turn %d/∞ · %.0fs elapsed)\n", TUI_YELLOW, TUI_RESET,
                cur_turn, elapsed_sec);
    } else {
        int max_t = max_turns > 0 ? max_turns : 1;
        fprintf(stderr, "\n  %s⏸  paused%s  (turn %d/%d · %.0fs elapsed)\n", TUI_YELLOW, TUI_RESET,
                cur_turn, max_t, elapsed_sec);
    }
    fprintf(stderr,
            "  %s[R]%s Resume  %s[C]%s Cancel  "
            "%s(Esc again = cancel)%s  ▸ ",
            TUI_BOLD, TUI_RESET, TUI_BOLD, TUI_RESET, TUI_DIM, TUI_RESET);
    fflush(stderr);

    bool resume = false;
    for (;;) {
        char key = 0;
        if (read(STDIN_FILENO, &key, 1) != 1)
            break;
        if (key == 'r' || key == 'R') {
            resume = true;
            break;
        }
        if (key == 'c' || key == 'C' || key == '\033' || key == 'q' || key == 'Q' ||
            key == '\003' /* Ctrl+C */) {
            resume = false;
            break;
        }
        fprintf(stderr,
                "\r  %s[R]%s Resume  %s[C]%s Cancel  "
                "%s(Esc again = cancel)%s  ▸ ",
                TUI_BOLD, TUI_RESET, TUI_BOLD, TUI_RESET, TUI_DIM, TUI_RESET);
        fflush(stderr);
    }

    if (tio_ok)
        tcsetattr(STDIN_FILENO, TCSANOW, &tio_saved);
    fprintf(stderr, "\n");
    return resume;
}

/* Forward declaration for FSM access */
static tui_fsm_t s_streaming_fsm;

/* Forward declaration: defined further down, but fsm_text_enter (above
 * the definition) calls it to print the `▌ assistant` chat header. */
static void print_role_header(const char *role, bool ok, const char *trail);

/* ── FSM state callbacks ─────────────────────────────────────────────── */
/* These fire automatically on state enter/exit via tui_fsm_send().
 * They replace the manual boolean-flag rendering scattered through the
 * old streaming callbacks.  Each callback acquires tui_term_lock when
 * it writes to stderr so it serializes with the render thread. */

static void fsm_thinking_enter(void *ctx) {
    (void)ctx;
    tui_thinking_init(&s_thinking);
    if (!g_features.collapsible_thinking) {
        tui_term_lock();
        fprintf(stderr, "  \033[2m\033[3m[thinking]\n");
        fflush(stderr);
        tui_term_unlock();
    }
}

static void fsm_thinking_exit(void *ctx) {
    (void)ctx;
    tui_term_lock();
    if (g_features.collapsible_thinking) {
        tui_thinking_end(&s_thinking);
    } else {
        fprintf(stderr, "\033[0m\n");
        fflush(stderr);
    }
    tui_term_unlock();
}

static void fsm_text_enter(void *ctx) {
    (void)ctx;
    tui_word_counter_init(&s_word_counter);
    tui_term_lock();
    print_role_header("assistant", true, NULL);
    fputs("  ", stderr);
    fflush(stderr);
    tui_term_unlock();
}

static void fsm_text_exit(void *ctx) {
    (void)ctx;
    tui_term_lock();
    md_flush(&s_md);
    fprintf(stderr, "\n");
    fflush(stderr);
    tui_term_unlock();
    tui_word_counter_end(&s_word_counter);
}

static void fsm_tool_pending_enter(void *ctx) {
    (void)ctx;
    tui_transition_divider();
}

static void on_stream_text(const char *text, void *ctx) {
    (void)ctx;
    tui_stream_heartbeat_poke(&s_heartbeat, NULL);

    /* FSM drives all transitions — thinking_exit and text_enter fire
     * automatically if we were in thinking or idle. */
    int cur = s_streaming_fsm.current;
    if (cur == TUI_STREAM_ST_THINKING)
        tui_fsm_send(&s_streaming_fsm, TUI_FSM_EVT_THINKING_END);
    if (s_streaming_fsm.current != TUI_STREAM_ST_TEXT)
        tui_fsm_send(&s_streaming_fsm, TUI_FSM_EVT_TEXT_START);

    s_turn_streamed_text = true;

    /* F2: Typing cadence — buffer and flush at steady rate */
    tui_term_lock();
    if (g_features.typing_cadence) {
        tui_cadence_feed(&s_cadence, text);
    } else {
        md_feed_str(&s_md, text);
    }
    fflush(stderr);
    tui_term_unlock();

    /* F5: Live word count */
    tui_word_counter_feed(&s_word_counter, text);
    tui_word_counter_render(&s_word_counter);
    /* F39: Throughput tracking */
    tui_throughput_tick(&s_throughput, tui_estimate_tokens(text));
}

static void on_stream_thinking(const char *text, void *ctx) {
    (void)ctx;
    tui_stream_heartbeat_poke(&s_heartbeat, "thinking...");

    /* FSM: thinking_enter fires on first chunk (prints header, inits state) */
    if (s_streaming_fsm.current != TUI_STREAM_ST_THINKING)
        tui_fsm_send(&s_streaming_fsm, TUI_FSM_EVT_THINKING_START);

    /* F4: Collapsible thinking — count silently instead of printing */
    tui_term_lock();
    if (g_features.collapsible_thinking) {
        tui_thinking_feed(&s_thinking, text);
    } else {
        fprintf(stderr, " %s", text);
        fflush(stderr);
    }
    tui_term_unlock();
}

static void on_stream_tool_start(const char *name, const char *id, void *ctx) {
    (void)ctx;
    tui_stream_heartbeat_poke(&s_heartbeat, NULL);

    /* FSM: TOOL_START from any state — thinking_exit / text_exit / divider
     * all fire automatically via the state callbacks. */
    tui_fsm_send(&s_streaming_fsm, TUI_FSM_EVT_TOOL_START);

    /* F7: Citation footnotes — assign footnote number */
    if (g_features.citation_footnotes) {
        int fn = tui_citation_add(&s_citations, name, id, NULL, 0);
        if (g_outq) {
            tui_outq_writef(g_outq, "  %s[%d]%s ", TUI_DIM, fn, TUI_RESET);
        } else {
            fprintf(stderr, "  %s[%d]%s ", TUI_DIM, fn, TUI_RESET);
        }
    }
    baseline_log("tool", name, "tool_use started", NULL);
}

/* Extract tool args into a single-line preview string */
static void extract_tool_preview(const char *name, const char *input_json, char *out,
                                 size_t out_sz) {
    (void)name;
    out[0] = '\0';
    if (!input_json || strcmp(input_json, "{}") == 0)
        return;

    /* Try to extract key values from JSON for common tools */
    char *cmd = json_get_str(input_json, "command");
    char *code = json_get_str(input_json, "code");
    char *query = json_get_str(input_json, "query");
    char *path = json_get_str(input_json, "file_path");
    char *url = json_get_str(input_json, "url");
    char *pattern = json_get_str(input_json, "pattern");
    char *expr = json_get_str(input_json, "expression");

    int max = (int)out_sz - 1;
    if (max > 200)
        max = 200;

    if (cmd) {
        snprintf(out, out_sz, "$ %.*s", max - 2, cmd);
    } else if (code) {
        const char *nl = strchr(code, '\n');
        int len = nl ? (int)(nl - code) : (int)strlen(code);
        if (len > max)
            len = max;
        snprintf(out, out_sz, "%.*s%s", len, code, nl ? " ..." : "");
    } else if (path && pattern) {
        snprintf(out, out_sz, "%s ~ /%s/", path, pattern);
    } else if (path) {
        snprintf(out, out_sz, "%s", path);
    } else if (query) {
        snprintf(out, out_sz, "%.*s", max, query);
    } else if (url) {
        snprintf(out, out_sz, "%.*s", max, url);
    } else if (pattern) {
        snprintf(out, out_sz, "/%.*s/", max - 2, pattern);
    } else if (expr) {
        snprintf(out, out_sz, "%.*s", max, expr);
    } else {
        const char *start = input_json;
        if (*start == '{')
            start++;
        int len = (int)strlen(start);
        if (len > 0 && start[len - 1] == '}')
            len--;
        if (len > max)
            len = max;
        if (len > 0)
            snprintf(out, out_sz, "%.*s", len, start);
    }

    free(cmd);
    free(code);
    free(query);
    free(path);
    free(url);
    free(pattern);
    free(expr);

    /* Replace newlines with spaces for single-line display */
    for (char *c = out; *c; c++) {
        if (*c == '\n' || *c == '\r')
            *c = ' ';
    }
}

/* Print a role header (`▌ role`) in the chat-log style.
 *   role  — one of "user" / "assistant" / "tool_call" / "tool_response"
 *   ok    — only meaningful for tool_response; selects green vs red bar
 *   trail — extra text appended on the header line after the role (may be NULL) */
static void print_role_header(const char *role, bool ok, const char *trail) {
    bool rgb = tui_detect_color_level() >= TUI_COLOR_256;
    /* Bar color per role: cyan user, mauve assistant, orange tool_call,
     * green/red tool_response based on `ok`. */
    int br = 255, bg = 255, bb = 255;
    const char *fb = TUI_BWHITE;
    if (strcmp(role, "user") == 0) {
        br = 100;
        bg = 200;
        bb = 255;
        fb = TUI_BCYAN;
    } else if (strcmp(role, "assistant") == 0) {
        br = 200;
        bg = 170;
        bb = 255;
        fb = TUI_BMAGENTA;
    } else if (strcmp(role, "tool_call") == 0) {
        br = 255;
        bg = 150;
        bb = 60;
        fb = TUI_BYELLOW;
    } else if (strcmp(role, "tool_response") == 0) {
        if (ok) {
            br = 80;
            bg = 220;
            bb = 120;
            fb = TUI_GREEN;
        } else {
            br = 255;
            bg = 80;
            bb = 80;
            fb = TUI_RED;
        }
    }

    if (rgb) {
        fprintf(stderr, "\n  \033[38;2;%d;%d;%dm▌\033[0m \033[1m%s\033[0m", br, bg, bb, role);
    } else {
        fprintf(stderr, "\n  %s▌%s %s%s%s", fb, TUI_RESET, TUI_BOLD, role, TUI_RESET);
    }
    if (trail && trail[0])
        fprintf(stderr, "%s %s%s", TUI_DIM, trail, TUI_RESET);
    fprintf(stderr, "\n");
    fflush(stderr);
}

/* Print `▌ tool_call  name(args)` block. */
static void print_tool_start_line(const char *name, const char *input_json) {
    char preview[200];
    extract_tool_preview(name, input_json, preview, sizeof(preview));

    char trail[256];
    if (preview[0])
        snprintf(trail, sizeof(trail), "%s(%s)", name, preview);
    else
        snprintf(trail, sizeof(trail), "%s", name);
    print_role_header("tool_call", true, trail);
}

static void print_tool_result_ex(const char *name, bool ok, const char *result, double elapsed_ms) {
    char elapsed_str[32] = "";
    if (elapsed_ms > 0) {
        if (elapsed_ms < 1000.0)
            snprintf(elapsed_str, sizeof(elapsed_str), "%.0fms", elapsed_ms);
        else
            snprintf(elapsed_str, sizeof(elapsed_str), "%.1fs", elapsed_ms / 1000.0);
    }

    char size_str[32] = "";
    size_t total = strlen(result);
    if (total > 1024)
        snprintf(size_str, sizeof(size_str), "%.1fKB", total / 1024.0);

    char trail[256];
    int tpos = snprintf(trail, sizeof(trail), "%s", name);
    if (elapsed_str[0] && tpos < (int)sizeof(trail))
        tpos += snprintf(trail + tpos, sizeof(trail) - tpos, " · %s", elapsed_str);
    if (size_str[0] && tpos < (int)sizeof(trail))
        tpos += snprintf(trail + tpos, sizeof(trail) - tpos, " · %s", size_str);
    print_role_header("tool_response", ok, trail);

    /* Display-art tools (plot) render in full color; otherwise a dim preview
     * of the first ~10 lines / 800 bytes. */
    if (ok && tui_print_tool_art(name, result)) {
        /* full colored art already printed */
    } else if (result && result[0]) {
        const char *p = result;
        int lines = 0;
        size_t emitted = 0;
        while (*p && lines < 10 && emitted < 800) {
            const char *nl = strchr(p, '\n');
            int len = nl ? (int)(nl - p) : (int)strlen(p);
            if (len > 200)
                len = 200;
            fprintf(stderr, "  %s%.*s%s\n", TUI_DIM, len, p, TUI_RESET);
            emitted += (size_t)len + 1;
            lines++;
            if (!nl)
                break;
            p = nl + 1;
        }
        if (*p)
            fprintf(stderr, "  %s… (%zu bytes total)%s\n", TUI_DIM, total, TUI_RESET);
    }
    baseline_log(ok ? "tool_result" : "tool_error", name, result, NULL);
}

/* Legacy wrapper for any callers that don't have elapsed info */
static void print_tool_result(const char *name, bool ok, const char *result) {
    print_tool_result_ex(name, ok, result, 0.0);
}

static void print_usage_ex(usage_t *u, const char *model, session_state_t *session) {
    bool truecolor = tui_supports_truecolor();
    const tui_glyphs_t *gl = tui_glyph();

    if (truecolor) {
        tui_rgb_t dim_c = tui_hsv_to_rgb(220.0f, 0.10f, 0.45f);
        tui_rgb_t in_c = tui_hsv_to_rgb(210.0f, 0.30f, 0.60f);
        tui_rgb_t out_c = tui_hsv_to_rgb(160.0f, 0.30f, 0.60f);

        fprintf(stderr, "  \033[38;2;%d;%d;%dm[", dim_c.r, dim_c.g, dim_c.b);
        fprintf(stderr, "\033[38;2;%d;%d;%dmin:%d", in_c.r, in_c.g, in_c.b, u->input_tokens);
        fprintf(stderr, " \033[38;2;%d;%d;%dmout:%d", out_c.r, out_c.g, out_c.b, u->output_tokens);

        if (u->cache_read_input_tokens > 0) {
            tui_rgb_t cr = tui_hsv_to_rgb(120.0f, 0.25f, 0.55f);
            fprintf(stderr, " \033[38;2;%d;%d;%dm%scache-read:%d", cr.r, cr.g, cr.b,
                    gl->icon_lightning, u->cache_read_input_tokens);
        }
        if (u->cache_creation_input_tokens > 0) {
            tui_rgb_t cw = tui_hsv_to_rgb(40.0f, 0.30f, 0.60f);
            fprintf(stderr, " \033[38;2;%d;%d;%dmcache-write:%d", cw.r, cw.g, cw.b,
                    u->cache_creation_input_tokens);
        }

        const model_info_t *mi = model_lookup(model);
        if (mi) {
            double turn_cost = u->input_tokens * mi->input_price / 1e6 +
                               u->output_tokens * mi->output_price / 1e6 +
                               u->cache_read_input_tokens * mi->cache_read_price / 1e6 +
                               u->cache_creation_input_tokens * mi->cache_write_price / 1e6;
            /* Cost color: green cheap → yellow → red expensive */
            float cost_hue = turn_cost < 0.01 ? 120.0f
                             : turn_cost < 0.10
                                 ? 120.0f - (float)((turn_cost - 0.01) / 0.09) * 60.0f
                             : turn_cost < 1.00 ? 60.0f - (float)((turn_cost - 0.10) / 0.90) * 60.0f
                                                : 0.0f;
            tui_rgb_t cost_c = tui_hsv_to_rgb(cost_hue, 0.45f, 0.75f);
            fprintf(stderr, " \033[38;2;%d;%d;%dm%s$%.4f", cost_c.r, cost_c.g, cost_c.b,
                    gl->icon_money, turn_cost);
            if (session) {
                double total = session_cost(session);
                tui_rgb_t tot_c = tui_hsv_to_rgb(220.0f, 0.10f, 0.50f);
                fprintf(stderr, " \033[38;2;%d;%d;%dm(total: $%.4f)", tot_c.r, tot_c.g, tot_c.b,
                        total);
            }
        }
        fprintf(stderr, "\033[38;2;%d;%d;%dm]\033[0m\n", dim_c.r, dim_c.g, dim_c.b);
    } else {
        fprintf(stderr, "%s  [in:%d out:%d", TUI_DIM, u->input_tokens, u->output_tokens);
        if (u->cache_read_input_tokens > 0)
            fprintf(stderr, " cache-read:%d", u->cache_read_input_tokens);
        if (u->cache_creation_input_tokens > 0)
            fprintf(stderr, " cache-write:%d", u->cache_creation_input_tokens);
        const model_info_t *mi = model_lookup(model);
        if (mi) {
            double turn_cost = u->input_tokens * mi->input_price / 1e6 +
                               u->output_tokens * mi->output_price / 1e6 +
                               u->cache_read_input_tokens * mi->cache_read_price / 1e6 +
                               u->cache_creation_input_tokens * mi->cache_write_price / 1e6;
            fprintf(stderr, " $%.4f", turn_cost);
            if (session) {
                double total = session_cost(session);
                fprintf(stderr, " (total: $%.4f)", total);
            }
        }
        fprintf(stderr, "]%s\n", TUI_RESET);
    }
}

/* ── Read input line (readline or fgets) ───────────────────────────────── */

static void park_transcript_cursor_after_pane(void) {
    /* Ephemeral panel: nothing to do. tui_composer_read already cleared
     * the panel rows and echoed the input into scrollback. The cursor
     * sits where the next streamed text should begin. */
}

static char *read_input_line_prompt(char *buf, size_t buf_sz, const char *prompt) {
    const char *p = prompt ? prompt : "\033[1m\033[95m\xe2\x9d\xaf\033[0m ";

    /* Surface any background agents that finished since the last prompt. */
    drain_pet_notifications(g_winch_sb);

    /* Persistent-pane path: use the multi-line composer box. */
    if (g_winch_sb && g_winch_sb->visible) {
        /* Feed the composer's live slash-command dropdown once. s_slash_commands
         * shares tui_cmd_entry_t's {name, desc} layout, so it registers directly. */
        static bool slash_registered = false;
        if (!slash_registered) {
            tui_composer_set_slash_commands((const tui_cmd_entry_t *)s_slash_commands,
                                            slash_commands_count());
            slash_registered = true;
        }
        char *r = tui_composer_read(g_winch_sb, p, buf, buf_sz);
        if (!r)
            return NULL;
#ifdef HAVE_READLINE
        if (buf[0]) {
            HIST_ENTRY *prev = history_get(where_history());
            if (!prev || strcmp(prev->line, buf) != 0)
                add_history(buf);
        }
#endif
        /* Panel is ephemeral — composer_read already cleared the 3 panel
         * rows and parked the cursor where streamed text should begin.
         * Re-rendering the panel here would put it back under the cursor,
         * so streaming would overwrite it and scroll the chrome up into
         * the response. The next composer_read call repaints the panel. */
        park_transcript_cursor_after_pane();
        return buf;
    }

#ifdef HAVE_READLINE
    char *line = readline(p);
    if (!line)
        return NULL;
    if (line[0]) {
        HIST_ENTRY *prev = history_get(where_history());
        if (!prev || strcmp(prev->line, line) != 0)
            add_history(line);
    }
    snprintf(buf, buf_sz, "%s", line);
    free(line);
    return buf;
#else
    fprintf(stderr, "%s", p);
    fflush(stderr);
    if (!fgets(buf, (int)buf_sz, stdin)) {
        if (ferror(stdin) && errno == EINTR) {
            clearerr(stdin);
            return buf; /* will be empty, caller handles */
        }
        return NULL;
    }
    size_t len = strlen(buf);
    if (len > 0 && buf[len - 1] != '\n') {
        int ch;
        while ((ch = fgetc(stdin)) != EOF && ch != '\n')
            ;
    }
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
        buf[--len] = '\0';
    return buf;
#endif
}

/* ── Main agent loop ───────────────────────────────────────────────────── */

void agent_run(const char *api_key, const char *model, const char *topology_name,
               bool topology_auto, const char *provider_override) {
    g_provider_override_name = provider_override;
    /* Register terminal reset FIRST — ensures scroll region, bracketed paste,
       and SGR attrs are cleaned up on ANY exit path through exit(). */
    atexit(terminal_reset_atexit);

    struct sigaction sa_int;
    memset(&sa_int, 0, sizeof(sa_int));
    sa_int.sa_handler = sigint_handler;
    sigemptyset(&sa_int.sa_mask);
    sigaction(SIGINT, &sa_int, NULL);

    struct sigaction sa_tstp;
    memset(&sa_tstp, 0, sizeof(sa_tstp));
    sa_tstp.sa_handler = sigtstp_handler;
    sigemptyset(&sa_tstp.sa_mask);
    sigaction(SIGTSTP, &sa_tstp, NULL);

    /* Auto-save on SIGTERM/SIGHUP */
    struct sigaction sa_term;
    memset(&sa_term, 0, sizeof(sa_term));
    sa_term.sa_handler = sigterm_autosave;
    sigemptyset(&sa_term.sa_mask);
    sigaction(SIGTERM, &sa_term, NULL);
    sigaction(SIGHUP, &sa_term, NULL);

    tools_init();
    tools_hint_init();
    tools_cooc_load();
    dsco_locks_init(&g_locks);
    /* Load agent profiles and re-apply active filter if set */
    agent_profiles_init();
    if (agent_profile_active_name()) {
        tools_set_profile_filter(agent_profile_tool_allowed);
    }
    md_init(&s_md, stderr);

    /* Initialize 40 UI features */
    tui_features_init(&g_features);
    g_tui_features = &g_features;
    tui_cadence_init(&s_cadence, cadence_flush_to_md, &s_md);
    tui_word_counter_init(&s_word_counter);
    tui_thinking_init(&s_thinking);
    tui_flame_init(&s_flame);
    tui_dag_init(&s_dag);
    tui_branch_init(&s_branch);
    tui_citation_init(&s_citations);
    tui_throughput_init(&s_throughput);
    tui_ghost_init(&s_ghost);
    memset(&s_last_latency, 0, sizeof(s_last_latency));
    /* F29: Adaptive theme detection */
    tui_apply_theme(tui_detect_theme());

    /* MCP servers connect in the background after the input panel is up
     * (see mcp_bg_init_start below). Connecting synchronously here would
     * block the prompt for seconds — especially for HTTP servers like Modal
     * that cold-start on first request. Discovered tools register
     * progressively; the LLM picks up whatever is registered at request-
     * build time. */

    /* Rate limiter: 5 requests/second burst, 1/second sustained */
    rate_limiter_t rate_limiter;
    rate_limiter_init(&rate_limiter, 5.0, 1.0);

    /* Per-tool metrics */
    tool_metrics_t tool_metrics;
    tool_metrics_init(&tool_metrics);

    /* Tool result cache */
    tool_cache_t tool_cache;
    tool_cache_init(&tool_cache);

    /* Multi-tier compaction config + post-compact file restoration */
    compact_config_t compact_cfg;
    compact_config_init(&compact_cfg);
    post_compact_restore_t file_restore;
    post_compact_restore_init(&file_restore);

    /* ── Output serialization & animation clock ─────────────────────── */
    /* All TUI writes route through g_outq; a single render thread
     * drains on 16ms ticks, preventing race conditions between
     * heartbeat, md_feed, ESC poller, and the main agent loop. */
    static tui_output_queue_t s_outq;
    tui_outq_init(&s_outq, stderr);
    g_outq = &s_outq;

    static tui_anim_clock_t s_anim_clock;
    tui_anim_clock_init(&s_anim_clock, 16);
    g_anim_clock = &s_anim_clock;

    /* Streaming FSM replaces ad-hoc boolean flags.
     * Uses the file-scope s_streaming_fsm so callbacks can access it. */
    tui_streaming_fsm_create(&s_streaming_fsm, NULL);

    /* Patch in rendering callbacks — defined in agent.c where they have
     * access to s_thinking, s_md, s_word_counter, g_features etc. */
    s_streaming_fsm.states[TUI_STREAM_ST_THINKING].on_enter = fsm_thinking_enter;
    s_streaming_fsm.states[TUI_STREAM_ST_THINKING].on_exit = fsm_thinking_exit;
    s_streaming_fsm.states[TUI_STREAM_ST_TEXT].on_enter = fsm_text_enter;
    s_streaming_fsm.states[TUI_STREAM_ST_TEXT].on_exit = fsm_text_exit;
    s_streaming_fsm.states[TUI_STREAM_ST_TOOL_PENDING].on_enter = fsm_tool_pending_enter;

    /* Set terminal title */
    tui_set_title_fmt("dsco · %s", model);

#ifdef HAVE_READLINE
    rl_attempted_completion_function = command_completion;
    /* command_completion_display is void(char**,int,int). Cast to the hook
     * field's own type via typeof — portable across readline builds that name
     * it rl_compdisp_func_t* vs libedit VFunction* vs a bare pointer. */
    rl_completion_display_matches_hook =
        (typeof(rl_completion_display_matches_hook))command_completion_display;
    rl_bind_key('/', slash_completion_key);
    rl_bind_key('\f', dsco_clear_screen); /* Ctrl+L clears screen */
    rl_basic_word_break_characters = " \t\n";
    /* Load persisted history */
    {
        const char *home = getenv("HOME");
        if (home) {
            char hist_path[560];
            snprintf(hist_path, sizeof(hist_path), "%s/.dsco/history", home);
            read_history(hist_path);
        }
    }
    stifle_history(1000);
#endif

    conversation_t conv;
    conv_init(&conv);
    g_autosave_conv = &conv;
    atexit(exit_autosave_handler);

    /* Session state */
    session_state_t session;
    session_state_init(&session, model);
    setenv("DSCO_TRUST_TIER", session_trust_tier_to_string(session.trust_tier), 1);
    if (topology_name && topology_name[0]) {
        const topology_t *cli_topology = topology_find(topology_name);
        if (cli_topology) {
            snprintf(session.active_topology, sizeof(session.active_topology), "%s",
                     cli_topology->name);
        } else {
            fprintf(stderr, "  %swarning:%s unknown topology '%s'\n", TUI_BYELLOW, TUI_RESET,
                    topology_name);
        }
    }
    session.topology_auto = topology_auto;
    g_autosave_session = &session;

    /* Initialize provider based on model */
    ensure_provider(&session, api_key);
    tools_set_runtime_api_key(api_key);
    tools_set_runtime_model(session.model);
    tools_set_context_window(session.context_window);

    char input_buf[MAX_INPUT_LINE];
    char last_user_input[MAX_INPUT_LINE] = {0}; /* for /retry */
    int last_input_tokens = 0;
    int consecutive_tool_failures = 0; /* for router failure escalation */

    /* Command aliases: /alias <name> <expansion> */
#define MAX_ALIASES 32
    command_alias_t *aliases = safe_malloc(MAX_ALIASES * sizeof(*aliases));
    memset(aliases, 0, MAX_ALIASES * sizeof(*aliases));
    int alias_count = 0;

    int tool_count;
    tools_get_all(&tool_count);
    tool_count += g_external_tool_count; /* include MCP tools */
    int core_count = tools_get_core_count() + g_external_tool_count;
    int display_core = g_cheap_mode ? TOOL_REG_ALWAYS : core_count;

    /* Auto-resume if supervisor set DSCO_RESUME_AFTER_CRASH=1
     * (fires on crash, OOM kill, or pre-emption restart). */
    {
        const char *resume = getenv("DSCO_RESUME_AFTER_CRASH");
        const char *home   = getenv("HOME");
        char autosave_path[560]; autosave_path[0] = '\0';
        if (home)
            snprintf(autosave_path, sizeof(autosave_path),
                     "%s/.dsco/sessions/_autosave.json", home);
        if (resume && resume[0] == '1' && autosave_path[0]
                && access(autosave_path, R_OK) == 0) {
            fprintf(stderr, "  %s[supervisor] session crashed, resuming...%s\n",
                    TUI_DIM, TUI_RESET);
            /* Write a startup command file the REPL will consume on first turn */
            {
                char startup_f[512];
                const char *h2 = getenv("HOME");
                if (h2) {
                    snprintf(startup_f, sizeof(startup_f),
                             "%s/.dsco/sessions/_startup_cmd", h2);
                    FILE *sf = fopen(startup_f, "w");
                    if (sf) { fputs("/load _autosave", sf); fclose(sf); }
                }
            }
            unsetenv("DSCO_RESUME_AFTER_CRASH");
        } else if (autosave_path[0] && access(autosave_path, R_OK) == 0) {
            fprintf(stderr, "  %sautosave found. /load _autosave to resume%s\n",
                    TUI_DIM, TUI_RESET);
        }
    }

    /* Initialize cost budgets from env vars */
    init_cost_budgets();

    /* Welcome banner */
    tui_welcome(session.model, display_core, tool_count, DSCO_VERSION);

    if (g_cheap_mode) {
        fprintf(stderr,
                "  %s⚡ cheap mode%s — %d/%d tools active, discover_tools + load_tools to page in "
                "more\n",
                TUI_BYELLOW, TUI_RESET, TOOL_REG_ALWAYS, tool_count);
    }

    /* Enhanced startup info */
    {
        /* Active feature count */
        int active_features = 0;
        const bool *flags = (const bool *)&g_features;
        for (int fi = 0; fi < TUI_FEATURE_COUNT; fi++)
            if (flags[fi])
                active_features++;

        fprintf(stderr, "  %s%d/%d features active%s · %strust: %s%s", TUI_DIM, active_features,
                TUI_FEATURE_COUNT, TUI_RESET, TUI_DIM,
                session_trust_tier_to_string(session.trust_tier), TUI_RESET);

        /* Git branch on startup (F19) */
        if (g_features.branch_indicator) {
            FILE *gf = popen("git rev-parse --abbrev-ref HEAD 2>/dev/null", "r");
            if (gf) {
                char branch[128] = "";
                if (fgets(branch, sizeof(branch), gf)) {
                    size_t bl = strlen(branch);
                    if (bl > 0 && branch[bl - 1] == '\n')
                        branch[bl - 1] = '\0';
                    if (branch[0])
                        fprintf(stderr, " · %s%s %s%s", TUI_BMAGENTA,
                                tui_glyph()->icon_git ? tui_glyph()->icon_git : "", branch,
                                TUI_RESET);
                }
                pclose(gf);
            }
        }
        fprintf(stderr, "\n");
        if (session.active_topology[0]) {
            fprintf(stderr, "  %stopology:%s %s\n", TUI_DIM, TUI_RESET, session.active_topology);
        } else if (session.topology_auto) {
            fprintf(stderr, "  %stopology:%s auto\n", TUI_DIM, TUI_RESET);
        }
    }

    /* Initialize status bar */
    tui_status_bar_t status_bar;
    tui_status_bar_init(&status_bar, session.model);
    /* Always show clock in the persistent pane (it's the right-cluster anchor). */
    tui_status_bar_set_clock(&status_bar, true);

    /* Enable persistent bottom pane in interactive TTY sessions only.
     * Set DSCO_NO_PANE=1 to disable for plain logging. */
    bool enable_pane = isatty(STDIN_FILENO) && isatty(STDERR_FILENO);
    const char *no_pane = getenv("DSCO_NO_PANE");
    if (no_pane && (no_pane[0] == '1' || no_pane[0] == 't' || no_pane[0] == 'T'))
        enable_pane = false;
    if (enable_pane) {
        tui_status_bar_enable(&status_bar);
        /* No DECSTBM, no pre-render — the panel is ephemeral and renders
         * itself just-in-time inside tui_composer_read. Welcome banner
         * and startup notes print normally above the eventual panel. */
    }

    /* SIGWINCH handler for terminal resize */
    g_winch_sb = &status_bar;
    struct sigaction sa_winch;
    memset(&sa_winch, 0, sizeof(sa_winch));
    sa_winch.sa_handler = sigwinch_handler;
    sigemptyset(&sa_winch.sa_mask);
    sa_winch.sa_flags = SA_RESTART;
    sigaction(SIGWINCH, &sa_winch, NULL);

    /* Always show today's spend — awareness, not enforcement */
    {
        double daily = baseline_daily_cost();
        fprintf(stderr, "  %stoday:%s $%.2f", TUI_DIM, TUI_RESET, daily);
        if (g_cost_budget > 0)
            fprintf(stderr, " · session cap $%.2f", g_cost_budget);
        if (g_daily_budget > 0)
            fprintf(stderr, " · daily cap $%.2f", g_daily_budget);
        fprintf(stderr, "\n");
    }

    fprintf(stderr, "  %stype a message, /help for commands, quit to exit%s\n", TUI_DIM, TUI_RESET);

    /* Kick off MCP server discovery in the background. The input panel is now
     * up; mcp_init's HTTP roundtrips run on a worker so the user can start
     * typing immediately. A panel notification fires when discovery
     * completes. Set DSCO_MCP_SYNC=1 to fall back to blocking foreground
     * init (useful when scripting / debugging). */
    {
        const char *sync_env = getenv("DSCO_MCP_SYNC");
        bool sync_mcp =
            sync_env && (sync_env[0] == '1' || sync_env[0] == 't' || sync_env[0] == 'T');
        if (sync_mcp) {
            int n = mcp_init(&g_mcp);
            if (n > 0)
                mcp_register_discovered_tools(&g_mcp);
            if (g_mcp.tool_count > 0) {
                fprintf(stderr, "  %smcp: %d tools registered%s\n", TUI_DIM, g_mcp.tool_count,
                        TUI_RESET);
            }
        } else {
            mcp_bg_init_start(enable_pane ? &status_bar : NULL);
        }
    }

    while (1) {
        g_interrupted = 0;
        g_escape_state = ESC_RUNNING;
        output_guard_reset();
        terminal_input_echo_restore();

        /* Build dynamic prompt: [turn N] model · $cost · context% ▸ */
        char dyn_prompt[256];
        {
            double cost = session_cost(&session);
            /* Context occupancy = size of the LAST request (the full conversation
             * we just sent), not the cumulative sum of every turn's tokens —
             * otherwise the gauge climbs past 100% as turns accumulate. Matches
             * the /context bar (print_context). */
            int ctx_used = session.last_input_tokens > 0
                               ? session.last_input_tokens
                               : session.total_input_tokens + session.total_output_tokens;
            int ctx_max =
                session.context_window > 0 ? session.context_window : CONTEXT_WINDOW_TOKENS;
            double ctx_pct = ctx_max > 0 ? 100.0 * ctx_used / ctx_max : 0;
            if (ctx_pct > 100.0)
                ctx_pct = 100.0;
            const char *ctx_color =
                ctx_pct < 60 ? TUI_GREEN : (ctx_pct < 85 ? TUI_YELLOW : TUI_RED);

            /* Shorten model name for prompt */
            const model_info_t *mi = model_lookup(session.model);
            const char *short_model = mi ? mi->alias : session.model;

            if (session.turn_count > 0) {
                snprintf(dyn_prompt, sizeof(dyn_prompt),
                         "\001" TUI_DIM "\002"
                         "["
                         "\001" TUI_RESET "\002"
                         "\001" TUI_BCYAN "\002"
                         "t%d"
                         "\001" TUI_RESET "\002"
                         "\001" TUI_DIM "\002"
                         "] "
                         "\001" TUI_RESET "\002"
                         "\001" TUI_BWHITE "\002"
                         "%s"
                         "\001" TUI_RESET "\002"
                         "\001" TUI_DIM "\002"
                         " · "
                         "\001" TUI_RESET "\002"
                         "\001" TUI_GREEN "\002"
                         "$%.2f"
                         "\001" TUI_RESET "\002"
                         "\001" TUI_DIM "\002"
                         " · "
                         "\001" TUI_RESET "\002"
                         "\001%s\002"
                         "%.0f%%"
                         "\001" TUI_RESET "\002"
                         "\001" TUI_DIM "\002"
                         " ▸"
                         "\001" TUI_RESET "\002"
                         " ",
                         session.turn_count, short_model, cost, ctx_color, ctx_pct);
            } else {
                snprintf(dyn_prompt, sizeof(dyn_prompt),
                         "\001" TUI_BOLD "\002"
                         "\001" TUI_BMAGENTA "\002"
                         "❯"
                         "\001" TUI_RESET "\002"
                         " ");
            }
        }

        /* ── Synchronize terminal state before input ──────────────────
         * After streaming, cursor position may be undefined.
         * Reset scroll region, flush stderr, then re-establish the
         * bottom panel and place cursor on the input row.  This prevents
         * the input prompt from appearing in the middle of output. */
        fflush(stderr);
        if (!read_input_line_prompt(input_buf, sizeof(input_buf), dyn_prompt))
            break;

        size_t len = strlen(input_buf);
        if (len == 0)
            continue;

        /* Strip bracketed paste markers if present */
        {
            char *ps = strstr(input_buf, "\033[200~");
            if (ps)
                memmove(ps, ps + 6, strlen(ps + 6) + 1);
            char *pe = strstr(input_buf, "\033[201~");
            if (pe)
                *pe = '\0';
        }
        dsco_strip_terminal_controls_inplace(input_buf);
        len = strlen(input_buf);
        if (len == 0)
            continue;

        /* Detect multi-line paste (newlines in input) */
        {
            int newlines = 0;
            for (const char *p = input_buf; *p; p++)
                if (*p == '\n')
                    newlines++;
            if (newlines > 0) {
                fprintf(stderr, "  %s[%d lines pasted]%s\n", TUI_DIM, newlines + 1, TUI_RESET);
            }
        }

        /* Consume startup command (e.g. auto-resume after crash) */
        {
            static bool startup_consumed = false;
            if (!startup_consumed) {
                startup_consumed = true;
                const char *h3 = getenv("HOME");
                if (h3) {
                    char scf[512];
                    snprintf(scf, sizeof(scf),
                             "%s/.dsco/sessions/_startup_cmd", h3);
                    FILE *sf = fopen(scf, "r");
                    if (sf) {
                        if (fgets(input_buf, (int)sizeof(input_buf)-1, sf)) {
                            /* trim newline */
                            size_t sl = strlen(input_buf);
                            if (sl > 0 && input_buf[sl-1] == '\n')
                                input_buf[sl-1] = '\0';
                        }
                        fclose(sf);
                        remove(scf);
                    }
                }
            }
        }
                if (strcmp(input_buf, "quit") == 0 || strcmp(input_buf, "exit") == 0 ||
            strcmp(input_buf, "/exit") == 0 || strcmp(input_buf, "/quit") == 0) {
            /* Tell supervisor not to relaunch on this voluntary exit. */
            setenv("DSCO_SUPERVISE_RESTART", "transient", 1);
            break;
        }

        /* ── Alias expansion ───────────────────────────────────────────── */
        {
            for (int ai = 0; ai < alias_count; ai++) {
                size_t nlen = strlen(aliases[ai].name);
                if (strncmp(input_buf, aliases[ai].name, nlen) == 0 &&
                    (input_buf[nlen] == '\0' || input_buf[nlen] == ' ')) {
                    char tail[MAX_INPUT_LINE] = {0};
                    if (input_buf[nlen] == ' ')
                        snprintf(tail, sizeof(tail), " %s", input_buf + nlen + 1);
                    snprintf(input_buf, sizeof(input_buf), "%s%s", aliases[ai].expansion, tail);
                    break;
                }
            }
        }

        /* ── Slash commands ────────────────────────────────────────────── */

        if (strcmp(input_buf, "/clear") == 0) {
            conv_free(&conv);
            conv_init(&conv);
            session.total_input_tokens = 0;
            session.total_output_tokens = 0;
            session.total_cache_read_tokens = 0;
            session.total_cache_write_tokens = 0;
            session.total_reported_cost_usd = 0;
            session.turn_count = 0;
            tui_success("conversation cleared");
            baseline_log("command", "/clear", NULL, NULL);
            continue;
        }
        if (strncmp(input_buf, "/models", 7) == 0) {
            local_server_t servers[8];
            int sn = local_llm_probe_servers(servers, 8);
            bool any_up = false;
            for (int i = 0; i < sn; i++)
                if (servers[i].up)
                    any_up = true;
            if (!any_up) {
                fprintf(stderr,
                        "  %sno local LLM servers reachable%s\n"
                        "  %sstart one: %sollama serve%s %s/ LM Studio :1234 / mlx :8181%s\n",
                        TUI_DIM, TUI_RESET, TUI_DIM, TUI_RESET, TUI_DIM, TUI_DIM, TUI_RESET);
            } else {
                local_model_t models[64];
                int mn = local_llm_list_models(models, 64);
                for (int i = 0; i < sn; i++) {
                    if (!servers[i].up)
                        continue;
                    fprintf(stderr, "  %s%s%s %s(%s, %d models)%s\n", TUI_BOLD, servers[i].server,
                            TUI_RESET, TUI_DIM, servers[i].base_url, servers[i].model_count,
                            TUI_RESET);
                    for (int j = 0; j < mn; j++) {
                        if (strcmp(models[j].server, servers[i].server) != 0)
                            continue;
                        fprintf(stderr, "    %s/model %s%s\n", TUI_DIM, models[j].qualified,
                                TUI_RESET);
                    }
                }
            }
            baseline_log("command", "/models", NULL, NULL);
            continue;
        }
        if (strncmp(input_buf, "/login", 6) == 0) {
            const char *arg = input_buf + 6;
            while (*arg == ' ')
                arg++;
            if (*arg == '\0' || strcmp(arg, "chatgpt") == 0 || strcmp(arg, "openai") == 0) {
                int rc = openai_oauth_login();
                if (rc == 0) {
                    tui_success("ChatGPT account linked — openai/* models now use your "
                                "subscription");
                }
            } else if (strcmp(arg, "status") == 0) {
                fprintf(stderr, "  %schatgpt:%s %s\n", TUI_DIM, TUI_RESET,
                        openai_oauth_source_name());
                fprintf(stderr, "  %sclaude: %s %s\n", TUI_DIM, TUI_RESET,
                        provider_claude_code_oauth_source());
            } else if (strcmp(arg, "claude") == 0) {
                fprintf(stderr,
                        "  %sClaude OAuth uses Claude Code credentials. Run %sclaude /login%s "
                        "%sin Claude Code, or set CLAUDE_CODE_OAUTH_TOKEN.%s\n",
                        TUI_DIM, TUI_RESET, TUI_DIM, TUI_DIM, TUI_RESET);
            } else {
                fprintf(stderr, "  %susage: /login [chatgpt|claude|status]%s\n", TUI_DIM,
                        TUI_RESET);
            }
            baseline_log("command", "/login", arg, NULL);
            continue;
        }
        if (strncmp(input_buf, "/logout", 7) == 0) {
            const char *arg = input_buf + 7;
            while (*arg == ' ')
                arg++;
            if (*arg == '\0' || strcmp(arg, "chatgpt") == 0 || strcmp(arg, "openai") == 0) {
                if (openai_oauth_logout() == 0)
                    tui_success("signed out of ChatGPT (dsco token cache cleared)");
                else
                    fprintf(stderr, "  %sno dsco ChatGPT token cache to clear%s\n", TUI_DIM,
                            TUI_RESET);
            } else {
                fprintf(stderr, "  %susage: /logout [chatgpt]%s\n", TUI_DIM, TUI_RESET);
            }
            baseline_log("command", "/logout", arg, NULL);
            continue;
        }
        if (strncmp(input_buf, "/model", 6) == 0) {
            const char *arg = input_buf + 6;
            while (*arg == ' ')
                arg++;
            if (*arg == '\0') {
                /* Show current model */
                fprintf(stderr, "  %smodel:%s %s\n", TUI_DIM, TUI_RESET, session.model);
                fprintf(stderr, "  %savailable:%s", TUI_DIM, TUI_RESET);
                for (int i = 0; MODEL_REGISTRY[i].alias; i++)
                    fprintf(stderr, " %s", MODEL_REGISTRY[i].alias);
                fprintf(stderr, "\n");
            } else if (local_llm_is_local_ref(arg)) {
                /* Local model: "<server>:<model>". Resolve context window from
                 * the local server rather than the static registry. */
                char server[16] = {0};
                const char *colon = strchr(arg, ':');
                const char *model_name = colon ? colon + 1 : arg;
                size_t slen = colon ? (size_t)(colon - arg) : 0;
                if (slen >= sizeof(server))
                    slen = sizeof(server) - 1;
                memcpy(server, arg, slen);
                if (strcmp(server, "local") == 0)
                    snprintf(server, sizeof(server), "ollama");
                snprintf(session.model, sizeof(session.model), "%s", arg);
                int ctx = local_llm_context_window(server, model_name);
                session.context_window = ctx > 0 ? ctx : 32768;
                tools_set_context_window(session.context_window);
                session.model_locked = true;
                tools_set_runtime_model(session.model);
                char msg[256];
                snprintf(msg, sizeof(msg), "model switched to %s (local %s, ctx: %dk)", arg, server,
                         session.context_window / 1000);
                tui_success(msg);
            } else {
                const char *resolved = model_resolve_alias(arg);
                snprintf(session.model, sizeof(session.model), "%s", resolved);
                session.context_window = model_context_window(resolved);
                tools_set_context_window(session.context_window);
                session.model_locked = true; /* user explicitly chose; block auto-switching */
                tools_set_runtime_model(session.model);
                const model_info_t *mi = model_lookup(resolved);
                char msg[256];
                snprintf(msg, sizeof(msg), "model switched to %s (ctx: %dk)", resolved,
                         session.context_window / 1000);
                tui_success(msg);
                /* Update thinking gate */
                if (mi && !mi->supports_thinking) {
                    fprintf(stderr, "  %snote: adaptive thinking not available for this model%s\n",
                            TUI_DIM, TUI_RESET);
                }
            }
            baseline_log("command", "/model", session.model, NULL);
            continue;
        }
        if (strncmp(input_buf, "/route", 6) == 0) {
            const char *arg = input_buf + 6;
            while (*arg == ' ')
                arg++;
            if (*arg == '\0' || strcmp(arg, "status") == 0) {
                char buf[4096];
                router_to_json(&g_router, buf, sizeof(buf));
                fprintf(stderr, "  %srouter status:%s\n  policy: %s\n  session_cost: $%.4f\n%s\n",
                        TUI_DIM, TUI_RESET, router_policy_name(g_router.policy),
                        g_router.session_cost_usd, buf);
            } else if (strncmp(arg, "policy ", 7) == 0) {
                const char *pol = arg + 7;
                g_router.policy = router_policy_parse(pol);
                char msg[128];
                snprintf(msg, sizeof(msg), "routing policy → %s",
                         router_policy_name(g_router.policy));
                tui_success(msg);
            } else if (strncmp(arg, "budget ", 7) == 0) {
                double b = atof(arg + 7);
                g_router.cost_budget_usd = b;
                char msg[128];
                snprintf(msg, sizeof(msg), "cost budget → $%.4f", b);
                tui_success(msg);
            } else if (strcmp(arg, "history") == 0) {
                char buf[8192];
                router_history_to_json(&g_router, buf, sizeof(buf));
                fprintf(stderr, "%s\n", buf);
            } else if (strcmp(arg, "recommend") == 0) {
                int ctx_pct = last_input_tokens * 100 /
                              (session.context_window > 0 ? session.context_window : 200000);
                task_complexity_t c = router_classify_task(NULL, session.turn_count, 0, ctx_pct);
                router_decision_t rd =
                    router_decide(&g_router, session.model, c, session_cost(&session), 0.0,
                                  consecutive_tool_failures);
                fprintf(stderr,
                        "  %srouter recommend:%s %s  (complexity=%s reason=%s confidence=%.0f%%)\n"
                        "  %s%s%s\n",
                        TUI_DIM, TUI_RESET, rd.model_id, task_complexity_name(rd.complexity),
                        switch_reason_name(rd.reason), (double)rd.confidence * 100.0, TUI_DIM,
                        rd.rationale, TUI_RESET);
            } else if (strcmp(arg, "unlock") == 0) {
                session.model_locked = false;
                tui_success("model unlocked — router may now auto-switch");
            } else if (strcmp(arg, "lock") == 0) {
                session.model_locked = true;
                tui_success("model locked — router will not auto-switch");
            } else {
                fprintf(stderr, "  usage: /route [status|policy <name>|budget "
                                "<usd>|history|recommend|lock|unlock]\n");
                fprintf(stderr, "  policies: fixed cost latency quality balanced adaptive\n");
            }
            baseline_log("command", "/route", arg, NULL);
            continue;
        }
        if (strncmp(input_buf, "/effort", 7) == 0) {
            const char *arg = input_buf + 7;
            while (*arg == ' ')
                arg++;
            if (*arg == '\0') {
                fprintf(stderr, "  %seffort:%s %s  (options: low, medium, high)\n", TUI_DIM,
                        TUI_RESET, session.effort);
            } else if (strcmp(arg, "low") == 0 || strcmp(arg, "medium") == 0 ||
                       strcmp(arg, "high") == 0) {
                snprintf(session.effort, sizeof(session.effort), "%s", arg);
                char msg[64];
                snprintf(msg, sizeof(msg), "effort set to %s", arg);
                tui_success(msg);
            } else {
                tui_error("effort must be: low, medium, or high");
            }
            baseline_log("command", "/effort", session.effort, NULL);
            continue;
        }
        if (strcmp(input_buf, "/cheap") == 0) {
            g_cheap_mode = 1;
            session.tool_budget_ratio = 0.0f;
            fprintf(stderr,
                    "  %s⚡ cheap mode%s — 5 core tools, no catalog, no workspace prompt\n"
                    "  %sdiscover_tools + load_tools to page in what you need%s\n",
                    TUI_BYELLOW, TUI_RESET, TUI_DIM, TUI_RESET);
            baseline_log("command", "/cheap", "on", NULL);
            continue;
        }
        if (strcmp(input_buf, "/full") == 0) {
            g_cheap_mode = 0;
            session.tool_budget_ratio = 1.0f;
            fprintf(stderr, "  %s✦ full mode%s — all tools + catalog + workspace prompt restored\n",
                    TUI_BGREEN, TUI_RESET);
            baseline_log("command", "/full", "on", NULL);
            continue;
        }
        if (strcmp(input_buf, "/cost") == 0) {
            print_cost(&session);
            baseline_log("command", "/cost", NULL, NULL);
            continue;
        }
        if (strcmp(input_buf, "/context") == 0) {
            print_context(&session, last_input_tokens);
            /* F15: Context pressure gauge */
            tui_context_gauge(session.total_input_tokens + session.total_output_tokens,
                              session.context_window, 0);
            baseline_log("command", "/context", NULL, NULL);
            continue;
        }
        if (strcmp(input_buf, "/compact") == 0) {
            /* Manual compaction: use tiered pipeline */
            compact_config_t manual_cfg;
            compact_config_init(&manual_cfg);
            manual_cfg.max_result_chars = 128; /* aggressive for manual */
            int msgs_before = conv.count;
            compact_result_t cr = conv_auto_compact(&conv, &session, &manual_cfg);
            /* Post-compact: re-inject recently-read files (only if we
             * actually shrank, otherwise we'd grow the conversation). */
            int freed_tokens = cr.pre_token_count - cr.post_token_count;
            int msgs_dropped = msgs_before - conv.count;
            if (cr.tier_used >= COMPACT_SNIP && (freed_tokens > 0 || msgs_dropped > 0))
                post_compact_restore_inject(&file_restore, &conv);

            char msg[256];
            if (freed_tokens > 0 || msgs_dropped > 0) {
                snprintf(msg, sizeof(msg),
                         "compacted (tier %d): %dk→%dk tokens, "
                         "-%d msgs, %d remain (%.1fms)",
                         cr.tier_used, cr.pre_token_count / 1000, cr.post_token_count / 1000,
                         msgs_dropped, conv.count, cr.duration_ms);
                tui_success(msg);
            } else {
                /* No-op: report honestly so the user knows compact is
                 * already at the floor (most likely cause: conversation
                 * is small enough that reserve windows cover everything,
                 * or the conv body is already minimal and the overhead
                 * is system prompt + tool schemas that /compact can't
                 * touch). */
                int thresh = auto_compact_threshold(&session);
                int overhead = session.non_conv_overhead_tokens;
                snprintf(msg, sizeof(msg),
                         "no-op (tier %d): %dk tokens "
                         "(threshold %dk, prompt/tools overhead %dk) — "
                         "nothing droppable in conv body",
                         cr.tier_used, cr.pre_token_count / 1000, thresh / 1000, overhead / 1000);
                tui_warning(msg);
            }
            baseline_log("command", "/compact", NULL, NULL);
            continue;
        }
        if (strcmp(input_buf, "/undo") == 0) {
            if (!conv_pop_last_turn(&conv)) {
                tui_warning("nothing to undo");
            } else {
                if (session.turn_count > 0)
                    session.turn_count--;
                char msg[64];
                snprintf(msg, sizeof(msg), "last turn removed (%d messages remain)", conv.count);
                tui_success(msg);
                last_user_input[0] = '\0';
            }
            continue;
        }
        if (strncmp(input_buf, "/retry", 6) == 0) {
            const char *arg = input_buf + 6;
            while (*arg == ' ')
                arg++;
            if (!last_user_input[0]) {
                tui_warning("no previous message to retry");
                continue;
            }
            /* Optionally switch model first */
            if (*arg) {
                const char *resolved = model_resolve_alias(arg);
                snprintf(session.model, sizeof(session.model), "%s", resolved);
                session.context_window = model_context_window(resolved);
                ensure_provider(&session, api_key);
                char msmsg[128];
                snprintf(msmsg, sizeof(msmsg), "model → %s", resolved);
                tui_success(msmsg);
            }
            if (conv_pop_last_turn(&conv) && session.turn_count > 0) {
                session.turn_count--;
            }
            snprintf(input_buf, sizeof(input_buf), "%s", last_user_input);
            /* Fall through by NOT continuing — let input_buf reach the LLM dispatch */
            goto send_to_llm;
        }
        if (strncmp(input_buf, "/diff", 5) == 0) {
            const char *arg = input_buf + 5;
            while (*arg == ' ')
                arg++;
            bool staged = (strcmp(arg, "--staged") == 0 || strcmp(arg, "--cached") == 0);
            const char *cmd = staged ? "git diff --staged 2>&1" : "git diff 2>&1";
            FILE *fp = popen(cmd, "r");
            if (!fp) {
                tui_error("git diff failed");
            } else {
                char line[512];
                int lines = 0;
                fprintf(stderr, "\n");
                while (fgets(line, sizeof(line), fp)) {
                    fprintf(stderr, "%s", line);
                    lines++;
                }
                pclose(fp);
                if (lines == 0)
                    fprintf(stderr, "  %s(no changes)%s\n", TUI_DIM, TUI_RESET);
                fprintf(stderr, "\n");
            }
            continue;
        }
        if (strncmp(input_buf, "/note", 5) == 0) {
            const char *arg = input_buf + 5;
            while (*arg == ' ')
                arg++;
            if (!*arg) {
                fprintf(stderr, "  %susage: /note <text>%s\n", TUI_DIM, TUI_RESET);
            } else {
                char note_buf[MAX_INPUT_LINE];
                snprintf(note_buf, sizeof(note_buf), "[note] %s", arg);
                conv_add_user_text(&conv, note_buf);
                tui_success("note added to context");
            }
            continue;
        }
        if (strncmp(input_buf, "/dialog", 7) == 0 &&
            (input_buf[7] == '\0' || input_buf[7] == ' ')) {
            const char *arg = input_buf + 7;
            while (*arg == ' ')
                arg++;
            if (!*arg) {
                fprintf(stderr,
                        "  %susage: /dialog <json-spec>  OR  "
                        "/dialog Question? | opt1 | opt2 [ ;; Q2? | a | b ]%s\n",
                        TUI_DIM, TUI_RESET);
                continue;
            }
            char *spec = (arg[0] == '{') ? safe_strdup(arg) : dialog_shorthand_to_json(arg);
            char *dres = malloc(MAX_INPUT_LINE);
            bool ok = dres && dsco_run_ask_dialog(spec, dres, MAX_INPUT_LINE);
            free(spec);
            if (!ok) {
                tui_error("dialog unavailable (needs an interactive terminal)");
                free(dres);
                continue;
            }
            /* Render answers and fall through so they become this turn's input;
             * cancel just returns to the prompt. */
            char summary[MAX_INPUT_LINE];
            bool proceed = dialog_answers_to_text(dres, summary, sizeof summary);
            free(dres);
            if (!proceed) {
                fprintf(stderr, "  %sdialog cancelled%s\n", TUI_DIM, TUI_RESET);
                continue;
            }
            snprintf(input_buf, MAX_INPUT_LINE, "%s", summary);
            /* no continue — input_buf is now the user message for the turn */
        }
        if (strncmp(input_buf, "/add-dir", 8) == 0) {
            const char *arg = input_buf + 8;
            while (*arg == ' ')
                arg++;
            if (!*arg)
                arg = ".";
            char dir_path[4096];
            if (!expand_home_path(arg, dir_path, sizeof(dir_path))) {
                tui_error("directory path too long");
            } else {
                jbuf_t ctx;
                jbuf_init(&ctx, 4096);
                if (!append_directory_listing(&ctx, dir_path)) {
                    int err = errno;
                    char msg[256];
                    snprintf(msg, sizeof(msg), "directory listing failed: %s", strerror(err));
                    tui_error(msg);
                } else {
                    conv_add_user_text(&conv, ctx.data ? ctx.data : "");
                    char msg[128];
                    snprintf(msg, sizeof(msg), "directory '%s' added to context", dir_path);
                    tui_success(msg);
                }
                jbuf_free(&ctx);
            }
            continue;
        }
        if (strncmp(input_buf, "/branch", 7) == 0) {
            const char *arg = input_buf + 7;
            while (*arg == ' ')
                arg++;
            const char *bname = (*arg) ? arg : "default";
            char safe[64] = {0};
            int si = 0;
            for (const char *c = bname; *c && si < 63; c++) {
                safe[si++] = (isalnum((unsigned char)*c) || *c == '-' || *c == '_') ? *c : '_';
            }
            const char *bhome = getenv("HOME");
            if (!bhome)
                bhome = "/tmp";
            char bpath[600];
            snprintf(bpath, sizeof(bpath), "%s/.dsco/sessions/_branch_%s.json", bhome, safe);
            if (conv_save(&conv, bpath)) {
                char msg[256];
                snprintf(msg, sizeof(msg), "branch '%s' saved (%d messages)", safe, conv.count);
                tui_success(msg);
            } else {
                tui_error("branch save failed");
            }
            continue;
        }
        if (strncmp(input_buf, "/pin", 4) == 0 && (input_buf[4] == '\0' || input_buf[4] == ' ')) {
            const char *arg = input_buf + 4;
            while (*arg == ' ')
                arg++;
            if (*arg == '\0') {
                if (session.pin_text[0]) {
                    fprintf(stderr, "  %spinned:%s %s\n", TUI_DIM, TUI_RESET, session.pin_text);
                    fprintf(stderr, "  %suse /unpin to clear%s\n", TUI_DIM, TUI_RESET);
                } else {
                    fprintf(stderr, "  %s(no pin set)%s\n", TUI_DIM, TUI_RESET);
                }
            } else {
                snprintf(session.pin_text, sizeof(session.pin_text), "%s", arg);
                tui_success("pinned context set (injected at conversation start)");
            }
            continue;
        }
        if (strcmp(input_buf, "/unpin") == 0) {
            session.pin_text[0] = '\0';
            tui_success("pinned context cleared");
            continue;
        }
        if (strncmp(input_buf, "/git", 4) == 0 && (input_buf[4] == '\0' || input_buf[4] == ' ')) {
            const char *arg = input_buf + 4;
            while (*arg == ' ')
                arg++;
            if (!*arg) {
                fprintf(stderr, "  %susage: /git <git-args>%s\n", TUI_DIM, TUI_RESET);
            } else {
                char gitcmd[1024];
                snprintf(gitcmd, sizeof(gitcmd), "git %s 2>&1", arg);
                FILE *gitfp = popen(gitcmd, "r");
                if (!gitfp) {
                    tui_error("popen failed");
                } else {
                    char gline[512];
                    fprintf(stderr, "\n");
                    bool gany = false;
                    while (fgets(gline, sizeof(gline), gitfp)) {
                        fprintf(stderr, "%s", gline);
                        gany = true;
                    }
                    pclose(gitfp);
                    if (!gany)
                        fprintf(stderr, "  %s(no output)%s\n", TUI_DIM, TUI_RESET);
                    fprintf(stderr, "\n");
                }
            }
            continue;
        }
        if (strncmp(input_buf, "/ask", 4) == 0 && input_buf[4] == ' ') {
            const char *arg = input_buf + 5;
            while (*arg == ' ')
                arg++;
            const char *aq = strchr(arg, ' ');
            if (!aq || !aq[1]) {
                fprintf(stderr, "  %susage: /ask <model> <question>%s\n", TUI_DIM, TUI_RESET);
            } else {
                char ask_model[128];
                size_t amlen = (size_t)(aq - arg);
                if (amlen >= sizeof(ask_model))
                    amlen = sizeof(ask_model) - 1;
                memcpy(ask_model, arg, amlen);
                ask_model[amlen] = '\0';
                const char *question = aq + 1;
                while (*question == ' ')
                    question++;
                const char *resolved = model_resolve_alias(ask_model);
                swarm_t *sw = tools_swarm_instance();
                const char *exe =
                    (sw && sw->dsco_path && sw->dsco_path[0]) ? sw->dsco_path : "dsco";
                char qexe[4096];
                char qmodel[256];
                char qquestion[(MAX_INPUT_LINE * 2) + 8];
                if (!shell_quote_single(exe, qexe, sizeof(qexe)) ||
                    !shell_quote_single(resolved, qmodel, sizeof(qmodel)) ||
                    !shell_quote_single(question, qquestion, sizeof(qquestion))) {
                    tui_error("ask command too long");
                } else {
                    jbuf_t acmd;
                    jbuf_init(&acmd, strlen(qexe) + strlen(qmodel) + strlen(qquestion) + 32);
                    jbuf_append(&acmd, qexe);
                    jbuf_append(&acmd, " -m ");
                    jbuf_append(&acmd, qmodel);
                    jbuf_append(&acmd, " ");
                    jbuf_append(&acmd, qquestion);
                    jbuf_append(&acmd, " 2>&1");
                    FILE *afp = popen(acmd.data, "r");
                    jbuf_free(&acmd);
                    if (!afp) {
                        tui_error("popen failed");
                    } else {
                        fprintf(stderr, "\n  %s[%s]%s ", TUI_BCYAN, resolved, TUI_RESET);
                        char aline[512];
                        bool aany = false;
                        while (fgets(aline, sizeof(aline), afp)) {
                            fprintf(stderr, "%s", aline);
                            aany = true;
                        }
                        pclose(afp);
                        if (!aany)
                            fprintf(stderr, "(no response)\n");
                        fprintf(stderr, "\n");
                    }
                }
            }
            continue;
        }
        if (strncmp(input_buf, "/alias", 6) == 0) {
            const char *arg = input_buf + 6;
            while (*arg == ' ')
                arg++;
            if (*arg == '\0') {
                if (alias_count == 0) {
                    fprintf(stderr, "  %s(no aliases defined)%s\n", TUI_DIM, TUI_RESET);
                } else {
                    for (int ai = 0; ai < alias_count; ai++) {
                        fprintf(stderr, "  %s%-20s%s -> %s\n", TUI_BCYAN, aliases[ai].name,
                                TUI_RESET, aliases[ai].expansion);
                    }
                }
            } else {
                const char *asp = strchr(arg, ' ');
                if (!asp) {
                    bool afound = false;
                    for (int ai = 0; ai < alias_count; ai++) {
                        if (strcmp(aliases[ai].name, arg) == 0) {
                            memmove(&aliases[ai], &aliases[ai + 1],
                                    (size_t)(alias_count - ai - 1) * sizeof(aliases[0]));
                            alias_count--;
                            tui_success("alias removed");
                            afound = true;
                            break;
                        }
                    }
                    if (!afound)
                        tui_warning("alias not found");
                } else {
                    char aname[64];
                    size_t alen = (size_t)(asp - arg);
                    if (alen >= sizeof(aname))
                        alen = sizeof(aname) - 1;
                    memcpy(aname, arg, alen);
                    aname[alen] = '\0';
                    const char *expansion = asp + 1;
                    while (*expansion == ' ')
                        expansion++;
                    bool aupdated = false;
                    for (int ai = 0; ai < alias_count; ai++) {
                        if (strcmp(aliases[ai].name, aname) == 0) {
                            snprintf(aliases[ai].expansion, sizeof(aliases[0].expansion), "%s",
                                     expansion);
                            tui_success("alias updated");
                            aupdated = true;
                            break;
                        }
                    }
                    if (!aupdated) {
                        if (alias_count >= MAX_ALIASES) {
                            tui_error("alias table full");
                        } else {
                            snprintf(aliases[alias_count].name, sizeof(aliases[0].name), "%s",
                                     aname);
                            snprintf(aliases[alias_count].expansion, sizeof(aliases[0].expansion),
                                     "%s", expansion);
                            alias_count++;
                            char amsg[128];
                            snprintf(amsg, sizeof(amsg), "alias '%s' -> '%s'", aname, expansion);
                            tui_success(amsg);
                        }
                    }
                }
            }
            continue;
        }
        if (strcmp(input_buf, "/version") == 0) {
            fprintf(stderr, "  dsco v%s (built %s, %s)\n", DSCO_VERSION, BUILD_DATE, GIT_HASH);
            continue;
        }
        /* /dht — distributed peer discovery over a private Kademlia overlay */
        if (strncmp(input_buf, "/dht", 4) == 0 && (input_buf[4] == '\0' || input_buf[4] == ' ')) {
            const char *arg = input_buf + 4;
            while (*arg == ' ')
                arg++;
            dsco_dht_t *d = dsco_dht_global();
            if (strncmp(arg, "start", 5) == 0) {
                const char *key = arg + 5;
                while (*key == ' ')
                    key++;
                if (!*key)
                    key = getenv("DSCO_DHT_SWARM");
                if (!key || !*key)
                    key = "dsco-mesh";
                uint16_t mp = 7337;
                const char *mpe = getenv("DSCO_MESH_PORT");
                if (mpe && atoi(mpe) > 0)
                    mp = (uint16_t)atoi(mpe);
                uint16_t dp = 7600;
                const char *dpe = getenv("DSCO_DHT_PORT");
                if (dpe && atoi(dpe) > 0)
                    dp = (uint16_t)atoi(dpe);
                dsco_dht_config_t dc = {.udp_port = dp, .mesh_port = mp, .swarm_key = key};
                d = dsco_dht_start(&dc);
                if (d)
                    tui_success("dht: joined overlay, announcing mesh port");
                else
                    tui_error("dht: failed to start (need libsodium + free UDP port)");
            } else if (strcmp(arg, "find") == 0) {
                if (d) {
                    dsco_dht_find_peers(d);
                    tui_success("dht: searching for peers");
                } else
                    tui_warning("dht: not running — /dht start first");
            } else if (strncmp(arg, "boot", 4) == 0) {
                const char *hp = arg + 4;
                while (*hp == ' ')
                    hp++;
                char host[256];
                const char *colon = strrchr(hp, ':');
                if (!d)
                    tui_warning("dht: not running — /dht start first");
                else if (!colon || colon == hp)
                    tui_error("usage: /dht boot <host:port>");
                else {
                    size_t hl = (size_t)(colon - hp);
                    if (hl >= sizeof(host))
                        hl = sizeof(host) - 1;
                    memcpy(host, hp, hl);
                    host[hl] = '\0';
                    if (dsco_dht_bootstrap(d, host, (uint16_t)atoi(colon + 1)))
                        tui_success("dht: bootstrap node added");
                    else
                        tui_error("dht: could not resolve/ping bootstrap node");
                }
            } else if (strcmp(arg, "stop") == 0) {
                if (d) {
                    dsco_dht_stop(d);
                    tui_success("dht: left overlay");
                } else
                    tui_warning("dht: not running");
            } else {
                /* status (default) */
                if (!d) {
                    fprintf(stderr, "  %sdht:%s not running\n", TUI_DIM, TUI_RESET);
                    fprintf(stderr,
                            "  %s/dht start [swarm-key]%s to join a private overlay "
                            "(or set DSCO_DHT_SWARM)\n",
                            TUI_DIM, TUI_RESET);
                } else {
                    dsco_dht_stats_t s;
                    dsco_dht_get_stats(d, &s);
                    fprintf(stderr,
                            "  %sdht:%s running · nodes: %d good / %d dubious / %d cached"
                            " · incoming %d\n",
                            TUI_DIM, TUI_RESET, s.good, s.dubious, s.cached, s.incoming);
                    fprintf(stderr, "  %s     peers discovered: %d · searches: %d%s\n", TUI_DIM,
                            s.peers_found, s.searches, TUI_RESET);
                }
            }
            continue;
        }
        /* /force — tool_choice control */
        if (strncmp(input_buf, "/force", 6) == 0) {
            const char *arg = input_buf + 6;
            while (*arg == ' ')
                arg++;
            if (*arg == '\0') {
                if (session.tool_choice[0]) {
                    fprintf(stderr, "  %stool_choice:%s %s\n", TUI_DIM, TUI_RESET,
                            session.tool_choice);
                } else {
                    fprintf(stderr, "  %stool_choice:%s auto (default)\n", TUI_DIM, TUI_RESET);
                }
                fprintf(
                    stderr,
                    "  %susage: /force <tool_name> | /force any | /force none | /force auto%s\n",
                    TUI_DIM, TUI_RESET);
            } else if (strcmp(arg, "auto") == 0) {
                session.tool_choice[0] = '\0';
                tui_success("tool_choice reset to auto");
            } else if (strcmp(arg, "any") == 0 || strcmp(arg, "none") == 0) {
                snprintf(session.tool_choice, sizeof(session.tool_choice), "%s", arg);
                char msg[64];
                snprintf(msg, sizeof(msg), "tool_choice set to %s", arg);
                tui_success(msg);
            } else {
                snprintf(session.tool_choice, sizeof(session.tool_choice), "tool:%s", arg);
                char msg[160];
                snprintf(msg, sizeof(msg), "next call will force tool: %s (resets to auto after)",
                         arg);
                tui_success(msg);
            }
            baseline_log("command", "/force", session.tool_choice, NULL);
            continue;
        }
        /* /prefill — seed assistant response */
        if (strncmp(input_buf, "/prefill", 8) == 0) {
            const char *arg = input_buf + 8;
            while (*arg == ' ')
                arg++;
            if (*arg == '\0') {
                if (session.prefill[0]) {
                    fprintf(stderr, "  %sprefill:%s %s\n", TUI_DIM, TUI_RESET, session.prefill);
                } else {
                    fprintf(stderr, "  %sno prefill set%s\n", TUI_DIM, TUI_RESET);
                }
                fprintf(stderr, "  %susage: /prefill { (for JSON) or /prefill <text>%s\n", TUI_DIM,
                        TUI_RESET);
            } else if (strcmp(arg, "clear") == 0) {
                session.prefill[0] = '\0';
                tui_success("prefill cleared");
            } else {
                snprintf(session.prefill, sizeof(session.prefill), "%s", arg);
                char msg[128];
                snprintf(msg, sizeof(msg), "next response will start with: %s", session.prefill);
                tui_success(msg);
            }
            continue;
        }
        /* /json — shortcut to force JSON output via prefill */
        if (strcmp(input_buf, "/json") == 0) {
            snprintf(session.prefill, sizeof(session.prefill), "{");
            tui_success("JSON mode: next response will be JSON (prefill=\"{\")");
            continue;
        }
        /* /web — toggle server-side web search */
        if (strcmp(input_buf, "/web") == 0 || strcmp(input_buf, "/web on") == 0 ||
            strcmp(input_buf, "/web off") == 0) {
            if (strcmp(input_buf, "/web on") == 0)
                session.web_search = true;
            else if (strcmp(input_buf, "/web off") == 0)
                session.web_search = false;
            else
                session.web_search = !session.web_search;
            char msg[64];
            snprintf(msg, sizeof(msg), "web search %s",
                     session.web_search ? "enabled" : "disabled");
            tui_success(msg);
            baseline_log("command", "/web", session.web_search ? "on" : "off", NULL);
            continue;
        }
        /* /code — toggle server-side code execution */
        if (strcmp(input_buf, "/code") == 0 || strcmp(input_buf, "/code on") == 0 ||
            strcmp(input_buf, "/code off") == 0) {
            if (strcmp(input_buf, "/code on") == 0)
                session.code_execution = true;
            else if (strcmp(input_buf, "/code off") == 0)
                session.code_execution = false;
            else
                session.code_execution = !session.code_execution;
            char msg[64];
            snprintf(msg, sizeof(msg), "code execution %s",
                     session.code_execution ? "enabled" : "disabled");
            tui_success(msg);
            baseline_log("command", "/code", session.code_execution ? "on" : "off", NULL);
            continue;
        }
        /* ── /image — image attachment help ──────────────────────────────── */
        if (strcmp(input_buf, "/image") == 0) {
            tui_info("Image attachment — two ways:\n"
                     "  \033[1m@\033[0m in the composer  → live image browser (↑/↓ navigate, Tab/Enter insert)\n"
                     "  \033[1mAlt+I\033[0m              → paste image from system clipboard\n"
                     "  drag & drop path   → type or paste any image path directly\n"
                     "Supported: png jpg jpeg gif webp bmp tif heic heif avif");
            baseline_log("command", "/image", NULL, NULL);
            continue;
        }
        /* ── /sessions, /resume, /new, /rename — session workspace ─────── */
        if (strncmp(input_buf, "/sessions", 9) == 0 &&
            (input_buf[9] == '\0' || input_buf[9] == ' ')) {
            saved_session_entry_t *entries = session_entries_alloc();
            int count = session_load_entries(entries, DSCO_SESSION_LIST_MAX, &session);
            session_print_entries(entries, count);
            free(entries);
            baseline_log("command", "/sessions", NULL, NULL);
            continue;
        }
        if (strncmp(input_buf, "/resume", 7) == 0 &&
            (input_buf[7] == '\0' || input_buf[7] == ' ')) {
            const char *query = input_buf + 7;
            while (*query == ' ')
                query++;

            saved_session_entry_t *entries = session_entries_alloc();
            int count = session_load_entries(entries, DSCO_SESSION_LIST_MAX, &session);
            if (!query[0]) {
                session_print_entries(entries, count);
                free(entries);
                continue;
            }

            bool ambiguous = false;
            int idx = session_find_entry(entries, count, query, &ambiguous);
            if (idx < 0) {
                tui_error(ambiguous ? "session query is ambiguous; use the numeric index"
                                    : "session not found");
                if (count > 0)
                    session_print_entries(entries, count);
                free(entries);
                continue;
            }

            if (session_switch_to_file(&conv, &session, &status_bar, entries[idx].name,
                                       entries[idx].path, true)) {
                char msg[512];
                snprintf(msg, sizeof(msg), "resumed '%s' (%s, %d messages)", entries[idx].name,
                         session.model[0] ? session.model : "?", conv.count);
                tui_success(msg);
                baseline_log("command", "/resume", entries[idx].name, NULL);
            } else {
                tui_error("failed to resume session");
                baseline_log("error", "/resume", "load failed", NULL);
            }
            free(entries);
            continue;
        }
        if (strncmp(input_buf, "/new", 4) == 0 && (input_buf[4] == '\0' || input_buf[4] == ' ')) {
            const char *rest = input_buf + 4;
            while (*rest == ' ')
                rest++;

            char raw_name[128] = "";
            char model_arg[128] = "";
            if (rest[0]) {
                const char *sp = strchr(rest, ' ');
                if (sp) {
                    snprintf(raw_name, sizeof(raw_name), "%.*s", (int)(sp - rest), rest);
                    while (*sp == ' ')
                        sp++;
                    snprintf(model_arg, sizeof(model_arg), "%s", sp);
                } else {
                    snprintf(raw_name, sizeof(raw_name), "%s", rest);
                }
            } else {
                time_t now = time(NULL);
                struct tm *tm = localtime(&now);
                if (tm)
                    strftime(raw_name, sizeof(raw_name), "session-%Y%m%d-%H%M%S", tm);
                else
                    snprintf(raw_name, sizeof(raw_name), "session");
            }

            char new_name[64];
            session_sanitize_name(raw_name, new_name, sizeof(new_name));

            char cur_path[1024];
            session_file_path(session_current_name(&session), cur_path, sizeof(cur_path));
            conv_save_ex(&conv, &session, cur_path);

            conv_free(&conv);
            conv_init(&conv);
            session_reset_usage_for_new(&session);
            snprintf(session.slot_name, sizeof(session.slot_name), "%s", new_name);
            if (model_arg[0]) {
                const char *resolved = model_resolve_alias(model_arg);
                snprintf(session.model, sizeof(session.model), "%s",
                         resolved ? resolved : model_arg);
                session.context_window = model_context_window(session.model);
                session.model_locked = true;
            }
            tools_set_runtime_model(session.model);
            tui_status_bar_set_model(&status_bar, session.model, session.slot_name);

            char new_path[1024];
            session_file_path(new_name, new_path, sizeof(new_path));
            conv_save_ex(&conv, &session, new_path);

            char msg[256];
            snprintf(msg, sizeof(msg), "new session '%s' (%s)", new_name,
                     session.model[0] ? session.model : "?");
            tui_success(msg);
            baseline_log("command", "/new", new_name, NULL);
            continue;
        }
        if (strncmp(input_buf, "/rename", 7) == 0 &&
            (input_buf[7] == '\0' || input_buf[7] == ' ')) {
            const char *name_arg = input_buf + 7;
            while (*name_arg == ' ')
                name_arg++;
            if (!name_arg[0]) {
                tui_error("usage: /rename <new-name>");
                continue;
            }

            char old_name[64];
            snprintf(old_name, sizeof(old_name), "%s", session_current_name(&session));
            char old_path[1024], new_path[1024], new_name[64];
            session_sanitize_name(name_arg, new_name, sizeof(new_name));
            session_file_path(old_name, old_path, sizeof(old_path));
            session_file_path(new_name, new_path, sizeof(new_path));

            snprintf(session.slot_name, sizeof(session.slot_name), "%s", new_name);
            if (conv_save_ex(&conv, &session, new_path)) {
                if (strcmp(old_path, new_path) != 0)
                    unlink(old_path);
                tui_status_bar_set_model(&status_bar, session.model, session.slot_name);
                char msg[256];
                snprintf(msg, sizeof(msg), "renamed session '%s' to '%s'", old_name, new_name);
                tui_success(msg);
                baseline_log("command", "/rename", new_name, NULL);
            } else {
                snprintf(session.slot_name, sizeof(session.slot_name), "%s",
                         strcmp(old_name, "default") == 0 ? "" : old_name);
                tui_status_bar_set_model(&status_bar, session.model, session.slot_name);
                tui_error("failed to rename session");
            }
            continue;
        }
        /* ── /slot — workspace multiplexer ──────────────────────────────── */
        if (strncmp(input_buf, "/slot", 5) == 0 && (input_buf[5] == '\0' || input_buf[5] == ' ')) {
            const char *slot_args = input_buf + 5;
            while (*slot_args == ' ')
                slot_args++;

            const char *home = getenv("HOME");
            if (!home)
                home = "/tmp";
            char slot_dir[512];
            snprintf(slot_dir, sizeof(slot_dir), "%s/.dsco/sessions", home);
            mkdir(slot_dir, 0755);

            /* /slot  or  /slot list — list all slots */
            if (slot_args[0] == '\0' || strncmp(slot_args, "list", 4) == 0) {
                saved_session_entry_t *entries = session_entries_alloc();
                int count = session_load_entries(entries, DSCO_SESSION_LIST_MAX, &session);
                session_print_entries(entries, count);
                free(entries);
                continue;
            }

            /* /slot del <name> */
            if (strncmp(slot_args, "del ", 4) == 0 || strncmp(slot_args, "delete ", 7) == 0) {
                const char *target = slot_args + (slot_args[1] == 'e' && slot_args[2] == 'l'
                                                      ? (slot_args[3] == ' ' ? 4 : 7)
                                                      : 4);
                while (*target == ' ')
                    target++;
                if (!target[0]) {
                    tui_error("usage: /slot del <name>");
                    continue;
                }
                char del_path[768];
                snprintf(del_path, sizeof(del_path), "%s/%s.json", slot_dir, target);
                if (unlink(del_path) == 0) {
                    char msg[256];
                    snprintf(msg, sizeof(msg), "slot '%s' deleted", target);
                    tui_success(msg);
                } else {
                    char msg[256];
                    snprintf(msg, sizeof(msg), "slot '%s' not found", target);
                    tui_error(msg);
                }
                continue;
            }

            /* /slot new <name> [model] */
            if (strncmp(slot_args, "new ", 4) == 0) {
                const char *rest = slot_args + 4;
                while (*rest == ' ')
                    rest++;
                char new_name[64] = "";
                char new_model[128] = "";
                const char *sp = strchr(rest, ' ');
                if (sp) {
                    snprintf(new_name, sizeof(new_name), "%.*s", (int)(sp - rest), rest);
                    const char *marg = sp + 1;
                    while (*marg == ' ')
                        marg++;
                    snprintf(new_model, sizeof(new_model), "%s", marg);
                } else {
                    snprintf(new_name, sizeof(new_name), "%s", rest);
                }
                if (!new_name[0]) {
                    tui_error("usage: /slot new <name> [model]");
                    continue;
                }
                char safe_new_name[64];
                session_sanitize_name(new_name, safe_new_name, sizeof(safe_new_name));

                /* Save current slot before switching */
                const char *cur_slot = session.slot_name[0] ? session.slot_name : "default";
                char cur_path[768];
                snprintf(cur_path, sizeof(cur_path), "%s/%s.json", slot_dir, cur_slot);
                conv_save_ex(&conv, &session, cur_path);

                /* Switch to new slot */
                conv_free(&conv);
                conv_init(&conv);
                session_reset_usage_for_new(&session);
                snprintf(session.slot_name, sizeof(session.slot_name), "%s", safe_new_name);
                if (new_model[0]) {
                    const char *resolved = model_resolve_alias(new_model);
                    snprintf(session.model, sizeof(session.model), "%s",
                             resolved ? resolved : new_model);
                    session.context_window = model_context_window(session.model);
                    session.model_locked = true;
                    tools_set_runtime_model(session.model);
                    tui_status_bar_set_model(&status_bar, session.model, session.slot_name);
                } else {
                    tui_status_bar_set_model(&status_bar, session.model, session.slot_name);
                }
                /* Save the new (empty) slot immediately so it appears in /slot list */
                char new_path[768];
                snprintf(new_path, sizeof(new_path), "%s/%s.json", slot_dir, safe_new_name);
                conv_save_ex(&conv, &session, new_path);

                char msg[256];
                snprintf(msg, sizeof(msg), "new slot '%s'%s%s", safe_new_name,
                         new_model[0] ? " → " : "", new_model[0] ? session.model : "");
                tui_success(msg);
                baseline_log("command", "/slot new", safe_new_name, NULL);
                continue;
            }

            /* /slot <name> — switch to existing slot */
            {
                char target_name[64];
                snprintf(target_name, sizeof(target_name), "%s", slot_args);

                /* Save current slot */
                const char *cur_slot = session.slot_name[0] ? session.slot_name : "default";
                char cur_path[768];
                snprintf(cur_path, sizeof(cur_path), "%s/%s.json", slot_dir, cur_slot);
                conv_save_ex(&conv, &session, cur_path);

                /* Load target */
                char load_path[768];
                snprintf(load_path, sizeof(load_path), "%s/%s.json", slot_dir, target_name);
                if (conv_load_ex(&conv, &session, load_path)) {
                    snprintf(session.slot_name, sizeof(session.slot_name), "%s", target_name);
                    session.model_locked = true;
                    tools_set_runtime_model(session.model);
                    tui_status_bar_set_model(&status_bar, session.model, session.slot_name);
                    char msg[256];
                    snprintf(msg, sizeof(msg), "slot '%s' (%s, %d messages)", target_name,
                             session.model[0] ? session.model : "?", conv.count);
                    tui_success(msg);
                    baseline_log("command", "/slot", target_name, NULL);
                } else {
                    char msg[256];
                    snprintf(msg, sizeof(msg), "slot '%s' not found — use /slot new %s [model]",
                             target_name, target_name);
                    tui_error(msg);
                }
                continue;
            }
        }
        if (strncmp(input_buf, "/save", 5) == 0) {
            const char *name = input_buf + 5;
            while (*name == ' ')
                name++;
            if (*name == '\0')
                name = "default";

            char dir_path[512];
            const char *home = getenv("HOME");
            if (!home)
                home = "/tmp";
            snprintf(dir_path, sizeof(dir_path), "%s/.dsco/sessions", home);
            mkdir(dir_path, 0755);

            char save_path[1024];
            snprintf(save_path, sizeof(save_path), "%s/%s.json", dir_path, name);

            if (conv_save_ex(&conv, &session, save_path)) {
                char msg[1100];
                snprintf(msg, sizeof(msg), "session saved to %s (%d messages)", save_path,
                         conv.count);
                tui_success(msg);
                baseline_log("command", "/save", save_path, NULL);
            } else {
                tui_error("failed to save session");
                baseline_log("error", "/save", "save failed", NULL);
            }
            continue;
        }
        if (strncmp(input_buf, "/load", 5) == 0) {
            const char *name = input_buf + 5;
            while (*name == ' ')
                name++;
            if (*name == '\0')
                name = "default";

            char load_path[1024];
            const char *home = getenv("HOME");
            if (!home)
                home = "/tmp";
            snprintf(load_path, sizeof(load_path), "%s/.dsco/sessions/%s.json", home, name);

            if (conv_load_ex(&conv, &session, load_path)) {
                snprintf(session.slot_name, sizeof(session.slot_name), "%s", name);
                session.model_locked = true;
                tools_set_runtime_model(session.model);
                tui_status_bar_set_model(&status_bar, session.model, session.slot_name);
                char msg[1100];
                snprintf(msg, sizeof(msg), "session loaded from %s (%d messages)", load_path,
                         conv.count);
                tui_success(msg);
                /* F18: Session diff summary */
                {
                    int tc = 0, est = 0;
                    for (int i = 0; i < conv.count; i++) {
                        for (int j = 0; j < conv.msgs[i].content_count; j++) {
                            if (conv.msgs[i].content[j].tool_name)
                                tc++;
                            if (conv.msgs[i].content[j].text)
                                est += tui_estimate_tokens(conv.msgs[i].content[j].text);
                        }
                    }
                    tui_session_diff(conv.count, tc, est, session.model);
                }
                baseline_log("command", "/load", load_path, NULL);
            } else {
                char msg[1100];
                snprintf(msg, sizeof(msg), "failed to load session '%s'", name);
                tui_error(msg);
                baseline_log("error", "/load", "load failed", NULL);
            }
            continue;
        }
        if (strcmp(input_buf, "/workspace") == 0 ||
            strcmp(input_buf, "/workspace bootstrap") == 0 ||
            strcmp(input_buf, "/workspace reload") == 0 ||
            strcmp(input_buf, "/workspace prompt") == 0) {
            if (strcmp(input_buf, "/workspace bootstrap") == 0) {
                char summary[768];
                int rc = dsco_workspace_bootstrap(summary, sizeof(summary));
                if (rc < 0)
                    tui_error(summary);
                else
                    tui_success(summary);
            } else if (strcmp(input_buf, "/workspace reload") == 0) {
                dsco_workspace_prompt_invalidate();
                tui_success("workspace prompt cache reloaded");
            } else if (strcmp(input_buf, "/workspace prompt") == 0) {
                const char *prompt = dsco_workspace_prompt();
                if (prompt && *prompt)
                    fprintf(stderr, "\n%s\n\n", prompt);
                else
                    tui_info("workspace prompt is empty");
            } else {
                dsco_workspace_status_t ws;
                char summary[768];
                dsco_workspace_status(&ws, summary, sizeof(summary));
                fprintf(stderr, "\n");
                tui_header("Workspace", TUI_BCYAN);
                fprintf(stderr, "  %sRoot:%s        %s\n", TUI_DIM, TUI_RESET,
                        dsco_workspace_root());
                fprintf(stderr, "  %sIdentity:%s    %s\n", TUI_DIM, TUI_RESET,
                        ws.has_identity ? "present" : "missing");
                fprintf(stderr, "  %sUser:%s        %s\n", TUI_DIM, TUI_RESET,
                        ws.has_user ? "present" : "missing");
                fprintf(stderr, "  %sSoul:%s        %s\n", TUI_DIM, TUI_RESET,
                        ws.has_soul ? "present" : "missing");
                fprintf(stderr, "  %sMemory:%s      %s\n", TUI_DIM, TUI_RESET,
                        ws.has_memory ? "present" : "missing");
                fprintf(stderr, "  %sSkills:%s      %d installed\n", TUI_DIM, TUI_RESET,
                        ws.installed_skills);
                fprintf(stderr, "  %sActive skill:%s %s\n", TUI_DIM, TUI_RESET,
                        session.active_skill[0] ? session.active_skill : "(none)");
                fprintf(stderr, "  %sLegacy prompt:%s %s\n\n", TUI_DIM, TUI_RESET,
                        ws.has_legacy_prompt ? "present" : "missing");
            }
            baseline_log("command", "/workspace", NULL, NULL);
            continue;
        }
        if (strcmp(input_buf, "/skills") == 0 || strncmp(input_buf, "/skills ", 8) == 0) {
            const char *arg = input_buf + 7;
            while (*arg == ' ')
                arg++;
            if (*arg == '\0') {
                char out[8192];
                int rc = dsco_workspace_list_skills(out, sizeof(out));
                if (rc < 0)
                    tui_error(out);
                else
                    fprintf(stderr, "\n%s\n\n", out);
            } else if (strncmp(arg, "show ", 5) == 0) {
                const char *name = arg + 5;
                while (*name == ' ')
                    name++;
                char out[16384];
                if (dsco_workspace_show_skill(name, out, sizeof(out)) < 0)
                    tui_error(out);
                else
                    fprintf(stderr, "\n%s\n\n", out);
            } else if (strncmp(arg, "use ", 4) == 0) {
                const char *name = arg + 4;
                while (*name == ' ')
                    name++;
                char out[4096];
                if (dsco_workspace_show_skill(name, out, sizeof(out)) < 0) {
                    tui_error(out);
                } else {
                    snprintf(session.active_skill, sizeof(session.active_skill), "%s", name);
                    char msg[192];
                    snprintf(msg, sizeof(msg), "active skill set to %s", name);
                    tui_success(msg);
                }
            } else if (strcmp(arg, "clear") == 0 || strcmp(arg, "off") == 0) {
                session.active_skill[0] = '\0';
                tui_success("active skill cleared");
            } else {
                tui_error(
                    "usage: /skills | /skills show <name> | /skills use <name> | /skills clear");
            }
            baseline_log("command", "/skills", NULL, NULL);
            continue;
        }
        if (strcmp(input_buf, "/topology") == 0 || strncmp(input_buf, "/topology ", 10) == 0) {
            const char *arg = input_buf + 9;
            while (*arg == ' ')
                arg++;
            if (*arg == '\0') {
                fprintf(stderr, "\n");
                tui_header("Topology", TUI_BYELLOW);
                fprintf(stderr, "  %sActive:%s      %s\n", TUI_DIM, TUI_RESET,
                        session.active_topology[0] ? session.active_topology : "(none)");
                fprintf(stderr, "  %sAuto:%s        %s\n", TUI_DIM, TUI_RESET,
                        session.topology_auto ? "on" : "off");
                fprintf(stderr,
                        "  %sUsage:%s       /topology list | show <name> | use <name> | run <name> "
                        "<task> | auto | off\n\n",
                        TUI_DIM, TUI_RESET);
            } else if (strcmp(arg, "list") == 0) {
                print_topology_registry_brief();
            } else if (strncmp(arg, "show ", 5) == 0) {
                const char *name = arg + 5;
                while (*name == ' ')
                    name++;
                const topology_t *topo = topology_find(name);
                if (!topo)
                    tui_error("topology not found");
                else
                    print_topology_summary(topo);
            } else if (strncmp(arg, "use ", 4) == 0) {
                const char *name = arg + 4;
                while (*name == ' ')
                    name++;
                const topology_t *topo = topology_find(name);
                if (!topo) {
                    tui_error("topology not found");
                } else {
                    snprintf(session.active_topology, sizeof(session.active_topology), "%s",
                             topo->name);
                    session.topology_auto = false;
                    char msg[192];
                    snprintf(msg, sizeof(msg), "active topology set to %s", topo->name);
                    tui_success(msg);
                }
            } else if (strncmp(arg, "run ", 4) == 0) {
                char *copy = safe_strdup(arg + 4);
                char *name = copy;
                while (*name == ' ')
                    name++;
                char *task = name;
                while (*task && *task != ' ')
                    task++;
                if (*task) {
                    *task++ = '\0';
                    while (*task == ' ')
                        task++;
                }
                const topology_t *topo = topology_find(name);
                if (!topo || !task || !*task) {
                    tui_error("usage: /topology run <name> <task>");
                } else {
                    char saved_topology[sizeof(session.active_topology)];
                    bool saved_auto = session.topology_auto;
                    snprintf(saved_topology, sizeof(saved_topology), "%s", session.active_topology);
                    snprintf(session.active_topology, sizeof(session.active_topology), "%s",
                             topo->name);
                    session.topology_auto = false;
                    (void)run_topology_prompt(&session, api_key, &conv, task, &last_input_tokens);
                    snprintf(session.active_topology, sizeof(session.active_topology), "%s",
                             saved_topology);
                    session.topology_auto = saved_auto;
                }
                free(copy);
            } else if (strcmp(arg, "auto") == 0 || strcmp(arg, "auto on") == 0) {
                session.topology_auto = true;
                session.active_topology[0] = '\0';
                tui_success("topology auto-selection enabled");
            } else if (strcmp(arg, "off") == 0 || strcmp(arg, "clear") == 0 ||
                       strcmp(arg, "auto off") == 0) {
                session.topology_auto = false;
                session.active_topology[0] = '\0';
                tui_success("topology selection disabled");
            } else {
                tui_error("usage: /topology list | show <name> | use <name> | run <name> <task> | "
                          "auto | off");
            }
            baseline_log("command", "/topology",
                         session.active_topology[0] ? session.active_topology : NULL, NULL);
            continue;
        }
        if (strcmp(input_buf, "/swarm") == 0 || strncmp(input_buf, "/swarm ", 7) == 0) {
            const char *arg = input_buf + 6;
            while (*arg == ' ')
                arg++;
            if (*arg == '\0' || strcmp(arg, "status") == 0 || strcmp(arg, "list") == 0) {
                print_swarm_summary(-1, true);
            } else if (strncmp(arg, "show ", 5) == 0) {
                int gid = atoi(arg + 5);
                swarm_t *sw = tools_swarm_instance();
                if (gid < 0 || gid >= sw->group_count)
                    tui_error("invalid group id");
                else
                    print_swarm_summary(gid, true);
            } else if (strncmp(arg, "wait ", 5) == 0) {
                int gid = -1;
                int timeout = 300;
                if (sscanf(arg + 5, "%d %d", &gid, &timeout) < 1) {
                    tui_error("usage: /swarm wait <group_id> [timeout_s]");
                } else {
                    char in[128];
                    char out[MAX_RESPONSE_SIZE];
                    snprintf(in, sizeof(in), "{\"group_id\":%d,\"timeout\":%d}", gid, timeout);
                    if (!tools_execute("swarm_collect", in, out, sizeof(out))) {
                        tui_error(out);
                    } else {
                        print_swarm_summary(gid, true);
                    }
                }
            } else if (strncmp(arg, "kill-group ", 11) == 0) {
                int gid = atoi(arg + 11);
                swarm_t *sw = tools_swarm_instance();
                if (gid < 0 || gid >= sw->group_count) {
                    tui_error("invalid group id");
                } else {
                    swarm_group_kill(sw, gid);
                    swarm_poll(sw, 0);
                    tui_success("swarm group terminated");
                    print_swarm_summary(gid, true);
                }
            } else if (strncmp(arg, "kill ", 5) == 0) {
                int aid = atoi(arg + 5);
                swarm_t *sw = tools_swarm_instance();
                if (!swarm_kill(sw, aid))
                    tui_error("agent not running");
                else {
                    tui_success("swarm agent terminated");
                    print_swarm_summary(-1, true);
                }
            } else {
                tui_error("usage: /swarm | status | show <group_id> | wait <group_id> [timeout_s] "
                          "| kill <agent_id> | kill-group <group_id>");
            }
            baseline_log("command", "/swarm", arg && *arg ? arg : NULL, NULL);
            continue;
        }
        if (strcmp(input_buf, "/identity") == 0 || strcmp(input_buf, "/user") == 0 ||
            strcmp(input_buf, "/soul") == 0 || strcmp(input_buf, "/memory") == 0) {
            const char *doc = input_buf + 1;
            char out[16384];
            if (dsco_workspace_read_doc(doc, out, sizeof(out)) < 0)
                tui_error(out);
            else
                fprintf(stderr, "\n%s\n\n", out);
            baseline_log("command", doc, NULL, NULL);
            continue;
        }
        if (strcmp(input_buf, "/setup") == 0 || strcmp(input_buf, "/setup --force") == 0) {
            bool force = (strcmp(input_buf, "/setup --force") == 0);
            char summary[768];
            int discovered = dsco_setup_autopopulate(force, true, summary, sizeof(summary));
            if (discovered < 0) {
                tui_error(summary);
                baseline_log("setup", "setup_failed", summary, NULL);
            } else {
                tui_success(summary);
                baseline_log("setup", force ? "setup_force" : "setup", summary, NULL);
            }
            continue;
        }
        if (strcmp(input_buf, "/setup report") == 0) {
            char report[32768];
            if (dsco_setup_report(report, sizeof(report)) < 0) {
                tui_error("setup report failed");
                baseline_log("setup", "setup_report_failed", NULL, NULL);
            } else {
                fprintf(stderr, "\n%s\n", report);
                baseline_log("setup", "setup_report", dsco_setup_env_path(), NULL);
            }
            continue;
        }
        /* ── /agents ──────────────────────────────────────────────── */
        if (strcmp(input_buf, "/agents") == 0 || strncmp(input_buf, "/agents ", 8) == 0) {
            agent_profiles_init();
            const char *arg = input_buf + 7;
            while (*arg == ' ')
                arg++;

            if (*arg == '\0' || strcmp(arg, "list") == 0) {
                /* List all profiles */
                int n = agent_profiles_count();
                const char *active = agent_profile_active_name();
                fprintf(stderr, "\n");
                tui_header("Agent Profiles", TUI_BCYAN);
                bool use_rgb = tui_detect_color_level() >= TUI_COLOR_256;
                if (n == 0) {
                    fprintf(stderr, "  %sno profiles defined%s  %s/agents new <name>%s\n", TUI_DIM,
                            TUI_RESET, TUI_DIM, TUI_RESET);
                } else {
                    for (int i = 0; i < n; i++) {
                        const agent_profile_t *p = agent_profile_get(i);
                        bool is_active = (active && strcmp(active, p->name) == 0);
                        /* ⏺ name  description  groups:N tools:N  [active] */
                        if (use_rgb) {
                            fprintf(stderr,
                                    "  \033[38;2;255;95;0m" TUI_RECORD "\033[0m"
                                    " \033[1m%-18s\033[0m  \033[2m%s\033[0m",
                                    p->name, p->description[0] ? p->description : "no description");
                        } else {
                            fprintf(
                                stderr, "  " TUI_ORANGE TUI_RECORD TUI_RESET " %s%-18s%s  %s%s%s",
                                TUI_BOLD, p->name, TUI_RESET, TUI_DIM,
                                p->description[0] ? p->description : "no description", TUI_RESET);
                        }
                        /* group/tool counts */
                        if (p->group_count > 0 || p->tool_count > 0) {
                            fprintf(stderr, "  %s", TUI_DIM);
                            if (p->group_count > 0)
                                fprintf(stderr, "%d groups", p->group_count);
                            if (p->group_count > 0 && p->tool_count > 0)
                                fprintf(stderr, " + ");
                            if (p->tool_count > 0)
                                fprintf(stderr, "%d tools", p->tool_count);
                            fprintf(stderr, "%s", TUI_RESET);
                        }
                        /* active badge */
                        if (is_active)
                            fprintf(stderr, "  %s● active%s", TUI_BGREEN, TUI_RESET);
                        fprintf(stderr, "\n");
                    }
                }
                fprintf(stderr,
                        "\n  %suse <name> · off · show <name> · new <name> · edit <name> · delete "
                        "<name> · groups%s\n\n",
                        TUI_DIM, TUI_RESET);

            } else if (strncmp(arg, "use ", 4) == 0) {
                const char *name = arg + 4;
                while (*name == ' ')
                    name++;
                if (!*name) {
                    tui_error("usage: /agents use <name>");
                    goto agents_done;
                }
                if (!agent_profile_find(name)) {
                    char err[128];
                    snprintf(err, sizeof(err), "profile not found: %s", name);
                    tui_error(err);
                    goto agents_done;
                }
                agent_profile_set_active(name);
                tools_set_profile_filter(agent_profile_tool_allowed);
                char ok[128];
                snprintf(ok, sizeof(ok), "agent profile active: %s", name);
                tui_success(ok);

            } else if (strcmp(arg, "off") == 0) {
                agent_profile_set_active(NULL);
                tools_clear_profile_filter();
                tui_success("agent profile deactivated — all tools available");

            } else if (strncmp(arg, "show ", 5) == 0) {
                const char *name = arg + 5;
                while (*name == ' ')
                    name++;
                const agent_profile_t *p = agent_profile_find(name);
                if (!p) {
                    char err[128];
                    snprintf(err, sizeof(err), "profile not found: %s", name);
                    tui_error(err);
                    goto agents_done;
                }
                fprintf(stderr, "\n");
                tui_header(p->name, TUI_BCYAN);
                fprintf(stderr, "  %sdescription  %s%s\n", TUI_DIM, TUI_RESET, p->description);
                if (p->model_hint[0])
                    fprintf(stderr, "  %smodel        %s%s\n", TUI_DIM, TUI_RESET, p->model_hint);
                if (p->budget_usd > 0)
                    fprintf(stderr, "  %sbudget       %s$%.4f\n", TUI_DIM, TUI_RESET,
                            p->budget_usd);
                if (p->prompt_prefix[0])
                    fprintf(stderr, "  %sprompt       %s%s\n", TUI_DIM, TUI_RESET,
                            p->prompt_prefix);
                if (p->group_count > 0) {
                    fprintf(stderr, "  %sgroups       %s", TUI_DIM, TUI_RESET);
                    for (int i = 0; i < p->group_count; i++)
                        fprintf(stderr, "%s%s", i > 0 ? "  " : "", p->groups[i]);
                    fprintf(stderr, "\n");
                }
                if (p->tool_count > 0) {
                    fprintf(stderr, "  %stools (%d)%s\n", TUI_DIM, p->tool_count, TUI_RESET);
                    for (int i = 0; i < p->tool_count; i++)
                        fprintf(stderr, "    %s%s%s\n", TUI_DIM, p->tools[i], TUI_RESET);
                }
                if (p->group_count == 0 && p->tool_count == 0)
                    fprintf(stderr, "  %sfilter        none  (all tools available)%s\n", TUI_DIM,
                            TUI_RESET);
                fprintf(stderr, "\n");

            } else if (strncmp(arg, "new ", 4) == 0) {
                const char *name = arg + 4;
                while (*name == ' ')
                    name++;
                if (!*name) {
                    tui_error("usage: /agents new <name>");
                    goto agents_done;
                }
                if (agent_profile_find(name)) {
                    char err[128];
                    snprintf(err, sizeof(err), "profile already exists: %s (use /agents edit)",
                             name);
                    tui_error(err);
                    goto agents_done;
                }
                agent_profile_t p;
                memset(&p, 0, sizeof(p));
                snprintf(p.name, sizeof(p.name), "%s", name);

                /* Interactive prompts */
                fprintf(stderr, "  %sdescription%s  ", TUI_DIM, TUI_RESET);
                fflush(stderr);
                char line[256];
                if (fgets(line, sizeof(line), stdin)) {
                    line[strcspn(line, "\n")] = '\0';
                    snprintf(p.description, sizeof(p.description), "%s", line);
                }

                fprintf(stderr, "  %stool groups%s  %s(comma-sep, enter for all)%s\n", TUI_DIM,
                        TUI_RESET, TUI_DIM, TUI_RESET);
                fprintf(stderr,
                        "  %sfile_io  git  network  shell  code  crypto  swarm  ast  pipeline  "
                        "math  search  general  market  prediction  memory%s\n",
                        TUI_DIM, TUI_RESET);
                fprintf(stderr, "  > ");
                fflush(stderr);
                if (fgets(line, sizeof(line), stdin)) {
                    line[strcspn(line, "\n")] = '\0';
                    char *tok = strtok(line, ", ");
                    while (tok && p.group_count < AP_MAX_GROUPS) {
                        while (*tok == ' ')
                            tok++;
                        if (*tok)
                            snprintf(p.groups[p.group_count++], 32, "%s", tok);
                        tok = strtok(NULL, ", ");
                    }
                }

                fprintf(stderr, "  %smodel hint%s   ", TUI_DIM, TUI_RESET);
                fflush(stderr);
                if (fgets(line, sizeof(line), stdin)) {
                    line[strcspn(line, "\n")] = '\0';
                    snprintf(p.model_hint, sizeof(p.model_hint), "%s", line);
                }

                if (agent_profile_save(&p)) {
                    char ok[128];
                    snprintf(ok, sizeof(ok), "profile created: %s", name);
                    tui_success(ok);
                    fprintf(stderr, "  %s/agents use %s%s\n\n", TUI_DIM, name, TUI_RESET);
                } else {
                    tui_error("failed to save profile");
                }

            } else if (strncmp(arg, "edit ", 5) == 0) {
                const char *name = arg + 5;
                while (*name == ' ')
                    name++;
                agent_profile_t *existing = agent_profile_find(name);
                if (!existing) {
                    char err[128];
                    snprintf(err, sizeof(err), "profile not found: %s", name);
                    tui_error(err);
                    goto agents_done;
                }
                agent_profile_t p = *existing;

                fprintf(stderr, "  %sediting profile: %s%s\n", TUI_DIM, name, TUI_RESET);
                fprintf(stderr, "  %sdescription [%s]:%s ", TUI_DIM, p.description, TUI_RESET);
                fflush(stderr);
                char line[512];
                if (fgets(line, sizeof(line), stdin)) {
                    line[strcspn(line, "\n")] = '\0';
                    if (line[0])
                        snprintf(p.description, sizeof(p.description), "%s", line);
                }

                fprintf(stderr, "  %stool groups (comma-sep, enter to keep current):%s\n", TUI_DIM,
                        TUI_RESET);
                if (p.group_count > 0) {
                    fprintf(stderr, "  %scurrent: ", TUI_DIM);
                    for (int i = 0; i < p.group_count; i++)
                        fprintf(stderr, "%s%s", i > 0 ? "," : "", p.groups[i]);
                    fprintf(stderr, "%s\n", TUI_RESET);
                }
                fprintf(stderr,
                        "  %savailable: "
                        "file_io,git,network,shell,code,crypto,swarm,ast,pipeline,math,search,"
                        "general,market,prediction,memory%s\n",
                        TUI_DIM, TUI_RESET);
                fprintf(stderr, "  > ");
                fflush(stderr);
                if (fgets(line, sizeof(line), stdin)) {
                    line[strcspn(line, "\n")] = '\0';
                    if (line[0]) {
                        p.group_count = 0;
                        char *tok = strtok(line, ", ");
                        while (tok && p.group_count < AP_MAX_GROUPS) {
                            while (*tok == ' ')
                                tok++;
                            if (*tok)
                                snprintf(p.groups[p.group_count++], 32, "%s", tok);
                            tok = strtok(NULL, ", ");
                        }
                    }
                }

                fprintf(stderr, "  %smodel hint [%s]:%s ", TUI_DIM, p.model_hint, TUI_RESET);
                fflush(stderr);
                if (fgets(line, sizeof(line), stdin)) {
                    line[strcspn(line, "\n")] = '\0';
                    if (line[0])
                        snprintf(p.model_hint, sizeof(p.model_hint), "%s", line);
                }

                if (agent_profile_save(&p)) {
                    /* Re-apply filter if this is the active profile */
                    const char *an = agent_profile_active_name();
                    if (an && strcmp(an, name) == 0)
                        tools_set_profile_filter(agent_profile_tool_allowed);
                    char ok[128];
                    snprintf(ok, sizeof(ok), "profile updated: %s", name);
                    tui_success(ok);
                } else {
                    tui_error("failed to save profile");
                }

            } else if (strncmp(arg, "delete ", 7) == 0) {
                const char *name = arg + 7;
                while (*name == ' ')
                    name++;
                if (!agent_profile_find(name)) {
                    char err[128];
                    snprintf(err, sizeof(err), "profile not found: %s", name);
                    tui_error(err);
                    goto agents_done;
                }
                bool was_active = false;
                const char *an = agent_profile_active_name();
                if (an && strcmp(an, name) == 0)
                    was_active = true;
                if (agent_profile_delete(name)) {
                    if (was_active)
                        tools_clear_profile_filter();
                    char ok[128];
                    snprintf(ok, sizeof(ok), "profile deleted: %s", name);
                    tui_success(ok);
                } else {
                    tui_error("failed to delete profile");
                }

            } else if (strcmp(arg, "groups") == 0) {
                fprintf(stderr, "\n");
                tui_header("Tool Groups", TUI_BCYAN);
                static const char *groups[] = {"file_io",  "git",        "network", "shell",
                                               "code",     "crypto",     "swarm",   "ast",
                                               "pipeline", "math",       "search",  "general",
                                               "market",   "prediction", "memory",  NULL};
                static const char *descs[] = {"file read/write/edit, directory ops",
                                              "git status/log/diff/commit/branch",
                                              "HTTP, DNS, ping, port scan, websocket",
                                              "bash execution, compile, run",
                                              "code analysis, snippets, eval",
                                              "hashing, encoding, HMAC",
                                              "agent spawning, swarm management",
                                              "AST analysis, self-inspection",
                                              "pipeline construction and execution",
                                              "math, statistics, calculations",
                                              "web search, document search",
                                              "general purpose utilities",
                                              "trading, market data, orders",
                                              "prediction markets (Kalshi, Polymarket)",
                                              "memory tiers, semantic search",
                                              NULL};
                bool use_rgb2 = tui_detect_color_level() >= TUI_COLOR_256;
                for (int i = 0; groups[i]; i++) {
                    if (use_rgb2)
                        fprintf(stderr,
                                "  \033[38;2;255;95;0m" TUI_RECORD "\033[0m"
                                " \033[1m%-14s\033[0m  \033[2m%s\033[0m\n",
                                groups[i], descs[i]);
                    else
                        fprintf(stderr, "  " TUI_ORANGE TUI_RECORD TUI_RESET " %s%-14s%s  %s%s%s\n",
                                TUI_BOLD, groups[i], TUI_RESET, TUI_DIM, descs[i], TUI_RESET);
                }
                fprintf(stderr, "\n");
            } else {
                tui_error("usage: /agents [list | use <name> | off | show <name> | new <name> | "
                          "edit <name> | delete <name> | groups]");
            }
        agents_done:
            baseline_log("command", "/agents", arg && *arg ? arg : NULL, NULL);
            continue;
        }

        if (strcmp(input_buf, "/tools") == 0) {
            int count;
            const tool_def_t *tools = tools_get_all(&count);
            fprintf(stderr, "\n");
            tui_header("Tools", TUI_BCYAN);

            const char *last_prefix = "";
            for (int i = 0; i < count; i++) {
                const char *name = tools[i].name;
                const char *category = "misc";
                if (strstr(name, "file") || strstr(name, "read") || strstr(name, "write") ||
                    strstr(name, "edit") || strstr(name, "append") || strstr(name, "mkdir") ||
                    strstr(name, "tree") || strstr(name, "wc") || strstr(name, "head") ||
                    strstr(name, "tail") || strstr(name, "symlink") || strstr(name, "page") ||
                    strcmp(name, "list_directory") == 0 || strcmp(name, "find_files") == 0 ||
                    strcmp(name, "grep_files") == 0 || strcmp(name, "chmod") == 0 ||
                    strcmp(name, "move_file") == 0 || strcmp(name, "copy_file") == 0 ||
                    strcmp(name, "delete_file") == 0 || strcmp(name, "file_info") == 0)
                    category = "file";
                else if (strstr(name, "git"))
                    category = "git";
                else if (strstr(name, "agent") || strstr(name, "swarm") || strstr(name, "spawn"))
                    category = "swarm";
                else if (strstr(name, "self_") || strstr(name, "inspect") ||
                         strstr(name, "call_graph") || strstr(name, "dependency"))
                    category = "ast";
                else if (strstr(name, "http") || strstr(name, "curl") || strstr(name, "dns") ||
                         strstr(name, "ping") || strstr(name, "port") || strstr(name, "net") ||
                         strstr(name, "cert") || strstr(name, "whois") ||
                         strstr(name, "download") || strstr(name, "upload") ||
                         strstr(name, "websocket") || strstr(name, "traceroute") ||
                         strcmp(name, "market_quote") == 0)
                    category = "network";
                else if (strstr(name, "docker"))
                    category = "docker";
                else if (strstr(name, "ssh") || strstr(name, "scp"))
                    category = "remote";
                else if (strstr(name, "compile") || strstr(name, "run_") ||
                         strcmp(name, "bash") == 0)
                    category = "exec";

                if (strcmp(category, last_prefix) != 0) {
                    last_prefix = category;
                    const char *cat_color = TUI_DIM;
                    if (strcmp(category, "swarm") == 0)
                        cat_color = TUI_BYELLOW;
                    else if (strcmp(category, "ast") == 0)
                        cat_color = TUI_BMAGENTA;
                    fprintf(stderr, "  %s%s%s\n", cat_color, category, TUI_RESET);
                }
                /* Show call count from metrics if any */
                const tool_metric_t *tm = tool_metrics_get(&tool_metrics, tools[i].name);
                if (tm && tm->calls > 0) {
                    fprintf(stderr, "    %s%-22s%s %s%3dx%s %s%s%s\n", TUI_CYAN, tools[i].name,
                            TUI_RESET, TUI_BWHITE, tm->calls, TUI_RESET, TUI_DIM,
                            tools[i].description, TUI_RESET);
                } else {
                    fprintf(stderr, "    %s%-22s%s     %s%s%s\n", TUI_CYAN, tools[i].name,
                            TUI_RESET, TUI_DIM, tools[i].description, TUI_RESET);
                }
            }
            /* List MCP/external tools grouped by server */
            if (g_external_tool_count > 0) {
                fprintf(stderr, "  %smcp%s\n", TUI_BYELLOW, TUI_RESET);
                for (int i = 0; i < g_external_tool_count; i++) {
                    const tool_metric_t *tm =
                        tool_metrics_get(&tool_metrics, g_external_tools[i].name);
                    if (tm && tm->calls > 0) {
                        fprintf(stderr, "    %s%-22s%s %s%3dx%s %s%s%s\n", TUI_CYAN,
                                g_external_tools[i].name, TUI_RESET, TUI_BWHITE, tm->calls,
                                TUI_RESET, TUI_DIM, g_external_tools[i].description, TUI_RESET);
                    } else {
                        fprintf(stderr, "    %s%-22s%s     %s%s%s\n", TUI_CYAN,
                                g_external_tools[i].name, TUI_RESET, TUI_DIM,
                                g_external_tools[i].description, TUI_RESET);
                    }
                }
            }
            fprintf(stderr, "\n  %s%d builtin + %d MCP + web_search + code_execution%s\n\n",
                    TUI_DIM, count, g_external_tool_count, TUI_RESET);
            baseline_log("command", "/tools", NULL, NULL);
            continue;
        }
        if (strcmp(input_buf, "/help") == 0) {
            fprintf(stderr, "\n");
            tui_header("Commands", TUI_BCYAN);
            fprintf(stderr, "  %s/clear%s       reset conversation\n", TUI_CYAN, TUI_RESET);
            fprintf(stderr, "  %s/model [name]%s switch model (glm52/kimi/opus/sonnet/haiku)\n",
                    TUI_CYAN, TUI_RESET);
            fprintf(stderr, "  %s/effort [lvl]%s set effort (low/medium/high)\n", TUI_CYAN,
                    TUI_RESET);
            fprintf(stderr, "  %s/cost%s        show session cost\n", TUI_CYAN, TUI_RESET);
            fprintf(stderr, "  %s/context%s     show token usage\n", TUI_CYAN, TUI_RESET);
            fprintf(stderr, "  %s/compact%s     trim conversation history\n", TUI_CYAN, TUI_RESET);
            fprintf(stderr, "  %s/undo%s        remove last exchange\n", TUI_CYAN, TUI_RESET);
            fprintf(stderr, "  %s/retry [model]%s re-run last message\n", TUI_CYAN, TUI_RESET);
            fprintf(stderr, "  %s/diff [--staged]%s show git diff\n", TUI_CYAN, TUI_RESET);
            fprintf(stderr, "  %s/note <text>%s add annotation to context\n", TUI_CYAN, TUI_RESET);
            fprintf(stderr, "  %s/add-dir [path]%s inject directory listing into context\n",
                    TUI_CYAN, TUI_RESET);
            fprintf(stderr, "  %s/save [name]%s save session\n", TUI_CYAN, TUI_RESET);
            fprintf(stderr, "  %s/load [name]%s load session\n", TUI_CYAN, TUI_RESET);
            fprintf(stderr, "  %s/sessions%s   list saved sessions with preview metadata\n",
                    TUI_CYAN, TUI_RESET);
            fprintf(stderr, "  %s/resume <q>%s  resume by index, name, or title search\n", TUI_CYAN,
                    TUI_RESET);
            fprintf(stderr, "  %s/new [name]%s  start a fresh named session\n", TUI_CYAN,
                    TUI_RESET);
            fprintf(stderr, "  %s/rename <n>%s  rename the current session\n", TUI_CYAN, TUI_RESET);
            fprintf(stderr, "  %s/workspace%s  show claw workspace status\n", TUI_CYAN, TUI_RESET);
            fprintf(stderr, "  %s/workspace bootstrap%s create missing workspace files\n", TUI_CYAN,
                    TUI_RESET);
            fprintf(stderr, "  %s/workspace reload%s reload composed workspace prompt\n", TUI_CYAN,
                    TUI_RESET);
            fprintf(stderr, "  %s/skills%s     list installed skills\n", TUI_CYAN, TUI_RESET);
            fprintf(stderr, "  %s/skills use [name]%s activate one skill for future turns\n",
                    TUI_CYAN, TUI_RESET);
            fprintf(stderr, "  %s/skills show [name]%s print a skill body\n", TUI_CYAN, TUI_RESET);
            fprintf(stderr, "  %s/identity%s   show workspace identity file\n", TUI_CYAN,
                    TUI_RESET);
            fprintf(stderr, "  %s/user%s       show workspace user file\n", TUI_CYAN, TUI_RESET);
            fprintf(stderr, "  %s/soul%s       show workspace soul file\n", TUI_CYAN, TUI_RESET);
            fprintf(stderr, "  %s/memory%s     show workspace memory file\n", TUI_CYAN, TUI_RESET);
            fprintf(stderr, "  %s/setup%s      save API keys to %s\n", TUI_CYAN, TUI_RESET,
                    dsco_setup_env_path());
            fprintf(stderr, "  %s/force [tool]%s force next tool call\n", TUI_CYAN, TUI_RESET);
            fprintf(stderr, "  %s/web%s        toggle web search\n", TUI_CYAN, TUI_RESET);
            fprintf(stderr, "  %s/code%s       toggle code execution\n", TUI_CYAN, TUI_RESET);
            fprintf(stderr, "  %s/budget [$]%s  set cost budget\n", TUI_CYAN, TUI_RESET);
            fprintf(stderr, "  %s/exec <backend> <prompt>%s  run via external CLI\n", TUI_CYAN,
                    TUI_RESET);
            fprintf(stderr, "  %s/claude <prompt>%s  shorthand: /exec claude\n", TUI_CYAN,
                    TUI_RESET);
            fprintf(stderr, "  %s/codex <prompt>%s   shorthand: /exec codex\n", TUI_CYAN,
                    TUI_RESET);
            fprintf(stderr, "  %s/thinking [auto|>=1024]%s set thinking budget (in tokens)\n",
                    TUI_CYAN, TUI_RESET);
            fprintf(stderr, "  %s/trust [tier]%s set trust tier (trusted/standard/untrusted)\n",
                    TUI_CYAN, TUI_RESET);
            fprintf(stderr, "  %s/agents%s     define named tool-filtered agent profiles\n",
                    TUI_CYAN, TUI_RESET);
            fprintf(stderr, "  %s/topology%s   list/show/use/run topologies\n", TUI_CYAN,
                    TUI_RESET);
            fprintf(stderr, "  %s/swarm%s      inspect/wait/kill swarm agents and groups\n",
                    TUI_CYAN, TUI_RESET);
            fprintf(stderr, "  %s/status%s     full session status\n", TUI_CYAN, TUI_RESET);
            fprintf(stderr, "  %s/tools%s      list all tools\n", TUI_CYAN, TUI_RESET);
            fprintf(stderr, "  %s/plugins%s    list loaded plugins\n", TUI_CYAN, TUI_RESET);
            fprintf(
                stderr,
                "  %s/plugins validate [manifest] [lock]%s validate plugin manifest + lockfile\n",
                TUI_CYAN, TUI_RESET);
            fprintf(stderr, "  %s/mcp%s        show MCP servers + tools\n", TUI_CYAN, TUI_RESET);
            fprintf(stderr, "  %s/provider%s   show/detect API provider\n", TUI_CYAN, TUI_RESET);
            fprintf(stderr, "  %s/version%s    show version + build info\n", TUI_CYAN, TUI_RESET);
            fprintf(stderr, "  %s/features%s   toggle UI features (F1-F40)\n", TUI_CYAN, TUI_RESET);
            fprintf(stderr, "  %s/perf%s       latency waterfall + throughput\n", TUI_CYAN,
                    TUI_RESET);
            fprintf(stderr, "  %s/minimap%s    conversation minimap\n", TUI_CYAN, TUI_RESET);
            fprintf(stderr, "  %s/dashboard%s  rich session overview panel\n", TUI_CYAN, TUI_RESET);
            fprintf(stderr, "  %s/top%s        tool leaderboard by calls\n", TUI_CYAN, TUI_RESET);
            fprintf(stderr, "  %s/flame%s      flame timeline for tool executions\n", TUI_CYAN,
                    TUI_RESET);
            fprintf(stderr, "  %s/help%s       show this help\n", TUI_CYAN, TUI_RESET);
            fprintf(stderr, "  %squit%s        exit dsco\n", TUI_CYAN, TUI_RESET);
            fprintf(stderr, "\n");
            tui_header("Images", TUI_BMAGENTA);
            fprintf(stderr, "  %sDrag & drop image files into the prompt:%s\n", TUI_DIM, TUI_RESET);
            fprintf(stderr, "  %s\"describe /path/to/screenshot.png\"%s\n", TUI_DIM, TUI_RESET);
            fprintf(
                stderr,
                "  %sSupported: .png .jpg .jpeg .gif .webp .bmp .tif .tiff .heic .heif .avif%s\n\n",
                TUI_DIM, TUI_RESET);
            tui_header("Swarm", TUI_BYELLOW);
            fprintf(stderr, "  %sSpawn sub-agents for parallel work:%s\n", TUI_DIM, TUI_RESET);
            fprintf(stderr, "  %s\"spawn 3 agents to build a REST API, frontend, and tests\"%s\n\n",
                    TUI_DIM, TUI_RESET);
            tui_header("AST Introspection", TUI_BMAGENTA);
            fprintf(stderr, "  %sAnalyze C codebases at the AST level:%s\n", TUI_DIM, TUI_RESET);
            fprintf(
                stderr,
                "  %s\"inspect your own source code and find the most complex functions\"%s\n\n",
                TUI_DIM, TUI_RESET);
            tui_header("Crypto", TUI_BRED);
            fprintf(stderr, "  %sSHA-256, MD5, HMAC, UUID, HKDF, JWT — all pure C:%s\n", TUI_DIM,
                    TUI_RESET);
            fprintf(stderr, "  %s\"hash this file with SHA-256\" or \"generate 5 UUIDs\"%s\n\n",
                    TUI_DIM, TUI_RESET);
            tui_header("Pipeline", TUI_BGREEN);
            fprintf(stderr, "  %sCoroutine-powered streaming data transforms:%s\n", TUI_DIM,
                    TUI_RESET);
            fprintf(stderr, "  %s\"pipe this log through filter:error|sort|uniq|head:20\"%s\n\n",
                    TUI_DIM, TUI_RESET);
            tui_header("Eval", TUI_BCYAN);
            fprintf(stderr, "  %sMath engine with 50+ functions:%s\n", TUI_DIM, TUI_RESET);
            fprintf(stderr, "  %s\"eval sqrt(2)^3 + sin(pi/4)\" or \"big_factorial 100\"%s\n\n",
                    TUI_DIM, TUI_RESET);
            baseline_log("command", "/help", NULL, NULL);
            continue;
        }
        if (strncmp(input_buf, "/plugins validate", 17) == 0) {
            const char *arg = input_buf + 17;
            while (*arg == ' ')
                arg++;

            char *copy = safe_strdup(arg);
            char *manifest_path = NULL;
            char *lock_path = NULL;
            if (*copy) {
                manifest_path = strtok(copy, " \t");
                lock_path = strtok(NULL, " \t");
            }

            char out[2048];
            bool ok = plugin_validate_manifest_and_lock(manifest_path, lock_path, out, sizeof(out));
            if (ok)
                tui_success(out);
            else
                tui_error(out);
            baseline_log("command", "/plugins validate", out, NULL);
            free(copy);
            continue;
        }
        if (strcmp(input_buf, "/plugins") == 0) {
            char buf[4096];
            plugin_list(&g_plugins, buf, sizeof(buf));
            fprintf(stderr, "\n%s\n", buf);
            baseline_log("command", "/plugins", NULL, NULL);
            continue;
        }
        if (strcmp(input_buf, "/mcp") == 0 || strcmp(input_buf, "/mcp reload") == 0) {
            if (strcmp(input_buf, "/mcp reload") == 0) {
                /* Drain any in-flight background loader before mutating g_mcp. */
                mcp_bg_init_join();
                mcp_shutdown(&g_mcp);
                tools_reset_external();
                int n = mcp_init(&g_mcp);
                if (n > 0)
                    mcp_register_discovered_tools(&g_mcp);
                char msg[128];
                snprintf(msg, sizeof(msg), "MCP reloaded: %d tools from %d servers (%d failed)",
                         g_mcp.tool_count, g_mcp.server_count, g_mcp.failed_count);
                tui_success(msg);
            } else {
                fprintf(stderr, "\n");
                tui_header("MCP Servers", TUI_BCYAN);
                if (g_mcp.server_count == 0) {
                    fprintf(stderr, "  %sno MCP servers configured%s\n", TUI_DIM, TUI_RESET);
                    fprintf(stderr,
                            "  %sDSCO scans ~/.dsco, ./.dsco, ./.mcp.json, Claude config, and "
                            "Codex config%s\n\n",
                            TUI_DIM, TUI_RESET);
                } else {
                    for (int i = 0; i < g_mcp.server_count; i++) {
                        const char *endpoint = g_mcp.servers[i].transport == MCP_TRANSPORT_HTTP
                                                   ? g_mcp.servers[i].url
                                                   : g_mcp.servers[i].command;
                        fprintf(
                            stderr, "  %s%s%s  %s%s%s  %s%s%s  %s%s%s  %s%s%s\n", TUI_CYAN,
                            g_mcp.servers[i].name, TUI_RESET, TUI_DIM, endpoint, TUI_RESET, TUI_DIM,
                            g_mcp.servers[i].transport == MCP_TRANSPORT_HTTP ? "http" : "stdio",
                            TUI_RESET, TUI_DIM, g_mcp.servers[i].source, TUI_RESET,
                            g_mcp.servers[i].initialized ? TUI_GREEN : TUI_RED,
                            g_mcp.servers[i].initialized ? "connected" : "disconnected", TUI_RESET);
                    }
                    fprintf(stderr,
                            "\n  %s%d MCP tools registered · %d connected · %d failed%s\n\n",
                            TUI_DIM, g_mcp.tool_count, g_mcp.server_count, g_mcp.failed_count,
                            TUI_RESET);
                    /* List MCP tools */
                    for (int i = 0; i < g_mcp.tool_count; i++) {
                        fprintf(stderr, "    %s%-30s%s %s%s%s\n", TUI_CYAN, g_mcp.tools[i].name,
                                TUI_RESET, TUI_DIM, g_mcp.tools[i].description, TUI_RESET);
                    }
                    fprintf(stderr, "\n");
                }
            }
            baseline_log("command", "/mcp", NULL, NULL);
            continue;
        }
        if (strcmp(input_buf, "/provider") == 0) {
            fprintf(stderr, "  %sprovider:%s %s\n", TUI_DIM, TUI_RESET,
                    g_provider ? g_provider->name : "none");
            fprintf(stderr, "  %sdetected from:%s model=%s\n", TUI_DIM, TUI_RESET, session.model);
            fprintf(stderr, "  %savailable:%s anthropic, openai\n", TUI_DIM, TUI_RESET);
            fprintf(stderr, "  %snote: provider auto-detected from /model selection%s\n\n", TUI_DIM,
                    TUI_RESET);
            continue;
        }
        if (strncmp(input_buf, "/budget", 7) == 0) {
            const char *arg = input_buf + 7;
            while (*arg == ' ')
                arg++;
            if (*arg == '\0') {
                double cost = session_cost(&session);
                double daily = baseline_daily_cost() + cost;
                fprintf(stderr, "  %ssession:%s $%.4f", TUI_DIM, TUI_RESET, cost);
                if (g_cost_budget > 0)
                    fprintf(stderr, " / $%.2f (%.0f%%)", g_cost_budget,
                            100.0 * cost / g_cost_budget);
                else
                    fprintf(stderr, " (no session limit)");
                fprintf(stderr, "\n");
                fprintf(stderr, "  %sdaily:%s   $%.2f", TUI_DIM, TUI_RESET, daily);
                if (g_daily_budget > 0)
                    fprintf(stderr, " / $%.2f (%.0f%%)", g_daily_budget,
                            100.0 * daily / g_daily_budget);
                else
                    fprintf(stderr, " (no daily limit)");
                fprintf(stderr, "\n");
                fprintf(stderr,
                        "  %susage: /budget <dollars> | /budget daily <dollars> | /budget off%s\n",
                        TUI_DIM, TUI_RESET);
            } else if (strcmp(arg, "off") == 0 || strcmp(arg, "none") == 0) {
                g_cost_budget = 0;
                g_daily_budget = 0;
                tui_success("all cost budgets disabled");
            } else if (strncmp(arg, "daily ", 6) == 0) {
                double db = atof(arg + 6);
                if (db > 0) {
                    g_daily_budget = db;
                    char msg[128];
                    snprintf(msg, sizeof(msg), "daily budget set to $%.2f", db);
                    tui_success(msg);
                } else if (strcmp(arg + 6, "off") == 0) {
                    g_daily_budget = 0;
                    tui_success("daily budget disabled");
                } else {
                    tui_error("daily budget must be a positive number");
                }
            } else {
                double budget = atof(arg);
                if (budget > 0) {
                    g_cost_budget = budget;
                    char msg[160];
                    /* The daily cap is a separate, cross-session gate. A bare
                     * `/budget <n>` that leaves the daily cap below the new
                     * session budget would still block mid-session — which is
                     * exactly the surprise users hit. Lift the daily cap so the
                     * session budget is actually spendable today. Use
                     * already-spent + budget so `n` means "n more dollars". */
                    if (g_daily_budget > 0) {
                        double needed = baseline_daily_cost() + budget;
                        if (g_daily_budget < needed) {
                            g_daily_budget = needed;
                            snprintf(msg, sizeof(msg),
                                     "session budget $%.2f · daily cap raised to $%.2f", budget,
                                     g_daily_budget);
                        } else {
                            snprintf(msg, sizeof(msg), "session budget set to $%.2f", budget);
                        }
                    } else {
                        snprintf(msg, sizeof(msg), "session budget set to $%.2f", budget);
                    }
                    tui_success(msg);
                } else {
                    tui_error("budget must be a positive number");
                }
            }
            baseline_log("command", "/budget", NULL, NULL);
            continue;
        }
        /* /exec <backend> <prompt>, /claude <prompt>, /codex <prompt> */
        if (strncmp(input_buf, "/exec", 5) == 0 || strncmp(input_buf, "/claude", 7) == 0 ||
            strncmp(input_buf, "/codex", 6) == 0) {

            const char *backend_name = NULL;
            const char *prompt = NULL;

            if (strncmp(input_buf, "/claude", 7) == 0) {
                backend_name = "claude";
                prompt = input_buf + 7;
            } else if (strncmp(input_buf, "/codex", 6) == 0) {
                backend_name = "codex";
                prompt = input_buf + 6;
            } else {
                /* /exec <backend> [prompt] */
                const char *rest = input_buf + 5;
                while (*rest == ' ')
                    rest++;
                if (!rest[0] || strcmp(rest, "list") == 0) {
                    /* /exec or /exec list — show available backends */
                    fprintf(stderr, "\n  %sExternal CLIs%s\n", TUI_BOLD, TUI_RESET);
                    const char *backends[] = {"claude", "codex", NULL};
                    const char *bins[] = {"claude", "codex", NULL};
                    const char *descs[] = {"Claude Code (Anthropic)", "Codex CLI (OpenAI)", NULL};
                    for (int bi = 0; backends[bi]; bi++) {
                        char check[256];
                        snprintf(check, sizeof(check), "command -v %s >/dev/null 2>&1", bins[bi]);
                        bool avail = (system(check) == 0);
                        fprintf(stderr, "  %-12s %-28s %s%s%s\n", backends[bi], descs[bi],
                                avail ? TUI_GREEN : "\033[31m", avail ? "ready" : "not found",
                                TUI_RESET);
                    }
                    fprintf(stderr, "\n  %sNative API Providers%s (use with -e from CLI)\n",
                            TUI_BOLD, TUI_RESET);
                    struct {
                        const char *name;
                        const char *desc;
                        const char *env;
                    } nprovs[] = {{"anthropic", "Anthropic Claude", "ANTHROPIC_API_KEY"},
                                  {"openai", "OpenAI", "OPENAI_API_KEY"},
                                  {"openrouter", "OpenRouter", "OPENROUTER_API_KEY"},
                                  {"groq", "Groq", "GROQ_API_KEY"},
                                  {"deepseek", "DeepSeek", "DEEPSEEK_API_KEY"},
                                  {"xai", "xAI Grok", "XAI_API_KEY"},
                                  {"mistral", "Mistral AI", "MISTRAL_API_KEY"},
                                  {"together", "Together AI", "TOGETHER_API_KEY"},
                                  {"perplexity", "Perplexity", "PERPLEXITY_API_KEY"},
                                  {"cerebras", "Cerebras", "CEREBRAS_API_KEY"},
                                  {"cohere", "Cohere", "COHERE_API_KEY"},
                                  {NULL, NULL, NULL}};
                    for (int ni = 0; nprovs[ni].name; ni++) {
                        const char *k = getenv(nprovs[ni].env);
                        bool has = k && k[0];
                        fprintf(stderr, "  %-12s %-28s %s%s%s\n", nprovs[ni].name, nprovs[ni].desc,
                                has ? TUI_GREEN : TUI_DIM, has ? "key set" : "no key", TUI_RESET);
                    }
                    fprintf(stderr, "\n  %susage: /exec <backend> <prompt>%s\n", TUI_DIM,
                            TUI_RESET);
                    fprintf(stderr, "  %s       /claude <prompt>   /codex <prompt>%s\n\n", TUI_DIM,
                            TUI_RESET);
                    continue;
                }
                /* parse backend name */
                const char *space = strchr(rest, ' ');
                if (space) {
                    char bname[32];
                    size_t blen = (size_t)(space - rest);
                    if (blen >= sizeof(bname))
                        blen = sizeof(bname) - 1;
                    memcpy(bname, rest, blen);
                    bname[blen] = '\0';
                    backend_name = (strcmp(bname, "claude") == 0)  ? "claude"
                                   : (strcmp(bname, "codex") == 0) ? "codex"
                                                                   : NULL;
                    if (!backend_name) {
                        char msg[128];
                        snprintf(msg, sizeof(msg), "unknown executor '%s' (use: claude, codex)",
                                 bname);
                        tui_error(msg);
                        continue;
                    }
                    prompt = space;
                } else {
                    /* /exec <backend> with no prompt — show usage */
                    backend_name = rest;
                    fprintf(stderr, "  %susage: /exec %s <prompt>%s\n", TUI_DIM, rest, TUI_RESET);
                    continue;
                }
            }

            while (prompt && *prompt == ' ')
                prompt++;
            if (!prompt || !prompt[0]) {
                fprintf(stderr, "  %susage: /%s <prompt>%s\n", TUI_DIM, backend_name, TUI_RESET);
                continue;
            }

            bool is_claude = (strcmp(backend_name, "claude") == 0);

            /* Show what we're doing */
            fprintf(stderr, "\n  %s%s %s %s%s\n", TUI_DIM, tui_glyph()->arrow_right, backend_name,
                    is_claude ? "claude -p" : "codex exec", TUI_RESET);

            struct timeval t0, t1;
            gettimeofday(&t0, NULL);

            pid_t pid = fork();
            if (pid == 0) {
                /* Child — inherit terminal directly */
                if (is_claude) {
                    execlp("claude", "claude", "-p", prompt, (char *)NULL);
                } else {
                    execlp("codex", "codex", "exec", prompt, (char *)NULL);
                }
                _exit(127);
            } else if (pid > 0) {
                int status = 0;
                waitpid(pid, &status, 0);
                gettimeofday(&t1, NULL);
                double elapsed = (t1.tv_sec - t0.tv_sec) + (t1.tv_usec - t0.tv_usec) / 1e6;

                if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
                    char msg[128];
                    snprintf(msg, sizeof(msg), "%s exited with status %d (%.1fs)", backend_name,
                             WEXITSTATUS(status), elapsed);
                    tui_warning(msg);
                } else {
                    fprintf(stderr, "\n  %s%s done (%.1fs)%s\n", TUI_DIM, backend_name, elapsed,
                            TUI_RESET);
                }
            } else {
                tui_error("fork failed");
            }
            baseline_log("command", "/exec", backend_name, prompt);
            continue;
        }
        if (strncmp(input_buf, "/trust", 6) == 0) {
            const char *arg = input_buf + 6;
            while (*arg == ' ')
                arg++;
            if (*arg == '\0') {
                fprintf(stderr, "  %strust tier:%s %s\n", TUI_DIM, TUI_RESET,
                        session_trust_tier_to_string(session.trust_tier));
                fprintf(stderr, "  %susage: /trust trusted|standard|untrusted%s\n", TUI_DIM,
                        TUI_RESET);
            } else {
                bool ok = false;
                dsco_trust_tier_t tier = session_trust_tier_from_string(arg, &ok);
                if (!ok) {
                    tui_error("invalid trust tier (use trusted, standard, or untrusted)");
                } else {
                    session.trust_tier = tier;
                    setenv("DSCO_TRUST_TIER", session_trust_tier_to_string(session.trust_tier), 1);
                    char msg[128];
                    snprintf(msg, sizeof(msg), "trust tier set to %s",
                             session_trust_tier_to_string(session.trust_tier));
                    tui_success(msg);
                    baseline_log("security", "trust_tier_set",
                                 session_trust_tier_to_string(session.trust_tier), NULL);
                }
            }
            continue;
        }
        if (strcmp(input_buf, "/status") == 0) {
            double cost = session_cost(&session);
            double avg_ttft = session.telemetry_samples > 0
                                  ? session.total_ttft_ms / session.telemetry_samples
                                  : 0;
            double avg_stream = session.telemetry_samples > 0
                                    ? session.total_stream_ms / session.telemetry_samples
                                    : 0;
            fprintf(stderr, "\n");
            tui_header("Session Status", TUI_BCYAN);
            fprintf(stderr, "  %sModel:%s       %s\n", TUI_DIM, TUI_RESET, session.model);
            fprintf(stderr, "  %sProvider:%s    %s\n", TUI_DIM, TUI_RESET,
                    g_provider ? g_provider->name : "none");
            fprintf(stderr, "  %sEffort:%s      %s\n", TUI_DIM, TUI_RESET, session.effort);
            fprintf(stderr, "  %sTrust tier:%s  %s\n", TUI_DIM, TUI_RESET,
                    session_trust_tier_to_string(session.trust_tier));
            fprintf(stderr, "  %sActive skill:%s %s\n", TUI_DIM, TUI_RESET,
                    session.active_skill[0] ? session.active_skill : "(none)");
            fprintf(stderr, "  %sTopology:%s    %s%s%s\n", TUI_DIM, TUI_RESET,
                    session.active_topology[0] ? session.active_topology : "(none)",
                    session.topology_auto ? " " : "", session.topology_auto ? "(auto)" : "");
            {
                swarm_t *sw = tools_swarm_instance();
                swarm_poll(sw, 0);
                fprintf(stderr, "  %sSwarm:%s       %d agents / %d groups / %d active\n", TUI_DIM,
                        TUI_RESET, sw->child_count, sw->group_count, swarm_active_count(sw));
            }
            if (session.temperature >= 0)
                fprintf(stderr, "  %sTemperature:%s %.2f\n", TUI_DIM, TUI_RESET,
                        session.temperature);
            if (session.thinking_budget > 0)
                fprintf(stderr, "  %sThinking:%s   %d tokens\n", TUI_DIM, TUI_RESET,
                        session.thinking_budget);
            fprintf(stderr, "  %sTurns:%s       %d\n", TUI_DIM, TUI_RESET, session.turn_count);
            fprintf(stderr, "  %sCost:%s        $%.4f", TUI_DIM, TUI_RESET, cost);
            if (g_cost_budget > 0)
                fprintf(stderr, " / $%.2f (%.0f%%)", g_cost_budget, 100.0 * cost / g_cost_budget);
            fprintf(stderr, "\n");
            fprintf(stderr, "  %sMessages:%s    %d\n", TUI_DIM, TUI_RESET, conv.count);
            fprintf(stderr, "  %sContext:%s     %d / %d tokens\n", TUI_DIM, TUI_RESET,
                    last_input_tokens, session.context_window);
            if (session.telemetry_samples > 0) {
                fprintf(stderr, "  %sAvg TTFT:%s    %.0fms\n", TUI_DIM, TUI_RESET, avg_ttft);
                fprintf(stderr, "  %sAvg stream:%s  %.0fms\n", TUI_DIM, TUI_RESET, avg_stream);
            }
            if (session.fallback_count > 0) {
                fprintf(stderr, "  %sFallback:%s    ", TUI_DIM, TUI_RESET);
                for (int fi = 0; fi < session.fallback_count; fi++)
                    fprintf(stderr, "%s%s", fi ? " -> " : "", session.fallback_models[fi]);
                fprintf(stderr, "\n");
            }
            fprintf(stderr, "  %sWeb search:%s  %s\n", TUI_DIM, TUI_RESET,
                    session.web_search ? "on" : "off");
            fprintf(stderr, "  %sCode exec:%s   %s\n", TUI_DIM, TUI_RESET,
                    session.code_execution ? "on" : "off");
            fprintf(stderr, "  %sMCP servers:%s %d (%d tools)\n", TUI_DIM, TUI_RESET,
                    g_mcp.server_count, g_mcp.tool_count);
            fprintf(stderr, "  %sCache:%s       %d hits / %d misses\n", TUI_DIM, TUI_RESET,
                    tool_cache.hits, tool_cache.misses);
            fprintf(stderr, "  %sTotal tools:%s %d\n\n", TUI_DIM, TUI_RESET, tool_count);
            if (session.active_topology[0]) {
                const topology_t *topo = topology_find(session.active_topology);
                if (topo) {
                    char ascii[4096];
                    topology_render_ascii(topo, ascii, sizeof(ascii));
                    fprintf(stderr, "%s\n\n", ascii);
                }
            }
            if (tools_swarm_instance()->child_count > 0) {
                swarm_t *sw = tools_swarm_instance();
                int swarm_done = 0, swarm_running = 0, swarm_errored = 0;
                for (int i = 0; i < sw->child_count; i++) {
                    if (sw->children[i].status == SWARM_DONE)
                        swarm_done++;
                    else if (sw->children[i].status == SWARM_RUNNING ||
                             sw->children[i].status == SWARM_STREAMING)
                        swarm_running++;
                    else if (sw->children[i].status == SWARM_ERROR ||
                             sw->children[i].status == SWARM_KILLED)
                        swarm_errored++;
                }
                tui_agent_rollup(sw->child_count, swarm_done, swarm_running, swarm_errored);
                fprintf(stderr, "\n");
            }
            continue;
        }
        /* /temp — temperature control */
        if (strncmp(input_buf, "/temp", 5) == 0) {
            const char *arg = input_buf + 5;
            while (*arg == ' ')
                arg++;
            if (*arg == '\0') {
                fprintf(stderr, "  %stemperature:%s %s\n", TUI_DIM, TUI_RESET,
                        session.temperature >= 0 ? "custom" : "default");
                if (session.temperature >= 0)
                    fprintf(stderr, "  %svalue:%s %.2f\n", TUI_DIM, TUI_RESET, session.temperature);
                fprintf(stderr, "  %susage: /temp 0.0-2.0 | /temp off%s\n", TUI_DIM, TUI_RESET);
            } else if (strcmp(arg, "off") == 0 || strcmp(arg, "default") == 0) {
                session.temperature = -1.0;
                tui_success("temperature reset to default");
            } else {
                double t = atof(arg);
                if (t >= 0.0 && t <= 2.0) {
                    session.temperature = t;
                    char msg[64];
                    snprintf(msg, sizeof(msg), "temperature set to %.2f", t);
                    tui_success(msg);
                } else {
                    tui_error("temperature must be 0.0-2.0");
                }
            }
            continue;
        }
        /* /thinking — thinking budget control */
        if (strncmp(input_buf, "/thinking", 9) == 0) {
            const char *arg = input_buf + 9;
            while (*arg == ' ')
                arg++;
            if (*arg == '\0' || strcmp(arg, "auto") == 0 || strcmp(arg, "adaptive") == 0) {
                session.thinking_budget = 0;
                tui_success("thinking set to adaptive (auto)");
            } else {
                int budget = atoi(arg);
                if (budget >= 1024) {
                    session.thinking_budget = budget;
                    char msg[64];
                    snprintf(msg, sizeof(msg), "thinking budget: %d tokens", budget);
                    tui_success(msg);
                } else {
                    tui_error("thinking budget must be >= 1024 tokens, or 'auto'");
                }
            }
            continue;
        }
        /* /fallback — model fallback chain */
        if (strncmp(input_buf, "/fallback", 9) == 0) {
            const char *arg = input_buf + 9;
            while (*arg == ' ')
                arg++;
            if (*arg == '\0') {
                if (session.fallback_count == 0) {
                    fprintf(stderr, "  %sfallback:%s none (single model)\n", TUI_DIM, TUI_RESET);
                } else {
                    fprintf(stderr, "  %sfallback chain:%s ", TUI_DIM, TUI_RESET);
                    for (int fi = 0; fi < session.fallback_count; fi++)
                        fprintf(stderr, "%s%s", fi ? " -> " : "", session.fallback_models[fi]);
                    fprintf(stderr, "\n");
                }
                fprintf(stderr, "  %susage: /fallback glm52,kimi,opus | /fallback off%s\n", TUI_DIM,
                        TUI_RESET);
            } else if (strcmp(arg, "off") == 0 || strcmp(arg, "none") == 0) {
                session.fallback_count = 0;
                tui_success("fallback chain disabled");
            } else {
                session.fallback_count = 0;
                char *copy = safe_strdup(arg);
                char *tok = strtok(copy, ",");
                while (tok && session.fallback_count < 4) {
                    while (*tok == ' ')
                        tok++;
                    const char *resolved = model_resolve_alias(tok);
                    snprintf(session.fallback_models[session.fallback_count],
                             sizeof(session.fallback_models[0]), "%s", resolved);
                    session.fallback_count++;
                    tok = strtok(NULL, ",");
                }
                free(copy);
                char msg[256];
                snprintf(msg, sizeof(msg), "fallback chain: %d models", session.fallback_count);
                tui_success(msg);
            }
            continue;
        }
        /* /metrics — per-tool performance metrics */
        if (strcmp(input_buf, "/metrics") == 0) {
            fprintf(stderr, "\n");
            tui_header("Tool Metrics", TUI_BCYAN);
            if (tool_metrics.count == 0) {
                fprintf(stderr, "  %sno tool calls recorded yet%s\n\n", TUI_DIM, TUI_RESET);
            } else {
                fprintf(stderr, "  %s%-22s %6s %5s %5s %4s %8s %8s%s\n", TUI_DIM, "TOOL", "CALLS",
                        "OK", "FAIL", "TMO", "AVG(ms)", "MAX(ms)", TUI_RESET);
                for (int i = 0; i < tool_metrics.count; i++) {
                    tool_metric_t *e = &tool_metrics.entries[i];
                    double avg = e->calls > 0 ? e->total_latency_ms / e->calls : 0;
                    fprintf(stderr, "  %-22s %6d %s%5d%s %s%5d%s %s%4d%s %8.0f %8.0f\n", e->name,
                            e->calls, TUI_GREEN, e->successes, TUI_RESET,
                            e->failures > 0 ? TUI_RED : TUI_DIM, e->failures, TUI_RESET,
                            e->timeouts > 0 ? TUI_YELLOW : TUI_DIM, e->timeouts, TUI_RESET, avg,
                            e->max_latency_ms);
                }
                fprintf(stderr, "\n  %sCache: %d hits / %d misses (%.0f%% hit rate)%s\n\n", TUI_DIM,
                        tool_cache.hits, tool_cache.misses,
                        (tool_cache.hits + tool_cache.misses) > 0
                            ? 100.0 * tool_cache.hits / (tool_cache.hits + tool_cache.misses)
                            : 0.0,
                        TUI_RESET);
            }
            continue;
        }
        /* /telemetry — streaming performance */
        if (strcmp(input_buf, "/telemetry") == 0) {
            fprintf(stderr, "\n");
            tui_header("Streaming Telemetry", TUI_BCYAN);
            if (session.telemetry_samples == 0) {
                fprintf(stderr, "  %sno streaming data yet%s\n\n", TUI_DIM, TUI_RESET);
            } else {
                fprintf(stderr, "  %sSamples:%s    %d\n", TUI_DIM, TUI_RESET,
                        session.telemetry_samples);
                fprintf(stderr, "  %sAvg TTFT:%s   %.0f ms\n", TUI_DIM, TUI_RESET,
                        session.total_ttft_ms / session.telemetry_samples);
                fprintf(stderr, "  %sAvg total:%s  %.0f ms\n", TUI_DIM, TUI_RESET,
                        session.total_stream_ms / session.telemetry_samples);
                fprintf(stderr, "\n");
            }
            continue;
        }
        /* /cache — tool cache management */
        if (strncmp(input_buf, "/cache", 6) == 0) {
            const char *arg = input_buf + 6;
            while (*arg == ' ')
                arg++;
            if (*arg == '\0') {
                fprintf(stderr, "  %scache:%s %d entries, %d hits, %d misses\n", TUI_DIM, TUI_RESET,
                        tool_cache.count, tool_cache.hits, tool_cache.misses);
            } else if (strcmp(arg, "clear") == 0) {
                tool_cache_free(&tool_cache);
                tool_cache_init(&tool_cache);
                tui_success("tool cache cleared");
            }
            continue;
        }

        /* /trace — view trace spans */
        if (strncmp(input_buf, "/trace", 6) == 0) {
            const char *arg = input_buf + 6;
            while (*arg == ' ')
                arg++;
            if (*arg == '\0') {
                trace_query_recent(10);
            } else {
                /* Treat arg as a trace_id to show waterfall */
                trace_print_waterfall(arg);
            }
            continue;
        }

        /* /features — toggle UI features */
        if (strncmp(input_buf, "/features", 9) == 0) {
            const char *arg = input_buf + 9;
            while (*arg == ' ')
                arg++;
            if (*arg == '\0') {
                tui_features_list(&g_features);
            } else {
                tui_features_toggle(&g_features, arg);
            }
            continue;
        }

        /* /perf — latency waterfall */
        if (strcmp(input_buf, "/perf") == 0) {
            fprintf(stderr, "\n");
            tui_header("Performance", TUI_BCYAN);
            tui_latency_breakdown_t lb = {
                .dns_ms = s_last_latency.dns_ms,
                .connect_ms = s_last_latency.connect_ms,
                .tls_ms = s_last_latency.tls_ms,
                .ttfb_ms = s_last_latency.ttfb_ms,
                .total_ms = s_last_latency.total_ms,
            };
            tui_latency_waterfall(&lb);
            tui_throughput_render(&s_throughput);
            fprintf(stderr, "\n");
            continue;
        }

        /* /minimap — conversation minimap (F16) */
        if (strcmp(input_buf, "/minimap") == 0) {
            tui_minimap_entry_t entries[256];
            int mc = 0;
            for (int i = 0; i < conv.count && mc < 256; i++) {
                entries[mc].type = conv.msgs[i].role == ROLE_USER ? 'u' : 'a';
                /* Estimate tokens from content */
                int est = 0;
                for (int j = 0; j < conv.msgs[i].content_count; j++) {
                    if (conv.msgs[i].content[j].text)
                        est += tui_estimate_tokens(conv.msgs[i].content[j].text);
                    if (conv.msgs[i].content[j].tool_name) {
                        entries[mc].type = 't';
                        est += 50; /* tool overhead estimate */
                    }
                }
                entries[mc].tokens = est > 0 ? est : 10;
                mc++;
            }
            tui_minimap_render(entries, mc, 0);
            continue;
        }

        /* /dashboard — rich session overview */
        if (strcmp(input_buf, "/dashboard") == 0) {
            fprintf(stderr, "\n");
            tui_header("Dashboard", TUI_BCYAN);
            double cost = session_cost(&session);
            const model_info_t *mi = model_lookup(session.model);
            int ctx_used = session.total_input_tokens + session.total_output_tokens;
            int ctx_max =
                session.context_window > 0 ? session.context_window : CONTEXT_WINDOW_TOKENS;
            double avg_ttft = session.telemetry_samples > 0
                                  ? session.total_ttft_ms / session.telemetry_samples
                                  : 0;
            double avg_stream = session.telemetry_samples > 0
                                    ? session.total_stream_ms / session.telemetry_samples
                                    : 0;
            double avg_tps =
                avg_stream > 0 && session.total_output_tokens > 0
                    ? (session.total_output_tokens / (double)session.telemetry_samples) /
                          (avg_stream / 1000.0)
                    : 0;

            /* Count total tools from metrics */
            int dash_total_tools = 0;
            for (int ti = 0; ti < tool_metrics.count; ti++)
                dash_total_tools += tool_metrics.entries[ti].calls;

            /* Session stats — the agentic loop has no fixed turn cap, so the
             * dashboard shows turns as unbounded (∞) rather than a fraction. */
            int max_t = 999999;
            fprintf(stderr, "  %s┌─ Session ───────────────────────────────────┐%s\n", TUI_DIM,
                    TUI_RESET);
            if (max_t >= 999999) {
                fprintf(
                    stderr,
                    "  %s│%s  Turns: %s%-4d%s %s(∞)%s  Tools: %s%-4d%s  Msgs: %s%-4d%s  %s│%s\n",
                    TUI_DIM, TUI_RESET, TUI_BWHITE, session.turn_count, TUI_RESET, TUI_DIM,
                    TUI_RESET, TUI_BWHITE, dash_total_tools, TUI_RESET, TUI_BWHITE, conv.count,
                    TUI_RESET, TUI_DIM, TUI_RESET);
            } else {
                fprintf(stderr,
                        "  %s│%s  Turns: %s%d/%d%s  Tools: %s%-4d%s  Msgs: %s%-4d%s  %s│%s\n",
                        TUI_DIM, TUI_RESET, TUI_BWHITE, session.turn_count, max_t, TUI_RESET,
                        TUI_BWHITE, dash_total_tools, TUI_RESET, TUI_BWHITE, conv.count, TUI_RESET,
                        TUI_DIM, TUI_RESET);
            }
            fprintf(stderr, "  %s│%s  Model: %s%-20s%s  Trust: %s%-8s%s %s│%s\n", TUI_DIM,
                    TUI_RESET, TUI_BCYAN, mi ? mi->alias : session.model, TUI_RESET, TUI_BYELLOW,
                    session_trust_tier_to_string(session.trust_tier), TUI_RESET, TUI_DIM,
                    TUI_RESET);
            fprintf(stderr, "  %s│%s  Topology: %s%-17s%s  Auto: %s%-3s%s %s│%s\n", TUI_DIM,
                    TUI_RESET, TUI_BWHITE,
                    session.active_topology[0] ? session.active_topology : "(none)", TUI_RESET,
                    TUI_BWHITE, session.topology_auto ? "on" : "off", TUI_RESET, TUI_DIM,
                    TUI_RESET);
            fprintf(stderr, "  %s└─────────────────────────────────────────────┘%s\n", TUI_DIM,
                    TUI_RESET);

            {
                swarm_t *sw = tools_swarm_instance();
                swarm_poll(sw, 0);
                if (sw->child_count > 0) {
                    double swarm_cost = 0.0;
                    for (int i = 0; i < sw->child_count; i++)
                        swarm_cost += sw->children[i].est_cost_usd;
                    fprintf(stderr, "\n  %s┌─ Swarm ────────────────────────────────────┐%s\n",
                            TUI_DIM, TUI_RESET);
                    fprintf(stderr,
                            "  %s│%s  Agents: %s%-4d%s  Groups: %s%-4d%s  Active: %s%-4d%s %s│%s\n",
                            TUI_DIM, TUI_RESET, TUI_BWHITE, sw->child_count, TUI_RESET, TUI_BWHITE,
                            sw->group_count, TUI_RESET, TUI_BCYAN, swarm_active_count(sw),
                            TUI_RESET, TUI_DIM, TUI_RESET);
                    fprintf(
                        stderr, "  %s│%s  Est:    %s$%.4f%s                                %s│%s\n",
                        TUI_DIM, TUI_RESET, TUI_BCYAN, swarm_cost, TUI_RESET, TUI_DIM, TUI_RESET);
                    fprintf(stderr, "  %s└─────────────────────────────────────────────┘%s\n",
                            TUI_DIM, TUI_RESET);
                }
            }

            /* Cost breakdown */
            fprintf(stderr, "\n  %s┌─ Cost ─────────────────────────────────────┐%s\n", TUI_DIM,
                    TUI_RESET);
            if (mi) {
                double in_cost = session.total_input_tokens * mi->input_price / 1e6;
                double out_cost = session.total_output_tokens * mi->output_price / 1e6;
                double cache_r = session.total_cache_read_tokens * mi->cache_read_price / 1e6;
                double cache_w = session.total_cache_write_tokens * mi->cache_write_price / 1e6;
                fprintf(stderr, "  %s│%s  Input:  %s$%.4f%s (%dk tok)                %s│%s\n",
                        TUI_DIM, TUI_RESET, TUI_GREEN, in_cost, TUI_RESET,
                        session.total_input_tokens / 1000, TUI_DIM, TUI_RESET);
                fprintf(stderr, "  %s│%s  Output: %s$%.4f%s (%dk tok)                %s│%s\n",
                        TUI_DIM, TUI_RESET, TUI_GREEN, out_cost, TUI_RESET,
                        session.total_output_tokens / 1000, TUI_DIM, TUI_RESET);
                fprintf(stderr, "  %s│%s  Cache:  %s$%.4f%s read + %s$%.4f%s write    %s│%s\n",
                        TUI_DIM, TUI_RESET, TUI_BCYAN, cache_r, TUI_RESET, TUI_BCYAN, cache_w,
                        TUI_RESET, TUI_DIM, TUI_RESET);
                fprintf(stderr, "  %s│%s  Total:  %s%s$%.4f%s                         %s│%s\n",
                        TUI_DIM, TUI_RESET, TUI_BOLD, TUI_BGREEN, cost, TUI_RESET, TUI_DIM,
                        TUI_RESET);
            }
            if (g_cost_budget > 0) {
                fprintf(stderr, "  %s│%s  Budget: $%.2f (%.0f%% used)               %s│%s\n",
                        TUI_DIM, TUI_RESET, g_cost_budget, 100.0 * cost / g_cost_budget, TUI_DIM,
                        TUI_RESET);
            }
            fprintf(stderr, "  %s└─────────────────────────────────────────────┘%s\n", TUI_DIM,
                    TUI_RESET);

            /* Context gauge */
            fprintf(stderr, "\n  %sContext:%s ", TUI_DIM, TUI_RESET);
            tui_context_gauge(ctx_used, ctx_max, 40);

            /* Streaming performance */
            if (session.telemetry_samples > 0) {
                fprintf(stderr, "  %s┌─ Streaming ─────────────────────────────────┐%s\n", TUI_DIM,
                        TUI_RESET);
                fprintf(stderr, "  %s│%s  Avg TTFT:  %s%.0fms%s                          %s│%s\n",
                        TUI_DIM, TUI_RESET, TUI_BCYAN, avg_ttft, TUI_RESET, TUI_DIM, TUI_RESET);
                fprintf(stderr, "  %s│%s  Avg tok/s: %s%.0f%s                             %s│%s\n",
                        TUI_DIM, TUI_RESET, TUI_BCYAN, avg_tps, TUI_RESET, TUI_DIM, TUI_RESET);
                fprintf(stderr, "  %s└─────────────────────────────────────────────┘%s\n", TUI_DIM,
                        TUI_RESET);
            }

            /* Top tools */
            if (tool_metrics.count > 0) {
                fprintf(stderr, "\n  %sTop Tools:%s\n", TUI_DIM, TUI_RESET);
                /* Find top 5 by call count */
                int indices[5] = {-1, -1, -1, -1, -1};
                for (int t = 0; t < 5 && t < tool_metrics.count; t++) {
                    int best = -1;
                    for (int i = 0; i < tool_metrics.count; i++) {
                        bool skip = false;
                        for (int j = 0; j < t; j++)
                            if (indices[j] == i)
                                skip = true;
                        if (skip)
                            continue;
                        if (best < 0 ||
                            tool_metrics.entries[i].calls > tool_metrics.entries[best].calls)
                            best = i;
                    }
                    if (best >= 0)
                        indices[t] = best;
                }
                for (int t = 0; t < 5; t++) {
                    if (indices[t] < 0)
                        break;
                    tool_metric_t *e = &tool_metrics.entries[indices[t]];
                    double avg = e->calls > 0 ? e->total_latency_ms / e->calls : 0;
                    fprintf(stderr, "    %s%d.%s %s%-20s%s %s%d calls%s  avg %s%.0fms%s\n", TUI_DIM,
                            t + 1, TUI_RESET, TUI_CYAN, e->name, TUI_RESET, TUI_BWHITE, e->calls,
                            TUI_RESET, TUI_DIM, avg, TUI_RESET);
                }
            }

            /* Cache hit rate */
            int cache_total = tool_cache.hits + tool_cache.misses;
            if (cache_total > 0) {
                fprintf(stderr, "\n  %sCache:%s %d/%d hits (%.0f%%)\n", TUI_DIM, TUI_RESET,
                        tool_cache.hits, cache_total, 100.0 * tool_cache.hits / cache_total);
            }

            /* Git branch */
            {
                FILE *gf = popen("git rev-parse --abbrev-ref HEAD 2>/dev/null", "r");
                if (gf) {
                    char branch[128] = "";
                    if (fgets(branch, sizeof(branch), gf)) {
                        size_t bl = strlen(branch);
                        if (bl > 0 && branch[bl - 1] == '\n')
                            branch[bl - 1] = '\0';
                        if (branch[0])
                            fprintf(stderr, "  %sGit:%s %s%s%s\n", TUI_DIM, TUI_RESET, TUI_BMAGENTA,
                                    branch, TUI_RESET);
                    }
                    pclose(gf);
                }
            }

            /* Fallback chain */
            if (session.fallback_count > 0) {
                fprintf(stderr, "  %sFallback:%s ", TUI_DIM, TUI_RESET);
                for (int fi = 0; fi < session.fallback_count; fi++)
                    fprintf(stderr, "%s%s%s%s", fi ? " → " : "", TUI_BYELLOW,
                            session.fallback_models[fi], TUI_RESET);
                fprintf(stderr, "\n");
            }

            /* Active features count */
            {
                int active = 0;
                const bool *flags = (const bool *)&g_features;
                for (int fi = 0; fi < TUI_FEATURE_COUNT; fi++)
                    if (flags[fi])
                        active++;
                fprintf(stderr, "  %sFeatures:%s %d/%d active\n", TUI_DIM, TUI_RESET, active,
                        TUI_FEATURE_COUNT);
            }

            fprintf(stderr, "\n");
            continue;
        }

        /* /top — tool leaderboard */
        if (strcmp(input_buf, "/top") == 0) {
            fprintf(stderr, "\n");
            tui_header("Tool Leaderboard", TUI_BCYAN);
            if (tool_metrics.count == 0) {
                fprintf(stderr, "  %sno tool calls recorded yet%s\n\n", TUI_DIM, TUI_RESET);
            } else {
                /* Sort indices by call count descending */
                int idx[256];
                int n = tool_metrics.count > 256 ? 256 : tool_metrics.count;
                for (int i = 0; i < n; i++)
                    idx[i] = i;
                for (int i = 0; i < n - 1; i++) {
                    for (int j = i + 1; j < n; j++) {
                        if (tool_metrics.entries[idx[j]].calls >
                            tool_metrics.entries[idx[i]].calls) {
                            int tmp = idx[i];
                            idx[i] = idx[j];
                            idx[j] = tmp;
                        }
                    }
                }
                fprintf(stderr, "  %s%-4s %-22s %6s %7s %8s %8s%s\n", TUI_DIM, "RANK", "TOOL",
                        "CALLS", "OK%", "AVG(ms)", "COST", TUI_RESET);

                const model_info_t *mi_top = model_lookup(session.model);
                for (int i = 0; i < n; i++) {
                    tool_metric_t *e = &tool_metrics.entries[idx[i]];
                    double avg = e->calls > 0 ? e->total_latency_ms / e->calls : 0;
                    double ok_pct = e->calls > 0 ? 100.0 * e->successes / e->calls : 0;
                    /* Estimate tool cost: rough token estimate per call */
                    double est_cost = 0;
                    if (mi_top)
                        est_cost = e->calls * 500.0 * (mi_top->input_price + mi_top->output_price) /
                                   2.0 / 1e6;

                    const char *speed_color =
                        avg < 500 ? TUI_GREEN : (avg < 2000 ? TUI_YELLOW : TUI_RED);
                    const char *ok_color =
                        ok_pct >= 95 ? TUI_GREEN : (ok_pct >= 80 ? TUI_YELLOW : TUI_RED);

                    fprintf(stderr, "  %s%2d.%s  %s%-22s%s %6d %s%6.0f%%%s %s%7.0f%s %s$%.3f%s\n",
                            TUI_DIM, i + 1, TUI_RESET, TUI_CYAN, e->name, TUI_RESET, e->calls,
                            ok_color, ok_pct, TUI_RESET, speed_color, avg, TUI_RESET, TUI_DIM,
                            est_cost, TUI_RESET);
                }
                fprintf(stderr, "\n");
            }
            continue;
        }

        /* /flame — flame timeline for last turn */
        if (strcmp(input_buf, "/flame") == 0) {
            fprintf(stderr, "\n");
            tui_header("Flame Timeline", TUI_BCYAN);
            if (s_flame.count == 0) {
                fprintf(stderr, "  %sno tool executions recorded this session%s\n\n", TUI_DIM,
                        TUI_RESET);
            } else {
                tui_flame_render(&s_flame);
                fprintf(stderr, "\n");
            }
            continue;
        }

    send_to_llm:
        if ((session.active_topology[0] || session.topology_auto) &&
            run_topology_prompt(&session, api_key, &conv, input_buf, &last_input_tokens)) {
            continue;
        }

        baseline_log("user", "prompt", input_buf, NULL);

        /* F19: Branch detection */
        tui_branch_detect(&s_branch, input_buf);
        tui_branch_push(&s_branch, input_buf);

        /* F21: Ghost suggestion history */
        tui_ghost_push(&s_ghost, input_buf);

        /* F22: Prompt token counter */
        if (g_features.prompt_tokens) {
            int est = tui_estimate_tokens(input_buf);
            int remaining =
                session.context_window - session.total_input_tokens - session.total_output_tokens;
            tui_prompt_token_display(est, remaining);
        }

        /* Prompt injection detection */
        injection_level_t inj = detect_prompt_injection(input_buf);
        if (inj == INJECTION_HIGH) {
            tui_warning("potential prompt injection detected (high confidence) — input sanitized");
            baseline_log("security", "injection_high", input_buf, NULL);
        } else if (inj == INJECTION_MED) {
            fprintf(stderr, "  %ssecurity: potential injection pattern detected%s\n", TUI_DIM,
                    TUI_RESET);
        }

        /* Check for image URLs (http/https links to images) */
        bool has_url_image = false;
        {
            char *url_start = strstr(input_buf, "http");
            if (url_start) {
                /* Extract the URL (ends at space or end of string) */
                char *url_end = url_start;
                while (*url_end && *url_end != ' ' && *url_end != '\t')
                    url_end++;
                size_t url_len = (size_t)(url_end - url_start);
                char url_buf[4096];
                if (url_len < sizeof(url_buf)) {
                    memcpy(url_buf, url_start, url_len);
                    url_buf[url_len] = '\0';
                    char ext_buf[32];
                    if (img_media_type_for_url(url_buf, ext_buf, sizeof(ext_buf))) {
                        /* Extract surrounding text */
                        char text_before[MAX_INPUT_LINE] = "";
                        char text_after[MAX_INPUT_LINE] = "";
                        if (url_start > input_buf) {
                            size_t pre = (size_t)(url_start - input_buf);
                            if (pre < sizeof(text_before)) {
                                memcpy(text_before, input_buf, pre);
                                text_before[pre] = '\0';
                            }
                        }
                        if (*url_end) {
                            snprintf(text_after, sizeof(text_after), "%s", url_end + 1);
                        }
                        /* Combine text */
                        char combined[MAX_INPUT_LINE];
                        snprintf(combined, sizeof(combined), "%s%s", text_before, text_after);
                        /* Trim */
                        char *txt = combined;
                        while (*txt == ' ')
                            txt++;
                        size_t tl = strlen(txt);
                        while (tl > 0 && txt[tl - 1] == ' ')
                            txt[--tl] = '\0';

                        tui_spinner_t spin;
                        tui_spinner_init(&spin, SPINNER_DOTS, "loading image from URL...",
                                         TUI_CYAN);
                        tui_spinner_tick(&spin);

                        const char *url_media_type = NULL;
                        char dl_err[160];
                        char *b64 = download_and_encode_image_url(url_buf, &url_media_type, &spin,
                                                                  dl_err, sizeof(dl_err));
                        if (b64) {
                            has_url_image = true;
                            conv_add_user_image_base64(&conv, url_media_type, b64,
                                                       tl > 0 ? txt : "Describe this image.");
                            free(b64);
                            tui_spinner_done(&spin, "image loaded from URL");
                        } else {
                            tui_spinner_done(&spin,
                                             dl_err[0] ? dl_err : "failed to load image URL");
                        }
                    }
                }
            }
        }

        /* Inject pinned context as first user message if set */
        if (session.pin_text[0] && conv.count == 0) {
            char pinbuf[1200];
            snprintf(pinbuf, sizeof(pinbuf), "[pinned] %s", session.pin_text);
            conv_add_user_text(&conv, pinbuf);
            conv_add_assistant_text(&conv, "Understood, I'll keep that context in mind.");
        }

        /* Check for dragged image file paths in the input */
        if (!has_url_image) {
            int n_images = process_dragged_images(input_buf, &conv);
            if (n_images == 0) {
                snprintf(last_user_input, sizeof(last_user_input), "%s", input_buf);
                conv_add_user_text(&conv, input_buf);
            }
        } else {
            snprintf(last_user_input, sizeof(last_user_input), "%s", input_buf);
        }

        /* Extract tool hints from user input */
        tools_hint_add_user(input_buf);

        int turns = 0;
        int total_input = 0, total_output = 0, total_cache_read = 0;
        int total_tools_used = 0;
        int pause_turn_streak = 0;

        /* Loop breaker: detect repeated identical cached tool failures */
        uint32_t last_fail_hash = 0;
        int consecutive_cached_fails = 0;
        tools_context_turn_begin();
        tools_loop_control_reset();
        tools_set_active_conversation(&conv);
        tools_playbook_advance_turn();
        arena_scratch_reset(); /* §2: reset per-turn scratch arena */

        /* Per-prompt trace ID */
        char trace_id[37];
        trace_new_id(trace_id, sizeof(trace_id));
        char prompt_span[37] = "";
        trace_span_begin(trace_id, "user_turn", NULL, prompt_span);

        /* Per-turn arena allocator */
        arena_t turn_arena;
        arena_init(&turn_arena);
        terminal_input_echo_suspend();
        esc_poller_start();
        g_turn_start_time = now_ms();
        bool prompt_done = false;
        /* Agentic loop: run to the goal, not to an arbitrary turn count. The
         * checkpoint cadence (base_turn_limit) only governs how often we surface
         * a "still working" status; hard_ceiling is the runaway backstop. Real
         * stops — cost budget, context exhaustion, interrupt, repeated identical
         * failures — break out of the loop body below, not via the count. */
        int base_turn_limit = tools_loop_control_effective_max_turns(dsco_max_agent_turns());
        int hard_ceiling = dsco_hard_turn_ceiling();
        if (hard_ceiling < base_turn_limit)
            hard_ceiling = base_turn_limit;
        int next_checkpoint = base_turn_limit;

    resume_turn_loop:
        while (turns < hard_ceiling && !g_interrupted) {
            turns++;
            md_reset(&s_md);

            /* Agentic checkpoint: at each cadence boundary the loop is still
             * running because the model keeps issuing tool calls AND we are
             * under budget (cost is hard-checked below). Surface a visible
             * "still working" line — not a stop — so the user sees convergence
             * and can interrupt if it's spinning. */
            if (turns >= next_checkpoint) {
                double ck_cost = session_cost(&session);
                int ck_eff = effective_context_window(&session);
                int ck_used = session.total_input_tokens + session.total_output_tokens;
                int ck_ctx = ck_eff > 0 ? (int)(100.0 * ck_used / ck_eff) : 0;
                if (g_cost_budget > 0)
                    fprintf(stderr,
                            "  %s↻ turn %d — continuing toward goal "
                            "(cost $%.2f/$%.2f · ctx %d%%) · Esc to pause%s\n",
                            TUI_DIM, turns, ck_cost, g_cost_budget, ck_ctx, TUI_RESET);
                else
                    fprintf(stderr,
                            "  %s↻ turn %d — continuing toward goal "
                            "(cost $%.2f · ctx %d%%) · Esc to pause%s\n",
                            TUI_DIM, turns, ck_cost, ck_ctx, TUI_RESET);
                baseline_log("agent", "turn_checkpoint", NULL, NULL);
                next_checkpoint += base_turn_limit;
            }

            /* Self-improvement: close out the turn that just finished (recording
             * its cost/token/context deltas) and reset per-turn counters for the
             * new one. The just-completed turn's tool calls were recorded inline. */
            {
                static double si_prev_cost = 0.0;
                static int si_prev_in = 0, si_prev_out = 0;
                if (turns > 1) {
                    double c = session_cost(&session);
                    int in = session.total_input_tokens;
                    int out = session.total_output_tokens;
                    int eff = effective_context_window(&session);
                    int ctx_pct = eff > 0 ? (int)(100.0 * (in + out) / eff) : 0;
                    double budget_pct = g_cost_budget > 0 ? 100.0 * c / g_cost_budget : 0.0;
                    SI_RECORD_TURN(turns - 1, c - si_prev_cost, in - si_prev_in, out - si_prev_out,
                                   ctx_pct, budget_pct);
                    si_prev_cost = c;
                    si_prev_in = in;
                    si_prev_out = out;
                }
                SI_TURN_RESET();
            }

            /* Decay tool hints each turn */
            tools_hint_decay();

            /* Co-occurrence temporal decay: every 10 turns, multiply all
             * counters by 0.95 to forget stale tool co-occurrence patterns.
             * Prevents ancient patterns from dominating predictions. */
            if (turns % 10 == 0 && turns > 0)
                tools_cooc_decay(0.95f);

            /* Update budget ratio for adaptive tool paging.
             * Uses output-aware context window for accurate pressure. */
            {
                double cost = session_cost(&session);
                float cost_ratio = (g_cost_budget > 0) ? (float)(1.0 - cost / g_cost_budget) : 1.0f;

                /* Also factor in context pressure */
                int eff_win = effective_context_window(&session);
                int used = session.total_input_tokens + session.total_output_tokens;
                float ctx_ratio = (eff_win > 0) ? (float)(eff_win - used) / (float)eff_win : 1.0f;

                /* Use the more constrained of cost vs context */
                session.tool_budget_ratio = (cost_ratio < ctx_ratio) ? cost_ratio : ctx_ratio;
                if (session.tool_budget_ratio < 0.0f)
                    session.tool_budget_ratio = 0.0f;
            }

            /* API quorum gate: cheap model pre-filters tool groups (opt-in).
             * Fires every 3 turns to amortize gate cost. Injects HINT_PLAN
             * hints that bias the register-file tool selection. */
            if (turns % 3 == 1) {
                const char *last_user_msg = NULL;
                for (int ci = conv.count - 1; ci >= 0; ci--) {
                    if (conv.msgs[ci].role == ROLE_USER) {
                        for (int cj = 0; cj < conv.msgs[ci].content_count; cj++) {
                            if (conv.msgs[ci].content[cj].text) {
                                last_user_msg = conv.msgs[ci].content[cj].text;
                                goto found_user_msg;
                            }
                        }
                    }
                }
            found_user_msg:
                if (last_user_msg)
                    tool_quorum_gate_api(last_user_msg, api_key);
            }

            if (conv.count > 10) {
                int est = conv_token_estimate(&conv, &session);
                int thresh = auto_compact_threshold(&session);
                if (est > thresh * 3 / 4) { /* start micro-compact at 75% of threshold */
                    int before = conv.count;
                    conv_strip_binaries(&conv, 6);
                    conv_trim_old_results(&conv, 6, 256);
                    /* F17: Auto-compact notification */
                    if (conv.count < before)
                        tui_compact_flash(before, conv.count);
                }
            }

            /* Phase 3: Memory → context injection (Claude Code methodology).
               Every 3 turns, search semantic memory for relevant entries and
               inject top-5 into the system prompt for the next API call. */
            session.memory_context[0] = '\0';
            if (g_agent_memory_inited && turns % 3 == 1) {
                const char *last_user_text = NULL;
                for (int ci = conv.count - 1; ci >= 0; ci--) {
                    if (conv.msgs[ci].role == ROLE_USER) {
                        for (int cj = 0; cj < conv.msgs[ci].content_count; cj++) {
                            if (conv.msgs[ci].content[cj].text &&
                                conv.msgs[ci].content[cj].type == NULL) {
                                last_user_text = conv.msgs[ci].content[cj].text;
                                goto found_user_for_memory;
                            }
                        }
                    }
                }
            found_user_for_memory:
                if (last_user_text) {
                    const memory_entry_t *hits[5];
                    int nhits = memory_search_semantic(&g_agent_memory, last_user_text, hits, 5);
                    if (nhits > 0) {
                        int pos = snprintf(session.memory_context, sizeof(session.memory_context),
                                           "[Recalled from memory (%d entries)]\n", nhits);
                        for (int mi2 = 0;
                             mi2 < nhits && pos < (int)sizeof(session.memory_context) - 200;
                             mi2++) {
                            pos += snprintf(session.memory_context + pos,
                                            sizeof(session.memory_context) - (size_t)pos,
                                            "- %s: %.*s\n", hits[mi2]->key, 300, hits[mi2]->value);
                        }
                    }
                }
            }

            /* Cost budget check */
            if (!check_cost_budget(&session)) {
                tui_error("cost budget exceeded — /budget <n> raises both caps, or /budget off");
                break;
            }

            /* Rate limit */
            if (!rate_limiter_acquire(&rate_limiter) || g_interrupted) {
                break;
            }

            /* Build request via provider */
            ensure_provider(&session, api_key);
            const char *cur_key = resolve_provider_key(api_key);
            provider_debug_log_request(g_provider ? g_provider->name : "anthropic", session.model,
                                       cur_key);
            char *req =
                g_provider
                    ? g_provider->build_request(g_provider, &conv, &session, MAX_TOKENS, cur_key)
                    : llm_build_request_ex_for_credential(&conv, &session, MAX_TOKENS, cur_key);
            if (!req) {
                tui_error("failed to build request");
                baseline_log("error", "request_build_failed", NULL, NULL);
                break;
            }

            /* Pre-flight context check: output-aware tiered compaction.
             * Estimate from the conversation model (rough(conv) + API-measured
             * overhead), NOT strlen(req)/4 — the serialized request inlines
             * base64 image data, which is ~200x larger than its true token
             * cost and was triggering phantom every-turn compaction loops. */
            {
                int est_tokens = conv_token_estimate(&conv, &session);
                int eff_window = effective_context_window(&session);
                int thresh = auto_compact_threshold(&session);
                if (est_tokens > thresh) {
                    /* §9: Preserve context summary in episodic memory before compaction */
                    {
                        agent_memory_ensure_init();
                        char compact_key[128];
                        snprintf(compact_key, sizeof(compact_key), "compact_t%d_%d",
                                 session.turn_count, (int)time(NULL));
                        char summary[MEMTIER_VALUE_LEN];
                        snprintf(summary, sizeof(summary),
                                 "Context compacted at turn %d. %d messages in conversation. "
                                 "Estimated %dk tokens, threshold %dk.",
                                 session.turn_count, conv.count, est_tokens / 1000, thresh / 1000);
                        memory_store(&g_agent_memory, MEM_EPISODIC, compact_key, summary, 0.8);
                    }

                    /* Tiered auto-compact: micro → snip, with circuit breaker */
                    compact_result_t cr = conv_auto_compact(&conv, &session, &compact_cfg);

                    /* Post-compact: re-inject recently-read files */
                    if (cr.tier_used >= COMPACT_SNIP)
                        post_compact_restore_inject(&file_restore, &conv);

                    free(req);
                    req = g_provider ? g_provider->build_request(g_provider, &conv, &session,
                                                                 MAX_TOKENS, cur_key)
                                     : llm_build_request_ex_for_credential(&conv, &session,
                                                                           MAX_TOKENS, cur_key);
                    if (!req) {
                        tui_error("failed to rebuild request after auto-compact");
                        break;
                    }
                    int new_tokens = conv_token_estimate(&conv, &session);
                    if (g_outq) {
                        tui_outq_writef(g_outq,
                                        "  \033[33m%s auto-compact (tier %d): %dk→%dk tokens "
                                        "(%.0f%%→%.0f%% of %dk effective)\033[0m\n",
                                        tui_glyph()->warn, cr.tier_used, est_tokens / 1000,
                                        new_tokens / 1000, 100.0 * est_tokens / eff_window,
                                        100.0 * new_tokens / eff_window, eff_window / 1000);
                    } else {
                        fprintf(stderr,
                                "  \033[33m%s auto-compact (tier %d): %dk→%dk tokens "
                                "(%.0f%%→%.0f%% of %dk effective)\033[0m\n",
                                tui_glyph()->warn, cr.tier_used, est_tokens / 1000,
                                new_tokens / 1000, 100.0 * est_tokens / eff_window,
                                100.0 * new_tokens / eff_window, eff_window / 1000);
                    }
                }
            }

            /* Stream via provider with fallback chain */
            char llm_span[37] = "";
            /* Reactive-compaction retry state (SOTA, cf. Claude Code
             * reactive_compact_retry): on a context-overflow rejection we
             * compact and re-stream this same request instead of ending the
             * turn. Declared before the retry label so it survives the goto. */
            int reactive_attempts = 0;
            stream_result_t sr;
        reactive_retry:
            trace_span_begin(trace_id, "llm_stream", prompt_span, llm_span);

            /* Start heartbeat — auto-detects silent stream and shows spinner */
            tui_stream_heartbeat_start(&s_heartbeat);
            g_stream_heartbeat = &s_heartbeat;

            /* FSM: mark stream start */
            tui_fsm_send(&s_streaming_fsm, TUI_FSM_EVT_STREAM_START);
            s_turn_streamed_text = false;

            /* Update terminal title with turn info */
            tui_set_title_fmt("dsco · %s · turn %d", session.model, turns);

            sr = g_provider ? g_provider->stream(g_provider, cur_key, req, on_stream_text,
                                                 on_stream_tool_start, on_stream_thinking, NULL)
                            : llm_stream(api_key, req, on_stream_text, on_stream_tool_start,
                                         on_stream_thinking, NULL);

            /* Fallback: if failed and fallback chain configured, try next model */
            if (!sr.ok && session.fallback_count > 0) {
                for (int fi = 0; fi < session.fallback_count && !sr.ok && !g_interrupted; fi++) {
                    const char *fb_model = session.fallback_models[fi];
                    if (strcmp(fb_model, session.model) == 0)
                        continue;

                    if (g_outq)
                        tui_outq_writef(g_outq, "  %sfallback: trying %s%s\n", TUI_YELLOW, fb_model,
                                        TUI_RESET);
                    else
                        fprintf(stderr, "  %sfallback: trying %s%s\n", TUI_YELLOW, fb_model,
                                TUI_RESET);
                    tui_stream_heartbeat_poke(&s_heartbeat, "fallback...");
                    json_free_response(&sr.parsed);

                    /* Rebuild request with fallback model */
                    char saved_model[128];
                    snprintf(saved_model, sizeof(saved_model), "%s", session.model);
                    snprintf(session.model, sizeof(session.model), "%s", fb_model);
                    ensure_provider(&session, api_key);
                    const char *fb_key = resolve_provider_key(api_key);

                    free(req);
                    req = g_provider ? g_provider->build_request(g_provider, &conv, &session,
                                                                 MAX_TOKENS, fb_key)
                                     : llm_build_request_ex_for_credential(&conv, &session,
                                                                           MAX_TOKENS, fb_key);
                    if (!req)
                        break;

                    sr = g_provider
                             ? g_provider->stream(g_provider, fb_key, req, on_stream_text,
                                                  on_stream_tool_start, on_stream_thinking, NULL)
                             : llm_stream(fb_key, req, on_stream_text, on_stream_tool_start,
                                          on_stream_thinking, NULL);

                    if (sr.ok) {
                        if (g_outq)
                            tui_outq_writef(g_outq, "  %sfallback succeeded with %s%s\n", TUI_GREEN,
                                            fb_model, TUI_RESET);
                        else
                            fprintf(stderr, "  %sfallback succeeded with %s%s\n", TUI_GREEN,
                                    fb_model, TUI_RESET);
                    } else {
                        /* Restore original model for next fallback attempt */
                        snprintf(session.model, sizeof(session.model), "%s", saved_model);
                    }
                }
            }
            free(req);

            /* Stop heartbeat — stream is done */
            g_stream_heartbeat = NULL;
            tui_stream_heartbeat_stop(&s_heartbeat);

            /* Reset forced tool_choice after first turn (single-shot) */
            if (turns == 1 && session.tool_choice[0]) {
                session.tool_choice[0] = '\0';
            }

            if (!sr.ok) {
                /* SOTA reactive compaction (cf. Claude Code reactive_compact_retry):
                 * if the provider rejected the prompt as too long, aggressively but
                 * structure-preservingly compact the conversation (hard-trim OLD
                 * tool results + strip images, keeping the recent turns intact) and
                 * re-stream the SAME request — up to 2 escalating attempts — instead
                 * of ending the turn. Old results are re-derivable; the model keeps
                 * its recent working context. */
                bool overflow =
                    sr.context_overflow || provider_msg_is_context_overflow(dsco_err_msg());
                if (overflow && reactive_attempts < 2 && !g_interrupted) {
                    reactive_attempts++;
                    json_free_response(&sr.parsed);
                    trace_span_end(llm_span, "reactive_compact", NULL);
                    g_stream_heartbeat = NULL;
                    tui_stream_heartbeat_stop(&s_heartbeat);
                    dsco_err_clear();
                    /* Escalate aggressiveness across the two attempts. */
                    int keep = reactive_attempts == 1 ? 4 : 2;
                    int budget = reactive_attempts == 1 ? 200 : 100;
                    conv_strip_binaries(&conv, keep);
                    conv_trim_old_results(&conv, keep, budget);
                    post_compact_restore_inject(&file_restore, &conv);
                    free(req);
                    req = g_provider ? g_provider->build_request(g_provider, &conv, &session,
                                                                 MAX_TOKENS, cur_key)
                                     : llm_build_request_ex_for_credential(&conv, &session,
                                                                           MAX_TOKENS, cur_key);
                    if (req) {
                        fprintf(stderr,
                                "  %s\xe2\x86\xaf prompt too long \xe2\x80\x94 reactive compaction "
                                "(attempt %d/2, keep %d), retrying%s\n",
                                TUI_DIM, reactive_attempts, keep, TUI_RESET);
                        baseline_log("agent", "reactive_compact_retry", NULL, NULL);
                        goto reactive_retry;
                    }
                    /* request rebuild failed — fall through to the error path */
                }
                char err[128];
                snprintf(err, sizeof(err), "stream failed (HTTP %d)", sr.http_status);
                trace_span_end(llm_span, "error", NULL);
                /* Show structured error if available */
                if (dsco_err_code() != DSCO_ERR_OK) {
                    if (g_outq)
                        tui_outq_writef(g_outq, "  \033[2m[%s] %s\033[0m\n",
                                        dsco_err_code_str(dsco_err_code()), dsco_err_msg());
                    else
                        fprintf(stderr, "  \033[2m[%s] %s\033[0m\n",
                                dsco_err_code_str(dsco_err_code()), dsco_err_msg());
                    dsco_err_clear();
                }
                tui_error(err);
                baseline_log("error", "stream_failed", err, NULL);
                json_free_response(&sr.parsed);
                if (turns == 1) {
                    conv_pop_last(&conv);
                }
                break;
            }
            trace_span_end(llm_span, "ok", NULL);

            /* Drain any bytes still held by the typing-cadence buffer
               before STREAM_END fires md_flush — otherwise the tail of
               the response would be discarded. */
            if (g_features.typing_cadence)
                tui_cadence_drain(&s_cadence);

            /* FSM-driven cleanup — STREAM_END triggers text_exit
               (md_flush + newline) or thinking_exit as needed, then
               transitions to 'done'. */
            {
                int st = s_streaming_fsm.current;
                if (st != TUI_STREAM_ST_IDLE && st != TUI_STREAM_ST_DONE)
                    tui_fsm_send(&s_streaming_fsm, TUI_FSM_EVT_STREAM_END);
            }

            /* Print OpenRouter/provider metadata AFTER md_flush has completed
             * (STREAM_END above already called md_flush via fsm_text_exit).
             * Printing inside openai_stream() caused partial-echo duplication:
             * the footer text extended the terminal line but md_flush computed
             * erase width from stale partial_echo_pos → re-rendered last line. */
            if (sr.actual_model || sr.cost_usd > 0 || sr.generation_id) {
                fprintf(stderr, "  \033[2m");
                if (sr.actual_model)
                    fprintf(stderr, "model=%s ", sr.actual_model);
                if (sr.cost_usd > 0)
                    fprintf(stderr, "$%.6f ", sr.cost_usd);
                if (sr.usage.cache_read_input_tokens > 0)
                    fprintf(stderr, "cached=%d ", sr.usage.cache_read_input_tokens);
                if (sr.reasoning_tokens > 0)
                    fprintf(stderr, "reasoning=%d ", sr.reasoning_tokens);
                if (sr.generation_id)
                    fprintf(stderr, "gen=%s", sr.generation_id);
                fprintf(stderr, "\033[0m\n");
            }
            free(sr.actual_model);   sr.actual_model = NULL;
            free(sr.generation_id);  sr.generation_id = NULL;

                        total_input += sr.usage.input_tokens;
            total_output += sr.usage.output_tokens;
            total_cache_read += sr.usage.cache_read_input_tokens;
            last_input_tokens = sr.usage.input_tokens;

            /* Feed token usage to tools layer for inline budget calculation */
            tools_set_context_usage(sr.usage.input_tokens, sr.usage.output_tokens);

            /* Accumulate session cost */
            session.total_input_tokens += sr.usage.input_tokens;
            session.total_output_tokens += sr.usage.output_tokens;
            session.total_cache_read_tokens += sr.usage.cache_read_input_tokens;
            session.total_cache_write_tokens += sr.usage.cache_creation_input_tokens;
            /* Accumulate authoritative cost: trust the provider's reported cost
             * (OpenRouter usage.cost) when present, else fall back to token math.
             * This is what the budget enforces, so caching discounts count. */
            session.total_reported_cost_usd +=
                (sr.cost_usd > 0) ? sr.cost_usd : turn_token_cost(&session, &sr.usage);
            session.turn_count++;

            /* Calibrate non-conv overhead from this response's input usage.
             * The API counted (system_prompt + tool_schemas + cache_prefix + conv).
             * We know rough(conv) — the delta is everything outside conv.
             * conv_token_estimate adds this back so threshold checks see the
             * true context size, but pre/post compaction deltas still
             * reflect real shrinkage of `conv`. */
            session.last_input_tokens = sr.usage.input_tokens;
            {
                int conv_rough = conv_rough_estimate(&conv);
                int overhead = sr.usage.input_tokens - conv_rough;
                if (overhead < 0)
                    overhead = 0;
                session.non_conv_overhead_tokens = overhead;
            }

            /* Pre-count tool_use blocks so we can suppress noisy per-turn
               usage/telemetry lines when tools will show inline metadata */
            int tool_count_this_turn = 0;
            for (int ti = 0; ti < sr.parsed.count; ti++) {
                content_block_t *tb = &sr.parsed.blocks[ti];
                if (tb->type && strcmp(tb->type, "tool_use") == 0)
                    tool_count_this_turn++;
            }

            /* Surface answer text that arrived but was never streamed live.
             * Happens when the provider promoted a reasoning-only turn to a
             * text block (empty delta.content + non-empty delta.reasoning):
             * on_stream_text never fired, so the FSM went thinking→done and
             * printed nothing. Render the block here so the turn isn't silent. */
            if (tool_count_this_turn == 0 && !s_turn_streamed_text) {
                for (int ti = 0; ti < sr.parsed.count; ti++) {
                    content_block_t *tb = &sr.parsed.blocks[ti];
                    if (tb->type && strcmp(tb->type, "text") == 0 && tb->text && tb->text[0]) {
                        tui_term_lock();
                        print_role_header("assistant", true, NULL);
                        fputs("  ", stderr);
                        md_feed_str(&s_md, tb->text);
                        md_flush(&s_md);
                        fprintf(stderr, "\n");
                        fflush(stderr);
                        tui_term_unlock();
                        break;
                    }
                }
            }

            /* Only print usage/telemetry for non-tool turns (tool turns fold it inline) */
            if (tool_count_this_turn == 0)
                print_usage_ex(&sr.usage, session.model, &session);

            /* Update status bar with full cost (including cache pricing).
             * Don't render here — the panel is ephemeral and not on screen
             * between turns; painting the powerline at row N would overwrite
             * the last line of the response. The next composer_read repaints
             * the panel with the updated values. */
            {
                double cost = session_cost(&session);
                tui_status_bar_update(&status_bar, session.total_input_tokens,
                                      session.total_output_tokens, cost, session.turn_count,
                                      total_tools_used);
            }

            /* Streaming telemetry — suppress for tool turns */
            if (sr.telemetry.ttft_ms > 0) {
                session.total_ttft_ms += sr.telemetry.ttft_ms;
                session.total_stream_ms += sr.telemetry.total_ms;
                session.telemetry_samples++;
                if (tool_count_this_turn == 0)
                    fprintf(stderr, "%s  [ttft:%.0fms total:%.0fms %.0f tok/s]%s\n", TUI_DIM,
                            sr.telemetry.ttft_ms, sr.telemetry.total_ms,
                            sr.telemetry.tokens_per_sec, TUI_RESET);
                /* Paging telemetry: log tier sizes and retrieval stats */
                if (g_page_telemetry.retrieval_ms > 0) {
                    fprintf(stderr,
                            "%s  [paging: P%d W%d D%d hints:%d cooc:%d emb:%d %.1fms -%dtok]%s\n",
                            TUI_DIM, g_page_telemetry.pinned_count, g_page_telemetry.working_count,
                            g_page_telemetry.discovery_count, g_page_telemetry.hint_count,
                            g_page_telemetry.cooc_predictions, g_page_telemetry.centroid_matches,
                            g_page_telemetry.retrieval_ms, g_page_telemetry.schema_tokens_saved,
                            TUI_RESET);
                }
                /* F40: Save latency breakdown for /perf */
                s_last_latency.dns_ms = sr.telemetry.latency.dns_ms;
                s_last_latency.connect_ms = sr.telemetry.latency.connect_ms;
                s_last_latency.tls_ms = sr.telemetry.latency.tls_ms;
                s_last_latency.ttfb_ms = sr.telemetry.latency.ttfb_ms;
                s_last_latency.total_ms = sr.telemetry.latency.total_ms;
            }
            /* F39: Throughput data kept for /perf; rendered inline in section divider */
            /* F7: Citations already shown inline as [N] markers during streaming */
            tui_citation_init(&s_citations); /* reset for next turn */
            /* Reset per-turn flame + DAG (data kept for /perf) */
            tui_flame_init(&s_flame);
            tui_dag_init(&s_dag);

            /* ── Router: record turn + decide successor model ─────────────── */
            {
                const model_info_t *cur_mi = model_lookup(session.model);
                double turn_cost = 0.0;
                if (cur_mi) {
                    turn_cost =
                        sr.usage.input_tokens * cur_mi->input_price / 1e6 +
                        sr.usage.output_tokens * cur_mi->output_price / 1e6 +
                        sr.usage.cache_read_input_tokens * cur_mi->cache_read_price / 1e6 +
                        sr.usage.cache_creation_input_tokens * cur_mi->cache_write_price / 1e6;
                }
                router_record_turn(&g_router, session.model, sr.usage.input_tokens,
                                   sr.usage.output_tokens, sr.telemetry.total_ms, turn_cost,
                                   sr.telemetry.tokens_per_sec, sr.ok);

                if (tool_count_this_turn == 0) {
                    /* Only suggest after non-tool turns to avoid noise */
                    int ctx_window_now =
                        session.context_window > 0 ? session.context_window : CONTEXT_WINDOW_TOKENS;
                    int ctx_pct = sr.usage.input_tokens * 100 /
                                  (ctx_window_now > 0 ? ctx_window_now : 200000);
                    task_complexity_t complexity =
                        router_classify_task(NULL, session.turn_count, 0, ctx_pct);
                    router_decision_t rd =
                        router_decide(&g_router, session.model, complexity, session_cost(&session),
                                      sr.telemetry.total_ms, consecutive_tool_failures);
                    if (rd.should_switch && rd.model_id[0] &&
                        strcmp(rd.model_id, session.model) != 0 && !session.model_locked) {
                        const char *cur_pname =
                            g_provider ? g_provider->name
                                       : provider_route_for_model(session.model, api_key,
                                                                  g_provider_override_name);
                        const char *rd_pname = NULL;
                        bool rd_routable = provider_model_is_routable(
                            rd.model_id, api_key, g_provider_override_name, &rd_pname);
                        bool cross_provider =
                            rd_pname && cur_pname && strcmp(cur_pname, rd_pname) != 0;
                        bool cross_disabled =
                            env_truthy(getenv("DSCO_DISABLE_CROSS_PROVIDER_ROUTING"));

                        if (g_provider_override_name &&
                            strcmp(g_provider_override_name, "openrouter") != 0 &&
                            strcmp(provider_detect(rd.model_id, NULL), g_provider_override_name) !=
                                0)
                            goto skip_router_switch;
                        if (cross_provider && cross_disabled)
                            goto skip_router_switch;
                        if (rd_routable) {
                            char prev_model[128];
                            snprintf(prev_model, sizeof(prev_model), "%s", session.model);
                            snprintf(session.model, sizeof(session.model), "%s", rd.model_id);
                            session.context_window = model_context_window(rd.model_id);
                            tools_set_runtime_model(session.model);
                            ensure_provider(&session, api_key);
                            fprintf(
                                stderr,
                                "  \033[2m[router] %s → %s  reason=%s  confidence=%.0f%%\033[0m\n",
                                prev_model, session.model, switch_reason_name(rd.reason),
                                (double)rd.confidence * 100.0);
                        }
                    skip_router_switch:;
                    }
                }
            }

            /* Token budget awareness — output-aware threshold */
            {
                int eff_window = effective_context_window(&session);
                int thresh = auto_compact_threshold(&session);
                if (sr.usage.input_tokens > thresh) {
                    fprintf(
                        stderr,
                        "  \033[33m\xe2\x9a\xa0 token budget: %dk/%dk effective (%.0f%%)\033[0m\n",
                        sr.usage.input_tokens / 1000, eff_window / 1000,
                        100.0 * sr.usage.input_tokens / eff_window);
                    /* Tiered compaction with circuit breaker */
                    compact_result_t cr = conv_auto_compact(&conv, &session, &compact_cfg);
                    if (cr.tier_used >= COMPACT_SNIP)
                        post_compact_restore_inject(&file_restore, &conv);
                } else if (sr.usage.input_tokens > (int)(eff_window * TOKEN_BUDGET_WARN)) {
                    fprintf(stderr,
                            "  \033[33m\xe2\x9a\xa0 context pressure: %dk/%dk (%.0f%%)\033[0m\n",
                            sr.usage.input_tokens / 1000, eff_window / 1000,
                            100.0 * sr.usage.input_tokens / eff_window);
                    /* Light compaction: strip binaries + trim old results */
                    conv_strip_binaries(&conv, 6);
                    conv_trim_old_results(&conv, 6, 256);
                }
            }

            conv_add_assistant_raw(&conv, &sr.parsed);

            /* Execute tools — parallel when multiple independent calls.
               Continue the loop only when we created follow-up user input
               (local tool results, tool-generated media, etc.). */
            bool needs_followup_turn = false;

            /* tool_count_this_turn already computed above */
            total_tools_used += tool_count_this_turn;

            if (tool_count_this_turn == 1) {
                /* Single tool — execute inline with metrics + caching + watchdog */
                needs_followup_turn = true;
                for (int i = 0; i < sr.parsed.count; i++) {
                    content_block_t *blk = &sr.parsed.blocks[i];
                    if (blk->type && strcmp(blk->type, "tool_use") == 0) {
                        /* Show merged ⚡ name + args on one line */
                        print_tool_start_line(blk->tool_name, blk->tool_input);

                        char trust_reason[256];
                        const char *tier = session_trust_tier_to_string(session.trust_tier);
                        const char *exec_tier = tier;
                        if (!tools_is_allowed_for_tier(blk->tool_name, tier, trust_reason,
                                                       sizeof(trust_reason))) {
                            char deny_reason[256];
                            if (!maybe_escalate_tool_permission(blk->tool_name, tier, trust_reason,
                                                                &exec_tier, deny_reason,
                                                                sizeof(deny_reason))) {
                                print_tool_result(blk->tool_name, false, deny_reason);
                                baseline_log("security", "tool_blocked", deny_reason, NULL);
                                conv_add_tool_result_named(&conv, blk->tool_id, blk->tool_name,
                                                           deny_reason, true);
                                break;
                            }
                        }

                        /* Validate input schema */
                        char val_err[256];
                        if (!tools_validate_input(blk->tool_name, blk->tool_input, val_err,
                                                  sizeof(val_err))) {
                            fprintf(stderr, "  \033[31m\xe2\x9c\x98 %s\033[0m\n", val_err);
                            conv_add_tool_result_named(&conv, blk->tool_id, blk->tool_name, val_err,
                                                       true);
                            break;
                        }

                        char *tool_result = safe_malloc(MAX_TOOL_RESULT);
                        tool_result[0] = '\0';
                        bool ok = false;

                        /* Check cache (under lock) */
                        pthread_mutex_lock(&g_locks.cache_lock);
                        bool cache_hit =
                            tool_cache_get(&tool_cache, blk->tool_name, blk->tool_input,
                                           tool_result, MAX_TOOL_RESULT, &ok);
                        pthread_mutex_unlock(&g_locks.cache_lock);
                        dsco_strip_terminal_controls_inplace(tool_result);

                        if (cache_hit) {
                            /* Loop breaker: detect repeated identical cached failures */
                            if (!ok) {
                                uint32_t h = 2166136261u;
                                for (const char *p = blk->tool_name; *p; p++)
                                    h = (h ^ (uint8_t)*p) * 16777619u;
                                if (blk->tool_input) {
                                    for (const char *p = blk->tool_input; *p; p++)
                                        h = (h ^ (uint8_t)*p) * 16777619u;
                                }
                                if (h == last_fail_hash) {
                                    consecutive_cached_fails++;
                                } else {
                                    last_fail_hash = h;
                                    consecutive_cached_fails = 1;
                                }
                                if (consecutive_cached_fails >= 2) {
                                    /* Append stop nudge to break the loop */
                                    size_t tl = strlen(tool_result);
                                    snprintf(
                                        tool_result + tl, MAX_TOOL_RESULT - tl,
                                        "\n\n[STOP: You have called %s with identical arguments %d "
                                        "times "
                                        "and gotten the same error. Do NOT retry. Try a different "
                                        "tool or approach, or ask the user for help.]",
                                        blk->tool_name, consecutive_cached_fails);
                                    fprintf(
                                        stderr,
                                        "  %s⚠ loop detected: %s called %dx with same error%s\n",
                                        TUI_BYELLOW, blk->tool_name, consecutive_cached_fails,
                                        TUI_RESET);
                                }
                            } else {
                                consecutive_cached_fails = 0;
                            }

                            /* ▌ tool_response  name · cached  [in:.. out:.. $..] */
                            char trail[256];
                            int tpos =
                                snprintf(trail, sizeof(trail), "%s · cached", blk->tool_name);
                            {
                                const model_info_t *mi = model_lookup(session.model);
                                if (mi && tpos < (int)sizeof(trail)) {
                                    double tc = sr.usage.input_tokens * mi->input_price / 1e6 +
                                                sr.usage.output_tokens * mi->output_price / 1e6 +
                                                sr.usage.cache_read_input_tokens *
                                                    mi->cache_read_price / 1e6 +
                                                sr.usage.cache_creation_input_tokens *
                                                    mi->cache_write_price / 1e6;
                                    snprintf(trail + tpos, sizeof(trail) - tpos,
                                             "  [in:%d out:%d $%.4f]", sr.usage.input_tokens,
                                             sr.usage.output_tokens, tc);
                                }
                            }
                            print_role_header("tool_response", ok, trail);
                            /* Indented preview body — first line, up to 80 cols. */
                            if (tool_result[0]) {
                                const char *nl = strchr(tool_result, '\n');
                                int plen = nl ? (int)(nl - tool_result) : (int)strlen(tool_result);
                                if (plen > 80)
                                    plen = 80;
                                if (plen > 0)
                                    fprintf(stderr, "  %s%.*s%s\n", TUI_DIM, plen, tool_result,
                                            TUI_RESET);
                            }
                        } else {
                            /* Trace span for tool execution */
                            char tool_span[37] = "";
                            trace_span_begin(trace_id, blk->tool_name, prompt_span, tool_span);

                            /* Start async spinner + watchdog */
                            tui_async_spinner_t spinner;
                            tui_tool_type_t tt = tui_classify_tool(blk->tool_name);
                            tui_async_spinner_start(&spinner, blk->tool_name, tt);

                            tool_watchdog_t wd;
                            int timeout = tool_timeout_for(blk->tool_name);
                            g_tool_timed_out = 0;
                            watchdog_start(&wd, pthread_self(), blk->tool_name, timeout);
                            tl_tool_cancelled = 0;

                            double t0 = now_ms();
                            ok = tools_execute_for_tier(blk->tool_name, blk->tool_input, exec_tier,
                                                        tool_result, MAX_TOOL_RESULT);
                            dsco_strip_terminal_controls_inplace(tool_result);

                            /* Warn model if output was truncated */
                            size_t result_len = strlen(tool_result);
                            if (result_len >= MAX_TOOL_RESULT - 256) {
                                size_t cur = strlen(tool_result);
                                snprintf(tool_result + cur, MAX_TOOL_RESULT - cur,
                                         "\n[WARNING: output truncated at %zu bytes — full output "
                                         "exceeds %d byte limit]",
                                         result_len, MAX_TOOL_RESULT);
                            }

                            double elapsed = (now_ms() - t0) * 1000.0;

                            bool was_timeout = wd.timed_out;
                            watchdog_stop(&wd);

                            /* Build inline usage+cost suffix */
                            char spin_suffix[128] = "";
                            {
                                const model_info_t *mi = model_lookup(session.model);
                                if (mi) {
                                    double tc2 = sr.usage.input_tokens * mi->input_price / 1e6 +
                                                 sr.usage.output_tokens * mi->output_price / 1e6 +
                                                 sr.usage.cache_read_input_tokens *
                                                     mi->cache_read_price / 1e6 +
                                                 sr.usage.cache_creation_input_tokens *
                                                     mi->cache_write_price / 1e6;
                                    snprintf(spin_suffix, sizeof(spin_suffix),
                                             "[in:%d out:%d $%.4f]", sr.usage.input_tokens,
                                             sr.usage.output_tokens, tc2);
                                } else {
                                    snprintf(spin_suffix, sizeof(spin_suffix), "[in:%d out:%d]",
                                             sr.usage.input_tokens, sr.usage.output_tokens);
                                }
                            }

                            /* Stop spinner — shows completion line with inline metadata */
                            tui_async_spinner_stop(&spinner, ok && !was_timeout, tool_result,
                                                   elapsed, spin_suffix);

                            /* If watchdog set g_interrupted (not user), clear it */
                            if (was_timeout && g_tool_timed_out) {
                                g_interrupted = 0;
                                g_tool_timed_out = 0;
                                ok = false;
                                size_t cur = strlen(tool_result);
                                snprintf(tool_result + cur, MAX_TOOL_RESULT - cur,
                                         "\n[timeout: %s exceeded %ds]", blk->tool_name, timeout);
                            }

                            /* F8: Add to flame timeline */
                            tui_flame_add(&s_flame, blk->tool_name, t0 * 1000.0,
                                          (t0 + elapsed / 1000.0) * 1000.0, ok && !was_timeout, tt);
                            /* F10: Track tool dependency (sequential implies dependency) */
                            {
                                int node = tui_dag_add_node(&s_dag, blk->tool_name);
                                if (s_dag.node_count > 1 && node > 0)
                                    tui_dag_add_edge(&s_dag, node - 1, node);
                            }

                            /* Record metrics (under lock) */
                            pthread_mutex_lock(&g_locks.metrics_lock);
                            tool_metrics_record(&tool_metrics, blk->tool_name, ok, elapsed);
                            if (was_timeout) {
                                const tool_metric_t *m =
                                    tool_metrics_get(&tool_metrics, blk->tool_name);
                                if (m)
                                    ((tool_metric_t *)m)->timeouts++;
                            }
                            pthread_mutex_unlock(&g_locks.metrics_lock);
                            SI_RECORD_TOOL(blk->tool_name, ok && !was_timeout, elapsed,
                                           (int)(strlen(tool_result) / 4));

                            /* Cache result (under lock) — don't cache timeouts */
                            if (!was_timeout) {
                                pthread_mutex_lock(&g_locks.cache_lock);
                                tool_cache_put(&tool_cache, blk->tool_name, blk->tool_input,
                                               tool_result, ok, 60.0);
                                pthread_mutex_unlock(&g_locks.cache_lock);
                            }

                            /* Reset loop breaker on fresh (non-cached) execution */
                            if (ok)
                                consecutive_cached_fails = 0;

                            trace_span_end(tool_span,
                                           was_timeout ? "timeout" : (ok ? "ok" : "error"), NULL);
                        }
                        /* Track file reads for post-compact restoration */
                        if (ok && blk->tool_name &&
                            (strcmp(blk->tool_name, "read_file") == 0 ||
                             strcmp(blk->tool_name, "view_file") == 0)) {
                            /* Extract path from tool_input JSON */
                            const char *path_key = strstr(blk->tool_input, "\"path\"");
                            if (path_key) {
                                const char *q1 = strchr(path_key + 6, '"');
                                if (q1) {
                                    const char *q2 = strchr(q1 + 1, '"');
                                    if (q2 && (q2 - q1 - 1) < 500) {
                                        char fpath[512];
                                        int plen = (int)(q2 - q1 - 1);
                                        memcpy(fpath, q1 + 1, plen);
                                        fpath[plen] = '\0';
                                        post_compact_restore_track(&file_restore, fpath,
                                                                   tool_result);
                                    }
                                }
                            }
                        }

                        /* Spinner printed result for non-cached; cache-hit printed inline above */
                        conv_add_tool_result_named(&conv, blk->tool_id, blk->tool_name, tool_result,
                                                   !ok);

                        /* §9: Auto-embed pipeline — capture tool results into working memory */
                        if (ok && tool_result && strlen(tool_result) > 200 &&
                            g_embed_batch.count < EMBED_BATCH_CAP) {
                            agent_memory_ensure_init();
                            extract_bookend_summary(tool_result,
                                                    g_embed_batch.text[g_embed_batch.count],
                                                    sizeof(g_embed_batch.text[0]));
                            snprintf(g_embed_batch.key[g_embed_batch.count],
                                     sizeof(g_embed_batch.key[0]), "tool_%s_t%d",
                                     blk->tool_name ? blk->tool_name : "?", session.turn_count);
                            g_embed_batch.count++;
                            if (g_embed_batch.count >= EMBED_BATCH_CAP)
                                flush_embed_batch();
                        }

                        free(tool_result);
                    }
                }
            } else if (tool_count_this_turn > 1) {
                /* Multiple tools — concurrent read-only + serial write.
                   Phase 2: Claude Code methodology — read-only tools that declare
                   is_concurrent run in parallel via pthreads (up to 256). */
                needs_followup_turn = true;

                /* Collect tool names for batch spinner */
                const char *batch_names[TUI_BATCH_MAX];
                int batch_indices[TUI_BATCH_MAX];
                int batch_n = 0;
                for (int i = 0; i < sr.parsed.count && batch_n < TUI_BATCH_MAX; i++) {
                    content_block_t *blk = &sr.parsed.blocks[i];
                    if (blk->type && strcmp(blk->type, "tool_use") == 0) {
                        batch_names[batch_n] = blk->tool_name;
                        batch_indices[batch_n] = i;
                        batch_n++;
                    }
                }

                const char *tier = session_trust_tier_to_string(session.trust_tier);
                const char *batch_tiers[TUI_BATCH_MAX];
                bool batch_policy_blocked[TUI_BATCH_MAX];
                char batch_policy_reason[TUI_BATCH_MAX][256];
                for (int bi = 0; bi < batch_n; bi++) {
                    content_block_t *blk = &sr.parsed.blocks[batch_indices[bi]];
                    batch_tiers[bi] = tier;
                    batch_policy_blocked[bi] = false;
                    batch_policy_reason[bi][0] = '\0';

                    char trust_reason[256];
                    if (!tools_is_allowed_for_tier(blk->tool_name, tier, trust_reason,
                                                   sizeof(trust_reason))) {
                        const char *exec_tier = tier;
                        if (maybe_escalate_tool_permission(blk->tool_name, tier, trust_reason,
                                                           &exec_tier, batch_policy_reason[bi],
                                                           sizeof(batch_policy_reason[bi]))) {
                            batch_tiers[bi] = exec_tier;
                        } else {
                            batch_policy_blocked[bi] = true;
                        }
                    }
                }

                tui_batch_spinner_t batch_spinner;
                tui_batch_spinner_start(&batch_spinner, batch_names, batch_n);

                /* ── Partition: concurrent (read-only) vs serial (write) ── */
                concurrent_tool_slot_t *conc_slots =
                    safe_malloc((batch_n > 0 ? batch_n : 1) * sizeof(concurrent_tool_slot_t));
                int conc_count = 0;
                int serial_indices_arr[TUI_BATCH_MAX];
                int serial_count = 0;

                /* Pre-process: policy decision, validation, cache check, then route */
                for (int bi = 0; bi < batch_n; bi++) {
                    content_block_t *blk = &sr.parsed.blocks[batch_indices[bi]];

                    /* Populate args preview */
                    {
                        char bp[128];
                        extract_tool_preview(blk->tool_name, blk->tool_input, bp, sizeof(bp));
                        pthread_mutex_lock(&batch_spinner.mutex);
                        if (bi < batch_spinner.count)
                            snprintf(batch_spinner.entries[bi].args_preview,
                                     sizeof(batch_spinner.entries[bi].args_preview), "%s", bp);
                        pthread_mutex_unlock(&batch_spinner.mutex);
                    }

                    if (batch_policy_blocked[bi]) {
                        tui_batch_spinner_complete(&batch_spinner, bi, false,
                                                   batch_policy_reason[bi], 0.0);
                        baseline_log("security", "tool_blocked", batch_policy_reason[bi], NULL);
                        conv_add_tool_result_named(&conv, blk->tool_id, blk->tool_name,
                                                   batch_policy_reason[bi], true);
                        continue;
                    }

                    /* Validation */
                    char val_err[256];
                    if (!tools_validate_input(blk->tool_name, blk->tool_input, val_err,
                                              sizeof(val_err))) {
                        tui_batch_spinner_complete(&batch_spinner, bi, false, val_err, 0.0);
                        conv_add_tool_result_named(&conv, blk->tool_id, blk->tool_name, val_err,
                                                   true);
                        continue;
                    }

                    /* Cache check */
                    char *cached_result = safe_malloc(MAX_TOOL_RESULT);
                    cached_result[0] = '\0';
                    bool cached_ok = false;
                    pthread_mutex_lock(&g_locks.cache_lock);
                    bool cache_hit = tool_cache_get(&tool_cache, blk->tool_name, blk->tool_input,
                                                    cached_result, MAX_TOOL_RESULT, &cached_ok);
                    pthread_mutex_unlock(&g_locks.cache_lock);

                    if (cache_hit) {
                        dsco_strip_terminal_controls_inplace(cached_result);
                        if (!cached_ok) {
                            uint32_t h = 2166136261u;
                            for (const char *p = blk->tool_name; *p; p++)
                                h = (h ^ (uint8_t)*p) * 16777619u;
                            if (blk->tool_input)
                                for (const char *p = blk->tool_input; *p; p++)
                                    h = (h ^ (uint8_t)*p) * 16777619u;
                            if (h == last_fail_hash)
                                consecutive_cached_fails++;
                            else {
                                last_fail_hash = h;
                                consecutive_cached_fails = 1;
                            }
                            if (consecutive_cached_fails >= 2) {
                                size_t tl = strlen(cached_result);
                                snprintf(cached_result + tl, MAX_TOOL_RESULT - tl,
                                         "\n\n[STOP: identical call failed %d times. "
                                         "Try a different approach.]",
                                         consecutive_cached_fails);
                            }
                        } else {
                            consecutive_cached_fails = 0;
                        }
                        tui_batch_spinner_complete(&batch_spinner, bi, cached_ok, "cached", 0.0);
                        conv_add_tool_result_named(&conv, blk->tool_id, blk->tool_name,
                                                   cached_result, !cached_ok);
                        free(cached_result);
                        continue;
                    }
                    free(cached_result);

                    /* Route to concurrent or serial */
                    if (tool_is_concurrent_safe(blk->tool_name) &&
                        conc_count < CONCURRENT_TOOL_MAX) {
                        concurrent_tool_slot_t *s = &conc_slots[conc_count];
                        memset(s, 0, sizeof(*s));
                        s->tool_name = blk->tool_name;
                        s->tool_id = blk->tool_id;
                        s->tool_input = blk->tool_input;
                        s->tier = batch_tiers[bi];
                        s->block_index = batch_indices[bi];
                        s->batch_index = bi;
                        s->result = safe_malloc(MAX_TOOL_RESULT);
                        s->result[0] = '\0';
                        conc_count++;
                    } else {
                        serial_indices_arr[serial_count++] = bi;
                    }
                }

                /* ── Launch all concurrent read-only tools ── */
                double conc_t0 = now_ms();
                for (int ci = 0; ci < conc_count; ci++) {
                    pthread_create(&conc_slots[ci].thread, NULL, concurrent_tool_thread,
                                   &conc_slots[ci]);
                }

                /* ── Execute serial (write) tools while concurrent tools run ── */
                for (int si = 0; si < serial_count; si++) {
                    int bi = serial_indices_arr[si];
                    content_block_t *blk = &sr.parsed.blocks[batch_indices[bi]];
                    if (g_interrupted) {
                        conv_add_tool_result_named(&conv, blk->tool_id, blk->tool_name,
                                                   "tool execution interrupted by user", true);
                        tui_batch_spinner_complete(&batch_spinner, bi, false, "interrupted", 0.0);
                        continue;
                    }

                    char *tool_result = safe_malloc(MAX_TOOL_RESULT);
                    tool_result[0] = '\0';
                    bool ok = false;

                    char tool_span[37] = "";
                    trace_span_begin(trace_id, blk->tool_name, prompt_span, tool_span);

                    tool_watchdog_t wd;
                    int timeout = tool_timeout_for(blk->tool_name);
                    g_tool_timed_out = 0;
                    watchdog_start(&wd, pthread_self(), blk->tool_name, timeout);
                    tl_tool_cancelled = 0;

                    double t0 = now_ms();
                    ok = tools_execute_for_tier(blk->tool_name, blk->tool_input, batch_tiers[bi],
                                                tool_result, MAX_TOOL_RESULT);
                    dsco_strip_terminal_controls_inplace(tool_result);
                    double elapsed = (now_ms() - t0) * 1000.0;

                    size_t result_len2 = strlen(tool_result);
                    if (result_len2 >= MAX_TOOL_RESULT - 256) {
                        size_t cur = strlen(tool_result);
                        snprintf(tool_result + cur, MAX_TOOL_RESULT - cur,
                                 "\n[WARNING: output truncated at %zu bytes]", result_len2);
                    }

                    bool was_timeout = wd.timed_out;
                    watchdog_stop(&wd);

                    if (was_timeout && g_tool_timed_out) {
                        g_interrupted = 0;
                        g_tool_timed_out = 0;
                        ok = false;
                        size_t cur = strlen(tool_result);
                        snprintf(tool_result + cur, MAX_TOOL_RESULT - cur,
                                 "\n[timeout: %s exceeded %ds]", blk->tool_name, timeout);
                    }

                    tui_batch_spinner_complete(&batch_spinner, bi, ok && !was_timeout, tool_result,
                                               elapsed);

                    pthread_mutex_lock(&g_locks.metrics_lock);
                    tool_metrics_record(&tool_metrics, blk->tool_name, ok, elapsed);
                    if (was_timeout) {
                        const tool_metric_t *m = tool_metrics_get(&tool_metrics, blk->tool_name);
                        if (m)
                            ((tool_metric_t *)m)->timeouts++;
                    }
                    pthread_mutex_unlock(&g_locks.metrics_lock);
                    SI_RECORD_TOOL(blk->tool_name, ok && !was_timeout, elapsed,
                                   (int)(strlen(tool_result) / 4));

                    if (!was_timeout) {
                        pthread_mutex_lock(&g_locks.cache_lock);
                        tool_cache_put(&tool_cache, blk->tool_name, blk->tool_input, tool_result,
                                       ok, 60.0);
                        pthread_mutex_unlock(&g_locks.cache_lock);
                    }
                    trace_span_end(tool_span, was_timeout ? "timeout" : (ok ? "ok" : "error"),
                                   NULL);

                    /* Track file reads */
                    if (ok && blk->tool_name &&
                        (strcmp(blk->tool_name, "read_file") == 0 ||
                         strcmp(blk->tool_name, "view_file") == 0)) {
                        const char *path_key = strstr(blk->tool_input, "\"path\"");
                        if (path_key) {
                            const char *q1 = strchr(path_key + 6, '"');
                            if (q1) {
                                const char *q2 = strchr(q1 + 1, '"');
                                if (q2 && (q2 - q1 - 1) < 500) {
                                    char fpath[512];
                                    int plen = (int)(q2 - q1 - 1);
                                    memcpy(fpath, q1 + 1, plen);
                                    fpath[plen] = '\0';
                                    post_compact_restore_track(&file_restore, fpath, tool_result);
                                }
                            }
                        }
                    }

                    conv_add_tool_result_named(&conv, blk->tool_id, blk->tool_name, tool_result,
                                               !ok);
                    free(tool_result);
                }

                /* ── Join concurrent tools and collect results ── */
                for (int ci = 0; ci < conc_count; ci++) {
                    pthread_join(conc_slots[ci].thread, NULL);
                    concurrent_tool_slot_t *s = &conc_slots[ci];
                    content_block_t *blk = &sr.parsed.blocks[s->block_index];

                    tui_batch_spinner_complete(&batch_spinner, s->batch_index,
                                               s->ok && !s->was_timeout, s->result, s->elapsed_ms);

                    pthread_mutex_lock(&g_locks.metrics_lock);
                    tool_metrics_record(&tool_metrics, s->tool_name, s->ok, s->elapsed_ms);
                    if (s->was_timeout) {
                        const tool_metric_t *m = tool_metrics_get(&tool_metrics, s->tool_name);
                        if (m)
                            ((tool_metric_t *)m)->timeouts++;
                    }
                    pthread_mutex_unlock(&g_locks.metrics_lock);
                    /* Post-join aggregation loop — single-threaded, safe to record. */
                    SI_RECORD_TOOL(s->tool_name, s->ok && !s->was_timeout, s->elapsed_ms,
                                   (int)(strlen(s->result) / 4));

                    if (!s->was_timeout) {
                        pthread_mutex_lock(&g_locks.cache_lock);
                        tool_cache_put(&tool_cache, s->tool_name, s->tool_input, s->result, s->ok,
                                       60.0);
                        pthread_mutex_unlock(&g_locks.cache_lock);
                    }

                    /* Track file reads */
                    if (s->ok && s->tool_name &&
                        (strcmp(s->tool_name, "read_file") == 0 ||
                         strcmp(s->tool_name, "view_file") == 0)) {
                        const char *path_key = strstr(s->tool_input, "\"path\"");
                        if (path_key) {
                            const char *q1 = strchr(path_key + 6, '"');
                            if (q1) {
                                const char *q2 = strchr(q1 + 1, '"');
                                if (q2 && (q2 - q1 - 1) < 500) {
                                    char fpath[512];
                                    int plen = (int)(q2 - q1 - 1);
                                    memcpy(fpath, q1 + 1, plen);
                                    fpath[plen] = '\0';
                                    post_compact_restore_track(&file_restore, fpath, s->result);
                                }
                            }
                        }
                    }

                    conv_add_tool_result_named(&conv, blk->tool_id, blk->tool_name, s->result,
                                               !s->ok);

                    tui_flame_add(&s_flame, s->tool_name, conc_t0 * 1000.0,
                                  (conc_t0 + s->elapsed_ms / 1000.0) * 1000.0,
                                  s->ok && !s->was_timeout, tui_classify_tool(s->tool_name));

                    free(s->result);
                }

                if (conc_count > 0) {
                    fprintf(stderr, "  %s\xe2\x9a\xa1 %d tools ran concurrently (read-only)%s\n",
                            TUI_DIM, conc_count, TUI_RESET);
                }

                free(conc_slots);

                tui_batch_spinner_stop(&batch_spinner);

                /* Update co-occurrence matrix */
                {
                    const char *cooc_names[TUI_BATCH_MAX];
                    for (int ci = 0; ci < batch_n; ci++)
                        cooc_names[ci] = batch_names[ci];
                    if (batch_n >= 2)
                        tools_cooc_update(cooc_names, batch_n);
                    tools_cooc_inject_hints(cooc_names, batch_n);
                }

                /* Batch aggregate summary */
                if (batch_n >= 2) {
                    const model_info_t *mi = model_lookup(session.model);
                    char batch_cost_suffix[128] = "";
                    if (mi) {
                        double tc2 =
                            sr.usage.input_tokens * mi->input_price / 1e6 +
                            sr.usage.output_tokens * mi->output_price / 1e6 +
                            sr.usage.cache_read_input_tokens * mi->cache_read_price / 1e6 +
                            sr.usage.cache_creation_input_tokens * mi->cache_write_price / 1e6;
                        snprintf(batch_cost_suffix, sizeof(batch_cost_suffix),
                                 "[in:%d out:%d $%.4f]", sr.usage.input_tokens,
                                 sr.usage.output_tokens, tc2);
                    }
                    tui_batch_summary(&batch_spinner, batch_cost_suffix);
                }
            }

            /* Hard loop break: if 3+ consecutive identical cached failures,
               force end of turn to prevent burning budget on repeated errors */
            if (consecutive_cached_fails >= 3 && needs_followup_turn) {
                fprintf(stderr, "  %s⚠ breaking loop: %d consecutive identical cached failures%s\n",
                        TUI_BYELLOW, consecutive_cached_fails, TUI_RESET);
                needs_followup_turn = false;
            }

            /* NOTE: we deliberately do NOT compact the most-recent tool turn
             * here. Doing so rewrites the tool_result the model is about to
             * read for the first time down to a tiny preview, so the model
             * never sees tool output larger than the cap and spins re-reading
             * it (a 4.8 KB file read truncated to ~728 chars → endless retry
             * loop). Anthropic accepts a tool_use/tool_result continuation
             * with extended thinking enabled and no replayed thinking block,
             * so the flatten-to-text workaround is unnecessary. Genuine
             * context pressure is handled by conv_auto_compact (which trims
             * OLD results) and by ctx_persist_and_truncate (which caps huge
             * outputs at creation time and stashes the full text in the VFS). */

            /* F10: Render tool dependency graph (compact, 1 line) */
            tui_dag_render(&s_dag);

            /* F30: Enhanced section divider with success/fail/cache/context */
            {
                double turn_cost = session_cost(&session);
                double tps = sr.telemetry.tokens_per_sec;
                int ctx_used = session.total_input_tokens + session.total_output_tokens;
                int ctx_max =
                    session.context_window > 0 ? session.context_window : CONTEXT_WINDOW_TOKENS;
                double ctx_pct = ctx_max > 0 ? 100.0 * ctx_used / ctx_max : 0;

                /* Count successes/failures for this turn from flame data */
                int turn_ok = 0, turn_fail = 0, turn_cache = 0;
                for (int fi = 0; fi < s_flame.count; fi++) {
                    if (s_flame.entries[fi].ok)
                        turn_ok++;
                    else
                        turn_fail++;
                }
                /* If no flame data, assume all tools succeeded */
                if (s_flame.count == 0 && tool_count_this_turn > 0)
                    turn_ok = tool_count_this_turn;
                turn_cache = tool_cache.hits; /* session-level cache hits */

                /* Get git branch for divider */
                char div_branch[128] = "";
                if (g_features.branch_indicator) {
                    FILE *gbf = popen("git rev-parse --abbrev-ref HEAD 2>/dev/null", "r");
                    if (gbf) {
                        if (fgets(div_branch, sizeof(div_branch), gbf)) {
                            size_t bl = strlen(div_branch);
                            if (bl > 0 && div_branch[bl - 1] == '\n')
                                div_branch[bl - 1] = '\0';
                        }
                        pclose(gbf);
                    }
                }

                tui_section_divider_ex(session.turn_count, turn_ok, turn_fail, turn_cache,
                                       turn_cost, session.model, tps, ctx_pct, div_branch);
            }

            /* Check for tool-generated images */
            char img_tmp[256];
            snprintf(img_tmp, sizeof(img_tmp), "/tmp/dsco_img_%d.b64", getpid());
            FILE *img_f = fopen(img_tmp, "r");
            if (img_f) {
                char media_type[64] = "";
                if (fgets(media_type, sizeof(media_type), img_f)) {
                    size_t mt_len = strlen(media_type);
                    if (mt_len > 0 && media_type[mt_len - 1] == '\n')
                        media_type[mt_len - 1] = '\0';

                    fseek(img_f, 0, SEEK_END);
                    long b64_size = ftell(img_f) - (long)(strlen(media_type) + 1);
                    fseek(img_f, (long)(strlen(media_type) + 1), SEEK_SET);

                    if (b64_size > 0 && b64_size < 10 * 1024 * 1024) {
                        char *b64_data = safe_malloc((size_t)b64_size + 1);
                        size_t nr = fread(b64_data, 1, (size_t)b64_size, img_f);
                        b64_data[nr] = '\0';

                        conv_add_user_image_base64(&conv, media_type, b64_data,
                                                   "Analyze this image.");
                        needs_followup_turn = true;
                        free(b64_data);
                    }
                }
                fclose(img_f);
                unlink(img_tmp);
            }

            /* Check for tool-generated documents */
            char doc_tmp[256];
            snprintf(doc_tmp, sizeof(doc_tmp), "/tmp/dsco_doc_%d.b64", getpid());
            FILE *doc_f = fopen(doc_tmp, "r");
            if (doc_f) {
                char media_type[64] = "";
                if (fgets(media_type, sizeof(media_type), doc_f)) {
                    size_t mt_len = strlen(media_type);
                    if (mt_len > 0 && media_type[mt_len - 1] == '\n')
                        media_type[mt_len - 1] = '\0';

                    fseek(doc_f, 0, SEEK_END);
                    long b64_size = ftell(doc_f) - (long)(strlen(media_type) + 1);
                    fseek(doc_f, (long)(strlen(media_type) + 1), SEEK_SET);

                    if (b64_size > 0 && b64_size < 50 * 1024 * 1024) {
                        char *b64_data = safe_malloc((size_t)b64_size + 1);
                        size_t nr = fread(b64_data, 1, (size_t)b64_size, doc_f);
                        b64_data[nr] = '\0';

                        conv_add_user_document(&conv, media_type, b64_data, NULL,
                                               "Analyze this document.");
                        needs_followup_turn = true;
                        free(b64_data);
                    }
                }
                fclose(doc_f);
                unlink(doc_tmp);
            }

            /* Phase 4: Output token auto-escalation (Claude Code methodology).
               If model hit max_tokens, escalate to 64k and retry. Up to 3 recovery
               attempts with continuation messages after escalation. */
            if (sr.ok && sr.parsed.stop_reason &&
                strcmp(sr.parsed.stop_reason, "max_tokens") == 0 && !needs_followup_turn) {
                if (session.max_output_override == 0) {
                    /* First hit: escalate to 64k */
                    session.max_output_override = 64000;
                    session.max_output_recovery_count = 0;
                    fprintf(stderr, "  %s\xe2\x86\x91 max_tokens hit — escalating to 64k%s\n",
                            TUI_DIM, TUI_RESET);
                    /* Keep the truncated output, add continuation request */
                    conv_add_user_text(
                        &conv,
                        "[Your previous response was truncated at the output limit. "
                        "The limit has been raised. Please continue from where you left off.]");
                    needs_followup_turn = true;
                } else if (session.max_output_recovery_count < 3) {
                    session.max_output_recovery_count++;
                    fprintf(stderr, "  %s\xe2\x86\x91 output truncated again (recovery %d/3)%s\n",
                            TUI_DIM, session.max_output_recovery_count, TUI_RESET);
                    conv_add_user_text(
                        &conv,
                        "[Output was truncated again. Please continue from where you stopped.]");
                    needs_followup_turn = true;
                }
                /* After 3 recoveries: accept truncation and move on */
            }

            bool pause_turn =
                (sr.parsed.stop_reason && strcmp(sr.parsed.stop_reason, "pause_turn") == 0);
            if (pause_turn && !needs_followup_turn) {
                pause_turn_streak++;
            } else {
                pause_turn_streak = 0;
            }

            bool done = !needs_followup_turn && !pause_turn;
            if (pause_turn_streak >= 3) {
                tui_warning("provider returned pause_turn repeatedly; ending turn");
                done = true;
            }
            /* Agent invoked self_exit tool — finish this turn then terminate */
            if (g_agent_exit_requested)
                done = true;

            if (!g_agent_exit_requested && !g_interrupted) {
                loop_control_decision_t loop_decision;
                tools_loop_control_decide(turns, done, needs_followup_turn, &loop_decision);
                if (loop_decision.force_continue && loop_decision.prompt[0]) {
                    conv_add_user_text(&conv, loop_decision.prompt);
                    done = false;
                    pause_turn_streak = 0;
                    fprintf(stderr, "  %sloop construct: %s%s\n", TUI_DIM,
                            loop_decision.reason[0] ? loop_decision.reason : "continuing",
                            TUI_RESET);
                    baseline_log("agent", "loop_construct_continue", loop_decision.reason, NULL);
                }
                if (loop_decision.force_done) {
                    done = true;
                    baseline_log("agent", "loop_construct_done", loop_decision.reason, NULL);
                }
            }

            /* Phase 6: Classify turn transition for telemetry */
            turn_transition_t transition;
            if (g_agent_exit_requested)
                transition = TURN_STOP_EXIT_REQUESTED;
            else if (g_interrupted)
                transition = TURN_STOP_INTERRUPTED;
            else if (done)
                transition = TURN_STOP_DONE;
            else if (session.max_output_override > 0 && sr.parsed.stop_reason &&
                     strcmp(sr.parsed.stop_reason, "max_tokens") == 0)
                transition = (session.max_output_recovery_count == 0)
                                 ? TURN_CONTINUE_MAX_OUTPUT_ESCALATE
                                 : TURN_CONTINUE_MAX_OUTPUT_RECOVER;
            else
                transition = TURN_CONTINUE_TOOL_RESULTS;

            baseline_log("agent", "turn_transition", turn_transition_name(transition), NULL);

            /* F34: Notification bell when multi-turn response completes */
            if (done && turns > 1) {
                tui_notify("dsco", "response complete");
            }

            baseline_log_usage("turn", done ? "turn_done" : "turn_continue",
                               sr.parsed.stop_reason ? sr.parsed.stop_reason : "", NULL,
                               sr.usage.input_tokens, sr.usage.output_tokens,
                               sr.usage.cache_read_input_tokens,
                               sr.usage.cache_creation_input_tokens);

            /* IPC: heartbeat + inject pending messages */
            ipc_heartbeat();
            int ipc_flags = ipc_poll();
            if (ipc_flags & 1) {
                ipc_message_t msgs[8];
                int msg_count = ipc_recv(msgs, 8);
                if (msg_count > 0 && done) {
                    jbuf_t mb;
                    jbuf_init(&mb, 2048);
                    jbuf_append(&mb, "[IPC] Incoming messages from other agents:\n");
                    for (int mi = 0; mi < msg_count; mi++) {
                        char hdr[256];
                        snprintf(hdr, sizeof(hdr), "  From %s (topic: %s): ", msgs[mi].from_agent,
                                 msgs[mi].topic);
                        jbuf_append(&mb, hdr);
                        jbuf_append(&mb, msgs[mi].body ? msgs[mi].body : "");
                        jbuf_append(&mb, "\n");
                        free(msgs[mi].body);
                    }
                    if (mb.data) {
                        conv_add_user_text(&conv, mb.data);
                        done = false;
                    }
                    jbuf_free(&mb);
                } else {
                    for (int mi = 0; mi < msg_count; mi++)
                        free(msgs[mi].body);
                }
            }

            /* §9: End-of-turn memory maintenance */
            if (g_agent_memory_inited) {
                flush_embed_batch();          /* flush any pending embeddings */
                memory_tick(&g_agent_memory); /* decay + consolidate */
            }

            json_free_response(&sr.parsed);
            arena_reset(&turn_arena);
            if (done) {
                prompt_done = true;
                break;
            }
        }

        /* §9: Persist semantic memories at end of agent response */
        if (g_agent_memory_inited) {
            memory_persist_semantic(&g_agent_memory);
        }

        /* End prompt-level trace span */
        trace_span_end(prompt_span, g_interrupted ? "interrupted" : "ok", NULL);
        esc_poller_stop();
        arena_free(&turn_arena);
        terminal_input_echo_restore();

        if (g_interrupted && g_escape_state == ESC_PAUSED) {
            double elapsed = now_ms() - g_turn_start_time;
            /* Pass the ∞ sentinel: the agentic loop has no fixed turn cap, so
             * the menu renders "turn N/∞" rather than a misleading fraction. */
            bool resume = show_pause_menu(turns, 999999, elapsed);
            if (resume) {
                g_interrupted = 0;
                g_escape_state = ESC_RUNNING;
                /* Re-enter raw mode and restart the turns loop from where we paused */
                terminal_input_echo_suspend();
                esc_poller_start();
                goto resume_turn_loop;
            }
            /* User chose to cancel — fall through to normal post-loop handling */
            g_escape_state = ESC_RUNNING;
        } else if (g_interrupted) {
            fprintf(stderr, "\n");
            tui_warning("interrupted (press Ctrl+C again to force quit)");
        }
        /* Only the runaway backstop produces a "stopped short" warning now —
         * the loop otherwise ends because the model finished (prompt_done),
         * the user interrupted, or cost/context tripped (each with its own
         * message). Hitting the hard ceiling means a likely no-progress spin. */
        if (!prompt_done && !g_interrupted && turns >= hard_ceiling) {
            char msg[128];
            snprintf(msg, sizeof(msg),
                     "hard turn ceiling reached (%d) — stopping to prevent a "
                     "runaway loop; raise via DSCO_HARD_TURN_CEILING",
                     hard_ceiling);
            tui_warning(msg);
        }
        if (turns > 1) {
            double multi_turn_cost = session_cost(&session);
            fprintf(stderr, "%s  [%d turns | in:%d out:%d cache-read:%d | $%.4f]%s\n", TUI_DIM,
                    turns, total_input, total_output, total_cache_read, multi_turn_cost, TUI_RESET);
        }
        /* Use stderr for the blank line separator — stdout writes can
           desync with the scroll region and push the cursor into the
           bottom panel, causing input to appear in the middle. */
        fprintf(stderr, "\n");
        fflush(stderr);

        /* Agent self-exit: break outer REPL loop after delivering final message */
        if (g_agent_exit_requested)
            break;

        /* Periodic auto-save every 5 turns */
        if (session.turn_count % 5 == 0 && conv.count > 0) {
            autosave(&conv, &session);
        }
    }

    /* Disable bracketed paste mode */
    terminal_input_echo_restore();
    fprintf(stderr, "\033[?2004l");
    fflush(stderr);

    g_winch_sb = NULL;
    tui_status_bar_disable(&status_bar);

    g_autosave_conv = NULL;
    g_autosave_session = NULL;
    autosave(&conv, &session);
    conv_free(&conv);
#ifdef HAVE_READLINE
    {
        const char *home = getenv("HOME");
        if (home) {
            char hist_path[560];
            snprintf(hist_path, sizeof(hist_path), "%s/.dsco/history", home);
            write_history(hist_path);
        }
    }
#endif
    /* Wait for any in-flight background MCP init before tearing down the
     * registry — otherwise stop_server() races with the worker's still-open
     * HTTP/stdio handles. */
    mcp_bg_init_join();
    mcp_shutdown(&g_mcp);
    provider_free(g_provider);
    g_provider = NULL;
    free(aliases);
    tools_cooc_persist();
    tools_cooc_free();
    tool_map_free(&g_tool_map);
    tool_cache_free(&tool_cache);
    dsco_locks_destroy(&g_locks);
    post_compact_restore_free(&file_restore);

    /* Tear down output serialization & animation clock */
    tui_reset_title();
    if (g_anim_clock) {
        tui_anim_clock_destroy(g_anim_clock);
        g_anim_clock = NULL;
    }
    /* Flush final output then destroy queue */
    if (g_outq) {
        tui_outq_writef(g_outq, "%s  goodbye%s\n", TUI_DIM, TUI_RESET);
        tui_outq_flush_sync(g_outq);
        tui_outq_destroy(g_outq);
        g_outq = NULL;
    } else {
        fprintf(stderr, "%s  goodbye%s\n", TUI_DIM, TUI_RESET);
    }
}
