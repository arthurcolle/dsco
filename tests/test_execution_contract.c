/* test_execution_contract.c — acceptance-gate classification contract.
 *
 * execution_contract_check() decides whether a headless run may release
 * completion. It must distinguish two failures that look alike in a string:
 *   - the acceptance command RAN and FAILED  -> bounded repair (RETRY, max 2)
 *   - the acceptance command was DENIED      -> stop (BLOCK), never repaired
 * Regression guarded here: the original classifier keyed off an "exit_code"
 * JSON field that the bash tool never emits, so every real failure was
 * misread as a denial and the bounded-repair path was dead code.
 */
#include "execution_contract.h"
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks;
#define CHECK(x) do { checks++; if (!(x)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

/* ── Harness state ─────────────────────────────────────────────────────── */
static const char *g_reply;          /* result the stubbed bash tool returns */
static bool        g_ok;             /* its success flag */
static int         g_exec_calls;     /* dispatches through the gate */
static char        g_last_tier[64];
static char        g_last_name[64];
static char        g_last_input[1024];
static int         g_assistant_turns, g_tool_results, g_user_prompts;
static bool        g_last_result_was_error;
static char        g_last_user_text[1024];

/* ── Stubs for the surrounding runtime ─────────────────────────────────── */
bool tools_execute_for_tier(const char *name, const char *input, const char *tier,
                            char *out, size_t len) {
    g_exec_calls++;
    snprintf(g_last_name, sizeof(g_last_name), "%s", name ? name : "");
    snprintf(g_last_tier, sizeof(g_last_tier), "%s", tier ? tier : "");
    snprintf(g_last_input, sizeof(g_last_input), "%s", input ? input : "");
    snprintf(out, len, "%s", g_reply ? g_reply : "");
    return g_ok;
}
void conv_add_assistant_raw(conversation_t *c, parsed_response_t *r) {
    (void)c; CHECK(r && r->count == 1 && r->blocks);
    CHECK(strcmp(r->blocks[0].type, "tool_use") == 0);
    CHECK(strcmp(r->blocks[0].tool_name, "bash") == 0);
    g_assistant_turns++;
}
void conv_add_tool_result(conversation_t *c, const char *id, const char *res, bool is_error) {
    (void)c; (void)id; (void)res; g_last_result_was_error = is_error; g_tool_results++;
}
void conv_add_user_text(conversation_t *c, const char *text) {
    (void)c; snprintf(g_last_user_text, sizeof(g_last_user_text), "%s", text ? text : "");
    g_user_prompts++;
}
bool baseline_log(const char *cat, const char *title, const char *detail, const char *meta) {
    (void)title; (void)detail; (void)meta;
    CHECK(cat && (strcmp(cat, "verification_pass") == 0 ||
                  strcmp(cat, "verification_fail") == 0));
    return true;
}

static void reset(void) {
    g_exec_calls = g_assistant_turns = g_tool_results = g_user_prompts = 0;
    g_last_user_text[0] = '\0';
}

int main(void) {
    conversation_t conv;            /* opaque to the gate; stubs ignore it */
    memset(&conv, 0, sizeof conv);
    char diag[512];

    /* 1. No acceptance command configured => nothing to verify, nothing runs. */
    {
        execution_contract_t st = {0}; reset();
        CHECK(execution_contract_check(&st, NULL, "trusted", &conv, diag, sizeof diag)
              == EXECUTION_CONTRACT_PASS);
        CHECK(execution_contract_check(&st, "", "trusted", &conv, diag, sizeof diag)
              == EXECUTION_CONTRACT_PASS);
        CHECK(g_exec_calls == 0);
        CHECK(diag[0] == '\0');
    }

    /* 2. Missing state/conversation must fail closed, never silently pass. */
    {
        execution_contract_t st = {0}; reset();
        CHECK(execution_contract_check(NULL, "make test", "trusted", &conv, diag, sizeof diag)
              == EXECUTION_CONTRACT_BLOCK);
        CHECK(execution_contract_check(&st, "make test", "trusted", NULL, diag, sizeof diag)
              == EXECUTION_CONTRACT_BLOCK);
        CHECK(g_exec_calls == 0);
    }

    /* 3. Passing command: dispatched through the gate at the caller's tier,
     *    recorded as a real tool exchange, released with no repair prompt. */
    {
        execution_contract_t st = {0}; reset();
        g_ok = true; g_reply = "ok\n";
        CHECK(execution_contract_check(&st, "make test", "untrusted-test-tier", &conv,
                                       diag, sizeof diag) == EXECUTION_CONTRACT_PASS);
        CHECK(g_exec_calls == 1);
        CHECK(strcmp(g_last_name, "bash") == 0);
        CHECK(strcmp(g_last_tier, "untrusted-test-tier") == 0);   /* no taint reset */
        CHECK(strstr(g_last_input, "\"command\":\"make test\"") != NULL);
        CHECK(strstr(g_last_input, "\"timeout\":") != NULL);
        CHECK(g_assistant_turns == 1 && g_tool_results == 1);
        CHECK(!g_last_result_was_error);
        CHECK(g_user_prompts == 0);
        CHECK(st.checks == 1 && st.repairs == 0);
    }

    /* 4. REGRESSION: a command that ran and failed is plain text with no
     *    exit_code field. It must trigger bounded repair, not a hard block. */
    {
        const char *failures[] = {
            "make: *** [test] Error 1\n",          /* captured stderr */
            "command exited with status 7",        /* tool_bash status line */
            "ok\n[artifact-verification-failed: missing report.md]",
            "",                                    /* silent nonzero exit */
        };
        for (size_t i = 0; i < sizeof failures / sizeof *failures; i++) {
            execution_contract_t st = {0}; reset();
            g_ok = false; g_reply = failures[i];
            CHECK(execution_contract_check(&st, "make test", "trusted", &conv,
                                           diag, sizeof diag) == EXECUTION_CONTRACT_RETRY);
            CHECK(st.repairs == 1);
            CHECK(g_last_result_was_error);
            CHECK(g_user_prompts == 1);
            CHECK(strstr(diag, "bounded repair 1/2") != NULL);
            /* The repair prompt must not invite weakening the test or the gate. */
            CHECK(strstr(g_last_user_text, "not complete") != NULL);
            CHECK(strstr(g_last_user_text, "without "
                         "changing or weakening acceptance tests") != NULL);
            CHECK(strstr(g_last_user_text, "Do not work around permission denials") != NULL);
        }
    }

    /* 5. Repair is bounded: RETRY, RETRY, then BLOCK — and the gate keeps
     *    rerunning the command rather than trusting the model's claim. */
    {
        execution_contract_t st = {0}; reset();
        g_ok = false; g_reply = "still failing";
        CHECK(execution_contract_check(&st, "make test", "trusted", &conv,
                                       diag, sizeof diag) == EXECUTION_CONTRACT_RETRY);
        CHECK(execution_contract_check(&st, "make test", "trusted", &conv,
                                       diag, sizeof diag) == EXECUTION_CONTRACT_RETRY);
        CHECK(strstr(diag, "bounded repair 2/2") != NULL);
        CHECK(execution_contract_check(&st, "make test", "trusted", &conv,
                                       diag, sizeof diag) == EXECUTION_CONTRACT_BLOCK);
        CHECK(strstr(diag, "still fails after bounded repair") != NULL);
        CHECK(st.repairs == 2);
        CHECK(st.checks == 3 && g_exec_calls == 3);   /* re-verified every time */
        CHECK(g_user_prompts == 2);                   /* no prompt after BLOCK */
    }

    /* 6. A governance denial is NOT an ordinary failure: block immediately,
     *    never spend a repair, never ask the model to route around it. */
    {
        const char *denials[] = {
            "{\"error\":\"governance_block\",\"tool\":\"bash\",\"stage\":\"capability\","
            "\"reason\":\"capability_denied\",\"gsu_remaining\":0.00}",
            "{\"error\":\"governance_block\",\"tool\":\"bash\",\"stage\":\"audit\","
            "\"reason\":\"event_capture_failed\",\"gsu_remaining\":0.00}",
            "{\"error\":\"governance_block\",\"tool\":\"bash\",\"stage\":\"tool_hook\","
            "\"reason\":\"before_hook_denied\",\"gsu_remaining\":0.00}",
        };
        for (size_t i = 0; i < sizeof denials / sizeof *denials; i++) {
            execution_contract_t st = {0}; reset();
            g_ok = false; g_reply = denials[i];
            CHECK(execution_contract_check(&st, "make test", "trusted", &conv,
                                           diag, sizeof diag) == EXECUTION_CONTRACT_BLOCK);
            CHECK(st.repairs == 0);
            CHECK(g_user_prompts == 0);
            CHECK(strstr(diag, "denied or could not execute") != NULL);
        }
    }

    /* 7. Denial detection must not be spoofable by command output. A failing
     *    command that merely PRINTS the envelope still ran, so it is repairable;
     *    only a genuine structured envelope (error + stage) is a denial. */
    {
        const char *lookalikes[] = {
            "echo: {\"error\":\"governance_block\"} was printed by the test",
            "{\"error\":\"governance_block\"}",              /* no stage field */
            "{\"error\":\"assertion_failed\",\"stage\":\"capability\"}",
        };
        for (size_t i = 0; i < sizeof lookalikes / sizeof *lookalikes; i++) {
            execution_contract_t st = {0}; reset();
            g_ok = false; g_reply = lookalikes[i];
            CHECK(execution_contract_check(&st, "make test", "trusted", &conv,
                                           diag, sizeof diag) == EXECUTION_CONTRACT_RETRY);
            CHECK(st.repairs == 1);
        }
    }

    /* 8. Command text is JSON-escaped into the tool payload, not concatenated. */
    {
        execution_contract_t st = {0}; reset();
        g_ok = true; g_reply = "ok";
        CHECK(execution_contract_check(&st, "printf \"a\\tb\" && ./x --flag=\"q\"",
                                       "trusted", &conv, diag, sizeof diag)
              == EXECUTION_CONTRACT_PASS);
        CHECK(strstr(g_last_input, "\\\"q\\\"") != NULL);
        CHECK(g_last_input[0] == '{');
    }

    /* 9. Timeout is operator-tunable and clamped by dsco_env_int bounds. */
    {
        execution_contract_t st = {0}; reset();
        g_ok = true; g_reply = "ok";
        setenv("DSCO_VERIFY_TIMEOUT", "5", 1);
        CHECK(execution_contract_check(&st, "make test", "trusted", &conv,
                                       diag, sizeof diag) == EXECUTION_CONTRACT_PASS);
        CHECK(strstr(g_last_input, "\"timeout\":5") != NULL);
        setenv("DSCO_VERIFY_TIMEOUT", "99999", 1);   /* above max => clamped */
        CHECK(execution_contract_check(&st, "make test", "trusted", &conv,
                                       diag, sizeof diag) == EXECUTION_CONTRACT_PASS);
        CHECK(strstr(g_last_input, "\"timeout\":600") != NULL);
        unsetenv("DSCO_VERIFY_TIMEOUT");
    }

    /* 10. A NULL diagnostic buffer must not crash the gate. */
    {
        execution_contract_t st = {0}; reset();
        g_ok = false; g_reply = "boom";
        CHECK(execution_contract_check(&st, "make test", "trusted", &conv, NULL, 0)
              == EXECUTION_CONTRACT_RETRY);
    }

    printf("test_execution_contract: %d checks passed\n", checks);
    return 0;
}
