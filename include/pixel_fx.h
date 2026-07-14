#ifndef DSCO_PIXEL_FX_H
#define DSCO_PIXEL_FX_H

#include <stdbool.h>
#include <stdint.h>

/* ── Native pixel effects ────────────────────────────────────────────────
 * Compositor-grade drawing on the packed 24-bit RGB surfaces the Kitty
 * transport already uses (pixel_tui, plan overlays, generative scenes).
 * Everything here is antialiased where an edge is visible, alpha-blends
 * source-over into the opaque canvas, honors a clip stack, and allocates
 * only for the blur scratch buffer. Coordinates are logical pixels.
 *
 * Distance-field corners keep rounded surfaces resolution-independent:
 * interior spans are flat fills, and only the radius x radius corner
 * blocks pay for per-pixel coverage. */

typedef struct { uint8_t r, g, b; } pixel_fx_rgb_t;

/* One color stop for gradients; t in [0,1] along the gradient axis. */
typedef struct {
    float t;
    pixel_fx_rgb_t color;
} pixel_fx_stop_t;

#define PIXEL_FX_CLIP_DEPTH 8

typedef struct {
    int width, height;
    uint8_t *rgb;                 /* packed 24-bit, row-major, no padding */
    struct { int x, y, w, h; } clips[PIXEL_FX_CLIP_DEPTH];
    int clip_depth;
} pixel_fx_surface_t;

void pixel_fx_surface_init(pixel_fx_surface_t *s, uint8_t *rgb,
                           int width, int height);

/* Clip rects intersect with the current clip; unbalanced pops are ignored. */
bool pixel_fx_clip_push(pixel_fx_surface_t *s, int x, int y, int w, int h);
void pixel_fx_clip_pop(pixel_fx_surface_t *s);

/* Antialiased rounded rectangle fill/stroke. radius is clamped to half the
 * smaller side; radius 0 degenerates to a crisp rect. */
void pixel_fx_fill_rounded(pixel_fx_surface_t *s, int x, int y, int w, int h,
                           int radius, pixel_fx_rgb_t color, double alpha);
void pixel_fx_stroke_rounded(pixel_fx_surface_t *s, int x, int y, int w, int h,
                             int radius, int thickness, pixel_fx_rgb_t color,
                             double alpha);

/* Soft drop shadow cast by a rounded rect. Coverage decays with the signed
 * distance to the rect over `blur` pixels, so no scratch buffer is needed.
 * Painted outside (and under the edge of) the rect only. */
void pixel_fx_shadow(pixel_fx_surface_t *s, int x, int y, int w, int h,
                     int radius, int blur, int offset_y,
                     pixel_fx_rgb_t color, double alpha);

/* Multi-stop linear gradient across the rect. Stops must be ordered by t. */
void pixel_fx_gradient(pixel_fx_surface_t *s, int x, int y, int w, int h,
                       const pixel_fx_stop_t *stops, int stop_count,
                       bool horizontal, double alpha);

/* Radial gradient from (cx, cy); t is distance / radius, clamped. */
void pixel_fx_gradient_radial(pixel_fx_surface_t *s, int cx, int cy, int radius,
                              const pixel_fx_stop_t *stops, int stop_count,
                              double alpha);

/* Approximate gaussian blur of a region (3-pass separable box blur).
 * Radius is in pixels; the region is clamped to surface and clip bounds.
 * Returns false only when scratch allocation fails. */
bool pixel_fx_blur(pixel_fx_surface_t *s, int x, int y, int w, int h, int radius);

/* Frosted-glass panel: blur what's behind the rect, then lay a tinted,
 * rounded translucent surface over it. The standard scrim/overlay ground. */
void pixel_fx_glass(pixel_fx_surface_t *s, int x, int y, int w, int h,
                    int corner_radius, int blur_radius,
                    pixel_fx_rgb_t tint, double tint_alpha);

#endif /* DSCO_PIXEL_FX_H */
