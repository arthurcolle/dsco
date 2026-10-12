#include "parallel_pricing.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static double per_1000(double rate, size_t units) {
    return rate * (double)units / 1000.0;
}

static int is_name_or_fast(const char *value, const char *name) {
    size_t n;
    if (!value || !name)
        return 0;
    if (!strcmp(value, name))
        return 1;
    n = strlen(name);
    if (strlen(value) == n + 5 && !strncmp(value, name, n) &&
        !strcmp(value + n, "-fast"))
        return 1;
    return strlen(value) == n + 5 && !strncmp(value, "fast-", 5) &&
           !strcmp(value + 5, name);
}

double parallel_ai_task_rate_per_1000(const char *processor) {
    const char *name = processor && processor[0] ? processor : "base";
    if (is_name_or_fast(name, "lite")) return 5.0;
    if (is_name_or_fast(name, "base")) return 10.0;
    if (is_name_or_fast(name, "core")) return 25.0;
    if (is_name_or_fast(name, "core2x")) return 50.0;
    if (is_name_or_fast(name, "pro")) return 100.0;
    if (is_name_or_fast(name, "ultra")) return 300.0;
    if (is_name_or_fast(name, "ultra2") || is_name_or_fast(name, "ultra2x"))
        return 600.0;
    if (is_name_or_fast(name, "ultra4") || is_name_or_fast(name, "ultra4x"))
        return 1200.0;
    if (is_name_or_fast(name, "ultra8") || is_name_or_fast(name, "ultra8x"))
        return 2400.0;
    return 0.0;
}

double parallel_ai_search_rate_per_1000(const char *mode) {
    if (!mode || !strcmp(mode, "turbo") || !strcmp(mode, "fast") ||
        !strcmp(mode, "fast1") || !strcmp(mode, "fast-1")) return 1.0;
    if (!strcmp(mode, "basic") || !strcmp(mode, "advanced")) return 5.0;
    return 0.0;
}

double parallel_ai_responses_rate_per_1000(const char *reasoning_effort) {
    if (!reasoning_effort || !strcmp(reasoning_effort, "medium")) return 50.0;
    if (!strcmp(reasoning_effort, "low")) return 10.0;
    if (!strcmp(reasoning_effort, "high")) return 250.0;
    return 0.0;
}

double parallel_ai_monitor_rate_per_1000(const char *processor) {
    if (!processor || !strcmp(processor, "lite")) return 3.0;
    if (!strcmp(processor, "base")) return 10.0;
    return 0.0;
}

double parallel_ai_chat_rate_per_1000(const char *model) {
    if (!model || !strcmp(model, "speed")) return 5.0;
    if (!strcmp(model, "lite") || !strcmp(model, "research-lite")) return 5.0;
    if (!strcmp(model, "base") || !strcmp(model, "research-base")) return 10.0;
    if (!strcmp(model, "core") || !strcmp(model, "research-core")) return 25.0;
    return 0.0;
}

int parallel_ai_findall_rates(const char *generator, double *fixed_usd,
                              double *per_match_usd) {
    double fixed, per_match;
    if (!generator || !strcmp(generator, "base")) {
        fixed = 0.25; per_match = 0.03;
    } else if (!strcmp(generator, "preview")) {
        fixed = 0.10; per_match = 0.0;
    } else if (!strcmp(generator, "core")) {
        fixed = 2.0; per_match = 0.15;
    } else if (!strcmp(generator, "pro")) {
        fixed = 10.0; per_match = 1.0;
    } else {
        return 0;
    }
    if (fixed_usd) *fixed_usd = fixed;
    if (per_match_usd) *per_match_usd = per_match;
    return 1;
}

double parallel_ai_entity_search_rate_per_1000(void) { return 5.0; }
double parallel_ai_entity_search_additional_rate_per_1000(void) { return 0.05; }

double parallel_ai_task_cost_usd(const char *processor, size_t successful_runs) {
    double rate = parallel_ai_task_rate_per_1000(processor);
    return rate > 0.0 ? per_1000(rate, successful_runs) : NAN;
}

double parallel_ai_search_cost_usd(const char *mode, size_t requests,
                                   const size_t *result_counts,
                                   size_t result_count) {
    double rate = parallel_ai_search_rate_per_1000(mode);
    if (rate <= 0.0 || (requests > 0 && !result_counts) ||
        result_count != requests)
        return NAN;
    size_t additional_results = 0;
    for (size_t i = 0; i < requests; i++) {
        if (result_counts[i] > 10)
            additional_results += result_counts[i] - 10;
    }
    return per_1000(rate, requests) + per_1000(1.0, additional_results);
}

double parallel_ai_extract_cost_usd(size_t urls) { return per_1000(1.0, urls); }

double parallel_ai_responses_cost_usd(const char *reasoning_effort,
                                      size_t successful_requests) {
    double rate = parallel_ai_responses_rate_per_1000(reasoning_effort);
    return rate > 0.0 ? per_1000(rate, successful_requests) : NAN;
}

double parallel_ai_monitor_cost_usd(const char *processor, size_t checks) {
    double rate = parallel_ai_monitor_rate_per_1000(processor);
    return rate > 0.0 ? per_1000(rate, checks) : NAN;
}

double parallel_ai_findall_cost_usd(const char *generator, size_t matches) {
    double fixed, per_match;
    if (!parallel_ai_findall_rates(generator, &fixed, &per_match))
        return NAN;
    return fixed + per_match * (double)matches;
}

double parallel_ai_entity_search_cost_usd(size_t requests,
                                           const size_t *result_counts,
                                           size_t result_count) {
    if ((requests > 0 && !result_counts) || result_count != requests)
        return NAN;
    size_t additional_results = 0;
    for (size_t i = 0; i < requests; i++) {
        if (result_counts[i] > 100)
            additional_results += result_counts[i] - 100;
    }
    return per_1000(parallel_ai_entity_search_rate_per_1000(), requests) +
           per_1000(parallel_ai_entity_search_additional_rate_per_1000(),
                    additional_results);
}

double parallel_ai_chat_cost_usd(const char *model, size_t requests) {
    double rate = parallel_ai_chat_rate_per_1000(model);
    return rate > 0.0 ? per_1000(rate, requests) : NAN;
}

int parallel_ai_pricing_catalog_json(char *out, size_t out_len) {
    int n;
    if (!out || out_len == 0)
        return 0;
    n = snprintf(
        out, out_len,
        "{\"type\":\"parallel_ai_upstream_pricing\","
        "\"currency\":\"USD\",\"pricing_scope\":\"upstream_reference\","
        "\"source\":\"%s\","
        "\"task\":{\"unit\":\"successful_run\",\"per_1000_usd\":{"
        "\"lite\":%.10g,\"base\":%.10g,\"core\":%.10g,"
        "\"core2x\":%.10g,\"pro\":%.10g,\"ultra\":%.10g,"
        "\"ultra2x\":%.10g,\"ultra4x\":%.10g,\"ultra8x\":%.10g},"
        "\"fast_aliases\":\"prefix_or_suffix_same_as_standard\"},"
        "\"search\":{\"unit\":\"request\",\"per_1000_usd\":{"
        "\"turbo\":%.10g,\"fast\":%.10g,\"basic\":%.10g,"
        "\"advanced\":%.10g},\"included_results\":10,"
        "\"result_counts\":\"one_per_request\","
        "\"additional_result_per_1000_usd\":1},"
        "\"extract\":{\"unit\":\"url\",\"per_1000_usd\":1},"
        "\"responses\":{\"unit\":\"successful_request\",\"per_1000_usd\":{"
        "\"low\":%.10g,\"medium\":%.10g,\"high\":%.10g}},"
        "\"monitor\":{\"unit\":\"check\",\"per_1000_usd\":{"
        "\"lite\":%.10g,\"base\":%.10g}},"
        "\"findall\":{\"unit\":\"run_plus_match\",\"fixed_usd\":{"
        "\"preview\":%.10g,\"base\":%.10g,\"core\":%.10g,"
        "\"pro\":%.10g},\"per_match_usd\":{\"preview\":0,"
        "\"base\":%.10g,\"core\":%.10g,\"pro\":%.10g}},"
        "\"entity_search\":{\"unit\":\"request\",\"per_1000_usd\":%.10g,"
        "\"included_results\":100,\"result_counts\":\"one_per_request\","
        "\"additional_result_per_1000_usd\":%.10g},"
        "\"chat\":{\"unit\":\"request\",\"per_1000_usd\":{"
        "\"speed\":%.10g,\"research-lite\":%.10g,"
        "\"research-base\":%.10g,\"research-core\":%.10g}}}",
        PARALLEL_AI_PRICING_SOURCE,
        parallel_ai_task_rate_per_1000("lite"),
        parallel_ai_task_rate_per_1000("base"),
        parallel_ai_task_rate_per_1000("core"),
        parallel_ai_task_rate_per_1000("core2x"),
        parallel_ai_task_rate_per_1000("pro"),
        parallel_ai_task_rate_per_1000("ultra"),
        parallel_ai_task_rate_per_1000("ultra2x"),
        parallel_ai_task_rate_per_1000("ultra4x"),
        parallel_ai_task_rate_per_1000("ultra8x"),
        parallel_ai_search_rate_per_1000("turbo"),
        parallel_ai_search_rate_per_1000("fast"),
        parallel_ai_search_rate_per_1000("basic"),
        parallel_ai_search_rate_per_1000("advanced"),
        parallel_ai_responses_rate_per_1000("low"),
        parallel_ai_responses_rate_per_1000("medium"),
        parallel_ai_responses_rate_per_1000("high"),
        parallel_ai_monitor_rate_per_1000("lite"),
        parallel_ai_monitor_rate_per_1000("base"),
        0.10, 0.25, 2.0, 10.0, 0.03, 0.15, 1.0,
        parallel_ai_entity_search_rate_per_1000(),
        parallel_ai_entity_search_additional_rate_per_1000(),
        parallel_ai_chat_rate_per_1000("speed"),
        parallel_ai_chat_rate_per_1000("research-lite"),
        parallel_ai_chat_rate_per_1000("research-base"),
        parallel_ai_chat_rate_per_1000("research-core"));
    if (n < 0 || (size_t)n >= out_len) {
        out[0] = '\0';
        return 0;
    }
    return n;
}
