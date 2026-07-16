/* executive.c — executive_decision: model-invocable session control. */

#include "executive.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "json_util.h"

static executive_hooks_t g_hooks;
static bool g_hooks_set = false;
static bool g_spending_paused = false;

void executive_set_hooks(const executive_hooks_t *hooks) {
    if (hooks) {
        g_hooks = *hooks;
        g_hooks_set = true;
    } else {
        memset(&g_hooks, 0, sizeof(g_hooks));
        g_hooks_set = false;
    }
    g_spending_paused = false;
}

bool executive_spending_paused(void) {
    return g_spending_paused;
}

static const char *k_names[EXEC_DECISION_COUNT] = {
    "end_conversation", "pause_spending",  "resume_spending", "downshift_model",
    "raise_budget",     "lower_budget",    "escalate_to_user",
};

const char *executive_decision_name(exec_decision_t d) {
    if (d < 0 || d >= EXEC_DECISION_COUNT)
        return "?";
    return k_names[d];
}

exec_decision_t executive_parse_decision(const char *name, bool *ok) {
    if (ok)
        *ok = false;
    if (!name)
        return EXEC_DECISION_COUNT;
    /* Accept both snake_case and human phrasing ("end conversation now"). */
    char norm[64];
    size_t n = 0;
    for (const char *p = name; *p && n < sizeof(norm) - 1; p++)
        norm[n++] = (*p == ' ' || *p == '-') ? '_' : (char)tolower((unsigned char)*p);
    norm[n] = '\0';
    for (int i = 0; i < EXEC_DECISION_COUNT; i++) {
        if (strstr(norm, k_names[i]) || strcmp(norm, k_names[i]) == 0) {
            if (ok)
                *ok = true;
            return (exec_decision_t)i;
        }
    }
    /* Common aliases. */
    if (strstr(norm, "end_conversation") || strstr(norm, "stop_session") ||
        strstr(norm, "end_session") || strstr(norm, "terminate")) {
        if (ok)
            *ok = true;
        return EXEC_END_CONVERSATION;
    }
    if (strstr(norm, "ask_user") || strstr(norm, "escalate")) {
        if (ok)
            *ok = true;
        return EXEC_ESCALATE_TO_USER;
    }
    return EXEC_DECISION_COUNT;
}

static void audit(const char *title, const char *detail) {
    if (g_hooks.audit)
        g_hooks.audit("executive", title, detail);
}

static double budget_hard_ceiling(void) {
    const char *e = getenv("DSCO_EXEC_BUDGET_CEILING");
    if (e && e[0]) {
        double v = atof(e);
        if (v > 0)
            return v;
    }
    return EXEC_BUDGET_HARD_CEILING_DEFAULT;
}

static void reject(char *result, size_t rlen, const char *decision, const char *why) {
    snprintf(result, rlen,
             "{\"status\":\"rejected\",\"decision\":\"%s\",\"reason\":\"%s\"}", decision,
             why);
}

bool executive_decide(const char *input_json, char *result, size_t rlen) {
    if (!result || rlen == 0)
        return false;
    result[0] = '\0';

    char *decision_s = input_json ? json_get_str(input_json, "decision") : NULL;
    char *reason = input_json ? json_get_str(input_json, "reason") : NULL;
    double amount = input_json ? json_get_double(input_json, "amount_usd", 0.0) : 0.0;

    bool ok = false;
    exec_decision_t d = executive_parse_decision(decision_s, &ok);
    if (!ok) {
        snprintf(result, rlen,
                 "{\"status\":\"rejected\",\"error\":\"unknown decision %s\","
                 "\"valid\":[\"end_conversation\",\"pause_spending\",\"resume_spending\","
                 "\"downshift_model\",\"raise_budget\",\"lower_budget\","
                 "\"escalate_to_user\"]}",
                 decision_s ? decision_s : "(missing)");
        free(decision_s);
        free(reason);
        return false;
    }

    /* Every decision requires a reason — executive power demands evidence. */
    if (!reason || strlen(reason) < 8) {
        reject(result, rlen, k_names[d],
               "a substantive reason is required (what evidence drives this decision?)");
        free(decision_s);
        free(reason);
        return false;
    }

    bool accepted = false;
    char detail[512];
    snprintf(detail, sizeof(detail), "reason=%s", reason);

    switch (d) {
        case EXEC_END_CONVERSATION: {
            if (!g_hooks.request_exit) {
                reject(result, rlen, k_names[d], "host provides no exit hook");
                break;
            }
            g_hooks.request_exit(reason);
            snprintf(result, rlen,
                     "{\"status\":\"accepted\",\"decision\":\"end_conversation\","
                     "\"effect\":\"agent loop exits after this turn\",\"reason\":");
            size_t cur = strlen(result);
            jbuf_t jb;
            jbuf_init(&jb, 256);
            jbuf_append_json_str(&jb, reason);
            snprintf(result + cur, rlen - cur, "%s}", jb.data ? jb.data : "\"\"");
            jbuf_free(&jb);
            accepted = true;
            break;
        }
        case EXEC_PAUSE_SPENDING: {
            if (!g_hooks.force_phase_red) {
                reject(result, rlen, k_names[d], "host provides no governor hook");
                break;
            }
            g_spending_paused = true;
            g_hooks.force_phase_red(true);
            snprintf(result, rlen,
                     "{\"status\":\"accepted\",\"decision\":\"pause_spending\","
                     "\"effect\":\"governor forced to RED: effort low, output quartered, "
                     "aggressive context hygiene\"}");
            accepted = true;
            break;
        }
        case EXEC_RESUME_SPENDING: {
            if (!g_hooks.force_phase_red) {
                reject(result, rlen, k_names[d], "host provides no governor hook");
                break;
            }
            g_spending_paused = false;
            g_hooks.force_phase_red(false);
            snprintf(result, rlen,
                     "{\"status\":\"accepted\",\"decision\":\"resume_spending\","
                     "\"effect\":\"governor restored to signal-driven phasing\"}");
            accepted = true;
            break;
        }
        case EXEC_DOWNSHIFT_MODEL: {
            if (!g_hooks.request_model_downshift) {
                reject(result, rlen, k_names[d], "host provides no routing hook");
                break;
            }
            g_hooks.request_model_downshift(reason);
            snprintf(result, rlen,
                     "{\"status\":\"accepted\",\"decision\":\"downshift_model\","
                     "\"effect\":\"router advised to prefer a cheaper model for "
                     "subsequent leaf work\"}");
            accepted = true;
            break;
        }
        case EXEC_RAISE_BUDGET: {
            if (!g_hooks.get_session_budget || !g_hooks.set_session_budget) {
                reject(result, rlen, k_names[d], "host provides no budget hooks");
                break;
            }
            if (amount <= 0) {
                reject(result, rlen, k_names[d],
                       "amount_usd required: the new session budget in dollars");
                break;
            }
            double cur_budget = g_hooks.get_session_budget();
            double ceiling = budget_hard_ceiling();
            /* Bound 1: ≤ 2x current (when a budget exists). */
            if (cur_budget > 0 && amount > cur_budget * EXEC_RAISE_FACTOR_MAX) {
                char why[192];
                snprintf(why, sizeof(why),
                         "raise bounded to %.0fx current budget ($%.2f -> max $%.2f); "
                         "ask the operator for more",
                         EXEC_RAISE_FACTOR_MAX, cur_budget,
                         cur_budget * EXEC_RAISE_FACTOR_MAX);
                reject(result, rlen, k_names[d], why);
                break;
            }
            /* Bound 2: hard ceiling. */
            if (amount > ceiling) {
                char why[160];
                snprintf(why, sizeof(why),
                         "amount $%.2f exceeds the hard ceiling $%.2f "
                         "(DSCO_EXEC_BUDGET_CEILING)",
                         amount, ceiling);
                reject(result, rlen, k_names[d], why);
                break;
            }
            /* Bound 3: efficiency evidence. Raising the budget while OFF the
             * frontier subsidizes waste — reject with the decomposition. */
            if (g_hooks.frontier_snapshot) {
                double score = 1.0;
                bool on = true;
                char fsum[224] = "";
                if (g_hooks.frontier_snapshot(&score, &on, fsum, sizeof(fsum)) && !on) {
                    char why[400];
                    snprintf(why, sizeof(why),
                             "off the efficiency frontier (%.0f%% of recent spend "
                             "productive): %s — fix the waste channel before buying "
                             "more budget",
                             score * 100.0, fsum);
                    reject(result, rlen, k_names[d], why);
                    break;
                }
            }
            g_hooks.set_session_budget(amount);
            snprintf(result, rlen,
                     "{\"status\":\"accepted\",\"decision\":\"raise_budget\","
                     "\"budget_usd\":%.2f,\"previous_usd\":%.2f}",
                     amount, cur_budget);
            snprintf(detail, sizeof(detail), "%.2f -> %.2f; reason=%s", cur_budget,
                     amount, reason);
            accepted = true;
            break;
        }
        case EXEC_LOWER_BUDGET: {
            if (!g_hooks.get_session_budget || !g_hooks.set_session_budget) {
                reject(result, rlen, k_names[d], "host provides no budget hooks");
                break;
            }
            if (amount <= 0) {
                reject(result, rlen, k_names[d], "amount_usd required");
                break;
            }
            double cur_budget = g_hooks.get_session_budget();
            double spent = g_hooks.get_session_spent ? g_hooks.get_session_spent() : 0.0;
            if (amount < spent) {
                /* Lowering below spend = immediate exhaustion; make that an
                 * explicit end_conversation instead of a footgun. */
                char why[192];
                snprintf(why, sizeof(why),
                         "amount $%.2f is below already-spent $%.2f; use "
                         "end_conversation if the intent is to stop",
                         amount, spent);
                reject(result, rlen, k_names[d], why);
                break;
            }
            g_hooks.set_session_budget(amount);
            snprintf(result, rlen,
                     "{\"status\":\"accepted\",\"decision\":\"lower_budget\","
                     "\"budget_usd\":%.2f,\"previous_usd\":%.2f}",
                     amount, cur_budget);
            snprintf(detail, sizeof(detail), "%.2f -> %.2f; reason=%s", cur_budget,
                     amount, reason);
            accepted = true;
            break;
        }
        case EXEC_ESCALATE_TO_USER: {
            if (!g_hooks.escalate) {
                reject(result, rlen, k_names[d], "host provides no escalation hook");
                break;
            }
            g_hooks.escalate(reason);
            snprintf(result, rlen,
                     "{\"status\":\"accepted\",\"decision\":\"escalate_to_user\","
                     "\"effect\":\"question surfaced to operator; hold further "
                     "spending-heavy work until answered\"}");
            accepted = true;
            break;
        }
        default:
            reject(result, rlen, "?", "unreachable");
            break;
    }

    audit(accepted ? executive_decision_name(d) : "rejected", detail);
    free(decision_s);
    free(reason);
    return accepted;
}

bool tool_executive_decision(const char *input, char *result, size_t rlen) {
    return executive_decide(input, result, rlen);
}
