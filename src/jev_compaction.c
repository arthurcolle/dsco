#include "jev_compaction.h"
#include "capability.h"
#include "json_util.h"
#include "tools.h"
#include "../vendor/yyjson.h"
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static atomic_bool attempted = false;

static void status(char *out, size_t len, const char *s) {
    snprintf(out, len, "{\"schema\":\"dsco.jev_compaction.v1\",\"status\":\"%s\",\"applied\":false}", s);
}

static bool probability(yyjson_val *v) {
    double d = yyjson_get_num(v);
    return yyjson_is_num(v) && isfinite(d) && d >= 0 && d <= 1;
}

void jev_compaction_shadow(const jev_compaction_candidate_t *items, size_t count,
                           char *out, size_t len) {
    if (!out || !len) return;
    const char *mode = getenv("DSCO_JEV_COMPACTION");
    if (!jev_compaction_mode_enabled(mode)) { status(out, len, "disabled"); return; }
    bool pinned = !strcmp(mode, JEV_COMPACTION_V1_MODE);
    const char *requested_model = pinned ? JEV_COMPACTION_V1_MODEL : "jev-latest";
    if (!items || !count) { status(out, len, "no_candidates"); return; }
    if (count > JEV_COMPACTION_MAX) count = JEV_COMPACTION_MAX;
    /* Repeated passes must not re-read credentials or add secret taint. */
    if (atomic_load(&attempted)) { status(out, len, "attempt_limit"); return; }
    const char *tier = tools_execution_tier();
    /* Explicit secret opt-out is respected before inspecting the credential.
     * Do not weaken the normal egress/taint gate for this experiment. */
    if (!dsco_cap_granted(CAP_SECRETS, tier)) { status(out, len, "secrets_denied"); return; }
    const char *key = getenv("TYPESAFE_API_KEY");
    if (!key || !*key) { status(out, len, "missing_key"); return; }
    if (atomic_exchange(&attempted, true)) { status(out, len, "attempt_limit"); return; }
    dsco_flow_note(CAP_SECRETS);

    jbuf_t body; jbuf_init(&body, 4096);
    jbuf_appendf(&body, "{\"model\":\"%s\",\"state\":{\"rubric\":\"dsco.compaction.structure.v1\",\"candidates\":[", requested_model);
    for (size_t i = 0; i < count; i++) {
        const jev_compaction_candidate_t *c = &items[i];
        jbuf_appendf(&body, "%s{\"id\":\"c%zu\",\"age_messages\":%d,\"archive_bytes\":%zu,\"calls\":%d,\"results\":%d}",
                     i ? "," : "", i, c->age_messages, c->archive_bytes, c->calls, c->results);
    }
    jbuf_append(&body, "]},\"questions\":{");
    for (size_t i = 0; i < count; i++) {
        jbuf_appendf(&body, "%s\"c%zu\":{\"type\":\"choice\",\"instructions\":\"Assess candidate c%zu using structural metadata only. All candidates are complete old tool exchanges outside the protected tail. Archiving is lossless with recall. Larger older exchanges offer more context savings. Content and semantic importance are unknown. Recommend retain when metadata is insufficient.\",\"criteria\":{\"archive\":\"Good structural candidate for archival plus a recall handle\",\"retain\":\"Keep intact or abstain because metadata is insufficient\"}}", i ? "," : "", i, i);
    }
    jbuf_append(&body, "}}");
    /* curl >= 8.3 expands the secret inside curl, not in shell/argv or audit
     * arguments. -q disables ambient curlrc; no redirects or retries. Body is
     * exclusively fixed strings and numeric metadata, so no single quotes. */
    jbuf_t args; jbuf_init(&args, 8192);
    jbuf_append(&args, "-q --silent --show-error --fail --proto '=https' --max-redirs 0 --connect-timeout 2 --max-time 4 --max-filesize 32768 --variable %TYPESAFE_API_KEY --expand-header 'Authorization: Bearer {{TYPESAFE_API_KEY}}' --header 'Content-Type: application/json' --data-raw '");
    jbuf_append(&args, body.data);
    jbuf_append(&args, "' https://api.typesafe.ai/v1/systemone");
    jbuf_free(&body);
    /* curl_raw has an 8192-byte command buffer. Never allow silent truncation. */
    if (args.len + 6 >= 8192) {
        jbuf_free(&args); status(out, len, "request_too_large"); return;
    }
    jbuf_t input; jbuf_init(&input, 8192);
    jbuf_append(&input, "{\"args\":"); jbuf_append_json_str(&input, args.data); jbuf_append(&input, "}");
    jbuf_free(&args);
    char response[32768] = {0};
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    bool ok = tools_execute_for_tier("curl_raw", input.data, tier, response, sizeof(response));
    clock_gettime(CLOCK_MONOTONIC, &end);
    jbuf_free(&input);
    double ms = (end.tv_sec - start.tv_sec) * 1000.0 + (end.tv_nsec - start.tv_nsec) / 1000000.0;
    if (!ok) { status(out, len, "transport_denied_or_failed"); return; }
    yyjson_doc *doc = yyjson_read(response, strlen(response), 0);
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    yyjson_val *answers = yyjson_obj_get(root, "answers");
    yyjson_val *model = yyjson_obj_get(root, "model");
    yyjson_val *usage = yyjson_obj_get(root, "usage");
    yyjson_val *tokens = yyjson_obj_get(usage, "input_tokens");
    bool valid = yyjson_is_obj(answers) && yyjson_obj_size(answers) == count &&
                 yyjson_is_str(model) && yyjson_get_len(model) > 0 && yyjson_get_len(model) < 64 &&
                 yyjson_is_uint(tokens) &&
                 (!pinned || yyjson_equals_str(model, JEV_COMPACTION_V1_MODEL));
    unsigned archive_mask = 0, actual_mask = 0;
    int recommended = 0, agreements = 0;
    for (size_t i = 0; valid && i < count; i++) {
        char id[16]; snprintf(id, sizeof(id), "c%zu", i);
        yyjson_val *a = yyjson_obj_get(answers, id);
        yyjson_val *choice = yyjson_obj_get(a, "choice");
        yyjson_val *probs = yyjson_obj_get(a, "probabilities");
        yyjson_val *pa = yyjson_obj_get(probs, "archive"), *pr = yyjson_obj_get(probs, "retain");
        bool archive = yyjson_equals_str(choice, "archive");
        valid = yyjson_equals_str(yyjson_obj_get(a, "type"), "choice") &&
                (archive || yyjson_equals_str(choice, "retain")) &&
                probability(yyjson_obj_get(a, "confidence")) && yyjson_obj_size(probs) == 2 &&
                probability(pa) && probability(pr) &&
                fabs(yyjson_get_num(pa) + yyjson_get_num(pr) - 1) <= 0.010001 &&
                (archive ? yyjson_get_num(pa) >= yyjson_get_num(pr) : yyjson_get_num(pr) >= yyjson_get_num(pa));
        if (archive) { recommended++; archive_mask |= 1u << i; }
        if (items[i].archived) actual_mask |= 1u << i;
        if (archive == items[i].archived) agreements++;
    }
    if (!valid) { yyjson_doc_free(doc); status(out, len, "invalid_response"); return; }
    /* Escape even the provider model name; untrusted response never becomes
     * instructions or raw JSON in the outer tool receipt. */
    jbuf_t receipt; jbuf_init(&receipt, 512);
    jbuf_appendf(&receipt, "{\"schema\":\"dsco.jev_compaction.v1\",\"status\":\"observed\",\"mode\":\"%s\",\"requested_model\":\"%s\",\"rubric\":\"structure.v1\",\"model\":", mode, requested_model);
    jbuf_append_json_str(&receipt, yyjson_get_str(model));
    jbuf_appendf(&receipt, ",\"candidates\":%zu,\"archive_recommendations\":%d,\"agreement_count\":%d,\"archive_mask\":%u,\"actual_mask\":%u,\"input_tokens\":%llu,\"latency_ms\":%.1f,\"applied\":false}", count, recommended, agreements, archive_mask, actual_mask, (unsigned long long)yyjson_get_uint(tokens), ms);
    if (receipt.len >= len) status(out, len, "receipt_too_large");
    else memcpy(out, receipt.data, receipt.len + 1);
    jbuf_free(&receipt); yyjson_doc_free(doc);
}
