#include "pixel_fx.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ── surface + clip ──────────────────────────────────────────────────── */

void pixel_fx_surface_init(pixel_fx_surface_t *s, uint8_t *rgb,
                           int width, int height) {
    if (!s) return;
    memset(s, 0, sizeof(*s));
    s->rgb = rgb;
    s->width = width > 0 ? width : 0;
    s->height = height > 0 ? height : 0;
}

static void active_clip(const pixel_fx_surface_t *s,
                        int *cx, int *cy, int *cw, int *ch) {
    *cx = 0;
    *cy = 0;
    *cw = s->width;
    *ch = s->height;
    if (s->clip_depth > 0) {
        *cx = s->clips[s->clip_depth - 1].x;
        *cy = s->clips[s->clip_depth - 1].y;
        *cw = s->clips[s->clip_depth - 1].w;
        *ch = s->clips[s->clip_depth - 1].h;
    }
}

bool pixel_fx_clip_push(pixel_fx_surface_t *s, int x, int y, int w, int h) {
    if (!s || s->clip_depth >= PIXEL_FX_CLIP_DEPTH) return false;
    int cx, cy, cw, ch;
    active_clip(s, &cx, &cy, &cw, &ch);
    int left = x > cx ? x : cx;
    int top = y > cy ? y : cy;
    int right = x + w < cx + cw ? x + w : cx + cw;
    int bottom = y + h < cy + ch ? y + h : cy + ch;
    s->clips[s->clip_depth].x = left;
    s->clips[s->clip_depth].y = top;
    s->clips[s->clip_depth].w = right > left ? right - left : 0;
    s->clips[s->clip_depth].h = bottom > top ? bottom - top : 0;
    s->clip_depth++;
    return true;
}

void pixel_fx_clip_pop(pixel_fx_surface_t *s) {
    if (s && s->clip_depth > 0) s->clip_depth--;
}

/* Intersect a draw rect with surface + clip. Returns false when empty. */
static bool clamp_rect(const pixel_fx_surface_t *s, int *x, int *y,
                       int *w, int *h) {
    if (!s || !s->rgb || *w <= 0 || *h <= 0) return false;
    int cx, cy, cw, ch;
    active_clip(s, &cx, &cy, &cw, &ch);
    int left = *x > cx ? *x : cx;
    int top = *y > cy ? *y : cy;
    int right = *x + *w < cx + cw ? *x + *w : cx + cw;
    int bottom = *y + *h < cy + ch ? *y + *h : cy + ch;
    if (right <= left || bottom <= top) return false;
    *x = left;
    *y = top;
    *w = right - left;
    *h = bottom - top;
    return true;
}

static inline void blend_px(uint8_t *p, pixel_fx_rgb_t color, double alpha) {
    if (alpha <= 0.0) return;
    if (alpha >= 1.0) {
        p[0] = color.r;
        p[1] = color.g;
        p[2] = color.b;
        return;
    }
    p[0] = (uint8_t)(p[0] + (color.r - p[0]) * alpha);
    p[1] = (uint8_t)(p[1] + (color.g - p[1]) * alpha);
    p[2] = (uint8_t)(p[2] + (color.b - p[2]) * alpha);
}

static inline uint8_t *px_at(const pixel_fx_surface_t *s, int x, int y) {
    return s->rgb + ((size_t)y * (size_t)s->width + (size_t)x) * 3;
}

static double clamp01(double v) {
    return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
}

/* Signed distance from point to a rounded rect centered on (cx, cy) with
 * half extents (hx, hy) and corner radius r. Negative inside. */
static double rounded_sdf(double px, double py, double cx, double cy,
                          double hx, double hy, double r) {
    double qx = fabs(px - cx) - (hx - r);
    double qy = fabs(py - cy) - (hy - r);
    double ox = qx > 0.0 ? qx : 0.0;
    double oy = qy > 0.0 ? qy : 0.0;
    double outside = sqrt(ox * ox + oy * oy);
    double inside = qx > qy ? qx : qy;
    if (inside > 0.0) inside = 0.0;
    return outside + inside - r;
}

static int clamp_radius(int radius, int w, int h) {
    int cap = (w < h ? w : h) / 2;
    if (radius > cap) radius = cap;
    return radius < 0 ? 0 : radius;
}

/* ── rounded fill / stroke ───────────────────────────────────────────── */

void pixel_fx_fill_rounded(pixel_fx_surface_t *s, int x, int y, int w, int h,
                           int radius, pixel_fx_rgb_t color, double alpha) {
    if (!s || !s->rgb || w <= 0 || h <= 0) return;
    alpha = clamp01(alpha);
    if (alpha <= 0.0) return;
    radius = clamp_radius(radius, w, h);
    double cx = x + w * 0.5, cy = y + h * 0.5;
    double hx = w * 0.5, hy = h * 0.5;
    int dx = x, dy = y, dw = w, dh = h;
    if (!clamp_rect(s, &dx, &dy, &dw, &dh)) return;
    /* Corner pixels need coverage; everything at least `radius + 1` inside
     * both axes is guaranteed interior and takes the flat-fill fast path. */
    int core_left = x + radius + 1, core_right = x + w - radius - 1;
    int core_top = y + radius + 1, core_bottom = y + h - radius - 1;
    for (int yy = dy; yy < dy + dh; yy++) {
        bool row_core = yy >= core_top && yy < core_bottom;
        uint8_t *row = px_at(s, dx, yy);
        for (int xx = dx; xx < dx + dw; xx++, row += 3) {
            double coverage = 1.0;
            if (radius > 0 &&
                !(row_core || (xx >= core_left && xx < core_right))) {
                double d = rounded_sdf(xx + 0.5, yy + 0.5, cx, cy, hx, hy,
                                       (double)radius);
                coverage = clamp01(0.5 - d);
            }
            if (coverage > 0.0) blend_px(row, color, alpha * coverage);
        }
    }
}

void pixel_fx_stroke_rounded(pixel_fx_surface_t *s, int x, int y, int w, int h,
                             int radius, int thickness, pixel_fx_rgb_t color,
                             double alpha) {
    if (!s || !s->rgb || w <= 0 || h <= 0 || thickness < 1) return;
    alpha = clamp01(alpha);
    if (alpha <= 0.0) return;
    radius = clamp_radius(radius, w, h);
    double cx = x + w * 0.5, cy = y + h * 0.5;
    double hx = w * 0.5, hy = h * 0.5;
    int dx = x, dy = y, dw = w, dh = h;
    if (!clamp_rect(s, &dx, &dy, &dw, &dh)) return;
    /* Interior rows only touch the left/right bands; skip the middle. */
    int band = radius > thickness ? radius : thickness;
    for (int yy = dy; yy < dy + dh; yy++) {
        bool edge_row = yy < y + band + 1 || yy >= y + h - band - 1;
        for (int xx = dx; xx < dx + dw; xx++) {
            if (!edge_row && xx >= x + band + 1 && xx < x + w - band - 1)
                continue;
            double d = rounded_sdf(xx + 0.5, yy + 0.5, cx, cy, hx, hy,
                                   (double)radius);
            /* Ring of width `thickness` just inside the silhouette. */
            double outer = clamp01(0.5 - d);
            double inner = clamp01(0.5 - (d + thickness));
            double coverage = outer - inner;
            if (coverage > 0.0)
                blend_px(px_at(s, xx, yy), color, alpha * coverage);
        }
    }
}

/* ── shadow ──────────────────────────────────────────────────────────── */

void pixel_fx_shadow(pixel_fx_surface_t *s, int x, int y, int w, int h,
                     int radius, int blur, int offset_y,
                     pixel_fx_rgb_t color, double alpha) {
    if (!s || !s->rgb || w <= 0 || h <= 0 || blur < 1) return;
    alpha = clamp01(alpha);
    if (alpha <= 0.0) return;
    radius = clamp_radius(radius, w, h);
    int sy = y + offset_y;
    double cx = x + w * 0.5, cy = sy + h * 0.5;
    double hx = w * 0.5, hy = h * 0.5;
    int dx = x - blur, dy = sy - blur, dw = w + blur * 2, dh = h + blur * 2;
    if (!clamp_rect(s, &dx, &dy, &dw, &dh)) return;
    for (int yy = dy; yy < dy + dh; yy++) {
        for (int xx = dx; xx < dx + dw; xx++) {
            double d = rounded_sdf(xx + 0.5, yy + 0.5, cx, cy, hx, hy,
                                   (double)radius);
            if (d >= (double)blur) continue;
            /* smoothstep falloff from the silhouette out to `blur`. */
            double t = clamp01(1.0 - d / (double)blur);
            double coverage = t * t * (3.0 - 2.0 * t);
            if (coverage > 0.0)
                blend_px(px_at(s, xx, yy), color, alpha * coverage);
        }
    }
}

/* ── gradients ───────────────────────────────────────────────────────── */

static pixel_fx_rgb_t gradient_sample(const pixel_fx_stop_t *stops,
                                      int stop_count, double t) {
    if (stop_count < 1) return (pixel_fx_rgb_t){0, 0, 0};
    if (t <= stops[0].t || stop_count == 1) return stops[0].color;
    for (int i = 1; i < stop_count; i++) {
        if (t <= stops[i].t) {
            double span = stops[i].t - stops[i - 1].t;
            double local = span > 0.0 ? (t - stops[i - 1].t) / span : 1.0;
            pixel_fx_rgb_t a = stops[i - 1].color, b = stops[i].color;
            return (pixel_fx_rgb_t){
                (uint8_t)(a.r + (b.r - a.r) * local),
                (uint8_t)(a.g + (b.g - a.g) * local),
                (uint8_t)(a.b + (b.b - a.b) * local),
            };
        }
    }
    return stops[stop_count - 1].color;
}

void pixel_fx_gradient(pixel_fx_surface_t *s, int x, int y, int w, int h,
                       const pixel_fx_stop_t *stops, int stop_count,
                       bool horizontal, double alpha) {
    if (!s || !s->rgb || stop_count < 1) return;
    alpha = clamp01(alpha);
    if (alpha <= 0.0) return;
    int dx = x, dy = y, dw = w, dh = h;
    if (!clamp_rect(s, &dx, &dy, &dw, &dh)) return;
    int span = horizontal ? (w > 1 ? w - 1 : 1) : (h > 1 ? h - 1 : 1);
    if (horizontal) {
        for (int xx = dx; xx < dx + dw; xx++) {
            pixel_fx_rgb_t color =
                gradient_sample(stops, stop_count, (double)(xx - x) / span);
            for (int yy = dy; yy < dy + dh; yy++)
                blend_px(px_at(s, xx, yy), color, alpha);
        }
    } else {
        for (int yy = dy; yy < dy + dh; yy++) {
            pixel_fx_rgb_t color =
                gradient_sample(stops, stop_count, (double)(yy - y) / span);
            uint8_t *row = px_at(s, dx, yy);
            if (alpha >= 1.0) {
                for (int xx = 0; xx < dw; xx++, row += 3) {
                    row[0] = color.r;
                    row[1] = color.g;
                    row[2] = color.b;
                }
            } else {
                for (int xx = 0; xx < dw; xx++, row += 3)
                    blend_px(row, color, alpha);
            }
        }
    }
}

void pixel_fx_gradient_radial(pixel_fx_surface_t *s, int cx, int cy, int radius,
                              const pixel_fx_stop_t *stops, int stop_count,
                              double alpha) {
    if (!s || !s->rgb || stop_count < 1 || radius < 1) return;
    alpha = clamp01(alpha);
    if (alpha <= 0.0) return;
    int dx = cx - radius, dy = cy - radius;
    int dw = radius * 2 + 1, dh = radius * 2 + 1;
    if (!clamp_rect(s, &dx, &dy, &dw, &dh)) return;
    for (int yy = dy; yy < dy + dh; yy++) {
        for (int xx = dx; xx < dx + dw; xx++) {
            double distance = sqrt((double)(xx - cx) * (xx - cx) +
                                   (double)(yy - cy) * (yy - cy));
            if (distance > (double)radius) continue;
            pixel_fx_rgb_t color =
                gradient_sample(stops, stop_count, distance / (double)radius);
            /* Feather the disc rim by one pixel so it composes cleanly. */
            double edge = clamp01((double)radius - distance);
            blend_px(px_at(s, xx, yy), color, alpha * edge);
        }
    }
}

/* ── blur / glass ────────────────────────────────────────────────────── */

/* One box-blur pass along a line of `count` pixels with stride `stride`
 * (in pixels). src/dst are packed RGB. */
static void box_pass(const uint8_t *src, uint8_t *dst, int count, int stride,
                     int radius) {
    int window = radius * 2 + 1;
    int sum[3] = {0, 0, 0};
    for (int i = -radius; i <= radius; i++) {
        int at = i < 0 ? 0 : (i >= count ? count - 1 : i);
        const uint8_t *p = src + (size_t)at * stride * 3;
        sum[0] += p[0];
        sum[1] += p[1];
        sum[2] += p[2];
    }
    for (int i = 0; i < count; i++) {
        uint8_t *out = dst + (size_t)i * stride * 3;
        out[0] = (uint8_t)(sum[0] / window);
        out[1] = (uint8_t)(sum[1] / window);
        out[2] = (uint8_t)(sum[2] / window);
        int leave = i - radius;
        int enter = i + radius + 1;
        if (leave < 0) leave = 0;
        if (enter >= count) enter = count - 1;
        const uint8_t *lp = src + (size_t)leave * stride * 3;
        const uint8_t *ep = src + (size_t)enter * stride * 3;
        sum[0] += ep[0] - lp[0];
        sum[1] += ep[1] - lp[1];
        sum[2] += ep[2] - lp[2];
    }
}

bool pixel_fx_blur(pixel_fx_surface_t *s, int x, int y, int w, int h,
                   int radius) {
    if (!s || !s->rgb || radius < 1) return true;
    int dx = x, dy = y, dw = w, dh = h;
    if (!clamp_rect(s, &dx, &dy, &dw, &dh)) return true;
    size_t bytes = (size_t)dw * (size_t)dh * 3;
    uint8_t *a = malloc(bytes);
    uint8_t *b = malloc(bytes);
    if (!a || !b) {
        free(a);
        free(b);
        return false;
    }
    for (int yy = 0; yy < dh; yy++)
        memcpy(a + (size_t)yy * dw * 3, px_at(s, dx, dy + yy), (size_t)dw * 3);
    /* Three box passes approximate a gaussian; split the radius so the
     * effective kernel stays close to the requested size. */
    int pass_radius = radius / 2 > 0 ? radius / 2 : 1;
    for (int pass = 0; pass < 3; pass++) {
        for (int yy = 0; yy < dh; yy++)
            box_pass(a + (size_t)yy * dw * 3, b + (size_t)yy * dw * 3,
                     dw, 1, pass_radius);
        for (int xx = 0; xx < dw; xx++)
            box_pass(b + (size_t)xx * 3, a + (size_t)xx * 3, dh, dw,
                     pass_radius);
    }
    for (int yy = 0; yy < dh; yy++)
        memcpy(px_at(s, dx, dy + yy), a + (size_t)yy * dw * 3, (size_t)dw * 3);
    free(a);
    free(b);
    return true;
}

void pixel_fx_glass(pixel_fx_surface_t *s, int x, int y, int w, int h,
                    int corner_radius, int blur_radius,
                    pixel_fx_rgb_t tint, double tint_alpha) {
    if (!s || !s->rgb || w <= 0 || h <= 0) return;
    if (blur_radius > 0) (void)pixel_fx_blur(s, x, y, w, h, blur_radius);
    pixel_fx_fill_rounded(s, x, y, w, h, corner_radius, tint,
                          clamp01(tint_alpha));
}
