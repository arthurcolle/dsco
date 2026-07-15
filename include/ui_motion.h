#ifndef DSCO_UI_MOTION_H
#define DSCO_UI_MOTION_H

#include <stdbool.h>
#include <stdint.h>

/* ── Retained animation timeline ─────────────────────────────────────────
 * Compositor-side property animation: a fixed-capacity set of tracks, each
 * animating one scalar identified by (key, prop). Producers set targets;
 * the render loop samples values with an explicit clock. Everything is
 * deterministic given `now`, so the module is unit-testable and the render
 * cadence can be anything.
 *
 * Retargeting is first-class: setting a new target mid-flight starts from
 * the current animated value and velocity, so interrupted transitions bend
 * instead of jumping. Reduced-motion timelines resolve every set
 * instantly — callers never branch on accessibility themselves. */

typedef enum {
    UI_MOTION_LINEAR = 0,
    UI_MOTION_EASE_OUT,      /* cubic ease-out; UI default */
    UI_MOTION_EASE_IN_OUT,   /* smoothstep */
    UI_MOTION_OVERSHOOT,     /* back ease-out; small spring-like settle */
    UI_MOTION_SPRING,        /* critically damped; duration = response time */
} ui_motion_curve_t;

#define UI_MOTION_MAX_TRACKS 192

typedef struct {
    uint64_t key;
    uint16_t prop;
    bool used;
    ui_motion_curve_t curve;
    double from;
    double target;
    double velocity;      /* value units / second at segment start */
    double start_s;
    double duration_s;
} ui_motion_track_t;

typedef struct {
    ui_motion_track_t tracks[UI_MOTION_MAX_TRACKS];
    int count;
    bool reduced;
} ui_motion_t;

void ui_motion_init(ui_motion_t *m, bool reduced_motion);

/* Animate (key, prop) toward `target` over `duration_s` starting at `now`.
 * A live track retargets from its current value/velocity; a new track starts
 * at `target` unless it was seeded with ui_motion_snap first (no pop-in from
 * a meaningless origin). Reduced motion applies the target immediately. */
void ui_motion_set(ui_motion_t *m, uint64_t key, uint16_t prop, double target,
                   double duration_s, ui_motion_curve_t curve, double now);

/* Force (key, prop) to `value` with no animation (also clears velocity). */
void ui_motion_snap(ui_motion_t *m, uint64_t key, uint16_t prop, double value);

/* Current animated value, or `fallback` when the track does not exist. */
double ui_motion_value(const ui_motion_t *m, uint64_t key, uint16_t prop,
                       double now, double fallback);

/* True while any track still moves at `now` (render loop keep-awake). */
bool ui_motion_active(const ui_motion_t *m, double now);

/* Drop tracks that finished before `now - linger_s`. Resting values are
 * forgotten, so only prune props whose resting value equals the fallback
 * the renderer would use anyway. */
void ui_motion_prune(ui_motion_t *m, double now, double linger_s);

/* Remove one track outright (e.g. its owner disappeared). */
void ui_motion_clear(ui_motion_t *m, uint64_t key, uint16_t prop);

#endif /* DSCO_UI_MOTION_H */
