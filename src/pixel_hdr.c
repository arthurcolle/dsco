/*
 * pixel_hdr.c — scene-referred linear compositing for the Kitty transport.
 *
 * Kitty graphics is an 8-bit wire (f=24/f=32/f=100); there is no format key
 * for bit depth or transfer function.  So the dynamic range lives entirely on
 * this side of the boundary: render unbounded, resolve once, ship RGB24.
 */

#include "pixel_hdr.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ── transfer functions ──────────────────────────────────────────────── */

static inline float srgb_to_linear(float c) {
    if (c <= 0.04045f) return c / 12.92f;
    return powf((c + 0.055f) / 1.055f, 2.4f);
}

static inline float linear_to_srgb(float c) {
    if (c <= 0.0f) return 0.0f;
    if (c <= 0.0031308f) return c * 12.92f;
    return 1.055f * powf(c, 1.0f / 2.4f) - 0.055f;
}

float pixel_hdr_luminance(float r, float g, float b) {
    return 0.2126f * r + 0.7152f * g + 0.0722f * b;
}

/* ── tonemap curves ──────────────────────────────────────────────────── */

/* Narkowicz ACES fit: cheap, monotonic, filmic shoulder.
 *
 * The raw rational fit asymptotes at a/c = 2.51/2.43 = 1.0329, so it crosses
 * 1.0 near six stops over white and would clamp flat there, destroying the
 * shoulder it exists to provide.  Normalizing by the asymptote keeps the
 * curve strictly below 1 for every finite input while preserving shape. */
static inline float aces_fit(float x) {
    const float a = 2.51f, b = 0.03f, c = 2.43f, d = 0.59f, e = 0.14f;
    const float asymptote = a / c; /* limit as x -> +inf */
    float n = x * (a * x + b);
    float m = x * (c * x + d) + e;
    if (!(m > 0.0f)) return 0.0f;
    return (n / m) / asymptote;
}

/* AgX-style log-domain sigmoid.  Compresses in log2 space so saturated
 * accents desaturate far less than Reinhard on the way to white. */
static inline float agx_curve(float x) {
    const float min_ev = -12.47393f, max_ev = 4.026069f;
    if (x <= 0.0f) return 0.0f;
    float ev = log2f(x);
    if (ev < min_ev) ev = min_ev;
    if (ev > max_ev) ev = max_ev;
    float t = (ev - min_ev) / (max_ev - min_ev);
    /* Smooth polynomial sigmoid on the normalized log axis. */
    float t2 = t * t;
    return t2 * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

float pixel_hdr_tonemap_channel(pixel_hdr_tonemap_t curve, float x) {
    if (!(x > 0.0f)) return 0.0f; /* also catches NaN */
    switch (curve) {
    case PIXEL_HDR_TONEMAP_CLIP:
        return x > 1.0f ? 1.0f : x;
    case PIXEL_HDR_TONEMAP_REINHARD:
        return x / (1.0f + x);
    case PIXEL_HDR_TONEMAP_AGX:
        return agx_curve(x);
    case PIXEL_HDR_TONEMAP_ACES:
    default:
        return aces_fit(x);
    }
}

/* ── surface lifecycle ───────────────────────────────────────────────── */

void pixel_hdr_resolve_opts_default(pixel_hdr_resolve_opts_t *opts) {
    if (!opts) return;
    opts->curve = PIXEL_HDR_TONEMAP_ACES;
    opts->exposure = 0.0f;
    opts->bloom_threshold = 1.0f;
    opts->bloom_strength = 0.35f;
    opts->bloom_levels = 5;
    opts->dither = true;
    opts->dither_seed = 0x9E3779B9U;
}

bool pixel_hdr_surface_init(pixel_hdr_surface_t *s, int width, int height) {
    if (!s || width <= 0 || height <= 0) return false;
    if (width > 16384 || height > 16384) return false;
    size_t n = (size_t)width * (size_t)height * 3U;
    if (n / 3U / (size_t)width != (size_t)height) return false; /* overflow */
    s->rgb = calloc(n, sizeof(float));
    if (!s->rgb) return false;
    s->width = width;
    s->height = height;
    return true;
}

void pixel_hdr_surface_free(pixel_hdr_surface_t *s) {
    if (!s) return;
    free(s->rgb);
    s->rgb = NULL;
    s->width = s->height = 0;
}

void pixel_hdr_surface_clear(pixel_hdr_surface_t *s) {
    if (!s || !s->rgb) return;
    memset(s->rgb, 0, (size_t)s->width * (size_t)s->height * 3U * sizeof(float));
}

void pixel_hdr_from_srgb(pixel_hdr_surface_t *dst, const uint8_t *src24) {
    if (!dst || !dst->rgb || !src24) return;
    static float lut[256];
    static bool lut_ready = false;
    if (!lut_ready) {
        for (int i = 0; i < 256; i++) lut[i] = srgb_to_linear((float)i / 255.0f);
        lut_ready = true;
    }
    size_t px = (size_t)dst->width * (size_t)dst->height;
    for (size_t i = 0; i < px * 3U; i++) dst->rgb[i] = lut[src24[i]];
}

/* ── emissive deposition ─────────────────────────────────────────────── */

void pixel_hdr_add_rect(pixel_hdr_surface_t *s, int x, int y, int w, int h,
                        pixel_fx_rgb_t color, float intensity) {
    if (!s || !s->rgb || w <= 0 || h <= 0 || !(intensity > 0.0f)) return;
    float lr = srgb_to_linear((float)color.r / 255.0f) * intensity;
    float lg = srgb_to_linear((float)color.g / 255.0f) * intensity;
    float lb = srgb_to_linear((float)color.b / 255.0f) * intensity;
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w > s->width ? s->width : x + w;
    int y1 = y + h > s->height ? s->height : y + h;
    for (int yy = y0; yy < y1; yy++) {
        float *row = s->rgb + ((size_t)yy * (size_t)s->width + (size_t)x0) * 3U;
        for (int xx = x0; xx < x1; xx++) {
            row[0] += lr;
            row[1] += lg;
            row[2] += lb;
            row += 3;
        }
    }
}

void pixel_hdr_add_emissive(pixel_hdr_surface_t *s, int cx, int cy, int radius,
                            pixel_fx_rgb_t color, float intensity) {
    if (!s || !s->rgb || radius < 0 || !(intensity > 0.0f)) return;
    float lr = srgb_to_linear((float)color.r / 255.0f) * intensity;
    float lg = srgb_to_linear((float)color.g / 255.0f) * intensity;
    float lb = srgb_to_linear((float)color.b / 255.0f) * intensity;
    int x0 = cx - radius, x1 = cx + radius;
    int y0 = cy - radius, y1 = cy + radius;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= s->width) x1 = s->width - 1;
    if (y1 >= s->height) y1 = s->height - 1;
    float rr = (float)radius > 0.0f ? (float)radius : 1.0f;
    for (int yy = y0; yy <= y1; yy++) {
        for (int xx = x0; xx <= x1; xx++) {
            float dx = (float)(xx - cx), dy = (float)(yy - cy);
            float d = sqrtf(dx * dx + dy * dy) / rr;
            if (d > 1.0f) continue;
            /* Smooth radial falloff so the core is flat and the rim is soft. */
            float f = 1.0f - d * d;
            f *= f;
            float *p = s->rgb + ((size_t)yy * (size_t)s->width + (size_t)xx) * 3U;
            p[0] += lr * f;
            p[1] += lg * f;
            p[2] += lb * f;
        }
    }
}

void pixel_hdr_expand_highlights(pixel_hdr_surface_t *s, float knee, float gain) {
    if (!s || !s->rgb) return;
    if (!(gain > 1.0f)) return;
    if (knee < 0.0f) knee = 0.0f;
    if (knee >= 1.0f) knee = 0.999f;
    float span = 1.0f - knee;
    size_t px = (size_t)s->width * (size_t)s->height;
    for (size_t i = 0; i < px; i++) {
        float *p = s->rgb + i * 3U;
        float lum = pixel_hdr_luminance(p[0], p[1], p[2]);
        if (lum <= knee) continue;
        float t = (lum - knee) / span;
        if (t > 1.0f) t = 1.0f;
        /* smoothstep keeps the transition invisible at the knee */
        float w = t * t * (3.0f - 2.0f * t);
        float scale = 1.0f + (gain - 1.0f) * w;
        p[0] *= scale;
        p[1] *= scale;
        p[2] *= scale;
    }
}

/* ── bloom pyramid ───────────────────────────────────────────────────── */

typedef struct {
    int w, h;
    float *rgb;
} mip_t;

static void mip_free_all(mip_t *mips, int count) {
    for (int i = 0; i < count; i++) free(mips[i].rgb);
}

/* 2x box downsample. */
static void downsample(const mip_t *src, mip_t *dst) {
    for (int y = 0; y < dst->h; y++) {
        for (int x = 0; x < dst->w; x++) {
            int sx0 = x * 2, sy0 = y * 2;
            int sx1 = sx0 + 1 < src->w ? sx0 + 1 : sx0;
            int sy1 = sy0 + 1 < src->h ? sy0 + 1 : sy0;
            for (int c = 0; c < 3; c++) {
                float a = src->rgb[((size_t)sy0 * src->w + sx0) * 3 + c];
                float b = src->rgb[((size_t)sy0 * src->w + sx1) * 3 + c];
                float e = src->rgb[((size_t)sy1 * src->w + sx0) * 3 + c];
                float f = src->rgb[((size_t)sy1 * src->w + sx1) * 3 + c];
                dst->rgb[((size_t)y * dst->w + x) * 3 + c] = (a + b + e + f) * 0.25f;
            }
        }
    }
}

/* Bilinear upsample-and-accumulate into dst. */
static void upsample_add(const mip_t *src, mip_t *dst, float weight) {
    for (int y = 0; y < dst->h; y++) {
        float sy = ((float)y + 0.5f) * 0.5f - 0.5f;
        int y0 = (int)floorf(sy);
        float fy = sy - (float)y0;
        int y1 = y0 + 1;
        if (y0 < 0) y0 = 0;
        if (y1 < 0) y1 = 0;
        if (y0 >= src->h) y0 = src->h - 1;
        if (y1 >= src->h) y1 = src->h - 1;
        for (int x = 0; x < dst->w; x++) {
            float sx = ((float)x + 0.5f) * 0.5f - 0.5f;
            int x0 = (int)floorf(sx);
            float fx = sx - (float)x0;
            int x1 = x0 + 1;
            if (x0 < 0) x0 = 0;
            if (x1 < 0) x1 = 0;
            if (x0 >= src->w) x0 = src->w - 1;
            if (x1 >= src->w) x1 = src->w - 1;
            for (int c = 0; c < 3; c++) {
                float a = src->rgb[((size_t)y0 * src->w + x0) * 3 + c];
                float b = src->rgb[((size_t)y0 * src->w + x1) * 3 + c];
                float e = src->rgb[((size_t)y1 * src->w + x0) * 3 + c];
                float f = src->rgb[((size_t)y1 * src->w + x1) * 3 + c];
                float top = a + (b - a) * fx;
                float bot = e + (f - e) * fx;
                dst->rgb[((size_t)y * dst->w + x) * 3 + c] +=
                    (top + (bot - top) * fy) * weight;
            }
        }
    }
}

/* Separable 3-tap blur, in place via scratch. */
static void blur_mip(mip_t *m, float *scratch) {
    size_t n = (size_t)m->w * (size_t)m->h * 3U;
    for (int y = 0; y < m->h; y++) {
        for (int x = 0; x < m->w; x++) {
            int xm = x > 0 ? x - 1 : 0;
            int xp = x + 1 < m->w ? x + 1 : m->w - 1;
            for (int c = 0; c < 3; c++) {
                scratch[((size_t)y * m->w + x) * 3 + c] =
                    (m->rgb[((size_t)y * m->w + xm) * 3 + c] +
                     2.0f * m->rgb[((size_t)y * m->w + x) * 3 + c] +
                     m->rgb[((size_t)y * m->w + xp) * 3 + c]) * 0.25f;
            }
        }
    }
    for (int y = 0; y < m->h; y++) {
        int ym = y > 0 ? y - 1 : 0;
        int yp = y + 1 < m->h ? y + 1 : m->h - 1;
        for (int x = 0; x < m->w; x++) {
            for (int c = 0; c < 3; c++) {
                m->rgb[((size_t)y * m->w + x) * 3 + c] =
                    (scratch[((size_t)ym * m->w + x) * 3 + c] +
                     2.0f * scratch[((size_t)y * m->w + x) * 3 + c] +
                     scratch[((size_t)yp * m->w + x) * 3 + c]) * 0.25f;
            }
        }
    }
    (void)n;
}

/* Build the glare pyramid and accumulate it back into `accum` (full res). */
static bool bloom_accumulate(const pixel_hdr_surface_t *src, float *accum,
                             const pixel_hdr_resolve_opts_t *opts) {
    int levels = opts->bloom_levels;
    if (levels < 1 || !(opts->bloom_strength > 0.0f)) return true;
    if (levels > 8) levels = 8;

    mip_t mips[8];
    memset(mips, 0, sizeof(mips));
    int built = 0;

    mips[0].w = src->width;
    mips[0].h = src->height;
    mips[0].rgb = malloc((size_t)mips[0].w * mips[0].h * 3U * sizeof(float));
    if (!mips[0].rgb) return false;
    built = 1;

    /* Bright pass: keep only energy above the threshold, soft knee. */
    size_t px = (size_t)src->width * (size_t)src->height;
    for (size_t i = 0; i < px; i++) {
        float r = src->rgb[i * 3 + 0];
        float g = src->rgb[i * 3 + 1];
        float b = src->rgb[i * 3 + 2];
        float lum = pixel_hdr_luminance(r, g, b);
        float over = lum - opts->bloom_threshold;
        float scale = (over > 0.0f && lum > 1e-6f) ? (over / lum) : 0.0f;
        mips[0].rgb[i * 3 + 0] = r * scale;
        mips[0].rgb[i * 3 + 1] = g * scale;
        mips[0].rgb[i * 3 + 2] = b * scale;
    }

    for (int i = 1; i < levels; i++) {
        int w = mips[i - 1].w / 2, h = mips[i - 1].h / 2;
        if (w < 2 || h < 2) break;
        mips[i].w = w;
        mips[i].h = h;
        mips[i].rgb = malloc((size_t)w * h * 3U * sizeof(float));
        if (!mips[i].rgb) { mip_free_all(mips, built); return false; }
        built++;
        downsample(&mips[i - 1], &mips[i]);
    }

    size_t scratch_px = (size_t)mips[0].w * mips[0].h * 3U;
    float *scratch = malloc(scratch_px * sizeof(float));
    if (!scratch) { mip_free_all(mips, built); return false; }

    for (int i = 1; i < built; i++) blur_mip(&mips[i], scratch);

    /* Walk back up, each level contributing its share of the glare. */
    for (int i = built - 1; i >= 1; i--) upsample_add(&mips[i], &mips[i - 1], 1.0f);

    float norm = opts->bloom_strength / (float)(built > 0 ? built : 1);
    for (size_t i = 0; i < px * 3U; i++) accum[i] += mips[0].rgb[i] * norm;

    free(scratch);
    mip_free_all(mips, built);
    return true;
}

/* ── dither ──────────────────────────────────────────────────────────── */

static inline uint32_t hash_u32(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7FEB352DU;
    x ^= x >> 15;
    x *= 0x846CA68BU;
    x ^= x >> 16;
    return x;
}

/* Triangular PDF noise in [-1, 1] LSB, from two uniform draws. */
static inline float tpdf_noise(uint32_t seed, size_t index) {
    uint32_t a = hash_u32((uint32_t)index ^ seed);
    uint32_t b = hash_u32((uint32_t)index * 2654435761U + seed + 0x517CC1B7U);
    float ua = (float)(a >> 8) / (float)(1U << 24);
    float ub = (float)(b >> 8) / (float)(1U << 24);
    return ua - ub;
}

/* ── resolve ─────────────────────────────────────────────────────────── */

bool pixel_hdr_resolve(const pixel_hdr_surface_t *src, uint8_t *dst24,
                       const pixel_hdr_resolve_opts_t *opts) {
    if (!src || !src->rgb || !dst24) return false;
    pixel_hdr_resolve_opts_t local;
    if (!opts) {
        pixel_hdr_resolve_opts_default(&local);
        opts = &local;
    }
    size_t px = (size_t)src->width * (size_t)src->height;
    size_t n = px * 3U;

    float *work = malloc(n * sizeof(float));
    if (!work) return false;
    memcpy(work, src->rgb, n * sizeof(float));

    if (!bloom_accumulate(src, work, opts)) {
        free(work);
        return false;
    }

    float gain = exp2f(opts->exposure);
    float amp = opts->dither ? (0.5f / 255.0f) : 0.0f;

    for (size_t i = 0; i < n; i++) {
        float v = work[i] * gain;
        v = pixel_hdr_tonemap_channel(opts->curve, v);
        v = linear_to_srgb(v);
        if (amp > 0.0f) v += tpdf_noise(opts->dither_seed, i) * amp;
        if (v <= 0.0f) { dst24[i] = 0; continue; }
        if (v >= 1.0f) { dst24[i] = 255; continue; }
        int q = (int)(v * 255.0f + 0.5f);
        dst24[i] = (uint8_t)(q < 0 ? 0 : (q > 255 ? 255 : q));
    }

    free(work);
    return true;
}
