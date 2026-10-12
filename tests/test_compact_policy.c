/* test_compact_policy.c — unit tests for per-agent compaction thresholds. */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "compact_policy.h"

static int g_failures = 0;
#define CHECK(cond, msg) do {                                       \
        if (!(cond)) {                                              \
            fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
            g_failures++;                                           \
        }                                                           \
    } while (0)

int main(void) {
    CHECK(compact_rolling_trigger(1000000, 0) == 48000, "million-token window bounded early");
    CHECK(compact_rolling_trigger(100000, 0) == 48000, "medium window capped early");
    CHECK(compact_rolling_trigger(60000, 0) == 30000, "small window proportional");
    CHECK(compact_rolling_trigger(100000, 30000) == 40000, "burst predicts pressure");
    CHECK(compact_rolling_trigger(100000, 2147483647) == 25000, "burst cannot overflow");
    CHECK(compact_rolling_trigger(2147483647, 0) == 48000, "window cannot overflow");
    CHECK(compact_rolling_trigger(0, 0) == 0, "invalid budget disabled");
    CHECK(compact_rolling_trigger(1, 0) == 1, "tiny budget bounded");
    CHECK(compact_rolling_trigger(100000, -1) == 48000, "negative growth ignored");
    compact_policy_t p;
    /* Global defaults mirror llm.c: min 10000, max 40000. */
    int root_min = 10000, root_max = 40000;

    /* ROOT is identity: thresholds unchanged, full restore kept. */
    CHECK(compact_policy_for_role(COMPACT_ROLE_ROOT, root_min, root_max, &p), "root computes");
    CHECK(p.session_min_tokens == 10000 && p.session_max_tokens == 40000, "root is identity");
    CHECK(p.inherit_full_restore, "root keeps full-restore budget");
    CHECK(p.max_result_chars == 256, "root micro cap unchanged");

    /* SUBAGENT compacts at half the window, drops full restore. */
    CHECK(compact_policy_for_role(COMPACT_ROLE_SUBAGENT, root_min, root_max, &p), "subagent computes");
    CHECK(p.session_min_tokens == 5000 && p.session_max_tokens == 20000, "subagent = 50%");
    CHECK(!p.inherit_full_restore, "subagent drops full restore");
    CHECK(p.max_result_chars == 128, "subagent truncates results harder");

    /* EVAL compacts at 30%, most aggressive. */
    CHECK(compact_policy_for_role(COMPACT_ROLE_EVAL, root_min, root_max, &p), "eval computes");
    CHECK(p.session_min_tokens == 3000 && p.session_max_tokens == 12000, "eval = 30%");

    /* A role never loosens the root budget. */
    CHECK(compact_policy_for_role(COMPACT_ROLE_SUBAGENT, 4000, 8000, &p), "tiny root computes");
    CHECK(p.session_min_tokens >= 2000, "subagent floor respected");
    CHECK(p.session_max_tokens > p.session_min_tokens, "max > min always");

    /* Invalid inputs rejected. */
    CHECK(!compact_policy_for_role(COMPACT_ROLE_ROOT, 0, 40000, &p), "zero min rejected");
    CHECK(!compact_policy_for_role(COMPACT_ROLE_ROOT, 50000, 40000, &p), "min>=max rejected");
    CHECK(!compact_policy_for_role(COMPACT_ROLE_ROOT, root_min, root_max, NULL), "NULL out rejected");

    /* Role parsing. */
    compact_role_t r;
    CHECK(compact_role_from_str("subagent", &r) && r == COMPACT_ROLE_SUBAGENT, "parse subagent");
    CHECK(compact_role_from_str("worker", &r) && r == COMPACT_ROLE_SUBAGENT, "parse worker");
    CHECK(compact_role_from_str("eval", &r) && r == COMPACT_ROLE_EVAL, "parse eval");
    CHECK(compact_role_from_str("root", &r) && r == COMPACT_ROLE_ROOT, "parse root");
    CHECK(!compact_role_from_str("garbage", &r), "unknown role rejected (fail safe)");

    /* JSON round-trip. */
    compact_policy_for_role(COMPACT_ROLE_EVAL, root_min, root_max, &p);
    char buf[256];
    CHECK(compact_policy_json(&p, buf, sizeof(buf)) == buf, "json renders");
    CHECK(strstr(buf, "\"role\":\"eval\"") != NULL, "json has role");
    CHECK(strstr(buf, "\"session_min_tokens\":3000") != NULL, "json has min");

    if (g_failures) { fprintf(stderr, "compact_policy: %d FAILURE(S)\n", g_failures); return 1; }
    printf("compact_policy: all tests passed (root identity, subagent 50%%, eval 30%%, floors, parse)\n");
    return 0;
}
