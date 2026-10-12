#!/usr/bin/env python3
"""Compile the real interactive routing block; no model or credential access."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'src/agent.c').read_text()
start = source.index('        /* Fail-closed request routing:')
end = source.index('        int turns = 0;', start)
block = source[start:end]
unit = r'''
#include <assert.h>
#include <stdbool.h>
#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "request_intent.h"
#include "tool_assurance.h"
/* This fixture exercises route selection; the later headless guard is tested separately. */
typedef struct { int unused; } headless_tool_guard_t;
static void headless_tool_guard_begin(headless_tool_guard_t *g, const char *prompt,
                                      bool disabled, bool required) {
    (void)g; (void)prompt; (void)disabled; (void)required;
}
static bool env_truthy(const char *s) { return s && !strcmp(s, "1"); }
static size_t dsco_env_size(const char *name, size_t fallback, size_t min, size_t max) {
    (void)name; (void)min; (void)max; return fallback;
}
static int hints;
enum { DSCO_GOAL_NONE, DSCO_GOAL_COMPLETE };
#define TUI_DIM ""
#define TUI_RESET ""
typedef struct { bool direct_answer_mode, goal; int goal_status;
                 char tool_choice[32], goal_objective[32]; } session_t;
static bool goal_is_active(session_t *s) { return s->goal; }
/* Goal auto-planning is independent concurrent work, not exercised here. */
static bool goal_should_auto_start(const char *s) { (void)s; return false; }
static bool goal_start(session_t *s, const char *p, bool active) {
    (void)s; (void)p; (void)active; return false;
}
static void baseline_log(const char *a, const char *b, const char *c, const char *d) {
    (void)a; (void)b; (void)c; (void)d;
}
static void tools_set_self_exit_allowed(bool allowed) { (void)allowed; }
static void tools_hint_add_user(const char *s) { assert(s && *s); hints++; }
''' + r'''
static void route(const char *input_buf, session_t *state) {
    session_t session = *state;
    bool goal_autorun_pending = false;
''' + block + r'''
    *state = session;
}
int main(void) {
    unsetenv("DSCO_DISABLE_DIRECT_ANSWER_GATE");
    const char *agentic_prompts[] = {
        "What is the weather in DC?",
        "use tools + what is the weather in DC",
        "What is my live Stripe balance?",
        "Why did the last command fail?",
        "How do our workers look right now?",
        "Compare today's prices with yesterday's",
        "Who is the current CEO?",
        "Explain this: https://example.invalid/report",
        "What are the errors in the attached log?",
        "ok", "proceed", "Inspect this file"
    };
    int failed = 0, count = 0;
    for (unsigned i = 0; i < sizeof(agentic_prompts)/sizeof(*agentic_prompts); i++) {
        session_t state = { .direct_answer_mode = true };
        hints = 0;
        route(agentic_prompts[i], &state);
        count++;
        if (state.direct_answer_mode || hints != 1) {
            fprintf(stderr, "FAIL: agentic route lost tools: %s\n", agentic_prompts[i]);
            failed++;
        }
    }
    const char *direct_prompts[] = {
        "What is a mutex?", "Explain the CAP theorem.",
        "Derive the quadratic formula.", "Reply with exactly READY."
    };
    for (unsigned i = 0; i < sizeof(direct_prompts)/sizeof(*direct_prompts); i++) {
        session_t state = {0};
        hints = 0;
        route(direct_prompts[i], &state);
        count++;
        if (!state.direct_answer_mode || hints != 0) {
            fprintf(stderr, "FAIL: direct route paid tool tax: %s\n", direct_prompts[i]);
            failed++;
        }
    }
    const char *choices[] = {"none", "auto", "any", "tool:read_file"};
    for (unsigned i = 0; i < sizeof(choices)/sizeof(*choices); i++) {
        session_t state = {0};
        snprintf(state.tool_choice, sizeof(state.tool_choice), "%s", choices[i]);
        route("What is the weather?", &state);
        assert(!strcmp(state.tool_choice, choices[i]));
        assert(!state.direct_answer_mode);
        count++;
    }
    session_t goal = {.goal = true};
    route("What is the weather?", &goal);
    assert(goal.goal && !goal.direct_answer_mode);
    count++;
    printf("interactive routing: %d/%d passed\n", count - failed, count);
    return failed ? 1 : 0;
}
'''
with tempfile.TemporaryDirectory(prefix='dsco-tool-routing-') as directory:
    path = Path(directory)
    (path / 'routing.c').write_text(unit)
    subprocess.run(shlex.split(os.environ.get('CC', 'cc')) + [
        '-std=c11', '-D_DARWIN_C_SOURCE', '-D_POSIX_C_SOURCE=200809L',
        '-I', str(ROOT / 'include'), str(ROOT / 'src/tool_assurance.c'),
        str(ROOT / 'src/request_intent.c'),
        str(path / 'routing.c'), '-o', str(path / 'routing')], check=True)
    subprocess.run([str(path / 'routing')], check=True)
