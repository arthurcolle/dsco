/* Native fleet-fanout test.
   Points HOME at a fake fleet, runs bridge/fanout, and verifies:
   - all matching hosts run
   - a role filter narrows the set
   - durable per-host RESULT.json envelopes are written to the run_dir */
#include "tools.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

extern bool tool_net_dispatch(const char *input, char *result, size_t rlen);

static int fails = 0;
#define CHECK(cond, msg)                                                                            \
    do {                                                                                            \
        if (!(cond)) {                                                                              \
            printf("FAIL: %s\n", msg);                                                             \
            fails++;                                                                               \
        } else {                                                                                    \
            printf("ok:   %s\n", msg);                                                             \
        }                                                                                          \
    } while (0)

static int file_exists(const char *p) {
    struct stat st;
    return stat(p, &st) == 0;
}

/* crude extract of a "key":"value" or "key":N from a JSON blob */
static void extract(const char *json, const char *key, char *out, size_t n) {
    out[0] = '\0';
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char *p = strstr(json, pat);
    if (!p)
        return;
    p += strlen(pat);
    if (*p == '"')
        p++;
    size_t i = 0;
    while (*p && *p != '"' && *p != ',' && *p != '}' && i + 1 < n)
        out[i++] = *p++;
    out[i] = '\0';
}

int main(void) {
    const char *home = getenv("FAKE_HOME");
    if (!home)
        home = "/tmp/fake_home";
    setenv("HOME", home, 1);

    char result[4096];

    /* 1) fanout across ALL hosts (web01, web02, db01). */
    bool ok = tool_net_dispatch("{\"action\":\"bridge/fanout\",\"cmd\":\"uptime\"}", result,
                                sizeof(result));
    CHECK(ok, "fanout dispatched");
    char hosts[32], run_dir[512];
    extract(result, "hosts", hosts, sizeof(hosts));
    extract(result, "run_dir", run_dir, sizeof(run_dir));
    CHECK(atoi(hosts) == 3, "all 3 fleet hosts fanned out");
    CHECK(run_dir[0] != '\0', "run_dir reported");

    /* durable envelopes exist */
    char p1[640];
    snprintf(p1, sizeof(p1), "%s/web01.json", run_dir);
    CHECK(file_exists(p1), "web01 RESULT.json envelope written");
    snprintf(p1, sizeof(p1), "%s/db01.json", run_dir);
    CHECK(file_exists(p1), "db01 RESULT.json envelope written");

    /* 2) role filter narrows to web hosts only. */
    ok = tool_net_dispatch("{\"action\":\"bridge/fanout\",\"cmd\":\"uptime\",\"role\":\"web\"}",
                           result, sizeof(result));
    extract(result, "hosts", hosts, sizeof(hosts));
    CHECK(ok && atoi(hosts) == 2, "role=web filtered to 2 hosts");

    /* 3) missing cmd is rejected. */
    ok = tool_net_dispatch("{\"action\":\"bridge/fanout\"}", result, sizeof(result));
    CHECK(!ok && strstr(result, "cmd required"), "missing cmd rejected");

    if (fails == 0)
        printf("\nALL NET-FANOUT TESTS PASSED\n");
    else
        printf("\n%d TEST(S) FAILED\n", fails);
    return fails == 0 ? 0 : 1;
}
