/* Animated Distributed Systems wordmark over the Kitty graphics protocol.
 *
 * All pixel dynamics are functions of a phase that wraps at 2π across the
 * frame count, so the loop is seamless and can be handed to the terminal:
 * we transmit every frame up front (a=t + a=f, zlib RGBA payloads), set the
 * per-frame gaps, then start server-side looping with a=a,s=3. Nothing here
 * stays resident — Kitty animates the placement from then on. */

#include "kitty_banner.h"

#include "kitty_banner_mask.h"
#include "kitty_graphics.h"
#include "px_theme.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

/* Artwork units: the legacy 718×108 wordmark footprint every effect constant
 * (margins, grid spacing, trail lengths, sweep width) was tuned against. The
 * coverage mask ships at a higher resolution (BANNER_MASK_W×BANNER_MASK_H)
 * and is resampled from artwork space, so mask fidelity can grow without
 * retuning the scene. */
#define BANNER_ART_W 718
#define BANNER_ART_H 108
#define BANNER_MARGIN_X 20
#define BANNER_MARGIN_Y 12
#define BANNER_W (BANNER_ART_W + 2 * BANNER_MARGIN_X)
#define BANNER_H (BANNER_ART_H + 2 * BANNER_MARGIN_Y)
#define BANNER_FRAMES 24
#define BANNER_FRAME_GAP_MS 70
#define BANNER_MAX_ROWS 10
#define BANNER_PARTICLES 18

typedef struct {
    uint8_t r, g, b, a;
} banner_px_t;

/* ── Wordmark coverage ───────────────────────────────────────────────── */

static uint8_t *banner_mask_decode(void) {
    const size_t total = (size_t)BANNER_MASK_W * BANNER_MASK_H;
    uint8_t *mask = malloc(total);
    if (!mask) return NULL;
    size_t off = 0;
    for (size_t i = 0; i + 1 < sizeof(k_banner_mask_rle); i += 2) {
        size_t run = k_banner_mask_rle[i];
        if (run > total - off) run = total - off;
        memset(mask + off, k_banner_mask_rle[i + 1], run);
        off += run;
        if (off == total) break;
    }
    if (off != total) {
        free(mask);
        return NULL;
    }
    return mask;
}

/* ── Theme tint ──────────────────────────────────────────────────────────
 *
 * Every scene color derives from the active px_theme's brand color, so the
 * wordmark follows /theme (matrix renders phosphor green, amber-crt renders
 * amber, …). The default quiet-console brand reproduces the classic
 * Distributed crimson exactly. */

typedef struct {
    float r, g, b;
} banner_tint_t;

static banner_tint_t banner_brand(void) {
    const px_theme_t *theme = px_theme_active();
    return (banner_tint_t){(float)theme->brand.r, (float)theme->brand.g,
                           (float)theme->brand.b};
}

static banner_tint_t banner_tint_scale(banner_tint_t c, float k) {
    return (banner_tint_t){c.r * k, c.g * k, c.b * k};
}

static banner_tint_t banner_tint_lift(banner_tint_t c, float k) {
    return (banner_tint_t){c.r + (255.0f - c.r) * k,
                           c.g + (255.0f - c.g) * k,
                           c.b + (255.0f - c.b) * k};
}

/* ── Deterministic per-pixel noise ───────────────────────────────────── */

static inline uint32_t banner_hash(uint32_t x, uint32_t y) {
    uint32_t h = x * 0x9E3779B1u ^ y * 0x85EBCA77u;
    h ^= h >> 13;
    h *= 0xC2B2AE3Du;
    h ^= h >> 16;
    return h;
}

static inline float banner_noise(uint32_t h) {
    return (float)(h & 0xFFFFu) / 65536.0f;
}

/* ── Frame synthesis ─────────────────────────────────────────────────── */

static inline void banner_blend_over(banner_px_t *dst, float r, float g, float b,
                                     float a) {
    if (a <= 0.0f) return;
    if (a > 1.0f) a = 1.0f;
    float da = dst->a / 255.0f;
    float oa = a + da * (1.0f - a);
    if (oa <= 0.0f) return;
    dst->r = (uint8_t)((r * a + dst->r * da * (1.0f - a)) / oa);
    dst->g = (uint8_t)((g * a + dst->g * da * (1.0f - a)) / oa);
    dst->b = (uint8_t)((b * a + dst->b * da * (1.0f - a)) / oa);
    dst->a = (uint8_t)(oa * 255.0f + 0.5f);
}

/* ── Scene ───────────────────────────────────────────────────────────── */

/* A render target: canvas dimensions plus the affine artwork→canvas map.
 * scale==1 with zero offsets reproduces the classic 758×132 raster (the
 * glyph-mosaic fallbacks still downsample from it); the graphics-protocol
 * paths size the canvas to the placement's device-pixel footprint so the
 * terminal never resamples — one image pixel is one screen pixel. */
typedef struct {
    int w, h;
    float scale;  /* canvas px per artwork px */
    float ox, oy; /* artwork origin in canvas px */
    const uint8_t *mask;
} banner_scene_t;

static void banner_scene_init(banner_scene_t *sc, const uint8_t *mask, int w,
                              int h) {
    sc->w = w;
    sc->h = h;
    sc->mask = mask;
    float sx = (float)w / (float)BANNER_W;
    float sy = (float)h / (float)BANNER_H;
    sc->scale = sx < sy ? sx : sy;
    sc->ox = ((float)w - sc->scale * (float)BANNER_W) * 0.5f;
    sc->oy = ((float)h - sc->scale * (float)BANNER_H) * 0.5f;
}

/* Bilinear coverage sample at a canvas pixel centre. Canvas → artwork units,
 * then artwork → mask texels, so the high-resolution mask is tapped directly
 * whenever the canvas outresolves the legacy artwork raster. */
static float banner_scene_cov(const banner_scene_t *sc, int x, int y) {
    const float mx_scale = (float)BANNER_MASK_W / (float)BANNER_ART_W;
    const float my_scale = (float)BANNER_MASK_H / (float)BANNER_ART_H;
    float ax = (((float)x + 0.5f - sc->ox) / sc->scale -
                (float)BANNER_MARGIN_X) *
                   mx_scale -
               0.5f;
    float ay = (((float)y + 0.5f - sc->oy) / sc->scale -
                (float)BANNER_MARGIN_Y) *
                   my_scale -
               0.5f;
    if (ax <= -1.0f || ay <= -1.0f || ax >= (float)BANNER_MASK_W ||
        ay >= (float)BANNER_MASK_H)
        return 0.0f;
    int x0 = (int)floorf(ax), y0 = (int)floorf(ay);
    float fx = ax - (float)x0, fy = ay - (float)y0;
    float c = 0.0f;
    for (int j = 0; j < 2; j++)
        for (int i = 0; i < 2; i++) {
            int mx = x0 + i, my = y0 + j;
            if (mx < 0 || my < 0 || mx >= BANNER_MASK_W || my >= BANNER_MASK_H)
                continue;
            float wgt = (i ? fx : 1.0f - fx) * (j ? fy : 1.0f - fy);
            c += wgt * (float)sc->mask[(size_t)my * BANNER_MASK_W + mx];
        }
    return c / 255.0f;
}

/* Packet streams drifting through the margins behind the wordmark. Integer
 * traversal counts per loop keep positions continuous across the wrap. */
typedef struct {
    int count;
    uint32_t salt;
    int laps_min;   /* integer traversals per loop keep the wrap seamless */
    int laps_extra; /* hash-derived 0..laps_extra additional traversals */
    int trail;      /* artwork px; scaled to the canvas */
    float alpha;
    float body[3];
    float head[3];
} banner_particle_cfg_t;

static void banner_scene_particles(banner_px_t *px, const banner_scene_t *sc,
                                   float frac, float phase,
                                   const banner_particle_cfg_t *cfg) {
    int trail = (int)lroundf((float)cfg->trail * sc->scale);
    if (trail < 4) trail = 4;
    int thick = (int)lroundf(sc->scale * 0.6f);
    if (thick < 1) thick = 1;
    int hot_len = (int)lroundf(3.0f * sc->scale);
    if (hot_len < 3) hot_len = 3;
    for (int p = 0; p < cfg->count; p++) {
        uint32_t h = banner_hash(cfg->salt + (uint32_t)p, 0xA6E27u);
        int lane = (int)(h % (uint32_t)sc->h);
        int dir = (p & 1) ? 1 : -1;
        int laps = cfg->laps_min +
                   (cfg->laps_extra ? (int)(h >> 8) % (cfg->laps_extra + 1) : 0);
        float head = fmodf(
            (float)(h % (uint32_t)sc->w) + frac * (float)(laps * sc->w),
            (float)sc->w);
        float pulse = 0.55f + 0.45f * sinf(phase + (float)p * 1.7f);
        for (int k = 0; k < trail; k++) {
            int x = (int)head - dir * k;
            x %= sc->w;
            if (x < 0) x += sc->w;
            float fade = 1.0f - (float)k / (float)trail;
            float a = cfg->alpha * fade * fade * pulse;
            const float *c = k < hot_len ? cfg->head : cfg->body;
            for (int t = 0; t < thick && lane + t < sc->h; t++)
                banner_blend_over(&px[(size_t)(lane + t) * sc->w + x], c[0],
                                  c[1], c[2], a);
        }
    }
}

static void banner_scene_streams(banner_px_t *px, const banner_scene_t *sc,
                                 float frac, float phase) {
    banner_tint_t brand = banner_brand();
    banner_tint_t body = banner_tint_scale(brand, 0.86f);
    banner_tint_t head = banner_tint_lift(brand, 0.58f);
    banner_particle_cfg_t cfg = {
        .count = BANNER_PARTICLES,
        .salt = 0xD15Cu,
        .laps_min = 1,
        .laps_extra = 1,
        .trail = 34,
        .alpha = 0.30f,
        .body = {body.r, body.g, body.b},
        .head = {head.r, head.g, head.b},
    };
    banner_scene_particles(px, sc, frac, phase, &cfg);
}

/* Faint breathing grid keeps the empty margins alive without competing
 * with the wordmark on light or dark terminal themes. */
static void banner_scene_grid(banner_px_t *px, const banner_scene_t *sc,
                              float phase) {
    int spacing = (int)lroundf(26.0f * sc->scale);
    if (spacing < 4) spacing = 4;
    int thick = (int)lroundf(sc->scale * 0.75f);
    if (thick < 1) thick = 1;
    float wave = 0.045f / sc->scale;
    banner_tint_t grid = banner_tint_scale(banner_brand(), 0.82f);
    for (int y = 0; y < sc->h; y++)
        for (int x = 0; x < sc->w; x++) {
            bool line = (x % spacing) < thick || (y % spacing) < thick;
            if (!line) continue;
            float a = 0.5f *
                      (0.045f + 0.030f * sinf(phase + (float)(x + y) * wave));
            banner_blend_over(&px[(size_t)y * sc->w + x], grid.r, grid.g,
                              grid.b, a);
        }
}

/* Sweep centre for a loop fraction: travels the full diagonal span once per
 * loop, exiting right as it re-enters left so the wrap frame is continuous. */
#define BANNER_SWEEP_HW 64.0f /* artwork px */

static inline float banner_scene_sweep_center(const banner_scene_t *sc,
                                              float frac) {
    const float hw = BANNER_SWEEP_HW * sc->scale;
    const float span = (float)sc->w + 0.45f * (float)sc->h + 2.0f * hw;
    return frac * span - hw;
}

/* Wordmark body. Pass a finite sweep_c to bake the specular sweep into the
 * same surface (flattened single-image path); pass BANNER_NO_SWEEP when the
 * sweep lives on its own layer. */
#define BANNER_NO_SWEEP (-1.0e9f)

static void banner_scene_wordmark(banner_px_t *px, const banner_scene_t *sc,
                                  float phase, float sweep_c) {
    const float sweep_hw = BANNER_SWEEP_HW * sc->scale;
    int scan = (int)lroundf(3.0f * sc->scale);
    if (scan < 3) scan = 3;
    int scan_dark = scan / 3;
    /* Canvas bounding box of the wordmark, one pixel of slack for bilinear. */
    int x0 = (int)floorf(sc->ox + ((float)BANNER_MARGIN_X - 1.0f) * sc->scale);
    int x1 = (int)ceilf(sc->ox +
                        ((float)(BANNER_MARGIN_X + BANNER_ART_W) + 1.0f) *
                            sc->scale);
    int y0 = (int)floorf(sc->oy + ((float)BANNER_MARGIN_Y - 1.0f) * sc->scale);
    int y1 = (int)ceilf(sc->oy +
                        ((float)(BANNER_MARGIN_Y + BANNER_ART_H) + 1.0f) *
                            sc->scale);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > sc->w) x1 = sc->w;
    if (y1 > sc->h) y1 = sc->h;
    const float mask_top = sc->oy + (float)BANNER_MARGIN_Y * sc->scale;
    const float mask_hpx = (float)BANNER_ART_H * sc->scale;
    const banner_tint_t base = banner_brand();
    const banner_tint_t spark = banner_tint_lift(base, 0.76f);

    for (int y = y0; y < y1; y++) {
        /* Slight top-lit vertical gradient across the lockup. */
        float rel = ((float)y - mask_top) / mask_hpx;
        if (rel < 0.0f) rel = 0.0f;
        if (rel > 1.0f) rel = 1.0f;
        float shade = 1.0f - 0.14f * rel;
        for (int x = x0; x < x1; x++) {
            float cov = banner_scene_cov(sc, x, y);
            if (cov <= 1.0f / 255.0f) continue;
            uint32_t h = banner_hash((uint32_t)x, (uint32_t)y);
            float n = banner_noise(h);

            /* Per-pixel energize flicker (k=2 harmonics keep the loop exact).
             * Hashed on canvas pixels, so the grain is device-pixel fine. */
            float flicker = 0.84f + 0.16f * sinf(2.0f * phase + n * 6.28318f);

            float r = base.r * flicker * shade;
            float g = base.g * flicker * shade;
            float b = base.b * flicker * shade;

            float d = (float)x + 0.45f * (float)(sc->h - y);
            float t = 1.0f - fabsf(d - sweep_c) / sweep_hw;
            if (t > 0.0f) {
                float boost = t * t * 0.9f;
                r += (255.0f - r) * boost;
                g += (238.0f - g) * boost;
                b += (220.0f - b) * boost;
            }

            /* Sparse pixels arc bright out of phase with their neighbours. */
            if (((h >> 21) & 15u) == 0) {
                float sp = sinf(3.0f * phase + n * 25.0f);
                if (sp > 0.55f) {
                    float k = (sp - 0.55f) / 0.45f * 0.55f;
                    r += (spark.r - r) * k;
                    g += (spark.g - g) * k;
                    b += (spark.b - b) * k;
                }
            }

            if ((y % scan) >= scan - scan_dark) { /* CRT-ish scanline texture */
                r *= 0.90f;
                g *= 0.90f;
                b *= 0.90f;
            }
            banner_blend_over(&px[(size_t)y * sc->w + x], r, g, b, cov);
        }
    }
}

/* Flattened frame: the full stack composited into one surface (single-image
 * transmission paths and the glyph-mosaic fallbacks). */
static void banner_scene_frame(banner_px_t *px, const banner_scene_t *sc,
                               int frame, int frames) {
    const float frac = (float)frame / (float)frames;
    const float phase = frac * 2.0f * (float)M_PI;
    memset(px, 0, (size_t)sc->w * sc->h * sizeof(*px));
    banner_scene_grid(px, sc, phase);
    banner_scene_streams(px, sc, frac, phase);
    banner_scene_wordmark(px, sc, phase, banner_scene_sweep_center(sc, frac));
}

/* Identity-scene wrapper for the fixed 758×132 raster consumers. */
static void banner_render_frame(banner_px_t *px, const uint8_t *mask, int frame,
                                int frames) {
    banner_scene_t sc;
    banner_scene_init(&sc, mask, BANNER_W, BANNER_H);
    banner_scene_frame(px, &sc, frame, frames);
}

/* ── Layer stack ─────────────────────────────────────────────────────────
 *
 * The layered variant decomposes the scene into independent Kitty images
 * stacked with placement z-indexes over the same cell anchor. Each layer is
 * its own server-side animation loop with its own frame count and gap, so
 * the depths drift at different tempos (temporal parallax) while the
 * terminal alpha-composites them — no flattening on our side, and every
 * loop keeps running after this process exits. */

typedef struct {
    const char *name;
    int frames;
    int gap_ms;
    int z;
    void (*render)(banner_px_t *px, const banner_scene_t *sc, float frac,
                   float phase);
} banner_layer_t;

/* Farthest depth: slow, dim, long-trailed embers. */
static void banner_layer_deep(banner_px_t *px, const banner_scene_t *sc,
                              float frac, float phase) {
    banner_tint_t brand = banner_brand();
    banner_tint_t body = banner_tint_scale(brand, 0.52f);
    banner_tint_t head = banner_tint_scale(banner_tint_lift(brand, 0.12f),
                                           0.65f);
    banner_particle_cfg_t cfg = {
        .count = 12,
        .salt = 0xDEE9u,
        .laps_min = 1,
        .laps_extra = 0,
        .trail = 52,
        .alpha = 0.12f,
        .body = {body.r, body.g, body.b},
        .head = {head.r, head.g, head.b},
    };
    banner_scene_particles(px, sc, frac, phase, &cfg);
}

static void banner_layer_grid(banner_px_t *px, const banner_scene_t *sc,
                              float frac, float phase) {
    (void)frac;
    banner_scene_grid(px, sc, phase);
}

/* Near depth: the bright packet streams, faster than everything behind. */
static void banner_layer_streams(banner_px_t *px, const banner_scene_t *sc,
                                 float frac, float phase) {
    banner_scene_streams(px, sc, frac, phase);
}

static void banner_layer_wordmark(banner_px_t *px, const banner_scene_t *sc,
                                  float frac, float phase) {
    (void)frac;
    banner_scene_wordmark(px, sc, phase, BANNER_NO_SWEEP);
}

/* Topmost pass: the specular sweep on its own loop. Full-strength over the
 * wordmark coverage, a faint beam across the margins so it reads as light
 * travelling through the whole stack. */
static void banner_layer_gloss(banner_px_t *px, const banner_scene_t *sc,
                               float frac, float phase) {
    (void)phase;
    const float hw = BANNER_SWEEP_HW * sc->scale;
    const float sweep_c = banner_scene_sweep_center(sc, frac);
    for (int y = 0; y < sc->h; y++) {
        /* Only the band where the sweep has support on this row. */
        float xc = sweep_c - 0.45f * (float)(sc->h - y);
        int bx0 = (int)floorf(xc - hw), bx1 = (int)ceilf(xc + hw) + 1;
        if (bx0 < 0) bx0 = 0;
        if (bx1 > sc->w) bx1 = sc->w;
        for (int x = bx0; x < bx1; x++) {
            float d = (float)x + 0.45f * (float)(sc->h - y);
            float t = 1.0f - fabsf(d - sweep_c) / hw;
            if (t <= 0.0f) continue;
            float cov = banner_scene_cov(sc, x, y);
            float a = t * t * (cov > 0.0f ? 0.80f * cov : 0.045f);
            banner_blend_over(&px[(size_t)y * sc->w + x], 255.0f, 238.0f,
                              220.0f, a);
        }
    }
}

/* Gaps are tuned for the client-driven flip: aggregate ~47 flips/s, and the
 * only byte-heavy plane (the noise-textured wordmark) flips at just ~8/s —
 * the terminal decodes ~15MB/s at a 2260×400 canvas, well inside what kitty
 * and ghostty parse comfortably. Ratios preserve the depth illusion:
 * closer planes flip faster. */
static const banner_layer_t k_banner_layers[] = {
    {"deep", 36, 160, -3, banner_layer_deep},
    {"grid", 24, 150, -2, banner_layer_grid},
    {"streams", 30, 90, -1, banner_layer_streams},
    {"wordmark", 24, 120, 0, banner_layer_wordmark},
    {"gloss", 48, 70, 1, banner_layer_gloss},
};
#define BANNER_LAYER_COUNT \
    (sizeof(k_banner_layers) / sizeof(k_banner_layers[0]))

static inline uint32_t banner_layer_image_id(size_t layer) {
    return (0x4C00u | (uint32_t)layer) << 16 | ((uint32_t)getpid() & 0xFFFFu);
}

/* Which renderer left images resident in the terminal, so exit teardown
 * (kitty_banner_clear) deletes exactly the ids this process transmitted and
 * stays silent on terminals that never received graphics. */
static bool g_banner_root_resident;
static bool g_banner_cells_resident;
static bool g_banner_layers_resident;

/* ── Kitty transport ─────────────────────────────────────────────────── */

static bool banner_send(FILE *out, const char *control, const banner_px_t *px,
                        size_t pixel_count) {
    kitty_graphics_send_options_t options;
    kitty_graphics_send_options_default(&options);
    options.continuation_control = "q=2";
    return kitty_graphics_send_pixels(out, control, px,
                                      pixel_count * sizeof(*px), &options);
}

static bool banner_env_false(const char *name) {
    const char *v = getenv(name);
    return v && (!strcmp(v, "0") || !strcasecmp(v, "false") ||
                 !strcasecmp(v, "no") || !strcasecmp(v, "off"));
}

/* ── Cell renderer ───────────────────────────────────────────────────── */

/* Highest fidelity first: on a graphics-protocol terminal the "cell" render
 * is pixel-native — every frame is transmitted once and the loop is driven
 * client-side by retargeting the placement's current frame, so a finite
 * number of loops plays at full canvas resolution. Elsewhere each frame is
 * box-downsampled to a sub-pixel grid carried by Unicode mosaics: sextant
 * blocks (2×3 sub-pixels per cell, fully covered cells split into a
 * bright/dark pair so fg+bg carry two colours) or U+2580 half blocks (1×2)
 * as the legacy mode. Sub-pixels whose alpha stays under the threshold keep
 * the terminal's own background so the margins remain transparent.
 * DSCO_BANNER_CELLS=pixel|sextant|half pins a mode; default is best-fit. */

#define BANNER_CELL_GAP_MS 45
#define BANNER_CELL_ALPHA_MIN 10

typedef enum {
    BANNER_CELLS_AUTO,
    BANNER_CELLS_PIXEL,
    BANNER_CELLS_SEXTANT,
    BANNER_CELLS_HALF,
} banner_cells_mode_t;

static banner_cells_mode_t banner_cells_mode(void) {
    const char *v = getenv("DSCO_BANNER_CELLS");
    if (!v || !*v) return BANNER_CELLS_AUTO;
    if (!strcasecmp(v, "pixel") || !strcasecmp(v, "kitty"))
        return BANNER_CELLS_PIXEL;
    if (!strcasecmp(v, "sextant") || !strcasecmp(v, "hires"))
        return BANNER_CELLS_SEXTANT;
    if (!strcasecmp(v, "half") || !strcasecmp(v, "legacy") ||
        !strcasecmp(v, "blocks"))
        return BANNER_CELLS_HALF;
    return BANNER_CELLS_AUTO;
}

static bool banner_placement_geometry(FILE *out, int *place_cols,
                                      int *place_rows, int *canvas_w,
                                      int *canvas_h);

typedef struct {
    uint8_t r, g, b;
    bool on;
} banner_cell_px_t;

static bool banner_truecolor(void) {
    const char *ct = getenv("COLORTERM");
    if (ct && (strstr(ct, "truecolor") || strstr(ct, "24bit"))) return true;
    const char *term = getenv("TERM");
    return term && (strstr(term, "kitty") || strstr(term, "ghostty") ||
                    strstr(term, "direct") || strstr(term, "iterm"));
}

static int banner_rgb_to_256(int r, int g, int b) {
    int lo = r < g ? (r < b ? r : b) : (g < b ? g : b);
    int hi = r > g ? (r > b ? r : b) : (g > b ? g : b);
    if (hi - lo < 12) { /* near-grey → 24-step ramp */
        int v = (r + g + b) / 3;
        if (v < 8) return 16;
        if (v > 238) return 231;
        return 232 + (v - 8) / 10;
    }
    return 16 + 36 * (r * 6 / 256) + 6 * (g * 6 / 256) + (b * 6 / 256);
}

static void banner_cell_fg(FILE *out, bool tc, banner_cell_px_t c) {
    if (tc)
        fprintf(out, "\033[38;2;%d;%d;%dm", c.r, c.g, c.b);
    else
        fprintf(out, "\033[38;5;%dm", banner_rgb_to_256(c.r, c.g, c.b));
}

static void banner_cell_bg(FILE *out, bool tc, banner_cell_px_t c) {
    if (tc)
        fprintf(out, "\033[48;2;%d;%d;%dm", c.r, c.g, c.b);
    else
        fprintf(out, "\033[48;5;%dm", banner_rgb_to_256(c.r, c.g, c.b));
}

/* Box-downsample one synthesised frame into the sub_w×sub_h sub-pixel grid. */
static void banner_downsample(const banner_px_t *px, banner_cell_px_t *grid,
                              int sub_w, int sub_h) {
    for (int sy = 0; sy < sub_h; sy++) {
        int y0 = sy * BANNER_H / sub_h;
        int y1 = (sy + 1) * BANNER_H / sub_h;
        if (y1 <= y0) y1 = y0 + 1;
        for (int sx = 0; sx < sub_w; sx++) {
            int x0 = sx * BANNER_W / sub_w;
            int x1 = (sx + 1) * BANNER_W / sub_w;
            if (x1 <= x0) x1 = x0 + 1;
            /* Premultiplied accumulation so transparent pixels don't dilute
             * the colour of covered ones. */
            uint32_t ar = 0, ag = 0, ab = 0, aa = 0, n = 0;
            for (int y = y0; y < y1 && y < BANNER_H; y++)
                for (int x = x0; x < x1 && x < BANNER_W; x++) {
                    const banner_px_t *p = &px[(size_t)y * BANNER_W + x];
                    ar += (uint32_t)p->r * p->a;
                    ag += (uint32_t)p->g * p->a;
                    ab += (uint32_t)p->b * p->a;
                    aa += p->a;
                    n++;
                }
            banner_cell_px_t *o = &grid[(size_t)sy * sub_w + sx];
            uint32_t a = n ? aa / n : 0;
            if (a < BANNER_CELL_ALPHA_MIN) {
                *o = (banner_cell_px_t){0, 0, 0, false};
                continue;
            }
            /* Un-premultiply, then composite over the dark backdrop. */
            uint32_t r = ar / aa, g = ag / aa, b = ab / aa;
            o->r = (uint8_t)((r * a + 13u * (255u - a)) / 255u);
            o->g = (uint8_t)((g * a + 13u * (255u - a)) / 255u);
            o->b = (uint8_t)((b * a + 16u * (255u - a)) / 255u);
            o->on = true;
        }
    }
}

static void banner_emit_cell_frame(FILE *out, const banner_cell_px_t *grid,
                                   int cols, int rows, bool tc) {
    for (int cy = 0; cy < rows; cy++) {
        fputs("\r  ", out);
        banner_cell_px_t fg = {0, 0, 0, false}, bg = {0, 0, 0, false};
        bool have_fg = false, have_bg = false, bg_default = false;
        for (int cx = 0; cx < cols; cx++) {
            banner_cell_px_t top = grid[(size_t)(cy * 2) * cols + cx];
            banner_cell_px_t bot = grid[(size_t)(cy * 2 + 1) * cols + cx];
            if (!top.on && !bot.on) {
                if (have_fg || have_bg || bg_default) {
                    fputs("\033[0m", out);
                    have_fg = have_bg = bg_default = false;
                }
                fputc(' ', out);
                continue;
            }
            /* A transparent half keeps the terminal background: flip the
             * glyph instead of painting the backdrop colour. */
            banner_cell_px_t want_fg = top.on ? top : bot;
            const char *glyph = top.on ? "\342\226\200" /* ▀ */
                                       : "\342\226\204" /* ▄ */;
            if (top.on && bot.on) {
                if (!have_bg || bg.r != bot.r || bg.g != bot.g || bg.b != bot.b) {
                    banner_cell_bg(out, tc, bot);
                    bg = bot;
                    have_bg = true;
                    bg_default = false;
                }
            } else if (!bg_default) {
                fputs("\033[49m", out);
                have_bg = false;
                bg_default = true;
            }
            if (!have_fg || fg.r != want_fg.r || fg.g != want_fg.g ||
                fg.b != want_fg.b) {
                banner_cell_fg(out, tc, want_fg);
                fg = want_fg;
                have_fg = true;
            }
            fputs(glyph, out);
        }
        fputs("\033[0m\033[K\n", out);
    }
}

static void banner_put_utf8(FILE *out, uint32_t cp) {
    if (cp < 0x80) {
        fputc((int)cp, out);
    } else if (cp < 0x800) {
        fputc(0xC0 | (int)(cp >> 6), out);
        fputc(0x80 | (int)(cp & 63u), out);
    } else if (cp < 0x10000) {
        fputc(0xE0 | (int)(cp >> 12), out);
        fputc(0x80 | (int)((cp >> 6) & 63u), out);
        fputc(0x80 | (int)(cp & 63u), out);
    } else {
        fputc(0xF0 | (int)(cp >> 18), out);
        fputc(0x80 | (int)((cp >> 12) & 63u), out);
        fputc(0x80 | (int)((cp >> 6) & 63u), out);
        fputc(0x80 | (int)(cp & 63u), out);
    }
}

/* 6-bit sextant pattern (bit 0 = top-left, row-major) → codepoint. The
 * U+1FB00 block counts patterns in binary order but folds the four shapes
 * Unicode already had back onto their legacy codepoints. */
static uint32_t banner_sextant_cp(unsigned pat) {
    if (pat == 0) return ' ';
    if (pat == 21) return 0x258C; /* ▌ left column */
    if (pat == 42) return 0x2590; /* ▐ right column */
    if (pat == 63) return 0x2588; /* █ */
    return 0x1FB00u + pat - 1u - (pat > 21u) - (pat > 42u);
}

static inline int banner_cell_luma(banner_cell_px_t c) {
    return (299 * c.r + 587 * c.g + 114 * c.b) / 1000;
}

static banner_cell_px_t banner_cell_avg(const banner_cell_px_t *sp,
                                        unsigned bits) {
    uint32_t r = 0, g = 0, b = 0, n = 0;
    for (int k = 0; k < 6; k++)
        if (bits & (1u << k)) {
            r += sp[k].r;
            g += sp[k].g;
            b += sp[k].b;
            n++;
        }
    if (!n) return (banner_cell_px_t){0, 0, 0, false};
    return (banner_cell_px_t){(uint8_t)(r / n), (uint8_t)(g / n),
                              (uint8_t)(b / n), true};
}

/* One cell = 2×3 sub-pixels. Partially covered cells keep the terminal
 * background behind the lit pattern; fully covered cells split into a
 * bright/dark pair by luminance so fg+bg carry two colours per cell. */
static void banner_emit_sextant_frame(FILE *out, const banner_cell_px_t *grid,
                                      int cols, int rows, bool tc) {
    const int sub_w = cols * 2;
    for (int cy = 0; cy < rows; cy++) {
        fputs("\r  ", out);
        banner_cell_px_t fg = {0, 0, 0, false}, bg = {0, 0, 0, false};
        bool have_fg = false, have_bg = false, bg_default = false;
        for (int cx = 0; cx < cols; cx++) {
            banner_cell_px_t sp[6];
            unsigned on = 0;
            for (int j = 0; j < 3; j++)
                for (int i = 0; i < 2; i++) {
                    banner_cell_px_t p =
                        grid[(size_t)(cy * 3 + j) * sub_w + cx * 2 + i];
                    sp[j * 2 + i] = p;
                    if (p.on) on |= 1u << (j * 2 + i);
                }
            if (!on) {
                if (have_fg || have_bg || bg_default) {
                    fputs("\033[0m", out);
                    have_fg = have_bg = bg_default = false;
                }
                fputc(' ', out);
                continue;
            }
            unsigned pat = on;
            banner_cell_px_t want_fg, want_bg = {0, 0, 0, false};
            bool paint_bg = false;
            if (on == 63u) {
                int lmin = 255, lmax = 0;
                for (int k = 0; k < 6; k++) {
                    int l = banner_cell_luma(sp[k]);
                    if (l < lmin) lmin = l;
                    if (l > lmax) lmax = l;
                }
                if (lmax - lmin >= 24) {
                    int thresh = (lmin + lmax) / 2;
                    pat = 0;
                    for (int k = 0; k < 6; k++)
                        if (banner_cell_luma(sp[k]) > thresh) pat |= 1u << k;
                    want_fg = banner_cell_avg(sp, pat);
                    want_bg = banner_cell_avg(sp, ~pat & 63u);
                    paint_bg = true;
                } else {
                    want_fg = banner_cell_avg(sp, 63u);
                }
            } else {
                want_fg = banner_cell_avg(sp, on);
            }
            if (paint_bg) {
                if (!have_bg || bg.r != want_bg.r || bg.g != want_bg.g ||
                    bg.b != want_bg.b) {
                    banner_cell_bg(out, tc, want_bg);
                    bg = want_bg;
                    have_bg = true;
                    bg_default = false;
                }
            } else if (!bg_default) {
                fputs("\033[49m", out);
                have_bg = false;
                bg_default = true;
            }
            if (!have_fg || fg.r != want_fg.r || fg.g != want_fg.g ||
                fg.b != want_fg.b) {
                banner_cell_fg(out, tc, want_fg);
                fg = want_fg;
                have_fg = true;
            }
            banner_put_utf8(out, banner_sextant_cp(pat));
        }
        fputs("\033[0m\033[K\n", out);
    }
}

/* Pixel-native "cells": frames go over the graphics protocol once, then the
 * loop is driven client-side by retargeting the placement's current frame
 * (a=a,c=N). Same contract as the glyph renderers — plays `loops` cycles at
 * full canvas resolution, settles on the root frame, nothing stays resident
 * and no server-side loop is left running. */
static int banner_render_cells_pixel(FILE *out, int loops) {
    if (!kitty_graphics_available(out)) return 0;
    int place_cols, place_rows, cw, ch;
    if (!banner_placement_geometry(out, &place_cols, &place_rows, &cw, &ch))
        return 0;

    uint8_t *mask = banner_mask_decode();
    if (!mask) return 0;
    banner_px_t *px = malloc((size_t)cw * ch * sizeof(*px));
    if (!px) {
        free(mask);
        return 0;
    }
    banner_scene_t sc;
    banner_scene_init(&sc, mask, cw, ch);

    uint32_t image_id = 0x4350u << 16 | ((uint32_t)getpid() & 0xFFFFu);
    char control[160];

    /* Anchor over reserved rows so the image scrolls like text. The root
     * frame must go out as a=T (transmit AND display): kitty only repaints
     * frame changes on the implicit placement a=T creates — an a=p placement
     * stays frozen on frame 1 (verified on kitty 0.47.4). */
    for (int i = 0; i < place_rows; i++)
        fputc('\n', out);
    fprintf(out, "\033[%dA\r  ", place_rows);
    banner_scene_frame(px, &sc, 0, BANNER_FRAMES);
    snprintf(control, sizeof(control),
             "a=T,t=d,f=32,o=z,s=%d,v=%d,i=%u,c=%d,r=%d,C=1,q=2",
             cw, ch, image_id, place_cols, place_rows);
    bool ok = banner_send(out, control, px, (size_t)cw * ch);
    fprintf(out, "\r\033[%dB", place_rows);
    for (int f = 1; f < BANNER_FRAMES && ok; f++) {
        banner_scene_frame(px, &sc, f, BANNER_FRAMES);
        snprintf(control, sizeof(control), "a=f,i=%u,f=32,o=z,s=%d,v=%d,q=2",
                 image_id, cw, ch);
        ok = banner_send(out, control, px, (size_t)cw * ch);
    }
    free(px);
    free(mask);
    if (!ok) return 0;
    fflush(out);
    g_banner_cells_resident = true;

    if (loops < 1) loops = 1;
    int total = loops * BANNER_FRAMES;
    for (int f = 1; f <= total && !ferror(out); f++) {
        usleep(BANNER_CELL_GAP_MS * 1000);
        fprintf(out, "\033_Ga=a,i=%u,c=%d,q=2\033\\", image_id,
                f % BANNER_FRAMES + 1);
        fflush(out);
    }
    return ferror(out) ? 0 : place_rows;
}

int dsco_banner_render_cells(FILE *out, int loops) {
    if (!out || banner_env_false("DSCO_BANNER")) return 0;
    int fd = fileno(out);
    struct winsize ws;
    memset(&ws, 0, sizeof(ws));
    if (fd < 0 || !isatty(fd) || ioctl(fd, TIOCGWINSZ, &ws) != 0 ||
        ws.ws_col < 24 || ws.ws_row < 6)
        return 0;

    banner_cells_mode_t mode = banner_cells_mode();
    if (mode == BANNER_CELLS_AUTO || mode == BANNER_CELLS_PIXEL) {
        int rendered = banner_render_cells_pixel(out, loops);
        if (rendered > 0) return rendered;
        mode = mode == BANNER_CELLS_PIXEL ? BANNER_CELLS_SEXTANT : mode;
    }
    const int tile_w = mode == BANNER_CELLS_HALF ? 1 : 2;
    const int tile_h = mode == BANNER_CELLS_HALF ? 2 : 3;

    int cols = ws.ws_col - 4;
    /* Never upscale: keep at least two canvas pixels per sub-pixel. */
    if (cols * tile_w > BANNER_W / 2) cols = BANNER_W / (2 * tile_w);
    int rows = (int)lroundf((float)cols * (float)BANNER_H /
                            (2.0f * (float)BANNER_W));
    if (rows > BANNER_MAX_ROWS) {
        cols = cols * BANNER_MAX_ROWS / rows;
        rows = BANNER_MAX_ROWS;
    }
    if (rows < 2) rows = 2;
    if (cols < 20 || rows + 1 >= ws.ws_row) return 0;
    int sub_w = cols * tile_w;
    int sub_h = rows * tile_h;

    uint8_t *mask = banner_mask_decode();
    if (!mask) return 0;
    banner_px_t *px = malloc((size_t)BANNER_W * BANNER_H * sizeof(*px));
    banner_cell_px_t *grid = malloc((size_t)sub_w * sub_h * sizeof(*grid));
    if (!px || !grid) {
        free(px);
        free(grid);
        free(mask);
        return 0;
    }

    bool tc = banner_truecolor();
    if (loops < 1) loops = 1;
    int total = loops * BANNER_FRAMES;
    fputs("\033[?25l", out);
    for (int f = 0; f < total; f++) {
        banner_render_frame(px, mask, f % BANNER_FRAMES, BANNER_FRAMES);
        banner_downsample(px, grid, sub_w, sub_h);
        if (f > 0) fprintf(out, "\033[%dA", rows);
        /* Synchronised-update guards keep the in-place redraw tear-free on
         * terminals that support DEC 2026; others ignore them. */
        fputs("\033[?2026h", out);
        if (tile_w == 1)
            banner_emit_cell_frame(out, grid, cols, rows, tc);
        else
            banner_emit_sextant_frame(out, grid, cols, rows, tc);
        fputs("\033[?2026l", out);
        fflush(out);
        if (f < total - 1) usleep(BANNER_CELL_GAP_MS * 1000);
    }
    fputs("\033[?25h", out);
    fflush(out);
    free(grid);
    free(px);
    free(mask);
    return ferror(out) ? 0 : rows;
}

/* ── Public API ──────────────────────────────────────────────────────── */

void kitty_banner_clear(FILE *out) {
    if (!out) return;
    if (g_banner_root_resident) {
        fprintf(out, "\033_Ga=d,d=I,i=%u,q=2\033\\",
                0x4453u << 16 | ((uint32_t)getpid() & 0xFFFFu));
        g_banner_root_resident = false;
    }
    if (g_banner_cells_resident) {
        fprintf(out, "\033_Ga=d,d=I,i=%u,q=2\033\\",
                0x4350u << 16 | ((uint32_t)getpid() & 0xFFFFu));
        g_banner_cells_resident = false;
    }
    if (g_banner_layers_resident) {
        for (size_t l = 0; l < BANNER_LAYER_COUNT; l++)
            fprintf(out, "\033_Ga=d,d=I,i=%u,q=2\033\\",
                    banner_layer_image_id(l));
        g_banner_layers_resident = false;
    }
    fflush(out);
}

bool kitty_banner_available(FILE *out) {
    if (!out || banner_env_false("DSCO_KITTY_BANNER")) return false;
    return kitty_graphics_available(out);
}

/* The server-side animation loop (a=f frames + a=a,s=3) is only trusted on
 * real kitty. Other terminals that render kitty images (iTerm2, ghostty,
 * wezterm) ignore or quota-limit the animation extension, which strands the
 * placement frozen on frame 1 — for those the client-driven layered renderer
 * keeps the pixels moving over base graphics commands alone. */
static bool banner_server_animation_hint(void) {
    if (getenv("KITTY_WINDOW_ID")) return true;
    const char *term = getenv("TERM");
    return term && strstr(term, "kitty");
}

int kitty_banner_render_auto(FILE *out) {
    if (!kitty_banner_available(out)) return 0;
    if (banner_server_animation_hint()) {
        int rows = kitty_banner_render(out);
        if (rows > 0) return rows;
    }
    return kitty_banner_render_layers(out, 1);
}

/* Cell footprint for the placement: full width minus margins, aspect-scaled
 * to the pixel canvas, capped at BANNER_MAX_ROWS. `canvas_w`/`canvas_h`
 * receive the footprint in device pixels (cell metrics from the tty), so a
 * canvas of exactly that size renders 1:1 — the terminal never resamples.
 * When the terminal does not report pixel sizes we assume 9×18 cells; the
 * canvas is then still ≥ the classic raster and only ever downscaled. */
#define BANNER_CANVAS_MAX_W 8192
#define BANNER_CANVAS_MAX_H 2048

/* Device-pixel supersample factor. TIOCGWINSZ pixel fields from macOS
 * terminals (iTerm2, Terminal.app) are in points, so a 1:1 canvas gets
 * upscaled 2× onto the Retina framebuffer by the terminal; rendering at 2×
 * makes one image pixel one device pixel again. kitty reports true device
 * pixels (cell widths well past 12), so it stays at 1×. DSCO_BANNER_SCALE
 * pins a factor of 1..4. */
static int banner_supersample(int cell_w) {
    const char *v = getenv("DSCO_BANNER_SCALE");
    if (v && *v) {
        int s = atoi(v);
        if (s >= 1 && s <= 4) return s;
    }
    return cell_w <= 12 ? 2 : 1;
}

static bool banner_placement_geometry(FILE *out, int *place_cols,
                                      int *place_rows, int *canvas_w,
                                      int *canvas_h) {
    struct winsize ws;
    memset(&ws, 0, sizeof(ws));
    if (ioctl(fileno(out), TIOCGWINSZ, &ws) != 0 || ws.ws_col < 24 ||
        ws.ws_row < 8)
        return false;
    int cell_w = ws.ws_col && ws.ws_xpixel ? ws.ws_xpixel / ws.ws_col : 9;
    int cell_h = ws.ws_row && ws.ws_ypixel ? ws.ws_ypixel / ws.ws_row : 18;
    if (cell_w < 4) cell_w = 9;
    if (cell_h < 8) cell_h = 18;
    int ss = banner_supersample(cell_w);

    int cols = ws.ws_col - 4;
    int rows = (int)lroundf((float)cols * (float)cell_w * (float)BANNER_H /
                            ((float)BANNER_W * (float)cell_h));
    if (rows > BANNER_MAX_ROWS) {
        cols = cols * BANNER_MAX_ROWS / rows;
        rows = BANNER_MAX_ROWS;
    }
    if (rows < 2) rows = 2;
    if (cols < 20 || rows >= ws.ws_row) return false;
    *place_cols = cols;
    *place_rows = rows;
    if (canvas_w) {
        int w = cols * cell_w * ss;
        *canvas_w = w > BANNER_CANVAS_MAX_W ? BANNER_CANVAS_MAX_W : w;
    }
    if (canvas_h) {
        int h = rows * cell_h * ss;
        *canvas_h = h > BANNER_CANVAS_MAX_H ? BANNER_CANVAS_MAX_H : h;
    }
    return true;
}

int kitty_banner_render(FILE *out) {
    int place_cols, place_rows, cw, ch;
    if (!banner_placement_geometry(out, &place_cols, &place_rows, &cw, &ch))
        return 0;

    uint8_t *mask = banner_mask_decode();
    if (!mask) return 0;
    banner_px_t *px = malloc((size_t)cw * ch * sizeof(*px));
    if (!px) {
        free(mask);
        return 0;
    }
    banner_scene_t sc;
    banner_scene_init(&sc, mask, cw, ch);

    uint32_t image_id = 0x4453u << 16 | ((uint32_t)getpid() & 0xFFFFu);
    char control[160];

    /* Root frame goes out as a=T (transmit AND display) with the scaling keys
     * inline. Load-bearing: kitty (verified on 0.47.4) only drives the
     * animation timer for the implicit placement a=T creates — an a=p
     * placement, whenever it is issued, stays frozen on frame 1. Same
     * sequence kitten icat uses: a=T → frames → a=a,s=3. Rows are reserved
     * first so the placement lives in the scrollback like text. */
    for (int i = 0; i < place_rows; i++)
        fputc('\n', out);
    fprintf(out, "\033[%dA\r  ", place_rows);
    banner_scene_frame(px, &sc, 0, BANNER_FRAMES);
    snprintf(control, sizeof(control),
             "a=T,t=d,f=32,o=z,s=%d,v=%d,i=%u,c=%d,r=%d,C=1,q=2", cw, ch,
             image_id, place_cols, place_rows);
    bool ok = banner_send(out, control, px, (size_t)cw * ch);
    fprintf(out, "\r\033[%dB", place_rows);

    /* Root-frame gap + loading state while the remaining frames stream in. */
    fprintf(out, "\033_Ga=a,i=%u,s=2,v=1,r=1,z=%d,q=2\033\\", image_id,
            BANNER_FRAME_GAP_MS);
    for (int f = 1; f < BANNER_FRAMES && ok; f++) {
        banner_scene_frame(px, &sc, f, BANNER_FRAMES);
        snprintf(control, sizeof(control), "a=f,i=%u,f=32,o=z,s=%d,v=%d,z=%d,q=2",
                 image_id, cw, ch, BANNER_FRAME_GAP_MS);
        ok = banner_send(out, control, px, (size_t)cw * ch);
    }
    free(px);
    free(mask);
    if (!ok) return 0;

    /* Hand the loop to the terminal (s=3 = run looping, v=1 = loop forever).
     * The animation survives this process exiting. */
    fprintf(out, "\033_Ga=a,i=%u,s=3,v=1,r=1,z=%d,q=2\033\\", image_id,
            BANNER_FRAME_GAP_MS);
    fflush(out);
    if (ferror(out)) return 0;
    g_banner_root_resident = true;
    return place_rows;
}

static long banner_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

int kitty_banner_render_layers(FILE *out, int loops) {
    int place_cols, place_rows, cw, ch;
    if (!banner_placement_geometry(out, &place_cols, &place_rows, &cw, &ch))
        return 0;

    uint8_t *mask = banner_mask_decode();
    if (!mask) return 0;
    banner_px_t *px = malloc((size_t)cw * ch * sizeof(*px));
    if (!px) {
        free(mask);
        return 0;
    }
    banner_scene_t sc;
    banner_scene_init(&sc, mask, cw, ch);

    /* Client-driven flipbook over BASE graphics commands only. The animation
     * extension (a=f/a=a) is a trap here: ghostty/wezterm/konsole render
     * kitty images but ignore it, and five native-resolution animations
     * (~160 frames) overflow even kitty's own frame-storage quota — both
     * failure modes leave frame 1 frozen on screen. Instead this process
     * keeps the clock: each tick re-transmits the due layer's next frame
     * under its stable image id with a=T (same id ⇒ old image and placement
     * replaced atomically), so at most five images are ever resident and the
     * pixels change because *we* change them. Ticks batch all due layers
     * inside a DEC 2026 synchronized update so mid-tick repaints never show
     * a half-updated stack. Plays `loops` wordmark cycles, settles on the
     * final stack, and leaves it in the scrollback. */
    for (int i = 0; i < place_rows; i++)
        fputc('\n', out);
    fputs("\033[?25l", out);

    if (loops < 1) loops = 3;
    long start = banner_now_ms();
    long total_ms = (long)loops * 2400;
    long next_ms[BANNER_LAYER_COUNT];
    for (size_t l = 0; l < BANNER_LAYER_COUNT; l++)
        next_ms[l] = 0; /* every layer due on the first tick */

    char control[176];
    bool ok = true;
    for (;;) {
        long now = banner_now_ms() - start;
        bool due = false;
        for (size_t l = 0; l < BANNER_LAYER_COUNT; l++)
            if (next_ms[l] <= now) due = true;

        if (due) {
            fputs("\033[?2026h", out);
            fprintf(out, "\033[%dA\r  ", place_rows);
            for (size_t l = 0; l < BANNER_LAYER_COUNT && ok; l++) {
                if (next_ms[l] > now) continue;
                const banner_layer_t *layer = &k_banner_layers[l];
                /* Frame follows the wall clock, so a slow tick (or slow
                 * terminal) skips frames instead of dilating time — the
                 * tempos stay honest and the schedule can never spiral into
                 * permanent catch-up. */
                long slot = now / layer->gap_ms;
                int f = (int)(slot % layer->frames);
                float frac = (float)f / (float)layer->frames;
                memset(px, 0, (size_t)cw * ch * sizeof(*px));
                layer->render(px, &sc, frac, frac * 2.0f * (float)M_PI);
                snprintf(control, sizeof(control),
                         "a=T,t=d,f=32,o=z,s=%d,v=%d,i=%u,c=%d,r=%d,z=%d,C=1,q=2",
                         cw, ch, banner_layer_image_id(l), place_cols,
                         place_rows, layer->z);
                ok = banner_send(out, control, px, (size_t)cw * ch);
                next_ms[l] = (slot + 1) * layer->gap_ms;
            }
            fprintf(out, "\r\033[%dB", place_rows);
            fputs("\033[?2026l", out);
            fflush(out);
        }
        if (!ok || now >= total_ms) break;

        long next_due = next_ms[0];
        for (size_t l = 1; l < BANNER_LAYER_COUNT; l++)
            if (next_ms[l] < next_due) next_due = next_ms[l];
        long sleep_ms = next_due - (banner_now_ms() - start);
        if (sleep_ms < 1) sleep_ms = 1;
        if (sleep_ms > 50) sleep_ms = 50;
        usleep((useconds_t)(sleep_ms * 1000));
    }

    fputs("\033[?25h", out);
    fflush(out);
    free(px);
    free(mask);
    /* Even a failed run may have landed some layer images; deleting an
     * unknown id is a no-op (q=2), so always arm the exit teardown. */
    g_banner_layers_resident = true;
    return ok && !ferror(out) ? place_rows : 0;
}

/* ── PPM artifacts ───────────────────────────────────────────────────── */

static bool banner_write_ppm_buf(const char *path, const banner_px_t *px) {
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    fprintf(f, "P6\n%d %d\n255\n", BANNER_W, BANNER_H);
    bool ok = true;
    for (size_t i = 0; i < (size_t)BANNER_W * BANNER_H && ok; i++) {
        float a = px[i].a / 255.0f;
        uint8_t rgb[3] = {
            (uint8_t)(px[i].r * a + 16.0f * (1.0f - a)),
            (uint8_t)(px[i].g * a + 16.0f * (1.0f - a)),
            (uint8_t)(px[i].b * a + 20.0f * (1.0f - a)),
        };
        ok = fwrite(rgb, 1, 3, f) == 3;
    }
    if (fclose(f) != 0) ok = false;
    return ok;
}

bool kitty_banner_write_layers_ppm(const char *dir) {
    if (!dir || !*dir) return false;
    uint8_t *mask = banner_mask_decode();
    if (!mask) return false;
    const size_t total = (size_t)BANNER_W * BANNER_H;
    banner_px_t *px = malloc(total * sizeof(*px));
    banner_px_t *comp = malloc(total * sizeof(*comp));
    if (!px || !comp) {
        free(px);
        free(comp);
        free(mask);
        return false;
    }

    bool ok = true;
    char path[1024];
    banner_scene_t sc;
    banner_scene_init(&sc, mask, BANNER_W, BANNER_H);

    /* Each layer alone, mid-loop so time-dependent features are visible. */
    for (size_t l = 0; l < BANNER_LAYER_COUNT && ok; l++) {
        const banner_layer_t *layer = &k_banner_layers[l];
        float frac = 0.5f;
        memset(px, 0, total * sizeof(*px));
        layer->render(px, &sc, frac, frac * 2.0f * (float)M_PI);
        snprintf(path, sizeof(path), "%s/layer_%zu_%s.ppm", dir, l,
                 layer->name);
        ok = banner_write_ppm_buf(path, px);
    }

    /* Software composite of the full stack at a few wall-clock instants —
     * the same result the terminal produces from the z-ordered placements. */
    static const int sample_ms[] = {0, 420, 840, 1260};
    for (size_t s = 0; s < sizeof(sample_ms) / sizeof(sample_ms[0]) && ok;
         s++) {
        memset(comp, 0, total * sizeof(*comp));
        for (size_t l = 0; l < BANNER_LAYER_COUNT; l++) {
            const banner_layer_t *layer = &k_banner_layers[l];
            int frame = sample_ms[s] / layer->gap_ms % layer->frames;
            float frac = (float)frame / (float)layer->frames;
            memset(px, 0, total * sizeof(*px));
            layer->render(px, &sc, frac, frac * 2.0f * (float)M_PI);
            for (size_t i = 0; i < total; i++)
                banner_blend_over(&comp[i], px[i].r, px[i].g, px[i].b,
                                  px[i].a / 255.0f);
        }
        snprintf(path, sizeof(path), "%s/composite_%zu.ppm", dir, s);
        ok = banner_write_ppm_buf(path, comp);
    }

    free(px);
    free(comp);
    free(mask);
    return ok;
}

bool kitty_banner_write_ppm(const char *path, int frame, int frames) {
    if (!path || !*path || frames <= 0) return false;
    uint8_t *mask = banner_mask_decode();
    if (!mask) return false;
    banner_px_t *px = malloc((size_t)BANNER_W * BANNER_H * sizeof(*px));
    if (!px) {
        free(mask);
        return false;
    }
    banner_render_frame(px, mask, frame % frames, frames);
    free(mask);
    bool ok = banner_write_ppm_buf(path, px);
    free(px);
    return ok;
}
