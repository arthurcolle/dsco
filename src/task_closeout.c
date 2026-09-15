#include "task_closeout.h"
#include "tools.h"
#include "chronicle.h"
#include "json_util.h"
#include "../vendor/yyjson.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool snapshot(task_closeout_t *s) {
    s->count = 0;
    int indices[TASK_CLOSEOUT_SCHEMA_MAX];
    int total = 0;
    const tool_def_t *tools = tools_get_all(&total);
    if (tools_loaded_builtin_count() > TASK_CLOSEOUT_SCHEMA_MAX) return false;
    int n = tools_loaded_builtin_indices(indices, TASK_CLOSEOUT_SCHEMA_MAX);
    for (int i = 0; i < n; i++) {
        if (!tools || indices[i] < 0 || indices[i] >= total) return false;
        snprintf(s->names[s->count++], sizeof(s->names[0]), "%s", tools[indices[i]].name);
    }
    if (tools_loaded_external_count() == 0) return true;
    external_tool_snapshot_t ext = tools_external_snapshot();
    if (!ext.items) return false;
    bool ok = true;
    for (int i = 0; i < ext.count; i++) {
        if (!ext.items[i].loaded) continue;
        if (s->count == TASK_CLOSEOUT_SCHEMA_MAX) { ok = false; break; }
        snprintf(s->names[s->count++], sizeof(s->names[0]), "%s", ext.items[i].name);
    }
    tools_external_snapshot_free(&ext);
    return ok;
}

void task_closeout_begin(task_closeout_t *s) {
    if (!s) return;
    memset(s, 0, sizeof(*s));
    const char *mode = getenv("DSCO_TASK_CLOSEOUT");
    s->enabled = !mode || (strcmp(mode, "0") && strcmp(mode, "off"));
    if (s->enabled) s->valid = snapshot(s);
}

static bool contains(const task_closeout_t *s, const char *name) {
    for (int i = 0; i < s->count; i++)
        if (!strcmp(s->names[i], name)) return true;
    return false;
}

bool task_closeout_finish(task_closeout_t *s, const char *tier,
                         bool terminal, int workers, char *report, size_t len) {
    if (!s || !report || len < 1024 || workers < 0) return false;
    if (!s->enabled) {
        snprintf(report, len, "{\"status\":\"disabled\",\"changed\":false}");
        return true;
    }
    /* Consume the lease exactly once, including deferred/failed closeouts. */
    s->enabled = false;
    task_closeout_t current = {0};
    bool ok = s->valid && snapshot(&current);
    int candidates = 0, evicted = 0;
    bool deferred = !terminal || workers > 0;
    if (ok && !deferred) {
        for (int i = 0; i < current.count; i++) {
            if (contains(s, current.names[i])) continue;
            candidates++;
            /* One name per call bounds failure reports and avoids an all:true
             * reset of unrelated leases. A denial is final; never bypass it. */
            jbuf_t input;
            jbuf_init(&input, 320);
            jbuf_append(&input, "{\"tools\":[");
            jbuf_append_json_str(&input, current.names[i]);
            jbuf_append(&input, "]}");
            char result[2048] = "";
            bool dispatched = tools_execute_for_tier("evict_tools", input.data, tier,
                                                     result, sizeof(result));
            jbuf_free(&input);
            bool still_loaded = tools_is_builtin_loaded(current.names[i]) ||
                                tools_is_external_loaded(current.names[i]);
            if (dispatched && !still_loaded) evicted++;
            else { ok = false; break; }
        }
    }
    const char *status = !ok ? "error" : deferred ? "deferred" : "closed";
    /* Fixed labels and counts only. No prompt, response, tool name, args,
     * result, identity or reward leaks into the learning projection. */
    snprintf(report, len,
             "{\"schema\":\"dsco.task_closeout.v1\",\"status\":\"%s\","
             "\"response_terminal\":%s,\"workers_active\":%d,"
             "\"schemas_before\":%d,\"baseline_schemas\":%d,"
             "\"eviction_attempts\":%d,\"schemas_evicted\":%d,\"changed\":%s,"
             "\"history_compacted\":false,\"summary\":\"final_response_retained\","
             "\"verification\":\"unverified\",\"training_eligible\":false}",
             status, terminal ? "true" : "false", workers, current.count, s->count,
             candidates, evicted, evicted ? "true" : "false");
    /* Respect an absent/disabled journal; do not create another storage sink. */
    if (chronicle_run_id() && chronicle_run_id()[0] &&
        !chronicle_journal_append("task.closeout.v1", report, true)) return false;
    return ok;
}

static size_t result_bytes(const conversation_t *conv) {
    size_t n = 0;
    for (int i = 0; i < conv->count; i++)
        for (int j = 0; j < conv->msgs[i].content_count; j++) {
            const msg_content_t *c = &conv->msgs[i].content[j];
            if (c->type && !strcmp(c->type, "tool_result") && c->text) n += strlen(c->text);
        }
    return n;
}

bool task_closeout_compact(conversation_t *conv, const char *input,
                          char *result, size_t len) {
    if (!result || len < 512) return false;
    int keep = 6, max_chars = 800;
    bool aggressive = false;
    yyjson_doc *doc = yyjson_read(input ? input : "{}", input ? strlen(input) : 2, 0);
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    bool valid = yyjson_is_obj(root);
    unsigned seen = 0;
    size_t i, n;
    yyjson_val *key, *value;
    if (valid) yyjson_obj_foreach(root, i, n, key, value) {
        unsigned bit = 0;
        if (yyjson_equals_str(key, "aggressive")) {
            bit = 1;
            if (!yyjson_is_bool(value)) valid = false;
            else aggressive = yyjson_get_bool(value);
        } else if (yyjson_equals_str(key, "keep_recent") ||
                   yyjson_equals_str(key, "max_result_chars")) {
            bool recent = yyjson_equals_str(key, "keep_recent");
            bit = recent ? 2 : 4;
            /* Reject wrong types and negatives rather than scanning into the
             * following property, overflowing atoi, or reading past the input. */
            if (!yyjson_is_uint(value) || yyjson_get_uint(value) > 1000000) valid = false;
            else if (recent) keep = yyjson_get_uint(value) < 2 ? 2 : (int)yyjson_get_uint(value);
            else max_chars = yyjson_get_uint(value) < 100 ? 100 : (int)yyjson_get_uint(value);
        } else valid = false;
        if (seen & bit) valid = false;
        seen |= bit;
    }
    yyjson_doc_free(doc);
    if (!valid) {
        snprintf(result, len, "{\"error\":\"invalid context_compact options: booleans and integers 0..1000000 only; unknown/duplicate fields rejected\"}");
        return false;
    }
    if (!conv) {
        snprintf(result, len, "{\"error\":\"no active conversation; context_compact requires the agent loop\"}");
        return false;
    }
    int before = conv->count, compacted = 0;
    size_t bytes_before = result_bytes(conv);
    conv_trim_old_results(conv, keep, max_chars);
    if (aggressive)
        while (compacted < 10 && conv_compact_recent_tool_turn(conv, max_chars, keep)) compacted++;
    size_t bytes_after = result_bytes(conv);
    snprintf(result, len,
             "{\"messages_before\":%d,\"messages_after\":%d,\"tool_turns_compacted\":%d,"
             "\"keep_recent\":%d,\"max_result_chars\":%d,\"aggressive\":%s,"
             "\"tool_result_bytes_before\":%zu,\"tool_result_bytes_after\":%zu,"
             "\"changed\":%s,\"semantic_summary\":false}",
             before, conv->count, compacted, keep, max_chars, aggressive ? "true" : "false",
             bytes_before, bytes_after, compacted || bytes_before != bytes_after ? "true" : "false");
    return true;
}
