#define _POSIX_C_SOURCE 200809L

#include "pixel_tui_perf.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static pthread_mutex_t g_perf_mutex = PTHREAD_MUTEX_INITIALIZER;
static pixel_tui_perf_snapshot_t g_perf;

static bool perf_false(const char *value) {
    return !value || !*value || !strcmp(value, "0") ||
           !strcasecmp(value, "false") || !strcasecmp(value, "no") ||
           !strcasecmp(value, "off");
}

bool pixel_tui_perf_enabled(void) {
    return !perf_false(getenv("DSCO_PIXEL_TUI_PERF"));
}

void pixel_tui_perf_reset(void) {
    (void)pthread_mutex_lock(&g_perf_mutex);
    memset(&g_perf, 0, sizeof(g_perf));
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

static void add_time(double value, double *total, double *maximum) {
    if (value < 0.0) value = 0.0;
    *total += value;
    if (value > *maximum) *maximum = value;
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
    if (sample->has_render) {
        g_perf.render_samples++;
        add_time(sample->render_ms, &g_perf.render_ms_total,
                 &g_perf.render_ms_max);
    }
    if (sample->has_diff) {
        g_perf.diff_samples++;
        add_time(sample->diff_ms, &g_perf.diff_ms_total, &g_perf.diff_ms_max);
    }
    if (sample->has_encode) {
        g_perf.encode_samples++;
        add_time(sample->encode_ms, &g_perf.encode_ms_total,
                 &g_perf.encode_ms_max);
    }
    if (sample->has_upload) {
        g_perf.upload_samples++;
        add_time(sample->upload_ms, &g_perf.upload_ms_total,
                 &g_perf.upload_ms_max);
        add_time(sample->flush_ms, &g_perf.flush_ms_total,
                 &g_perf.flush_ms_max);
    }
    add_time(sample->frame_ms, &g_perf.frame_ms_total, &g_perf.frame_ms_max);
    (void)pthread_mutex_unlock(&g_perf_mutex);
}

pixel_tui_perf_snapshot_t pixel_tui_perf_snapshot(void) {
    pixel_tui_perf_snapshot_t snapshot;
    (void)pthread_mutex_lock(&g_perf_mutex);
    snapshot = g_perf;
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
        "\"frames\":{\"total\":%llu,\"identical\":%llu,\"patch\":%llu,"
        "\"full\":%llu,\"failed\":%llu,\"damage_rects\":%llu},"
        "\"time_ms\":{"
        "\"render\":{\"avg\":%.3f,\"max\":%.3f,\"total\":%.3f},"
        "\"diff\":{\"avg\":%.3f,\"max\":%.3f,\"total\":%.3f},"
        "\"encode\":{\"avg\":%.3f,\"max\":%.3f,\"total\":%.3f},"
        "\"upload\":{\"avg\":%.3f,\"max\":%.3f,\"total\":%.3f},"
        "\"flush\":{\"avg\":%.3f,\"max\":%.3f,\"total\":%.3f},"
        "\"frame\":{\"avg\":%.3f,\"max\":%.3f,\"total\":%.3f}},"
        "\"transport\":{\"raw_bytes\":%llu,\"packed_bytes\":%llu,"
        "\"encoded_bytes\":%llu,\"wire_bytes\":%llu,\"chunks\":%llu},"
        "\"memory\":{\"peak_retained_bytes\":%zu,"
        "\"peak_transient_bytes\":%zu}}\n",
        (unsigned long long)p.stream_requests,
        (unsigned long long)p.coalesced_requests,
        (unsigned long long)p.throttled_repaints,
        (unsigned long long)dropped,
        (unsigned long long)p.frames,
        (unsigned long long)p.identical_frames,
        (unsigned long long)p.patch_frames,
        (unsigned long long)p.full_frames,
        (unsigned long long)p.failed_frames,
        (unsigned long long)p.damage_rects,
        average(p.render_ms_total, p.render_samples), p.render_ms_max,
        p.render_ms_total,
        average(p.diff_ms_total, p.diff_samples), p.diff_ms_max, p.diff_ms_total,
        average(p.encode_ms_total, p.encode_samples), p.encode_ms_max,
        p.encode_ms_total,
        average(p.upload_ms_total, p.upload_samples), p.upload_ms_max,
        p.upload_ms_total,
        average(p.flush_ms_total, p.upload_samples), p.flush_ms_max,
        p.flush_ms_total,
        average(p.frame_ms_total, p.frames), p.frame_ms_max, p.frame_ms_total,
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
