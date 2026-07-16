#include "px_backend.h"

#include "px_widgets.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static double clamp01(double value) {
    if (value < 0.0) return 0.0;
    if (value > 1.0) return 1.0;
    return value;
}

static uint8_t alpha_scale(uint8_t opacity, double scale) {
    double value = (double)opacity * clamp01(scale);
    if (value < 0.0) return 0;
    if (value > 255.0) return 255;
    return (uint8_t)(value + 0.5);
}

px_backend_color_t px_backend_resolve_color(const px_backend_t *backend,
                                            native_ui_color_token_t token) {
    if (!backend) return (px_backend_color_t){0, 0, 0};
    if (token < 0 || token >= NATIVE_UI_COLOR_COUNT)
        token = NATIVE_UI_COLOR_TEXT;
    return backend->palette.colors[token];
}

void px_backend_init(px_backend_t *backend, void *surface,
                     const px_backend_palette_t *palette,
                     const px_backend_ops_t *ops) {
    if (!backend) return;
    memset(backend, 0, sizeof(*backend));
    backend->surface = surface;
    if (palette) backend->palette = *palette;
    if (ops) backend->ops = *ops;
}

const native_ui_damage_t *px_backend_last_damage(const px_backend_t *backend) {
    return backend ? &backend->last_damage : NULL;
}

static void backend_begin_frame(void *context, native_ui_rect_t viewport,
                                const native_ui_damage_t *damage) {
    px_backend_t *backend = context;
    if (!backend) return;
    if (damage)
        backend->last_damage = *damage;
    else
        memset(&backend->last_damage, 0, sizeof(backend->last_damage));
    if (backend->ops.begin_frame)
        backend->ops.begin_frame(backend->surface, viewport, damage);
}

static void backend_push_clip(void *context, native_ui_rect_t rect) {
    px_backend_t *backend = context;
    if (backend && backend->ops.push_clip)
        backend->ops.push_clip(backend->surface, rect);
}

static void backend_pop_clip(void *context) {
    px_backend_t *backend = context;
    if (backend && backend->ops.pop_clip)
        backend->ops.pop_clip(backend->surface);
}

static void backend_fill_rect(void *context, native_ui_rect_t rect,
                              native_ui_color_token_t token,
                              uint8_t opacity, uint8_t radius) {
    px_backend_t *backend = context;
    if (!backend || !backend->ops.fill_rect) return;
    backend->ops.fill_rect(backend->surface, rect,
                           px_backend_resolve_color(backend, token), opacity,
                           radius, token == NATIVE_UI_COLOR_SURFACE_RAISED);
}

static void backend_stroke_rect(void *context, native_ui_rect_t rect,
                                native_ui_color_token_t token,
                                uint8_t width, uint8_t radius) {
    px_backend_t *backend = context;
    if (!backend || !backend->ops.stroke_rect) return;
    backend->ops.stroke_rect(backend->surface, rect,
                             px_backend_resolve_color(backend, token), 140,
                             width > 0 ? width : 1, radius);
}

static void backend_draw_text(void *context, native_ui_rect_t rect,
                              const char *text, native_ui_type_token_t type,
                              native_ui_color_token_t token, uint8_t opacity) {
    px_backend_t *backend = context;
    if (!backend || !backend->ops.draw_text || !text || !*text) return;
    px_backend_color_t color = px_backend_resolve_color(backend, token);
    if (type == NATIVE_UI_TYPE_CODE)
        color = px_backend_resolve_color(backend, NATIVE_UI_COLOR_WARNING);
    backend->ops.draw_text(backend->surface, rect, text, type, color, opacity);
}

static void backend_draw_icon(void *context, native_ui_rect_t rect,
                              const char *name, native_ui_color_token_t token,
                              uint8_t opacity) {
    px_backend_t *backend = context;
    if (!backend) return;
    px_backend_color_t color = px_backend_resolve_color(backend, token);
    if (backend->ops.draw_icon) {
        backend->ops.draw_icon(backend->surface, rect, name, color, opacity);
        return;
    }
    if (backend->ops.fill_circle) {
        int radius = (rect.width < rect.height ? rect.width : rect.height) / 3;
        if (radius < 2) radius = 2;
        backend->ops.fill_circle(backend->surface,
                                 rect.x + rect.width / 2,
                                 rect.y + rect.height / 2,
                                 radius, color, alpha_scale(opacity, 0.85));
    }
}

static void backend_draw_meter(px_backend_t *backend,
                               const native_ui_node_t *node) {
    if (!backend->ops.fill_rect) return;
    native_ui_rect_t rect = node->frame;
    int track_y = rect.y + rect.height / 2 - 2;
    native_ui_rect_t track = {rect.x, track_y, rect.width, 4};
    backend->ops.fill_rect(
        backend->surface, track,
        px_backend_resolve_color(backend, NATIVE_UI_COLOR_TEXT_MUTED),
        alpha_scale(node->style.opacity, 0.18), 2, false);
    px_backend_color_t fill =
        px_backend_resolve_color(backend, node->style.foreground);
    if (node->value < 0.0f && (node->state & NATIVE_UI_STATE_LIVE)) {
        /* Indeterminate sweep: px_widget_progress packs the phase into
         * -(1 + phase). A 25%-wide window travels the track and wraps. */
        double phase = clamp01(-(double)node->value - 1.0);
        int window = rect.width / 4;
        if (window < 6) window = 6;
        int start = (int)(phase * (double)(rect.width + window)) - window;
        int x0 = start < 0 ? 0 : start;
        int x1 = start + window > rect.width ? rect.width : start + window;
        if (x1 <= x0) return;
        track.x = rect.x + x0;
        track.width = x1 - x0;
        backend->ops.fill_rect(backend->surface, track, fill,
                               alpha_scale(node->style.opacity, 0.70), 2,
                               false);
        return;
    }
    double value = clamp01((double)node->value);
    int fill_width = (int)((double)rect.width * value);
    if (fill_width <= 0) return;
    track.width = fill_width;
    backend->ops.fill_rect(backend->surface, track, fill,
                           alpha_scale(node->style.opacity, 0.75), 2, false);
}

static void backend_draw_sparkline(px_backend_t *backend,
                                   const native_ui_node_t *node) {
    if (!backend->ops.draw_line) return;
    double series[48];
    int count = 0;
    const char *cursor = node->text;
    while (*cursor && count < (int)(sizeof(series) / sizeof(series[0]))) {
        char *end = NULL;
        double value = strtod(cursor, &end);
        if (end == cursor) {
            cursor++;
            continue;
        }
        series[count++] = value;
        cursor = end;
    }
    native_ui_rect_t rect = node->frame;
    if (count < 2 || rect.width < 8 || rect.height < 4) return;
    double low = series[0], high = series[0];
    for (int i = 1; i < count; i++) {
        if (series[i] < low) low = series[i];
        if (series[i] > high) high = series[i];
    }
    double span = high - low > 1e-9 ? high - low : 1.0;
    px_backend_color_t color =
        px_backend_resolve_color(backend, node->style.foreground);
    int last_x = rect.x, last_y = rect.y + rect.height / 2;
    for (int i = 0; i < count; i++) {
        int x = rect.x + (int)((double)i / (double)(count - 1) *
                               (double)(rect.width - 1));
        int y = rect.y + rect.height - 2 -
                (int)((series[i] - low) / span * (double)(rect.height - 4));
        if (i > 0)
            backend->ops.draw_line(backend->surface, last_x, last_y, x, y,
                                   color,
                                   alpha_scale(node->style.opacity, 0.78));
        last_x = x;
        last_y = y;
    }
    if (backend->ops.fill_circle)
        backend->ops.fill_circle(backend->surface, last_x, last_y, 2, color,
                                 node->style.opacity);
}

static void backend_draw_image(px_backend_t *backend,
                               const native_ui_node_t *node) {
    native_ui_rect_t rect = node->frame;
    if (backend->ops.fill_rect)
        backend->ops.fill_rect(
            backend->surface, rect,
            px_backend_resolve_color(backend, NATIVE_UI_COLOR_SURFACE_RAISED),
            alpha_scale(node->style.opacity, 0.80), 6, true);
    if (backend->ops.stroke_rect)
        backend->ops.stroke_rect(
            backend->surface, rect,
            px_backend_resolve_color(backend, NATIVE_UI_COLOR_BORDER),
            alpha_scale(node->style.opacity, 0.40), 1, 6);
    if (backend->ops.draw_text) {
        native_ui_rect_t label = {rect.x + 8, rect.y, rect.width - 16,
                                  rect.height};
        backend->ops.draw_text(
            backend->surface, label,
            node->text[0] ? node->text : "ARTIFACT", NATIVE_UI_TYPE_LABEL,
            px_backend_resolve_color(backend, NATIVE_UI_COLOR_TEXT_MUTED),
            alpha_scale(node->style.opacity, 0.70));
    }
}

static bool backend_draw_widget_gauge(px_backend_t *backend,
                                      const native_ui_node_t *node) {
    if (!backend->ops.draw_line) return true;
    native_ui_rect_t rect = node->frame;
    int cx = rect.x + rect.width / 2;
    int cy = rect.y + rect.height / 2 + 5;
    int radius = (rect.width < rect.height ? rect.width : rect.height) / 2 - 12;
    if (radius < 8) radius = 8;
    const double start = 2.35619449019; /* 135° */
    const double sweep = 4.71238898038; /* 270° */
    const int segments = 36;
    int active = (int)(clamp01(node->value) * segments + 0.5);
    px_backend_color_t track =
        px_backend_resolve_color(backend, NATIVE_UI_COLOR_TEXT_MUTED);
    px_backend_color_t signal =
        px_backend_resolve_color(backend, node->style.foreground);
    for (int i = 0; i < segments; i++) {
        double a0 = start + sweep * (double)i / (double)segments;
        double a1 = start + sweep * (double)(i + 1) / (double)segments;
        px_backend_color_t color = i < active ? signal : track;
        uint8_t opacity = i < active ? alpha_scale(node->style.opacity, 0.88)
                                     : alpha_scale(node->style.opacity, 0.20);
        backend->ops.draw_line(backend->surface,
            cx + (int)(cos(a0) * radius), cy + (int)(sin(a0) * radius),
            cx + (int)(cos(a1) * radius), cy + (int)(sin(a1) * radius),
            color, opacity);
    }
    if (backend->ops.draw_text) {
        const char *label = strchr(node->text, ':');
        if (label && label[1]) {
            native_ui_rect_t text_rect = {rect.x + 8, cy - 11,
                                          rect.width - 16, 22};
            backend->ops.draw_text(backend->surface, text_rect, label + 1,
                NATIVE_UI_TYPE_LABEL,
                px_backend_resolve_color(backend, NATIVE_UI_COLOR_TEXT),
                alpha_scale(node->style.opacity, 0.92));
        }
    }
    return true;
}

static bool backend_draw_widget_bars(px_backend_t *backend,
                                     const native_ui_node_t *node) {
    if (!backend->ops.fill_rect) return true;
    const char *cursor = strchr(node->text, ':');
    if (!cursor) return true;
    cursor++;
    double values[32];
    int count = 0;
    double high = 0.0;
    while (*cursor && count < 32) {
        char *end = NULL;
        double value = strtod(cursor, &end);
        if (end == cursor) { cursor++; continue; }
        if (value < 0.0) value = 0.0;
        values[count++] = value;
        if (value > high) high = value;
        cursor = end;
    }
    if (count < 1) return true;
    if (high <= 0.0) high = 1.0;
    native_ui_rect_t rect = node->frame;
    int gap = count > 16 ? 1 : 3;
    int width = (rect.width - gap * (count - 1)) / count;
    if (width < 1) width = 1;
    px_backend_color_t color =
        px_backend_resolve_color(backend, node->style.foreground);
    for (int i = 0; i < count; i++) {
        int height = (int)(values[i] / high * (double)(rect.height - 4));
        if (values[i] > 0.0 && height < 2) height = 2;
        native_ui_rect_t bar = {rect.x + i * (width + gap),
                                rect.y + rect.height - height,
                                width, height};
        backend->ops.fill_rect(backend->surface, bar, color,
            alpha_scale(node->style.opacity, 0.42 + 0.5 * values[i] / high),
            width > 3 ? 2 : 0, false);
    }
    return true;
}

static bool backend_draw_widget_spinner(px_backend_t *backend,
                                        const native_ui_node_t *node) {
    if (!backend->ops.fill_circle) return true;
    native_ui_rect_t rect = node->frame;
    int cx = rect.x + rect.width / 2, cy = rect.y + rect.height / 2;
    int orbit = (rect.width < rect.height ? rect.width : rect.height) / 2 - 4;
    if (orbit < 5) orbit = 5;
    int head = (int)(clamp01(node->value) * 8.0) % 8;
    px_backend_color_t color =
        px_backend_resolve_color(backend, node->style.foreground);
    for (int i = 0; i < 8; i++) {
        double angle = 6.28318530718 * (double)i / 8.0;
        int distance = (head - i + 8) % 8;
        double alpha = distance == 0 ? 1.0 : 0.16 + 0.56 * (7-distance) / 7.0;
        backend->ops.fill_circle(backend->surface,
            cx + (int)(cos(angle) * orbit), cy + (int)(sin(angle) * orbit),
            distance == 0 ? 3 : 2, color,
            alpha_scale(node->style.opacity, alpha));
    }
    return true;
}

static bool backend_draw_widget_dots(px_backend_t *backend,
                                     const native_ui_node_t *node) {
    if (!backend->ops.fill_circle) return true;
    native_ui_rect_t rect = node->frame;
    int cy = rect.y + rect.height / 2;
    int center = rect.x + rect.width / 2;
    px_backend_color_t color =
        px_backend_resolve_color(backend, node->style.foreground);
    for (int i = 0; i < 3; i++) {
        double wave = 0.5 + 0.5 * sin(6.28318530718 *
                      (clamp01(node->value) - (double)i / 3.0));
        backend->ops.fill_circle(backend->surface, center + (i - 1) * 11,
            cy, wave > 0.72 ? 4 : 3, color,
            alpha_scale(node->style.opacity, 0.28 + 0.72 * wave));
    }
    return true;
}

static bool backend_draw_px_widget(px_backend_t *backend,
                                   const native_ui_node_t *node) {
    if (strncmp(node->text, PX_WIDGET_KIND_PREFIX,
                strlen(PX_WIDGET_KIND_PREFIX)) != 0)
        return false;
    if (!strncmp(node->text, PX_WIDGET_KIND_GAUGE,
                 strlen(PX_WIDGET_KIND_GAUGE)))
        return backend_draw_widget_gauge(backend, node);
    if (!strncmp(node->text, PX_WIDGET_KIND_BARS,
                 strlen(PX_WIDGET_KIND_BARS)))
        return backend_draw_widget_bars(backend, node);
    if (!strcmp(node->text, PX_WIDGET_KIND_SPINNER))
        return backend_draw_widget_spinner(backend, node);
    if (!strcmp(node->text, PX_WIDGET_KIND_DOTS))
        return backend_draw_widget_dots(backend, node);
    return false;
}

static void backend_draw_custom(void *context, const native_ui_node_t *node) {
    px_backend_t *backend = context;
    if (!backend || !node) return;
    if (node->element == NATIVE_UI_ELEMENT_METER) {
        backend_draw_meter(backend, node);
        return;
    }
    if (node->element == NATIVE_UI_ELEMENT_SPARKLINE) {
        backend_draw_sparkline(backend, node);
        return;
    }
    if (node->element == NATIVE_UI_ELEMENT_IMAGE) {
        backend_draw_image(backend, node);
        return;
    }
    if (backend_draw_px_widget(backend, node)) return;
    if (backend->ops.draw_custom)
        backend->ops.draw_custom(backend->surface, node, &backend->palette);
}

static void backend_end_frame(void *context) {
    px_backend_t *backend = context;
    if (backend && backend->ops.end_frame)
        backend->ops.end_frame(backend->surface);
}

const native_ui_backend_t *px_backend_native_vtable(void) {
    static const native_ui_backend_t vtable = {
        .begin_frame = backend_begin_frame,
        .push_clip = backend_push_clip,
        .pop_clip = backend_pop_clip,
        .fill_rect = backend_fill_rect,
        .stroke_rect = backend_stroke_rect,
        .draw_text = backend_draw_text,
        .draw_icon = backend_draw_icon,
        .draw_custom = backend_draw_custom,
        .end_frame = backend_end_frame,
    };
    return &vtable;
}
