#include "swarm_hierarchy.h"
#include "json_util.h"
#include "../vendor/yyjson.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int calls, launches, collects, kills, aborts, group, workers, next_run;
static bool deny_spawn, partial_spawn, deny_abort, bad_collect, long_output, binary_output;
static int ids[32], runs[32];
static bool cancelled[32];
static char tasks[32][20000], last_launch[1024 * 1024], last_kill[512];
static int checks;
#define CHECK(x) do { checks++; assert(x); } while (0)

/* Production links the real gate. This spy verifies every nested operation
 * carries the same authority and simulates worker slot/group recycling. */
bool tools_execute_for_tier(const char *name, const char *input, const char *tier,
                            char *out, size_t cap) {
    CHECK(!strcmp(tier, "sandbox")); calls++;
    yyjson_doc *d = yyjson_read(input, strlen(input), 0); CHECK(d);
    yyjson_val *r = yyjson_doc_get_root(d);
    const char *action = yyjson_get_str(yyjson_obj_get(r, "action")); CHECK(action);
    jbuf_t b; jbuf_init(&b, 4096); bool ok = true;
    if (!strcmp(action, "create_executor_swarm")) {
        CHECK(!strcmp(name, "swarm")); launches++;
        snprintf(last_launch, sizeof(last_launch), "%s", input);
        CHECK(yyjson_get_sint(yyjson_obj_get(r, "max_worker_turns")) > 0);
        CHECK(yyjson_get_sint(yyjson_obj_get(r, "max_tokens")) > 0);
        CHECK(yyjson_is_true(yyjson_obj_get(r, "retain_until_collect")));
        if (deny_spawn) { jbuf_append(&b, "{\"error\":\"capability denied\"}"); ok = false; }
        else {
            group++; yyjson_val *a = yyjson_obj_get(r, "tasks"); workers = (int)yyjson_arr_size(a);
            CHECK(workers > 0 && workers <= 32);
            if (partial_spawn && workers > 1) workers--;
            jbuf_appendf(&b, "{\"group_id\":0,\"group_generation\":\"gen-%d\",\"agents_spawned\":%d,\"agent_ids\":[", group, workers);
            for (int i = 0; i < workers; i++) {
                ids[i] = i; runs[i] = ++next_run; cancelled[i] = false;
                snprintf(tasks[i], sizeof(tasks[i]), "%s", yyjson_get_str(yyjson_obj_get(yyjson_arr_get(a, i), "task")));
                jbuf_appendf(&b, "%s%d", i ? "," : "", ids[i]);
            }
            jbuf_append(&b, "],\"agents\":[");
            for (int i = 0; i < workers; i++) jbuf_appendf(&b, "%s{\"id\":%d,\"run_id\":%d,\"model\":\"fixture-model\"}", i ? "," : "", ids[i], runs[i]);
            jbuf_append(&b, "]}");
        }
    } else if (!strcmp(action, "collect") || !strcmp(action, "abort")) {
        CHECK(!strcmp(name, "swarm"));
        char generation[64]; snprintf(generation, sizeof(generation), "gen-%d", group);
        CHECK(!strcmp(generation, yyjson_get_str(yyjson_obj_get(r, "group_generation"))));
        CHECK(yyjson_get_sint(yyjson_obj_get(r, "group_id")) == 0);
        bool aborting = !strcmp(action, "abort");
        if (aborting) aborts++; else collects++;
        if (aborting && deny_abort) { jbuf_append(&b, "{\"error\":\"abort denied\"}"); ok = false; }
        else {
            jbuf_appendf(&b, "{\"group_id\":0,\"group_generation\":\"gen-%d\",\"complete\":true,\"active\":0,\"artifact_dir\":\"fixture-artifacts\",\"results\":[", group);
            for (int i = 0; i < workers; i++) {
                bool error = strstr(tasks[i], "FAIL_NODE") != NULL;
                const char *status = aborting || cancelled[i] ? "killed" : error ? "error" : "done";
                jbuf_appendf(&b, "%s{\"id\":%d,\"run_id\":%d,\"status\":\"%s\",\"interop_outcome\":\"%s\",\"output\":", i ? "," : "", ids[i], runs[i] + (bad_collect ? 100 : 0), status, error ? "exit_error" : "success");
                if (binary_output) {
                    jbuf_append_json_str(&b, "{\"schema\":\"dsco.agent.interop/v1\",\"output\":\"A\\u0000\\u00ff\",\"output_base64\":\"QQD/\",\"truncated\":false}");
                } else if (long_output) {
                    jbuf_append_char(&b, '"');
                    for (int j = 0; j < 3000; j++) jbuf_append(&b, "\xe2\x82\xac");
                    jbuf_append_char(&b, '"');
                } else {
                    char output[100]; snprintf(output, sizeof(output), "RESULT_RUN_%d", runs[i]);
                    jbuf_append_json_str(&b, output);
                }
                jbuf_append_char(&b, '}');
            }
            jbuf_append(&b, "]}");
        }
    } else if (!strcmp(action, "kill")) {
        CHECK(!strcmp(name, "agent")); kills++;
        snprintf(last_kill, sizeof(last_kill), "%s", input);
        int id = (int)yyjson_get_sint(yyjson_obj_get(r, "id"));
        CHECK(id >= 0 && id < workers);
        CHECK(yyjson_get_sint(yyjson_obj_get(r, "run_id")) == runs[id]);
        char generation[64]; snprintf(generation, sizeof(generation), "gen-%d", group);
        CHECK(!strcmp(generation, yyjson_get_str(yyjson_obj_get(r, "group_generation"))));
        cancelled[id] = true; jbuf_append(&b, "{\"killed\":true}");
    } else CHECK(false);
    CHECK(b.len < cap); memcpy(out, b.data, b.len + 1);
    jbuf_free(&b); yyjson_doc_free(d); return ok;
}
static yyjson_doc *run(const char *input, bool expected) {
    char out[131072]; bool ok = swarm_hierarchy_execute(input, "sandbox", 32, 7, out, sizeof(out));
    if (ok != expected) fprintf(stderr, "request %s\nreply %s\n", input, out);
    CHECK(ok == expected); yyjson_doc *d = yyjson_read(out, strlen(out), 0); CHECK(d); return d;
}
static void invalid(const char *input) {
    int before = calls; yyjson_doc *d = run(input, false); CHECK(calls == before); yyjson_doc_free(d);
}
static void handle(yyjson_doc *d, char *id) {
    const char *s = yyjson_get_str(yyjson_obj_get(yyjson_doc_get_root(d), "hierarchy_id")); CHECK(s); strcpy(id, s); yyjson_doc_free(d);
}
static yyjson_doc *control(const char *id, const char *operation, const char *node, bool expected) {
    char input[512];
    snprintf(input, sizeof(input), "{\"operation\":\"%s\",\"hierarchy_id\":\"%s\"%s%s%s}", operation, id, node ? ",\"node_id\":\"" : "", node ? node : "", node ? "\"" : "");
    return run(input, expected);
}
static yyjson_val *node(yyjson_doc *d, const char *id) {
    yyjson_val *nodes = yyjson_obj_get(yyjson_doc_get_root(d), "nodes"); size_t i, count; yyjson_val *v;
    yyjson_arr_foreach(nodes, i, count, v) if (!strcmp(yyjson_get_str(yyjson_obj_get(v, "id")), id)) return v;
    CHECK(false); return NULL;
}
static const char *status(yyjson_doc *d, const char *id) { return yyjson_get_str(yyjson_obj_get(node(d, id), "status")); }
static void finish(const char *id) {
    for (int i = 0; i < 70; i++) {
        yyjson_doc *d = control(id, "advance", NULL, true);
        bool done = yyjson_is_true(yyjson_obj_get(yyjson_doc_get_root(d), "complete")); yyjson_doc_free(d);
        if (done) return;
    }
    CHECK(false);
}
int main(void) {
    invalid("{}"); invalid("{\"nodes\":[]}");
    invalid("{\"nodes\":[{\"id\":\"r\",\"task\":\"x\",\"task\":\"y\"}]}");
    invalid("{\"nodes\":[{\"id\":\"r\",\"task\":\"x\\u0000y\"}]}");
    invalid("{\"nodes\":[{\"id\":\"r\",\"task\":\"x\"}],\"dry_run\":1}");
    invalid("{\"nodes\":[{\"id\":\"r\",\"task\":\"x\"}],\"budget\":1}");
    invalid("{\"nodes\":[{\"id\":\"r\",\"task\":\"x\"}],\"cwd\":\"/dsco-hierarchy-no-such-directory\"}");
    invalid("{\"nodes\":[{\"id\":\"r\",\"task\":\"x\",\"executor\":\"interop:unknown\"}]}");
    invalid("{\"nodes\":[{\"id\":\"r\",\"task\":\"x\",\"executor\":\"interop:codex\",\"provider\":\"native\"}]}");
    invalid("{\"nodes\":[{\"id\":\"r\",\"task\":\"x\",\"parent\":42}]}");
    invalid("{\"nodes\":[{\"id\":\"r\",\"task\":\"x\",\"parent\":\"missing\"}]}");
    invalid("{\"nodes\":[{\"id\":\"r\",\"task\":\"x\",\"parent\":\"r\"}]}");
    invalid("{\"nodes\":[{\"id\":\"r\",\"task\":\"x\"},{\"id\":\"r\",\"task\":\"y\"}]}");
    invalid("{\"nodes\":[{\"id\":\"r\",\"task\":\"x\"},{\"id\":\"s\",\"task\":\"y\"}]}");
    invalid("{\"nodes\":[{\"id\":\"r\",\"task\":\"x\"},{\"id\":\"s\",\"task\":\"y\",\"parent\":\"t\"},{\"id\":\"t\",\"task\":\"z\",\"parent\":\"s\"}]}");
    invalid("{\"nodes\":[{\"id\":\"r\\n\",\"task\":\"x\"}]}");
    invalid("{\"operation\":\"advance\",\"hierarchy_id\":\"stale\",\"timeout\":21}");
    invalid("{\"operation\":\"status\",\"hierarchy_id\":\"stale\"}");
    const char *tree = "{\"name\":\"study\",\"provider\":\"native\",\"model\":\"m\",\"nodes\":[{\"id\":\"leaf\",\"parent\":\"left\",\"task\":\"leaf-task\"},{\"id\":\"root\",\"task\":\"root-task\"},{\"id\":\"left\",\"parent\":\"root\",\"task\":\"left-task\",\"executor\":\"interop:codex\"},{\"id\":\"right\",\"parent\":\"root\",\"task\":\"right-task\"}]}";
    char dry[2048]; snprintf(dry, sizeof(dry), "{\"dry_run\":true,%s", tree + 1);
    yyjson_doc *d = run(dry, true); CHECK(calls == 0);
    CHECK(yyjson_get_sint(yyjson_obj_get(yyjson_doc_get_root(d), "logical_depth")) == 3);
    CHECK(!strcmp(yyjson_get_str(yyjson_obj_get(node(d, "left"), "provider")), "")); yyjson_doc_free(d);
    char tiny[128]; CHECK(!swarm_hierarchy_execute(tree, "sandbox", 32, 7, tiny, sizeof(tiny)) && calls == 0);
    CHECK(!swarm_hierarchy_execute(tree, "sandbox", 32, 2, tiny, sizeof(tiny)) && calls == 0);

    char id[96]; handle(run(tree, true), id); CHECK(launches == 1 && workers == 1);
    CHECK(strstr(tasks[0], "node=root parent=none depth=1")); int root_run = runs[0];
    d = control(id, "status", NULL, true); CHECK(launches == 1 && collects == 0); yyjson_doc_free(d);
    d = control(id, "advance", NULL, true); CHECK(workers == 2 && launches == 2);
    CHECK(strstr(tasks[0], "parent=root depth=2")); char marker[80]; snprintf(marker, sizeof(marker), "RESULT_RUN_%d", root_run); CHECK(strstr(tasks[0], marker));
    CHECK(!strcmp(status(d, "root"), "done")); CHECK(!strcmp(status(d, "leaf"), "queued")); yyjson_doc_free(d);
    d = control(id, "advance", NULL, true); CHECK(workers == 1 && strstr(tasks[0], "parent=left depth=3")); yyjson_doc_free(d);
    finish(id); d = control(id, "status", "root", true);
    CHECK(yyjson_get_uint(yyjson_obj_get(node(d, "root"), "run_id")) == (unsigned)root_run);
    CHECK(!strcmp(yyjson_get_str(yyjson_obj_get(node(d, "root"), "output")), marker)); yyjson_doc_free(d);

    const char *branches = "{\"nodes\":[{\"id\":\"r\",\"task\":\"root\"},{\"id\":\"a\",\"parent\":\"r\",\"task\":\"branch\"},{\"id\":\"b\",\"parent\":\"r\",\"task\":\"branch\"},{\"id\":\"aa\",\"parent\":\"a\",\"task\":\"leaf\"},{\"id\":\"bb\",\"parent\":\"b\",\"task\":\"leaf\"}]}";
    handle(run(branches, true), id); d = control(id, "advance", NULL, true); yyjson_doc_free(d);
    int before = kills; d = control(id, "abort", "a", true);
    CHECK(kills == before + 1 && strstr(last_kill, "run_id") && strstr(last_kill, "group_generation"));
    CHECK(!strcmp(status(d, "a"), "cancel_pending") && !strcmp(status(d, "aa"), "cancelled") && !strcmp(status(d, "b"), "running")); yyjson_doc_free(d);
    d = control(id, "advance", NULL, true); CHECK(workers == 1 && strstr(tasks[0], "node=bb")); CHECK(!strcmp(status(d, "a"), "cancelled")); yyjson_doc_free(d); finish(id);

    const char *failure = "{\"nodes\":[{\"id\":\"r\",\"task\":\"root\"},{\"id\":\"bad\",\"parent\":\"r\",\"task\":\"FAIL_NODE\"},{\"id\":\"ok\",\"parent\":\"r\",\"task\":\"ok\"},{\"id\":\"badleaf\",\"parent\":\"bad\",\"task\":\"leaf\"},{\"id\":\"okleaf\",\"parent\":\"ok\",\"task\":\"leaf\"}]}";
    handle(run(failure, true), id); d = control(id, "advance", NULL, true); yyjson_doc_free(d);
    d = control(id, "advance", NULL, true); CHECK(!strcmp(status(d, "badleaf"), "blocked") && workers == 1 && strstr(tasks[0], "node=okleaf")); yyjson_doc_free(d); finish(id);

    handle(run(tree, true), id); before = launches; bad_collect = true;
    d = control(id, "advance", NULL, false); CHECK(launches == before && !strcmp(status(d, "left"), "queued")); yyjson_doc_free(d);
    bad_collect = false; finish(id);
    deny_spawn = true; handle(run(tree, false), id); deny_spawn = false;
    d = control(id, "status", NULL, true); CHECK(yyjson_is_true(yyjson_obj_get(yyjson_doc_get_root(d), "complete"))); yyjson_doc_free(d);
    handle(run(branches, true), id); partial_spawn = true; before = aborts;
    d = control(id, "advance", NULL, false); CHECK(aborts == before + 1 && yyjson_is_true(yyjson_obj_get(yyjson_doc_get_root(d), "complete"))); yyjson_doc_free(d); partial_spawn = false;
    handle(run(branches, true), id); partial_spawn = deny_abort = true;
    d = control(id, "advance", NULL, false); CHECK(!yyjson_is_true(yyjson_obj_get(yyjson_doc_get_root(d), "complete"))); yyjson_doc_free(d);
    partial_spawn = deny_abort = false; d = control(id, "abort", NULL, true); yyjson_doc_free(d);

    handle(run(tree, true), id); long_output = true;
    d = control(id, "advance", NULL, true); CHECK(yyjson_is_true(yyjson_obj_get(node(d, "left"), "parent_context_truncated"))); yyjson_doc_free(d); long_output = false;
    d = control(id, "status", "root", true); const char *text = yyjson_get_str(yyjson_obj_get(node(d, "root"), "output"));
    CHECK(strlen(text) == 2046 && strlen(text) % 3 == 0); yyjson_doc_free(d); finish(id);

    handle(run(tree, true), id); binary_output = true;
    d = control(id, "advance", NULL, true); CHECK(strstr(tasks[0], "QQD/") && strstr(tasks[0], "base64")); yyjson_doc_free(d); binary_output = false;
    d = control(id, "status", "root", true);
    CHECK(!strcmp(yyjson_get_str(yyjson_obj_get(node(d, "root"), "output_encoding")), "base64"));
    CHECK(!strcmp(yyjson_get_str(yyjson_obj_get(node(d, "root"), "output")), "QQD/")); yyjson_doc_free(d); finish(id);

    /* A broad level is chunked, rather than oversubscribing or dropping nodes. */
    jbuf_t wide; jbuf_init(&wide, 8192); jbuf_append(&wide, "{\"max_concurrency\":8,\"nodes\":[{\"id\":\"r\",\"task\":\"root\"}");
    for (int i = 0; i < 63; i++) jbuf_appendf(&wide, ",{\"id\":\"n%d\",\"parent\":\"r\",\"task\":\"leaf-%d\"}", i, i);
    jbuf_append(&wide, "]}"); before = launches; handle(run(wide.data, true), id); finish(id);
    CHECK(launches == before + 9); d = control(id, "status", NULL, true);
    CHECK(yyjson_arr_size(yyjson_obj_get(yyjson_doc_get_root(d), "nodes")) == 64 && !strcmp(yyjson_get_str(yyjson_obj_get(yyjson_doc_get_root(d), "outcome")), "success")); yyjson_doc_free(d); jbuf_free(&wide);
    printf("swarm hierarchy: %d checks passed; validation, route pinning, dependencies, cancellation, failure isolation, slot recycling, bounded context, 64-node batching\n", checks);
    return 0;
}
