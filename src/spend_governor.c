/* spend_governor.c — graduated cost/context control plane (pure logic). */

#include "spend_governor.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "config.h"

/* ── Effort ranking ───────────────────────────────────────────────────── */

int spend_effort_rank(const char *effort) {
    if (!effort || !effort[0] || strcmp(effort, EFFORT_AUTO) == 0)
        return 4; /* auto ≈ high */
    if (strcmp(effort, EFFORT_NONE) == 0)
        return 0;
    if (strcmp(effort, EFFORT_MINIMAL) == 0)
        return 1;
    if (strcmp(effort, EFFORT_LOW) == 0)
        return 2;
    if (strcmp(effort, EFFORT_MEDIUM) == 0)
        return 3;
    if (strcmp(effort, EFFORT_HIGH) == 0)
        return 4;
    if (strcmp(effort, EFFORT_XHIGH) == 0)
        return 5;
    if (strcmp(effort, EFFORT_MAX) == 0)
        return 6;
    return 4;
}

bool spend_effort_is_downshift(const char *current, const char *candidate) {
    return spend_effort_rank(candidate) < spend_effort_rank(current);
}

const char *spend_phase_label(spend_phase_t phase) {
    switch (phase) {
        case SPEND_GREEN:
            return "green";
        case SPEND_YELLOW:
            return "yellow";
        case SPEND_ORANGE:
            return "orange";
        case SPEND_RED:
            return "red";
        case SPEND_EXHAUSTED:
            return "exhausted";
    }
    return "?";
}

/* ── Default budgets ──────────────────────────────────────────────────── */

void spend_governor_default_budgets(bool had_explicit, double *session_usd,
                                    double *daily_usd) {
    if (session_usd)
        *session_usd = 0.0;
    if (daily_usd)
        *daily_usd = 0.0;
    if (had_explicit)
        return; /* user made a choice (including "off") — respect it */

    const char *off = getenv("DSCO_NO_DEFAULT_BUDGET");
    if (off && (off[0] == '1' || strcasecmp(off, "true") == 0))
        return;

    double s = 25.0;  /* a session should not silently cost more than this */
    double d = 100.0; /* a day should not silently cost more than this */
    const char *se = getenv("DSCO_DEFAULT_SESSION_BUDGET");
    if (se && se[0]) {
        double v = atof(se);
        if (v >= 0)
            s = v;
    }
    const char *de = getenv("DSCO_DEFAULT_DAILY_BUDGET");
    if (de && de[0]) {
        double v = atof(de);
        if (v >= 0)
            d = v;
    }
    if (session_usd)
        *session_usd = s;
    if (daily_usd)
        *daily_usd = d;
}

/* ── Plan ─────────────────────────────────────────────────────────────── */

static double ratio_or_zero(double spent, double budget) {
    if (budget <= 0.0)
        return 0.0;
    return spent / budget;
}

spend_plan_t spend_governor_plan(const spend_signals_t *sig) {
    spend_plan_t p;
    memset(&p, 0, sizeof(p));
    p.tool_budget_ratio = 1.0f;
    p.runway_turns = -1.0;
    p.trim_keep_recent = 8;
    p.trim_max_chars = 512;

    if (!sig) {
        p.phase = SPEND_GREEN;
        snprintf(p.reason, sizeof(p.reason), "no signals; full parameters");
        return p;
    }

    double sr = ratio_or_zero(sig->session_spent_usd, sig->session_budget_usd);
    double dr = ratio_or_zero(sig->daily_spent_usd, sig->daily_budget_usd);
    double pressure = sr > dr ? sr : dr;

    /* Context pressure joins on the same scale: nearly-full context forces
     * the same hygiene as nearly-spent budget (it IS spend — resent tokens). */
    double cr = 0.0;
    if (sig->context_window_tokens > 0 && sig->context_used_tokens > 0)
        cr = (double)sig->context_used_tokens / (double)sig->context_window_tokens;
    /* Context maps softer: 100% context ≈ ORANGE (compaction handles the
     * hard edge), so scale by 0.85. */
    double cpress = cr * 0.85;
    if (cpress > pressure)
        pressure = cpress;
    p.pressure = pressure;

    /* Runway: turns left at current burn against the tightest remaining $. */
    double turn_cost = sig->avg_turn_cost_usd > 0 ? sig->avg_turn_cost_usd
                                                  : sig->last_turn_cost_usd;
    if (turn_cost > 0) {
        double remaining = -1.0;
        if (sig->session_budget_usd > 0) {
            double r = sig->session_budget_usd - sig->session_spent_usd;
            remaining = r;
        }
        if (sig->daily_budget_usd > 0) {
            double r = sig->daily_budget_usd - sig->daily_spent_usd;
            if (remaining < 0 || r < remaining)
                remaining = r;
        }
        if (remaining >= 0)
            p.runway_turns = remaining / turn_cost;
    }

    /* Short runway escalates phase even at a low percentage: a $0.90/turn
     * burn with $3 left is RED regardless of the ratio. */
    spend_phase_t phase;
    if (pressure >= 1.0)
        phase = SPEND_EXHAUSTED;
    else if (pressure >= 0.90)
        phase = SPEND_RED;
    else if (pressure >= 0.75)
        phase = SPEND_ORANGE;
    else if (pressure >= 0.50)
        phase = SPEND_YELLOW;
    else
        phase = SPEND_GREEN;

    if (p.runway_turns >= 0 && phase < SPEND_RED) {
        if (p.runway_turns < 3.0)
            phase = SPEND_RED;
        else if (p.runway_turns < 8.0 && phase < SPEND_ORANGE)
            phase = SPEND_ORANGE;
        else if (p.runway_turns < 15.0 && phase < SPEND_YELLOW)
            phase = SPEND_YELLOW;
    }
    p.phase = phase;

    switch (phase) {
        case SPEND_GREEN:
            snprintf(p.reason, sizeof(p.reason),
                     "green: %.0f%% of budget; full parameters", pressure * 100.0);
            break;
        case SPEND_YELLOW:
            p.tool_budget_ratio = 0.75f;
            p.trim_old_results = true;
            p.trim_keep_recent = 8;
            p.trim_max_chars = 512;
            snprintf(p.reason, sizeof(p.reason),
                     "yellow: %.0f%% of budget; trimming stale tool results",
                     pressure * 100.0);
            break;
        case SPEND_ORANGE:
            p.tool_budget_ratio = 0.5f;
            snprintf(p.effort_ceiling, sizeof(p.effort_ceiling), "%s", EFFORT_MEDIUM);
            p.max_output_tokens = dsco_max_tokens() / 2;
            p.trim_old_results = true;
            p.trim_keep_recent = 6;
            p.trim_max_chars = 384;
            p.strip_binaries = true;
            p.suggest_model_downshift = true;
            if (p.runway_turns >= 0)
                snprintf(p.reason, sizeof(p.reason),
                         "orange: %.0f%% of budget (runway %.0f turns); effort≤medium, "
                         "output halved",
                         pressure * 100.0, p.runway_turns);
            else
                snprintf(p.reason, sizeof(p.reason),
                         "orange: %.0f%% of budget; effort≤medium, output halved",
                         pressure * 100.0);
            break;
        case SPEND_RED:
            p.tool_budget_ratio = 0.25f;
            snprintf(p.effort_ceiling, sizeof(p.effort_ceiling), "%s", EFFORT_LOW);
            p.max_output_tokens = dsco_max_tokens() / 4;
            p.trim_old_results = true;
            p.trim_keep_recent = 4;
            p.trim_max_chars = 256;
            p.strip_binaries = true;
            p.suggest_model_downshift = true;
            if (p.runway_turns >= 0)
                snprintf(p.reason, sizeof(p.reason),
                         "red: %.0f%% of budget (runway %.0f turns); effort≤low, "
                         "output quartered, downshift leaf work",
                         pressure * 100.0, p.runway_turns);
            else
                snprintf(p.reason, sizeof(p.reason),
                         "red: %.0f%% of budget; effort≤low, output quartered, "
                         "downshift leaf work",
                         pressure * 100.0);
            break;
        case SPEND_EXHAUSTED:
            p.block_turn = true;
            p.tool_budget_ratio = 0.0f;
            snprintf(p.reason, sizeof(p.reason),
                     "exhausted: %.0f%% of budget; blocking paid turns "
                     "(/budget <n> raises the cap)",
                     pressure * 100.0);
            break;
    }

    if (sig->quality_critical_work && phase >= SPEND_ORANGE &&
        phase < SPEND_EXHAUSTED) {
        p.preserve_quality = true;
        p.require_user_checkpoint = true;
        p.suggest_model_downshift = false;
        p.effort_ceiling[0] = '\0';
        p.max_output_tokens = 0;
        if (p.tool_budget_ratio < 0.75f)
            p.tool_budget_ratio = 0.75f;
        if (p.trim_keep_recent < 6)
            p.trim_keep_recent = 6;
        if (p.trim_max_chars < 384)
            p.trim_max_chars = 384;
        if (p.runway_turns >= 0)
            snprintf(p.reason, sizeof(p.reason),
                     "%s: %.0f%% of budget (runway %.0f turns); "
                     "quality-critical checkpoint required",
                     spend_phase_label(phase), pressure * 100.0, p.runway_turns);
        else
            snprintf(p.reason, sizeof(p.reason),
                     "%s: %.0f%% of budget; quality-critical checkpoint required",
                     spend_phase_label(phase), pressure * 100.0);
    }

    /* Floor the per-turn output cap so a graduated phase never breaks tool
     * calling outright. */
    if (p.max_output_tokens > 0 && p.max_output_tokens < 2048)
        p.max_output_tokens = 2048;

    /* Cache TTL economics: if the cadence between turns approaches/exceeds
     * the 5-minute TTL and the hit ratio is poor after enough turns to have
     * warmed up, the session is paying repeated cache WRITES with no reads.
     * The 1h TTL costs 2x on write and pays back on the first hit. */
    if (sig->turns >= 3 && sig->avg_turn_interval_sec > 240.0 &&
        sig->cache_telemetry_seen && sig->cache_hit_ratio < 0.30) {
        p.recommend_1h_cache = true;
    }

    return p;
}

void spend_plan_apply_learned(spend_plan_t *plan, const spend_learned_t *lw,
                              const spend_signals_t *sig) {
    if (!plan || !lw || !sig || plan->block_turn)
        return;

    /* Sessions that keep missing the 5m TTL push cache_aggressiveness up:
     * recommend the 1h TTL on a looser trigger than the phase default. */
    if (lw->cache_aggressiveness > 0.6 && sig->turns >= 3 && sig->cache_telemetry_seen &&
        sig->cache_hit_ratio < 0.50)
        plan->recommend_1h_cache = true;

    /* Learned cost sensitivity: suggest the leaf-work downshift one phase
     * earlier than the ORANGE default. */
    if (lw->model_cost_sensitivity > 0.7 && plan->phase >= SPEND_YELLOW &&
        !plan->preserve_quality)
        plan->suggest_model_downshift = true;

    /* Learned compaction point: start trimming when the context ratio
     * crosses it, even if budget pressure alone would not trim yet. */
    if (lw->context_compaction_thresh > 0.0 && lw->context_compaction_thresh < 1.0 &&
        sig->context_window_tokens > 0 && !plan->trim_old_results) {
        double ratio = (double)sig->context_used_tokens / (double)sig->context_window_tokens;
        if (ratio > lw->context_compaction_thresh) {
            plan->trim_old_results = true;
            plan->trim_keep_recent = 8; /* YELLOW-tier trim parameters */
            plan->trim_max_chars = 512;
        }
    }
}
