#include "context_eviction.h"
#include "context_fabric.h"
#include "json_util.h"
#include "../vendor/yyjson.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool is_type(const msg_content_t *c, const char *type) {
    return c->type && !strcmp(c->type, type);
}

static char *archive_pair(const message_t *a, const message_t *b) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (!doc) return NULL;
    yyjson_mut_val *root = yyjson_mut_arr(doc);
    yyjson_mut_doc_set_root(doc, root);
    const message_t *messages[] = {a, b};
    for (int i = 0; i < 2; i++) {
        yyjson_mut_val *m = yyjson_mut_obj(doc), *content = yyjson_mut_arr(doc);
        yyjson_mut_obj_add_str(doc, m, "role", i ? "user" : "assistant");
        yyjson_mut_obj_add_val(doc, m, "content", content);
        yyjson_mut_arr_add_val(root, m);
        for (int j = 0; j < messages[i]->content_count; j++) {
            const msg_content_t *c = &messages[i]->content[j];
            yyjson_mut_val *block = yyjson_mut_obj(doc);
            const char *keys[] = {"type", "text", "tool_name", "tool_id", "tool_input"};
            const char *values[] = {c->type, c->text, c->tool_name, c->tool_id, c->tool_input};
            for (int k = 0; k < 5; k++)
                if (values[k]) yyjson_mut_obj_add_str(doc, block, keys[k], values[k]);
            yyjson_mut_arr_add_val(content, block);
        }
    }
    char *json = yyjson_mut_write(doc, 0, NULL);
    yyjson_mut_doc_free(doc);
    return json;
}

bool context_evict(conversation_t *conv, const char *input, char *result, size_t len) {
    if (!result || len < 768) return false;
    int keep = 6, max_turns = 8, min_bytes = 2048;
    yyjson_doc *doc = yyjson_read(input ? input : "{}", input ? strlen(input) : 2, 0);
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    bool valid = yyjson_is_obj(root);
    unsigned seen = 0;
    size_t i, n; yyjson_val *key, *value;
    if (valid) yyjson_obj_foreach(root, i, n, key, value) {
        unsigned bit = yyjson_equals_str(key, "keep_recent") ? 1 :
                       yyjson_equals_str(key, "max_turns") ? 2 :
                       yyjson_equals_str(key, "min_bytes") ? 4 : 0;
        if (!bit || (seen & bit) || !yyjson_is_uint(value) || yyjson_get_uint(value) > 1000000) {
            valid = false; continue;
        }
        seen |= bit;
        int v = (int)yyjson_get_uint(value);
        if (bit == 1) { if (v < 2) valid = false; else keep = v; }
        if (bit == 2) { if (v < 1 || v > 64) valid = false; else max_turns = v; }
        if (bit == 4) { if (v < 512) valid = false; else min_bytes = v; }
    }
    yyjson_doc_free(doc);
    if (!valid || !conv) {
        snprintf(result, len, "{\"error\":\"context_evict requires an active conversation and integer options: keep_recent 2..1000000, max_turns 1..64, min_bytes 512..1000000\"}");
        return false;
    }
    if (!conv_validate_tool_call_integrity(conv, true).ok) {
        snprintf(result, len, "{\"error\":\"tool protocol is incomplete; history retained\"}");
        return false;
    }
    int before = conv_rough_estimate(conv), evicted = 0, failures = 0;
    char last_key[CTX_KEY_STR_MAX] = "";
    for (int at = 0; at + 1 < conv->count - keep && evicted < max_turns; at++) {
        message_t *a = &conv->msgs[at], *b = &conv->msgs[at + 1];
        if (a->role != ROLE_ASSISTANT || b->role != ROLE_USER || !b->content_count) continue;
        bool eligible = true, has_call = false;
        for (int j = 0; j < a->content_count; j++) {
            const msg_content_t *c = &a->content[j];
            has_call |= is_type(c, "tool_use");
            if (!is_type(c, "text") && !is_type(c, "tool_use")) eligible = false;
        }
        for (int j = 0; j < b->content_count; j++)
            if (!is_type(&b->content[j], "tool_result")) eligible = false;
        if (!eligible || !has_call) continue;
        /* Validate the entire pair, including parallel calls, before archiving.
         * Split/missing outputs are left intact for a later complete exchange. */
        conversation_t pair = {.msgs = a, .count = 2, .cap = 2};
        if (!conv_validate_tool_call_integrity(&pair, false).ok) continue;
        char *archive = archive_pair(a, b);
        if (!archive) { failures++; break; }
        size_t bytes = strlen(archive);
        if (bytes < (size_t)min_bytes) { free(archive); continue; }
        ctx_broker_t *broker = ctx_broker_default();
        ctx_put_opts_t opts = {.kind = CTX_KIND_TOOL, .source = "context_evict", .embed = 0};
        ctxkey_t stored;
        char handle[CTX_KEY_STR_MAX];
        bool saved = broker && ctx_put(broker, archive, bytes, &opts, &stored, NULL) &&
                     ctxkey_format(&stored, handle, sizeof(handle)) > 0;
        size_t read_bytes = 0;
        char *check = saved ? ctx_get(broker, &stored, &read_bytes) : NULL;
        saved = check && read_bytes == bytes && !memcmp(check, archive, bytes);
        free(check); free(archive);
        if (!saved) { failures++; break; }
        /* This primitive changes only these two message contents, never the
         * array/count. Its zero tail applies to this already-protected view. */
        if (!conv_compact_recent_tool_turn(&pair, 384, 0)) continue;
        msg_content_t *excerpt = &b->content[0];
        jbuf_t replacement; jbuf_init(&replacement, 768);
        jbuf_appendf(&replacement, "[Archived completed tool exchange; key=%s; retrieve with context_recall, optionally #b:0-4096]\n", handle);
        jbuf_append(&replacement, excerpt->text ? excerpt->text : "");
        free(excerpt->text); excerpt->text = replacement.data;
        snprintf(last_key, sizeof(last_key), "%s", handle);
        evicted++; at++;
    }
    snprintf(result, len,
             "{\"changed\":%s,\"turns_evicted\":%d,\"archive_failures\":%d,\"last_archive_key\":\"%s\",\"keep_recent\":%d,"
             "\"estimated_tokens_before\":%d,\"estimated_tokens_after\":%d,"
             "\"retrieval\":\"context_recall\",\"semantic_summary\":false}",
             evicted ? "true" : "false", evicted, failures, last_key, keep, before,
             conv_rough_estimate(conv));
    return failures == 0;
}

bool context_archive_recall(const char *key, char *result, size_t len) {
    if (!result || len < 512) return false;
    ctxkey_t parsed;
    if (!ctxkey_parse(key, &parsed)) {
        snprintf(result, len, "error: invalid context archive key"); return false;
    }
    size_t bytes = 0;
    char *text = ctx_get(ctx_broker_default(), &parsed, &bytes);
    if (!text) { snprintf(result, len, "error: context archive not found"); return false; }
    snprintf(result, len, "[key=%s bytes=%zu; use #b:START-END slices for bounded retrieval]\n", key, bytes);
    size_t off = strlen(result), copy = bytes < len - off - 1 ? bytes : len - off - 1;
    while (copy && copy < bytes && ((unsigned char)text[copy] & 0xc0) == 0x80) copy--;
    memcpy(result + off, text, copy); result[off + copy] = '\0';
    free(text);
    return true;
}
