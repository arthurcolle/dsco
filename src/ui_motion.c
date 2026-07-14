#include "ui_motion.h"

#include <math.h>
#include <string.h>

void ui_motion_init(ui_motion_t *m, bool reduced_motion) {
    if (!m) return;
    memset(m, 0, sizeof(*m));
    m->reduced = reduced_motion;
}

static ui_motion_track_t *find_track(ui_motion_t *m, uint64_t key,
                                     uint16_t prop) {
    for (int i = 0; i < m->count; i++)
        if (m->tracks[i].used && m->tracks[i].key == key &&
            m->tracks[i].prop == prop)
            return &m->tracks[i];
    return NULL;
}

static ui_motion_track_t *alloc_track(ui_motion_t *m) {
    for (int i = 0; i < m->count; i++)
        if (!m->tracks[i].used) return &m->tracks[i];
    if (m->count >= UI_MOTION_MAX_TRACKS) return NULL;
    return &m->tracks[m->count++];
}

static double curve_progress(ui_motion_curve_t curve, double t) {
    if (t <= 0.0) return 0.0;
    if (t >= 1.0) return 1.0;
    switch (curve) {
    case UI_MOTION_LINEAR:
        return t;
    case UI_MOTION_EASE_IN_OUT:
        return t * t * (3.0 - 2.0 * t);
    case UI_MOTION_OVERSHOOT: {
        const double c1 = 1.70158;
        const double c3 = c1 + 1.0;
        double u = t - 1.0;
        return 1.0 + c3 * u * u * u + c1 * u * u;
    }
    case UI_MOTION_EASE_OUT:
    default: {
        double u = 1.0 - t;
        return 1.0 - u * u * u;
    }
    }
}

static double curve_progress_rate(ui_motion_curve_t curve, double t) {
    if (t < 0.0 || t > 1.0) return 0.0;
    switch (curve) {
    case UI_MOTION_LINEAR:
        return 1.0;
    case UI_MOTION_EASE_IN_OUT:
        return 6.0 * t * (1.0 - t);
    case UI_MOTION_OVERSHOOT: {
        const double c1 = 1.70158;
        const double c3 = c1 + 1.0;
        double u = t - 1.0;
        return 3.0 * c3 * u * u + 2.0 * c1 * u;
    }
    case UI_MOTION_EASE_OUT:
    default: {
        double u = 1.0 - t;
        return 3.0 * u * u;
    }
    }
}

/* Critically damped spring toward target; duration acts as response time. */
static double spring_omega(double duration_s) {
    return 6.0 / (duration_s > 1e-6 ? duration_s : 1e-6);
}

static void track_sample(const ui_motion_track_t *track, double now,
                         double *value, double *velocity) {
    double elapsed = now - track->start_s;
    if (elapsed <= 0.0) {
        *value = track->from;
        *velocity = track->velocity;
        return;
    }
    if (track->curve == UI_MOTION_SPRING) {
        double w = spring_omega(track->duration_s);
        double a = track->from - track->target;
        double b = track->velocity + w * a;
        double decay = exp(-w * elapsed);
        *value = track->target + (a + b * elapsed) * decay;
        *velocity = (b - w * (a + b * elapsed)) * decay;
        return;
    }
    double duration = track->duration_s > 1e-6 ? track->duration_s : 1e-6;
    double t = elapsed / duration;
    if (t >= 1.0) {
        *value = track->target;
        *velocity = 0.0;
        return;
    }
    double span = track->target - track->from;
    *value = track->from + span * curve_progress(track->curve, t);
    *velocity = span * curve_progress_rate(track->curve, t) / duration;
}

static bool track_settled(const ui_motion_track_t *track, double now) {
    double value = 0.0, velocity = 0.0;
    track_sample(track, now, &value, &velocity);
    if (track->curve != UI_MOTION_SPRING)
        return now >= track->start_s + track->duration_s;
    double span = fabs(track->target - track->from);
    double eps = 1e-3 * (span > 1.0 ? span : 1.0);
    return fabs(value - track->target) < eps && fabs(velocity) < eps * 10.0;
}

void ui_motion_set(ui_motion_t *m, uint64_t key, uint16_t prop, double target,
                   double duration_s, ui_motion_curve_t curve, double now) {
    if (!m || key == 0) return;
    ui_motion_track_t *track = find_track(m, key, prop);
    double from = target, velocity = 0.0;
    if (track)
        track_sample(track, now, &from, &velocity);
    else if (!(track = alloc_track(m)))
        return;
    if (m->reduced || duration_s <= 0.0) {
        from = target;
        velocity = 0.0;
        duration_s = 0.0;
    }
    *track = (ui_motion_track_t){
        .key = key,
        .prop = prop,
        .used = true,
        .curve = curve,
        .from = from,
        .target = target,
        .velocity = curve == UI_MOTION_SPRING ? velocity : 0.0,
        .start_s = now,
        .duration_s = duration_s,
    };
}

void ui_motion_snap(ui_motion_t *m, uint64_t key, uint16_t prop, double value) {
    if (!m || key == 0) return;
    ui_motion_track_t *track = find_track(m, key, prop);
    if (!track && !(track = alloc_track(m))) return;
    *track = (ui_motion_track_t){
        .key = key,
        .prop = prop,
        .used = true,
        .curve = UI_MOTION_LINEAR,
        .from = value,
        .target = value,
        .start_s = 0.0,
        .duration_s = 0.0,
    };
}

double ui_motion_value(const ui_motion_t *m, uint64_t key, uint16_t prop,
                       double now, double fallback) {
    if (!m) return fallback;
    const ui_motion_track_t *track = find_track((ui_motion_t *)m, key, prop);
    if (!track) return fallback;
    double value = 0.0, velocity = 0.0;
    track_sample(track, now, &value, &velocity);
    return value;
}

bool ui_motion_active(const ui_motion_t *m, double now) {
    if (!m || m->reduced) return false;
    for (int i = 0; i < m->count; i++) {
        const ui_motion_track_t *track = &m->tracks[i];
        if (!track->used || track->duration_s <= 0.0) continue;
        if (!track_settled(track, now)) return true;
    }
    return false;
}

void ui_motion_prune(ui_motion_t *m, double now, double linger_s) {
    if (!m) return;
    for (int i = 0; i < m->count; i++) {
        ui_motion_track_t *track = &m->tracks[i];
        if (!track->used) continue;
        double settle = track->curve == UI_MOTION_SPRING
                            ? track->start_s + track->duration_s * 2.5
                            : track->start_s + track->duration_s;
        if (settle + linger_s < now) track->used = false;
    }
    while (m->count > 0 && !m->tracks[m->count - 1].used) m->count--;
}

void ui_motion_clear(ui_motion_t *m, uint64_t key, uint16_t prop) {
    if (!m) return;
    ui_motion_track_t *track = find_track(m, key, prop);
    if (track) track->used = false;
    while (m->count > 0 && !m->tracks[m->count - 1].used) m->count--;
}
