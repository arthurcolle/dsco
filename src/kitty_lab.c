#define _POSIX_C_SOURCE 200809L

#include "kitty_lab.h"

#include "kitty_graphics.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

typedef struct { uint8_t r, g, b; } lab_rgb_t;
typedef struct { int width, height; uint8_t *pixels; } lab_canvas_t;

static const lab_rgb_t LAB_BG = {8, 10, 13};
static const lab_rgb_t LAB_PANEL = {17, 21, 26};
static const lab_rgb_t LAB_RAISED = {23, 29, 35};
static const lab_rgb_t LAB_TEXT = {210, 218, 224};
static const lab_rgb_t LAB_MUTED = {105, 118, 128};
static const lab_rgb_t LAB_CYAN = {91, 190, 204};
static const lab_rgb_t LAB_GREEN = {105, 190, 133};
static const lab_rgb_t LAB_AMBER = {230, 168, 82};
static const lab_rgb_t LAB_VIOLET = {158, 120, 218};

typedef struct { char ch; uint8_t rows[7]; } lab_glyph_t;

#define GLYPH(ch, a, b, c, d, e, f, g) {ch, {a, b, c, d, e, f, g}}
static const lab_glyph_t k_glyphs[] = {
    GLYPH(' ', 0,0,0,0,0,0,0),
    GLYPH('A', 14,17,17,31,17,17,17), GLYPH('B',30,17,17,30,17,17,30),
    GLYPH('C',14,17,16,16,16,17,14), GLYPH('D',30,17,17,17,17,17,30),
    GLYPH('E',31,16,16,30,16,16,31), GLYPH('F',31,16,16,30,16,16,16),
    GLYPH('G',14,17,16,23,17,17,14), GLYPH('H',17,17,17,31,17,17,17),
    GLYPH('I',31,4,4,4,4,4,31), GLYPH('J',7,2,2,2,2,18,12),
    GLYPH('K',17,18,20,24,20,18,17), GLYPH('L',16,16,16,16,16,16,31),
    GLYPH('M',17,27,21,21,17,17,17), GLYPH('N',17,25,21,19,17,17,17),
    GLYPH('O',14,17,17,17,17,17,14), GLYPH('P',30,17,17,30,16,16,16),
    GLYPH('Q',14,17,17,17,21,18,13), GLYPH('R',30,17,17,30,20,18,17),
    GLYPH('S',15,16,16,14,1,1,30), GLYPH('T',31,4,4,4,4,4,4),
    GLYPH('U',17,17,17,17,17,17,14), GLYPH('V',17,17,17,17,17,10,4),
    GLYPH('W',17,17,17,21,21,27,17), GLYPH('X',17,17,10,4,10,17,17),
    GLYPH('Y',17,17,10,4,4,4,4), GLYPH('Z',31,1,2,4,8,16,31),
    GLYPH('0',14,17,19,21,25,17,14), GLYPH('1',4,12,4,4,4,4,14),
    GLYPH('2',14,17,1,2,4,8,31), GLYPH('3',30,1,1,14,1,1,30),
    GLYPH('4',2,6,10,18,31,2,2), GLYPH('5',31,16,16,30,1,1,30),
    GLYPH('6',14,16,16,30,17,17,14), GLYPH('7',31,1,2,4,8,8,8),
    GLYPH('8',14,17,17,14,17,17,14), GLYPH('9',14,17,17,15,1,1,14),
    GLYPH('-', 0,0,0,28,0,0,0), GLYPH('_', 0,0,0,0,0,0,31),
    GLYPH(':', 0,4,0,0,0,4,0), GLYPH('.', 0,0,0,0,0,4,4),
    GLYPH('/', 1,2,2,4,8,8,16), GLYPH('%', 25,26,4,8,11,19,0),
    GLYPH('>', 16,8,4,2,4,8,16), GLYPH('#', 10,31,10,10,31,10,0),
};
#undef GLYPH

static const lab_glyph_t *glyph_for(char ch) {
    if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
    for (size_t i = 0; i < sizeof(k_glyphs) / sizeof(k_glyphs[0]); i++)
        if (k_glyphs[i].ch == ch) return &k_glyphs[i];
    return &k_glyphs[0];
}

static lab_canvas_t *canvas_new(int width, int height) {
    if (width < 320 || height < 180 || width > 4096 || height > 4096) return NULL;
    lab_canvas_t *c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->width = width;
    c->height = height;
    c->pixels = malloc((size_t)width * (size_t)height * 3U);
    if (!c->pixels) { free(c); return NULL; }
    return c;
}

static void canvas_free(lab_canvas_t *c) {
    if (!c) return;
    free(c->pixels);
    free(c);
}

static void put_pixel(lab_canvas_t *c, int x, int y, lab_rgb_t color, float alpha) {
    if (!c || !c->pixels || x < 0 || y < 0 || x >= c->width || y >= c->height) return;
    if (alpha <= 0.0f) return;
    if (alpha > 1.0f) alpha = 1.0f;
    uint8_t *p = c->pixels + ((size_t)y * (size_t)c->width + (size_t)x) * 3U;
    p[0] = (uint8_t)((float)p[0] + ((float)color.r - p[0]) * alpha);
    p[1] = (uint8_t)((float)p[1] + ((float)color.g - p[1]) * alpha);
    p[2] = (uint8_t)((float)p[2] + ((float)color.b - p[2]) * alpha);
}

static void fill_rect(lab_canvas_t *c, int x, int y, int w, int h,
                      lab_rgb_t color, float alpha) {
    if (!c || w <= 0 || h <= 0) return;
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w > c->width ? c->width : x + w;
    int y1 = y + h > c->height ? c->height : y + h;
    for (int yy = y0; yy < y1; yy++)
        for (int xx = x0; xx < x1; xx++) put_pixel(c, xx, yy, color, alpha);
}

static void stroke_rect(lab_canvas_t *c, int x, int y, int w, int h, lab_rgb_t color) {
    if (w < 2 || h < 2) return;
    fill_rect(c, x, y, w, 1, color, 0.85f);
    fill_rect(c, x, y + h - 1, w, 1, color, 0.44f);
    fill_rect(c, x, y, 1, h, color, 0.48f);
    fill_rect(c, x + w - 1, y, 1, h, color, 0.30f);
}

static void draw_line(lab_canvas_t *c, int x0, int y0, int x1, int y1,
                      lab_rgb_t color, float alpha) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        put_pixel(c, x0, y0, color, alpha);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static void draw_circle(lab_canvas_t *c, int cx, int cy, int radius,
                        lab_rgb_t color, float alpha) {
    if (radius < 1) return;
    for (int y = -radius; y <= radius; y++)
        for (int x = -radius; x <= radius; x++)
            if (x * x + y * y <= radius * radius)
                put_pixel(c, cx + x, cy + y, color, alpha);
}

static void draw_text(lab_canvas_t *c, int x, int y, int scale, const char *text,
                      lab_rgb_t color, float alpha, int max_width) {
    if (!c || !text || scale < 1) return;
    int advance = 6 * scale;
    int used = 0;
    for (const char *p = text; *p; p++) {
        if (max_width > 0 && used + advance > max_width) break;
        const lab_glyph_t *glyph = glyph_for(*p);
        for (int row = 0; row < 7; row++)
            for (int col = 0; col < 5; col++)
                if (glyph->rows[row] & (1U << (4 - col)))
                    fill_rect(c, x + used + col * scale, y + row * scale,
                              scale, scale, color, alpha);
        used += advance;
    }
}

static void draw_meter(lab_canvas_t *c, int x, int y, int w, float percent,
                       lab_rgb_t color) {
    if (percent < 0.0f) percent = 0.0f;
    if (percent > 100.0f) percent = 100.0f;
    fill_rect(c, x, y, w, 5, LAB_BG, 0.94f);
    fill_rect(c, x, y, (int)((float)w * percent / 100.0f), 5, color, 0.86f);
}

static void draw_sparkline(lab_canvas_t *c, int x, int y, int w, int h, float phase) {
    if (w < 20 || h < 8) return;
    int last_x = x, last_y = y + h / 2;
    for (int i = 0; i < w; i += 3) {
        float t = (float)i / (float)(w > 1 ? w - 1 : 1);
        float v = 0.50f + 0.22f * sinf(t * 13.0f + phase * 6.283f) +
                  0.12f * sinf(t * 31.0f - phase * 10.0f);
        int next_y = y + h - 2 - (int)(v * (float)(h - 4));
        if (i > 0) draw_line(c, last_x, last_y, x + i, next_y, LAB_CYAN, 0.82f);
        last_x = x + i;
        last_y = next_y;
    }
}

static void draw_node(lab_canvas_t *c, int x, int y, int radius, lab_rgb_t color,
                      bool active, float phase) {
    draw_circle(c, x, y, radius + 5, color, active ? 0.10f + 0.05f * sinf(phase) : 0.04f);
    draw_circle(c, x, y, radius, color, active ? 0.92f : 0.56f);
    if (active) {
        int px = x + (int)(cosf(phase) * (float)(radius + 8));
        int py = y + (int)(sinf(phase) * (float)(radius + 8));
        draw_circle(c, px, py, 2, LAB_TEXT, 0.82f);
    }
}

static void draw_panel(lab_canvas_t *c, int x, int y, int w, int h,
                       const char *title, lab_rgb_t accent) {
    fill_rect(c, x, y, w, h, LAB_PANEL, 0.96f);
    stroke_rect(c, x, y, w, h, LAB_MUTED);
    fill_rect(c, x, y, 3, h, accent, 0.72f);
    draw_text(c, x + 14, y + 12, 1, title, LAB_TEXT, 0.88f, w - 28);
    draw_line(c, x + 14, y + 25, x + w - 14, y + 25, LAB_MUTED, 0.26f);
}

static void lab_render_overview_frame(lab_canvas_t *c, int frame, int frames) {
    float phase = (float)(frame % (frames > 0 ? frames : 1)) /
                  (float)(frames > 0 ? frames : 1);
    float angle = phase * 6.2831853f;
    for (int y = 0; y < c->height; y++) {
        float t = (float)y / (float)(c->height > 1 ? c->height - 1 : 1);
        lab_rgb_t row = {(uint8_t)(LAB_BG.r + 5.0f * (1.0f - t)),
                         (uint8_t)(LAB_BG.g + 7.0f * (1.0f - t)),
                         (uint8_t)(LAB_BG.b + 10.0f * (1.0f - t))};
        fill_rect(c, 0, y, c->width, 1, row, 1.0f);
    }

    int margin = c->width / 40;
    if (margin < 16) margin = 16;
    fill_rect(c, margin, 16, c->width - margin * 2, 42, LAB_PANEL, 0.96f);
    stroke_rect(c, margin, 16, c->width - margin * 2, 42, LAB_MUTED);
    draw_text(c, margin + 18, 28, 2, "DSCO KITTY LAB", LAB_TEXT, 0.96f, c->width / 2);
    draw_text(c, c->width - margin - 180, 31, 1, "NATIVE SCENE / LIVE",
              LAB_CYAN, 0.84f, 165);
    fill_rect(c, margin + 2, 55, c->width - margin * 2 - 4, 2, LAB_CYAN, 0.45f);

    int gap = 16;
    int top_y = 74;
    int left_w = (c->width * 58) / 100;
    int right_w = c->width - margin * 2 - left_w - gap;
    int body_h = (c->height * 58) / 100;
    int left_x = margin, right_x = left_x + left_w + gap;
    draw_panel(c, left_x, top_y, left_w, body_h, "LIVE OPERATIONS", LAB_AMBER);
    draw_panel(c, right_x, top_y, right_w, body_h, "RESOURCE ENVELOPE", LAB_CYAN);

    int card_x = left_x + 18, card_w = left_w - 36;
    int card_y = top_y + 42, card_h = 55;
    const char *tools[] = {"crawl_repo", "compile_patch", "run_smoke"};
    const lab_rgb_t colors[] = {LAB_AMBER, LAB_CYAN, LAB_GREEN};
    for (int i = 0; i < 3; i++) {
        int yy = card_y + i * (card_h + 8);
        fill_rect(c, card_x, yy, card_w, card_h, LAB_RAISED, 0.84f);
        stroke_rect(c, card_x, yy, card_w, card_h, LAB_MUTED);
        draw_circle(c, card_x + 14, yy + 16, 4, colors[i], i == 0 ? 0.95f : 0.62f);
        draw_text(c, card_x + 28, yy + 10, 1, "TOOL", LAB_MUTED, 0.76f, 36);
        draw_text(c, card_x + 70, yy + 10, 1, tools[i], colors[i], 0.94f, card_w - 160);
        draw_text(c, card_x + card_w - 84, yy + 10, 1, i == 0 ? "RUNNING" : "DONE",
                  colors[i], 0.76f, 72);
        draw_meter(c, card_x + 28, yy + 35, card_w - 52,
                   i == 0 ? 42.0f + 28.0f * (0.5f + 0.5f * sinf(angle)) : 100.0f,
                   colors[i]);
    }

    /* Agent topology: the moving dot is an event packet, not idle ambience. */
    int graph_y = top_y + body_h - 76;
    int root_x = left_x + left_w / 2;
    int child_a = left_x + left_w / 4, child_b = left_x + (left_w * 3) / 4;
    draw_line(c, root_x, graph_y - 22, child_a, graph_y + 24, LAB_MUTED, 0.45f);
    draw_line(c, root_x, graph_y - 22, child_b, graph_y + 24, LAB_MUTED, 0.45f);
    draw_node(c, root_x, graph_y - 22, 10, LAB_VIOLET, true, angle);
    draw_node(c, child_a, graph_y + 24, 7, LAB_CYAN, false, angle);
    draw_node(c, child_b, graph_y + 24, 7, LAB_AMBER, true, -angle);
    draw_text(c, root_x - 34, graph_y - 4, 1, "OVERMIND", LAB_TEXT, 0.70f, 70);
    draw_text(c, child_a - 32, graph_y + 38, 1, "SEARCH", LAB_MUTED, 0.66f, 64);
    draw_text(c, child_b - 26, graph_y + 38, 1, "BUILD", LAB_MUTED, 0.66f, 52);

    int rx = right_x + 18, rw = right_w - 36;
    draw_text(c, rx, top_y + 48, 1, "CONTEXT", LAB_MUTED, 0.78f, rw / 2);
    draw_text(c, rx + rw - 55, top_y + 48, 1, "68%", LAB_GREEN, 0.88f, 50);
    draw_meter(c, rx, top_y + 64, rw, 68.0f, LAB_GREEN);
    draw_text(c, rx, top_y + 88, 1, "COST", LAB_MUTED, 0.78f, rw / 2);
    draw_text(c, rx + rw - 72, top_y + 88, 1, "$0.0421", LAB_TEXT, 0.82f, 70);
    draw_text(c, rx, top_y + 128, 1, "QUEUE", LAB_MUTED, 0.78f, rw / 2);
    draw_text(c, rx + rw - 42, top_y + 128, 1, "3 / 8", LAB_AMBER, 0.88f, 40);
    draw_meter(c, rx, top_y + 144, rw, 37.5f, LAB_AMBER);
    draw_text(c, rx, top_y + 174, 1, "THROUGHPUT", LAB_MUTED, 0.78f, rw);
    draw_sparkline(c, rx, top_y + 194, rw, 50, angle);
    draw_text(c, rx, top_y + body_h - 30, 1, "EVENTS  128   ERRORS  0",
              LAB_CYAN, 0.76f, rw);

    int bottom_y = top_y + body_h + gap;
    int bottom_h = c->height - bottom_y - margin;
    int bottom_w = (c->width - margin * 2 - gap * 2) / 3;
    draw_panel(c, margin, bottom_y, bottom_w, bottom_h, "COMMAND PALETTE", LAB_VIOLET);
    draw_text(c, margin + 16, bottom_y + 40, 1, "> /review", LAB_TEXT, 0.92f, bottom_w - 32);
    draw_text(c, margin + 16, bottom_y + 58, 1, "  /plan   /tools", LAB_MUTED, 0.74f, bottom_w - 32);
    draw_text(c, margin + 16, bottom_y + 76, 1, "  /replay /quit", LAB_MUTED, 0.74f, bottom_w - 32);

    int permission_x = margin + bottom_w + gap;
    draw_panel(c, permission_x, bottom_y, bottom_w, bottom_h, "PERMISSION", LAB_AMBER);
    draw_text(c, permission_x + 16, bottom_y + 42, 1, "NET / EXEC", LAB_AMBER, 0.94f, bottom_w - 32);
    draw_text(c, permission_x + 16, bottom_y + 60, 1, "scope: github.com", LAB_TEXT, 0.76f, bottom_w - 32);
    draw_text(c, permission_x + 16, bottom_y + 78, 1, "[A] ALLOW   [D] DENY", LAB_MUTED, 0.72f, bottom_w - 32);

    int trace_x = permission_x + bottom_w + gap;
    draw_panel(c, trace_x, bottom_y, bottom_w, bottom_h, "TURN TRACE", LAB_GREEN);
    draw_text(c, trace_x + 16, bottom_y + 40, 1, "READY  THINK", LAB_MUTED, 0.78f, bottom_w - 32);
    draw_text(c, trace_x + 16, bottom_y + 58, 1, "TOOLS  REPLY", LAB_GREEN, 0.90f, bottom_w - 32);
    draw_meter(c, trace_x + 16, bottom_y + 78, bottom_w - 32,
               40.0f + 30.0f * (0.5f + 0.5f * sinf(angle)), LAB_GREEN);
}

static void lab_background(lab_canvas_t *c) {
    if (!c) return;
    for (int y = 0; y < c->height; y++) {
        float t = (float)y / (float)(c->height > 1 ? c->height - 1 : 1);
        lab_rgb_t row = {(uint8_t)(LAB_BG.r + 5.0f * (1.0f - t)),
                         (uint8_t)(LAB_BG.g + 7.0f * (1.0f - t)),
                         (uint8_t)(LAB_BG.b + 10.0f * (1.0f - t))};
        fill_rect(c, 0, y, c->width, 1, row, 1.0f);
    }
}

static int lab_margin(const lab_canvas_t *c) {
    int margin = c ? c->width / 40 : 16;
    return margin < 16 ? 16 : margin;
}

static void lab_draw_header(lab_canvas_t *c, const char *title,
                            const char *subtitle, lab_rgb_t accent) {
    if (!c) return;
    int margin = lab_margin(c);
    fill_rect(c, margin, 16, c->width - margin * 2, 42, LAB_PANEL, 0.96f);
    stroke_rect(c, margin, 16, c->width - margin * 2, 42, LAB_MUTED);
    draw_text(c, margin + 18, 28, 2, title ? title : "DSCO PLANNER",
              LAB_TEXT, 0.96f, c->width / 2);
    draw_text(c, c->width - margin - 230, 31, 1,
              subtitle ? subtitle : "NATIVE SCENE / LIVE", accent, 0.84f, 215);
    fill_rect(c, margin + 2, 55, c->width - margin * 2 - 4, 2, accent, 0.45f);
}

static void draw_plan_row(lab_canvas_t *c, int x, int y, int w, int depth,
                          const char *label, const char *kind, lab_rgb_t color,
                          float progress, bool active, bool blocked) {
    if (!c || w < 100) return;
    int indent = depth * 18;
    int row_x = x + indent;
    int row_w = w - indent;
    if (row_w < 80) row_w = 80;
    fill_rect(c, row_x, y, row_w, 23, LAB_RAISED, active ? 0.92f : 0.66f);
    if (blocked) fill_rect(c, row_x, y, 3, 23, LAB_AMBER, 0.88f);
    else fill_rect(c, row_x, y, 3, 23, color, active ? 0.90f : 0.62f);
    if (depth > 0) {
        draw_line(c, x + (depth - 1) * 18 + 7, y - 10, x + (depth - 1) * 18 + 7,
                  y + 11, LAB_MUTED, 0.34f);
        draw_line(c, x + (depth - 1) * 18 + 7, y + 11, row_x + 3, y + 11,
                  LAB_MUTED, 0.34f);
    }
    draw_circle(c, row_x + 13, y + 11, active ? 5 : 4, color,
                blocked ? 0.36f : (active ? 0.92f : 0.68f));
    draw_text(c, row_x + 25, y + 5, 1, label, LAB_TEXT, active ? 0.96f : 0.78f,
              row_w - 118);
    draw_text(c, row_x + row_w - 106, y + 5, 1, kind, color, 0.78f, 54);
    draw_meter(c, row_x + row_w - 48, y + 8, 40, progress, color);
}

static void draw_action_node(lab_canvas_t *c, int x, int y, int w, int h,
                             const char *id, const char *label, lab_rgb_t color,
                             bool ready, bool active) {
    if (!c || w < 36 || h < 18) return;
    fill_rect(c, x, y, w, h, LAB_RAISED, active ? 0.94f : 0.84f);
    stroke_rect(c, x, y, w, h, color);
    fill_rect(c, x, y, 3, h, color, active ? 0.95f : 0.68f);
    draw_text(c, x + 9, y + 5, 1, id, color, 0.95f, w - 18);
    if (label && h >= 31)
        draw_text(c, x + 9, y + 21, 1, label, LAB_TEXT, 0.72f, w - 18);
    if (ready) draw_circle(c, x + w - 8, y + 8, 3, LAB_GREEN, 0.92f);
}

static void lab_render_plan_frame(lab_canvas_t *c, int frame, int frames) {
    if (!c) return;
    float phase = (float)(frame % (frames > 0 ? frames : 1)) /
                  (float)(frames > 0 ? frames : 1);
    float angle = phase * 6.2831853f;
    lab_background(c);
    lab_draw_header(c, "PLAN / DECOMPOSITION", "GOAL > STEP > ACTION", LAB_VIOLET);

    int margin = lab_margin(c), gap = 14, top_y = 74;
    int body_h = (c->height * 59) / 100;
    if (body_h < 160) body_h = 160;
    int left_w = (c->width * 59) / 100;
    int right_w = c->width - margin * 2 - left_w - gap;
    int left_x = margin, right_x = left_x + left_w + gap;
    draw_panel(c, left_x, top_y, left_w, body_h, "HIERARCHY / WORK BREAKDOWN", LAB_VIOLET);
    draw_panel(c, right_x, top_y, right_w, body_h, "READINESS / DEPENDENCIES", LAB_CYAN);

    draw_text(c, left_x + 16, top_y + 39, 1, "GOAL", LAB_MUTED, 0.76f, 40);
    draw_text(c, left_x + 64, top_y + 39, 1, "SHIP SAFE RELEASE", LAB_TEXT, 0.92f, left_w - 84);
    draw_line(c, left_x + 16, top_y + 57, left_x + left_w - 16, top_y + 57,
              LAB_MUTED, 0.26f);
    bool compact = body_h < 250 || left_w < 430;
    int row_step = compact ? 23 : 27;
    int row_y = top_y + 67;
    draw_plan_row(c, left_x + 16, row_y, left_w - 32, 0,
                  "RELEASE FEATURE", "ROOT", LAB_VIOLET, 72.0f, true, false);
    row_y += row_step;
    draw_plan_row(c, left_x + 16, row_y, left_w - 32, 1,
                  "DISCOVERY", "STEP", LAB_CYAN, 100.0f, false, false);
    row_y += row_step;
    if (compact) {
        draw_plan_row(c, left_x + 16, row_y, left_w - 32, 1,
                      "IMPLEMENT", "STEP", LAB_AMBER, 46.0f, true, false);
        row_y += row_step;
        draw_plan_row(c, left_x + 16, row_y, left_w - 32, 2,
                      "build native view", "ACTION", LAB_AMBER,
                      48.0f + 20.0f * (0.5f + 0.5f * sinf(angle)), true, false);
        row_y += row_step;
        draw_plan_row(c, left_x + 16, row_y, left_w - 32, 1,
                      "VERIFY", "STEP", LAB_MUTED, 0.0f, false, false);
        row_y += row_step;
        draw_plan_row(c, left_x + 16, row_y, left_w - 32, 2,
                      "visual smoke", "ACTION", LAB_MUTED, 0.0f, false, false);
    } else {
        draw_plan_row(c, left_x + 16, row_y, left_w - 32, 2,
                      "inspect repo + constraints", "ACTION", LAB_GREEN, 100.0f, false, false);
        row_y += row_step;
        draw_plan_row(c, left_x + 16, row_y, left_w - 32, 2,
                      "confirm terminal capability", "ACTION", LAB_GREEN, 100.0f, false, false);
        row_y += row_step;
        draw_plan_row(c, left_x + 16, row_y, left_w - 32, 1,
                      "IMPLEMENT", "STEP", LAB_AMBER, 46.0f, true, false);
        row_y += row_step;
        draw_plan_row(c, left_x + 16, row_y, left_w - 32, 2,
                      "build native visualization", "ACTION", LAB_AMBER,
                      48.0f + 20.0f * (0.5f + 0.5f * sinf(angle)), true, false);
        row_y += row_step;
        draw_plan_row(c, left_x + 16, row_y, left_w - 32, 2,
                      "wire semantic event stream", "ACTION", LAB_AMBER, 18.0f, false, true);
        row_y += row_step;
        draw_plan_row(c, left_x + 16, row_y, left_w - 32, 1,
                      "VERIFY", "STEP", LAB_MUTED, 0.0f, false, false);
        row_y += row_step;
        draw_plan_row(c, left_x + 16, row_y, left_w - 32, 2,
                      "run visual + protocol smoke", "ACTION", LAB_MUTED, 0.0f, false, false);
    }

    int rx = right_x + 15, rw = right_w - 30;
    draw_text(c, rx, top_y + 42, 1, "READY FRONTIER", LAB_TEXT, 0.82f, rw);
    draw_text(c, rx, top_y + 60, 1, "A-02", LAB_GREEN, 0.92f, 34);
    draw_text(c, rx + 42, top_y + 60, 1, "inspect repo", LAB_MUTED, 0.72f, rw - 42);
    draw_text(c, rx, top_y + 78, 1, "A-04", LAB_GREEN, 0.92f, 34);
    draw_text(c, rx + 42, top_y + 78, 1, "build native view", LAB_MUTED, 0.72f, rw - 42);
    draw_text(c, rx, top_y + 96, 1, "A-07", LAB_AMBER, 0.92f, 34);
    draw_text(c, rx + 42, top_y + 96, 1, "awaits A-06", LAB_AMBER, 0.72f, rw - 42);

    int gx = rx + 8;
    int graph_gap = compact ? 52 : 78;
    int gy = top_y + (compact ? 104 : 122);
    int node_w = compact ? 40 : (rw > 190 ? 58 : 48);
    int node_h = compact ? 28 : 32;
    int a1x = gx, a1y = gy + (compact ? 21 : 30);
    int a2x = gx + graph_gap, a2y = gy;
    int a3x = gx + graph_gap, a3y = gy + (compact ? 42 : 60);
    int a4x = gx + graph_gap * 2, a4y = a1y;
    draw_line(c, a1x + node_w, a1y + node_h / 2, a2x, a2y + node_h / 2,
              LAB_MUTED, 0.46f);
    draw_line(c, a1x + node_w, a1y + node_h / 2, a3x, a3y + node_h / 2,
              LAB_MUTED, 0.46f);
    draw_line(c, a2x + node_w, a2y + node_h / 2, a4x, a4y + node_h / 2,
              LAB_CYAN, 0.58f);
    draw_line(c, a3x + node_w, a3y + node_h / 2, a4x, a4y + node_h / 2,
              LAB_AMBER, 0.58f);
    draw_action_node(c, a1x, a1y, node_w, node_h, "A1", "scan", LAB_GREEN, true, false);
    draw_action_node(c, a2x, a2y, node_w, node_h, "A2", "shape", LAB_CYAN, true, true);
    draw_action_node(c, a3x, a3y, node_w, node_h, "A3", "gate", LAB_AMBER, false, false);
    draw_action_node(c, a4x, a4y, node_w, node_h, "A4", "ship", LAB_MUTED, false, false);
    int packet_x = a1x + node_w + (int)(phase * (float)(graph_gap - node_w));
    draw_circle(c, packet_x, a1y + node_h / 2, 2, LAB_TEXT, 0.90f);

    int metrics_y = top_y + body_h - (compact ? 34 : 57);
    draw_text(c, rx, metrics_y, 1, "CRITICAL PATH  3 / 7", LAB_CYAN, 0.78f, rw);
    draw_text(c, rx, metrics_y + 17, 1, "BLOCKED  1    PARALLEL  3X", LAB_AMBER, 0.78f, rw);
    if (!compact) draw_meter(c, rx, metrics_y + 36, rw, 43.0f, LAB_CYAN);

    int bottom_y = top_y + body_h + gap;
    int bottom_h = c->height - bottom_y - margin;
    if (bottom_h < 38) bottom_h = 38;
    draw_panel(c, margin, bottom_y, c->width - margin * 2, bottom_h,
               "NEXT ACTION / DECISION LOG", LAB_AMBER);
    draw_text(c, margin + 16, bottom_y + 40, 1,
              "A-06  reconcile dependency gate before verification", LAB_TEXT, 0.90f,
              c->width - margin * 2 - 32);
    if (bottom_h >= 72)
        draw_text(c, margin + 16, bottom_y + 58, 1,
                  "WHY: A-07 is blocked by the gate; no hidden work is scheduled.",
                  LAB_MUTED, 0.72f, c->width - margin * 2 - 32);
}

static void lab_render_actions_frame(lab_canvas_t *c, int frame, int frames) {
    if (!c) return;
    float phase = (float)(frame % (frames > 0 ? frames : 1)) /
                  (float)(frames > 0 ? frames : 1);
    float angle = phase * 6.2831853f;
    lab_background(c);
    lab_draw_header(c, "ACTION PLANNER", "DAG / FRONTIER / TIMELINE", LAB_CYAN);

    int margin = lab_margin(c), gap = 14, top_y = 74;
    int top_h = (c->height * 51) / 100;
    if (top_h < 150) top_h = 150;
    int left_w = (c->width * 58) / 100;
    int right_w = c->width - margin * 2 - left_w - gap;
    bool compact = top_h < 220 || left_w < 430;
    int left_x = margin, right_x = left_x + left_w + gap;
    draw_panel(c, left_x, top_y, left_w, top_h, "ACTION DAG / DEPENDENCY FLOW", LAB_CYAN);
    draw_panel(c, right_x, top_y, right_w, top_h, "ACTION QUEUE / POLICY", LAB_AMBER);

    int gx = left_x + (compact ? 12 : 22), gy = top_y + (compact ? 40 : 52);
    int nw = compact ? 58 : (left_w > 400 ? 88 : 68);
    int nh = compact ? 28 : 38;
    int x_gap = compact ? 76 : 120;
    int y_gap = compact ? 62 : 96;
    int n1x = gx, n1y = gy + (compact ? 32 : 48);
    int n2x = gx + x_gap, n2y = gy;
    int n3x = gx + x_gap, n3y = gy + y_gap;
    int n4x = gx + x_gap * 2, n4y = n1y;
    draw_line(c, n1x + nw, n1y + nh / 2, n2x, n2y + nh / 2, LAB_MUTED, 0.50f);
    draw_line(c, n1x + nw, n1y + nh / 2, n3x, n3y + nh / 2, LAB_MUTED, 0.50f);
    draw_line(c, n2x + nw, n2y + nh / 2, n4x, n4y + nh / 2, LAB_CYAN, 0.64f);
    draw_line(c, n3x + nw, n3y + nh / 2, n4x, n4y + nh / 2, LAB_AMBER, 0.64f);
    draw_action_node(c, n1x, n1y, nw, nh, "A-01", "discover", LAB_GREEN, true, false);
    draw_action_node(c, n2x, n2y, nw, nh, "A-02", "compose", LAB_CYAN, true, true);
    draw_action_node(c, n3x, n3y, nw, nh, "A-03", "authorize", LAB_AMBER, false, false);
    draw_action_node(c, n4x, n4y, nw, nh, "A-04", "verify", LAB_MUTED, false, false);
    int packet_x = n1x + nw + (int)(phase * 114.0f);
    int packet_y = n1y + nh / 2 - (int)(sinf(angle) * 12.0f);
    draw_circle(c, packet_x, packet_y, 3, LAB_TEXT, 0.94f);
    draw_text(c, left_x + 22, top_y + top_h - 32, 1,
              "SOLID = dependency   GLOW = active   DOT = ready frontier",
              LAB_MUTED, 0.70f, left_w - 44);

    int qx = right_x + 16, qw = right_w - 32;
    draw_text(c, qx, top_y + 43, 1, "READY NOW", LAB_GREEN, 0.86f, qw);
    const char *ids[] = {"A-02", "A-05", "A-08"};
    const char *labels[] = {"compose native scene", "write protocol test", "emit artifact"};
    const lab_rgb_t colors[] = {LAB_CYAN, LAB_GREEN, LAB_VIOLET};
    for (int i = 0; i < 3; i++) {
        int yy = top_y + 61 + i * 30;
        fill_rect(c, qx, yy, qw, 24, LAB_RAISED, 0.78f);
        draw_circle(c, qx + 10, yy + 12, 4, colors[i], i == 0 ? 0.94f : 0.72f);
        draw_text(c, qx + 21, yy + 5, 1, ids[i], colors[i], 0.90f, 34);
        draw_text(c, qx + 60, yy + 5, 1, labels[i], LAB_TEXT, 0.74f, qw - 68);
    }
    int policy_y = top_y + (compact ? 142 : 163);
    draw_text(c, qx, policy_y, 1, "POLICY CHECK", LAB_AMBER, 0.86f, qw);
    draw_text(c, qx, policy_y + 19, 1, "NET / EXEC  ·  scope: workspace", LAB_TEXT, 0.76f, qw);
    draw_text(c, qx, policy_y + 38, 1, "ALLOW  [A]     DENY  [D]", LAB_MUTED, 0.72f, qw);
    if (!compact)
        draw_meter(c, qx, top_y + top_h - 33, qw,
                   62.0f + 12.0f * sinf(angle), LAB_AMBER);

    int bottom_y = top_y + top_h + gap;
    int bottom_h = c->height - bottom_y - margin;
    if (bottom_h < 54) bottom_h = 54;
    draw_panel(c, margin, bottom_y, c->width - margin * 2, bottom_h,
               "EXECUTION TIMELINE / LANES", LAB_GREEN);
    const char *lanes[] = {"DISCOVER", "BUILD", "VERIFY"};
    const lab_rgb_t lane_colors[] = {LAB_GREEN, LAB_CYAN, LAB_AMBER};
    int tx = margin + 92, tw = c->width - margin * 2 - 112;
    int lane_y = bottom_y + (compact ? 31 : 39);
    int lane_step = compact ? 12 : 18;
    for (int i = 0; i < 3; i++) {
        int yy = lane_y + i * lane_step;
        draw_text(c, margin + 16, yy, 1, lanes[i], LAB_MUTED, 0.72f, 72);
        fill_rect(c, tx, yy + 3, tw, 6, LAB_BG, 0.88f);
        int start = i == 0 ? 0 : (i == 1 ? tw / 4 : tw * 2 / 3);
        int length = i == 0 ? tw / 4 : (i == 1 ? tw * 5 / 12 : tw / 4);
        fill_rect(c, tx + start, yy + 3, length, 6, lane_colors[i], 0.76f);
        if (i == 1) fill_rect(c, tx + start, yy + 3, (int)(length * 0.45f), 6, LAB_TEXT, 0.26f);
    }
    int cursor_x = tx + (int)((float)tw * phase);
    fill_rect(c, cursor_x, bottom_y + 31, 2, bottom_h - 38, LAB_TEXT, 0.72f);
    if (!compact)
        draw_text(c, margin + 16, bottom_y + bottom_h - 16,
                  1, "ETA 00:42   ·   3 parallel lanes   ·   1 gated action",
                  LAB_CYAN, 0.76f, c->width - margin * 2 - 32);
}

static void lab_render_scene(lab_canvas_t *c, int frame, int frames, kitty_lab_view_t view) {
    switch (view) {
    case KITTY_LAB_VIEW_PLAN:
        lab_render_plan_frame(c, frame, frames);
        break;
    case KITTY_LAB_VIEW_ACTIONS:
        lab_render_actions_frame(c, frame, frames);
        break;
    case KITTY_LAB_VIEW_OVERVIEW:
    default:
        lab_render_overview_frame(c, frame, frames);
        break;
    }
}

bool kitty_lab_write_ppm(const char *path, int width, int height, int frame, int frames) {
    return kitty_lab_write_ppm_view(path, width, height, frame, frames,
                                    KITTY_LAB_VIEW_OVERVIEW);
}

bool kitty_lab_write_ppm_view(const char *path, int width, int height, int frame,
                              int frames, kitty_lab_view_t view) {
    if (!path || !*path) return false;
    lab_canvas_t *canvas = canvas_new(width, height);
    if (!canvas) return false;
    lab_render_scene(canvas, frame, frames, view);
    FILE *file = fopen(path, "wb");
    if (!file) { canvas_free(canvas); return false; }
    bool ok = fprintf(file, "P6\n%d %d\n255\n", width, height) > 0 &&
              fwrite(canvas->pixels, 1, (size_t)width * (size_t)height * 3U, file) ==
              (size_t)width * (size_t)height * 3U;
    if (fclose(file) != 0) ok = false;
    canvas_free(canvas);
    return ok;
}

bool kitty_lab_render(FILE *out, int width, int height, int frames, bool animate) {
    return kitty_lab_render_view(out, width, height, frames, animate,
                                 KITTY_LAB_VIEW_OVERVIEW);
}

bool kitty_lab_render_view(FILE *out, int width, int height, int frames, bool animate,
                           kitty_lab_view_t view) {
    if (!out || !kitty_graphics_available(out)) return false;
    if (frames < 1) frames = 1;
    if (frames > 24) frames = 24;
    lab_canvas_t *canvas = canvas_new(width, height);
    if (!canvas) return false;

    uint32_t image_id = 0x4C414200U ^ ((uint32_t)getpid() << 4) ^ (uint32_t)width;
    if (!image_id) image_id = 1;
    char control[256];
    bool ok = true;
    int upload_frames = animate ? frames : 1;
    for (int frame = 0; frame < upload_frames && ok; frame++) {
        lab_render_scene(canvas, frame, frames, view);
        if (frame == 0) {
            snprintf(control, sizeof(control),
                     "a=t,t=d,f=24,s=%d,v=%d,i=%u,q=2,o=z",
                     width, height, image_id);
        } else {
            snprintf(control, sizeof(control),
                     "a=f,i=%u,f=24,s=%d,v=%d,z=90,X=1,q=2,o=z",
                     image_id, width, height);
        }
        kitty_graphics_send_options_t options;
        kitty_graphics_send_options_default(&options);
        options.continuation_control = frame == 0 ? "q=2" : "a=f,q=2";
        ok = kitty_graphics_send_pixels(out, control, canvas->pixels,
                                        (size_t)width * (size_t)height * 3U,
                                        &options);
    }
    if (ok && animate && upload_frames > 1) {
        fprintf(out, "\033_Ga=a,i=%u,r=1,z=90,q=2\033\\", image_id);
        fprintf(out, "\033_Ga=a,i=%u,s=3,v=1,q=2\033\\", image_id);
    }
    if (ok) {
        struct winsize ws;
        memset(&ws, 0, sizeof(ws));
        int cols = 96, rows = 30;
        if (ioctl(fileno(out), TIOCGWINSZ, &ws) == 0) {
            if (ws.ws_col > 2) cols = ws.ws_col - 2;
            if (ws.ws_row > 3) rows = ws.ws_row - 2;
        }
        fprintf(out, "\033_Ga=p,i=%u,p=1,c=%d,r=%d,C=1,z=1,q=2\033\\\n",
                image_id, cols, rows);
        fflush(out);
        ok = !ferror(out);
    }
    canvas_free(canvas);
    return ok;
}
