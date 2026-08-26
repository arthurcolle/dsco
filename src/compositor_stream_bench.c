#define _POSIX_C_SOURCE 200809L

#include "compositor_stream_bench.h"

#include "config.h"
#include "pixel_tui.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#define STREAM_BENCH_DEFAULT_CHUNKS 900
#define STREAM_BENCH_DEFAULT_INTERVAL_US 2000
#define STREAM_BENCH_MIN_CHUNKS 32
#define STREAM_BENCH_MAX_CHUNKS 10000

static double monotonic_ms(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0.0;
    return (double)now.tv_sec * 1000.0 + (double)now.tv_nsec / 1000000.0;
}

static int env_int(const char *name, int fallback, int minimum, int maximum) {
    const char *value = getenv(name);
    if (!value || !*value) return fallback;
    char *end = NULL;
    long parsed = strtol(value, &end, 10);
    if (end == value || *end != '\0') return fallback;
    if (parsed < minimum) parsed = minimum;
    if (parsed > maximum) parsed = maximum;
    return (int)parsed;
}

static bool env_true(const char *name) {
    const char *value = getenv(name);
    return value && (*value == '1' || !strcasecmp(value, "true") ||
                     !strcasecmp(value, "yes") || !strcasecmp(value, "on"));
}

void compositor_stream_bench_config_default(compositor_stream_bench_config_t *config) {
    if (!config) return;
    *config = (compositor_stream_bench_config_t){
        .chunks = STREAM_BENCH_DEFAULT_CHUNKS,
        .interval_us = env_int("DSCO_COMPOSITOR_BENCH_INTERVAL_US",
                               STREAM_BENCH_DEFAULT_INTERVAL_US, 0, 100000),
        .model = DEFAULT_MODEL,
    };
}

static compositor_stream_bench_config_t normalized_config(
    const compositor_stream_bench_config_t *requested) {
    compositor_stream_bench_config_t config;
    compositor_stream_bench_config_default(&config);
    if (!requested) return config;
    if (requested->chunks > 0) config.chunks = requested->chunks;
    if (requested->interval_us >= 0) config.interval_us = requested->interval_us;
    if (requested->model && *requested->model) config.model = requested->model;
    if (config.chunks < STREAM_BENCH_MIN_CHUNKS)
        config.chunks = STREAM_BENCH_MIN_CHUNKS;
    if (config.chunks > STREAM_BENCH_MAX_CHUNKS)
        config.chunks = STREAM_BENCH_MAX_CHUNKS;
    if (config.interval_us < 0) config.interval_us = 0;
    if (config.interval_us > 100000) config.interval_us = 100000;
    return config;
}

static void sleep_us(int microseconds) {
    if (microseconds <= 0) return;
    struct timespec delay = {
        .tv_sec = microseconds / 1000000,
        .tv_nsec = (long)(microseconds % 1000000) * 1000L,
    };
    while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {}
}

bool compositor_stream_bench_write_json(
    FILE *out, const compositor_stream_bench_config_t *requested,
    size_t source_bytes, double elapsed_ms,
    const pixel_tui_perf_snapshot_t *perf) {
    if (!out || !perf) return false;
    compositor_stream_bench_config_t config = normalized_config(requested);
    double seconds = elapsed_ms > 0.0 ? elapsed_ms / 1000.0 : 0.0;
    double chunks_per_second = seconds > 0.0 ? (double)config.chunks / seconds : 0.0;
    double source_bytes_per_second = seconds > 0.0 ? (double)source_bytes / seconds : 0.0;
    double compositor_fps = seconds > 0.0 ? (double)perf->frames / seconds : 0.0;
    double coalesced_fraction = perf->stream_requests
        ? (double)perf->coalesced_requests / (double)perf->stream_requests : 0.0;
    int rc = fprintf(out,
        "{\"schema\":\"dsco.compositor_stream_bench.v1\","
        "\"workload\":{\"chunks\":%d,\"interval_us\":%d,"
        "\"source_bytes\":%zu,\"elapsed_ms\":%.3f,"
        "\"chunks_per_second\":%.3f,\"source_bytes_per_second\":%.3f},"
        "\"repaints\":{\"requests\":%llu,\"coalesced\":%llu,"
        "\"coalesced_fraction\":%.6f,\"throttled\":%llu},"
        "\"frames\":{\"total\":%llu,\"identical\":%llu,\"patch\":%llu,"
        "\"full\":%llu,\"failed\":%llu,\"fps\":%.3f},"
        "\"latency_ms\":{"
        "\"producer_wait\":{\"p50\":%.3f,\"p95\":%.3f,\"p99\":%.3f,\"max\":%.3f},"
        "\"queue\":{\"p50\":%.3f,\"p95\":%.3f,\"p99\":%.3f,\"max\":%.3f},"
        "\"render\":{\"p50\":%.3f,\"p95\":%.3f,\"p99\":%.3f,\"max\":%.3f},"
        "\"frame\":{\"p50\":%.3f,\"p95\":%.3f,\"p99\":%.3f,\"max\":%.3f}},"
        "\"transport\":{\"raw_bytes\":%llu,\"packed_bytes\":%llu,"
        "\"wire_bytes\":%llu,\"chunks\":%llu},"
        "\"memory\":{\"peak_retained_bytes\":%zu,"
        "\"peak_transient_bytes\":%zu}}\n",
        config.chunks, config.interval_us, source_bytes, elapsed_ms,
        chunks_per_second, source_bytes_per_second,
        (unsigned long long)perf->stream_requests,
        (unsigned long long)perf->coalesced_requests, coalesced_fraction,
        (unsigned long long)perf->throttled_repaints,
        (unsigned long long)perf->frames,
        (unsigned long long)perf->identical_frames,
        (unsigned long long)perf->patch_frames,
        (unsigned long long)perf->full_frames,
        (unsigned long long)perf->failed_frames, compositor_fps,
        perf->producer_wait_ms_p50, perf->producer_wait_ms_p95,
        perf->producer_wait_ms_p99, perf->producer_wait_ms_max,
        perf->queue_ms_p50, perf->queue_ms_p95, perf->queue_ms_p99,
        perf->queue_ms_max,
        perf->render_ms_p50, perf->render_ms_p95, perf->render_ms_p99,
        perf->render_ms_max,
        perf->frame_ms_p50, perf->frame_ms_p95, perf->frame_ms_p99,
        perf->frame_ms_max,
        (unsigned long long)perf->raw_bytes,
        (unsigned long long)perf->packed_bytes,
        (unsigned long long)perf->wire_bytes,
        (unsigned long long)perf->chunks,
        perf->peak_retained_bytes, perf->peak_transient_bytes);
    return rc >= 0 && fflush(out) == 0 && !ferror(out);
}

int compositor_stream_bench_run(FILE *summary,
                                const compositor_stream_bench_config_t *requested) {
    compositor_stream_bench_config_t config = normalized_config(requested);
    if (!summary) summary = stdout;
    if (!isatty(STDERR_FILENO)) {
        fprintf(stderr,
                "error: --compositor-stream-bench requires a Kitty-compatible TTY\n");
        return 1;
    }

    static const char *fragments[] = {
        "Retained state receives another provider delta; ",
        "the compositor schedules one semantic repaint and returns immediately. ",
        "Markdown stays structured, including **strong text**, `inline code`, and links. ",
        "Damage tiles collapse into bounded rectangles before transport.\n\n",
        "```c\nframe.pending = true;  /* provider thread never blocks on raster */\n```\n",
        "Queue latency measures the oldest token waiting for a visible frame. ",
        "P50, P95, and P99 expose stalls hidden by an average. ",
        "The established ANSI compositor remains independently selectable.\n",
    };

    setenv("DSCO_PIXEL_TUI", "1", 1);
    if (!getenv("DSCO_PIXEL_TUI_ANIMATIONS"))
        setenv("DSCO_PIXEL_TUI_ANIMATIONS", "0", 1);
    pixel_tui_perf_set_capture(true);
    if (!pixel_tui_session_begin(stderr, config.model)) {
        pixel_tui_perf_set_capture(false);
        fprintf(stderr,
                "error: native compositor unavailable; run inside Kitty, Ghostty, or WezTerm\n");
        return 1;
    }

    bool input_bench = env_true("DSCO_COMPOSITOR_BENCH_INPUT");
    bool mixed_bench = input_bench && env_true("DSCO_COMPOSITOR_BENCH_MIXED");
    if (!input_bench || mixed_bench) {
        pixel_tui_session_set_state(stderr, PIXEL_TUI_RESPONDING);
        pixel_tui_session_begin_message(stderr, "ASSISTANT", "deterministic stream benchmark");
    }
    pixel_tui_perf_reset();
    size_t source_bytes = 0;
    double started_ms = monotonic_ms();
    if (input_bench) {
        static const char alphabet[] =
            "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-";
        char draft[sizeof(alphabet)] = {0};
        for (int i = 0; i < config.chunks; i++) {
            if (mixed_bench) {
                const char *fragment = fragments[(size_t)i %
                    (sizeof(fragments) / sizeof(fragments[0]))];
                source_bytes += strlen(fragment);
                pixel_tui_session_append_text(stderr, fragment);
            }
            size_t at = (size_t)i % (sizeof(alphabet) - 1U);
            if (at == 0)
                draft[0] = '\0';
            draft[at] = alphabet[at];
            draft[at + 1U] = '\0';
            source_bytes++;
            pixel_tui_session_set_input(stderr, draft, at + 1U, true);
            sleep_us(config.interval_us);
        }
        /* Give the single-owner compositor a bounded settle window so a
         * zero-interval burst proves latest-state convergence, not merely
         * mailbox publication throughput. */
        sleep_us(50000);
        pixel_tui_session_set_input(stderr, "", 0, false);
        sleep_us(20000);
    } else {
        for (int i = 0; i < config.chunks; i++) {
            const char *fragment = fragments[(size_t)i %
                (sizeof(fragments) / sizeof(fragments[0]))];
            source_bytes += strlen(fragment);
            pixel_tui_session_append_text(stderr, fragment);
            sleep_us(config.interval_us);
        }
        pixel_tui_session_end_message(stderr);
    }
    double elapsed_ms = monotonic_ms() - started_ms;
    pixel_tui_session_end(stderr);
    pixel_tui_perf_snapshot_t perf = pixel_tui_perf_snapshot();
    pixel_tui_perf_set_capture(false);
    return compositor_stream_bench_write_json(summary, &config, source_bytes,
                                               elapsed_ms, &perf) ? 0 : 1;
}
