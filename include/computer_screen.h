#ifndef DSCO_COMPUTER_SCREEN_H
#define DSCO_COMPUTER_SCREEN_H

#include <stdbool.h>
#include <stddef.h>

/* Screenshot coordinate space for the computer tool.
 *
 * Providers downscale large images before the model sees them (Anthropic caps
 * the long edge near 1568 px; OpenAI shrinks the short side to 768 px). On a
 * high-resolution display a full-size screenshot therefore puts the model in a
 * different coordinate space than the screen. We send a screenshot that no
 * provider will resize and map every coordinate between that space and
 * display points. */

#define COMPUTER_SCREEN_DEFAULT_LONG 1280
#define COMPUTER_SCREEN_DEFAULT_SHORT 768
#define COMPUTER_SCREEN_MIN_EDGE 320

typedef struct {
    int display_w, display_h; /* logical points, the space CGEvent uses */
    int shot_w, shot_h;       /* image the model sees */
} computer_space_t;

/* Fit the display inside long x short limits, preserving aspect. Limits come
 * from DSCO_COMPUTER_SCREENSHOT_MAX ("1280x768") when env is true. */
void computer_space_fit(int display_w, int display_h, int max_long, int max_short,
                        computer_space_t *out);
void computer_space_current(int display_w, int display_h, computer_space_t *out);

/* Screenshot pixel -> display point (clamped to the display). */
void computer_space_to_display(const computer_space_t *s, int x, int y, int *px, int *py);
/* Display point -> screenshot pixel. */
void computer_space_to_shot(const computer_space_t *s, int px, int py, int *x, int *y);

/* Capture the main display at full resolution and write a JPEG of exactly
 * shot_w x shot_h to a private temp file. */
bool computer_screen_capture(const computer_space_t *s, char *path, size_t path_len);

/* Capture a region given in screenshot coordinates at the display's native
 * resolution, scaled to fit the screenshot limits. out_w/out_h receive the
 * image size. */
bool computer_screen_zoom(const computer_space_t *s, int x1, int y1, int x2, int y2,
                          char *path, size_t path_len, int *out_w, int *out_h);

#endif
