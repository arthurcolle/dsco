#ifndef DSCO_PX_WIDGETS_H
#define DSCO_PX_WIDGETS_H

#include "native_ui.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Component library for agentic terminal surfaces.
 *
 * Constructors append retained native_ui nodes under a parent and return the
 * new node's index (-1 on failure); layout, diffing, focus, and rendering
 * stay with the scene. Colors are semantic tokens, so every component themes
 * through the active px_theme with no per-widget styling.
 *
 * The custom kinds (gauge, bar chart, spinner, activity dots) are rasterized
 * inside px_backend with the generic raster ops — kitty pixel surfaces,
 * native windows, and headless snapshot surfaces share one implementation.
 * A kind tag travels in node->text ("pxw.<kind>[:payload]"); scalar state
 * (value, animation phase) travels in node->value. */

typedef enum {
    PX_WIDGET_TONE_NEUTRAL = 0,
    PX_WIDGET_TONE_ACCENT,
    PX_WIDGET_TONE_SUCCESS,
    PX_WIDGET_TONE_WARNING,
    PX_WIDGET_TONE_DANGER,
} px_widget_tone_t;

native_ui_color_token_t px_widget_tone_token(px_widget_tone_t tone);

/* Custom-kind tags (node->text prefix; px_backend dispatches on "pxw."). */
#define PX_WIDGET_KIND_PREFIX "pxw."
#define PX_WIDGET_KIND_GAUGE "pxw.gauge"
#define PX_WIDGET_KIND_BARS "pxw.bars"
#define PX_WIDGET_KIND_SPINNER "pxw.spinner"
#define PX_WIDGET_KIND_DOTS "pxw.dots"

/* Derived child keys: constructors that add internal nodes (card title,
 * meter label, …) key them as px_widget_subkey(key, 1..n). */
static inline uint64_t px_widget_subkey(uint64_t key, unsigned child) {
    return key ^ ((uint64_t)(child + 1) << 56) ^ UINT64_C(0x5057494447);
}

/* ── Containers ──────────────────────────────────────────────────────── */

/* Titled panel (column flow). Returns the panel index; append content under
 * it. Pass NULL title for an untitled surface. */
int px_widget_card(native_ui_scene_t *scene, int parent, uint64_t key,
                   const char *title);

/* Label/value pair on one row (label muted, value bright). */
int px_widget_kv_row(native_ui_scene_t *scene, int parent, uint64_t key,
                     const char *label, const char *value);

/* ── Status & metrics ────────────────────────────────────────────────── */

/* Compact status pill. */
int px_widget_badge(native_ui_scene_t *scene, int parent, uint64_t key,
                    const char *text, px_widget_tone_t tone);

/* Labeled horizontal meter, value clamped to 0..1. */
int px_widget_meter(native_ui_scene_t *scene, int parent, uint64_t key,
                    const char *label, double value, px_widget_tone_t tone);

/* Line sparkline over up to 48 points (px_backend wire format). */
int px_widget_sparkline(native_ui_scene_t *scene, int parent, uint64_t key,
                        const double *values, int count,
                        px_widget_tone_t tone);

/* Progress bar. fraction in 0..1 is determinate; fraction < 0 renders an
 * indeterminate sweep driven by phase (0..1, caller-advanced per tick). */
int px_widget_progress(native_ui_scene_t *scene, int parent, uint64_t key,
                       double fraction, double phase, px_widget_tone_t tone);

/* 270° radial gauge with a centered caption, value clamped to 0..1. */
int px_widget_gauge(native_ui_scene_t *scene, int parent, uint64_t key,
                    const char *label, double value, px_widget_tone_t tone);

/* Mini vertical bar chart over up to 32 non-negative values. */
int px_widget_bar_chart(native_ui_scene_t *scene, int parent, uint64_t key,
                        const double *values, int count,
                        px_widget_tone_t tone);

/* ── Liveness ────────────────────────────────────────────────────────── */

/* Orbiting-dot spinner; phase 0..1 advances the head. */
int px_widget_spinner(native_ui_scene_t *scene, int parent, uint64_t key,
                      double phase, px_widget_tone_t tone);

/* Three-dot activity pulse (the "agent is thinking" idiom); phase 0..1. */
int px_widget_activity_dots(native_ui_scene_t *scene, int parent,
                            uint64_t key, double phase,
                            px_widget_tone_t tone);

/* ── Composites ──────────────────────────────────────────────────────── */

typedef struct {
    const char *name;  /* agent identity line */
    const char *task;  /* current task summary (may be NULL) */
    const char *model; /* backing model id (may be NULL) */
    native_ui_agent_state_t state;
    double progress; /* 0..1, < 0 hides the meter */
    double cost_usd; /* < 0 hides the cost */
} px_widget_agent_t;

/* Agent status card: identity, lifecycle badge, task, progress, cost. */
int px_widget_agent_card(native_ui_scene_t *scene, int parent, uint64_t key,
                         const px_widget_agent_t *agent);

/* Transient toast row with a tone accent bar. */
int px_widget_toast(native_ui_scene_t *scene, int parent, uint64_t key,
                    const char *text, px_widget_tone_t tone);

#endif /* DSCO_PX_WIDGETS_H */
