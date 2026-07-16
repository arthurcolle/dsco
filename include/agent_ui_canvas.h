#ifndef DSCO_AGENT_UI_CANVAS_H
#define DSCO_AGENT_UI_CANVAS_H

#include "agent_ui_theme.h"
#include "native_ui.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct agent_ui_canvas agent_ui_canvas_t;

/* physical_* are exact RGB backing-store dimensions. Scene coordinates stay
 * logical; backing_scale is applied once here to every shape and glyph. */
agent_ui_canvas_t *agent_ui_canvas_create(int physical_width,
                                          int physical_height,
                                          int backing_scale,
                                          const agent_ui_theme_t *theme);
void agent_ui_canvas_destroy(agent_ui_canvas_t *canvas);
bool agent_ui_canvas_set_theme(agent_ui_canvas_t *canvas,
                               const agent_ui_theme_t *theme);
bool agent_ui_canvas_render(agent_ui_canvas_t *canvas,
                            const native_ui_scene_t *scene,
                            const native_ui_scene_t *previous);
bool agent_ui_canvas_write_ppm(const agent_ui_canvas_t *canvas,
                               const char *path);

int agent_ui_canvas_physical_width(const agent_ui_canvas_t *canvas);
int agent_ui_canvas_physical_height(const agent_ui_canvas_t *canvas);
int agent_ui_canvas_logical_width(const agent_ui_canvas_t *canvas);
int agent_ui_canvas_logical_height(const agent_ui_canvas_t *canvas);
int agent_ui_canvas_backing_scale(const agent_ui_canvas_t *canvas);
const uint8_t *agent_ui_canvas_pixels(const agent_ui_canvas_t *canvas);
size_t agent_ui_canvas_size(const agent_ui_canvas_t *canvas);

#endif /* DSCO_AGENT_UI_CANVAS_H */
