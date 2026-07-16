#ifndef DSCO_PX_THEME_H
#define DSCO_PX_THEME_H

#include "px_backend.h"

#include <stdbool.h>

/* Named pixel themes for the agentic terminal.
 *
 * A theme is the single source of color truth for every pixel surface: the
 * kitty compositor (pixel_tui), the px_backend token palette, the widget
 * library, and the animated wordmark all resolve through the active theme.
 * Slots mirror the classic quiet-console vocabulary so existing draw code
 * themes without retuning: two accents (the old cyan/violet pair), three
 * semantic tones, a canvas gradient, and a brand color that tints the
 * Distributed wordmark. */

#define PX_THEME_CHART_SERIES 6

typedef struct {
    const char *name;        /* stable kebab-case identifier */
    const char *display_name;/* human-facing catalog label */
    const char *description; /* one-line human summary */
    bool light;              /* light-surface theme */
    bool high_contrast;
    float shadow_opacity;

    /* Surfaces: full-bleed canvas gradient plus two panel elevations. */
    px_backend_color_t bg_top, bg_bottom;
    px_backend_color_t panel, panel_alt;

    /* Ink. */
    px_backend_color_t text, dim;

    /* Accents: primary (classic cyan slot) and secondary (violet slot). */
    px_backend_color_t accent, accent_alt;

    /* Semantic tones. */
    px_backend_color_t success, warning, danger;

    /* Brand mark tint (animated wordmark, splash chrome). */
    px_backend_color_t brand;

    /* Categorical chart series, mutually distinguishable on the surfaces. */
    px_backend_color_t chart[PX_THEME_CHART_SERIES];
} px_theme_t;

int px_theme_count(void);
const px_theme_t *px_theme_get(int index);
const px_theme_t *px_theme_find(const char *name); /* NULL when unknown */

/* Active theme for the process. First use consults DSCO_PIXEL_THEME (then
 * DSCO_THEME); unset or unknown falls back to quiet-console. Never NULL. */
const px_theme_t *px_theme_active(void);

/* Select by name. Unknown or NULL names leave the selection unchanged.
 * Returns the (possibly unchanged) active theme. */
const px_theme_t *px_theme_set_active(const char *name);

/* Step through the registry (+1 forward, -1 back). Returns the new theme. */
const px_theme_t *px_theme_cycle(int direction);

/* px_backend token palette for a theme (NULL means the active theme). */
px_backend_palette_t px_theme_palette(const px_theme_t *theme);

#endif /* DSCO_PX_THEME_H */
