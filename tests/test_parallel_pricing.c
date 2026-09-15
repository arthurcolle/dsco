#include "parallel_pricing.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static void near(double actual, double expected) {
    assert(fabs(actual - expected) < 1e-9);
}

int main(void) {
    near(parallel_ai_task_rate_per_1000("lite"), 5.0);
    near(parallel_ai_task_rate_per_1000("base-fast"), 10.0);
    near(parallel_ai_task_rate_per_1000("core2x"), 50.0);
    near(parallel_ai_task_rate_per_1000("ultra8x-fast"), 2400.0);
    near(parallel_ai_task_rate_per_1000(NULL), 10.0);
    near(parallel_ai_task_cost_usd("pro", 3), 0.30);
    assert(parallel_ai_task_rate_per_1000("unknown") == 0.0);
    assert(isnan(parallel_ai_task_cost_usd("unknown", 1)));

    near(parallel_ai_search_rate_per_1000("turbo"), 1.0);
    near(parallel_ai_search_rate_per_1000("fast"), 1.0);
    near(parallel_ai_search_rate_per_1000("basic"), 5.0);
    near(parallel_ai_search_rate_per_1000("advanced"), 5.0);
    const size_t search_20_0[] = {20, 0};
    const size_t search_10_10[] = {10, 10};
    near(parallel_ai_search_cost_usd("basic", 2, search_20_0, 2), 0.020);
    near(parallel_ai_search_cost_usd("basic", 2, search_10_10, 2), 0.010);
    assert(isnan(parallel_ai_search_cost_usd("basic", 2, NULL, 0)));
    assert(isnan(parallel_ai_search_cost_usd("unknown", 1, search_20_0, 1)));
    near(parallel_ai_extract_cost_usd(3), 0.003);

    near(parallel_ai_responses_rate_per_1000("low"), 10.0);
    near(parallel_ai_responses_rate_per_1000("medium"), 50.0);
    near(parallel_ai_responses_rate_per_1000("high"), 250.0);
    near(parallel_ai_responses_cost_usd("high", 2), 0.50);

    near(parallel_ai_monitor_rate_per_1000("lite"), 3.0);
    near(parallel_ai_monitor_rate_per_1000("base"), 10.0);
    near(parallel_ai_monitor_cost_usd("lite", 1000), 3.0);

    near(parallel_ai_chat_rate_per_1000("speed"), 5.0);
    near(parallel_ai_chat_rate_per_1000("research-lite"), 5.0);
    near(parallel_ai_chat_rate_per_1000("research-base"), 10.0);
    near(parallel_ai_chat_rate_per_1000("research-core"), 25.0);

    double fixed, match;
    assert(parallel_ai_findall_rates("preview", &fixed, &match));
    near(fixed, 0.10); near(match, 0.0);
    assert(parallel_ai_findall_rates("base", &fixed, &match));
    near(fixed, 0.25); near(match, 0.03);
    near(parallel_ai_findall_cost_usd("core", 10), 3.50);
    near(parallel_ai_findall_cost_usd("pro", 2), 12.0);

    near(parallel_ai_entity_search_rate_per_1000(), 5.0);
    near(parallel_ai_entity_search_additional_rate_per_1000(), 0.05);
    const size_t entity_150_0[] = {150, 0};
    const size_t entity_75_75[] = {75, 75};
    near(parallel_ai_entity_search_cost_usd(2, entity_150_0, 2), 0.0125);
    near(parallel_ai_entity_search_cost_usd(2, entity_75_75, 2), 0.0100);
    assert(isnan(parallel_ai_entity_search_cost_usd(2, NULL, 0)));

    near(parallel_ai_task_rate_per_1000("fast-ultra2x"), 600.0);
    near(parallel_ai_task_rate_per_1000("fast-ultra2"), 600.0);
    near(parallel_ai_task_rate_per_1000("ultra2x-fast"), 600.0);
    near(parallel_ai_search_rate_per_1000("fast1"), 1.0);
    near(parallel_ai_search_rate_per_1000("fast-1"), 1.0);

    char catalog[8192];
    assert(parallel_ai_pricing_catalog_json(catalog, sizeof(catalog)) > 0);
    assert(strstr(catalog, "parallel_ai_upstream_pricing") != NULL);
    assert(strstr(catalog, "pricing_scope\":\"upstream_reference") != NULL);
    assert(strstr(catalog, "additional_result_per_1000_usd\":1") != NULL);
    assert(strstr(catalog, "result_counts\":\"one_per_request\"") != NULL);
    assert(parallel_ai_pricing_catalog_json(catalog, 8) == 0);
    assert(catalog[0] == '\0');
    puts("parallel pricing: upstream rates and unit formulas passed");
    return 0;
}
