#include "context_compaction.h"
#include "task_closeout.h"
#include "tools.h"
#include "json_util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool context_compaction_run(conversation_t *conv, const char *input, const char *tier,
                            char *result, size_t len) {
    if (!result || len < 2048) return false;
    task_compact_options_t options;
    if (!task_closeout_compact_options(input,&options,result,len)) return false;
    if (!conv || !tier) {
        snprintf(result,len,"{\"error\":\"no active conversation/session for compaction\"}");
        return false;
    }
    int before_messages = conv->count, before_tokens = conv_rough_estimate(conv);
    int passes = 0, tool_archives = 0, prose_archives = 0, folded = 0;
    bool archived = false, archive_ok = true;
    char key[256] = "", archive[2048] = "";
    int preview_limit = options.max_result_chars;
    const char *stop_reason = "no_eligible_old_content";
    {
        /* A pass visits at most 64 candidates. Continue only while actual
         * history shrinks, terminating on actual progress instead of a fixed
         * exchange count or the model's automatic trigger. Each archived message
         * becomes a receipt that context_evict does not archive again. */
        for (;;) {
            int count = conv->count, tokens = conv_rough_estimate(conv);
            char args[128];
            snprintf(args,sizeof(args),"{\"keep_recent\":%d,\"max_turns\":64,\"min_bytes\":%d,\"preview_chars\":%d}",
                     options.keep_recent,options.aggressive?512:2048,preview_limit);
            archive_ok = tools_execute_for_tier("context_evict",args,tier,archive,sizeof(archive));
            ++passes;
            bool changed = json_get_bool(archive,"changed",false);
            archived |= changed;
            tool_archives += json_get_int(archive,"turns_evicted",0);
            prose_archives += json_get_int(archive,"assistant_messages_archived",0);
            folded += json_get_int(archive,"receipts_folded",0);
            char *last = json_get_str(archive,"last_archive_key");
            if (last && last[0]) snprintf(key,sizeof(key),"%s",last);
            free(last);
            if (!archive_ok) { stop_reason="archive_denied_or_failed"; break; }
            if (!changed) { stop_reason="no_eligible_old_content"; break; }
            if (conv->count >= count && conv_rough_estimate(conv) >= tokens) {
                stop_reason="no_further_reduction"; break;
            }
        }
    }
    /* Retain ineligible/live evidence verbatim. No lossy trimming fallback. */
    bool changed = archived;
    jbuf_t out; jbuf_init(&out,2048);
    jbuf_appendf(&out,"{\"changed\":%s,\"messages_before\":%d,\"messages_after\":%d,"
        "\"estimated_tokens_before\":%d,\"estimated_tokens_after\":%d,"
        "\"archive_passes\":%d,\"tool_exchanges_archived\":%d,\"assistant_messages_archived\":%d,"
        "\"receipts_folded\":%d,\"archive_verified\":%s,\"keep_recent\":%d,"
        "\"aggressive\":%s,\"semantic_summary\":false,\"retrieval\":\"context_recall\","
        "\"last_archive_key\":",
        changed?"true":"false",before_messages,conv->count,before_tokens,conv_rough_estimate(conv),
        passes,tool_archives,prose_archives,folded,archived && archive_ok?"true":"false",options.keep_recent,
        options.aggressive?"true":"false");
    jbuf_append_json_str(&out,key);
    jbuf_appendf(&out,",\"preview_limit\":%d,\"lossless_archive\":true,\"stop_reason\":",preview_limit);
    jbuf_append_json_str(&out,stop_reason);
    if (!archive_ok) {
        jbuf_append(&out,",\"error\":\"compaction stopped; archive denial or failure; unarchived history retained\",\"detail\":");
        jbuf_append_json_str(&out,archive);
    } else {
        jbuf_append(&out,",\"archive_receipt\":"); jbuf_append(&out,archive);
        if (!changed) {
            char *reason = json_get_str(archive,"no_op_reason");
            jbuf_append(&out,",\"no_op_reason\":");
            jbuf_append_json_str(&out,reason?reason:"no_eligible_old_content"); free(reason);
        }
    }
    jbuf_append(&out,"}");
    if (!out.data || out.len >= len) {
        snprintf(result,len,"{\"changed\":%s,\"error\":\"compaction receipt exceeds output capacity\"}",changed?"true":"false");
        jbuf_free(&out); return false;
    }
    memcpy(result,out.data,out.len+1); jbuf_free(&out); return archive_ok;
}
