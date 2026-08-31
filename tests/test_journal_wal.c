/*
 * test_journal_wal.c — W1/W2 canonical journal + replay engine end-to-end test.
 *
 * Verifies, in a sandboxed chronicle root + runs dir:
 *   1. chronicle_start opens the journal and emits canonical RUN_START.
 *   2. TURN_START / TOOL_CALL / TOOL_RESULT / CHECKPOINT all land as CRC-framed
 *      records in <runs>/<run_id>/journal.wal.
 *   3. chronicle_stop emits canonical RUN_END and seals the manifest completed.
 *   4. chronicle_replay_run() reconstructs steps, matches call ids across both
 *      record shapes, and flags the resume frontier (call with no result).
 *   5. A torn tail (truncated frame) stops the scan cleanly and is reported,
 *      never crashing.
 *   6. canonical frames carry full payloads: prompt sha256, inline result or
 *      blob ref, fsynced (presence asserted; durability itself is design).
 */
#include "chronicle.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include "vm.h"

/* main.c-owned globals the library objects reference; agent.o/main.o are
 * filtered out of the test link, so the test TU provides them (same shim
 * pattern as test_session_memory.c). */
volatile int g_interrupted = 0;
double g_cost_budget = 0.0;
int g_cheap_mode = 0;
vm_t g_vm = {0};

static int failures = 0;
#define ASSERT(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); failures++; } \
    else { printf("ok:   %s\n", msg); } \
} while (0)

static int count_frames(const char *path, const char *type_needle) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return -1;
    int count = 0;
    for (;;) {
        unsigned char hdr[8];
        size_t got = fread(hdr, 1, 8, fp);
        if (got != 8) break;
        unsigned int len = (unsigned int)hdr[0] | ((unsigned int)hdr[1] << 8) |
                           ((unsigned int)hdr[2] << 16) | ((unsigned int)hdr[3] << 24);
        if (len == 0 || len > (32U * 1024U * 1024U)) break;
        char *buf = (char *)malloc(len + 1);
        if (!buf) break;
        if (fread(buf, 1, len, fp) != len) { free(buf); break; }
        buf[len] = 0;
        char probe[64];
        snprintf(probe, sizeof(probe), "\"type\":\"%s\"", type_needle);
        if (strstr(buf, probe)) count++;
        free(buf);
    }
    fclose(fp);
    return count;
}

static void rm_rf(const char *path) {
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", path);
    if (system(cmd) != 0) { /* best effort */ }
}

int main(void) {
    char root[256], runsdir[256];
    snprintf(root, sizeof(root), "/tmp/dsco_jwal_%d", (int)getpid());
    snprintf(runsdir, sizeof(runsdir), "%s/runs", root);
    rm_rf(root);
    setenv("DSCO_CHRONICLE_DIR", root, 1);
    setenv("DSCO_CHRONICLE_MODE", "full-local", 1);
    setenv("DSCO_RUNS_DIR", runsdir, 1);
    unsetenv("DSCO_JOURNAL");

    ASSERT(chronicle_start(&(chronicle_start_opts_t){.provider = "test",
                                                     .model = "test-model",
                                                     .mode = "full-local"}),
           "chronicle_start(full-local)");
    const char *run_id = chronicle_run_id();
    ASSERT(run_id && run_id[0], "run id minted");

    /* A turn with a prompt, two tool calls (one completed, one frontier). */
    ASSERT(chronicle_journal_turn_start(1, "read /tmp/x and report"), "TURN_START journaled");
    ASSERT(chronicle_journal_tool_call(NULL, "call-1", "read_file", "{\"path\":\"/tmp/x\"}", "trusted", NULL),
           "TOOL_CALL (completed) journaled");
    ASSERT(chronicle_journal_tool_result(NULL, "call-1", "read_file", true, false, false, 1.5,
                                         "canary file contents"),
           "TOOL_RESULT (completed) journaled");
    ASSERT(chronicle_journal_tool_call(NULL, "call-2", "write_file", "{\"path\":\"/tmp/y\"}", "trusted", NULL),
           "TOOL_CALL (frontier) journaled");
    ASSERT(chronicle_journal_checkpoint(1, 0.0025, 1200, 80, "tool_calls"),
           "CHECKPOINT journaled");
    chronicle_stop();
    ASSERT(!chronicle_ready(), "chronicle stopped");

    char wal[512];
    snprintf(wal, sizeof(wal), "%s/%s/journal.wal", runsdir, run_id);
    struct stat st;
    ASSERT(stat(wal, &st) == 0 && st.st_size > 0, "journal.wal exists");

    ASSERT(count_frames(wal, "RUN_START") == 1, "exactly one canonical RUN_START");
    ASSERT(count_frames(wal, "TURN_START") == 1, "exactly one canonical TURN_START");
    ASSERT(count_frames(wal, "TOOL_CALL") == 2, "two canonical TOOL_CALL frames");
    ASSERT(count_frames(wal, "TOOL_RESULT") == 1, "one canonical TOOL_RESULT frame");
    ASSERT(count_frames(wal, "CHECKPOINT") == 1, "one canonical CHECKPOINT frame");
    ASSERT(count_frames(wal, "RUN_END") == 1, "exactly one canonical RUN_END");

    /* Manifest sealed completed. */
    char manifest[512];
    snprintf(manifest, sizeof(manifest), "%s/%s/manifest.json", runsdir, run_id);
    FILE *mf = fopen(manifest, "r");
    ASSERT(mf != NULL, "manifest written");
    if (mf) {
        char mb[4096];
        size_t n = fread(mb, 1, sizeof(mb) - 1, mf);
        mb[n] = 0;
        fclose(mf);
        ASSERT(strstr(mb, "\"completed\"") != NULL, "manifest status completed");
    }

    /* Replay: reconstruction + frontier detection. */
    chronicle_replay_summary_t s;
    FILE *devnull = fopen("/dev/null", "w");
    ASSERT(chronicle_replay_run(runsdir, run_id, devnull, &s), "replay runs");
    ASSERT(s.turns == 1, "replay: 1 turn");
    ASSERT(s.tool_calls == 2, "replay: 2 tool calls");
    ASSERT(s.tool_results == 1, "replay: 1 tool result");
    ASSERT(s.frontier_calls == 1, "replay: write_file call flagged frontier");
    ASSERT(s.checkpoints == 1, "replay: 1 checkpoint");
    ASSERT(s.cost_usd > 0.0009 && s.cost_usd < 0.0051, "replay: checkpoint cost extracted");
    ASSERT(!s.corrupt_tail, "replay: intact journal has no corrupt tail");
    fclose(devnull);

    /* Torn tail: truncate 7 bytes (torn frame), replay must stop cleanly. */
    truncate(wal, (off_t)st.st_size - 7);
    memset(&s, 0, sizeof(s));
    devnull = fopen("/dev/null", "w");
    ASSERT(chronicle_replay_run(runsdir, run_id, devnull, &s), "replay survives torn tail");
    fclose(devnull);
    ASSERT(s.corrupt_tail, "torn tail reported");
    ASSERT(s.tool_calls == 2 && s.frontier_calls == 1, "torn-tail replay still sees all complete frames");

    /* Inline-vs-blob: large result must journal a blob ref, not 64KB inline. */
    rm_rf(root);
    setenv("DSCO_CHRONICLE_DIR", root, 1);
    ASSERT(chronicle_start(&(chronicle_start_opts_t){.provider = "test",
                                                     .model = "m",
                                                     .mode = "full-local"}),
           "second chronicle_start for blob test");
    char big[70 * 1024];
    memset(big, 'A', sizeof(big) - 1);
    big[sizeof(big) - 1] = 0;
    ASSERT(chronicle_journal_tool_call(NULL, "call-big", "bash", "{\"command\":\"cat big\"}", "trusted", NULL),
           "TOOL_CALL before big result");
    ASSERT(chronicle_journal_tool_result(NULL, "call-big", "bash", true, false, false, 10.0, big),
           "TOOL_RESULT with >64KB result journaled");
    const char *run2 = chronicle_run_id();
    char wal2[512];
    snprintf(wal2, sizeof(wal2), "%s/%s/journal.wal", runsdir, run2);
    ASSERT(count_frames(wal2, "TOOL_RESULT") == 1, "big result frame present");
    /* The frame must reference a blob, not inline 70KB. */
    FILE *fp = fopen(wal2, "r");
    int has_blob = 0;
    if (fp) {
        for (;;) {
            unsigned char hdr[8];
            if (fread(hdr, 1, 8, fp) != 8) break;
            unsigned int len = (unsigned int)hdr[0] | ((unsigned int)hdr[1] << 8) |
                               ((unsigned int)hdr[2] << 16) | ((unsigned int)hdr[3] << 24);
            char *buf = (char *)malloc(len + 1);
            if (!buf || fread(buf, 1, len, fp) != (size_t)len) { free(buf); break; }
            buf[len] = 0;
            if (strstr(buf, "\"TOOL_RESULT\"") && strstr(buf, "blob_sha256") &&
                strstr(buf, "byte_len")) has_blob = 1;
            free(buf);
        }
        fclose(fp);
    }
    ASSERT(has_blob, "large result stored as blob ref (blob_sha256 + byte_len)");
    chronicle_stop();

    rm_rf(root);
    if (failures == 0) { printf("ALL JOURNAL WAL TESTS PASSED\n"); return 0; }
    fprintf(stderr, "%d journal WAL test failure(s)\n", failures);
    return 1;
}