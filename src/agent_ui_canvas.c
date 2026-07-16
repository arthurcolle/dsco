#define _POSIX_C_SOURCE 200809L

#include "agent_ui_canvas.h"

#include "font_compat.h"
#include "px_backend.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AGENT_UI_CANVAS_MAX_PIXELS UINT64_C(40000000)
#define AGENT_UI_CLIP_DEPTH 16

typedef struct { int x, y, width, height; } device_rect_t;

struct agent_ui_canvas {
    int width;
    int height;
    int logical_width;
    int logical_height;
    int backing_scale;
    uint8_t *pixels;
    const agent_ui_theme_t *theme;
    device_rect_t clips[AGENT_UI_CLIP_DEPTH];
    int clip_count;
    px_backend_t backend;
};

typedef struct { char ch; uint8_t rows[7]; } fallback_glyph_t;
#define GLYPH(ch, a,b,c,d,e,f,g) {ch,{a,b,c,d,e,f,g}}
static const fallback_glyph_t FALLBACK_GLYPHS[] = {
    GLYPH(' ',0,0,0,0,0,0,0), GLYPH('A',14,17,17,31,17,17,17),
    GLYPH('B',30,17,17,30,17,17,30), GLYPH('C',14,17,16,16,16,17,14),
    GLYPH('D',30,17,17,17,17,17,30), GLYPH('E',31,16,16,30,16,16,31),
    GLYPH('F',31,16,16,30,16,16,16), GLYPH('G',14,17,16,23,17,17,14),
    GLYPH('H',17,17,17,31,17,17,17), GLYPH('I',31,4,4,4,4,4,31),
    GLYPH('J',7,2,2,2,2,18,12), GLYPH('K',17,18,20,24,20,18,17),
    GLYPH('L',16,16,16,16,16,16,31), GLYPH('M',17,27,21,21,17,17,17),
    GLYPH('N',17,25,21,19,17,17,17), GLYPH('O',14,17,17,17,17,17,14),
    GLYPH('P',30,17,17,30,16,16,16), GLYPH('Q',14,17,17,17,21,18,13),
    GLYPH('R',30,17,17,30,20,18,17), GLYPH('S',15,16,16,14,1,1,30),
    GLYPH('T',31,4,4,4,4,4,4), GLYPH('U',17,17,17,17,17,17,14),
    GLYPH('V',17,17,17,17,17,10,4), GLYPH('W',17,17,17,21,21,27,17),
    GLYPH('X',17,17,10,4,10,17,17), GLYPH('Y',17,17,10,4,4,4,4),
    GLYPH('Z',31,1,2,4,8,16,31), GLYPH('0',14,17,19,21,25,17,14),
    GLYPH('1',4,12,4,4,4,4,14), GLYPH('2',14,17,1,2,4,8,31),
    GLYPH('3',30,1,1,14,1,1,30), GLYPH('4',2,6,10,18,31,2,2),
    GLYPH('5',31,16,16,30,1,1,30), GLYPH('6',14,16,16,30,17,17,14),
    GLYPH('7',31,1,2,4,8,8,8), GLYPH('8',14,17,17,14,17,17,14),
    GLYPH('9',14,17,17,15,1,1,14), GLYPH('-',0,0,0,31,0,0,0),
    GLYPH('_',0,0,0,0,0,0,31), GLYPH(':',0,4,0,0,0,4,0),
    GLYPH('.',0,0,0,0,0,4,4), GLYPH('/',1,2,2,4,8,8,16),
    GLYPH('%',25,26,4,8,11,19,0), GLYPH('>',16,8,4,2,4,8,16),
    GLYPH('<',1,2,4,8,4,2,1), GLYPH('+',0,4,4,31,4,4,0),
    GLYPH('=',0,31,0,31,0,0,0), GLYPH('?',14,17,1,2,4,0,4),
    GLYPH('!',4,4,4,4,4,0,4), GLYPH('#',10,31,10,10,31,10,0),
    GLYPH('[',14,8,8,8,8,8,14), GLYPH(']',14,2,2,2,2,2,14),
    GLYPH('(',6,8,16,16,16,8,6), GLYPH(')',12,2,1,1,1,2,12),
    GLYPH('|',4,4,4,4,4,4,4), GLYPH(',',0,0,0,0,0,4,8),
};
#undef GLYPH

static int clamp_int(int value, int low, int high) {
    if (value < low) return low;
    if (value > high) return high;
    return value;
}

static device_rect_t rect_intersection(device_rect_t a, device_rect_t b) {
    int left = a.x > b.x ? a.x : b.x;
    int top = a.y > b.y ? a.y : b.y;
    int ar = a.x + a.width, br = b.x + b.width;
    int ab = a.y + a.height, bb = b.y + b.height;
    int right = ar < br ? ar : br;
    int bottom = ab < bb ? ab : bb;
    if (right < left) right = left;
    if (bottom < top) bottom = top;
    return (device_rect_t){left, top, right - left, bottom - top};
}

static device_rect_t canvas_clip(const agent_ui_canvas_t *canvas) {
    if (!canvas) return (device_rect_t){0,0,0,0};
    if (canvas->clip_count > 0) return canvas->clips[canvas->clip_count - 1];
    return (device_rect_t){0, 0, canvas->width, canvas->height};
}

static device_rect_t device_rect(const agent_ui_canvas_t *canvas,
                                 native_ui_rect_t logical) {
    int scale = canvas ? canvas->backing_scale : 1;
    return (device_rect_t){logical.x * scale, logical.y * scale,
                           logical.width * scale, logical.height * scale};
}

static void blend_pixel(agent_ui_canvas_t *canvas, int x, int y,
                        px_backend_color_t color, double alpha) {
    if (!canvas || !canvas->pixels || alpha <= 0.0 || x < 0 || y < 0 ||
        x >= canvas->width || y >= canvas->height) return;
    device_rect_t clip = canvas_clip(canvas);
    if (x < clip.x || y < clip.y || x >= clip.x + clip.width ||
        y >= clip.y + clip.height) return;
    if (alpha > 1.0) alpha = 1.0;
    uint8_t *pixel = canvas->pixels +
        ((size_t)y * (size_t)canvas->width + (size_t)x) * 3U;
    pixel[0] = (uint8_t)((double)pixel[0] + ((double)color.r - pixel[0]) * alpha);
    pixel[1] = (uint8_t)((double)pixel[1] + ((double)color.g - pixel[1]) * alpha);
    pixel[2] = (uint8_t)((double)pixel[2] + ((double)color.b - pixel[2]) * alpha);
}

static void fill_device_rect(agent_ui_canvas_t *canvas, device_rect_t rect,
                             px_backend_color_t color, double alpha) {
    if (!canvas || rect.width <= 0 || rect.height <= 0 || alpha <= 0.0) return;
    rect = rect_intersection(rect, canvas_clip(canvas));
    rect = rect_intersection(rect, (device_rect_t){0,0,canvas->width,canvas->height});
    if (rect.width <= 0 || rect.height <= 0) return;
    if (alpha >= 0.999) {
        for (int y = rect.y; y < rect.y + rect.height; y++) {
            uint8_t *pixel = canvas->pixels +
                ((size_t)y * (size_t)canvas->width + (size_t)rect.x) * 3U;
            for (int x = 0; x < rect.width; x++, pixel += 3) {
                pixel[0] = color.r; pixel[1] = color.g; pixel[2] = color.b;
            }
        }
        return;
    }
    for (int y = rect.y; y < rect.y + rect.height; y++)
        for (int x = rect.x; x < rect.x + rect.width; x++)
            blend_pixel(canvas, x, y, color, alpha);
}

static bool inside_round(int x, int y, device_rect_t rect, int radius) {
    if (x < rect.x || y < rect.y || x >= rect.x + rect.width ||
        y >= rect.y + rect.height) return false;
    if (radius <= 0) return true;
    if (radius * 2 > rect.width) radius = rect.width / 2;
    if (radius * 2 > rect.height) radius = rect.height / 2;
    int left = rect.x + radius, right = rect.x + rect.width - radius - 1;
    int top = rect.y + radius, bottom = rect.y + rect.height - radius - 1;
    int cx = x < left ? left : (x > right ? right : x);
    int cy = y < top ? top : (y > bottom ? bottom : y);
    int dx = x - cx, dy = y - cy;
    return dx * dx + dy * dy <= radius * radius;
}

static void fill_round(agent_ui_canvas_t *canvas, native_ui_rect_t logical,
                       int logical_radius, px_backend_color_t color, double alpha) {
    device_rect_t rect = device_rect(canvas, logical);
    int radius = logical_radius * canvas->backing_scale;
    if (radius <= 0) { fill_device_rect(canvas, rect, color, alpha); return; }
    device_rect_t bounds = rect_intersection(rect, canvas_clip(canvas));
    bounds = rect_intersection(bounds, (device_rect_t){0,0,canvas->width,canvas->height});
    for (int y = bounds.y; y < bounds.y + bounds.height; y++)
        for (int x = bounds.x; x < bounds.x + bounds.width; x++)
            if (inside_round(x, y, rect, radius)) blend_pixel(canvas, x, y, color, alpha);
}

static void stroke_round(agent_ui_canvas_t *canvas, native_ui_rect_t logical,
                         int logical_radius, int logical_width,
                         px_backend_color_t color, double alpha) {
    device_rect_t outer = device_rect(canvas, logical);
    int width = logical_width * canvas->backing_scale;
    int radius = logical_radius * canvas->backing_scale;
    if (width < 1) width = 1;
    device_rect_t inner = {outer.x + width, outer.y + width,
                           outer.width - width * 2, outer.height - width * 2};
    int inner_radius = radius > width ? radius - width : 0;
    device_rect_t bounds = rect_intersection(outer, canvas_clip(canvas));
    bounds = rect_intersection(bounds, (device_rect_t){0,0,canvas->width,canvas->height});
    for (int y = bounds.y; y < bounds.y + bounds.height; y++)
        for (int x = bounds.x; x < bounds.x + bounds.width; x++)
            if (inside_round(x, y, outer, radius) &&
                !inside_round(x, y, inner, inner_radius))
                blend_pixel(canvas, x, y, color, alpha);
}

static void fill_circle(agent_ui_canvas_t *canvas, int logical_cx, int logical_cy,
                        int logical_radius, px_backend_color_t color, double alpha) {
    int scale = canvas->backing_scale;
    int cx = logical_cx * scale, cy = logical_cy * scale;
    int radius = logical_radius * scale;
    int rr = radius * radius;
    for (int y = -radius; y <= radius; y++)
        for (int x = -radius; x <= radius; x++)
            if (x*x + y*y <= rr) blend_pixel(canvas, cx+x, cy+y, color, alpha);
}

static void draw_line(agent_ui_canvas_t *canvas, int lx0, int ly0, int lx1, int ly1,
                      px_backend_color_t color, double alpha) {
    int scale = canvas->backing_scale;
    int x0 = lx0 * scale, y0 = ly0 * scale;
    int x1 = lx1 * scale, y1 = ly1 * scale;
    int dx = abs(x1-x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1-y0), sy = y0 < y1 ? 1 : -1;
    int error = dx + dy;
    for (;;) {
        for (int oy = 0; oy < scale; oy++)
            for (int ox = 0; ox < scale; ox++)
                blend_pixel(canvas, x0+ox, y0+oy, color, alpha);
        if (x0 == x1 && y0 == y1) break;
        int twice = error * 2;
        if (twice >= dy) { error += dy; x0 += sx; }
        if (twice <= dx) { error += dx; y0 += sy; }
    }
}

static const fallback_glyph_t *fallback_glyph(char ch) {
    if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
    for (size_t i = 0; i < sizeof(FALLBACK_GLYPHS)/sizeof(FALLBACK_GLYPHS[0]); i++)
        if (FALLBACK_GLYPHS[i].ch == ch) return &FALLBACK_GLYPHS[i];
    return &FALLBACK_GLYPHS[0];
}

static void draw_fallback_text(agent_ui_canvas_t *canvas, int x, int y,
                               int max_width, const char *text,
                               int pixel_scale, px_backend_color_t color,
                               double alpha) {
    if (!text || pixel_scale < 1) return;
    int advance = 6 * pixel_scale;
    int used = 0;
    for (const char *p = text; *p && used + advance <= max_width; p++) {
        const fallback_glyph_t *glyph = fallback_glyph(*p);
        for (int row = 0; row < 7; row++)
            for (int col = 0; col < 5; col++)
                if (glyph->rows[row] & (1U << (4-col)))
                    fill_device_rect(canvas,
                        (device_rect_t){x+used+col*pixel_scale, y+row*pixel_scale,
                                        pixel_scale, pixel_scale}, color, alpha);
        used += advance;
    }
}

static float type_size(const agent_ui_theme_t *theme, native_ui_type_token_t type) {
    if (!theme) return 13.0f;
    switch (type) {
        case NATIVE_UI_TYPE_LABEL: return theme->type.label;
        case NATIVE_UI_TYPE_TITLE: return theme->type.title;
        case NATIVE_UI_TYPE_CODE: return theme->type.code;
        case NATIVE_UI_TYPE_MATH: return theme->type.body;
        case NATIVE_UI_TYPE_METRIC: return theme->type.metric;
        case NATIVE_UI_TYPE_BODY: default: return theme->type.body;
    }
}

static void draw_text_direct(agent_ui_canvas_t *canvas, native_ui_rect_t rect,
                             const char *text, native_ui_type_token_t type,
                             px_backend_color_t color, double alpha,
                             const agent_ui_theme_t *theme) {
    if (!canvas || !text || !*text || rect.width < 2 || rect.height < 2) return;
    int scale = canvas->backing_scale;
    float logical_size = type_size(theme, type);
    float physical_size = logical_size * (float)scale;
    bool bold = type == NATIVE_UI_TYPE_TITLE || type == NATIVE_UI_TYPE_METRIC;
    int line_height = font_compat_line_height(physical_size, bold);
    if (line_height < 1) line_height = (int)(logical_size * scale);
    int x = rect.x * scale;
    int y = rect.y * scale + (rect.height * scale - line_height) / 2;
    int max_width = rect.width * scale;
    int advance = font_compat_draw_rgb(
        canvas->pixels, canvas->width, canvas->height, canvas->width * 3,
        x, y, max_width, text, physical_size, bold,
        color.r, color.g, color.b, (float)alpha);
    if (advance < 0) {
        int bitmap_scale = clamp_int((int)(logical_size / 8.0f) * scale, 1, 6);
        int fallback_h = 7 * bitmap_scale;
        draw_fallback_text(canvas, x, rect.y * scale +
                           (rect.height * scale - fallback_h) / 2,
                           max_width, text, bitmap_scale, color, alpha);
    }
}

static void draw_theme_swatch(agent_ui_canvas_t *canvas,
                              const native_ui_node_t *node) {
    const char *prefix = "theme-swatch:";
    const char *id = node->text + strlen(prefix);
    char theme_id[64];
    size_t count = strcspn(id, ":");
    if (count >= sizeof(theme_id)) count = sizeof(theme_id)-1;
    memcpy(theme_id, id, count); theme_id[count] = '\0';
    const agent_ui_theme_t *theme = agent_ui_theme_find(theme_id);
    if (!theme) return;
    bool selected = strstr(id + count, ":1") != NULL ||
                    (node->state & NATIVE_UI_STATE_SELECTED);
    native_ui_rect_t r = node->frame;
    fill_round(canvas, r, theme->radius.md,
               theme->palette.colors[NATIVE_UI_COLOR_SURFACE], 1.0);
    stroke_round(canvas, r, theme->radius.md, selected ? 2 : 1,
                 theme->palette.colors[selected ? NATIVE_UI_COLOR_FOCUS :
                                               NATIVE_UI_COLOR_BORDER], 0.92);
    int pad = theme->spacing.md;
    native_ui_rect_t title = {r.x+pad, r.y+pad, r.width-pad*2, 22};
    draw_text_direct(canvas, title, theme->name, NATIVE_UI_TYPE_TITLE,
                     theme->palette.colors[NATIVE_UI_COLOR_TEXT], 1.0, theme);
    native_ui_rect_t desc = {r.x+pad, r.y+pad+23, r.width-pad*2, 18};
    draw_text_direct(canvas, desc, theme->description, NATIVE_UI_TYPE_LABEL,
                     theme->palette.colors[NATIVE_UI_COLOR_TEXT_MUTED], 0.95, theme);
    native_ui_color_token_t tokens[] = {
        NATIVE_UI_COLOR_CANVAS, NATIVE_UI_COLOR_SURFACE_RAISED,
        NATIVE_UI_COLOR_ACCENT, NATIVE_UI_COLOR_SUCCESS,
        NATIVE_UI_COLOR_WARNING, NATIVE_UI_COLOR_DANGER,
    };
    int swatch_y = r.y + r.height - pad - 22;
    int gap = 4;
    int width = (r.width - pad*2 - gap*5) / 6;
    for (int i = 0; i < 6; i++)
        fill_round(canvas, (native_ui_rect_t){r.x+pad+i*(width+gap),swatch_y,width,22},
                   4, theme->palette.colors[tokens[i]], 1.0);
}

static void draw_activity(agent_ui_canvas_t *canvas,
                          const native_ui_node_t *node,
                          const px_backend_palette_t *palette) {
    native_ui_rect_t r = node->frame;
    int cy = r.y + r.height/2;
    for (int i = 0; i < 3; i++)
        fill_circle(canvas, r.x + 6 + i*9, cy, i == 1 ? 3 : 2,
                    palette->colors[NATIVE_UI_COLOR_ACCENT],
                    i == 1 ? 0.95 : 0.48);
}

static void draw_topology(agent_ui_canvas_t *canvas,
                          const native_ui_node_t *node,
                          const px_backend_palette_t *palette) {
    int agents = 4, active = 3;
    char route[64] = "direct";
    (void)sscanf(node->text, "topology:%d:%d:%63s", &agents, &active, route);
    agents = clamp_int(agents, 2, 8);
    active = clamp_int(active, 0, agents);
    native_ui_rect_t r = node->frame;
    int cx = r.x + r.width/2, cy = r.y + r.height/2;
    int radius = (r.width < r.height ? r.width : r.height)/2 - 16;
    if (radius < 18) radius = 18;
    for (int i = 0; i < agents; i++) {
        double angle = -1.57079632679 + 6.28318530718 * (double)i / (double)agents;
        int x = cx + (int)(cos(angle)*radius);
        int y = cy + (int)(sin(angle)*radius);
        draw_line(canvas, cx, cy, x, y,
                  palette->colors[NATIVE_UI_COLOR_BORDER], 0.58);
        fill_circle(canvas, x, y, 5,
                    palette->colors[i < active ? NATIVE_UI_COLOR_SUCCESS :
                                               NATIVE_UI_COLOR_TEXT_MUTED],
                    i < active ? 0.95 : 0.55);
    }
    fill_circle(canvas, cx, cy, 8, palette->colors[NATIVE_UI_COLOR_ACCENT], 0.95);
    native_ui_rect_t label = {r.x+8, r.y+r.height-18, r.width-16, 14};
    draw_text_direct(canvas, label, route, NATIVE_UI_TYPE_LABEL,
                     palette->colors[NATIVE_UI_COLOR_TEXT_MUTED], 0.85,
                     canvas->theme);
}

static void backend_push_clip(void *surface, native_ui_rect_t rect) {
    agent_ui_canvas_t *canvas = surface;
    if (!canvas || canvas->clip_count >= AGENT_UI_CLIP_DEPTH) return;
    device_rect_t clip = device_rect(canvas, rect);
    clip = rect_intersection(clip, canvas_clip(canvas));
    canvas->clips[canvas->clip_count++] = clip;
}

static void backend_pop_clip(void *surface) {
    agent_ui_canvas_t *canvas = surface;
    if (canvas && canvas->clip_count > 0) canvas->clip_count--;
}

static void backend_fill(void *surface, native_ui_rect_t rect,
                         px_backend_color_t color, uint8_t opacity,
                         uint8_t radius, bool raised) {
    agent_ui_canvas_t *canvas = surface;
    if (!canvas) return;
    double alpha = (double)opacity / 255.0;
    if (raised && canvas->theme->shadow_opacity > 0.0f) {
        native_ui_rect_t shadow = rect;
        shadow.y += 3;
        fill_round(canvas, shadow, radius+2, (px_backend_color_t){0,0,0},
                   canvas->theme->shadow_opacity * alpha);
    }
    fill_round(canvas, rect, radius, color, alpha);
}

static void backend_stroke(void *surface, native_ui_rect_t rect,
                           px_backend_color_t color, uint8_t opacity,
                           uint8_t width, uint8_t radius) {
    agent_ui_canvas_t *canvas = surface;
    if (!canvas) return;
    stroke_round(canvas, rect, radius, width > 0 ? width : 1, color,
                 (double)opacity / 255.0);
}

static void backend_text(void *surface, native_ui_rect_t rect, const char *text,
                         native_ui_type_token_t type, px_backend_color_t color,
                         uint8_t opacity) {
    agent_ui_canvas_t *canvas = surface;
    draw_text_direct(canvas, rect, text, type, color,
                     (double)opacity / 255.0, canvas ? canvas->theme : NULL);
}

static void backend_icon(void *surface, native_ui_rect_t rect, const char *name,
                         px_backend_color_t color, uint8_t opacity) {
    agent_ui_canvas_t *canvas = surface;
    if (!canvas) return;
    int radius = (rect.width < rect.height ? rect.width : rect.height)/3;
    if (radius < 3) radius = 3;
    int cx = rect.x + rect.width/2, cy = rect.y + rect.height/2;
    fill_circle(canvas, cx, cy, radius, color, (double)opacity / 320.0);
    stroke_round(canvas, (native_ui_rect_t){cx-radius,cy-radius,radius*2,radius*2},
                 radius, 1, color, (double)opacity / 255.0);
    if (name && (!strcmp(name,"tool") || !strcmp(name,"command")))
        draw_line(canvas, cx-radius/2, cy, cx+radius/2, cy, color, 0.9);
}

static void backend_line(void *surface, int x0, int y0, int x1, int y1,
                         px_backend_color_t color, uint8_t opacity) {
    agent_ui_canvas_t *canvas = surface;
    if (canvas) draw_line(canvas, x0,y0,x1,y1,color,(double)opacity/255.0);
}

static void backend_circle(void *surface, int cx, int cy, int radius,
                           px_backend_color_t color, uint8_t opacity) {
    agent_ui_canvas_t *canvas = surface;
    if (canvas) fill_circle(canvas,cx,cy,radius,color,(double)opacity/255.0);
}

static void backend_custom(void *surface, const native_ui_node_t *node,
                           const px_backend_palette_t *palette) {
    agent_ui_canvas_t *canvas = surface;
    if (!canvas || !node || !palette) return;
    if (!strncmp(node->text, "theme-swatch:", 13))
        draw_theme_swatch(canvas, node);
    else if (!strcmp(node->text, "activity-pulse"))
        draw_activity(canvas, node, palette);
    else if (!strncmp(node->text, "topology:", 9))
        draw_topology(canvas, node, palette);
    else {
        native_ui_rect_t r = node->frame;
        fill_circle(canvas, r.x+r.width/2, r.y+r.height/2,
                    (r.width < r.height ? r.width : r.height)/5,
                    palette->colors[NATIVE_UI_COLOR_ACCENT], 0.72);
    }
}

static px_backend_ops_t canvas_ops(void) {
    return (px_backend_ops_t){
        .push_clip = backend_push_clip,
        .pop_clip = backend_pop_clip,
        .fill_rect = backend_fill,
        .stroke_rect = backend_stroke,
        .draw_text = backend_text,
        .draw_icon = backend_icon,
        .draw_line = backend_line,
        .fill_circle = backend_circle,
        .draw_custom = backend_custom,
    };
}

agent_ui_canvas_t *agent_ui_canvas_create(int physical_width,
                                          int physical_height,
                                          int backing_scale,
                                          const agent_ui_theme_t *theme) {
    if (physical_width < 320 || physical_height < 180 ||
        physical_width > 8192 || physical_height > 8192 ||
        backing_scale < 1 || backing_scale > 4 ||
        (uint64_t)physical_width * (uint64_t)physical_height >
            AGENT_UI_CANVAS_MAX_PIXELS)
        return NULL;
    agent_ui_canvas_t *canvas = calloc(1, sizeof(*canvas));
    if (!canvas) return NULL;
    canvas->width = physical_width;
    canvas->height = physical_height;
    canvas->backing_scale = backing_scale;
    canvas->logical_width = physical_width / backing_scale;
    canvas->logical_height = physical_height / backing_scale;
    canvas->pixels = malloc((size_t)physical_width * (size_t)physical_height * 3U);
    if (!canvas->pixels) { free(canvas); return NULL; }
    canvas->theme = theme ? theme : agent_ui_theme_default();
    px_backend_ops_t ops = canvas_ops();
    px_backend_init(&canvas->backend, canvas, &canvas->theme->palette, &ops);
    return canvas;
}

void agent_ui_canvas_destroy(agent_ui_canvas_t *canvas) {
    if (!canvas) return;
    free(canvas->pixels);
    free(canvas);
}

bool agent_ui_canvas_set_theme(agent_ui_canvas_t *canvas,
                               const agent_ui_theme_t *theme) {
    if (!canvas || !theme) return false;
    canvas->theme = theme;
    canvas->backend.palette = theme->palette;
    return true;
}

bool agent_ui_canvas_render(agent_ui_canvas_t *canvas,
                            const native_ui_scene_t *scene,
                            const native_ui_scene_t *previous) {
    if (!canvas || !scene || !canvas->theme || !canvas->pixels) return false;
    canvas->clip_count = 0;
    fill_device_rect(canvas, (device_rect_t){0,0,canvas->width,canvas->height},
                     canvas->theme->palette.colors[NATIVE_UI_COLOR_CANVAS], 1.0);
    native_ui_render(scene, previous, px_backend_native_vtable(), &canvas->backend);
    canvas->clip_count = 0;
    return true;
}

bool agent_ui_canvas_write_ppm(const agent_ui_canvas_t *canvas,
                               const char *path) {
    if (!canvas || !canvas->pixels || !path || !*path) return false;
    FILE *file = fopen(path, "wb");
    if (!file) return false;
    size_t bytes = (size_t)canvas->width * (size_t)canvas->height * 3U;
    bool ok = fprintf(file, "P6\n%d %d\n255\n", canvas->width, canvas->height) > 0 &&
              fwrite(canvas->pixels, 1, bytes, file) == bytes;
    if (fclose(file) != 0) ok = false;
    return ok;
}

int agent_ui_canvas_physical_width(const agent_ui_canvas_t *canvas) {
    return canvas ? canvas->width : 0;
}
int agent_ui_canvas_physical_height(const agent_ui_canvas_t *canvas) {
    return canvas ? canvas->height : 0;
}
int agent_ui_canvas_logical_width(const agent_ui_canvas_t *canvas) {
    return canvas ? canvas->logical_width : 0;
}
int agent_ui_canvas_logical_height(const agent_ui_canvas_t *canvas) {
    return canvas ? canvas->logical_height : 0;
}
int agent_ui_canvas_backing_scale(const agent_ui_canvas_t *canvas) {
    return canvas ? canvas->backing_scale : 0;
}
const uint8_t *agent_ui_canvas_pixels(const agent_ui_canvas_t *canvas) {
    return canvas ? canvas->pixels : NULL;
}
size_t agent_ui_canvas_size(const agent_ui_canvas_t *canvas) {
    return canvas ? (size_t)canvas->width * (size_t)canvas->height * 3U : 0;
}
