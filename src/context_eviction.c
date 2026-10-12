#include "context_eviction.h"
#include "context_archive.h"
#include "tool_integrity_index.h"
#include "context_fabric.h"
#include "jev_compaction.h"
#include "chronicle.h"
#include "json_util.h"
#include "../vendor/yyjson.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool is_type(const msg_content_t *c, const char *type) {
    return c->type && !strcmp(c->type, type);
}

/* Detect an older complete exchange whose ID was reused by a later call.
 * Archiving it repairs the wire graph even if it is below min_bytes. */
static bool pair_has_reused_id(const conversation_t *conv, int at, int span) {
    const message_t *call = &conv->msgs[at];
    for (int j = 0; j < call->content_count; j++) {
        const msg_content_t *block = &call->content[j];
        if (!is_type(block, "tool_use") || !block->tool_id) continue;
        for (int i = at + span; i < conv->count; i++) {
            const message_t *other = &conv->msgs[i];
            for (int k = 0; k < other->content_count; k++)
                if (is_type(&other->content[k], "tool_use") &&
                    other->content[k].tool_id &&
                    !strcmp(block->tool_id, other->content[k].tool_id)) return true;
        }
    }
    return false;
}

/* Validate only the exchange we are about to replace. A damaged or still-live
 * unrelated turn must not make every older, independently complete exchange
 * unevictable. A later complete pair may reuse a legacy ID; removing the older
 * pair resolves that duplicate, while earlier uses and orphan results fail
 * closed. */
static bool pair_is_complete_and_safe(const conversation_t *conv, int at, int span, bool globally_unique) {
    if (!conv || at < 0 || at + 1 >= conv->count) return false;
    const message_t *a = &conv->msgs[at], *b = &conv->msgs[at + 1];
    if (a->role != ROLE_ASSISTANT || b->role != ROLE_USER ||
        a->content_count <= 0 || b->content_count <= 0) return false;
    int calls = 0, results = 0;
    for (int j = 0; j < a->content_count; j++) {
        const msg_content_t *c = &a->content[j];
        if (is_type(c, "text") || msg_content_is_codex_reasoning(c)) continue;
        if (!is_type(c, "tool_use") || !c->tool_id || !c->tool_id[0]) return false;
        for (int k = 0; k < j; k++) {
            const msg_content_t *prior = &a->content[k];
            if (is_type(prior, "tool_use") && prior->tool_id &&
                !strcmp(prior->tool_id, c->tool_id)) return false;
        }
        calls++;
    }
    if (!calls) return false;
    for (int ri = at + 1; ri < at + span; ri++)
    for (int j = 0; j < conv->msgs[ri].content_count; j++) {
        const msg_content_t *c = &conv->msgs[ri].content[j];
        if (!is_type(c, "tool_result") || !c->tool_id || !c->tool_id[0]) return false;
        bool matched = false;
        for (int k = 0; k < a->content_count; k++) {
            const msg_content_t *call = &a->content[k];
            if (is_type(call, "tool_use") && call->tool_id &&
                !strcmp(call->tool_id, c->tool_id)) {
                if (matched) return false;
                matched = true;
            }
        }
        if (!matched) return false;
        results++;
    }
    if (results != calls) return false;
    /* Matching counts are insufficient: A,A must not stand in for A,B. */
    for (int j = 0; j < a->content_count; j++) {
        const msg_content_t *call = &a->content[j];
        if (!is_type(call,"tool_use")) continue;
        int matches = 0;
        for (int ri = at+1; ri < at+span; ri++)
            for (int k = 0; k < conv->msgs[ri].content_count; k++)
                matches += !strcmp(call->tool_id,conv->msgs[ri].content[k].tool_id);
        if (matches != 1) return false;
    }
    if(globally_unique) return true;
    /* A later complete pair may legitimately reuse a legacy synthetic ID.
     * Removing this older pair resolves that duplicate. An earlier use or an
     * orphan result with the same ID is not safe to splice around. */
    for (int j = 0; j < a->content_count; j++) {
        const msg_content_t *call = &a->content[j];
        if (!is_type(call, "tool_use") || !call->tool_id) continue;
        for (int i = 0; i < conv->count; i++) {
            if (i >= at && i < at + span) continue;
            const message_t *m = &conv->msgs[i];
            for (int k = 0; k < m->content_count; k++) {
                const msg_content_t *other = &m->content[k];
                if (!other->tool_id || strcmp(other->tool_id, call->tool_id)) continue;
                if (is_type(other, "tool_use") && i < at + 1) return false;
                if (!is_type(other, "tool_result")) continue;
                bool external_use = false;
                for (int u = 0; u < conv->count && !external_use; u++) {
                    if (u == at) continue;
                    const message_t *um = &conv->msgs[u];
                    for (int v = 0; v < um->content_count; v++) {
                        const msg_content_t *uc = &um->content[v];
                        if (is_type(uc, "tool_use") && uc->tool_id &&
                            !strcmp(uc->tool_id, call->tool_id) && u < i) {
                            external_use = true;
                            break;
                        }
                    }
                }
                if (!external_use) return false;
            }
        }
    }
    return true;
}

/* A receipt run is itself an immutable archive index. Folding prose as well
 * as tool receipts prevents keys/previews from becoming another unbounded
 * transcript. Child keys remain traversable after save/reload. */
static int fold_receipts(conversation_t *conv, int keep, int *failures, char *last_key, const char **failure_stage) {
    for (int start = 1; start + 7 < conv->count - keep; start++) {
        int end = start;
        while (end < conv->count - keep) {
            message_t *a = &conv->msgs[end];
            if(end+1<conv->count-keep && context_archive_legacy_receipt(a,&conv->msgs[end+1])) {
                end+=2;continue;
            }
            if (a->role != ROLE_ASSISTANT || a->content_count != 1 ||
                !is_type(&a->content[0],"text") || !a->content[0].tool_name ||
                strcmp(a->content[0].tool_name,"context_evict") || !a->content[0].text) break;
            if (!strncmp(a->content[0].text,"[Archived assistant response; key=",34)) { end++; continue; }
            if (end+1 >= conv->count-keep) break;
            message_t *r = &conv->msgs[end+1];
            if (r->role != ROLE_USER || r->content_count != 1 || !is_type(&r->content[0],"text") ||
                !r->content[0].text || strncmp(r->content[0].text,"[Archived completed tool exchange;",34)) break;
            end += 2;
        }
        if (end-start < 8) continue;
        char *archive = context_archive_serialize(conv->msgs+start,end-start);
        if (!archive) { *failure_stage="serialization_failed"; (*failures)++; return 0; }
        size_t bytes = strlen(archive);
        ctxkey_t key; char handle[CTX_KEY_STR_MAX];
        ctx_put_opts_t opts = {.kind=CTX_KIND_MSG,.source="context_receipt_index",.embed=0};
        bool saved=context_archive_store_verified_ex(archive,bytes,&opts,&key,failure_stage);
        if(saved && ctxkey_format(&key,handle,sizeof(handle))<=0){*failure_stage="key_format_failed";saved=false;}
        free(archive);
        if (!saved || !context_archive_replace(conv,start,end-start,handle,false,0,false)) {
            (*failures)++; return 0;
        }
        snprintf(last_key,CTX_KEY_STR_MAX,"%s",handle);
        return end-start-1;
    }
    return 0;
}

bool context_evict(conversation_t *conv, const char *input, char *result, size_t len) {
    if (!result || len < 1536) return false;
    int keep = 6, max_turns = 8, min_bytes = 2048, preview_chars = 0;
    yyjson_doc *doc = yyjson_read(input ? input : "{}", input ? strlen(input) : 2, 0);
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    bool valid = yyjson_is_obj(root);
    unsigned seen = 0;
    size_t i, n; yyjson_val *key, *value;
    if (valid) yyjson_obj_foreach(root, i, n, key, value) {
        unsigned bit = yyjson_equals_str(key, "keep_recent") ? 1 :
                       yyjson_equals_str(key, "max_turns") ? 2 :
                       yyjson_equals_str(key, "min_bytes") ? 4 :
                       yyjson_equals_str(key, "preview_chars") ? 8 : 0;
        if (!bit || (seen & bit) || !yyjson_is_uint(value) || yyjson_get_uint(value) > 1000000) {
            valid = false; continue;
        }
        seen |= bit;
        int v = (int)yyjson_get_uint(value);
        if (bit == 1) { if (v < 2) valid = false; else keep = v; }
        if (bit == 2) { if (v < 1 || v > 64) valid = false; else max_turns = v; }
        if (bit == 4) { if (v < 512) valid = false; else min_bytes = v; }
        if (bit == 8) { if (v < 0 || v > 8192) valid = false; else preview_chars = v; }
    }
    yyjson_doc_free(doc);
    if (!valid || !conv) {
        snprintf(result, len, "{\"error\":\"context_evict requires an active conversation and integer options: keep_recent 2..1000000, max_turns 1..64, min_bytes 512..1000000, preview_chars 0..8192\"}");
        return false;
    }
    tool_integrity_result_t initial_integrity = conv_validate_tool_call_integrity(conv, true);
    bool globally_unique=initial_integrity.ok && tool_integrity_index_unique_exchanges(conv);
    /* Validate candidates locally below. A pending or malformed unrelated
     * turn is retained, but must not prevent archival of older complete pairs.
     * This is important during live parallel batches: the current assistant
     * message can legitimately be incomplete while old work is reclaimable. */
    int before = conv_rough_estimate(conv), evicted = 0, failures = 0, repairs = 0;
    int scanned = 0, shape_ineligible = 0, unsafe = 0, below_min = 0;
    int compact_failures = 0, eligible_pairs = 0;
    char last_key[CTX_KEY_STR_MAX] = "";
    const char *failure_stage="none";
    jev_compaction_candidate_t candidates[JEV_COMPACTION_MAX] = {0};
    size_t candidate_count = 0;
    for (int at = 1; at + 1 < conv->count - keep && evicted < max_turns; at++) {
        scanned++;
        message_t *a = &conv->msgs[at], *b = &conv->msgs[at + 1];
        if (a->role != ROLE_ASSISTANT || b->role != ROLE_USER || !b->content_count) {
            shape_ineligible++;
            continue;
        }
        bool eligible = true, has_call = false;
        for (int j = 0; j < a->content_count; j++) {
            const msg_content_t *c = &a->content[j];
            has_call |= is_type(c, "tool_use");
            if (!is_type(c, "text") && !is_type(c, "tool_use") &&
                !msg_content_is_codex_reasoning(c)) eligible = false;
        }
        /* Parallel tool results may arrive in several consecutive user
         * messages. A real user block or protected boundary ends the span. */
        int end = at + 1;
        while (end < conv->count - keep) {
            const message_t *r = &conv->msgs[end];
            bool results_only = r->role == ROLE_USER && r->content_count > 0;
            for (int j = 0; j < r->content_count; j++)
                results_only &= is_type(&r->content[j],"tool_result");
            if (!results_only) break;
            end++;
        }
        int span = end - at;
        if (!eligible || !has_call || span < 2) { shape_ineligible++; continue; }
        if (!pair_is_complete_and_safe(conv, at, span, globally_unique)) { unsafe++; continue; }
        eligible_pairs++;
        char *archive = context_archive_serialize(a, span);
        if (!archive) { failure_stage="serialization_failed"; failures++; break; }
        size_t bytes = strlen(archive);
        bool repair = !initial_integrity.ok && pair_has_reused_id(conv, at, span);
        if (bytes < (size_t)min_bytes && !repair) {
            below_min++;
            free(archive);
            continue;
        }
        jev_compaction_candidate_t *candidate = NULL;
        if (bytes >= 512 && candidate_count < JEV_COMPACTION_MAX) {
            candidate = &candidates[candidate_count++];
            candidate->age_messages = conv->count - at - span;
            candidate->archive_bytes = bytes;
            for (int ri = at+1; ri < end; ri++) candidate->results += conv->msgs[ri].content_count;
            for (int j = 0; j < a->content_count; j++)
                candidate->calls += is_type(&a->content[j], "tool_use");
        }
        ctx_put_opts_t opts = {.kind = CTX_KIND_TOOL, .source = "context_evict", .embed = 0};
        ctxkey_t stored;
        char handle[CTX_KEY_STR_MAX];
        bool saved=context_archive_store_verified_ex(archive,bytes,&opts,&stored,&failure_stage);
        if(saved && ctxkey_format(&stored,handle,sizeof(handle))<=0){failure_stage="key_format_failed";saved=false;}
        free(archive);
        if (!saved) { failures++; break; }
        context_archive_replace_result_t replaced = context_archive_replace(conv,at,span,handle,true,preview_chars,repair);
        if (replaced == CONTEXT_ARCHIVE_NOT_REDUCING) { below_min++; continue; }
        if (replaced == CONTEXT_ARCHIVE_FAILED) {
            failure_stage="receipt_replacement_failed"; failures++; compact_failures++; break;
        }
        snprintf(last_key, sizeof(last_key), "%s", handle);
        if (candidate) candidate->archived = true;
        evicted++; at++;
        if (repair) repairs++;
    }
    /* Tool-only archival leaves long text conversations untouched. Archive
     * old assistant prose as a separately retrievable message, but never a
     * user instruction, a tool block, or anything inside the protected tail.
     * A verified write/readback is mandatory before changing live history. */
    int prose_evicted = 0;
    for (int at = 1; at < conv->count - keep && evicted + prose_evicted < max_turns && !failures; at++) {
        message_t *m = &conv->msgs[at];
        if (m->role != ROLE_ASSISTANT || m->content_count < 1) continue;
        bool prose = true;
        for (int j = 0; j < m->content_count; j++) {
            const msg_content_t *c = &m->content[j];
            if ((!is_type(c,"text") && !msg_content_is_codex_reasoning(c)) || c->tool_name ||
                (c->text && !strncmp(c->text,"[Archived assistant response; key=",34))) prose=false;
        }
        /* Keep a potential legacy receipt pair associated until its exact
         * archived provenance has been checked by the receipt folder. */
        if(prose && at+1<conv->count-keep) {
            message_t *next=&conv->msgs[at+1];
            if(next->role==ROLE_USER && next->content_count==1 && next->content[0].text &&
               !strncmp(next->content[0].text,"[Archived completed tool exchange;",34))prose=false;
        }
        if (!prose) continue;
        char *archive = context_archive_serialize(m, 1);
        if (!archive) { failure_stage="serialization_failed"; failures++; break; }
        size_t bytes = strlen(archive);
        if (bytes < (size_t)min_bytes) { free(archive); continue; }
        ctx_put_opts_t opts = {.kind = CTX_KIND_MSG, .source = "context_evict", .embed = 0};
        ctxkey_t stored;
        char handle[CTX_KEY_STR_MAX];
        bool saved=context_archive_store_verified_ex(archive,bytes,&opts,&stored,&failure_stage);
        if(saved && ctxkey_format(&stored,handle,sizeof(handle))<=0){failure_stage="key_format_failed";saved=false;}
        free(archive);
        if (!saved) { failures++; break; }
        context_archive_replace_result_t replaced = context_archive_replace(conv,at,1,handle,false,preview_chars,false);
        if (replaced == CONTEXT_ARCHIVE_NOT_REDUCING) { below_min++; continue; }
        if (replaced == CONTEXT_ARCHIVE_FAILED) {
            failure_stage="receipt_replacement_failed"; failures++; break;
        }
        snprintf(last_key, sizeof(last_key), "%s", handle);
        prose_evicted++;
    }
    int folded = failures ? 0 : fold_receipts(conv, keep, &failures, last_key, &failure_stage);
    tool_integrity_result_t after_integrity = conv_validate_tool_call_integrity(conv, true);
    if (!after_integrity.ok && !evicted && !prose_evicted && !folded && !failures) {
        snprintf(result, len, "{\"error\":\"unresolved tool protocol cannot be safely evicted in the selected range; history retained\"}");
        return false;
    }
    const char *no_op_reason = failures ? "archive_failure" :
        (evicted || prose_evicted || folded) ? "changed" : compact_failures ? "compact_failure" :
        below_min ? "below_min_bytes" : unsafe ? "unsafe_or_incomplete" :
        shape_ineligible ? "no_complete_tool_exchange" : "protected_tail_or_empty";
    snprintf(result, len,
             "{\"changed\":%s,\"turns_evicted\":%d,\"archive_failures\":%d,\"last_archive_key\":\"%s\",\"keep_recent\":%d,"
             "\"estimated_tokens_before\":%d,\"estimated_tokens_after\":%d,"
             "\"retrieval\":\"context_recall\",\"semantic_summary\":false,\"protocol_complete\":%s,\"integrity_repair_archives\":%d,"
             "\"no_op_reason\":\"%s\",\"pairs_scanned\":%d,\"eligible_pairs\":%d,"
             "\"shape_ineligible\":%d,\"unsafe_or_incomplete\":%d,\"below_min_bytes\":%d,"
             "\"compact_failures\":%d,\"protected_messages\":%d,\"receipts_folded\":%d,\"assistant_messages_archived\":%d,\"archive_failure_stage\":\"%s\"}",
             (evicted || prose_evicted || folded) ? "true" : "false", evicted, failures, last_key, keep, before,
             conv_rough_estimate(conv), after_integrity.ok ? "true" : "false", repairs,
             no_op_reason, scanned, eligible_pairs, shape_ineligible, unsafe, below_min,
             compact_failures, conv->count < keep ? conv->count : keep, folded, prose_evicted, failure_stage);
    const char *jev_mode = getenv("DSCO_JEV_COMPACTION");
    if (jev_compaction_mode_enabled(jev_mode)) {
        char shadow[512];
        jev_compaction_shadow(candidates, candidate_count, shadow, sizeof(shadow));
        chronicle_event("experiment.jev_compaction", NULL, NULL, NULL,
                        "runtime", jev_mode, shadow, "product_telemetry");
        size_t used = strlen(result);
        if (used && result[used - 1] == '}') {
            const char *receipt = used + strlen(shadow) + 17 < len ? shadow :
                                  "{\"status\":\"receipt_omitted\"}";
            if (used + strlen(receipt) + 17 < len)
                snprintf(result + used - 1, len - used + 1, ",\"jev_shadow\":%s}", receipt);
        }
    }
    return failures == 0;
}

bool context_archive_recall(const char *key, char *result, size_t len) {
    if (!result || len < 512) return false;
    ctxkey_t parsed;
    if (!ctxkey_parse(key, &parsed)) {
        snprintf(result, len, "error: invalid context archive key"); return false;
    }
    return context_archive_page(ctx_broker_default(),&parsed,result,len);
}
