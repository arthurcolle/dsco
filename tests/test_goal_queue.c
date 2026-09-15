#include "goal_queue.h"
#include "../vendor/yyjson.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static bool apply(goal_queue_t *queue, const char *action, int task_id,
                  const char *tail, char *out, size_t out_len) {
    char input[4096];
    snprintf(input, sizeof(input),
             "{\"action\":\"%s\",\"task_id\":%d,\"revision\":%d%s}",
             action, task_id, queue->revision, tail ? tail : "");
    return goal_queue_apply(queue, input, out, out_len);
}

static void test_hierarchical_two_queue_flow(void) {
    goal_queue_t queue;
    char out[32768];
    goal_queue_init(&queue);
    assert(goal_queue_sync(&queue, "ship a verified runtime", 100, true));
    assert(queue.plan_count == 1 && queue.work_count == 0 && queue.root_id == 1);
    assert(goal_queue_prepare(&queue));
    assert(queue.current_id == queue.root_id);
    assert(queue.tasks[0].lane == GOAL_QUEUE_LANE_PLAN);

    char stale[1024];
    snprintf(stale, sizeof(stale),
             "{\"action\":\"decompose\",\"task_id\":1,\"revision\":%d,"
             "\"children\":[{\"title\":\"bad\",\"lane\":\"work\"}]}",
             queue.revision - 1);
    assert(!goal_queue_apply(&queue, stale, out, sizeof(out)));
    assert(strstr(out, "stale") != NULL);

    assert(apply(&queue, "checkpoint", 1,
                 ",\"evidence\":\"repository surfaces inspected\"", out, sizeof(out)));
    assert(strstr(out, "\"schema\":\"dsco.goal_queue.receipt.v1\"") != NULL);
    assert(strlen(out) < 512);
    assert(!apply(&queue, "checkpoint", 1,
                  ",\"evidence\":\"repository surfaces inspected\"", out, sizeof(out)));
    assert(strstr(out, "distinct from the prior checkpoint") != NULL);
    assert(strstr(out, "\"revision\":") != NULL && strstr(out, "\"current_id\":1") != NULL);

    assert(apply(&queue, "decompose", 1,
                 ",\"children\":["
                 "{\"title\":\"implement core\",\"lane\":\"work\","
                 "\"acceptance\":\"binary behavior exists\"},"
                 "{\"title\":\"verify integration\",\"lane\":\"plan\","
                 "\"acceptance\":\"all checks are current\"}]",
                 out, sizeof(out)));
    assert(queue.plan_count == 1 && queue.work_count == 1 && queue.current_id == 0);

    /* Fair selection alternates away from the just-run planning lane. */
    assert(goal_queue_prepare(&queue));
    assert(queue.current_id == 2);
    assert(queue.tasks[1].lane == GOAL_QUEUE_LANE_WORK);
    assert(apply(&queue, "complete", 2, ",\"evidence\":\"runtime check passed\"",
                 out, sizeof(out)));
    assert(queue.plan_count == 1 && queue.work_count == 0);

    assert(goal_queue_prepare(&queue));
    assert(queue.current_id == 3);
    assert(apply(&queue, "decompose", 3,
                 ",\"children\":[{\"title\":\"run live fixture\","
                 "\"lane\":\"work\",\"max_attempts\":2}]",
                 out, sizeof(out)));
    assert(queue.work_count == 1);

    assert(goal_queue_prepare(&queue));
    assert(queue.current_id == 4);
    assert(apply(&queue, "fail", 4,
                 ",\"evidence\":\"fixture exit 1\",\"reason\":\"transient lock\"",
                 out, sizeof(out)));
    assert(queue.work_count == 1 && queue.current_id == 0);
    assert(goal_queue_prepare(&queue));
    assert(queue.current_id == 4 && queue.tasks[3].attempts == 2);
    assert(apply(&queue, "complete", 4, ",\"evidence\":\"fixture exit 0\"",
                 out, sizeof(out)));

    /* Child-plan review wakes only after its own children are terminal. */
    assert(queue.plan_count == 1);
    assert(goal_queue_prepare(&queue));
    assert(queue.current_id == 3);
    assert(apply(&queue, "complete", 3,
                 ",\"evidence\":\"fixture and runtime checks agree\"", out, sizeof(out)));
    assert(queue.plan_count == 1);
    assert(goal_queue_prepare(&queue));
    assert(queue.current_id == 1);
    assert(apply(&queue, "complete", 1,
                 ",\"evidence\":\"all root criteria verified\"", out, sizeof(out)));
    assert(goal_queue_terminal_ready(&queue));
    assert(queue.plan_count == 0 && queue.work_count == 0 && queue.current_id == 0);
    assert(strstr(out, "\"terminal_ready\":true") != NULL);
}

static void test_model_tool_schema(void) {
    yyjson_doc *doc = yyjson_read(GOAL_QUEUE_TOOL_SCHEMA, strlen(GOAL_QUEUE_TOOL_SCHEMA), 0);
    assert(doc != NULL);
    yyjson_val *root = yyjson_doc_get_root(doc);
    assert(yyjson_is_obj(root));
    yyjson_val *properties = yyjson_obj_get(root, "properties");
    yyjson_val *evidence = yyjson_obj_get(properties, "evidence");
    assert(yyjson_get_uint(yyjson_obj_get(evidence, "maxLength")) == 511);
    yyjson_doc_free(doc);
}

static void test_retry_exhaustion_and_recovery(void) {
    goal_queue_t queue;
    char out[32768];
    goal_queue_init(&queue);
    assert(goal_queue_sync(&queue, "recover a failing leaf", 200, true));
    assert(goal_queue_prepare(&queue));
    assert(apply(&queue, "decompose", 1,
                 ",\"children\":[{\"title\":\"primary route\",\"lane\":\"work\","
                 "\"max_attempts\":2}]", out, sizeof(out)));
    for (int attempt = 0; attempt < 2; attempt++) {
        assert(goal_queue_prepare(&queue));
        assert(queue.current_id == 2);
        assert(apply(&queue, "fail", 2,
                     ",\"evidence\":\"same controlled failure\","
                     "\"reason\":\"route unavailable\"", out, sizeof(out)));
    }
    assert(queue.tasks[1].status == GOAL_QUEUE_TASK_FAILED);
    assert(queue.plan_count == 1); /* root reviews the exhausted child */
    assert(goal_queue_prepare(&queue));
    assert(queue.current_id == 1);
    assert(apply(&queue, "decompose", 1,
                 ",\"children\":[{\"title\":\"bounded fallback\",\"lane\":\"work\"}]",
                 out, sizeof(out)));
    assert(goal_queue_prepare(&queue));
    assert(queue.current_id == 3);
    assert(apply(&queue, "complete", 3, ",\"evidence\":\"fallback verified\"",
                 out, sizeof(out)));

    /* The recovered parent remains valid across a crash boundary.  Planning
     * reserves a later review lease before accepting another decomposition. */
    jbuf_t session;
    jbuf_init(&session, 4096);
    jbuf_append(&session, "{\"session\":true");
    goal_queue_save_fields(&session, &queue);
    jbuf_append(&session, "}");
    goal_queue_t loaded;
    assert(goal_queue_load_fields(&loaded, session.data));
    queue = loaded;
    jbuf_free(&session);

    assert(goal_queue_prepare(&queue));
    assert(queue.current_id == 1);
    assert(apply(&queue, "complete", 1,
                 ",\"evidence\":\"fallback satisfies root despite recorded failed attempt\"",
                 out, sizeof(out)));
    assert(goal_queue_terminal_ready(&queue));
}

static void test_plan_review_lease_is_reserved(void) {
    goal_queue_t queue;
    char out[32768];
    goal_queue_init(&queue);
    assert(goal_queue_sync(&queue, "preserve a final planning review", 250, true));
    queue.tasks[0].max_attempts = 1;
    assert(goal_queue_prepare(&queue));
    assert(!apply(&queue, "decompose", queue.root_id,
                  ",\"children\":[{\"title\":\"work\",\"lane\":\"work\"}]",
                  out, sizeof(out)));
    assert(strstr(out, "one review lease remaining") != NULL);
    assert(queue.current_id == queue.root_id && queue.task_count == 1);
}

static void test_block_and_persistence(void) {
    goal_queue_t blocked;
    char out[32768];
    goal_queue_init(&blocked);
    assert(goal_queue_sync(&blocked, "requires external authority", 300, true));
    assert(goal_queue_prepare(&blocked));
    assert(apply(&blocked, "block", blocked.root_id,
                 ",\"evidence\":\"server returned explicit denial\","
                 "\"reason\":\"operator credential required\"", out, sizeof(out)));
    assert(goal_queue_root_blocked(&blocked));
    assert(!strcmp(goal_queue_root_reason(&blocked), "operator credential required"));

    goal_queue_t active;
    goal_queue_init(&active);
    assert(goal_queue_sync(&active, "resume without duplicate lease", 400, true));
    assert(goal_queue_prepare(&active));
    int saved_revision = active.revision;
    jbuf_t session;
    jbuf_init(&session, 4096);
    jbuf_append(&session, "{\"session\":true");
    goal_queue_save_fields(&session, &active);
    jbuf_append(&session, "}");

    goal_queue_t loaded;
    assert(goal_queue_load_fields(&loaded, session.data));
    assert(loaded.current_id == 0);
    assert(loaded.plan_count == 1 && loaded.work_count == 0);
    assert(loaded.tasks[0].attempts == 0);
    assert(loaded.revision > saved_revision);
    assert(goal_queue_prepare(&loaded));
    assert(loaded.current_id == loaded.root_id);
    char prompt[GOAL_QUEUE_PROMPT_MAX];
    assert(goal_queue_make_context(&loaded, "resume without duplicate lease", "",
                                   prompt, sizeof(prompt)));
    assert(strstr(prompt, "Planning queue") != NULL);
    assert(strstr(prompt, "Selected PLANNING task") != NULL);
    jbuf_free(&session);
}

static void test_strict_inputs_and_root_invariant(void) {
    goal_queue_t queue;
    char out[32768];
    goal_queue_init(&queue);
    assert(goal_queue_sync(&queue, "cannot skip decomposition", 500, true));
    assert(goal_queue_prepare(&queue));
    char oversized[900];
    char evidence[600];
    memset(evidence, 'x', sizeof(evidence) - 1);
    evidence[sizeof(evidence) - 1] = '\0';
    snprintf(oversized, sizeof(oversized),
             "{\"action\":\"complete\",\"task_id\":1,\"revision\":%d,"
             "\"evidence\":\"%s\"}", queue.revision, evidence);
    assert(!goal_queue_apply(&queue, oversized, out, sizeof(out)));
    assert(strstr(out, "1..511 UTF-8 bytes") != NULL);
    assert(strstr(out, "\"current_id\":1") != NULL);
    assert(queue.current_id == 1 && queue.tasks[0].status == GOAL_QUEUE_TASK_ACTIVE);
    assert(!apply(&queue, "complete", 1, ",\"evidence\":\"mere assertion\"",
                  out, sizeof(out)));
    assert(strstr(out, "root must be decomposed") != NULL);
    assert(!goal_queue_apply(&queue, "{\"action\":\"status\",\"extra\":true}",
                             out, sizeof(out)));
    assert(!goal_queue_apply(&queue,
                             "{\"action\":\"status\",\"action\":\"complete\"}",
                             out, sizeof(out)));

    const char *forged_terminal =
        "{\"goal_queue\":{\"schema_version\":1,\"enabled\":true,"
        "\"objective_hash\":1,\"goal_started_at\":500,\"revision\":2,"
        "\"next_id\":2,\"root_id\":1,\"last_lane\":\"plan\",\"tasks\":[{"
        "\"id\":1,\"parent_id\":0,\"depth\":0,\"lane\":\"plan\","
        "\"status\":\"complete\",\"attempts\":1,\"max_attempts\":2,"
        "\"title\":\"forged root\",\"acceptance\":\"none\","
        "\"evidence\":\"asserted\",\"reason\":\"\"}]}}";
    goal_queue_t loaded;
    assert(!goal_queue_load_fields(&loaded, forged_terminal));
}

static void test_instruction_lifecycle_and_exact_fields(void) {
    goal_queue_t queue;
    char out[32768], prompt[GOAL_QUEUE_PROMPT_MAX];
    goal_queue_init(&queue);
    assert(goal_queue_sync(&queue, "capture only this window", 600, true));
    assert(goal_queue_prepare(&queue));
    int root_revision = queue.revision;
    assert(goal_queue_make_context(&queue, "capture only this window", "valid PNG",
                                   prompt, sizeof(prompt)));
    assert(queue.revision == root_revision); /* instruction generation is read-only */
    assert(strstr(prompt, "Root must be decomposed first") != NULL);
    assert(strstr(prompt, "verify the existing result rather than repeat side effects") != NULL);
    assert(strstr(prompt, "decompose: action, task_id, revision, children ONLY") != NULL);
    assert(strstr(prompt, "Never guess task IDs or increment revisions yourself") != NULL);
    assert(strstr(prompt, "no extra update_goal") != NULL);

    /* The observed mistake: decompose does NOT accept evidence, even though
     * evidence is in the union schema. Rejection preserves the active lease. */
    assert(!apply(&queue, "decompose", 1,
                  ",\"children\":[{\"title\":\"verify existing PNG\",\"lane\":\"work\"}],"
                  "\"evidence\":\"already captured\"", out, sizeof(out)));
    assert(queue.revision == root_revision && queue.current_id == 1 && queue.task_count == 1);
    assert(apply(&queue, "decompose", 1,
                 ",\"children\":[{\"title\":\"verify existing PNG\",\"lane\":\"work\"}]",
                 out, sizeof(out)));
    int receipt_revision = queue.revision;
    assert(queue.current_id == 0);
    assert(goal_queue_apply(&queue, "{\"action\":\"status\"}", out, sizeof(out)));
    assert(queue.current_id == 0 && queue.revision == receipt_revision);
    assert(goal_queue_make_context(&queue, "capture only this window", "",
                                   prompt, sizeof(prompt)));
    assert(strstr(prompt, "current_id=0 immediately after a transition is normal") != NULL);

    assert(goal_queue_prepare(&queue));
    assert(queue.current_id == 2 && queue.revision > receipt_revision);
    assert(goal_queue_make_context(&queue, "capture only this window", "",
                                   prompt, sizeof(prompt)));
    assert(strstr(prompt, "Leaf completion is not root completion") != NULL);
    assert(apply(&queue, "checkpoint", 2, ",\"evidence\":\"PNG reopened\"", out, sizeof(out)));
    assert(queue.current_id == 2); /* checkpoint changes revision, not lease */
    assert(!apply(&queue, "complete", 2,
                  ",\"evidence\":\"PNG verified\",\"reason\":\"done\"", out, sizeof(out)));
    assert(apply(&queue, "complete", 2, ",\"evidence\":\"PNG verified\"", out, sizeof(out)));
    assert(goal_queue_prepare(&queue));
    assert(queue.current_id == 1);
    assert(goal_queue_make_context(&queue, "capture only this window", "",
                                   prompt, sizeof(prompt)));
    assert(strstr(prompt, "Review child evidence") != NULL);
    assert(strstr(prompt, "Root must be decomposed first") == NULL);
    assert(apply(&queue, "complete", 1,
                 ",\"evidence\":\"PNG meets the requested scope\"", out, sizeof(out)));
    assert(goal_queue_make_context(&queue, "capture only this window", "",
                                   prompt, sizeof(prompt)));
    assert(strstr(prompt, "without another update_goal") != NULL);
    assert(strstr(prompt, "Only for recovery") != NULL);
}

int main(void) {
    test_model_tool_schema();
    test_instruction_lifecycle_and_exact_fields();
    test_hierarchical_two_queue_flow();
    test_retry_exhaustion_and_recovery();
    test_plan_review_lease_is_reserved();
    test_block_and_persistence();
    test_strict_inputs_and_root_invariant();
    puts("goal queue: hierarchical planning/work flow, retries, persistence, and strict transitions passed");
    return 0;
}
