#define _POSIX_C_SOURCE 200809L

#include "kitty_taste_grid.h"

#include "font_compat.h"
#include "kitty_graphics.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

typedef struct { uint8_t r, g, b; } taste_rgb_t;
typedef struct {
    int width, height;                 /* backing/device pixels */
    int logical_width, logical_height; /* compositor coordinates */
    int backing_scale;
    uint8_t *pixels;
} taste_canvas_t;
typedef struct { int x, y, width, height; } taste_rect_t;

typedef struct {
    const char *name;
    const char *descriptor;
    taste_rgb_t canvas;
    taste_rgb_t surface;
    taste_rgb_t raised;
    taste_rgb_t text;
    taste_rgb_t muted;
    taste_rgb_t accent;
    taste_rgb_t border;
    int radius;
} taste_theme_t;

static uint32_t s_image_id;

static const taste_rgb_t BOARD_TOP = {10, 13, 17};
static const taste_rgb_t BOARD_BOTTOM = {5, 7, 10};
static const taste_rgb_t BOARD_TEXT = {232, 237, 241};
static const taste_rgb_t BOARD_MUTED = {127, 139, 150};
static const taste_rgb_t BOARD_ACCENT = {122, 178, 151};

static const taste_theme_t THEMES[8] = {
    {
        "Quiet Editorial", "reading first / restrained chrome",
        {20, 20, 18}, {27, 27, 24}, {34, 34, 30}, {239, 235, 226},
        {155, 150, 139}, {164, 198, 171}, {63, 62, 56}, 8,
    },
    {
        "Native Glass", "depth / softness / contextual rail",
        {7, 12, 18}, {16, 25, 34}, {26, 38, 49}, {225, 237, 245},
        {132, 153, 168}, {104, 190, 212}, {67, 91, 107}, 18,
    },
    {
        "Industrial", "hard grid / blunt hierarchy",
        {13, 13, 12}, {22, 22, 20}, {35, 34, 31}, {238, 232, 219},
        {156, 151, 139}, {255, 177, 41}, {205, 197, 180}, 0,
    },
    {
        "Phosphor", "terminal-native / signal dense",
        {0, 5, 2}, {1, 11, 5}, {2, 18, 8}, {126, 255, 158},
        {62, 157, 86}, {86, 255, 125}, {38, 119, 60}, 0,
    },
    {
        "Warm Studio", "human / conversational / tactile",
        {28, 20, 18}, {40, 29, 26}, {55, 39, 34}, {247, 234, 217},
        {178, 148, 131}, {231, 139, 105}, {91, 64, 55}, 14,
    },
    {
        "Blueprint", "technical / diagrammatic / precise",
        {5, 22, 40}, {8, 32, 55}, {11, 43, 70}, {210, 239, 247},
        {110, 166, 187}, {91, 207, 235}, {45, 112, 140}, 2,
    },
    {
        "Paper", "light / document-like / calm",
        {235, 231, 220}, {249, 247, 240}, {255, 254, 249}, {37, 39, 41},
        {112, 111, 105}, {55, 96, 181}, {196, 191, 178}, 7,
    },
    {
        "Spatial Command", "floating controls / focused telemetry",
        {10, 7, 17}, {20, 16, 31}, {30, 23, 47}, {236, 230, 247},
        {145, 132, 164}, {166, 133, 255}, {73, 57, 98}, 18,
    },
};

static taste_canvas_t *canvas_new(int width, int height, int backing_scale) {
    if (backing_scale < 1 || backing_scale > 4) return NULL;
    if (width < 1000 || height < 620 || width > 8192 || height > 8192 ||
        (size_t)width * (size_t)height > 40000000U)
        return NULL;
    taste_canvas_t *canvas = calloc(1, sizeof(*canvas));
    if (!canvas) return NULL;
    canvas->pixels = calloc((size_t)width * (size_t)height, 3U);
    if (!canvas->pixels) {
        free(canvas);
        return NULL;
    }
    canvas->width = width;
    canvas->height = height;
    canvas->backing_scale = backing_scale;
    canvas->logical_width = width / backing_scale;
    canvas->logical_height = height / backing_scale;
    return canvas;
}

static void canvas_free(taste_canvas_t *canvas) {
    if (!canvas) return;
    free(canvas->pixels);
    free(canvas);
}

static taste_rgb_t mix(taste_rgb_t a, taste_rgb_t b, float amount) {
    if (amount < 0.0f) amount = 0.0f;
    if (amount > 1.0f) amount = 1.0f;
    return (taste_rgb_t){
        (uint8_t)((float)a.r + ((float)b.r - a.r) * amount),
        (uint8_t)((float)a.g + ((float)b.g - a.g) * amount),
        (uint8_t)((float)a.b + ((float)b.b - a.b) * amount),
    };
}

static void put_pixel(taste_canvas_t *canvas, int x, int y,
                      taste_rgb_t color, float alpha) {
    if (!canvas || !canvas->pixels || x < 0 || y < 0 ||
        x >= canvas->width || y >= canvas->height || alpha <= 0.0f)
        return;
    if (alpha > 1.0f) alpha = 1.0f;
    uint8_t *pixel = canvas->pixels +
        ((size_t)y * (size_t)canvas->width + (size_t)x) * 3U;
    pixel[0] = (uint8_t)((float)pixel[0] + ((float)color.r - pixel[0]) * alpha);
    pixel[1] = (uint8_t)((float)pixel[1] + ((float)color.g - pixel[1]) * alpha);
    pixel[2] = (uint8_t)((float)pixel[2] + ((float)color.b - pixel[2]) * alpha);
}

static void fill_rect_device(taste_canvas_t *canvas, int x, int y,
                             int width, int height,
                             taste_rgb_t color, float alpha) {
    if (!canvas || width <= 0 || height <= 0) return;
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + width > canvas->width ? canvas->width : x + width;
    int y1 = y + height > canvas->height ? canvas->height : y + height;
    for (int yy = y0; yy < y1; yy++)
        for (int xx = x0; xx < x1; xx++)
            put_pixel(canvas, xx, yy, color, alpha);
}

static void fill_rect(taste_canvas_t *canvas, int x, int y, int width, int height,
                      taste_rgb_t color, float alpha) {
    if (!canvas) return;
    int scale = canvas->backing_scale;
    fill_rect_device(canvas, x * scale, y * scale, width * scale, height * scale,
                     color, alpha);
}

static bool rounded_contains(int local_x, int local_y,
                             int width, int height, int radius) {
    if (local_x < 0 || local_y < 0 || local_x >= width || local_y >= height)
        return false;
    if (radius <= 0 ||
        (local_x >= radius && local_x < width - radius) ||
        (local_y >= radius && local_y < height - radius))
        return true;
    int cx = local_x < radius ? radius - 1 : width - radius;
    int cy = local_y < radius ? radius - 1 : height - radius;
    int dx = local_x - cx;
    int dy = local_y - cy;
    return dx * dx + dy * dy <= radius * radius;
}

static void fill_rounded(taste_canvas_t *canvas, int x, int y, int width, int height,
                         int radius, taste_rgb_t color, float alpha) {
    if (!canvas || width <= 0 || height <= 0) return;
    int scale = canvas->backing_scale;
    x *= scale;
    y *= scale;
    width *= scale;
    height *= scale;
    radius *= scale;
    if (radius < 0) radius = 0;
    int max_radius = (width < height ? width : height) / 2;
    if (radius > max_radius) radius = max_radius;
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + width > canvas->width ? canvas->width : x + width;
    int y1 = y + height > canvas->height ? canvas->height : y + height;
    for (int yy = y0; yy < y1; yy++)
        for (int xx = x0; xx < x1; xx++)
            if (rounded_contains(xx - x, yy - y, width, height, radius))
                put_pixel(canvas, xx, yy, color, alpha);
}

static void stroke_rounded(taste_canvas_t *canvas, int x, int y,
                           int width, int height, int radius, int stroke,
                           taste_rgb_t color, float alpha) {
    if (!canvas) return;
    int scale = canvas->backing_scale;
    x *= scale;
    y *= scale;
    width *= scale;
    height *= scale;
    radius *= scale;
    stroke *= scale;
    if (stroke < 1) stroke = 1;
    for (int yy = 0; yy < height; yy++) {
        for (int xx = 0; xx < width; xx++) {
            bool outer = rounded_contains(xx, yy, width, height, radius);
            bool inner = rounded_contains(xx - stroke, yy - stroke,
                                          width - stroke * 2,
                                          height - stroke * 2,
                                          radius > stroke ? radius - stroke : 0);
            if (outer && !inner) put_pixel(canvas, x + xx, y + yy, color, alpha);
        }
    }
}

static void draw_shadow(taste_canvas_t *canvas, int x, int y, int width, int height,
                        int radius, float strength) {
    fill_rounded(canvas, x - 2, y + 7, width + 4, height + 2,
                 radius + 3, (taste_rgb_t){0, 0, 0}, strength * 0.18f);
    fill_rounded(canvas, x, y + 4, width, height,
                 radius + 1, (taste_rgb_t){0, 0, 0}, strength * 0.26f);
}

static void draw_line(taste_canvas_t *canvas, int x0, int y0, int x1, int y1,
                      taste_rgb_t color, float alpha) {
    if (!canvas) return;
    int thickness = canvas->backing_scale;
    x0 *= thickness;
    y0 *= thickness;
    x1 *= thickness;
    y1 *= thickness;
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int error = dx + dy;
    for (;;) {
        for (int yy = 0; yy < thickness; yy++)
            for (int xx = 0; xx < thickness; xx++)
                put_pixel(canvas, x0 + xx, y0 + yy, color, alpha);
        if (x0 == x1 && y0 == y1) break;
        int twice = error * 2;
        if (twice >= dy) { error += dy; x0 += sx; }
        if (twice <= dx) { error += dx; y0 += sy; }
    }
}

static void draw_circle(taste_canvas_t *canvas, int cx, int cy, int radius,
                        taste_rgb_t color, float alpha) {
    if (!canvas) return;
    int scale = canvas->backing_scale;
    cx *= scale;
    cy *= scale;
    radius *= scale;
    for (int yy = -radius; yy <= radius; yy++)
        for (int xx = -radius; xx <= radius; xx++)
            if (xx * xx + yy * yy <= radius * radius)
                put_pixel(canvas, cx + xx, cy + yy, color, alpha);
}

static void fill_vertical_gradient(taste_canvas_t *canvas, taste_rect_t rect,
                                   taste_rgb_t top, taste_rgb_t bottom) {
    for (int yy = 0; yy < rect.height; yy++) {
        float t = rect.height > 1 ? (float)yy / (float)(rect.height - 1) : 0.0f;
        fill_rect(canvas, rect.x, rect.y + yy, rect.width, 1, mix(top, bottom, t), 1.0f);
    }
}

static uint64_t fallback_glyph(char value) {
    char ch = (char)toupper((unsigned char)value);
#define G(a,b,c,d,e,f,g) (((uint64_t)(a)<<30)|((uint64_t)(b)<<25)|\
                          ((uint64_t)(c)<<20)|((uint64_t)(d)<<15)|\
                          ((uint64_t)(e)<<10)|((uint64_t)(f)<<5)|(uint64_t)(g))
    switch (ch) {
    case 'A': return G(14,17,17,31,17,17,17); case 'B': return G(30,17,17,30,17,17,30);
    case 'C': return G(14,17,16,16,16,17,14); case 'D': return G(30,17,17,17,17,17,30);
    case 'E': return G(31,16,16,30,16,16,31); case 'F': return G(31,16,16,30,16,16,16);
    case 'G': return G(14,17,16,23,17,17,14); case 'H': return G(17,17,17,31,17,17,17);
    case 'I': return G(31,4,4,4,4,4,31); case 'J': return G(7,2,2,2,2,18,12);
    case 'K': return G(17,18,20,24,20,18,17); case 'L': return G(16,16,16,16,16,16,31);
    case 'M': return G(17,27,21,21,17,17,17); case 'N': return G(17,25,21,19,17,17,17);
    case 'O': return G(14,17,17,17,17,17,14); case 'P': return G(30,17,17,30,16,16,16);
    case 'Q': return G(14,17,17,17,21,18,13); case 'R': return G(30,17,17,30,20,18,17);
    case 'S': return G(15,16,16,14,1,1,30); case 'T': return G(31,4,4,4,4,4,4);
    case 'U': return G(17,17,17,17,17,17,14); case 'V': return G(17,17,17,17,17,10,4);
    case 'W': return G(17,17,17,21,21,27,17); case 'X': return G(17,17,10,4,10,17,17);
    case 'Y': return G(17,17,10,4,4,4,4); case 'Z': return G(31,1,2,4,8,16,31);
    case '0': return G(14,17,19,21,25,17,14); case '1': return G(4,12,4,4,4,4,14);
    case '2': return G(14,17,1,2,4,8,31); case '3': return G(30,1,1,14,1,1,30);
    case '4': return G(2,6,10,18,31,2,2); case '5': return G(31,16,16,30,1,1,30);
    case '6': return G(14,16,16,30,17,17,14); case '7': return G(31,1,2,4,8,8,8);
    case '8': return G(14,17,17,14,17,17,14); case '9': return G(14,17,17,15,1,1,14);
    case '/': return G(1,2,2,4,8,8,16); case '-': return G(0,0,0,31,0,0,0);
    case '.': return G(0,0,0,0,0,4,4); case ':': return G(0,4,4,0,4,4,0);
    case '>': return G(8,4,2,1,2,4,8); case '[': return G(14,8,8,8,8,8,14);
    case ']': return G(14,2,2,2,2,2,14); case '$': return G(4,15,20,14,5,30,4);
    case ' ': return 0; default: return G(14,17,1,2,4,0,4);
    }
#undef G
}

static int draw_text(taste_canvas_t *canvas, int x, int y, int max_width,
                     const char *text, float size, bool bold, bool italic,
                     taste_rgb_t color, float alpha) {
    if (!canvas || !text || !*text || max_width <= 0) return 0;
    int backing_scale = canvas->backing_scale;
    x *= backing_scale;
    y *= backing_scale;
    max_width *= backing_scale;
    size *= (float)backing_scale;
    int advance = font_compat_draw_rgb_styled(
        canvas->pixels, canvas->width, canvas->height, canvas->width * 3,
        x, y, max_width, text, size, bold, italic,
        color.r, color.g, color.b, alpha);
    if (advance >= 0) return advance;
    int scale = size >= 15.0f ? 2 : 1;
    int used = 0;
    for (const char *cursor = text; *cursor; cursor++) {
        int glyph_width = 6 * scale;
        if (used + glyph_width > max_width) break;
        uint64_t glyph = fallback_glyph(*cursor);
        for (int yy = 0; yy < 7; yy++)
            for (int xx = 0; xx < 5; xx++)
                if ((glyph >> ((6 - yy) * 5 + (4 - xx))) & 1U)
                    fill_rect_device(canvas, x + used + xx * scale,
                                     y + yy * scale, scale, scale,
                                     color, alpha);
        used += glyph_width;
    }
    return used;
}

static void draw_pill(taste_canvas_t *canvas, int x, int y, int width, int height,
                      taste_rgb_t fill, taste_rgb_t text_color, const char *label) {
    fill_rounded(canvas, x, y, width, height, height / 2, fill, 0.96f);
    draw_text(canvas, x + 8, y + 4, width - 16, label, 9.5f, true, false,
              text_color, 0.96f);
}

static void draw_meter(taste_canvas_t *canvas, int x, int y, int width,
                       float value, taste_rgb_t track, taste_rgb_t fill) {
    fill_rounded(canvas, x, y, width, 5, 2, track, 0.72f);
    fill_rounded(canvas, x, y, (int)((float)width * value), 5, 2, fill, 0.94f);
}

static void draw_editorial(taste_canvas_t *c, taste_rect_t r,
                           const taste_theme_t *t) {
    fill_rect(c, r.x, r.y, r.width, r.height, t->canvas, 1.0f);
    int pad = 18;
    draw_text(c, r.x + pad, r.y + 15, r.width / 2, "DSCO", 15.0f, true, false,
              t->text, 0.98f);
    draw_text(c, r.x + pad + 52, r.y + 18, r.width / 2,
              "gpt-5.6-terra", 10.0f, false, false, t->muted, 0.92f);
    draw_text(c, r.x + r.width - 55, r.y + 18, 40, "Idle", 10.0f, false, false,
              t->accent, 0.96f);
    fill_rect(c, r.x + pad, r.y + 43, r.width - pad * 2, 1, t->border, 0.74f);
    int y = r.y + 62;
    draw_text(c, r.x + pad, y, 90, "System", 9.5f, true, false, t->muted, 0.86f);
    draw_text(c, r.x + 92, y, r.width - 112, "Ready · 285 tools · trusted",
              10.5f, false, false, t->muted, 0.82f);
    y += 42;
    draw_text(c, r.x + pad, y, 70, "You", 10.0f, true, false, t->accent, 0.96f);
    draw_text(c, r.x + 92, y, r.width - 112, "hello", 12.0f, false, false,
              t->text, 0.98f);
    y += 51;
    draw_text(c, r.x + pad, y, 70, "DSCO", 10.0f, true, false, t->muted, 0.90f);
    draw_text(c, r.x + 92, y, r.width - 112,
              "What are we building or repairing?", 12.0f, false, false,
              t->text, 0.98f);
    int composer_y = r.y + r.height - 50;
    fill_rect(c, r.x + pad, composer_y, r.width - pad * 2, 1, t->border, 0.82f);
    draw_text(c, r.x + pad, composer_y + 14, r.width - 80, "Ask DSCO…",
              11.5f, false, true, t->muted, 0.86f);
    draw_circle(c, r.x + r.width - pad - 13, composer_y + 22, 13, t->accent, 0.88f);
    draw_text(c, r.x + r.width - pad - 17, composer_y + 13, 12, ">", 11.0f,
              true, false, t->canvas, 1.0f);
}

static void draw_glass(taste_canvas_t *c, taste_rect_t r,
                       const taste_theme_t *t) {
    fill_vertical_gradient(c, r, t->canvas, mix(t->canvas, t->accent, 0.10f));
    draw_circle(c, r.x + r.width - 36, r.y + 45, 58, t->accent, 0.08f);
    draw_circle(c, r.x + 35, r.y + r.height - 40, 72,
                (taste_rgb_t){102, 112, 232}, 0.07f);
    int pad = 12;
    fill_rounded(c, r.x + pad, r.y + 12, r.width - pad * 2, 44, 15,
                 t->raised, 0.72f);
    stroke_rounded(c, r.x + pad, r.y + 12, r.width - pad * 2, 44, 15, 1,
                   t->border, 0.52f);
    draw_circle(c, r.x + 30, r.y + 34, 6, t->accent, 0.94f);
    draw_text(c, r.x + 44, r.y + 22, r.width / 2, "DSCO workspace", 12.5f,
              true, false, t->text, 0.96f);
    draw_pill(c, r.x + r.width - 72, r.y + 22, 48, 22, t->accent, t->canvas, "IDLE");
    int rail_w = 82;
    int body_y = r.y + 68;
    int body_h = r.height - 126;
    fill_rounded(c, r.x + pad, body_y, r.width - pad * 2 - rail_w - 8, body_h,
                 14, t->surface, 0.68f);
    fill_rounded(c, r.x + r.width - pad - rail_w, body_y, rail_w, body_h,
                 14, t->surface, 0.58f);
    int tx = r.x + pad + 14;
    draw_text(c, tx, body_y + 16, 90, "Ready", 9.5f, true, false,
              t->accent, 0.92f);
    draw_text(c, tx, body_y + 38, r.width - rail_w - 60,
              "285 tools connected", 10.0f, false, false, t->muted, 0.86f);
    fill_rounded(c, tx, body_y + 74, 126, 36, 11, t->raised, 0.84f);
    draw_text(c, tx + 12, body_y + 83, 102, "You   hello", 10.5f,
              false, false, t->text, 0.96f);
    fill_rounded(c, tx, body_y + 120, r.width - rail_w - 68, 48, 11,
                 mix(t->raised, t->accent, 0.10f), 0.86f);
    draw_text(c, tx + 12, body_y + 130, r.width - rail_w - 92,
              "What are we building?", 10.5f, false, false, t->text, 0.98f);
    int rx = r.x + r.width - pad - rail_w + 10;
    draw_text(c, rx, body_y + 16, rail_w - 20, "SESSION", 8.5f, true, false,
              t->muted, 0.76f);
    draw_text(c, rx, body_y + 46, rail_w - 20, "Context", 8.5f, false, false,
              t->muted, 0.78f);
    draw_meter(c, rx, body_y + 63, rail_w - 20, 0.18f, t->raised, t->accent);
    draw_text(c, rx, body_y + 87, rail_w - 20, "$0.00", 10.0f, true, false,
              t->text, 0.92f);
    int composer_y = r.y + r.height - 46;
    fill_rounded(c, r.x + 22, composer_y, r.width - 44, 34, 17,
                 t->raised, 0.88f);
    draw_text(c, r.x + 38, composer_y + 8, r.width - 100, "Ask DSCO…", 10.5f,
              false, false, t->muted, 0.88f);
    draw_circle(c, r.x + r.width - 39, composer_y + 17, 10, t->accent, 0.94f);
}

static void draw_industrial(taste_canvas_t *c, taste_rect_t r,
                            const taste_theme_t *t) {
    fill_rect(c, r.x, r.y, r.width, r.height, t->canvas, 1.0f);
    fill_rect(c, r.x + 10, r.y + 10, r.width - 20, 36, t->accent, 1.0f);
    draw_text(c, r.x + 20, r.y + 18, r.width - 130, "DSCO // OPERATIONS", 12.0f,
              true, false, t->canvas, 1.0f);
    fill_rect(c, r.x + r.width - 92, r.y + 10, 82, 36, t->text, 1.0f);
    draw_text(c, r.x + r.width - 75, r.y + 18, 52, "IDLE", 11.0f,
              true, false, t->canvas, 1.0f);
    int x = r.x + 10, y = r.y + 56, w = r.width - 20, h = r.height - 112;
    stroke_rounded(c, x, y, w, h, 0, 2, t->border, 1.0f);
    fill_rect(c, x + 2, y + 2, w - 4, h - 4, t->surface, 1.0f);
    fill_rect(c, x + 2, y + 2, 86, h - 4, t->raised, 1.0f);
    draw_text(c, x + 12, y + 18, 64, "SYSTEM", 9.0f, true, false,
              t->accent, 1.0f);
    draw_text(c, x + 100, y + 18, w - 112, "READY / 285 TOOLS / TRUSTED",
              10.0f, true, false, t->text, 0.94f);
    fill_rect(c, x + 2, y + 48, w - 4, 2, t->border, 0.68f);
    draw_text(c, x + 12, y + 66, 64, "YOU", 9.0f, true, false,
              t->accent, 1.0f);
    draw_text(c, x + 100, y + 64, w - 112, "HELLO", 12.0f, true, false,
              t->text, 1.0f);
    draw_text(c, x + 12, y + 111, 64, "DSCO", 9.0f, true, false,
              t->muted, 1.0f);
    draw_text(c, x + 100, y + 108, w - 112, "WHAT ARE WE BUILDING?", 11.0f,
              true, false, t->text, 1.0f);
    int composer_y = r.y + r.height - 46;
    fill_rect(c, r.x + 10, composer_y, 72, 34, t->accent, 1.0f);
    draw_text(c, r.x + 22, composer_y + 8, 48, "INPUT", 9.5f, true, false,
              t->canvas, 1.0f);
    stroke_rounded(c, r.x + 82, composer_y, r.width - 92, 34, 0, 2,
                   t->border, 1.0f);
    draw_text(c, r.x + 96, composer_y + 8, r.width - 120, "TYPE COMMAND_", 10.0f,
              true, false, t->text, 0.84f);
}

static void draw_phosphor(taste_canvas_t *c, taste_rect_t r,
                          const taste_theme_t *t) {
    fill_rect(c, r.x, r.y, r.width, r.height, t->canvas, 1.0f);
    for (int yy = r.y; yy < r.y + r.height; yy += 4)
        fill_rect(c, r.x, yy, r.width, 1, t->accent, 0.035f);
    stroke_rounded(c, r.x + 8, r.y + 8, r.width - 16, r.height - 16,
                   0, 1, t->border, 0.86f);
    draw_text(c, r.x + 20, r.y + 19, r.width - 130, "DSCO TTY / GPT-5.6-TERRA",
              11.0f, true, false, t->text, 0.98f);
    draw_text(c, r.x + r.width - 72, r.y + 19, 52, "[IDLE]", 10.0f,
              true, false, t->accent, 0.96f);
    fill_rect(c, r.x + 18, r.y + 43, r.width - 36, 1, t->border, 0.76f);
    int x = r.x + 22, y = r.y + 62;
    draw_text(c, x, y, r.width - 44, "> boot workspace", 10.5f, false, false,
              t->muted, 0.92f);
    draw_text(c, x, y + 28, r.width - 44, "  ok 285 tools / trusted", 10.5f,
              false, false, t->text, 0.92f);
    draw_text(c, x, y + 72, r.width - 44, "you@dsco:~$ hello", 11.0f,
              false, false, t->accent, 0.98f);
    draw_text(c, x, y + 105, r.width - 44, "dsco> What are we building?", 11.0f,
              false, false, t->text, 0.98f);
    draw_text(c, x, r.y + r.height - 46, r.width - 44, "you@dsco:~$ _", 11.0f,
              true, false, t->accent, 1.0f);
    draw_text(c, r.x + r.width - 112, r.y + r.height - 24, 92,
              "q:close  /help", 8.5f, false, false, t->muted, 0.82f);
}

static void draw_warm(taste_canvas_t *c, taste_rect_t r,
                      const taste_theme_t *t) {
    fill_vertical_gradient(c, r, mix(t->canvas, t->accent, 0.04f), t->canvas);
    int pad = 16;
    draw_text(c, r.x + pad, r.y + 17, r.width / 2, "DSCO", 15.0f, true, false,
              t->text, 0.98f);
    draw_text(c, r.x + pad + 53, r.y + 21, r.width / 2, "with Arthur", 10.0f,
              false, true, t->muted, 0.90f);
    draw_circle(c, r.x + r.width - 34, r.y + 27, 7, t->accent, 0.92f);
    int content_y = r.y + 60;
    draw_text(c, r.x + pad, content_y, r.width - pad * 2, "Ready when you are.",
              10.0f, false, false, t->muted, 0.88f);
    int user_w = 124;
    fill_rounded(c, r.x + r.width - pad - user_w, content_y + 34,
                 user_w, 40, 16, t->accent, 0.94f);
    draw_text(c, r.x + r.width - pad - user_w + 16, content_y + 44,
              user_w - 32, "hello", 11.5f, false, false, t->canvas, 1.0f);
    int assist_w = r.width - pad * 2 - 42;
    fill_rounded(c, r.x + pad, content_y + 88, assist_w, 54, 16,
                 t->raised, 0.98f);
    draw_text(c, r.x + pad + 16, content_y + 98, assist_w - 32,
              "What are we building", 11.5f, false, false, t->text, 0.98f);
    draw_text(c, r.x + pad + 16, content_y + 117, assist_w - 32,
              "or repairing?", 11.5f, false, false, t->text, 0.98f);
    int composer_y = r.y + r.height - 52;
    draw_shadow(c, r.x + pad, composer_y, r.width - pad * 2, 39, 18, 0.55f);
    fill_rounded(c, r.x + pad, composer_y, r.width - pad * 2, 39, 18,
                 t->surface, 1.0f);
    draw_text(c, r.x + pad + 16, composer_y + 10, r.width - 100,
              "Write a message…", 10.5f, false, false, t->muted, 0.88f);
    draw_circle(c, r.x + r.width - pad - 20, composer_y + 19, 13,
                t->accent, 0.92f);
}

static void draw_blueprint(taste_canvas_t *c, taste_rect_t r,
                           const taste_theme_t *t) {
    fill_rect(c, r.x, r.y, r.width, r.height, t->canvas, 1.0f);
    for (int xx = r.x; xx < r.x + r.width; xx += 18)
        fill_rect(c, xx, r.y, 1, r.height, t->accent, 0.055f);
    for (int yy = r.y; yy < r.y + r.height; yy += 18)
        fill_rect(c, r.x, yy, r.width, 1, t->accent, 0.055f);
    draw_text(c, r.x + 16, r.y + 14, r.width - 120, "DSCO / SYSTEM MAP",
              12.0f, true, false, t->text, 0.96f);
    draw_text(c, r.x + r.width - 90, r.y + 16, 74, "STATE: IDLE",
              8.5f, true, false, t->accent, 0.96f);
    fill_rect(c, r.x + 16, r.y + 40, r.width - 32, 1, t->accent, 0.58f);
    int body_y = r.y + 54;
    int rail_w = 88;
    stroke_rounded(c, r.x + 16, body_y, r.width - 40 - rail_w,
                   r.height - 116, 0, 1, t->border, 0.90f);
    stroke_rounded(c, r.x + r.width - 16 - rail_w, body_y, rail_w,
                   r.height - 116, 0, 1, t->border, 0.90f);
    draw_text(c, r.x + 26, body_y + 12, r.width - rail_w - 72,
              "TRANSCRIPT / A-01", 8.5f, true, false, t->muted, 0.88f);
    draw_text(c, r.x + 26, body_y + 42, r.width - rail_w - 72,
              "YOU  > hello", 10.0f, false, false, t->accent, 0.96f);
    draw_text(c, r.x + 26, body_y + 76, r.width - rail_w - 72,
              "DSCO > What are we building?", 10.0f, false, false,
              t->text, 0.98f);
    int rx = r.x + r.width - rail_w - 6;
    draw_text(c, rx, body_y + 12, rail_w - 20, "ENVELOPE", 8.0f, true, false,
              t->muted, 0.86f);
    draw_text(c, rx, body_y + 40, rail_w - 20, "CTX  0%", 8.5f, false, false,
              t->text, 0.90f);
    draw_meter(c, rx, body_y + 57, rail_w - 20, 0.08f, t->raised, t->accent);
    draw_text(c, rx, body_y + 81, rail_w - 20, "Q  0/8", 8.5f, false, false,
              t->text, 0.90f);
    int composer_y = r.y + r.height - 48;
    stroke_rounded(c, r.x + 16, composer_y, r.width - 32, 32, 0, 1,
                   t->accent, 0.92f);
    draw_text(c, r.x + 27, composer_y + 8, r.width - 54, "> INPUT VECTOR_",
              9.5f, true, false, t->text, 0.92f);
    draw_line(c, r.x + 8, r.y + 8, r.x + 26, r.y + 8, t->accent, 0.72f);
    draw_line(c, r.x + 8, r.y + 8, r.x + 8, r.y + 26, t->accent, 0.72f);
}

static void draw_paper(taste_canvas_t *c, taste_rect_t r,
                       const taste_theme_t *t) {
    fill_rect(c, r.x, r.y, r.width, r.height, t->canvas, 1.0f);
    draw_shadow(c, r.x + 12, r.y + 10, r.width - 24, r.height - 22, 8, 0.44f);
    fill_rounded(c, r.x + 12, r.y + 10, r.width - 24, r.height - 22, 8,
                 t->surface, 1.0f);
    int x = r.x + 30;
    draw_text(c, x, r.y + 27, r.width / 2, "DSCO workspace", 14.0f,
              true, false, t->text, 0.98f);
    draw_text(c, r.x + r.width - 86, r.y + 31, 56, "Idle · 0%", 9.0f,
              false, false, t->accent, 0.94f);
    fill_rect(c, x, r.y + 57, r.width - 60, 1, t->border, 0.92f);
    draw_text(c, x, r.y + 76, 94, "Session note", 9.0f, true, false,
              t->muted, 0.90f);
    draw_text(c, x + 92, r.y + 76, r.width - 152,
              "Ready · trusted · 285 tools", 10.0f, false, true,
              t->muted, 0.86f);
    draw_text(c, x, r.y + 118, 54, "You", 9.5f, true, false,
              t->accent, 0.96f);
    draw_text(c, x + 70, r.y + 116, r.width - 130, "hello", 11.5f,
              false, false, t->text, 0.98f);
    draw_text(c, x, r.y + 162, 54, "DSCO", 9.5f, true, false,
              t->muted, 0.92f);
    draw_text(c, x + 70, r.y + 160, r.width - 130,
              "What are we building or repairing?", 11.5f,
              false, false, t->text, 0.98f);
    int composer_y = r.y + r.height - 61;
    fill_rounded(c, x, composer_y, r.width - 60, 36, 7, t->raised, 1.0f);
    stroke_rounded(c, x, composer_y, r.width - 60, 36, 7, 1,
                   t->border, 0.86f);
    draw_text(c, x + 12, composer_y + 9, r.width - 120, "Ask DSCO…", 10.5f,
              false, true, t->muted, 0.82f);
    draw_text(c, r.x + r.width - 69, composer_y + 9, 28, "Send", 9.5f,
              true, false, t->accent, 0.96f);
}

static void draw_spatial(taste_canvas_t *c, taste_rect_t r,
                         const taste_theme_t *t) {
    fill_vertical_gradient(c, r, t->canvas, mix(t->canvas, t->accent, 0.09f));
    draw_circle(c, r.x + r.width / 2, r.y + r.height / 2, 104,
                t->accent, 0.035f);
    draw_circle(c, r.x + r.width - 30, r.y + 95, 54,
                (taste_rgb_t){72, 199, 211}, 0.055f);
    draw_shadow(c, r.x + 20, r.y + 16, r.width - 40, 42, 20, 0.70f);
    fill_rounded(c, r.x + 20, r.y + 16, r.width - 40, 42, 20,
                 t->surface, 0.94f);
    draw_circle(c, r.x + 40, r.y + 37, 6, t->accent, 0.96f);
    draw_text(c, r.x + 54, r.y + 25, r.width / 2, "DSCO", 12.5f,
              true, false, t->text, 0.96f);
    draw_text(c, r.x + 102, r.y + 28, r.width / 2, "gpt-5.6-terra", 9.0f,
              false, false, t->muted, 0.88f);
    draw_pill(c, r.x + r.width - 79, r.y + 26, 44, 20, t->accent, t->canvas, "IDLE");
    int card_x = r.x + 38;
    int card_y = r.y + 82;
    int card_w = r.width - 76;
    int card_h = r.height - 158;
    draw_shadow(c, card_x, card_y, card_w, card_h, 18, 0.82f);
    fill_rounded(c, card_x, card_y, card_w, card_h, 18, t->surface, 0.92f);
    draw_text(c, card_x + 18, card_y + 16, card_w - 36, "Live conversation",
              9.0f, true, false, t->muted, 0.82f);
    draw_circle(c, card_x + 22, card_y + 57, 5,
                (taste_rgb_t){72, 199, 211}, 0.92f);
    draw_text(c, card_x + 36, card_y + 48, card_w - 54, "You   hello", 10.5f,
              false, false, t->text, 0.98f);
    fill_rounded(c, card_x + 16, card_y + 82, card_w - 32, 52, 13,
                 t->raised, 0.92f);
    draw_text(c, card_x + 30, card_y + 93, card_w - 60,
              "What are we building?", 11.0f, false, false, t->text, 0.98f);
    draw_text(c, card_x + 30, card_y + 113, card_w - 60,
              "Ready when you are.", 9.5f, false, false, t->muted, 0.86f);
    int composer_y = r.y + r.height - 52;
    draw_shadow(c, r.x + 28, composer_y, r.width - 56, 36, 18, 0.75f);
    fill_rounded(c, r.x + 28, composer_y, r.width - 56, 36, 18,
                 t->raised, 0.98f);
    draw_text(c, r.x + 45, composer_y + 9, r.width - 124, "Command DSCO…",
              10.0f, false, false, t->muted, 0.88f);
    draw_circle(c, r.x + r.width - 47, composer_y + 18, 11, t->accent, 0.96f);
}

typedef void (*taste_draw_fn)(taste_canvas_t *, taste_rect_t,
                              const taste_theme_t *);

static void draw_card(taste_canvas_t *canvas, taste_rect_t card, int index,
                      taste_draw_fn draw) {
    const taste_theme_t *theme = &THEMES[index];
    int card_radius = index == 2 || index == 3 || index == 5 ? 5 : 15;
    draw_shadow(canvas, card.x, card.y, card.width, card.height,
                card_radius, index == 6 ? 0.30f : 0.85f);
    fill_rounded(canvas, card.x, card.y, card.width, card.height,
                 card_radius, theme->canvas, 1.0f);
    stroke_rounded(canvas, card.x, card.y, card.width, card.height,
                   card_radius, 1, mix(theme->border, theme->text, 0.08f), 0.58f);

    int label_h = 58;
    draw_pill(canvas, card.x + 14, card.y + 14, 32, 24,
              theme->accent, theme->canvas, index == 0 ? "01" :
              index == 1 ? "02" : index == 2 ? "03" : index == 3 ? "04" :
              index == 4 ? "05" : index == 5 ? "06" : index == 6 ? "07" : "08");
    draw_text(canvas, card.x + 56, card.y + 11, card.width - 70,
              theme->name, 13.0f, true, false, theme->text, 0.98f);
    draw_text(canvas, card.x + 56, card.y + 31, card.width - 70,
              theme->descriptor, 8.8f, false, false, theme->muted, 0.88f);
    taste_rect_t preview = {
        card.x + 9, card.y + label_h,
        card.width - 18, card.height - label_h - 9,
    };
    fill_rounded(canvas, preview.x, preview.y, preview.width, preview.height,
                 theme->radius, theme->canvas, 1.0f);
    draw(canvas, preview, theme);
}

static void render_grid(taste_canvas_t *canvas) {
    int logical_width = canvas->logical_width;
    int logical_height = canvas->logical_height;
    taste_rect_t full = {0, 0, logical_width, logical_height};
    fill_vertical_gradient(canvas, full, BOARD_TOP, BOARD_BOTTOM);
    draw_circle(canvas, logical_width - 130, 28, 140, BOARD_ACCENT, 0.025f);
    draw_text(canvas, 28, 18, logical_width - 56,
              "DSCO / TASTE STUDY", 23.0f, true, false, BOARD_TEXT, 0.98f);
    draw_text(canvas, 30, 50, logical_width - 60,
              "Eight directions · same content · compare hierarchy, density, surface, and tone",
              11.0f, false, false, BOARD_MUTED, 0.92f);
    char resolution[96];
    snprintf(resolution, sizeof(resolution), "%d × %d / %d× NATIVE RGB",
             canvas->width, canvas->height, canvas->backing_scale);
    draw_text(canvas, logical_width - 238, 24, 208,
              resolution, 10.0f, true, false, BOARD_ACCENT, 0.92f);

    const int margin = 24;
    const int gap = 16;
    const int grid_y = 82;
    const int footer_h = 32;
    int card_width = (logical_width - margin * 2 - gap * 3) / 4;
    int card_height = (logical_height - grid_y - margin - footer_h - gap) / 2;
    taste_draw_fn draws[8] = {
        draw_editorial, draw_glass, draw_industrial, draw_phosphor,
        draw_warm, draw_blueprint, draw_paper, draw_spatial,
    };
    for (int i = 0; i < 8; i++) {
        int column = i % 4;
        int row = i / 4;
        taste_rect_t card = {
            margin + column * (card_width + gap),
            grid_y + row * (card_height + gap),
            card_width, card_height,
        };
        draw_card(canvas, card, i, draws[i]);
    }
    draw_text(canvas, margin, logical_height - 25, logical_width - margin * 2,
              "Choose by number · Q / Esc / Enter closes · exact backing pixels / zero placement scaling",
              9.5f, false, false, BOARD_MUTED, 0.90f);
}

bool kitty_taste_grid_write_ppm(const char *path, int width, int height) {
    if (!path || !*path) return false;
    taste_canvas_t *canvas = canvas_new(width, height, 1);
    if (!canvas) return false;
    render_grid(canvas);
    FILE *file = fopen(path, "wb");
    if (!file) {
        canvas_free(canvas);
        return false;
    }
    size_t bytes = (size_t)width * (size_t)height * 3U;
    bool ok = fprintf(file, "P6\n%d %d\n255\n", width, height) > 0 &&
              fwrite(canvas->pixels, 1, bytes, file) == bytes;
    if (fclose(file) != 0) ok = false;
    canvas_free(canvas);
    return ok;
}

static int live_backing_scale(const struct winsize *window) {
    const char *override = getenv("DSCO_TASTE_DPR");
    if (override && *override) {
        char *end = NULL;
        long requested = strtol(override, &end, 10);
        if (end != override && *end == '\0' && requested >= 1 && requested <= 4)
            return (int)requested;
    }
    if (!window || window->ws_col == 0 || window->ws_row == 0) return 1;
    int cell_width = window->ws_xpixel / window->ws_col;
    int cell_height = window->ws_ypixel / window->ws_row;
    /* Kitty reports backing pixels in ws_xpixel/ws_ypixel. A normal 11-14pt
     * terminal cell is roughly 7x15 at 1x and 14x30 on a Retina backing
     * store. Keep logical composition stable while rasterizing every edge
     * and CoreText glyph into all of those device pixels. */
    return cell_width >= 14 || cell_height >= 24 ? 2 : 1;
}

bool kitty_taste_grid_render(FILE *out, int width, int height) {
    if (!out || !kitty_graphics_available(out)) return false;
    struct winsize window;
    memset(&window, 0, sizeof(window));
    (void)ioctl(fileno(out), TIOCGWINSZ, &window);
    if (width <= 0 && window.ws_xpixel > 0) width = window.ws_xpixel;
    if (height <= 0 && window.ws_ypixel > 0) height = window.ws_ypixel;
    if (width <= 0) width = 3600;
    if (height <= 0) height = 2000;
    int backing_scale = live_backing_scale(&window);
    taste_canvas_t *canvas = canvas_new(width, height, backing_scale);
    if (!canvas) return false;
    render_grid(canvas);
    s_image_id = 0x54535400U ^ ((uint32_t)getpid() << 5) ^ (uint32_t)width;
    if (!s_image_id) s_image_id = 1;
    fprintf(out, "\033[?1049h\033[2J\033[H\033[?25l");
    char control[256];
    snprintf(control, sizeof(control),
             "a=t,t=d,f=24,s=%d,v=%d,i=%u,q=2,o=z",
             width, height, s_image_id);
    kitty_graphics_send_options_t options;
    kitty_graphics_send_options_default(&options);
    bool ok = kitty_graphics_send_pixels(
        out, control, canvas->pixels,
        (size_t)width * (size_t)height * 3U, &options);
    if (ok) {
        int columns = 180, rows = 55;
        if (window.ws_col > 0) columns = window.ws_col;
        if (window.ws_row > 0) rows = window.ws_row;
        fprintf(out, "\0337\033[H\033_Ga=p,i=%u,p=1,c=%d,r=%d,C=1,z=1,q=2\033\\\0338",
                s_image_id, columns, rows);
        fflush(out);
        ok = !ferror(out);
    }
    if (!ok) {
        fprintf(out, "\033[?25h\033[?1049l");
        s_image_id = 0;
    }
    canvas_free(canvas);
    return ok;
}

void kitty_taste_grid_clear(FILE *out) {
    if (!out) return;
    if (s_image_id)
        fprintf(out, "\033_Ga=d,d=I,i=%u,q=2\033\\", s_image_id);
    fprintf(out, "\033[?25h\033[?1049l");
    fflush(out);
    s_image_id = 0;
}
