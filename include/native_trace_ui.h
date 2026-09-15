#ifndef DSCO_NATIVE_TRACE_UI_H
#define DSCO_NATIVE_TRACE_UI_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    NATIVE_TRACE_UI_RECORD, NATIVE_TRACE_UI_RECORDING, NATIVE_TRACE_UI_SAVING,
    NATIVE_TRACE_UI_ASK, NATIVE_TRACE_UI_QUEUED
} native_trace_ui_state_t;
typedef struct {
    uint64_t generation;
    native_trace_ui_state_t state;
    bool enabled;
    char label[48], detail[192];
} native_trace_ui_view_t;

void native_trace_ui_snapshot(native_trace_ui_view_t *view);
/* Call only for terminal input, outside compositor/model locks. The rendered
 * state/generation fences stale clicks. Recording uses the normal tool gate. */
bool native_trace_ui_activate(const native_trace_ui_view_t *view, const char *tier);
bool native_trace_ui_action_pending(void);
bool native_trace_ui_pop_intent(char *out, size_t cap);
void native_trace_ui_reset(void);
#endif
