#include "task_profile_tool.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static topology_t topologies[2];

const topology_t *topology_registry(int *count) {
    if (count)
        *count = 2;
    return topologies;
}

bool topology_is_runnable(const topology_t *topology) {
    return topology != NULL;
}

const topology_t *topology_find(const char *name) {
    for (size_t i = 0; i < 2; i++) {
        if (strcmp(topologies[i].name, name) == 0)
            return &topologies[i];
    }
    return NULL;
}

static void setup_topologies(void) {
    memset(topologies, 0, sizeof(topologies));
    strcpy(topologies[0].name, "triage");
    strcpy(topologies[0].description, "general triage");
    topologies[0].category = CAT_SPECIALIST;
    topologies[0].est_latency_mult = 1.0;
    topologies[0].est_cost_1k = 1.0;
    topologies[0].total_agents = 1;
    strcpy(topologies[1].name, "fanout_balance");
    strcpy(topologies[1].description, "parallel fanout");
    topologies[1].category = CAT_FANOUT;
    topologies[1].est_latency_mult = 2.0;
    topologies[1].est_cost_1k = 2.0;
    topologies[1].total_agents = 4;
}

static void test_decode_defaults(void) {
    json_view_t *view = json_view_open("{\"task\":\"inspect\"}");
    task_profile_tool_request_t request;
    assert(view);
    assert(task_profile_tool_request_decode(view, &request));
    assert(strcmp(request.task, "inspect") == 0);
    assert(!request.explain);
    json_view_close(view);
}

static void test_wire_and_escaping(void) {
    char out[8192];
    assert(task_profile_tool_execute("{\"task\":\"inspect \\\"quoted\\\" \\\\ path\"}", out,
                                     sizeof(out)));
    assert(strstr(out, "\\\"quoted\\\"") != NULL);
    assert(strstr(out, "\\\\ path") != NULL);

    assert(task_profile_tool_execute("{\"task\":\"analyze and review\",\"explain\":true}",
                                     out, sizeof(out)));
    assert(strstr(out, "\"explanation\":") != NULL);
    assert(strstr(out, "\"profile\":") != NULL);
}

static void test_errors_and_capacity(void) {
    char out[128];
    assert(!task_profile_tool_execute("{}", out, sizeof(out)));
    assert(strcmp(out, "{\"error\":\"task string is required\"}") == 0);
    assert(!task_profile_tool_execute("{\"task\":\"\"}", out, sizeof(out)));
    assert(strcmp(out, "{\"error\":\"task string is required\"}") == 0);

    char one[1] = {'x'};
    assert(!task_profile_tool_execute("{\"task\":\"inspect\"}", one, sizeof(one)));
    assert(one[0] == '\0');
    char tiny[8] = {'x'};
    assert(!task_profile_tool_execute("{\"task\":\"inspect\"}", tiny, sizeof(tiny)));
    assert(tiny[sizeof(tiny) - 1] == '\0' || tiny[0] == '\0');
    assert(!task_profile_tool_execute("{\"task\":\"inspect\"}", NULL, 0));
}

static void test_cleanup(void) {
    task_profile_tool_result_t result;
    task_profile_tool_result_init(&result);
    result.profile = task_profile("inspect", NULL);
    result.explanation = malloc(8);
    assert(result.profile && result.explanation);
    task_profile_tool_result_free(&result);
    assert(!result.profile && !result.explanation);
}

int main(void) {
    setup_topologies();
    test_decode_defaults();
    test_wire_and_escaping();
    test_errors_and_capacity();
    test_cleanup();
    puts("test_task_profile_tool: ok");
    return 0;
}
