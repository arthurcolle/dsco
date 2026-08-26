#define _POSIX_C_SOURCE 200809L

#include "pixel_tui_perf.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static pthread_mutex_t g_perf_mutex = PTHREAD_MUTEX_INITIALIZER;
static pixel_tui_perf_snapshot_t g_perf;
static atomic_bool g_perf_capture;

/* Quarter-millisecond buckets retain useful compositor tail resolution out to
 * 128 ms without storing an unbounded sample stream. The final bucket also
 * absorbs larger outliers; exact maxima remain available separately. */
#define PERF_HIST_BUCKETS 512
#define PERF_HIST_STEP_MS 0.25
typedef enum {
    PERF_HIST_PRODUCER_WAIT = 0,
    PERF_HIST_QUEUE,
    PERF_HIST_RENDER,
    PERF_HIST_DIFF,
    PERF_HIST_ENCODE,
    PERF_HIST_UPLOAD,
    PERF_HIST_FLUSH,
    PERF_HIST_FRAME,
    PERF_HIST_COUNT,
} perf_hist_kind_t;

typedef struct {
    uint64_t buckets[PERF_HIST_BUCKETS];
    uint64_t count;
} perf_hist_t;

static perf_hist_t g_histograms[PERF_HIST_COUNT];

static bool perf_false(const char *value) {
    return !value || !*value || !strcmp(value, "0") ||
           !strcasecmp(value, "false") || !strcasecmp(value, "no") ||
           !strcasecmp(value, "off");
}

bool pixel_tui_perf_enabled(void) {
    return atomic_load_explicit(&g_perf_capture, memory_order_relaxed) ||
           !perf_false(getenv("DSCO_PIXEL_TUI_PERF"));
}

void pixel_tui_perf_set_capture(bool enabled) {
    atomic_store_explicit(&g_perf_capture, enabled, memory_order_relaxed);
}

void pixel_tui_perf_reset(void) {
    (void)pthread_mutex_lock(&g_perf_mutex);
    memset(&g_perf, 0, sizeof(g_perf));
    memset(g_histograms, 0, sizeof(g_histograms));
    (void)pthread_mutex_unlock(&g_perf_mutex);
}

void pixel_tui_perf_note_stream_request(bool coalesced) {
    if (!pixel_tui_perf_enabled()) return;
    (void)pthread_mutex_lock(&g_perf_mutex);
    g_perf.stream_requests++;
    if (coalesced) g_perf.coalesced_requests++;
    (void)pthread_mutex_unlock(&g_perf_mutex);
}

void pixel_tui_perf_note_throttled(void) {
    if (!pixel_tui_perf_enabled()) return;
    (void)pthread_mutex_lock(&g_perf_mutex);
    g_perf.throttled_repaints++;
    (void)pthread_mutex_unlock(&g_perf_mutex);
}

void pixel_tui_perf_note_scheduler_wake(double lateness_ms, double frame_gap_ms,
                                        double frame_interval_ms, bool rendered) {
    if (!pixel_tui_perf_enabled()) return;
    if (lateness_ms < 0.0) lateness_ms = 0.0;
    if (frame_gap_ms < 0.0) frame_gap_ms = 0.0;
    if (frame_interval_ms < 1.0) frame_interval_ms = 1.0;
    (void)pthread_mutex_lock(&g_perf_mutex);
    g_perf.scheduler_wakeups++;
    if (rendered) g_perf.scheduler_renders++;
    g_perf.scheduler_lateness_ms_total += lateness_ms;
    if (lateness_ms > g_perf.scheduler_lateness_ms_max)
        g_perf.scheduler_lateness_ms_max = lateness_ms;
    g_perf.frame_gap_ms_total += frame_gap_ms;
    if (frame_gap_ms > g_perf.frame_gap_ms_max)
        g_perf.frame_gap_ms_max = frame_gap_ms;
    if (lateness_ms >= frame_interval_ms) g_perf.scheduler_deadline_misses++;
    if (rendered && frame_gap_ms >= frame_interval_ms * 2.0)
        g_perf.scheduler_long_gaps++;
    (void)pthread_mutex_unlock(&g_perf_mutex);
}

static void add_time(double value, double *total, double *maximum) {
    if (value < 0.0) value = 0.0;
    *total += value;
    if (value > *maximum) *maximum = value;
}

static void histogram_add(perf_hist_kind_t kind, double value) {
    if (kind < 0 || kind >= PERF_HIST_COUNT) return;
    if (value < 0.0) value = 0.0;
    size_t bucket = (size_t)(value / PERF_HIST_STEP_MS);
    if (bucket >= PERF_HIST_BUCKETS) bucket = PERF_HIST_BUCKETS - 1;
    g_histograms[kind].buckets[bucket]++;
    g_histograms[kind].count++;
}

void pixel_tui_perf_note_producer_wait(double wait_ms) {
    if (!pixel_tui_perf_enabled()) return;
    (void)pthread_mutex_lock(&g_perf_mutex);
    g_perf.producer_wait_samples++;
    add_time(wait_ms, &g_perf.producer_wait_ms_total,
             &g_perf.producer_wait_ms_max);
    histogram_add(PERF_HIST_PRODUCER_WAIT, wait_ms);
    (void)pthread_mutex_unlock(&g_perf_mutex);
}

static double histogram_quantile(const perf_hist_t *histogram, double quantile) {
    if (!histogram || histogram->count == 0) return 0.0;
    double target = (double)histogram->count * quantile;
    uint64_t rank = (uint64_t)target;
    if ((double)rank < target) rank++;
    if (rank < 1) rank = 1;
    uint64_t seen = 0;
    for (size_t i = 0; i < PERF_HIST_BUCKETS; i++) {
        seen += histogram->buckets[i];
        if (seen >= rank) return (double)(i + 1U) * PERF_HIST_STEP_MS;
    }
    return (double)PERF_HIST_BUCKETS * PERF_HIST_STEP_MS;
}

static void snapshot_quantiles(pixel_tui_perf_snapshot_t *snapshot,
                               perf_hist_kind_t kind,
                               double *p50, double *p95, double *p99) {
    if (!snapshot || !p50 || !p95 || !p99) return;
    const perf_hist_t *histogram = &g_histograms[kind];
    *p50 = histogram_quantile(histogram, 0.50);
    *p95 = histogram_quantile(histogram, 0.95);
    *p99 = histogram_quantile(histogram, 0.99);
}

void pixel_tui_perf_record(const pixel_tui_frame_sample_t *sample) {
    if (!sample || !pixel_tui_perf_enabled()) return;
    (void)pthread_mutex_lock(&g_perf_mutex);
    g_perf.frames++;
    switch (sample->kind) {
    case PIXEL_TUI_FRAME_IDENTICAL: g_perf.identical_frames++; break;
    case PIXEL_TUI_FRAME_PATCH: g_perf.patch_frames++; break;
    case PIXEL_TUI_FRAME_FULL: g_perf.full_frames++; break;
    case PIXEL_TUI_FRAME_FAILED: g_perf.failed_frames++; break;
    }
    g_perf.damage_rects += sample->damage_rects;
    g_perf.chunks += sample->chunks;
    g_perf.raw_bytes += sample->raw_bytes;
    g_perf.packed_bytes += sample->packed_bytes;
    g_perf.encoded_bytes += sample->encoded_bytes;
    g_perf.wire_bytes += sample->wire_bytes;
    if (sample->retained_bytes > g_perf.peak_retained_bytes)
        g_perf.peak_retained_bytes = sample->retained_bytes;
    if (sample->transient_bytes > g_perf.peak_transient_bytes)
        g_perf.peak_transient_bytes = sample->transient_bytes;
    if (sample->has_queue) {
        g_perf.queue_samples++;
        add_time(sample->queue_ms, &g_perf.queue_ms_total,
                 &g_perf.queue_ms_max);
        histogram_add(PERF_HIST_QUEUE, sample->queue_ms);
    }
    if (sample->has_render) {
        g_perf.render_samples++;
        add_time(sample->render_ms, &g_perf.render_ms_total,
                 &g_perf.render_ms_max);
        histogram_add(PERF_HIST_RENDER, sample->render_ms);
    }
    if (sample->has_diff) {
        g_perf.diff_samples++;
        add_time(sample->diff_ms, &g_perf.diff_ms_total, &g_perf.diff_ms_max);
        histogram_add(PERF_HIST_DIFF, sample->diff_ms);
    }
    if (sample->has_encode) {
        g_perf.encode_samples++;
        add_time(sample->encode_ms, &g_perf.encode_ms_total,
                 &g_perf.encode_ms_max);
        histogram_add(PERF_HIST_ENCODE, sample->encode_ms);
    }
    if (sample->has_upload) {
        g_perf.upload_samples++;
        add_time(sample->upload_ms, &g_perf.upload_ms_total,
                 &g_perf.upload_ms_max);
        add_time(sample->flush_ms, &g_perf.flush_ms_total,
                 &g_perf.flush_ms_max);
        histogram_add(PERF_HIST_UPLOAD, sample->upload_ms);
        histogram_add(PERF_HIST_FLUSH, sample->flush_ms);
    }
    add_time(sample->frame_ms, &g_perf.frame_ms_total, &g_perf.frame_ms_max);
    histogram_add(PERF_HIST_FRAME, sample->frame_ms);
    (void)pthread_mutex_unlock(&g_perf_mutex);
}

pixel_tui_perf_snapshot_t pixel_tui_perf_snapshot(void) {
    pixel_tui_perf_snapshot_t snapshot;
    (void)pthread_mutex_lock(&g_perf_mutex);
    snapshot = g_perf;
    snapshot_quantiles(&snapshot, PERF_HIST_PRODUCER_WAIT,
                       &snapshot.producer_wait_ms_p50,
                       &snapshot.producer_wait_ms_p95,
                       &snapshot.producer_wait_ms_p99);
    snapshot_quantiles(&snapshot, PERF_HIST_QUEUE,
                       &snapshot.queue_ms_p50, &snapshot.queue_ms_p95,
                       &snapshot.queue_ms_p99);
    snapshot_quantiles(&snapshot, PERF_HIST_RENDER,
                       &snapshot.render_ms_p50, &snapshot.render_ms_p95,
                       &snapshot.render_ms_p99);
    snapshot_quantiles(&snapshot, PERF_HIST_DIFF,
                       &snapshot.diff_ms_p50, &snapshot.diff_ms_p95,
                       &snapshot.diff_ms_p99);
    snapshot_quantiles(&snapshot, PERF_HIST_ENCODE,
                       &snapshot.encode_ms_p50, &snapshot.encode_ms_p95,
                       &snapshot.encode_ms_p99);
    snapshot_quantiles(&snapshot, PERF_HIST_UPLOAD,
                       &snapshot.upload_ms_p50, &snapshot.upload_ms_p95,
                       &snapshot.upload_ms_p99);
    snapshot_quantiles(&snapshot, PERF_HIST_FLUSH,
                       &snapshot.flush_ms_p50, &snapshot.flush_ms_p95,
                       &snapshot.flush_ms_p99);
    snapshot_quantiles(&snapshot, PERF_HIST_FRAME,
                       &snapshot.frame_ms_p50, &snapshot.frame_ms_p95,
                       &snapshot.frame_ms_p99);
    (void)pthread_mutex_unlock(&g_perf_mutex);
    return snapshot;
}

static double average(double total, uint64_t count) {
    return count ? total / (double)count : 0.0;
}

bool pixel_tui_perf_write_json(FILE *out) {
    if (!out) return false;
    pixel_tui_perf_snapshot_t p = pixel_tui_perf_snapshot();
    uint64_t dropped = p.coalesced_requests + p.throttled_repaints;
    int rc = fprintf(out,
        "{\"schema\":\"dsco.pixel_compositor_perf.v1\","
        "\"repaints\":{\"stream_requests\":%llu,\"coalesced\":%llu,"
        "\"throttled\":%llu,\"dropped_or_deferred\":%llu},"
        "\"scheduler\":{\"wakeups\":%llu,\"renders\":%llu,"
        "\"deadline_misses\":%llu,\"long_frame_gaps\":%llu,"
        "\"lateness_ms\":{\"avg\":%.3f,\"max\":%.3f},"
        "\"frame_gap_ms\":{\"avg\":%.3f,\"max\":%.3f}},"
        "\"frames\":{\"total\":%llu,\"identical\":%llu,\"patch\":%llu,"
        "\"full\":%llu,\"failed\":%llu,\"damage_rects\":%llu},"
        "\"time_ms\":{"
        "\"producer_wait\":{\"avg\":%.3f,\"p50\":%.3f,\"p95\":%.3f,\"p99\":%.3f,\"max\":%.3f,\"total\":%.3f},"
        "\"queue\":{\"avg\":%.3f,\"p50\":%.3f,\"p95\":%.3f,\"p99\":%.3f,\"max\":%.3f,\"total\":%.3f},"
        "\"render\":{\"avg\":%.3f,\"p50\":%.3f,\"p95\":%.3f,\"p99\":%.3f,\"max\":%.3f,\"total\":%.3f},"
        "\"diff\":{\"avg\":%.3f,\"p50\":%.3f,\"p95\":%.3f,\"p99\":%.3f,\"max\":%.3f,\"total\":%.3f},"
        "\"encode\":{\"avg\":%.3f,\"p50\":%.3f,\"p95\":%.3f,\"p99\":%.3f,\"max\":%.3f,\"total\":%.3f},"
        "\"upload\":{\"avg\":%.3f,\"p50\":%.3f,\"p95\":%.3f,\"p99\":%.3f,\"max\":%.3f,\"total\":%.3f},"
        "\"flush\":{\"avg\":%.3f,\"p50\":%.3f,\"p95\":%.3f,\"p99\":%.3f,\"max\":%.3f,\"total\":%.3f},"
        "\"frame\":{\"avg\":%.3f,\"p50\":%.3f,\"p95\":%.3f,\"p99\":%.3f,\"max\":%.3f,\"total\":%.3f}},"
        "\"transport\":{\"raw_bytes\":%llu,\"packed_bytes\":%llu,"
        "\"encoded_bytes\":%llu,\"wire_bytes\":%llu,\"chunks\":%llu},"
        "\"memory\":{\"peak_retained_bytes\":%zu,"
        "\"peak_transient_bytes\":%zu}}\n",
        (unsigned long long)p.stream_requests,
        (unsigned long long)p.coalesced_requests,
        (unsigned long long)p.throttled_repaints,
        (unsigned long long)dropped,
        (unsigned long long)p.scheduler_wakeups,
        (unsigned long long)p.scheduler_renders,
        (unsigned long long)p.scheduler_deadline_misses,
        (unsigned long long)p.scheduler_long_gaps,
        average(p.scheduler_lateness_ms_total, p.scheduler_wakeups),
        p.scheduler_lateness_ms_max,
        average(p.frame_gap_ms_total, p.scheduler_renders),
        p.frame_gap_ms_max,
        (unsigned long long)p.frames,
        (unsigned long long)p.identical_frames,
        (unsigned long long)p.patch_frames,
        (unsigned long long)p.full_frames,
        (unsigned long long)p.failed_frames,
        (unsigned long long)p.damage_rects,
        average(p.producer_wait_ms_total, p.producer_wait_samples),
        p.producer_wait_ms_p50, p.producer_wait_ms_p95,
        p.producer_wait_ms_p99, p.producer_wait_ms_max,
        p.producer_wait_ms_total,
        average(p.queue_ms_total, p.queue_samples), p.queue_ms_p50,
        p.queue_ms_p95, p.queue_ms_p99, p.queue_ms_max, p.queue_ms_total,
        average(p.render_ms_total, p.render_samples), p.render_ms_p50,
        p.render_ms_p95, p.render_ms_p99, p.render_ms_max,
        p.render_ms_total,
        average(p.diff_ms_total, p.diff_samples), p.diff_ms_p50,
        p.diff_ms_p95, p.diff_ms_p99, p.diff_ms_max, p.diff_ms_total,
        average(p.encode_ms_total, p.encode_samples), p.encode_ms_p50,
        p.encode_ms_p95, p.encode_ms_p99, p.encode_ms_max,
        p.encode_ms_total,
        average(p.upload_ms_total, p.upload_samples), p.upload_ms_p50,
        p.upload_ms_p95, p.upload_ms_p99, p.upload_ms_max,
        p.upload_ms_total,
        average(p.flush_ms_total, p.upload_samples), p.flush_ms_p50,
        p.flush_ms_p95, p.flush_ms_p99, p.flush_ms_max,
        p.flush_ms_total,
        average(p.frame_ms_total, p.frames), p.frame_ms_p50,
        p.frame_ms_p95, p.frame_ms_p99, p.frame_ms_max, p.frame_ms_total,
        (unsigned long long)p.raw_bytes,
        (unsigned long long)p.packed_bytes,
        (unsigned long long)p.encoded_bytes,
        (unsigned long long)p.wire_bytes,
        (unsigned long long)p.chunks,
        p.peak_retained_bytes, p.peak_transient_bytes);
    return rc >= 0 && fflush(out) == 0 && !ferror(out);
}

bool pixel_tui_perf_report_from_env(FILE *fallback) {
    const char *value = getenv("DSCO_PIXEL_TUI_PERF");
    if (perf_false(value)) return true;
    FILE *out = fallback;
    bool close_out = false;
    if (value[0] == '/') {
        out = fopen(value, "a");
        close_out = out != NULL;
    }
    if (!out) return false;
    bool ok = pixel_tui_perf_write_json(out);
    if (close_out && fclose(out) != 0) ok = false;
    return ok;
}
