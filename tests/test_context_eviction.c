#include "context_eviction.h"
#include "context_fabric.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Real conversation mechanics; injectable storage failure. The binary test
 * separately exercises the persistent broker and governed dispatch. */
static char *saved;
static bool storage_ok = true;
ctx_broker_t *ctx_broker_default(void) { return (ctx_broker_t *)1; }
bool ctx_put(ctx_broker_t *b, const void *p, size_t n, const ctx_put_opts_t *o,
             ctxkey_t *key, bool *deduped) {
    (void)b; (void)key; (void)deduped;
    assert(o->embed == 0 && o->kind == CTX_KIND_TOOL);
    if (!storage_ok) return false;
    free(saved); saved = strndup(p, n); return saved != NULL;
}
int ctxkey_format(const ctxkey_t *k, char *out, size_t n) {
    (void)k; return snprintf(out, n, "ck:tool:owned-archive");
}
bool ctxkey_parse(const char *s, ctxkey_t *k) {
    memset(k, 0, sizeof(*k)); return !strcmp(s, "ck:tool:owned-archive");
}
char *ctx_get(ctx_broker_t *b, const ctxkey_t *k, size_t *n) {
    (void)b; (void)k;
    if (!saved) return NULL;
    *n = strlen(saved); return strdup(saved);
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
    char out[4096];
    conversation_t c = fixture();
    assert(context_evict(&c, "{}", out, sizeof(out)) && strstr(out, "\"changed\":false"));
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
    assert(!context_evict(&c, "{\"keep_recent\":2}", out, sizeof(out)));
    conv_free(&c);
    /* Regression: aggressive compaction must look behind the protected tail. */
    c = fixture();
    assert(conv_compact_recent_tool_turn(&c, 384, 2));
    assert(!strcmp(c.msgs[1].content[0].type, "text"));
    assert(!strcmp(c.msgs[4].content[0].type, "tool_use"));
    assert(conv_validate_tool_call_integrity(&c, false).ok);
    conv_free(&c); free(saved);
    puts("PASS: archive/readback, large arguments, preserved objectives/recent/pending calls, parallel protocol, idempotence, failed storage and strict options");
}
