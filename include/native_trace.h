#ifndef DSCO_NATIVE_TRACE_H
#define DSCO_NATIVE_TRACE_H
#include "pixel_tui_perf.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { NT_X, NT_Y, NT_WIDTH, NT_HEIGHT, NT_REVISION, NT_BYTES, NT_CURSOR,
       NT_SCROLL, NT_STATUS, NT_VISIBLE, NT_FOCUSED, NT_DIRTY, NT_ANIMATED,
       NT_PIXELS, NT_COUNT, NT_PENDING, NT_INTERVAL, NT_FIELDS };
typedef struct { char id[64]; int64_t value[NT_FIELDS]; } native_trace_component_t;

typedef struct {
    uint64_t generation, frames, failed_frames;
    bool active, complete, write_failed;
    double duration_ms, elapsed_ms, max_frame_ms;
    char path[4096];
} native_trace_status_t;
/* In-process observation only; no file access or state mutation. */
void native_trace_status(native_trace_status_t *status);

bool native_trace_active(void);
void native_trace_components(const native_trace_component_t *components, size_t count);
void native_trace_frame(const pixel_tui_frame_sample_t *sample);
void native_trace_event(const char *kind, double a, double b, double c, double d);
void native_trace_shutdown(void);
bool tool_ui_trace(const char *input, char *result, size_t cap);
bool native_trace_command(const char *tail, const char *tier, char *result, size_t cap);

#define NATIVE_TRACE_DESCRIPTION \
    "Record a bounded native compositor timeline in the calling interactive process. " \
    "start records component field/pixel checksum deltas, input event kinds, scheduler deadlines " \
    "and frame timings for duration_ms (default 10000, max 600000). " \
    "Returns a private JSONL path, automatically finalized at the deadline. " \
    "status reports progress/path; stop finalizes early. No screenshots or document contents are recorded. " \
    "Pixel changes mean bytes submitted to the terminal, not proof of physical presentation. " \
    "Use the timeline to diagnose frozen animations, missing edits, repaint stalls and layout changes."
#define NATIVE_TRACE_SCHEMA \
    "{\"type\":\"object\",\"additionalProperties\":false,\"properties\":{" \
    "\"action\":{\"type\":\"string\",\"enum\":[\"start\",\"status\",\"stop\"]}," \
    "\"duration_ms\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":600000}," \
    "\"path\":{\"type\":\"string\",\"description\":\"Optional new JSONL file; never overwrites an existing file.\"}}," \
    "\"required\":[\"action\"]}"
#endif
