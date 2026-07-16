#ifndef DSCO_PX_BACKEND_H
#define DSCO_PX_BACKEND_H

#include "native_ui.h"

#include <stdbool.h>
#include <stdint.h>

/* Semantic native_ui -> packed-pixel adapter.
 *
 * The adapter owns token mapping and the standard custom primitives (meters,
 * sparklines, and artifact placeholders).  The transport supplies a compact
 * set of raster operations, so Kitty, a native window, and headless snapshot
 * surfaces can consume the same retained scene without cloning UI behavior. */

typedef struct {
    uint8_t r, g, b;
} px_backend_color_t;

typedef struct {
    px_backend_color_t colors[NATIVE_UI_COLOR_COUNT];
} px_backend_palette_t;

struct px_backend;

typedef struct {
    void (*begin_frame)(void *surface, native_ui_rect_t viewport,
                        const native_ui_damage_t *damage);
    void (*push_clip)(void *surface, native_ui_rect_t rect);
    void (*pop_clip)(void *surface);
    void (*fill_rect)(void *surface, native_ui_rect_t rect,
                      px_backend_color_t color, uint8_t opacity,
                      uint8_t radius, bool raised);
    void (*stroke_rect)(void *surface, native_ui_rect_t rect,
                        px_backend_color_t color, uint8_t opacity,
                        uint8_t width, uint8_t radius);
    void (*draw_text)(void *surface, native_ui_rect_t rect, const char *text,
                      native_ui_type_token_t type, px_backend_color_t color,
                      uint8_t opacity);
    void (*draw_icon)(void *surface, native_ui_rect_t rect, const char *name,
                      px_backend_color_t color, uint8_t opacity);
    void (*draw_line)(void *surface, int x0, int y0, int x1, int y1,
                      px_backend_color_t color, uint8_t opacity);
    void (*fill_circle)(void *surface, int cx, int cy, int radius,
                        px_backend_color_t color, uint8_t opacity);
    /* Transport-specific semantic extensions, such as the live DSCO soul
     * mark. Standard meter/sparkline/image elements are handled here first. */
    void (*draw_custom)(void *surface, const native_ui_node_t *node,
                        const px_backend_palette_t *palette);
    void (*end_frame)(void *surface);
} px_backend_ops_t;

typedef struct px_backend {
    void *surface;
    px_backend_palette_t palette;
    px_backend_ops_t ops;
    native_ui_damage_t last_damage;
} px_backend_t;

void px_backend_init(px_backend_t *backend, void *surface,
                     const px_backend_palette_t *palette,
                     const px_backend_ops_t *ops);
const native_ui_backend_t *px_backend_native_vtable(void);
px_backend_color_t px_backend_resolve_color(const px_backend_t *backend,
                                            native_ui_color_token_t token);
const native_ui_damage_t *px_backend_last_damage(const px_backend_t *backend);

#endif /* DSCO_PX_BACKEND_H */
