#ifndef DSCO_PIXEL_HDR_H
#define DSCO_PIXEL_HDR_H

#include <stdbool.h>
#include <stdint.h>

#include "pixel_fx.h"

/* ── High-dynamic-range compositing for the Kitty transport ──────────────
 * The Kitty graphics protocol carries 8-bit RGB24/RGBA32/PNG only: there is
 * no format key for bit depth, primaries, or transfer function.  So "HDR"
 * here means scene-referred rendering, not an HDR wire format.  Compose in
 * unbounded linear float, let highlights exceed 1.0, then resolve once at
 * the wire boundary: bloom -> exposure -> tonemap -> OETF -> dither -> RGB24.
 *
 * The existing pixel_fx surfaces are packed uint8 and clip at 255 on write,
 * so an overbright highlight is destroyed before any blur can bloom from it.
 * This module owns the float buffer; pixel_fx keeps owning the SDR one. */

typedef struct {
    int width, height;
    float *rgb; /* linear, 3 floats/px, row-major, UNBOUNDED above 1.0 */
} pixel_hdr_surface_t;

typedef enum {
    PIXEL_HDR_TONEMAP_CLIP = 0, /* reference/debug: saturate to [0,1] */
    PIXEL_HDR_TONEMAP_REINHARD, /* cheap, desaturating */
    PIXEL_HDR_TONEMAP_ACES,     /* Narkowicz fit; filmic shoulder */
    PIXEL_HDR_TONEMAP_AGX,      /* better hue retention on saturated accents */
} pixel_hdr_tonemap_t;

typedef struct {
    pixel_hdr_tonemap_t curve;
    float exposure;        /* stops; applied as exp2f(exposure) */
    float bloom_threshold; /* linear luminance above which glare is extracted */
    float bloom_strength;  /* 0..1 additive mix of the glare pyramid */
    int bloom_levels;      /* mip depth; 0 disables bloom entirely */
    bool dither;           /* triangular +-0.5 LSB before quantize */
    uint32_t dither_seed;  /* fixed seed keeps renders reproducible */
} pixel_hdr_resolve_opts_t;

void pixel_hdr_resolve_opts_default(pixel_hdr_resolve_opts_t *opts);

bool pixel_hdr_surface_init(pixel_hdr_surface_t *s, int width, int height);
void pixel_hdr_surface_free(pixel_hdr_surface_t *s);
void pixel_hdr_surface_clear(pixel_hdr_surface_t *s);

/* sRGB EOTF decode of a packed 24-bit canvas into linear float. */
void pixel_hdr_from_srgb(pixel_hdr_surface_t *dst, const uint8_t *src24);

/* Additive emissive disc: deposits energy that may exceed 1.0.  Intensity is
 * in linear units, so 8.0 is three stops above SDR white and is what the
 * bloom pyramid actually has headroom to spread. */
void pixel_hdr_add_emissive(pixel_hdr_surface_t *s, int cx, int cy, int radius,
                            pixel_fx_rgb_t color, float intensity);

/* Additive linear rect, unclamped. */
void pixel_hdr_add_rect(pixel_hdr_surface_t *s, int x, int y, int w, int h,
                        pixel_fx_rgb_t color, float intensity);

/* Reconstruct headroom in a scene that was authored in SDR.  Pixels whose
 * linear luminance sits above `knee` are pushed above 1.0 by up to `gain`,
 * with a smooth ramp so nothing pops at the threshold.  Without this an
 * 8-bit source has no energy over white for the bloom pyramid to spread,
 * and the whole HDR path degenerates to an expensive identity transform. */
void pixel_hdr_expand_highlights(pixel_hdr_surface_t *s, float knee, float gain);

/* Rec.709 relative luminance of a linear triple. */
float pixel_hdr_luminance(float r, float g, float b);

/* Scalar tonemap curve; monotonic, maps 0 -> 0 and stays below 1 for all
 * finite inputs (except CLIP, which saturates by definition). */
float pixel_hdr_tonemap_channel(pixel_hdr_tonemap_t curve, float x);

/* The wire boundary.  Writes width*height*3 bytes of RGB24 into dst24. */
bool pixel_hdr_resolve(const pixel_hdr_surface_t *src, uint8_t *dst24,
                       const pixel_hdr_resolve_opts_t *opts);

#endif /* DSCO_PIXEL_HDR_H */
