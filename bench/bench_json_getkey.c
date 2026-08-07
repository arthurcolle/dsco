/* bench_json_getkey.c — measures the json_get_* scan-to-key hot path.
 *
 * The agent loop calls json_get_str/int/bool repeatedly to pull many fields out
 * of a single response/tool-call object. Each call re-scans from the top and —
 * before the in-place-compare optimization — malloc'd a decode buffer for every
 * key it skipped. This bench reproduces that access pattern.
 *
 * Build: cc -O3 -Iinclude bench/bench_json_getkey.c src/json_util.c -o /tmp/bjk
 */
#include "json_util.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>

static double now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

/* Representative of an Anthropic tool_use input + a usage/meta object: many
 * simple string/number keys, values the reader pulls out one at a time. */
static const char *TOOL_CALL =
    "{\"model\":\"claude-opus-4-8\",\"role\":\"assistant\",\"stop_reason\":\"tool_use\","
    "\"command\":\"grep\",\"pattern\":\"find_key\",\"path\":\"src/json_util.c\","
    "\"limit\":250,\"offset\":0,\"case_sensitive\":true,\"multiline\":false,"
    "\"context\":3,\"max_results\":100,\"timeout_ms\":30000,\"cwd\":\"/Users/x/dsco\","
    "\"input_tokens\":18234,\"output_tokens\":642,\"cache_read\":17000,\"cache_write\":900}";

int main(void) {
    const long ITERS = 2000000;

    /* Warm + correctness sanity. */
    char *m = json_get_str(TOOL_CALL, "model");
    char *p = json_get_str(TOOL_CALL, "path");
    int lim = json_get_int(TOOL_CALL, "limit", -1);
    int in = json_get_int(TOOL_CALL, "input_tokens", -1);
    bool cs = json_get_bool(TOOL_CALL, "case_sensitive", false);
    if (!m || strcmp(m, "claude-opus-4-8") || !p || strcmp(p, "src/json_util.c") ||
        lim != 250 || in != 18234 || cs != true) {
        fprintf(stderr, "CORRECTNESS FAIL: model=%s path=%s limit=%d in=%d cs=%d\n",
                m ? m : "(null)", p ? p : "(null)", lim, in, cs);
        return 1;
    }
    free(m);
    free(p);

    volatile long sink = 0;
    double t0 = now_ns();
    for (long i = 0; i < ITERS; i++) {
        /* Mixed field pull spanning early, middle and late keys — the realistic
         * "read every field of the tool call" pattern. */
        char *mm = json_get_str(TOOL_CALL, "command");
        char *pp = json_get_str(TOOL_CALL, "path");
        char *cc = json_get_str(TOOL_CALL, "cwd");
        sink += (long)json_get_int(TOOL_CALL, "limit", 0);
        sink += (long)json_get_int(TOOL_CALL, "max_results", 0);
        sink += (long)json_get_i64(TOOL_CALL, "timeout_ms", 0);
        sink += (long)json_get_i64(TOOL_CALL, "input_tokens", 0);
        sink += json_get_bool(TOOL_CALL, "case_sensitive", false) ? 1 : 0;
        sink += (long)(mm ? mm[0] : 0);
        sink += (long)(pp ? pp[0] : 0);
        sink += (long)(cc ? cc[0] : 0);
        free(mm);
        free(pp);
        free(cc);
    }
    double t1 = now_ns();
    double per = (t1 - t0) / (double)ITERS;
    printf("json_get_* multi-field pull: %.1f ns/iter (%ld iters), sink=%ld\n",
           per, ITERS, (long)sink);
    return 0;
}
