#include "subscription_bench.h"

#include "config.h"
#include "json_util.h"
#include "llm.h"
#include "provider.h"
#include "provider_profiles.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SUB_BENCH_MAX_ROUNDS 20
#define SUB_BENCH_MAX_CONCURRENCY 16

static const dsco_subscription_lane_spec_t SUBSCRIPTION_LANES[] = {
    {"anthropic", "claude-sonnet-5", "Claude Max", "claude-code-oauth"},
    {"openai-codex", DEFAULT_MODEL, "ChatGPT Codex", "chatgpt-subscription"},
    {"kimi-code", KIMI_CODE_DEFAULT_MODEL, "Kimi Code", "kimi-code-subscription"},
    {"sakana", "fugu", "Sakana Fugu", "sakana-subscription-api-key"},
    {"zai", "glm-5.2", "Z.AI Coding Plan", "zai-coding-plan-api-key"},
};

typedef struct {
    bool ok;
    bool output_tokens_estimated;
    bool throughput_reliable;
    int http_status;
    int input_tokens;
    int output_tokens;
    int reasoning_tokens;
    int decode_tokens;
    size_t request_bytes;
    size_t output_chars;
    double queue_ms;
    double ttft_ms;
    double total_ms;
    double tokens_per_sec;
    double dns_ms;
    double connect_ms;
    double tls_ms;
    double ttfb_ms;
    long new_connections;
    char stop_reason[64];
} subscription_sample_t;

typedef struct {
    const dsco_subscription_lane_spec_t *lane;
    const char *prompt;
    char *credential;
    int max_tokens;
    provider_t *provider;
    subscription_sample_t *sample;
    double started_ms;
    double first_text_ms;
    size_t streamed_chars;
    int streamed_events;
} subscription_task_t;

typedef struct {
    const dsco_subscription_lane_spec_t *spec;
    bool ready;
    char auth_mode[64];
    char endpoint[512];
    char reason[96];
    char *credential;
} subscription_runtime_lane_t;

static double subscription_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
}

size_t dsco_subscription_lane_count(void) {
    return sizeof(SUBSCRIPTION_LANES) / sizeof(SUBSCRIPTION_LANES[0]);
}

const dsco_subscription_lane_spec_t *dsco_subscription_lane_at(size_t index) {
    return index < dsco_subscription_lane_count() ? &SUBSCRIPTION_LANES[index] : NULL;
}

static const char *subscription_lane_credential(const dsco_subscription_lane_spec_t *lane,
                                                const char *fallback_api_key) {
    if (!lane)
        return NULL;
    /* A Sakana benchmark must stay on the subscription allocation even when
     * the ordinary request resolver is temporarily preferring PAYG. */
    if (strcmp(lane->provider, "sakana") == 0)
        return provider_sakana_subscription_request_key();
    return provider_resolve_request_api_key(lane->provider, fallback_api_key);
}

static void subscription_copy(char *out, size_t out_len, const char *value) {
    if (!out || out_len == 0)
        return;
    snprintf(out, out_len, "%s", value ? value : "");
}

bool dsco_subscription_lane_native_ready(const dsco_subscription_lane_spec_t *lane,
                                         const char *fallback_api_key,
                                         char *auth_mode, size_t auth_mode_len,
                                         char *endpoint, size_t endpoint_len,
                                         char *reason, size_t reason_len) {
    subscription_copy(auth_mode, auth_mode_len, "missing");
    subscription_copy(endpoint, endpoint_len, "");
    subscription_copy(reason, reason_len, "unknown");
    if (!lane) {
        subscription_copy(reason, reason_len, "invalid_lane");
        return false;
    }

    const provider_profile_t *profile = provider_profile_find(lane->provider);
    if (!profile || !provider_profile_transport_supported(profile)) {
        subscription_copy(reason, reason_len, "native_transport_unimplemented");
        return false;
    }

    const char *credential = subscription_lane_credential(lane, fallback_api_key);
    if (!credential || !credential[0]) {
        subscription_copy(reason, reason_len, "subscription_credential_missing");
        return false;
    }

    const char *mode = provider_auth_mode(lane->provider, credential);
    subscription_copy(auth_mode, auth_mode_len, mode);
    if (!provider_usage_is_included(lane->provider, credential) ||
        strcmp(mode, lane->expected_auth_mode) != 0) {
        subscription_copy(reason, reason_len, "metered_or_wrong_auth_class");
        return false;
    }

    provider_t *provider = provider_create(lane->provider);
    if (!provider) {
        subscription_copy(reason, reason_len, "provider_unavailable");
        return false;
    }
    const char *url = provider->api_url;
    subscription_copy(endpoint, endpoint_len, url);
    bool native_http = url && (strncmp(url, "https://", 8) == 0 ||
                               strncmp(url, "http://", 7) == 0);
    provider_free(provider);
    if (!native_http) {
        subscription_copy(reason, reason_len, "external_executor_only");
        return false;
    }

    subscription_copy(reason, reason_len, "ready");
    return true;
}

static void subscription_append_lane_identity(jbuf_t *json,
                                              const subscription_runtime_lane_t *lane) {
    jbuf_append(json, "{\"label\":");
    jbuf_append_json_str(json, lane->spec->label);
    jbuf_append(json, ",\"provider\":");
    jbuf_append_json_str(json, lane->spec->provider);
    jbuf_append(json, ",\"model\":");
    jbuf_append_json_str(json, lane->spec->model);
    jbuf_append(json, ",\"transport\":\"native_http\",\"auth_mode\":");
    jbuf_append_json_str(json, lane->auth_mode);
    jbuf_append(json, ",\"endpoint\":");
    jbuf_append_json_str(json, lane->endpoint);
    jbuf_append(json, ",\"ready\":");
    jbuf_append(json, lane->ready ? "true" : "false");
    jbuf_append(json, ",\"reason\":");
    jbuf_append_json_str(json, lane->reason);
}

static int subscription_runtime_inventory(subscription_runtime_lane_t *lanes, size_t count,
                                          const char *fallback_api_key) {
    int ready = 0;
    for (size_t i = 0; i < count; i++) {
        memset(&lanes[i], 0, sizeof(lanes[i]));
        lanes[i].spec = dsco_subscription_lane_at(i);
        lanes[i].ready = dsco_subscription_lane_native_ready(
            lanes[i].spec, fallback_api_key, lanes[i].auth_mode, sizeof(lanes[i].auth_mode),
            lanes[i].endpoint, sizeof(lanes[i].endpoint), lanes[i].reason,
            sizeof(lanes[i].reason));
        if (lanes[i].ready) {
            const char *credential = subscription_lane_credential(lanes[i].spec, fallback_api_key);
            lanes[i].credential = credential ? strdup(credential) : NULL;
            if (!lanes[i].credential) {
                lanes[i].ready = false;
                subscription_copy(lanes[i].reason, sizeof(lanes[i].reason),
                                  "credential_copy_failed");
            } else {
                ready++;
            }
        }
    }
    return ready;
}

static void subscription_runtime_inventory_free(subscription_runtime_lane_t *lanes,
                                                size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (lanes[i].credential) {
            memset(lanes[i].credential, 0, strlen(lanes[i].credential));
            free(lanes[i].credential);
            lanes[i].credential = NULL;
        }
    }
}

int dsco_subscription_lanes_print_json(FILE *out, const char *fallback_api_key) {
    if (!out)
        return 1;
    size_t count = dsco_subscription_lane_count();
    subscription_runtime_lane_t *lanes = calloc(count, sizeof(*lanes));
    if (!lanes)
        return 1;
    int ready = subscription_runtime_inventory(lanes, count, fallback_api_key);

    jbuf_t json;
    jbuf_init(&json, 4096);
    jbuf_append(&json, "{\"schema\":\"dsco.subscription_lanes.v1\","
                       "\"tier\":1,\"native_only\":true,\"ready_count\":");
    jbuf_append_int(&json, ready);
    jbuf_append(&json, ",\"lanes\":[");
    for (size_t i = 0; i < count; i++) {
        if (i)
            jbuf_append(&json, ",");
        subscription_append_lane_identity(&json, &lanes[i]);
        jbuf_append(&json, "}");
    }
    jbuf_append(&json, "]}");
    fprintf(out, "%s\n", json.data ? json.data : "{}");
    jbuf_free(&json);
    subscription_runtime_inventory_free(lanes, count);
    free(lanes);
    return ready > 0 ? 0 : 1;
}

static void subscription_count_text(const char *text, void *ctx) {
    subscription_task_t *task = ctx;
    if (!task || !text)
        return;
    if (!text[0])
        return;
    if (task->first_text_ms <= 0.0)
        task->first_text_ms = subscription_now_ms();
    task->streamed_chars += strlen(text);
    task->streamed_events++;
}

static void *subscription_bench_worker(void *arg) {
    subscription_task_t *task = arg;
    subscription_sample_t *sample = task ? task->sample : NULL;
    if (!task || !sample)
        return NULL;
    memset(sample, 0, sizeof(*sample));

    provider_t *provider = task->provider;
    if (!provider) {
        subscription_copy(sample->stop_reason, sizeof(sample->stop_reason),
                          "provider_unavailable");
        return NULL;
    }

    session_state_t session;
    session_state_init(&session, task->lane->model);
    session.fallback_count = 0;
    session.direct_answer_mode = true;
    session.web_search = false;
    session.code_execution = false;
    snprintf(session.tool_choice, sizeof(session.tool_choice), "none");

    conversation_t conversation;
    conv_init(&conversation);
    conv_add_user_text(&conversation, task->prompt);
    char *request = provider->build_request(
        provider, &conversation, &session, task->max_tokens, task->credential);
    if (!request) {
        subscription_copy(sample->stop_reason, sizeof(sample->stop_reason),
                          "request_build_failed");
        conv_free(&conversation);
        return NULL;
    }
    sample->request_bytes = strlen(request);

    task->started_ms = subscription_now_ms();
    stream_result_t result = provider_stream_reuse(
        provider, task->credential, request, subscription_count_text, NULL, NULL, NULL, task);
    double wall_ms = subscription_now_ms() - task->started_ms;
    sample->ok = result.ok;
    sample->http_status = result.http_status;
    sample->input_tokens = result.usage.input_tokens;
    sample->output_tokens = result.usage.output_tokens;
    sample->reasoning_tokens = result.reasoning_tokens;
    sample->output_chars = task->streamed_chars;
    if (sample->output_tokens <= 0 && sample->output_chars > 0) {
        sample->output_tokens = (int)((sample->output_chars + 3) / 4);
        sample->output_tokens_estimated = true;
    }
    if (sample->reasoning_tokens < 0 || sample->reasoning_tokens > sample->output_tokens)
        sample->reasoning_tokens = 0;
    sample->decode_tokens = sample->output_tokens - sample->reasoning_tokens;
    sample->queue_ms = (double)provider_last_subscription_queue_ms();
    if (sample->queue_ms < 0.0)
        sample->queue_ms = 0.0;
    if (sample->queue_ms > wall_ms)
        sample->queue_ms = wall_ms;
    if (task->first_text_ms > 0.0)
        sample->ttft_ms = task->first_text_ms - task->started_ms;
    sample->total_ms = wall_ms;
    sample->dns_ms = result.telemetry.latency.dns_ms;
    sample->connect_ms = result.telemetry.latency.connect_ms;
    sample->tls_ms = result.telemetry.latency.tls_ms;
    sample->ttfb_ms = result.telemetry.latency.ttfb_ms;
    sample->new_connections = result.telemetry.latency.new_connections;
    double decode_ms = sample->total_ms - sample->ttft_ms;
    /* A single buffered callback is not a decode-rate sample. Requiring
     * provider-reported usage, multiple deltas, and a measurable interval
     * prevents tiny final chunks from producing absurd multi-kilohertz TPS. */
    sample->throughput_reliable = !sample->output_tokens_estimated &&
                                  task->streamed_events >= 2 && decode_ms >= 20.0 &&
                                  sample->decode_tokens > 0;
    if (sample->throughput_reliable) {
        sample->tokens_per_sec = sample->decode_tokens / (decode_ms / 1000.0);
    }
    subscription_copy(sample->stop_reason, sizeof(sample->stop_reason),
                      result.parsed.stop_reason ? result.parsed.stop_reason
                                                : (result.ok ? "complete" : "request_failed"));

    free(result.actual_model);
    free(result.generation_id);
    json_free_response(&result.parsed);
    free(request);
    conv_free(&conversation);
    return NULL;
}

static int subscription_clamp(int value, int fallback, int minimum, int maximum) {
    if (value <= 0)
        value = fallback;
    if (value < minimum)
        value = minimum;
    if (value > maximum)
        value = maximum;
    return value;
}

int dsco_subscription_bench_run(FILE *out,
                                const dsco_subscription_bench_options_t *options) {
    if (!out)
        return 1;
    const char *prompt = options && options->prompt && options->prompt[0]
                             ? options->prompt
                             : "Reply with exactly OK.";
    const char *fallback_api_key = options ? options->fallback_api_key : NULL;
    int rounds = subscription_clamp(options ? options->rounds : 0, 1, 1,
                                    SUB_BENCH_MAX_ROUNDS);
    int concurrency = subscription_clamp(options ? options->concurrency_per_lane : 0, 1, 1,
                                         SUB_BENCH_MAX_CONCURRENCY);
    int max_tokens = subscription_clamp(options ? options->max_tokens : 0, 64, 16, 4096);

    size_t lane_count = dsco_subscription_lane_count();
    subscription_runtime_lane_t *lanes = calloc(lane_count, sizeof(*lanes));
    if (!lanes)
        return 1;
    int ready_count = subscription_runtime_inventory(lanes, lane_count, fallback_api_key);
    if (ready_count <= 0) {
        subscription_runtime_inventory_free(lanes, lane_count);
        free(lanes);
        return dsco_subscription_lanes_print_json(out, fallback_api_key);
    }

    size_t samples_per_round = (size_t)ready_count * (size_t)concurrency;
    size_t sample_count = samples_per_round * (size_t)rounds;
    subscription_sample_t *samples = calloc(sample_count, sizeof(*samples));
    size_t *sample_lanes = calloc(sample_count, sizeof(*sample_lanes));
    int *sample_rounds = calloc(sample_count, sizeof(*sample_rounds));
    pthread_t *threads = calloc(samples_per_round, sizeof(*threads));
    subscription_task_t *tasks = calloc(samples_per_round, sizeof(*tasks));
    bool *thread_started = calloc(samples_per_round, sizeof(*thread_started));
    provider_t **providers = calloc(samples_per_round, sizeof(*providers));
    if (!samples || !sample_lanes || !sample_rounds || !threads || !tasks ||
        !thread_started || !providers) {
        free(samples);
        free(sample_lanes);
        free(sample_rounds);
        free(threads);
        free(tasks);
        free(thread_started);
        free(providers);
        subscription_runtime_inventory_free(lanes, lane_count);
        free(lanes);
        return 1;
    }

    size_t provider_index = 0;
    for (size_t lane_index = 0; lane_index < lane_count; lane_index++) {
        if (!lanes[lane_index].ready)
            continue;
        for (int replica = 0; replica < concurrency; replica++) {
            (void)replica;
            providers[provider_index] = provider_create(lanes[lane_index].spec->provider);
            if (providers[provider_index])
                (void)provider_prepare(providers[provider_index]);
            provider_index++;
        }
    }

    fprintf(stderr,
            "  native subscription benchmark: %d tier-1 lanes, %d round%s, "
            "%d request%s/lane/round\n",
            ready_count, rounds, rounds == 1 ? "" : "s", concurrency,
            concurrency == 1 ? "" : "s");
    double bench_start_ms = subscription_now_ms();
    size_t next_sample = 0;
    for (int round = 0; round < rounds; round++) {
        memset(tasks, 0, samples_per_round * sizeof(*tasks));
        memset(thread_started, 0, samples_per_round * sizeof(*thread_started));
        size_t wave_index = 0;
        for (size_t lane_index = 0; lane_index < lane_count; lane_index++) {
            if (!lanes[lane_index].ready)
                continue;
            for (int replica = 0; replica < concurrency; replica++) {
                size_t sample_index = next_sample++;
                sample_lanes[sample_index] = lane_index;
                sample_rounds[sample_index] = round;
                tasks[wave_index] = (subscription_task_t){
                    .lane = lanes[lane_index].spec,
                    .prompt = prompt,
                    .credential = lanes[lane_index].credential,
                    .max_tokens = max_tokens,
                    .provider = providers[wave_index],
                    .sample = &samples[sample_index],
                };
                if (pthread_create(&threads[wave_index], NULL, subscription_bench_worker,
                                   &tasks[wave_index]) == 0) {
                    thread_started[wave_index] = true;
                } else {
                    subscription_copy(samples[sample_index].stop_reason,
                                      sizeof(samples[sample_index].stop_reason),
                                      "thread_create_failed");
                }
                wave_index++;
            }
        }
        for (size_t i = 0; i < wave_index; i++) {
            if (thread_started[i])
                pthread_join(threads[i], NULL);
        }
    }
    double wall_ms = subscription_now_ms() - bench_start_ms;

    int total_attempts = 0;
    int total_successes = 0;
    int total_output_tokens = 0;
    int total_estimated_output_tokens = 0;
    int total_reasoning_tokens = 0;
    int total_decode_tokens = 0;
    int successful_lanes = 0;
    jbuf_t json;
    jbuf_init(&json, 8192);
    jbuf_append(&json, "{\"schema\":\"dsco.subscription_bench.v1\","
                       "\"tier\":1,\"native_only\":true,\"worker_processes\":false,"
                       "\"rounds\":");
    jbuf_append_int(&json, rounds);
    jbuf_append(&json, ",\"concurrency_per_lane\":");
    jbuf_append_int(&json, concurrency);
    jbuf_append(&json, ",\"max_tokens\":");
    jbuf_append_int(&json, max_tokens);
    jbuf_append(&json, ",\"connection_reuse\":true");
    jbuf_append(&json, ",\"prompt_bytes\":");
    jbuf_append_int(&json, (long long)strlen(prompt));
    jbuf_appendf(&json, ",\"wall_ms\":%.3f,\"lanes\":[", wall_ms);

    for (size_t lane_index = 0; lane_index < lane_count; lane_index++) {
        if (lane_index)
            jbuf_append(&json, ",");
        subscription_append_lane_identity(&json, &lanes[lane_index]);
        int attempts = 0, successes = 0, input_tokens = 0, output_tokens = 0;
        int estimated_output_tokens = 0, reasoning_tokens = 0, decode_tokens = 0;
        double ttft_sum = 0.0, ttft_min = 0.0, ttft_max = 0.0;
        double total_sum = 0.0, total_min = 0.0, total_max = 0.0, tps_sum = 0.0;
        double queue_sum = 0.0, queue_max = 0.0;
        double service_total_sum = 0.0, service_ttft_sum = 0.0;
        double dns_sum = 0.0, connect_sum = 0.0, tls_sum = 0.0, ttfb_sum = 0.0;
        double cold_ttft_sum = 0.0, warm_ttft_sum = 0.0;
        double cold_total_sum = 0.0, warm_total_sum = 0.0;
        size_t request_bytes_sum = 0, request_bytes_min = 0, request_bytes_max = 0;
        int request_samples = 0, transport_samples = 0;
        int cold_ttft_samples = 0, warm_ttft_samples = 0;
        int cold_total_samples = 0, warm_total_samples = 0;
        int new_connections = 0, warm_connection_reuse_samples = 0;
        int ttft_samples = 0, tps_samples = 0, last_http = 0;
        const char *last_stop = lanes[lane_index].ready ? "not_run" : lanes[lane_index].reason;
        for (size_t sample_index = 0; sample_index < sample_count; sample_index++) {
            if (sample_lanes[sample_index] != lane_index || !lanes[lane_index].ready)
                continue;
            subscription_sample_t *sample = &samples[sample_index];
            attempts++;
            total_attempts++;
            last_http = sample->http_status;
            last_stop = sample->stop_reason[0] ? sample->stop_reason : "unknown";
            input_tokens += sample->input_tokens;
            output_tokens += sample->output_tokens;
            total_output_tokens += sample->output_tokens;
            reasoning_tokens += sample->reasoning_tokens;
            total_reasoning_tokens += sample->reasoning_tokens;
            decode_tokens += sample->decode_tokens;
            total_decode_tokens += sample->decode_tokens;
            if (sample->output_tokens_estimated) {
                estimated_output_tokens += sample->output_tokens;
                total_estimated_output_tokens += sample->output_tokens;
            }
            if (sample->ok) {
                successes++;
                total_successes++;
            }
            if (sample->request_bytes > 0) {
                request_bytes_sum += sample->request_bytes;
                if (request_samples == 0 || sample->request_bytes < request_bytes_min)
                    request_bytes_min = sample->request_bytes;
                if (sample->request_bytes > request_bytes_max)
                    request_bytes_max = sample->request_bytes;
                request_samples++;
            }
            if (sample->ttft_ms > 0.0) {
                ttft_sum += sample->ttft_ms;
                if (ttft_samples == 0 || sample->ttft_ms < ttft_min)
                    ttft_min = sample->ttft_ms;
                if (sample->ttft_ms > ttft_max)
                    ttft_max = sample->ttft_ms;
                ttft_samples++;
                service_ttft_sum += sample->ttft_ms > sample->queue_ms
                                        ? sample->ttft_ms - sample->queue_ms
                                        : 0.0;
                if (sample_rounds[sample_index] == 0) {
                    cold_ttft_sum += sample->ttft_ms;
                    cold_ttft_samples++;
                } else {
                    warm_ttft_sum += sample->ttft_ms;
                    warm_ttft_samples++;
                }
            }
            if (sample->total_ms > 0.0) {
                total_sum += sample->total_ms;
                service_total_sum += sample->total_ms > sample->queue_ms
                                         ? sample->total_ms - sample->queue_ms
                                         : 0.0;
                if (attempts == 1 || sample->total_ms < total_min)
                    total_min = sample->total_ms;
                if (sample->total_ms > total_max)
                    total_max = sample->total_ms;
                dns_sum += sample->dns_ms;
                connect_sum += sample->connect_ms;
                tls_sum += sample->tls_ms;
                ttfb_sum += sample->ttfb_ms;
                new_connections += (int)sample->new_connections;
                transport_samples++;
                if (sample_rounds[sample_index] == 0) {
                    cold_total_sum += sample->total_ms;
                    cold_total_samples++;
                } else {
                    warm_total_sum += sample->total_ms;
                    warm_total_samples++;
                    if (sample->new_connections == 0)
                        warm_connection_reuse_samples++;
                }
            }
            queue_sum += sample->queue_ms;
            if (sample->queue_ms > queue_max)
                queue_max = sample->queue_ms;
            if (sample->throughput_reliable && sample->tokens_per_sec > 0.0) {
                tps_sum += sample->tokens_per_sec;
                tps_samples++;
            }
        }
        if (lanes[lane_index].ready && successes > 0)
            successful_lanes++;
        jbuf_append(&json, ",\"attempts\":");
        jbuf_append_int(&json, attempts);
        jbuf_append(&json, ",\"successes\":");
        jbuf_append_int(&json, successes);
        jbuf_append(&json, ",\"last_http_status\":");
        jbuf_append_int(&json, last_http);
        jbuf_append(&json, ",\"last_stop_reason\":");
        jbuf_append_json_str(&json, last_stop);
        jbuf_append(&json, ",\"input_tokens\":");
        jbuf_append_int(&json, input_tokens);
        jbuf_append(&json, ",\"output_tokens\":");
        jbuf_append_int(&json, output_tokens);
        jbuf_append(&json, ",\"estimated_output_tokens\":");
        jbuf_append_int(&json, estimated_output_tokens);
        jbuf_append(&json, ",\"reasoning_tokens\":");
        jbuf_append_int(&json, reasoning_tokens);
        jbuf_append(&json, ",\"decode_tokens\":");
        jbuf_append_int(&json, decode_tokens);
        jbuf_append(&json, ",\"request_bytes_avg\":");
        if (request_samples)
            jbuf_appendf(&json, "%.3f", (double)request_bytes_sum / request_samples);
        else
            jbuf_append(&json, "null");
        jbuf_append(&json, ",\"request_bytes_min\":");
        jbuf_append_int(&json, (long long)request_bytes_min);
        jbuf_append(&json, ",\"request_bytes_max\":");
        jbuf_append_int(&json, (long long)request_bytes_max);
        jbuf_append(&json, ",\"ttft_samples\":");
        jbuf_append_int(&json, ttft_samples);
        jbuf_append(&json, ",\"ttft_ms_avg\":");
        if (ttft_samples)
            jbuf_appendf(&json, "%.3f", ttft_sum / ttft_samples);
        else
            jbuf_append(&json, "null");
        jbuf_append(&json, ",\"ttft_ms_min\":");
        if (ttft_samples)
            jbuf_appendf(&json, "%.3f", ttft_min);
        else
            jbuf_append(&json, "null");
        jbuf_append(&json, ",\"ttft_ms_max\":");
        if (ttft_samples)
            jbuf_appendf(&json, "%.3f", ttft_max);
        else
            jbuf_append(&json, "null");
        jbuf_append(&json, ",\"cold_ttft_ms_avg\":");
        if (cold_ttft_samples)
            jbuf_appendf(&json, "%.3f", cold_ttft_sum / cold_ttft_samples);
        else
            jbuf_append(&json, "null");
        jbuf_append(&json, ",\"warm_ttft_ms_avg\":");
        if (warm_ttft_samples)
            jbuf_appendf(&json, "%.3f", warm_ttft_sum / warm_ttft_samples);
        else
            jbuf_append(&json, "null");
        jbuf_appendf(&json,
                     ",\"queue_ms_avg\":%.3f,\"queue_ms_max\":%.3f,"
                     "\"provider_service_ttft_ms_avg\":",
                     attempts ? queue_sum / attempts : 0.0, queue_max);
        if (ttft_samples)
            jbuf_appendf(&json, "%.3f", service_ttft_sum / ttft_samples);
        else
            jbuf_append(&json, "null");
        jbuf_append(&json, ",\"cold_latency_ms_avg\":");
        if (cold_total_samples)
            jbuf_appendf(&json, "%.3f", cold_total_sum / cold_total_samples);
        else
            jbuf_append(&json, "null");
        jbuf_append(&json, ",\"warm_latency_ms_avg\":");
        if (warm_total_samples)
            jbuf_appendf(&json, "%.3f", warm_total_sum / warm_total_samples);
        else
            jbuf_append(&json, "null");
        jbuf_appendf(&json,
                     ",\"transport_dns_ms_avg\":%.3f,"
                     "\"transport_connect_ms_avg\":%.3f,"
                     "\"transport_tls_ms_avg\":%.3f,"
                     "\"transport_ttfb_ms_avg\":%.3f,"
                     "\"transport_new_connections\":%d,"
                     "\"warm_connection_reuse_samples\":%d,"
                     "\"warm_transport_samples\":%d",
                     transport_samples ? dns_sum / transport_samples : 0.0,
                     transport_samples ? connect_sum / transport_samples : 0.0,
                     transport_samples ? tls_sum / transport_samples : 0.0,
                     transport_samples ? ttfb_sum / transport_samples : 0.0,
                     new_connections, warm_connection_reuse_samples,
                     warm_total_samples);
        jbuf_appendf(&json,
                     ",\"latency_ms_avg\":%.3f,"
                     "\"latency_ms_min\":%.3f,\"latency_ms_max\":%.3f,"
                     "\"provider_service_latency_ms_avg\":%.3f,"
                     "\"decode_throughput_samples\":%d,"
                     "\"decode_throughput_reliable\":%s,"
                     "\"decode_tokens_per_sec_avg\":",
                     attempts ? total_sum / attempts : 0.0, total_min, total_max,
                     attempts ? service_total_sum / attempts : 0.0, tps_samples,
                     successes > 0 && tps_samples == successes ? "true" : "false");
        if (tps_samples)
            jbuf_appendf(&json, "%.3f", tps_sum / tps_samples);
        else
            jbuf_append(&json, "null");
        jbuf_append(&json, "}");
    }
    double aggregate_tps = wall_ms > 0.0 ? total_output_tokens / (wall_ms / 1000.0) : 0.0;
    double aggregate_decode_tps =
        wall_ms > 0.0 ? total_decode_tokens / (wall_ms / 1000.0) : 0.0;
    jbuf_append(&json, "],\"ready_lanes\":");
    jbuf_append_int(&json, ready_count);
    jbuf_append(&json, ",\"successful_lanes\":");
    jbuf_append_int(&json, successful_lanes);
    jbuf_append(&json, ",\"attempts\":");
    jbuf_append_int(&json, total_attempts);
    jbuf_append(&json, ",\"successes\":");
    jbuf_append_int(&json, total_successes);
    jbuf_append(&json, ",\"output_tokens\":");
    jbuf_append_int(&json, total_output_tokens);
    jbuf_append(&json, ",\"estimated_output_tokens\":");
    jbuf_append_int(&json, total_estimated_output_tokens);
    jbuf_append(&json, ",\"reasoning_tokens\":");
    jbuf_append_int(&json, total_reasoning_tokens);
    jbuf_append(&json, ",\"decode_tokens\":");
    jbuf_append_int(&json, total_decode_tokens);
    jbuf_append(&json, ",\"aggregate_throughput_is_estimate\":");
    jbuf_append(&json, total_estimated_output_tokens > 0 ? "true" : "false");
    jbuf_appendf(&json,
                 ",\"aggregate_output_tokens_per_sec\":%.3f,"
                 "\"aggregate_decode_tokens_per_sec\":%.3f}",
                 aggregate_tps, aggregate_decode_tps);
    fprintf(out, "%s\n", json.data ? json.data : "{}");

    jbuf_free(&json);
    for (size_t i = 0; i < provider_index; i++)
        provider_free(providers[i]);
    free(samples);
    free(sample_lanes);
    free(sample_rounds);
    free(threads);
    free(tasks);
    free(thread_started);
    free(providers);
    subscription_runtime_inventory_free(lanes, lane_count);
    free(lanes);
    return successful_lanes == ready_count ? 0 : 1;
}
