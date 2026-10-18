/* test_jev_decide.c — unit tests for the typed-decision substrate.
 * Stubs the network/capability boundary (same pattern as test_jev_router) so
 * the real request-validation, response-parsing, decision, and compaction
 * logic is exercised without network access. */

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jev_decide.h"
#include "capability.h"

/* ---- boundary stubs ---- */
static bool g_secrets_granted = true;
static const char *g_canned_response = NULL;   /* what the fake API returns */
static int g_transport_fail = 0;

bool dsco_cap_granted(dsco_cap_t cap, const char *tier) {
    (void)tier;
    if (cap != CAP_SECRETS) return false;
    return g_secrets_granted;
}
void dsco_flow_note(unsigned caps) { (void)caps; }

/* The module builds a curl arg string and calls the governed curl_raw path.
 * We intercept and return the canned API response. */
bool tools_execute_for_tier(const char *name, const char *input, const char *tier,
                            char *out, size_t cap) {
    (void)input; (void)tier;
    if (strcmp(name, "curl_raw") != 0) return false;
    if (g_transport_fail) return false;
    if (!g_canned_response) return false;
    snprintf(out, cap, "%s", g_canned_response);
    return true;
}

/* sha256_hex is provided by crypto.c in the link; no stub needed. */

static int g_failures = 0;
#define CHECK(cond, msg) do {                                       \
        if (!(cond)) {                                              \
            fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
            g_failures++;                                           \
        }                                                           \
    } while (0)

static void set_noul_response(double p) {
    static char buf[512];
    snprintf(buf, sizeof(buf),
        "{\"model\":\"jev-1.13.0\",\"usage\":{\"input_tokens\":100},"
        "\"answers\":{\"q\":{\"type\":\"noul\",\"noul\":%.3f}}}", p);
    g_canned_response = buf;
}
static void set_choice_response(const char *label, const char *opts[], size_t n) {
    static char buf[1024];
    char probs[512] = {0};
    for (size_t i = 0; i < n; i++) {
        char p[64];
        snprintf(p, sizeof(p), "%s\"%s\":%.3f", i ? "," : "", opts[i],
                 !strcmp(opts[i], label) ? 0.9 : 0.1 / (n > 1 ? n - 1 : 1));
        strncat(probs, p, sizeof(probs) - strlen(probs) - 1);
    }
    snprintf(buf, sizeof(buf),
        "{\"model\":\"jev-1.13.0\",\"usage\":{\"input_tokens\":100},"
        "\"answers\":{\"q\":{\"type\":\"choice\",\"choice\":\"%s\",\"confidence\":0.9,"
        "\"probabilities\":{%s}}}}", label, probs);
    g_canned_response = buf;
}
static void set_score_response(double score, double conf) {
    static char buf[512];
    snprintf(buf, sizeof(buf),
        "{\"model\":\"jev-1.13.0\",\"usage\":{\"input_tokens\":100},"
        "\"answers\":{\"q\":{\"type\":\"score\",\"score\":%.3f,\"confidence\":%.3f}}}",
        score, conf);
    g_canned_response = buf;
}

int main(void) {
    char receipt[JEV_DECIDE_RECEIPT_MAX];
    jev_decide_result_t result;

    /* --- validation (no network needed) --- */
    CHECK(!jev_decide(JEV_DECIDE_BOOL, "d", "state", "i", NULL, 0, NULL,
                      receipt, sizeof(receipt), NULL), "NULL result rejected");
    CHECK(!jev_decide(JEV_DECIDE_BOOL, "d", "", "i", NULL, 0, &result,
                      receipt, sizeof(receipt), NULL), "empty state rejected");
    const char *one[] = {"only"};
    CHECK(!jev_decide(JEV_DECIDE_CHOICE, "d", "s", "i", one, 1,
                      &result, receipt, sizeof(receipt), NULL), "1 choice option rejected");

    /* --- BOOL decision: p_yes parsed, threshold applied --- */
    set_noul_response(0.87);
    bool dec = false; double p = 0.0;
    CHECK(jev_decide_bool("routing", "Design a rate limiter.",
                          "Does this need a strong model?", 0.5, &dec, &p,
                          receipt, sizeof(receipt), "test"), "bool call succeeds");
    CHECK(dec == true, "p=0.87 >= 0.5 -> true");
    CHECK(p > 0.86 && p < 0.88, "p_yes parsed as 0.87");
    CHECK(strstr(receipt, "\"status\":\"ok\"") != NULL, "receipt ok");
    CHECK(strstr(receipt, "\"kind\":\"noul\"") != NULL, "receipt kind noul");

    /* BOOL below threshold -> false. */
    set_noul_response(0.12);
    CHECK(jev_decide_bool("routing", "What is 2+2?", "Strong model needed?",
                          0.5, &dec, &p, receipt, sizeof(receipt), "test"), "bool low-p succeeds");
    CHECK(dec == false, "p=0.12 < 0.5 -> false");

    /* --- CHOICE decision --- */
    const char *opts[] = {"cheap", "strong"};
    set_choice_response("strong", opts, 2);
    CHECK(jev_decide(JEV_DECIDE_CHOICE, "routing", "Prove a theorem.",
                     "Which lane?", opts, 2, &result, receipt, sizeof(receipt), "test"),
          "choice call succeeds");
    CHECK(strcmp(result.choice, "strong") == 0, "choice selects strong");
    CHECK(result.confidence > 0.8, "choice confidence parsed");

    /* --- SCORE decision --- */
    set_score_response(2.5, 0.8);
    const char *levels[] = {"0", "1", "2", "3"};
    CHECK(jev_decide(JEV_DECIDE_SCORE, "retrieval", "doc about HNSW",
                     "Relevance 0-3", levels, 4, &result, receipt, sizeof(receipt), "test"),
          "score call succeeds");
    CHECK(result.score > 2.4 && result.score < 2.6, "score value parsed");

    /* --- fail-closed paths --- */
    g_secrets_granted = false;
    CHECK(!jev_decide_bool("d", "s", "i", 0.5, &dec, &p, receipt, sizeof(receipt), "test"),
          "secrets denied fails closed");
    CHECK(strstr(receipt, "secrets_denied") != NULL, "receipt says secrets_denied");
    g_secrets_granted = true;

    g_transport_fail = 1;
    CHECK(!jev_decide_bool("d", "s", "i", 0.5, &dec, &p, receipt, sizeof(receipt), "test"),
          "transport failure fails closed");
    g_transport_fail = 0;

    /* malformed response -> invalid_response */
    g_canned_response = "{\"model\":\"jev-1.13.0\",\"answers\":{\"q\":{\"type\":\"noul\",\"noul\":5.0}}}";
    CHECK(!jev_decide_bool("d", "s", "i", 0.5, &dec, &p, receipt, sizeof(receipt), "test"),
          "out-of-range probability rejected");

    /* --- compaction --- */
    char summary[1024], ctx_key[80];
    const char *turns =
        "We decided to build the seat_take module. The take rate is 12.5%. "
        "The fleet has five cloud nodes. node-3 is saturated at load 13. "
        "We will revisit pricing next week. The weather is nice today.";
    /* trust the Noul stub: make everything keep-worthy (p=0.9) then verify
     * summary + context key + receipt are produced. */
    set_noul_response(0.9);
    CHECK(jev_compact_turns("compact", turns, 2, summary, sizeof(summary),
                            ctx_key, sizeof(ctx_key), receipt, sizeof(receipt), "test"),
          "compaction succeeds");
    CHECK(strncmp(ctx_key, "ctx:compact:L2:", 15) == 0, "context key has level prefix");
    CHECK(strlen(summary) > 0, "summary produced");
    CHECK(strstr(receipt, "dsco.jev_compact.v1") != NULL, "compact receipt schema");
    CHECK(strstr(receipt, "\"context_key\":") != NULL, "compact receipt has key");
    CHECK(strstr(receipt, "\"compression\":") != NULL, "compact receipt has ratio");

    /* compaction validation */
    CHECK(!jev_compact_turns("d", "", 2, summary, sizeof(summary), ctx_key, sizeof(ctx_key),
                             receipt, sizeof(receipt), "test"), "compact empty turns rejected");

    if (g_failures) {
        fprintf(stderr, "jev_decide: %d FAILURE(S)\n", g_failures);
        return 1;
    }
    printf("jev_decide: all tests passed (bool/choice/score parse, fail-closed, compaction)\n");
    return 0;
}
