#include "rl_hooks.h"

#include "chronicle.h"
#include "json_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

static bool hooks_enabled(void) {
    const char *mode = getenv("DSCO_RL_HOOKS");
    return !mode || strcmp(mode, "off") != 0;
}

static const char *nz(const char *s) { return s ? s : ""; }

bool rl_hooks_format_event(const rl_hook_event_t *event, char *out, size_t out_len) {
    if (!event || !event->run_id || !event->run_id[0] || !event->event_name || !out || out_len == 0)
        return false;
    jbuf_t b;
    jbuf_init(&b, 384);
    jbuf_append(&b, "{\"schema\":\"dsco.rl_trajectory.v1\",\"run_id\":");
    jbuf_append_json_str(&b, event->run_id);
    jbuf_appendf(&b, ",\"sequence\":%llu,\"timestamp_ms\":%lld", event->sequence, event->timestamp_ms);
    jbuf_append(&b, ",\"event\":"); jbuf_append_json_str(&b, event->event_name);
    jbuf_append(&b, ",\"category\":"); jbuf_append_json_str(&b, nz(event->category));
    jbuf_append(&b, ",\"status\":"); jbuf_append_json_str(&b, nz(event->status));
    jbuf_appendf(&b, ",\"cost\":{\"usd_delta\":%.8f,\"input_tokens\":%d,\"output_tokens\":%d}",
                 event->usd_delta, event->input_tokens, event->output_tokens);
    jbuf_append(&b, ",\"rl_context\":{\"environment_id\":");
    jbuf_append_json_str(&b, nz(event->environment_id));
    jbuf_append(&b, ",\"environment_fidelity\":");
    jbuf_append_json_str(&b, nz(event->environment_fidelity));
    jbuf_append(&b, ",\"verifier_id\":");
    jbuf_append_json_str(&b, nz(event->verifier_id));
    jbuf_append(&b, ",\"verifier_version\":");
    jbuf_append_json_str(&b, nz(event->verifier_version));
    jbuf_append(&b, ",\"branch_id\":");
    jbuf_append_json_str(&b, nz(event->branch_id));
    jbuf_append(&b, ",\"checkpoint_id\":");
    jbuf_append_json_str(&b, nz(event->checkpoint_id));
    jbuf_appendf(&b, ",\"semantic_step\":%d,\"semantic_horizon\":%d}",
                 event->semantic_step, event->semantic_horizon);
    jbuf_append(&b, ",\"payload\":");
    if (event->payload_json && json_is_valid_container(event->payload_json)) jbuf_append(&b, event->payload_json);
    else jbuf_append(&b, "{}");
    jbuf_append(&b, "}");
    if (b.len + 1 > out_len) { jbuf_free(&b); return false; }
    memcpy(out, b.data, b.len + 1);
    jbuf_free(&b);
    return true;
}

static uint32_t read_u32le(const unsigned char hdr[4]) {
    return (uint32_t)hdr[0] | ((uint32_t)hdr[1] << 8) | ((uint32_t)hdr[2] << 16) |
           ((uint32_t)hdr[3] << 24);
}

bool rl_hooks_inspect_run(const char *run_id, char *out, size_t out_len) {
    if (!out || out_len == 0) return false;
    const char *id = (run_id && run_id[0]) ? run_id : chronicle_run_id();
    if (!id || !id[0] || strchr(id, '/')) return false;
    char path[PATH_MAX];
    const char *root = getenv("DSCO_RUNS_DIR");
    if (!root || !root[0]) {
        const char *home = getenv("HOME");
        snprintf(path, sizeof(path), "%s/.dsco/runs/%s/journal.wal", home && home[0] ? home : ".", id);
    } else snprintf(path, sizeof(path), "%s/%s/journal.wal", root, id);
    FILE *fp = fopen(path, "rb");
    if (!fp) return false;
    unsigned long long frames = 0, trajectory = 0, tools = 0, checkpoints = 0, branches = 0;
    bool environment = false, verifier = false, malformed = false;
    unsigned char hdr[8];
    while (fread(hdr, 1, sizeof(hdr), fp) == sizeof(hdr)) {
        uint32_t len = read_u32le(hdr);
        if (len == 0 || len > 16U * 1024U * 1024U) { malformed = true; break; }
        char *record = malloc((size_t)len + 1);
        if (!record || fread(record, 1, len, fp) != len) { free(record); malformed = true; break; }
        record[len] = '\0'; frames++;
        if (strstr(record, "\"type\":\"rl.trajectory.event\"")) {
            trajectory++;
            if (strstr(record, "\"category\":\"tool\"")) tools++;
            if (strstr(record, "\"environment_id\":\"") && !strstr(record, "\"environment_id\":\"\"")) environment = true;
            if (strstr(record, "\"verifier_id\":\"") && !strstr(record, "\"verifier_id\":\"\"")) verifier = true;
            if (strstr(record, "\"branch_id\":\"") && !strstr(record, "\"branch_id\":\"\"")) branches++;
            if (strstr(record, "\"checkpoint_id\":\"") && !strstr(record, "\"checkpoint_id\":\"\"")) checkpoints++;
        }
        free(record);
    }
    fclose(fp);
    int n = snprintf(out, out_len,
        "{\"schema\":\"dsco.learn_inspect.v1\",\"run_id\":\"%s\",\"journal_frames\":%llu,\"trajectory_events\":%llu,\"tool_events\":%llu,\"branches\":%llu,\"checkpoints\":%llu,\"environment_manifest\":%s,\"verifier_receipt\":%s,\"journal_complete\":%s,\"learning_readiness\":{\"trajectory_complete\":%s,\"environment_manifest\":%s,\"verifier_receipt\":%s,\"heldout_eval\":false,\"promotion_eligible\":false}}",
        id, frames, trajectory, tools, branches, checkpoints, environment ? "true" : "false",
        verifier ? "true" : "false", malformed ? "false" : "true", trajectory ? "true" : "false",
        environment ? "true" : "false", verifier ? "true" : "false");
    return n >= 0 && (size_t)n < out_len;
}

bool rl_hooks_record_event(const rl_hook_event_t *event) {
    if (!event || !event->event_name || !event->event_name[0]) return false;
    if (!hooks_enabled()) return true;
    const char *run_id = (event->run_id && event->run_id[0]) ? event->run_id : chronicle_run_id();
    if (!run_id || !run_id[0]) return true; /* Chronicle has not started: no durable run yet. */
    rl_hook_event_t normalized = *event;
    normalized.run_id = run_id;
    char line[8192];
    if (!rl_hooks_format_event(&normalized, line, sizeof(line))) return false;
    return chronicle_journal_append("rl.trajectory.event", line, false);
}
