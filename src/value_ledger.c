#include "value_ledger.h"
#include "chronicle.h"
#include "crypto.h"
#include "json_util.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define RECEIPT_TYPE "value.receipt.v1"

static const char *nz(const char *s) { return s ? s : ""; }

static void now_iso(char out[32]) {
    time_t now = time(NULL);
    struct tm tmv;
    gmtime_r(&now, &tmv);
    strftime(out, 32, "%Y-%m-%dT%H:%M:%SZ", &tmv);
}

/* Build the receipt body (without its own hash), so we can hash it and then
 * emit body+hash. Keeps the content hash deterministic over the payload. */
static void build_body(const value_receipt_t *r, const char *when, jbuf_t *b) {
    jbuf_init(b, 768);
    jbuf_append(b, "{\"schema\":\"value.receipt.v1\"");
    jbuf_append(b, ",\"timestamp\":");     jbuf_append_json_str(b, when);
    jbuf_append(b, ",\"workload_id\":");   jbuf_append_json_str(b, nz(r->workload_id));
    jbuf_append(b, ",\"principal\":");     jbuf_append_json_str(b, nz(r->principal));
    jbuf_append(b, ",\"outcome\":");       jbuf_append_json_str(b, nz(r->outcome));
    jbuf_append(b, ",\"authority\":");     jbuf_append(b, (r->authority_json && r->authority_json[0]) ? r->authority_json : "[]");
    jbuf_append(b, ",\"inputs_hash\":");   jbuf_append_json_str(b, nz(r->inputs_hash));
    jbuf_append(b, ",\"result_hash\":");   jbuf_append_json_str(b, nz(r->result_hash));
    jbuf_append(b, ",\"verification\":{\"method\":");
    jbuf_append_json_str(b, r->verify_method && r->verify_method[0] ? r->verify_method : "none");
    jbuf_appendf(b, ",\"passed\":%s}", r->verified ? "true" : "false");
    jbuf_appendf(b, ",\"autonomous\":%s", r->autonomous ? "true" : "false");
    jbuf_appendf(b, ",\"economics\":{\"price_usd\":%.6f,\"compute_cost_usd\":%.6f,\"human_minutes\":%.3f}",
                 r->price_usd, r->compute_cost_usd, r->human_minutes);
    jbuf_appendf(b, ",\"performance\":{\"latency_ms\":%.3f,\"retries\":%d,\"recovery_ms\":%.3f}",
                 r->latency_ms, r->retries, r->recovery_ms);
    jbuf_append(b, ",\"reuse\":");         jbuf_append(b, (r->reuse_json && r->reuse_json[0]) ? r->reuse_json : "{}");
    jbuf_appendf(b, ",\"risk\":{\"incident\":%s,\"rollback_verified\":%s}",
                 r->incident ? "true" : "false", r->rollback_verified ? "true" : "false");
    jbuf_append(b, ",\"next\":");          jbuf_append_json_str(b, nz(r->next));
    jbuf_append(b, "}");
}

bool value_ledger_emit(const value_receipt_t *r, char *version_out, size_t version_len) {
    if (version_out && version_len) version_out[0] = '\0';
    if (!r || (version_out && version_len < 65)) return false;
    char when[32];
    now_iso(when);
    jbuf_t body;
    build_body(r, when, &body);
    if (!body.data) return false;

    char version[65];
    sha256_hex((const uint8_t *)body.data, body.len, version);

    /* Wrap body with its own content hash for the journal payload. */
    jbuf_t payload;
    jbuf_init(&payload, body.len + 96);
    jbuf_appendf(&payload, "{\"version\":\"%s\",\"receipt\":", version);
    jbuf_append(&payload, body.data);
    jbuf_append(&payload, "}");
    jbuf_free(&body);
    if (!payload.data) return false;

    /* Missing Chronicle run or journal failure is an error (no-op success removed). */
    if (!chronicle_run_id() || !chronicle_run_id()[0]) {
        jbuf_free(&payload);
        return false;
    }
    bool ok = chronicle_journal_append(RECEIPT_TYPE, payload.data, true);
    jbuf_free(&payload);
    /* Only output hash if append succeeded. */
    if (ok && version_out && version_len)
        snprintf(version_out, version_len, "%s", version);
    return ok;
}

static uint32_t read_u32le(const unsigned char h[4]) {
    return (uint32_t)h[0] | ((uint32_t)h[1] << 8) | ((uint32_t)h[2] << 16) | ((uint32_t)h[3] << 24);
}

/* Receipts are wrapped by the journal record and nest economics/performance one
 * level deep, so top-level json_get_double cannot reach them. Locate the key by
 * substring within a single receipt frame and parse the number that follows. */
static double frame_num(const char *rec, const char *key) {
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char *p = strstr(rec, needle);
    if (!p) return 0;
    return strtod(p + strlen(needle), NULL);
}

static bool valid_run_id(const char *id) {
    if (!id || !id[0]) return false;
    size_t n = strlen(id);
    if (n > 128 || strcmp(id, ".") == 0 || strcmp(id, "..") == 0) return false;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)id[i];
        if (!(isalnum(c) || c == '-' || c == '_')) return false;
    }
    return true;
}

bool value_ledger_summary(const char *run_id, char *out, size_t out_len) {
    if (!out || out_len == 0) return false;
    const char *id = (run_id && run_id[0]) ? run_id : chronicle_run_id();
    if (!valid_run_id(id)) {
        snprintf(out, out_len, "{\"error\":\"invalid or missing run_id\"}");
        return false;
    }

    char path[PATH_MAX];
    const char *root = getenv("DSCO_RUNS_DIR");
    if (!root || !root[0]) {
        const char *home = getenv("HOME");
        snprintf(path, sizeof(path), "%s/.dsco/runs/%s/journal.wal", home && home[0] ? home : ".", id);
    } else {
        snprintf(path, sizeof(path), "%s/%s/journal.wal", root, id);
    }

    FILE *fp = fopen(path, "rb");
    if (!fp) { snprintf(out, out_len, "{\"receipts\":0,\"note\":\"no journal yet\"}"); return true; }

    unsigned long long n = 0, verified = 0, autonomous = 0, incidents = 0, recoveries = 0, reused = 0;
    double vwv = 0, cost = 0, human_min = 0;
    unsigned char hdr[8];
    while (fread(hdr, 1, sizeof(hdr), fp) == sizeof(hdr)) {
        uint32_t len = read_u32le(hdr);
        if (len == 0 || len > 16U * 1024U * 1024U) break;
        char *rec = malloc((size_t)len + 1);
        if (!rec || fread(rec, 1, len, fp) != len) { free(rec); break; }
        rec[len] = '\0';
        if (strstr(rec, "\"" RECEIPT_TYPE "\"") || strstr(rec, "value.receipt.v1")) {
            n++;
            bool passed = strstr(rec, "\"passed\":true") != NULL;
            if (passed) verified++;
            if (strstr(rec, "\"autonomous\":true")) autonomous++;
            if (strstr(rec, "\"incident\":true")) incidents++;
            if (strstr(rec, "\"recovery_ms\":") && !strstr(rec, "\"recovery_ms\":0.000")) recoveries++;
            if (!strstr(rec, "\"reuse\":{}")) reused++;
            /* Verified value excludes failed/unverified work. Costs and human
             * time include every attempt, so failures remain visible in margin
             * and human leverage rather than disappearing from the denominator. */
            if (passed) vwv += frame_num(rec, "price_usd");
            cost += frame_num(rec, "compute_cost_usd");
            human_min += frame_num(rec, "human_minutes");
        }
        free(rec);
    }
    fclose(fp);

    double acr = n ? (double)autonomous / (double)n : 0;
    double gcm = vwv > 0 ? (vwv - cost) / vwv : 0;
    double rr = incidents ? (double)recoveries / (double)incidents : (recoveries ? 1.0 : 0);
    double reuse_rate = n ? (double)reused / (double)n : 0;
    double human_hours = human_min / 60.0;
    double hl = human_hours > 0 ? vwv / human_hours : 0;

    jbuf_t b; jbuf_init(&b, 512);
    jbuf_appendf(&b, "{\"run_id\":\"%s\",\"receipts\":%llu,\"verified\":%llu", id, n, verified);
    jbuf_appendf(&b, ",\"verified_work_value_usd\":%.4f", vwv);
    jbuf_appendf(&b, ",\"compute_cost_usd\":%.4f", cost);
    jbuf_appendf(&b, ",\"gross_compute_margin\":%.4f", gcm);
    jbuf_appendf(&b, ",\"autonomous_completion_rate\":%.4f", acr);
    jbuf_appendf(&b, ",\"recovery_rate\":%.4f", rr);
    jbuf_appendf(&b, ",\"capability_reuse_rate\":%.4f", reuse_rate);
    jbuf_appendf(&b, ",\"human_leverage_usd_per_hr\":%.4f", hl);
    jbuf_appendf(&b, ",\"incidents\":%llu}", incidents);
    snprintf(out, out_len, "%s", b.data ? b.data : "{}");
    jbuf_free(&b);
    return true;
}
