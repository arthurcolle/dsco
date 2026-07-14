#ifndef DSCO_FONT_COMPAT_H
#define DSCO_FONT_COMPAT_H

#include <stdbool.h>
#include <stdint.h>

/* Platform font bridge for native-pixel surfaces. The buffer is packed RGB,
 * top-to-bottom, with an explicit byte stride. Coordinates are top-left.
 * Returns the rendered advance in pixels, or -1 when the platform backend is
 * unavailable so callers can use their deterministic bitmap fallback. */
int font_compat_draw_rgb(uint8_t *rgb, int width, int height, int stride,
                         int x, int y, int max_width, const char *utf8,
                         float point_size, bool bold,
                         uint8_t red, uint8_t green, uint8_t blue,
                         float opacity);

int font_compat_draw_rgb_styled(uint8_t *rgb, int width, int height, int stride,
                                int x, int y, int max_width, const char *utf8,
                                float point_size, bool bold, bool italic,
                                uint8_t red, uint8_t green, uint8_t blue,
                                float opacity);

/* Typographic advance for a shaped UTF-8 line. This uses the same font
 * selection and fallback cascade as draw_rgb, so cursor placement and
 * truncation agree with the pixels that are actually rendered. */
int font_compat_measure_utf8(const char *utf8, float point_size, bool bold);
int font_compat_measure_utf8_styled(const char *utf8, float point_size,
                                    bool bold, bool italic);
int font_compat_line_height(float point_size, bool bold);

bool font_compat_available(void);

#endif /* DSCO_FONT_COMPAT_H */
