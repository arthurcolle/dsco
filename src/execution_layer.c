#include "execution_layer.h"

#include "baseline.h"
#include "crypto.h"
#include "json_util.h"
#include "tools.h"

#include <stdio.h>
#include <string.h>
#include <sys/time.h>

static execution_receipt_t g_last_receipt;

static double exec_now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec * 1000.0 + (double)tv.tv_usec / 1000.0;
}

const char *execution_effect_name(execution_effect_t effect) {
    switch (effect) {
    case EXEC_EFFECT_READ:
        return "read";
    case EXEC_EFFECT_WRITE_FILE:
        return "write_file";
    case EXEC_EFFECT_SHELL:
        return "shell";
    case EXEC_EFFECT_NETWORK:
        return "network";
    case EXEC_EFFECT_PROCESS:
        return "process";
    case EXEC_EFFECT_EXTERNAL_WRITE:
        return "external_write";
    case EXEC_EFFECT_RUNTIME_CONTROL:
        return "runtime_control";
    case EXEC_EFFECT_UNKNOWN:
    default:
        return "unknown";
    }
}

const char *execution_status_name(execution_status_t status) {
    switch (status) {
    case EXEC_STATUS_PENDING:
        return "pending";
    case EXEC_STATUS_DENIED:
        return "denied";
    case EXEC_STATUS_EXECUTED:
        return "executed";
    case EXEC_STATUS_FAILED:
        return "failed";
    default:
        return "unknown";
    }
}

static void receipt_init(execution_receipt_t *r, const execution_intent_t *intent) {
    memset(r, 0, sizeof(*r));
    uuid_v4(r->execution_id);
    snprintf(r->tool_name, sizeof(r->tool_name), "%s",
             intent && intent->tool_name[0] ? intent->tool_name : "");
    snprintf(r->tier, sizeof(r->tier), "%s",
             intent && intent->tier[0] ? intent->tier : "standard");
    r->effect = intent ? intent->effect : EXEC_EFFECT_UNKNOWN;
    r->status = EXEC_STATUS_PENDING;
}

static void receipt_copy_out(const execution_receipt_t *src, execution_receipt_t *dst) {
    if (src)
        g_last_receipt = *src;
    if (src && dst)
        *dst = *src;
}

static void receipt_log(const execution_receipt_t *r) {
    if (!r)
        return;
    jbuf_t detail;
    jbuf_init(&detail, 384);
    jbuf_append(&detail, "{\"execution_id\":");
    jbuf_append_json_str(&detail, r->execution_id);
    jbuf_append(&detail, ",\"tool\":");
    jbuf_append_json_str(&detail, r->tool_name);
    jbuf_append(&detail, ",\"tier\":");
    jbuf_append_json_str(&detail, r->tier);
    jbuf_append(&detail, ",\"effect\":");
    jbuf_append_json_str(&detail, execution_effect_name(r->effect));
    jbuf_append(&detail, ",\"status\":");
    jbuf_append_json_str(&detail, execution_status_name(r->status));
    jbuf_append(&detail, ",\"allowed\":");
    jbuf_append(&detail, r->allowed ? "true" : "false");
    jbuf_append(&detail, ",\"executed\":");
    jbuf_append(&detail, r->executed ? "true" : "false");
    jbuf_append(&detail, ",\"verified\":");
    jbuf_append(&detail, r->verified ? "true" : "false");
    jbuf_append(&detail, ",\"denied_pass\":");
    jbuf_append_json_str(&detail, r->denied_pass);
    jbuf_append(&detail, ",\"reason\":");
    jbuf_append_json_str(&detail, r->denial_reason);
    jbuf_append(&detail, ",\"elapsed_ms\":");
    jbuf_appendf(&detail, "%.1f}", r->elapsed_ms);
    baseline_log("execution", execution_status_name(r->status), r->tool_name, detail.data);
    jbuf_free(&detail);
}

static bool deny_receipt(execution_receipt_t *r, execution_receipt_t *out,
                         char *result, size_t result_len,
                         const char *pass, const char *reason) {
    r->status = EXEC_STATUS_DENIED;
    r->allowed = false;
    snprintf(r->denied_pass, sizeof(r->denied_pass), "%s", pass ? pass : "preflight");
    snprintf(r->denial_reason, sizeof(r->denial_reason), "%s", reason ? reason : "denied");
    if (result && result_len) {
        jbuf_t b;
        jbuf_init(&b, 256);
        jbuf_append(&b, "{\"error\":\"execution_denied\",\"execution_id\":");
        jbuf_append_json_str(&b, r->execution_id);
        jbuf_append(&b, ",\"tool\":");
        jbuf_append_json_str(&b, r->tool_name);
        jbuf_append(&b, ",\"pass\":");
        jbuf_append_json_str(&b, r->denied_pass);
        jbuf_append(&b, ",\"reason\":");
        jbuf_append_json_str(&b, r->denial_reason);
        jbuf_append(&b, "}");
        snprintf(result, result_len, "%s", b.data ? b.data : "{}");
        jbuf_free(&b);
    }
    receipt_log(r);
    receipt_copy_out(r, out);
    return false;
}

bool execution_submit(const execution_intent_t *intent,
                      execution_receipt_t *receipt,
                      char *result,
                      size_t result_len) {
    execution_receipt_t r;
    receipt_init(&r, intent);
    double t0 = exec_now_ms();

    if (!intent || !intent->tool_name[0] || !intent->execute) {
        r.elapsed_ms = exec_now_ms() - t0;
        return deny_receipt(&r, receipt, result, result_len, "intent",
                            "invalid execution intent");
    }

    char err[256];
    err[0] = '\0';
    if (!tools_validate_input(intent->tool_name, intent->input_json, err, sizeof(err))) {
        r.elapsed_ms = exec_now_ms() - t0;
        return deny_receipt(&r, receipt, result, result_len, "preflight", err);
    }

    r.allowed = true;
    r.charged_gsu = intent->estimated_gsu;

    bool ok = intent->execute(intent->tool_name,
                              intent->input_json ? intent->input_json : "{}",
                              intent->tier[0] ? intent->tier : "standard",
                              result,
                              result_len);
    r.executed = true;
    r.verified = ok;
    r.status = ok ? EXEC_STATUS_EXECUTED : EXEC_STATUS_FAILED;
    r.elapsed_ms = exec_now_ms() - t0;

    if (!ok && result && result[0]) {
        snprintf(r.denial_reason, sizeof(r.denial_reason), "%.*s", 220, result);
    }

    receipt_log(&r);
    receipt_copy_out(&r, receipt);
    return ok;
}

bool execution_last_receipt_json(char *out, size_t out_len) {
    if (!out || out_len == 0)
        return false;
    const execution_receipt_t *r = &g_last_receipt;
    if (!r->execution_id[0]) {
        snprintf(out, out_len, "{}");
        return false;
    }
    jbuf_t b;
    jbuf_init(&b, 384);
    jbuf_append(&b, "{\"execution_id\":");
    jbuf_append_json_str(&b, r->execution_id);
    jbuf_append(&b, ",\"tool\":");
    jbuf_append_json_str(&b, r->tool_name);
    jbuf_append(&b, ",\"tier\":");
    jbuf_append_json_str(&b, r->tier);
    jbuf_append(&b, ",\"effect\":");
    jbuf_append_json_str(&b, execution_effect_name(r->effect));
    jbuf_append(&b, ",\"status\":");
    jbuf_append_json_str(&b, execution_status_name(r->status));
    jbuf_append(&b, ",\"allowed\":");
    jbuf_append(&b, r->allowed ? "true" : "false");
    jbuf_append(&b, ",\"executed\":");
    jbuf_append(&b, r->executed ? "true" : "false");
    jbuf_append(&b, ",\"verified\":");
    jbuf_append(&b, r->verified ? "true" : "false");
    jbuf_append(&b, ",\"rolled_back\":");
    jbuf_append(&b, r->rolled_back ? "true" : "false");
    jbuf_append(&b, ",\"denied_pass\":");
    jbuf_append_json_str(&b, r->denied_pass);
    jbuf_append(&b, ",\"denial_reason\":");
    jbuf_append_json_str(&b, r->denial_reason);
    jbuf_append(&b, ",\"charged_gsu\":");
    jbuf_appendf(&b, "%.3f,\"elapsed_ms\":%.3f}", r->charged_gsu, r->elapsed_ms);
    snprintf(out, out_len, "%s", b.data ? b.data : "{}");
    jbuf_free(&b);
    return true;
}
