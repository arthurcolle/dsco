#include "openrouter_lanes.h"
#include "openrouter_cache.h"

#include <stdio.h>
#include <string.h>

static int require(int ok, const char *msg) {
    if (ok)
        return 0;
    fprintf(stderr, "FAIL: %s\n", msg);
    return 1;
}

int main(void) {
    int failures = 0;
    failures += require(openrouter_cache_load_sync() > 0, "catalog loads");

    openrouter_lane_t lanes[64];
    openrouter_lane_query_t q = {
        .min_context = 131072,
        .min_quality = 20.0,
        .max_models = 16,
        .require_tools = true,
        .task = "agentic coding implementation",
    };
    int n = openrouter_lanes_build(&q, lanes, 64);
    failures += require(n > 1, "multiple model swimlanes materialize");
    for (int i = 0; i < n; i++) {
        failures += require(lanes[i].model[0] != '\0', "lane has model");
        failures += require(lanes[i].tool_capable, "lane supports tools");
        failures += require(lanes[i].context_window >= 131072, "context floor enforced");
        failures += require(lanes[i].quality >= 20.0, "quality floor enforced");
    }

    q.max_models = 1;
    q.endpoints_per_model = 3;
    int ep = openrouter_lanes_build(&q, lanes, 64);
    failures += require(ep > 0, "endpoint unroll returns lanes");
    failures += require(lanes[0].tag[0] != '\0', "endpoint lane has stable tag");

    if (failures)
        return 1;
    printf("PASS: %d model lanes; %d fully-unrolled endpoint lanes\n", n, ep);
    return 0;
}
