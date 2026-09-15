/* Offline regression: compile the real tools.c handler with a recording spawn.
 * The existing swarm instance-policy tests exercise the actual fork/env path. */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "swarm.h"
#include "json_util.h"

#define TUI_BCYAN ""
#define TUI_RESET ""
#define TUI_BOLD ""
#define TUI_BGREEN ""
#define TUI_DIM ""
static swarm_t g_swarm;
static int next_turns, next_tokens, observed_turns, observed_tokens, spawns;
static double next_budget, observed_budget;
static bool credential_ok = true;
static char next_effort[32], observed_effort[32];
static void ensure_swarm(void) {}
static int current_swarm_depth(void) { return 0; }
bool provider_has_usable_key(const char *provider, const char *key) {
    (void)provider; (void)key; return credential_ok;
}
double swarm_budget_remaining(swarm_t *s) { return s->swarm_budget_usd; }
double swarm_estimate_task_cost(swarm_t *s, const char *model) {
    (void)s; (void)model; return .001;
}
void swarm_set_next_instance(const char *effort, double temperature, double top_p,
                            int top_k, int thinking_budget, const char *tool_choice,
                            const char *system_prompt, int max_agent_turns) {
    (void)temperature; (void)top_p; (void)top_k; (void)thinking_budget;
    (void)tool_choice; (void)system_prompt;
    next_turns = max_agent_turns; next_tokens = 0; next_budget = 0;
    snprintf(next_effort, sizeof(next_effort), "%s", effort ? effort : "");
}
void swarm_set_next_budget_usd(double value) { next_budget = value; }
void swarm_set_next_max_tokens(int value) { next_tokens = value; }
int swarm_spawn_provider(swarm_t *s, int group, const char *task,
                         const char *model, const char *provider) {
    (void)group; (void)task; (void)provider;
    observed_budget = next_budget; observed_turns = next_turns;
    observed_tokens = next_tokens;
    snprintf(observed_effort, sizeof(observed_effort), "%s", next_effort);
    next_budget = 0; next_turns = 0; next_tokens = 0; next_effort[0] = 0;
    s->children[0] = (swarm_child_t){.id = 0, .pid = 1};
    snprintf(s->children[0].model, sizeof(s->children[0].model), "%s", model ? model : "default");
    spawns++; return 0;
}
swarm_child_t *swarm_get(swarm_t *s, int id) { return &s->children[id]; }
static void swarm_emit_child_event(const char *event, const swarm_child_t *child,
                                  const char *source, const char *extra) {
    (void)event; (void)child; (void)source; (void)extra;
}
#include "spawn_provider_under_test.h"

int main(void) {
    char out[2048];
    assert(tool_spawn_provider("{\"task\":\"offline\",\"provider\":\"cerebras\",\"model\":\"exact-id\",\"budget\":0.1,\"max_worker_turns\":2,\"max_tokens\":650,\"effort\":\"low\"}", out, sizeof(out)));
    assert(fabs(observed_budget - .1) < 1e-12);
    assert(observed_turns == 2 && observed_tokens == 650);
    assert(!strcmp(observed_effort, "low"));
    assert(fabs(g_swarm.children[0].budget_usd - .1) < 1e-12);
    assert(strstr(out, "agent action=status"));
    assert(tool_spawn_provider("{\"task\":\"offline\",\"provider\":\"cerebras\"}", out, sizeof(out)));
    assert(observed_budget == 0 && observed_turns == -1 && observed_tokens == 0);
    assert(!*observed_effort && g_swarm.children[0].budget_usd == 0);
    int count = spawns;
    credential_ok = false;
    assert(!tool_spawn_provider("{\"task\":\"offline\",\"provider\":\"missing\",\"budget\":0.2}", out, sizeof(out)));
    assert(spawns == count);
    credential_ok = true;
    g_swarm.swarm_budget_usd = .0001;
    assert(!tool_spawn_provider("{\"task\":\"offline\",\"provider\":\"cerebras\",\"budget\":0.2}", out, sizeof(out)));
    assert(spawns == count);
    puts("PASS: spawn_provider installs budget/turn/token/effort limits before spawn; defaults do not leak; admission failures do not spawn");
}
