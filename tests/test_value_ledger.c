#define _POSIX_C_SOURCE 200809L
#include "chronicle.h"
#include "value_ledger.h"
#include "vm.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Globals normally defined in main.o (which the test link excludes). */
int g_cheap_mode = 0;
vm_t g_vm;

int main(void) {
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "/tmp/dsco-value-ledger.%ld", (long)getpid());
    assert(mkdir(tmp, 0700) == 0);
    assert(setenv("HOME", tmp, 1) == 0);
    assert(setenv("DSCO_RUNS_DIR", tmp, 1) == 0);
    assert(setenv("DSCO_CHRONICLE_MODE", "full-local", 1) == 0);
    assert(setenv("DSCO_JOURNAL", "on", 1) == 0);

    assert(chronicle_start(&(chronicle_start_opts_t){
        .provider = "test", .model = "test", .mode = "test", .instance_id = "value-ledger-test"
    }));
    assert(chronicle_run_id() && chronicle_run_id()[0]);

    value_receipt_t a = {
        .workload_id = "test-workload", .principal = "DSI",
        .outcome = "first verified outcome", .authority_json = "[\"read\"]",
        .verify_method = "deterministic", .verified = true,
        .autonomous = true,
        .price_usd = 100.0, .compute_cost_usd = 20.0, .human_minutes = 30.0,
        .latency_ms = 1000, .retries = 0, .recovery_ms = 0,
        .reuse_json = "{\"tools\":[\"read_file\"]}",
        .incident = false, .rollback_verified = true, .next = "expand"
    };
    char hash_a[65];
    assert(value_ledger_emit(&a, hash_a, sizeof(hash_a)));
    assert(strlen(hash_a) == 64);

    value_receipt_t b = {
        .workload_id = "test-workload", .principal = "DSI",
        .outcome = "recovered outcome", .authority_json = "[\"read\",\"write:scoped\"]",
        .verify_method = "held-out", .verified = true,
        .autonomous = true,
        .price_usd = 50.0, .compute_cost_usd = 10.0, .human_minutes = 0.0,
        .latency_ms = 1500, .retries = 1, .recovery_ms = 250,
        .reuse_json = "{}", .incident = true, .rollback_verified = true, .next = "distill"
    };
    char hash_b[65];
    assert(value_ledger_emit(&b, hash_b, sizeof(hash_b)));
    assert(strlen(hash_b) == 64 && strcmp(hash_a, hash_b) != 0);

    char summary[2048];
    assert(value_ledger_summary(NULL, summary, sizeof(summary)));
    assert(strstr(summary, "\"receipts\":2"));
    assert(strstr(summary, "\"verified\":2"));
    assert(strstr(summary, "\"verified_work_value_usd\":150.0000"));
    assert(strstr(summary, "\"compute_cost_usd\":30.0000"));
    assert(strstr(summary, "\"gross_compute_margin\":0.8000"));
    assert(strstr(summary, "\"autonomous_completion_rate\":1.0000"));
    assert(strstr(summary, "\"recovery_rate\":1.0000"));
    assert(strstr(summary, "\"human_leverage_usd_per_hr\":300.0000"));
    assert(strstr(summary, "\"incidents\":1"));

    puts(summary);
    puts("ok: value ledger emit/hash/summary math");
    chronicle_stop();
    return 0;
}
