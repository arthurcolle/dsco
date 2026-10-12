#ifndef DSCO_AGENT_UI_THEME_H
#define DSCO_AGENT_UI_THEME_H

#include "px_backend.h"
#include "px_theme.h"

#include <stdbool.h>
#include <stddef.h>

/* A complete visual contract for agent-native surfaces. Components consume
 * spacing/type/shape tokens while renderers consume the semantic palette.
 * Keeping those together makes a theme portable across Kitty, ANSI, DOM, and
 * future native backends without putting literal colors in component code. */
typedef struct {
    int xs, sm, md, lg, xl;
} agent_ui_spacing_tokens_t;

typedef struct {
    int sm, md, lg, pill;
} agent_ui_radius_tokens_t;

typedef struct {
    float label, body, title, display, code, metric;
} agent_ui_type_tokens_t;

typedef struct {
    const char *id;
    const char *name;
    const char *description;
    bool light;
    bool high_contrast;
    float shadow_opacity;
    const px_theme_t *pixel_theme;
    px_backend_palette_t palette;
    agent_ui_spacing_tokens_t spacing;
    agent_ui_radius_tokens_t radius;
    agent_ui_type_tokens_t type;
} agent_ui_theme_t;

size_t agent_ui_theme_count(void);
const agent_ui_theme_t *agent_ui_theme_at(size_t index);
const agent_ui_theme_t *agent_ui_theme_find(const char *id);
const agent_ui_theme_t *agent_ui_theme_default(void);
const agent_ui_theme_t *agent_ui_theme_next(const agent_ui_theme_t *theme,
                                            int direction);

/* Checks catalog/theme invariants and minimum readable text contrast. */
bool agent_ui_theme_validate(const agent_ui_theme_t *theme,
                             char *message, size_t message_capacity);
double agent_ui_theme_contrast(px_backend_color_t foreground,
                               px_backend_color_t background);

#endif /* DSCO_AGENT_UI_THEME_H */
