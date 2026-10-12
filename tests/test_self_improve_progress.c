#define _DARWIN_C_SOURCE
#include "self_improve.h"
#include "json_util.h"
#include "baseline.h"
#include "cost_model.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* External observation sinks are inert; all learning, parsing and persistence
 * below execute the production implementation with an isolated HOME. */
bool baseline_log(const char *category, const char *title, const char *detail, const char *metadata) {
    (void)category; (void)title; (void)detail; (void)metadata;
    return true;
}
bool baseline_log_usage(const char *category, const char *title, const char *detail,
                        const char *metadata, int input, int output, int read, int write) {
    (void)category; (void)title; (void)detail; (void)metadata;
    (void)input; (void)output; (void)read; (void)write;
    return true;
}
void cost_model_learn(const char *topology, int tokens, double cost, double latency) {
    (void)topology; (void)tokens; (void)cost; (void)latency;
}
static int checks, failures;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); } } while (0)

static void write_history(const char *path, const char *body) {
    FILE *f = fopen(path, "w");
    CHECK(f != NULL);
    if (f) { CHECK(fputs(body, f) >= 0); CHECK(fclose(f) == 0); }
}

int main(void) {
    char home[] = "/tmp/dsco-learning-XXXXXX", directory[256], history[280];
    CHECK(mkdtemp(home) != NULL);
    CHECK(setenv("HOME", home, 1) == 0);
    CHECK(setenv("DSCO_TRACE", "0", 1) == 0);
    snprintf(directory, sizeof(directory), "%s/.dsco", home);
    snprintf(history, sizeof(history), "%s/self_improve.json", directory);
    CHECK(mkdir(directory, 0700) == 0);
    self_improve_t si;
    self_improve_init(&si);
    CHECK(self_improve_consolidate(&si) == 0 && si.consolidation_count == 0);
    for (int i = 0; i < 3; ++i)
        self_improve_record_tool(&si, "failing_fixture", false, 10.0, 2);
    CHECK(self_improve_consolidate(&si) > 0);
    CHECK(si.evidence_revision == 3 && si.consolidated_revision == 3);
    CHECK(si.consolidation_count == 1 && si.suggestion_count == 1);
    for (int i = 0; i < 100; ++i) {
        self_improve_record_tool(&si, "self_improve", true, 1.0, 10);
        self_improve_record_tool(&si, "self_assess", false, 1.0, 10);
        self_improve_record_redundant(&si, "self_improve");
    }
    CHECK(si.evidence_revision == 3 && si.total_tool_calls == 3);
    si_strategy_weights_t learned = si.weights;
    int occurrences = si.patterns[si.pattern_count - 1].occurrences;
    for (int i = 0; i < 1000; ++i) CHECK(self_improve_consolidate(&si) == 0);
    CHECK(!memcmp(&learned, &si.weights, sizeof(learned)));
    CHECK(si.consolidation_count == 1 && si.suggestion_count == 1);
    CHECK(si.patterns[si.pattern_count - 1].occurrences == occurrences);
    self_improve_record_tool(&si, "healthy_fixture", true, 10.0, 2);
    CHECK(self_improve_consolidate(&si) == 0);
    CHECK(!memcmp(&learned, &si.weights, sizeof(learned)));
    self_improve_record_tool(&si, "failing_fixture", false, 10.0, 2);
    CHECK(self_improve_consolidate(&si) == 0); /* recommendation refreshed */
    CHECK(si.weights.cache_aggressiveness > learned.cache_aggressiveness);
    CHECK(si.suggestion_count == 1);
    CHECK(strstr(si.suggestions[0].description, "4/4") != NULL);
    self_improve_acknowledge(&si, 0);
    int applied = si.improvements_applied;
    self_improve_acknowledge(&si, 0);
    CHECK(si.improvements_applied == applied);

    si.weights.tool_timeout_aggression = 0.17;
    CHECK(self_improve_save_history(&si));
    self_improve_t restored;
    self_improve_init(&restored);
    CHECK(self_improve_load_history(&restored));
    CHECK(fabs(restored.weights.tool_timeout_aggression - 0.17) < 0.0001);
    CHECK(restored.sessions_seen == si.sessions_seen + 1);
    CHECK(restored.evidence_revision == 0); /* previous evidence is a prior */
    restored.weights.cache_aggressiveness = 0.91;
    CHECK(!self_improve_load_history(&restored));
    CHECK(restored.weights.cache_aggressiveness == 0.91 && restored.sessions_seen == 1);
    FILE *f = fopen(history, "r");
    char original[8192] = {0};
    CHECK(f != NULL);
    if (f) { CHECK(fread(original, 1, sizeof(original) - 1, f) > 0); fclose(f); }
    CHECK(json_is_valid_container(original));
    CHECK(strstr(original, "dsco.self_improve.history.v2") != NULL);
    si.weights.cache_aggressiveness = NAN;
    CHECK(!self_improve_save_history(&si));
    f = fopen(history, "r");
    char intact[8192] = {0};
    CHECK(f != NULL);
    if (f) { CHECK(fread(intact, 1, sizeof(intact) - 1, f) > 0); fclose(f); }
    CHECK(!strcmp(original, intact));

    const char *bad[] = {"{\"sessions_seen\":999", "{\"sessions_seen\":-1}",
                         "{\"sessions_seen\":1e100}", "{\"sessions_seen\":1.5}",
                         "{\"weights\":{\"cache_aggressiveness\":NaN}}"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        write_history(history, bad[i]);
        self_improve_init(&restored);
        learned = restored.weights;
        CHECK(!self_improve_load_history(&restored));
        CHECK(restored.sessions_seen == 0 && !memcmp(&restored.weights, &learned, sizeof(learned)));
    }
    char oversized[9000];
    memset(oversized, ' ', sizeof(oversized));
    memcpy(oversized, "{\"sessions_seen\":7}", 19);
    oversized[sizeof(oversized) - 1] = 0;
    write_history(history, oversized);
    self_improve_init(&restored);
    CHECK(!self_improve_load_history(&restored) && restored.sessions_seen == 0);

    self_improve_init(&g_self_improve);
    char result[2048];
    self_improve_record_redundant(&g_self_improve, "fixture");
    CHECK(tool_self_improve("{\"action\":\"progress\"}", result, sizeof(result)));
    CHECK(json_is_valid_container(result));
    CHECK(json_get_bool(result, "pending_evidence", false));
    CHECK(!json_get_bool(result, "quality_verified", true));
    CHECK(!tool_self_improve("{\"action\":\"progress\"}", result, 8));
    self_improve_consolidate(&g_self_improve);
    CHECK(tool_self_improve("{\"action\":\"progress\"}", result, sizeof(result)));
    CHECK(!json_get_bool(result, "pending_evidence", true));
    CHECK(json_get_int(result, "consolidations", 0) == 1);
    unlink(history);
    rmdir(directory);
    rmdir(home);
    printf("self-improvement progress: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
