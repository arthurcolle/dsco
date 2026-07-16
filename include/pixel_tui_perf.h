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
    uint64_t render_samples;
    uint64_t diff_samples;
    uint64_t encode_samples;
    uint64_t upload_samples;
    size_t peak_retained_bytes;
    size_t peak_transient_bytes;
    double render_ms_total;
    double render_ms_max;
    double diff_ms_total;
    double diff_ms_max;
    double encode_ms_total;
    double encode_ms_max;
    double upload_ms_total;
    double upload_ms_max;
    double flush_ms_total;
    double flush_ms_max;
    double frame_ms_total;
    double frame_ms_max;
} pixel_tui_perf_snapshot_t;

bool pixel_tui_perf_enabled(void);
void pixel_tui_perf_reset(void);
void pixel_tui_perf_note_stream_request(bool coalesced);
void pixel_tui_perf_note_throttled(void);
void pixel_tui_perf_record(const pixel_tui_frame_sample_t *sample);
pixel_tui_perf_snapshot_t pixel_tui_perf_snapshot(void);
bool pixel_tui_perf_write_json(FILE *out);

/* DSCO_PIXEL_TUI_PERF=1 writes one JSON line to fallback. An absolute path
 * appends JSONL there instead, which keeps the alternate screen clean. */
bool pixel_tui_perf_report_from_env(FILE *fallback);

#endif /* DSCO_PIXEL_TUI_PERF_H */
