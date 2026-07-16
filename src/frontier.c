/* frontier.c — Pareto-frontier efficiency ledger (pure logic). */

#include "frontier.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

void frontier_init(frontier_ledger_t *l) {
    memset(l, 0, sizeof(*l));
    l->avg_fresh_input = -1.0; /* unseeded */
}

static double tok_usd(int tokens, double usd_per_m) {
    return (double)tokens * usd_per_m / 1e6;
}

frontier_decomp_t frontier_record(frontier_ledger_t *l, const frontier_turn_t *t,
                                  const frontier_prices_t *p) {
    frontier_decomp_t d;
    memset(&d, 0, sizeof(d));
    if (!l || !t || !p)
        return d;

    /* Total: trust provider-reported cost when present (caching discounts
     * included); else derive from tokens. */
    double derived = tok_usd(t->input_tokens, p->in_usd) +
                     tok_usd(t->output_tokens, p->out_usd) +
                     tok_usd(t->cache_read_tokens, p->cache_read_usd) +
                     tok_usd(t->cache_write_tokens, p->cache_write_usd);
    d.total = t->reported_cost_usd > 0 ? t->reported_cost_usd : derived;

    /* ── cache_waste ────────────────────────────────────────────────────
     * Steady-state expectation: this turn re-writes only the suffix that is
     * genuinely new — the previous assistant output plus this turn's fresh
     * input. Write tokens beyond that expectation are prefix that should
     * have been a 0.1x READ but billed as a 1.25x WRITE because the cache
     * missed (expired TTL, invalidated prefix, >20-block lookback gap).
     * Waste = excess × (write − read) price delta. */
    if (l->turns > 0) { /* turn 0's prefix write is a necessary cold start */
        int expected_write = l->prev_output_tokens + t->input_tokens;
        int excess_write = t->cache_write_tokens - expected_write;
        if (excess_write > 0 && p->cache_write_usd > p->cache_read_usd) {
            d.cache_waste =
                tok_usd(excess_write, p->cache_write_usd - p->cache_read_usd);
        }
    }

    /* ── retry_waste ────────────────────────────────────────────────────
     * A failed tool call forces a full extra round: the failure result rides
     * in history and the model burns another fresh-input pass to re-issue.
     * Attribute the failed fraction of this turn's fresh input + a share of
     * output to waste. */
    if (t->tool_calls > 0 && t->tool_failures > 0) {
        double fail_frac = (double)t->tool_failures / (double)t->tool_calls;
        d.retry_waste = fail_frac * (tok_usd(t->input_tokens, p->in_usd) +
                                     0.5 * tok_usd(t->output_tokens, p->out_usd));
    }

    /* ── redundancy_waste ───────────────────────────────────────────────
     * Duplicate tool calls re-bill their results in history with no new
     * information. Duplicate result tokens bill at the input rate every
     * subsequent turn; charge one turn's worth here. */
    if (t->duplicate_tool_calls > 0 && t->duplicate_result_tokens > 0) {
        d.redundancy_waste = tok_usd(t->duplicate_result_tokens, p->in_usd);
    }

    /* ── effort_waste ───────────────────────────────────────────────────
     * Reasoning tokens on a turn that neither called tools nor produced
     * non-trivial text: expensive thinking with no externalized result.
     * (Reasoning that leads to tool calls or real output is productive.) */
    if (t->reasoning_tokens > 0 && t->tool_calls == 0 && !t->produced_text) {
        d.effort_waste = tok_usd(t->reasoning_tokens, p->out_usd);
    }

    /* ── drag_waste ─────────────────────────────────────────────────────
     * Fresh (full-price) input above the session's running baseline means
     * history/schemas are being re-shipped uncached. Charge the delta above
     * 1.5x the EWMA baseline (headroom for genuinely bigger turns). */
    if (l->avg_fresh_input >= 0.0) {
        double ceiling = l->avg_fresh_input * 1.5;
        if ((double)t->input_tokens > ceiling && l->turns >= 3) {
            d.drag_waste = tok_usd((int)((double)t->input_tokens - ceiling), p->in_usd);
        }
    }

    d.waste = d.cache_waste + d.retry_waste + d.redundancy_waste + d.effort_waste +
              d.drag_waste;
    if (d.waste > d.total)
        d.waste = d.total;
    d.productive = d.total - d.waste;
    d.score = d.total > 0 ? d.productive / d.total : 1.0;

    /* ── Update ledger ──────────────────────────────────────────────────*/
    l->total_usd += d.total;
    l->productive_usd += d.productive;
    l->cache_waste_usd += d.cache_waste;
    l->retry_waste_usd += d.retry_waste;
    l->redundancy_waste_usd += d.redundancy_waste;
    l->effort_waste_usd += d.effort_waste;
    l->drag_waste_usd += d.drag_waste;
    l->turns++;

    l->win_cost[l->win_head] = d.total;
    l->win_out_tokens[l->win_head] = t->output_tokens;
    l->win_tool_calls[l->win_head] = t->tool_calls;
    l->win_tool_failures[l->win_head] = t->tool_failures;
    l->win_score[l->win_head] = d.score;
    l->win_head = (l->win_head + 1) % FRONTIER_WINDOW;
    if (l->win_fill < FRONTIER_WINDOW)
        l->win_fill++;

    if (l->avg_fresh_input < 0.0)
        l->avg_fresh_input = (double)t->input_tokens;
    else
        l->avg_fresh_input = 0.7 * l->avg_fresh_input + 0.3 * (double)t->input_tokens;
    l->prev_output_tokens = t->output_tokens;

    return d;
}

double frontier_waste_ratio(const frontier_ledger_t *l) {
    if (!l || l->total_usd <= 0)
        return 0.0;
    return 1.0 - l->productive_usd / l->total_usd;
}

frontier_verdict_t frontier_verdict(const frontier_ledger_t *l) {
    frontier_verdict_t v;
    memset(&v, 0, sizeof(v));
    v.score = 1.0;
    v.tool_success_rate = 1.0;
    v.on_frontier = true;
    v.dominant_waste = "none";

    if (!l || l->win_fill == 0) {
        snprintf(v.summary, sizeof(v.summary), "no turns recorded");
        return v;
    }

    int n = l->win_fill;
    /* Split the window into older/newer halves for the trend. */
    int half = n / 2;
    double cost_all = 0, out_all = 0, score_all = 0;
    double cost_old = 0, out_old = 0, cost_new = 0, out_new = 0;
    int calls = 0, fails = 0;
    for (int i = 0; i < n; i++) {
        /* i=0 is the OLDEST retained turn. */
        int idx = (l->win_head - n + i + 2 * FRONTIER_WINDOW) % FRONTIER_WINDOW;
        cost_all += l->win_cost[idx];
        out_all += (double)l->win_out_tokens[idx];
        score_all += l->win_score[idx];
        calls += l->win_tool_calls[idx];
        fails += l->win_tool_failures[idx];
        if (half > 0 && i < half) {
            cost_old += l->win_cost[idx];
            out_old += (double)l->win_out_tokens[idx];
        } else {
            cost_new += l->win_cost[idx];
            out_new += (double)l->win_out_tokens[idx];
        }
    }

    v.score = score_all / (double)n;
    v.marginal_usd_per_1k_out = out_all > 0 ? cost_all / (out_all / 1000.0) : 0.0;
    v.tool_success_rate = calls > 0 ? 1.0 - (double)fails / (double)calls : 1.0;

    if (half > 0 && out_old > 0 && out_new > 0) {
        double m_old = cost_old / (out_old / 1000.0);
        double m_new = cost_new / (out_new / 1000.0);
        if (m_old > 0)
            v.marginal_trend = (m_new - m_old) / m_old;
    }

    /* Dominant waste channel (cumulative — names the structural problem). */
    double mx = l->cache_waste_usd;
    v.dominant_waste = "cache-miss rewrites";
    v.dominant_waste_usd = l->cache_waste_usd;
    if (l->retry_waste_usd > mx) {
        mx = l->retry_waste_usd;
        v.dominant_waste = "failed-tool retries";
        v.dominant_waste_usd = l->retry_waste_usd;
    }
    if (l->redundancy_waste_usd > mx) {
        mx = l->redundancy_waste_usd;
        v.dominant_waste = "duplicate tool calls";
        v.dominant_waste_usd = l->redundancy_waste_usd;
    }
    if (l->effort_waste_usd > mx) {
        mx = l->effort_waste_usd;
        v.dominant_waste = "unproductive reasoning";
        v.dominant_waste_usd = l->effort_waste_usd;
    }
    if (l->drag_waste_usd > mx) {
        mx = l->drag_waste_usd;
        v.dominant_waste = "context drag";
        v.dominant_waste_usd = l->drag_waste_usd;
    }
    if (mx <= 0) {
        v.dominant_waste = "none";
        v.dominant_waste_usd = 0;
    }

    /* On the frontier: ≥80% of window spend productive AND marginal cost not
     * rising >25% half-over-half while tools degrade. */
    bool efficiency_ok = v.score >= 0.80;
    bool trend_ok = !(v.marginal_trend > 0.25 && v.tool_success_rate < 0.90);
    v.on_frontier = efficiency_ok && trend_ok;

    if (v.on_frontier) {
        snprintf(v.summary, sizeof(v.summary),
                 "ON frontier: %.0f%% of spend productive, $%.2f/1k out, "
                 "tools %.0f%% ok",
                 v.score * 100.0, v.marginal_usd_per_1k_out,
                 v.tool_success_rate * 100.0);
    } else if (!efficiency_ok) {
        snprintf(v.summary, sizeof(v.summary),
                 "OFF frontier: only %.0f%% of spend productive; dominant waste: "
                 "%s ($%.2f) — fix the waste channel, not the budget",
                 v.score * 100.0, v.dominant_waste, v.dominant_waste_usd);
    } else {
        snprintf(v.summary, sizeof(v.summary),
                 "LEAVING frontier: marginal cost +%.0f%% (now $%.2f/1k out) while "
                 "tools at %.0f%% — more spend is buying less; change strategy",
                 v.marginal_trend * 100.0, v.marginal_usd_per_1k_out,
                 v.tool_success_rate * 100.0);
    }
    return v;
}

const char *frontier_report(const frontier_ledger_t *l, char *buf, int buf_len) {
    if (!buf || buf_len <= 0)
        return "";
    buf[0] = '\0';
    if (!l || l->turns == 0) {
        snprintf(buf, buf_len, "  frontier: no turns recorded yet\n");
        return buf;
    }
    frontier_verdict_t v = frontier_verdict(l);
    double waste = l->total_usd - l->productive_usd;
    snprintf(buf, buf_len,
             "  %s\n"
             "  spend:      $%.4f over %d turns ($%.4f productive, $%.4f waste)\n"
             "  waste:      cache $%.4f · retries $%.4f · duplicates $%.4f · "
             "reasoning $%.4f · drag $%.4f\n"
             "  marginal:   $%.3f per 1k output tokens (trend %+.0f%%)\n"
             "  tools:      %.0f%% success in window\n",
             v.summary, l->total_usd, l->turns, l->productive_usd, waste,
             l->cache_waste_usd, l->retry_waste_usd, l->redundancy_waste_usd,
             l->effort_waste_usd, l->drag_waste_usd, v.marginal_usd_per_1k_out,
             v.marginal_trend * 100.0, v.tool_success_rate * 100.0);
    return buf;
}
