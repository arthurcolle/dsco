#ifndef DSCO_PIXEL_TUI_PERF_H
#define DSCO_PIXEL_TUI_PERF_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* Opt-in native compositor telemetry. The hot path only calls into this
 * module when DSCO_PIXEL_TUI_PERF is enabled. Times are local process times:
 * upload_ms covers terminal writes and flushes, not a terminal-side ACK. */
typedef enum {
    PIXEL_TUI_FRAME_IDENTICAL = 0,
    PIXEL_TUI_FRAME_PATCH,
    PIXEL_TUI_FRAME_FULL,
    PIXEL_TUI_FRAME_FAILED,
} pixel_tui_frame_kind_t;

typedef struct {
    pixel_tui_frame_kind_t kind;
    bool has_queue;
    bool has_render;
    bool has_diff;
    bool has_encode;
    bool has_upload;
    uint32_t damage_rects;
    uint64_t chunks;
    uint64_t raw_bytes;
    uint64_t packed_bytes;
    uint64_t encoded_bytes;
    uint64_t wire_bytes;
    size_t retained_bytes;
    size_t transient_bytes;
    double queue_ms;
    double render_ms;
    double diff_ms;
    double encode_ms;
    double upload_ms;
    double flush_ms;
    double frame_ms;
} pixel_tui_frame_sample_t;

typedef struct {
    uint64_t stream_requests;
    uint64_t coalesced_requests;
    uint64_t throttled_repaints;
    uint64_t scheduler_wakeups;
    uint64_t scheduler_renders;
    uint64_t scheduler_deadline_misses;
    uint64_t scheduler_long_gaps;
    uint64_t frames;
    uint64_t identical_frames;
    uint64_t patch_frames;
    uint64_t full_frames;
    uint64_t failed_frames;
    uint64_t damage_rects;
    uint64_t chunks;
    uint64_t raw_bytes;
    uint64_t packed_bytes;
    uint64_t encoded_bytes;
    uint64_t wire_bytes;
    uint64_t producer_wait_samples;
    uint64_t queue_samples;
    uint64_t render_samples;
    uint64_t diff_samples;
    uint64_t encode_samples;
    uint64_t upload_samples;
    size_t peak_retained_bytes;
    size_t peak_transient_bytes;
    double producer_wait_ms_total;
    double producer_wait_ms_max;
    double producer_wait_ms_p50;
    double producer_wait_ms_p95;
    double producer_wait_ms_p99;
    double queue_ms_total;
    double queue_ms_max;
    double queue_ms_p50;
    double queue_ms_p95;
    double queue_ms_p99;
    double render_ms_total;
    double render_ms_max;
    double render_ms_p50;
    double render_ms_p95;
    double render_ms_p99;
    double diff_ms_total;
    double diff_ms_max;
    double diff_ms_p50;
    double diff_ms_p95;
    double diff_ms_p99;
    double encode_ms_total;
    double encode_ms_max;
    double encode_ms_p50;
    double encode_ms_p95;
    double encode_ms_p99;
    double upload_ms_total;
    double upload_ms_max;
    double upload_ms_p50;
    double upload_ms_p95;
    double upload_ms_p99;
    double flush_ms_total;
    double flush_ms_max;
    double flush_ms_p50;
    double flush_ms_p95;
    double flush_ms_p99;
    double frame_ms_total;
    double frame_ms_max;
    double scheduler_lateness_ms_total;
    double scheduler_lateness_ms_max;
    double frame_gap_ms_total;
    double frame_gap_ms_max;
    double frame_ms_p50;
    double frame_ms_p95;
    double frame_ms_p99;
} pixel_tui_perf_snapshot_t;

bool pixel_tui_perf_enabled(void);
/* Explicit capture is used by the deterministic compositor benchmark. It is
 * process-local and does not cause the automatic end-of-session report. */
void pixel_tui_perf_set_capture(bool enabled);
void pixel_tui_perf_reset(void);
void pixel_tui_perf_note_stream_request(bool coalesced);
void pixel_tui_perf_note_throttled(void);
/* Scheduler health is recorded separately from paint cost: lateness is time
 * beyond the requested wake deadline, while frame_gap is time since the last
 * rendered frame. A deadline miss is a wake at least one frame interval late. */
void pixel_tui_perf_note_scheduler_wake(double lateness_ms, double frame_gap_ms,
                                        double frame_interval_ms, bool rendered);
void pixel_tui_perf_note_producer_wait(double wait_ms);
void pixel_tui_perf_record(const pixel_tui_frame_sample_t *sample);
pixel_tui_perf_snapshot_t pixel_tui_perf_snapshot(void);
bool pixel_tui_perf_write_json(FILE *out);

/* DSCO_PIXEL_TUI_PERF=1 writes one JSON line to fallback. An absolute path
 * appends JSONL there instead, which keeps the alternate screen clean. */
bool pixel_tui_perf_report_from_env(FILE *fallback);

#endif /* DSCO_PIXEL_TUI_PERF_H */
