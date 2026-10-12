#include "jev_compaction.h"
#include "capability.h"
#include "json_util.h"
#include "../vendor/yyjson.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int calls, notes;
static bool secret_grant = true, transport_ok = true;
static const char *reply, *mode;
const char *tools_execution_tier(void) { return "untrusted-test-tier"; }
bool dsco_cap_granted(dsco_cap_t cap, const char *tier) {
    assert(cap == CAP_SECRETS && !strcmp(tier, "untrusted-test-tier")); return secret_grant;
}
void dsco_flow_note(unsigned caps) { assert(caps == CAP_SECRETS); notes++; }
bool tools_execute_for_tier(const char *name, const char *input, const char *tier, char *out, size_t len) {
    assert(!strcmp(name, "curl_raw") && !strcmp(tier, "untrusted-test-tier"));
    assert(notes && !strstr(input, "SECRET_SENTINEL"));
    assert(strstr(input, "--variable %TYPESAFE_API_KEY") && strstr(input, "-q --silent"));
    assert(strstr(input, "--max-time 4") && !strstr(input, "--location"));
    assert(strstr(input, "https://api.typesafe.ai/v1/systemone"));
    yyjson_doc *d = yyjson_read(input, strlen(input), 0); assert(d);
    const char *args = yyjson_get_str(yyjson_obj_get(yyjson_doc_get_root(d), "args"));
    assert(args && strlen(args) < 8186);
    const char *body = strstr(args, "--data-raw '"); assert(body); body += strlen("--data-raw '");
    const char *end = strchr(body, '\''); assert(end);
    yyjson_doc *b = yyjson_read(body, (size_t)(end - body), 0); assert(b);
    assert(yyjson_equals_str(yyjson_obj_get(yyjson_doc_get_root(b), "model"),
           !strcmp(mode, JEV_COMPACTION_V1_MODE) ? JEV_COMPACTION_V1_MODEL : "jev-latest"));
    yyjson_val *state = yyjson_obj_get(yyjson_doc_get_root(b), "state");
    yyjson_val *candidates = yyjson_obj_get(state, "candidates");
    assert(yyjson_arr_size(candidates) <= JEV_COMPACTION_MAX);
    yyjson_val *c = yyjson_arr_get(candidates, 0); assert(yyjson_obj_size(c) == 5);
    assert(yyjson_get_uint(yyjson_obj_get(c, "archive_bytes")) == 9000);
    yyjson_doc_free(b); yyjson_doc_free(d);
    calls++; snprintf(out, len, "%s", reply ? reply : "broken"); return transport_ok;
}
int main(int argc, char **argv) {
    const char *scenario = argc > 1 ? argv[1] : "success";
    mode = argc > 2 ? argv[2] : "shadow";
    char out[512];
    jev_compaction_candidate_t c[JEV_COMPACTION_MAX] = {{.age_messages=20, .archive_bytes=9000, .calls=1, .results=1, .archived=true}};
    unsetenv("DSCO_JEV_COMPACTION"); setenv("TYPESAFE_API_KEY", "SECRET_SENTINEL", 1);
    jev_compaction_shadow(c, 1, out, sizeof(out)); assert(strstr(out, "disabled") && !calls && !notes);
    const char *off[] = {"", "off", "1", "true", "SHADOW", "shadow-v2", "apply"};
    for (size_t i = 0; i < sizeof(off)/sizeof(*off); i++) {
        setenv("DSCO_JEV_COMPACTION", off[i], 1);
        jev_compaction_shadow(c, 1, out, sizeof(out));
        assert(strstr(out, "disabled") && !calls && !notes);
    }
    setenv("DSCO_JEV_COMPACTION", mode, 1);
    jev_compaction_shadow(c, 1, NULL, 0);
    assert(!calls && !notes);
    jev_compaction_shadow(c, 0, out, sizeof(out)); assert(strstr(out, "no_candidates") && !calls);
    secret_grant = false; jev_compaction_shadow(c, 1, out, sizeof(out)); assert(strstr(out, "secrets_denied") && !calls && !notes);
    secret_grant = true; unsetenv("TYPESAFE_API_KEY");
    jev_compaction_shadow(c, 1, out, sizeof(out)); assert(strstr(out, "missing_key") && !calls);
    setenv("TYPESAFE_API_KEY", "SECRET_SENTINEL", 1);
    size_t count = !strcmp(scenario, "bound") ? JEV_COMPACTION_MAX + 3 : 1;
    jbuf_t good; jbuf_init(&good, 4096);
    const char *model = !strcmp(scenario, "model-mismatch") ? "jev-other" :
                        !strcmp(mode, JEV_COMPACTION_V1_MODE) ? JEV_COMPACTION_V1_MODEL : "jev-test";
    jbuf_appendf(&good, "{\"model\":\"%s\",\"usage\":{\"input_tokens\":500},\"answers\":{", model);
    for (size_t i = 0; i < count && i < JEV_COMPACTION_MAX; i++)
        jbuf_appendf(&good, "%s\"c%zu\":{\"type\":\"choice\",\"choice\":\"archive\",\"confidence\":0.9,\"probabilities\":{\"archive\":0.95,\"retain\":0.05}}", i ? "," : "", i);
    jbuf_append(&good, "}}"); reply = good.data;
    if (!strcmp(scenario, "denied")) transport_ok = false;
    if (!strcmp(scenario, "malformed")) reply = "bad gateway";
    if (!strcmp(scenario, "missing")) reply = "{\"answers\":{}}";
    if (!strcmp(scenario, "range")) reply = "{\"model\":\"jev-test\",\"usage\":{\"input_tokens\":500},\"answers\":{\"c0\":{\"type\":\"choice\",\"choice\":\"archive\",\"confidence\":2,\"probabilities\":{\"archive\":0.95,\"retain\":0.05}}}}";
    jev_compaction_shadow(c, count, out, !strcmp(scenario, "small-receipt") ? 128 : sizeof(out));
    assert(calls == 1);
    if (!strcmp(scenario, "success") || !strcmp(scenario, "bound") ||
        (!strcmp(scenario, "model-mismatch") && !strcmp(mode, "shadow"))) {
        assert(strstr(out, "observed") && strstr(out, "\"applied\":false") && strstr(out, "\"agreement_count\":1") && strstr(out, "dsco.jev_compaction.v1"));
        if (!strcmp(mode, JEV_COMPACTION_V1_MODE)) assert(strstr(out, "\"mode\":\"shadow-v1\""));
    } else if (!strcmp(scenario, "small-receipt")) assert(strstr(out, "receipt_too_large"));
    else if (!strcmp(scenario, "denied")) assert(strstr(out, "transport_denied_or_failed"));
    else assert(strstr(out, "invalid_response"));
    yyjson_doc *d = yyjson_read(out, strlen(out), 0); assert(d); yyjson_doc_free(d);
    assert(notes == 1);
    unsetenv("DSCO_JEV_COMPACTION");
    jev_compaction_shadow(c, 1, out, sizeof(out)); assert(strstr(out, "disabled") && calls == 1 && notes == 1);
    setenv("DSCO_JEV_COMPACTION", mode, 1);
    jev_compaction_shadow(c, 1, out, sizeof(out)); assert(strstr(out, "attempt_limit") && calls == 1 && notes == 1);
    jbuf_free(&good);
    printf("PASS jev_compaction %s\n", scenario);
    return 0;
}
