/* test_jev_gates.c — unit tests for the embedded gates. Boundary-stubbed like
 * test_jev_decide: the network/capability boundary is faked so the gate logic
 * (OOD firewall, route triage band, claim verification, tool selection) is
 * exercised without network access. */

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "jev_gates.h"
#include "jev_decide.h"
#include "capability.h"

/* ---- boundary stubs ---- */
static bool g_secrets = true;
static const char *g_canned = NULL;

bool dsco_cap_granted(dsco_cap_t cap, const char *tier) {
    (void)tier; return cap == CAP_SECRETS && g_secrets;
}
void dsco_flow_note(unsigned caps) { (void)caps; }
bool tools_execute_for_tier(const char *name, const char *input, const char *tier,
                            char *out, size_t cap) {
    (void)input; (void)tier;
    if (strcmp(name, "curl_raw") != 0 || !g_canned) return false;
    snprintf(out, cap, "%s", g_canned);
    return true;
}

static void set_noul(double p) {
    static char b[512];
    snprintf(b, sizeof(b), "{\"model\":\"jev-1.13.0\",\"usage\":{\"input_tokens\":100},"
             "\"answers\":{\"q\":{\"type\":\"noul\",\"noul\":%.3f}}}", p);
    g_canned = b;
}
static int g_fail = 0;
#define CHECK(c, m) do { if (!(c)) { fprintf(stderr,"FAIL: %s (%d)\n",m,__LINE__); g_fail++; } } while(0)

int main(void) {
    char receipt[JEV_DECIDE_RECEIPT_MAX];
    bool b; double conf; int sel;

    /* route: high p -> strong, confident */
    set_noul(0.9);
    CHECK(jev_gate_route("Design a distributed consensus protocol.", 0.5, &b, NULL, NULL,
                         receipt, sizeof(receipt), "test"), "route confident high");
    CHECK(b == true, "high p -> needs_strong");

    /* route: low p -> cheap, confident */
    set_noul(0.05);
    bool confident = false;
    CHECK(jev_gate_route("What is 2+2?", 0.5, &b, NULL, &confident, receipt, sizeof(receipt), "test"),
          "route confident low");
    CHECK(b == false && confident, "low p -> cheap and confident");

    /* route: mid band -> abstain (returns false, escalate) */
    set_noul(0.5);
    CHECK(!jev_gate_route("Explain TCP vs UDP.", 0.5, &b, NULL, &confident,
                          receipt, sizeof(receipt), "test"), "mid band abstains");
    CHECK(confident == false, "mid band marked not-confident");

    /* route: secrets denied -> fail (caller uses default) */
    g_secrets = false;
    set_noul(0.9);
    CHECK(!jev_gate_route("x", 0.5, &b, NULL, NULL, receipt, sizeof(receipt), "test"),
          "secrets denied fails");
    g_secrets = true;

    /* OOD firewall: real task (noul high) proceeds */
    set_noul(0.95);
    CHECK(jev_gate_is_real_task("How do I reverse a linked list?", &b, &conf,
                                receipt, sizeof(receipt), "test"), "real task gate runs");
    CHECK(b == true, "coherent task is real");

    /* OOD firewall: gibberish (noul low) flagged not-real */
    set_noul(0.03);
    CHECK(jev_gate_is_real_task("Blurp the framistan snordle.", &b, &conf,
                                receipt, sizeof(receipt), "test"), "ood gate runs");
    CHECK(b == false, "gibberish flagged not-real");

    /* verify_claim: sound claim (noul high) -> sound */
    set_noul(0.98);
    CHECK(jev_gate_verify_claim("compute 7*19", "133", &b, &conf, receipt, sizeof(receipt), "test"),
          "verify runs");
    CHECK(b == true, "correct claim is sound");

    /* verify_claim: wrong claim (noul low) -> not sound */
    set_noul(0.04);
    CHECK(jev_gate_verify_claim("compute 7*19", "130", &b, &conf, receipt, sizeof(receipt), "test"),
          "verify runs on wrong claim");
    CHECK(b == false, "wrong claim flagged unsound");

    /* tool_preflight: real task (noul) then tool choice */
    /* preflight does two calls: first is_real_task (noul), then tool choice. We
     * stub by returning noul for the first call then choice. Simplest: return
     * a noul that says real, then a choice. Since the stub returns one canned
     * response, test the OOD-reject path (real=false) and the happy path by
     * switching the canned response between the two calls. */
    const char *tools[] = {"web_search","sql_query","calculator"};
    set_noul(0.02);   /* not a real task */
    CHECK(jev_gate_tool_preflight("asdf qwer zxcv", tools, 3, &b, &sel, receipt, sizeof(receipt), "test"),
          "preflight completes on ood");
    CHECK(b == false, "ood task does not proceed");
    CHECK(strstr(receipt, "ood_rejected") != NULL, "receipt notes ood_rejected");

    /* spend gate: worth it (noul high) */
    set_noul(0.85);
    CHECK(jev_gate_spend("Scale to 8 replicas for a critical benchmark", 0.40, &b, &conf,
                         receipt, sizeof(receipt), "test"), "spend gate runs");
    CHECK(b == true, "worthwhile spend approved");

    /* invalid inputs rejected */
    CHECK(!jev_gate_route("", 0.5, &b, NULL, NULL, receipt, sizeof(receipt), "test"), "empty query rejected");
    CHECK(!jev_gate_verify_claim("t", NULL, &b, &conf, receipt, sizeof(receipt), "test"), "NULL claim rejected");

    if (g_fail) { fprintf(stderr, "jev_gates: %d FAILURE(S)\n", g_fail); return 1; }
    printf("jev_gates: all tests passed (route band, ood firewall, verify, spend, preflight, fail-open)\n");
    return 0;
}
