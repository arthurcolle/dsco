#include "agent_ui_theme.h"

#include <ctype.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#define AGENT_UI_THEME_CAP 32

static agent_ui_theme_t s_themes[AGENT_UI_THEME_CAP];
static size_t s_theme_count;
static pthread_once_t s_theme_once = PTHREAD_ONCE_INIT;

static bool is_tight_theme(const char *id) {
    return id && (!strcmp(id, "industrial") || !strcmp(id, "matrix") ||
                  !strcmp(id, "amber-crt") || !strcmp(id, "blueprint") ||
                  !strcmp(id, "high-contrast"));
}

static bool is_spatial_theme(const char *id) {
    return id && (!strcmp(id, "native-glass") ||
                  !strcmp(id, "spatial-command") || !strcmp(id, "synthwave"));
}

static void initialize_themes(void) {
    int count = px_theme_count();
    if (count < 0) count = 0;
    if (count > AGENT_UI_THEME_CAP) count = AGENT_UI_THEME_CAP;
    s_theme_count = (size_t)count;
    for (int i = 0; i < count; i++) {
        const px_theme_t *pixel = px_theme_get(i);
        agent_ui_theme_t *theme = &s_themes[i];
        theme->id = pixel->name;
        theme->name = pixel->display_name ? pixel->display_name : pixel->name;
        theme->description = pixel->description;
        theme->light = pixel->light;
        theme->high_contrast = pixel->high_contrast;
        theme->shadow_opacity = pixel->shadow_opacity;
        theme->pixel_theme = pixel;
        theme->palette = px_theme_palette(pixel);
        theme->spacing = (agent_ui_spacing_tokens_t){3, 7, 11, 17, 25};
        theme->radius = (agent_ui_radius_tokens_t){3, 7, 12, 999};
        theme->type = (agent_ui_type_tokens_t){10, 13, 17, 24, 12, 21};
        if (is_tight_theme(pixel->name)) {
            theme->spacing = (agent_ui_spacing_tokens_t){2, 5, 8, 12, 18};
            theme->radius = (agent_ui_radius_tokens_t){0, 1, 3, 2};
            theme->type = (agent_ui_type_tokens_t){10, 13, 16, 22, 13, 20};
        } else if (is_spatial_theme(pixel->name)) {
            theme->spacing = (agent_ui_spacing_tokens_t){4, 8, 12, 20, 30};
            theme->radius = (agent_ui_radius_tokens_t){8, 14, 22, 999};
            theme->type = (agent_ui_type_tokens_t){10, 13, 17, 25, 12, 22};
        } else if (pixel->light) {
            theme->spacing = (agent_ui_spacing_tokens_t){4, 8, 12, 18, 28};
            theme->radius = (agent_ui_radius_tokens_t){3, 7, 12, 999};
        }
        if (pixel->high_contrast)
            theme->type = (agent_ui_type_tokens_t){11, 14, 18, 25, 13, 22};
    }
}

static void ensure_themes(void) {
    (void)pthread_once(&s_theme_once, initialize_themes);
}

size_t agent_ui_theme_count(void) {
    ensure_themes();
    return s_theme_count;
}

const agent_ui_theme_t *agent_ui_theme_at(size_t index) {
    ensure_themes();
    return index < s_theme_count ? &s_themes[index] : NULL;
}

const agent_ui_theme_t *agent_ui_theme_find(const char *id) {
    if (!id || !*id) return NULL;
    ensure_themes();
    for (size_t i = 0; i < s_theme_count; i++)
        if (!strcmp(s_themes[i].id, id)) return &s_themes[i];
    return NULL;
}

const agent_ui_theme_t *agent_ui_theme_default(void) {
    const px_theme_t *active = px_theme_active();
    const agent_ui_theme_t *theme = active ? agent_ui_theme_find(active->name) : NULL;
    return theme ? theme : agent_ui_theme_at(0);
}

const agent_ui_theme_t *agent_ui_theme_next(const agent_ui_theme_t *theme,
                                            int direction) {
    ensure_themes();
    if (s_theme_count == 0) return NULL;
    size_t index = 0;
    for (size_t i = 0; i < s_theme_count; i++)
        if (theme == &s_themes[i] ||
            (theme && theme->id && !strcmp(theme->id, s_themes[i].id))) {
            index = i;
            break;
        }
    index = direction < 0 ? (index + s_theme_count - 1) % s_theme_count
                          : (index + 1) % s_theme_count;
    return &s_themes[index];
}

static double srgb_channel(uint8_t channel) {
    double value = (double)channel / 255.0;
    return value <= 0.04045 ? value / 12.92
                            : pow((value + 0.055) / 1.055, 2.4);
}

static double luminance(px_backend_color_t color) {
    return 0.2126 * srgb_channel(color.r) +
           0.7152 * srgb_channel(color.g) +
           0.0722 * srgb_channel(color.b);
}

double agent_ui_theme_contrast(px_backend_color_t foreground,
                               px_backend_color_t background) {
    double a = luminance(foreground), b = luminance(background);
    if (a < b) { double swap = a; a = b; b = swap; }
    return (a + 0.05) / (b + 0.05);
}

bool agent_ui_theme_validate(const agent_ui_theme_t *theme,
                             char *message, size_t message_capacity) {
    const char *error = NULL;
    if (!theme) error = "theme is null";
    else if (!theme->id || !*theme->id) error = "theme id is empty";
    else if (!theme->name || !*theme->name) error = "theme name is empty";
    if (!error) {
        for (const char *p = theme->id; *p; p++)
            if (!islower((unsigned char)*p) && !isdigit((unsigned char)*p) &&
                *p != '-') {
                error = "theme id must be lowercase kebab case";
                break;
            }
    }
    if (!error && agent_ui_theme_contrast(
            theme->palette.colors[NATIVE_UI_COLOR_TEXT],
            theme->palette.colors[NATIVE_UI_COLOR_CANVAS]) < 4.5)
        error = "text/canvas contrast is below 4.5:1";
    if (!error && (theme->spacing.xs < 0 || theme->spacing.sm < theme->spacing.xs ||
                   theme->spacing.md < theme->spacing.sm ||
                   theme->spacing.lg < theme->spacing.md ||
                   theme->spacing.xl < theme->spacing.lg))
        error = "spacing tokens are not monotonic";
    if (message && message_capacity)
        snprintf(message, message_capacity, "%s", error ? error : "ok");
    return error == NULL;
}
