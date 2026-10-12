#include "context_eviction.h"
#include "context_archive.h"
#include "context_fabric.h"
#include "jev_compaction.h"
#include "json_util.h"
#include "../vendor/yyjson.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* Offline hook fixture: real provider adapter is tested separately. */
static int jev_calls;
void jev_compaction_shadow(const jev_compaction_candidate_t *items, size_t count,
                           char *out, size_t len) {
    jev_calls++;
    if (count) {
        assert(items[0].archive_bytes > 512);
        assert(items[0].calls == 1 && items[0].results == 1);
    }
    snprintf(out, len, "{\"status\":\"fixture\",\"applied\":false}");
}
bool chronicle_event(const char *type, const char *trace, const char *span,
                     const char *parent, const char *actor, const char *id,
                     const char *payload, const char *sensitivity) {
    (void)trace; (void)span; (void)parent; (void)actor; (void)id; (void)sensitivity;
    assert(!strcmp(type, "experiment.jev_compaction") && strstr(payload, "fixture"));
    return true;
}

/* Real conversation mechanics; injectable storage failure. The binary test
 * separately exercises the persistent broker and governed dispatch. */
static char *saved;
static bool storage_ok = true;
static int get_fault, put_calls, fail_after = -1;
ctx_broker_t *ctx_broker_default(void) { return (ctx_broker_t *)1; }
bool ctx_put(ctx_broker_t *b, const void *p, size_t n, const ctx_put_opts_t *o,
             ctxkey_t *key, bool *deduped) {
    (void)b; (void)key; (void)deduped;
    assert(o->embed == 0 && (o->kind == CTX_KIND_TOOL || o->kind == CTX_KIND_MSG));
    ++put_calls;
    if (!storage_ok || (fail_after >= 0 && put_calls > fail_after)) return false;
    key->kind = o->kind;
    free(saved); saved = strndup(p, n); return saved != NULL;
}
int ctxkey_format(const ctxkey_t *k, char *out, size_t n) {
    return snprintf(out, n, "ck:%s:owned-archive", k->kind == CTX_KIND_MSG ? "msg" : "tool");
}
bool ctxkey_parse(const char *s, ctxkey_t *k) {
    memset(k, 0, sizeof(*k));
    if (!strcmp(s, "ck:msg:owned-archive")) { k->kind = CTX_KIND_MSG; return true; }
    if (!strcmp(s,"ck:tool:owned-archive")){k->kind=CTX_KIND_TOOL;return true;}
    return false;
}
char *ctx_get(ctx_broker_t *b, const ctxkey_t *k, size_t *n) {
    (void)b; (void)k;
    if (!saved) return NULL;
    if (get_fault == 1) return NULL;
    *n = strlen(saved);
    char *copy = strdup(saved);
    if (get_fault == 2) (*n)--;
    if (get_fault == 3 && copy) copy[0]='!';
    return copy;
}
static void block(message_t *m, const char *type, const char *id, const char *text) {
    m->content = realloc(m->content, (size_t)(m->content_count + 1) * sizeof(msg_content_t));
    assert(m->content);
    msg_content_t *b = &m->content[m->content_count++]; memset(b, 0, sizeof(*b));
    b->type = strdup(type); b->tool_id = id ? strdup(id) : NULL;
    b->tool_name = id ? strdup("write_file") : NULL;
    if (!strcmp(type, "tool_use")) b->tool_input = strdup(text);
    else b->text = strdup(text);
}
static conversation_t fixture(void) {
    conversation_t c = {.count = 6, .cap = 6, .msgs = calloc(6, sizeof(message_t))};
    assert(c.msgs);
    c.msgs[0].role = ROLE_USER; block(&c.msgs[0], "text", NULL, "KEEP_USER_OBJECTIVE");
    char args[30000]; memset(args, 'x', sizeof(args) - 1); args[sizeof(args) - 1] = 0;
    c.msgs[1].role = ROLE_ASSISTANT; block(&c.msgs[1], "tool_use", "old-call", args);
    c.msgs[2].role = ROLE_USER; block(&c.msgs[2], "tool_result", "old-call", "OLD_RESULT_VERIFIED");
    c.msgs[3].role = ROLE_USER; block(&c.msgs[3], "text", NULL, "KEEP_CORRECTION");
    c.msgs[4].role = ROLE_ASSISTANT; block(&c.msgs[4], "tool_use", "latest-call", args);
    c.msgs[5].role = ROLE_USER; block(&c.msgs[5], "tool_result", "latest-call", "KEEP_LATEST_EVIDENCE");
    return c;
}
int main(void) {
    unsetenv("DSCO_JEV_COMPACTION");
    char out[4096];
    conversation_t c = fixture();
    assert(context_evict(&c, "{}", out, sizeof(out)) && strstr(out, "\"changed\":false"));
    assert(strstr(out, "\"no_op_reason\":\"protected_tail_or_empty\""));
    assert(context_evict(&c, "{\"keep_recent\":2}", out, sizeof(out)));
    assert(strstr(out, "\"turns_evicted\":1"));
    assert(strstr(saved, "old-call") && strstr(saved, "OLD_RESULT_VERIFIED"));
    assert(!strstr(saved, "latest-call") && !strstr(saved, "KEEP_USER_OBJECTIVE"));
    assert(!strcmp(c.msgs[0].content[0].text, "KEEP_USER_OBJECTIVE"));
    assert(!strcmp(c.msgs[3].content[0].text, "KEEP_CORRECTION"));
    assert(!strcmp(c.msgs[5].content[0].text, "KEEP_LATEST_EVIDENCE"));
    assert(strlen(c.msgs[4].content[0].tool_input) == 29999);
    assert(strstr(c.msgs[2].content[0].text, "ck:tool:owned-archive"));
    assert(conv_validate_tool_call_integrity(&c, false).ok);
    assert(context_archive_recall("ck:tool:owned-archive", out, sizeof(out)) && strstr(out, "old-call"));
    assert(context_evict(&c, "{\"keep_recent\":2}", out, sizeof(out)) && strstr(out, "\"changed\":false"));
    assert(strstr(out, "\"no_op_reason\":\"no_complete_tool_exchange\""));
    assert(strstr(out, "\"pairs_scanned\":"));
    conv_free(&c);
    /* Native Responses replay state must not pin completed exchanges forever. */
    c = fixture();
    const char *reason_type = "openai_codex_reasoning:gpt-6-astra";
    const char *reason_text = "{\"type\":\"reasoning\",\"encrypted_content\":\"OLD_OPAQUE\"}";
    block(&c.msgs[1], reason_type, NULL, reason_text);
    block(&c.msgs[4], reason_type, NULL, "LATEST_OPAQUE");
    storage_ok = false;
    assert(!context_evict(&c, "{\"keep_recent\":2}", out, sizeof(out)));
    assert(!strcmp(c.msgs[1].content[1].text, reason_text));
    storage_ok = true;
    assert(context_evict(&c, "{\"keep_recent\":2}", out, sizeof(out)));
    assert(strstr(out, "\"turns_evicted\":1"));
    assert(strstr(saved, reason_type) && strstr(saved, "OLD_OPAQUE"));
    assert(c.msgs[1].content_count == 1);
    assert(!strstr(c.msgs[1].content[0].text, "OLD_OPAQUE"));
    assert(!strcmp(c.msgs[4].content[1].text, "LATEST_OPAQUE"));
    assert(conv_validate_tool_call_integrity(&c, false).ok);
    conv_free(&c);
    c = fixture();
    block(&c.msgs[1], "unknown_provider_block", NULL, "KEEP_UNKNOWN");
    assert(context_evict(&c, "{\"keep_recent\":2}", out, sizeof(out)));
    assert(strstr(out, "\"changed\":false"));
    assert(!strcmp(c.msgs[1].content[1].text, "KEEP_UNKNOWN"));
    conv_free(&c);
    c = fixture(); storage_ok = false;
    assert(!context_evict(&c, "{\"keep_recent\":2}", out, sizeof(out)));
    assert(!strcmp(c.msgs[1].content[0].type, "tool_use") && strlen(c.msgs[1].content[0].tool_input) == 29999);
    storage_ok = true;
    const char *bad[] = {"[]", "{", "{\"keep_recent\":1}", "{\"min_bytes\":-1}",
        "{\"max_turns\":65}", "{\"max_turns\":true}", "{\"keep_recent\":2,\"keep_recent\":3}", "{\"unknown\":1}"};
    for (size_t i = 0; i < sizeof(bad)/sizeof(*bad); i++) {
        assert(!context_evict(&c, bad[i], out, sizeof(out)));
        assert(!strcmp(c.msgs[1].content[0].type, "tool_use"));
    }
    /* A latest pending call cannot prevent older complete calls being compacted. */
    conv_pop_last(&c);
    assert(context_evict(&c, "{\"keep_recent\":2}", out, sizeof(out)));
    assert(!strcmp(c.msgs[4].content[0].type, "tool_use"));
    assert(conv_validate_tool_call_integrity(&c, true).ok);
    conv_free(&c);
    c = fixture();
    block(&c.msgs[1], "tool_use", "parallel-call", "{}");
    block(&c.msgs[2], "tool_result", "parallel-call", "PARALLEL_RESULT");
    assert(context_evict(&c, "{\"keep_recent\":2}", out, sizeof(out)));
    assert(strstr(saved, "parallel-call") && strstr(saved, "PARALLEL_RESULT"));
    assert(conv_validate_tool_call_integrity(&c, false).ok);
    conv_free(&c);
    c = fixture();
    block(&c.msgs[2], "text", NULL, "USER_TEXT_WITH_RESULTS");
    assert(context_evict(&c, "{\"keep_recent\":2}", out, sizeof(out)) && strstr(out, "\"changed\":false"));
    conv_free(&c);
    /* A partly returned final parallel batch is still live, not corrupt. */
    c = fixture();
    block(&c.msgs[4], "tool_use", "still-running", "{}");
    assert(!conv_validate_tool_call_integrity(&c, false).ok);
    assert(conv_validate_tool_call_integrity(&c, true).ok);
    assert(context_evict(&c, "{\"keep_recent\":2}", out, sizeof(out)));
    assert(strstr(out, "\"turns_evicted\":1"));
    assert(!strcmp(c.msgs[4].content[1].tool_id, "still-running"));
    assert(!strcmp(c.msgs[5].content[0].text, "KEEP_LATEST_EVIDENCE"));
    /* Real user text closes the pending allowance; do not silently repair it. */
    block(&c.msgs[5], "text", NULL, "USER_STEERING");
    assert(!conv_validate_tool_call_integrity(&c, true).ok);
    assert(!context_evict(&c, "{\"keep_recent\":2}", out, sizeof(out)));
    conv_free(&c);
    c = fixture();
    block(&c.msgs[1], "tool_use", "older-missing", "{}");
    assert(!conv_validate_tool_call_integrity(&c, true).ok);
    assert(!context_evict(&c, "{\"keep_recent\":2}", out, sizeof(out)));
    conv_free(&c);
    c = fixture();
    block(&c.msgs[4], "tool_use", "still-running", "{}");
    block(&c.msgs[5], "tool_result", "orphan-result", "INVALID");
    assert(!conv_validate_tool_call_integrity(&c, true).ok);
    /* The orphaned latest result remains intact, but it must not block
     * archival of the older complete exchange. */
    assert(context_evict(&c, "{\"keep_recent\":2}", out, sizeof(out)));
    assert(strstr(out, "\"turns_evicted\":1") && strstr(out, "\"protocol_complete\":false"));
    assert(strstr(saved, "old-call") && strstr(saved, "OLD_RESULT_VERIFIED"));
    conv_free(&c);
    /* Old synthetic preflight IDs were per-turn (pid + retry), so two
     * completed weather calls could share an ID and block ALL eviction.
     * Archive only the older valid pair, leaving a complete wire graph. */
    c = fixture();
    free(c.msgs[4].content[0].tool_id);
    c.msgs[4].content[0].tool_id = strdup("old-call");
    free(c.msgs[5].content[0].tool_id);
    c.msgs[5].content[0].tool_id = strdup("old-call");
    assert(!conv_validate_tool_call_integrity(&c, true).ok);
    assert(!context_evict(&c, "{\"keep_recent\":100}", out, sizeof(out)));
    assert(!strcmp(c.msgs[1].content[0].type, "tool_use"));
    assert(context_evict(&c, "{\"keep_recent\":2}", out, sizeof(out)));
    assert(strstr(out, "\"protocol_complete\":true"));
    assert(conv_validate_tool_call_integrity(&c, false).ok);
    assert(!strcmp(c.msgs[4].content[0].tool_id, "old-call"));
    assert(strstr(saved, "OLD_RESULT_VERIFIED"));
    conv_free(&c);
    /* The real repeated weather pair is roughly 530 bytes, below the
     * default 2048-byte threshold. Repair must archive it anyway. */
    c = fixture();
    free(c.msgs[2].content[0].text);
    c.msgs[2].content[0].text = strdup("short weather result");
    free(c.msgs[4].content[0].tool_id);
    c.msgs[4].content[0].tool_id = strdup("old-call");
    free(c.msgs[5].content[0].tool_id);
    c.msgs[5].content[0].tool_id = strdup("old-call");
    assert(context_evict(&c, "{\"keep_recent\":2}", out, sizeof(out)));
    assert(strstr(out, "\"integrity_repair_archives\":1"));
    assert(strstr(saved, "short weather result"));
    assert(conv_validate_tool_call_integrity(&c, false).ok);
    conv_free(&c);
    /* Regression: aggressive compaction must look behind the protected tail. */
    c = fixture();
    assert(conv_compact_recent_tool_turn(&c, 384, 2));
    assert(!strcmp(c.msgs[1].content[0].type, "text"));
    assert(!strcmp(c.msgs[4].content[0].type, "tool_use"));
    assert(conv_validate_tool_call_integrity(&c, false).ok);
    conv_free(&c);
    assert(jev_calls == 0);
    const char *modes[] = {"shadow", JEV_COMPACTION_V1_MODE, "off", "shadow-v2"};
    for (size_t i = 0; i < sizeof(modes)/sizeof(*modes); i++) {
    setenv("DSCO_JEV_COMPACTION", modes[i], 1);
    int before_calls = jev_calls;
    c = fixture();
    assert(context_evict(&c, "{\"keep_recent\":2}", out, sizeof(out)));
    if (i < 2) assert(jev_calls == before_calls + 1 && strstr(out, "\"jev_shadow\":") && strstr(out, "\"applied\":false"));
    else assert(jev_calls == before_calls && !strstr(out, "jev_shadow"));
    assert(!strcmp(c.msgs[0].content[0].text, "KEEP_USER_OBJECTIVE"));
    assert(!strcmp(c.msgs[5].content[0].text, "KEEP_LATEST_EVIDENCE"));
    assert(conv_validate_tool_call_integrity(&c, false).ok);
    assert(strstr(saved, "OLD_RESULT_VERIFIED"));
    conv_free(&c);
    }
    unsetenv("DSCO_JEV_COMPACTION");
    c = fixture();
    char prose[20000]; memset(prose, 'p', sizeof(prose)-1); prose[sizeof(prose)-1] = 0;
    block(&c.msgs[1], "text", NULL, prose);
    c.msgs[2].content[0].is_error = true;
    assert(context_evict(&c, "{\"keep_recent\":2}", out, sizeof(out)));
    assert(strlen(c.msgs[1].content[0].text) <= 512);
    assert(strstr(saved, prose) && strstr(saved, "\"is_error\":true"));
    conv_free(&c);
    /* Long prose-only sessions used to be a permanent no-op: there was no
     * tool exchange to archive, even when the request was over budget. */
    c = (conversation_t){.count = 6, .cap = 6, .msgs = calloc(6, sizeof(message_t))};
    assert(c.msgs);
    char long_answer[10001]; memset(long_answer, 'A', 10000); long_answer[10000] = 0;
    for (int i = 0; i < 6; i++) {
        c.msgs[i].role = i % 2 ? ROLE_ASSISTANT : ROLE_USER;
        block(&c.msgs[i], "text", NULL, i == 1 ? long_answer :
              i == 2 ? "KEEP_FOLLOWUP" : i == 3 ? "short answer" :
              i == 4 ? "KEEP_RECENT_USER" : i == 5 ? "KEEP_RECENT_ASSISTANT" : "KEEP_OBJECTIVE");
    }
    int tokens_before = conv_rough_estimate(&c);
    storage_ok = false;
    assert(!context_evict(&c, "{\"keep_recent\":2}", out, sizeof(out)));
    assert(!strcmp(c.msgs[1].content[0].text, long_answer));
    storage_ok = true;
    assert(context_evict(&c, "{\"keep_recent\":2}", out, sizeof(out)));
    assert(strstr(out, "\"changed\":true") && strstr(out, "\"assistant_messages_archived\":1"));
    assert(strstr(c.msgs[1].content[0].text, "ck:msg:owned-archive"));
    assert(strstr(saved, long_answer));
    assert(conv_rough_estimate(&c) < tokens_before / 2);
    assert(context_archive_recall("ck:msg:owned-archive", out, sizeof(out)));
    assert(strstr(out, "\"key\":\"ck:msg:owned-archive\"") && strstr(out, "AAAAAAAAAAAAAAAAAAAA"));
    assert(!strcmp(c.msgs[0].content[0].text, "KEEP_OBJECTIVE"));
    assert(!strcmp(c.msgs[2].content[0].text, "KEEP_FOLLOWUP"));
    assert(!strcmp(c.msgs[4].content[0].text, "KEEP_RECENT_USER"));
    assert(!strcmp(c.msgs[5].content[0].text, "KEEP_RECENT_ASSISTANT"));
    assert(conv_validate_tool_call_integrity(&c, false).ok);
    assert(context_evict(&c, "{\"keep_recent\":2}", out, sizeof(out)) && strstr(out, "\"changed\":false"));
    conv_free(&c);
    /* A successful put alone is insufficient. Missing/truncated/corrupt
     * readback retains exact live bytes and stops further mutations. */
    for (get_fault=1; get_fault<=3; get_fault++) {
        c=fixture(); int writes=put_calls;
        assert(!context_evict(&c,"{\"keep_recent\":2}",out,sizeof(out)));
        const char *stages[]={"","readback_failed","readback_length_mismatch","readback_content_mismatch"};
        assert(strstr(out,stages[get_fault]));
        assert(put_calls == writes+1 && !strcmp(c.msgs[1].content[0].type,"tool_use"));
        assert(strlen(c.msgs[1].content[0].tool_input)==29999);
        assert(!json_get_bool(out,"changed",true)); conv_free(&c);
    }
    get_fault=0;
    /* Parallel results split over adjacent messages form one atomic span. */
    c=fixture(); block(&c.msgs[1],"tool_use","parallel-split","{}");
    c.msgs=realloc(c.msgs,7*sizeof(message_t)); assert(c.msgs); c.cap=7;
    memmove(c.msgs+4,c.msgs+3,3*sizeof(message_t));
    memset(c.msgs+3,0,sizeof(message_t)); c.count=7; c.msgs[3].role=ROLE_USER;
    block(&c.msgs[3],"tool_result","parallel-split","SPLIT_RESULT_EXACT");
    assert(context_evict(&c,"{\"keep_recent\":2}",out,sizeof(out)));
    assert(c.count==6 && strstr(saved,"SPLIT_RESULT_EXACT"));
    assert(!strcmp(c.msgs[3].content[0].text,"KEEP_CORRECTION"));
    assert(conv_validate_tool_call_integrity(&c,false).ok); conv_free(&c);
    /* Duplicate result A,A cannot satisfy calls A,B. */
    c=fixture(); block(&c.msgs[1],"tool_use","missing-B","{}");
    block(&c.msgs[2],"tool_result","old-call","DUPLICATE_RESULT");
    int writes=put_calls;
    assert(!context_evict(&c,"{\"keep_recent\":2}",out,sizeof(out)));
    assert(put_calls==writes && c.msgs[1].content_count==2); conv_free(&c);
    /* A multi-block response carries citations, media metadata and encrypted
     * replay state. Every field must survive; ciphertext never enters preview. */
    conv_init(&c); conv_add_user_text(&c,"seed"); conv_add_assistant_text(&c,long_answer);
    block(&c.msgs[1],"text",NULL,"SECOND_BLOCK_EXACT");
    block(&c.msgs[1],reason_type,NULL,reason_text);
    msg_content_t *rich=&c.msgs[1].content[0];
    rich->image_media_type=strdup("image/png");rich->image_data=strdup("IMAGE_BYTES");rich->image_url=strdup("image-local");
    rich->video_media_type=strdup("video/mp4");rich->video_data=strdup("VIDEO_BYTES");rich->video_url=strdup("video-local");
    rich->doc_media_type=strdup("text/plain");rich->doc_data=strdup("DOC_BYTES");rich->doc_title=strdup("TITLE_EXACT");
    rich->cited_text=strdup("CITATION_EXACT");rich->cite_start=42;rich->cite_end=91;
    conv_add_user_text(&c,"latest user");conv_add_assistant_text(&c,"latest assistant");
    assert(context_evict(&c,"{\"keep_recent\":2,\"preview_chars\":100}",out,sizeof(out)));
    assert(strstr(saved,"SECOND_BLOCK_EXACT") && strstr(saved,"OLD_OPAQUE"));
    assert(!strstr(c.msgs[1].content[0].text,"OLD_OPAQUE"));
    yyjson_doc *archive_doc=yyjson_read(saved,strlen(saved),0);assert(archive_doc);
    yyjson_val *first=yyjson_arr_get(yyjson_obj_get(yyjson_arr_get(yyjson_doc_get_root(archive_doc),0),"content"),0);
    assert(yyjson_equals_str(yyjson_obj_get(first,"image_data"),"IMAGE_BYTES"));
    assert(yyjson_equals_str(yyjson_obj_get(first,"video_data"),"VIDEO_BYTES"));
    assert(yyjson_equals_str(yyjson_obj_get(first,"doc_title"),"TITLE_EXACT"));
    assert(yyjson_equals_str(yyjson_obj_get(first,"cited_text"),"CITATION_EXACT"));
    assert(yyjson_get_int(yyjson_obj_get(first,"cite_start"))==42 && yyjson_get_int(yyjson_obj_get(first,"cite_end"))==91);
    assert(yyjson_is_null(yyjson_obj_get(first,"tool_id"))); yyjson_doc_free(archive_doc);
    assert(context_evict(&c,"{\"keep_recent\":2}",out,sizeof(out)) && !json_get_bool(out,"changed",true)); conv_free(&c);
    /* A failure after a successful archive reports partial progress, retains
     * the next original, and does not fold/rewrite any more receipts. */
    c=fixture(); block(&c.msgs[3],"text",NULL,"user mixed evidence stays");
    free(c.msgs[3].content[0].type);c.msgs[3].content[0].type=strdup("text");
    c.msgs[3].role=ROLE_ASSISTANT; free(c.msgs[3].content[0].text);c.msgs[3].content[0].text=strdup(long_answer);
    fail_after=put_calls+1;
    assert(!context_evict(&c,"{\"keep_recent\":2}",out,sizeof(out)));
    assert(json_get_bool(out,"changed",false) && json_get_int(out,"archive_failures",0)==1);
    assert(!strcmp(c.msgs[3].content[0].text,long_answer));fail_after=-1;conv_free(&c);
    /* Serialization metadata alone must not cause receipts to expand live
     * history or stop reclamation of the next worthwhile candidate. */
    conv_init(&c);conv_add_user_text(&c,"seed");conv_add_assistant_text(&c,"small");
    c.msgs[1].content[0].cited_text=strdup(long_answer);
    conv_add_assistant_text(&c,long_answer);conv_add_user_text(&c,"latest");conv_add_assistant_text(&c,"latest evidence");
    assert(context_evict(&c,"{\"keep_recent\":2,\"min_bytes\":512}",out,sizeof(out)));
    assert(!strcmp(c.msgs[1].content[0].text,"small"));
    assert(json_get_int(out,"assistant_messages_archived",0)==1 && json_get_int(out,"below_min_bytes",0)>=1);
    conv_free(&c);
    conv_init(&c);conv_add_user_text(&c,"seed");
    char boundary[3001];memset(boundary,'x',3000);memcpy(boundary+99,"雪",3);boundary[3000]=0;
    conv_add_assistant_text(&c,boundary);conv_add_user_text(&c,"latest");conv_add_assistant_text(&c,"recent");
    assert(context_evict(&c,"{\"keep_recent\":2,\"preview_chars\":100}",out,sizeof(out)));
    char *preview=strrchr(c.msgs[1].content[0].text,'\n');assert(preview && strlen(preview+1)==99);
    assert(strstr(c.msgs[1].content[0].text,"Partial preview: 99 of 3000 bytes"));
    conv_free(&c);
    /* An untagged legacy receipt is reclaimable only when the original archive
     * regenerates both plain-text messages exactly. Spoofed user text stays. */
    c=fixture();
    conversation_t pair={.msgs=c.msgs+1,.count=2,.cap=2};
    free(saved);saved=context_archive_serialize(pair.msgs,2);assert(saved);
    assert(conv_render_legacy_tool_turn(&pair,384,0));
    char *legacy_assistant=strdup(c.msgs[1].content[0].text);
    char legacy_user[4096];snprintf(legacy_user,sizeof(legacy_user),
        "[Archived completed tool exchange; key=ck:tool:owned-archive; retrieve with context_recall, optionally #b:0-4096]\n%s",c.msgs[2].content[0].text);
    free(c.msgs[2].content[0].text);c.msgs[2].content[0].text=strdup(legacy_user);
    assert(context_archive_legacy_receipt(&c.msgs[1],&c.msgs[2]));
    free(c.msgs[2].content[0].text);c.msgs[2].content[0].text=strdup("[Archived completed tool exchange; key=ck:tool:owned-archive; retrieve with context_recall, optionally #b:0-4096]\nKEEP_USER_INSTRUCTION");
    assert(!context_archive_legacy_receipt(&c.msgs[1],&c.msgs[2]));conv_free(&c);
    conv_init(&c);conv_add_user_text(&c,"KEEP_USER_OBJECTIVE");
    for(int i=0;i<4;i++){conv_add_assistant_text(&c,legacy_assistant);conv_add_user_text(&c,legacy_user);}
    conv_add_user_text(&c,"KEEP_LATEST_INSTRUCTION");conv_add_assistant_text(&c,"KEEP_LATEST_EVIDENCE");
    assert(context_evict(&c,"{\"keep_recent\":2}",out,sizeof(out)));
    assert(json_get_int(out,"receipts_folded",0)==7 && c.count==4);
    assert(!strcmp(c.msgs[0].content[0].text,"KEEP_USER_OBJECTIVE"));
    assert(!strcmp(c.msgs[2].content[0].text,"KEEP_LATEST_INSTRUCTION"));
    assert(strstr(saved,"ck:tool:owned-archive"));conv_free(&c);free(legacy_assistant);
    free(saved);
    unsetenv("DSCO_JEV_COMPACTION");
    puts("PASS: archive/readback, prose-only sessions, large arguments, preserved objectives/recent/pending calls, parallel protocol, idempotence, split spans, exact rich block fields, duplicate results, readback faults, partial failure and strict options");
}
