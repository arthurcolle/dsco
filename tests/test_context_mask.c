/* Observation masking: keep windows, tool images vs user uploads, per-item
 * store failure isolation, idempotence, and untouched tool pairing. */
#include "context_mask.h"
#include "context_fabric.h"
#include "tool_integrity_index.h"
#include "../vendor/yyjson.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int puts_ok, fail_next;
ctx_broker_t *ctx_broker_default(void) { return (ctx_broker_t *)1; }
bool ctx_put(ctx_broker_t *b, const void *p, size_t n, const ctx_put_opts_t *o, ctxkey_t *key, bool *dedup) {
    (void)b; (void)p; (void)n; (void)dedup;
    if (fail_next) { fail_next--; return false; }
    memset(key, 0, sizeof(*key));
    key->kind = o->kind;
    snprintf(key->id, sizeof(key->id), "k%d", ++puts_ok);
    return true;
}
int ctxkey_format(const ctxkey_t *key, char *out, size_t n) { return snprintf(out, n, "ck:%s", key->id); }
bool chronicle_event(const char *a, const char *b, const char *c, const char *d, const char *e,
                     const char *f, const char *g, const char *h) {
    (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g; (void)h;
    return false;
}

static char *big(char ch, size_t n) {
    char *s = malloc(n + 1);
    memset(s, ch, n);
    s[n] = 0;
    return s;
}

static void add_exchange(conversation_t *c, int i, size_t bytes) {
    char id[16], *payload = big((char)('a' + i % 26), bytes);
    snprintf(id, sizeof(id), "t%d", i);
    conv_add_assistant_tool_use(c, id, "bash", "{\"command\":\"ls\"}");
    conv_add_tool_result_named(c, id, "bash", payload, false);
    free(payload);
}

static int count_masked(const conversation_t *c) {
    int n = 0;
    for (int i = 0; i < c->count; i++)
        for (int j = 0; j < c->msgs[i].content_count; j++)
            n += context_mask_is_placeholder(&c->msgs[i].content[j]);
    return n;
}

int main(void) {
    conversation_t c;
    conv_init(&c);
    conv_add_user_text(&c, "objective");
    for (int i = 0; i < 12; i++) add_exchange(&c, i, 4000);
    add_exchange(&c, 99, 100); /* tiny old-ish result stays verbatim */
    int before_count = c.count;

    context_mask_policy_t p = {.keep_results = 4, .keep_images = 2, .min_bytes = 1024};
    context_mask_stats_t st;

    /* Below the batch threshold nothing changes (cache-preserving deferral). */
    p.min_batch_bytes = 1u << 20;
    assert(!context_mask_observations(&c, &p, &st) && st.deferred && st.candidate_bytes == 9 * 4000);
    assert(count_masked(&c) == 0 && puts_ok == 0);
    p.min_batch_bytes = 0;
    assert(context_mask_observations(&c, &p, &st));
    /* 13 results; newest 4 kept (incl. the tiny one) -> 9 large old ones masked. */
    assert(st.results_masked == 9 && st.store_failures == 0);
    assert(c.count == before_count);                       /* nothing removed */
    assert(conv_validate_tool_call_integrity(&c, true).ok); /* pairing intact */
    assert(!strcmp(c.msgs[0].content[0].text, "objective"));
    const char *first = c.msgs[2].content[0].text;
    assert(strstr(first, "context_recall") && strstr(first, "ck:k9") && strstr(first, "4000 bytes from bash"));
    assert(strlen(c.msgs[c.count - 3].content[0].text) == 4000); /* recent kept */
    assert(st.bytes_after < st.bytes_before / 20);

    /* Idempotent: a second pass changes nothing and stores nothing. */
    int stored = puts_ok;
    assert(!context_mask_observations(&c, &p, &st) && st.already_masked == 9 && puts_ok == stored);

    /* Tool screenshots beyond keep_images are masked; user uploads never are. */
    conv_add_user_image_base64(&c, "image/png", "USERUPLOAD", "please look at this");
    for (int i = 0; i < 4; i++) {
        char *b64 = big('Q', 2000);
        conv_add_user_image_base64(&c, "image/jpeg", b64, "Tool observation.");
        free(b64);
    }
    p.keep_results = 1000;
    /* Hysteresis: 4 tool images, keep 2, batch 3 -> not yet due. */
    p.image_batch = 3;
    assert(!context_mask_observations(&c, &p, &st) && st.images_masked == 0);
    p.image_batch = 0;
    assert(context_mask_observations(&c, &p, &st) && st.images_masked == 2 && st.results_masked == 0);
    int images = 0, user_upload = 0;
    for (int i = 0; i < c.count; i++)
        for (int j = 0; j < c.msgs[i].content_count; j++) {
            msg_content_t *b = &c.msgs[i].content[j];
            if (b->type && !strcmp(b->type, "image")) {
                images++;
                if (!strcmp(b->image_data, "USERUPLOAD")) user_upload = 1;
            }
        }
    assert(images == 3 && user_upload);

    /* A store failure skips only that block; the pass continues. */
    conv_free(&c);
    conv_init(&c);
    conv_add_user_text(&c, "objective");
    for (int i = 0; i < 6; i++) add_exchange(&c, i, 3000);
    fail_next = 1;
    p.keep_results = 2;
    assert(context_mask_observations(&c, &p, &st));
    assert(st.store_failures == 1 && st.results_masked == 3 && count_masked(&c) == 3);
    assert(conv_validate_tool_call_integrity(&c, true).ok);
    /* Recall results are never re-masked (that would loop the model). */
    conv_free(&c);
    conv_init(&c);
    conv_add_user_text(&c, "objective");
    char *page = big('r', 5000);
    conv_add_assistant_tool_use(&c, "r1", "context_recall", "{\"key\":\"ck:x\"}");
    conv_add_tool_result_named(&c, "r1", "context_recall", page, false);
    free(page);
    for (int i = 0; i < 3; i++) add_exchange(&c, 20 + i, 10);
    context_mask_policy_t r = {.keep_results = 1, .min_bytes = 1024};
    assert(!context_mask_observations(&c, &r, &st) && strlen(c.msgs[2].content[0].text) == 5000);

    /* Large old tool inputs become a keyed JSON object; small actions stay. */
    conv_free(&c);
    conv_init(&c);
    conv_add_user_text(&c, "objective");
    char *content = big('x', 9000), input[9100];
    snprintf(input, sizeof(input), "{\"path\":\"a.c\",\"content\":\"%s\"}", content);
    free(content);
    conv_add_assistant_tool_use(&c, "w1", "write_file", input);
    conv_add_tool_result_named(&c, "w1", "write_file", "ok", false);
    for (int i = 0; i < 3; i++) add_exchange(&c, 10 + i, 10);
    context_mask_policy_t q = {.keep_results = 2, .min_input_bytes = 4096};
    assert(context_mask_observations(&c, &q, &st) && st.inputs_masked == 1);
    const char *stub = c.msgs[1].content[0].tool_input;
    yyjson_doc *doc = yyjson_read(stub, strlen(stub), 0);
    assert(doc && yyjson_is_obj(yyjson_doc_get_root(doc)) && strstr(stub, "context_recall"));
    yyjson_doc_free(doc);
    assert(!strcmp(c.msgs[3].content[0].tool_input, "{\"command\":\"ls\"}"));
    assert(conv_validate_tool_call_integrity(&c, true).ok);
    conv_free(&c);
    puts("PASS context_mask: keep windows, large-input stubs, user uploads preserved, failure isolation, idempotence, pairing intact");
    return 0;
}
