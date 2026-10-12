#ifndef DSCO_KITTY_LAB_H
#define DSCO_KITTY_LAB_H

#include <stdbool.h>
#include <stdio.h>

typedef enum {
    KITTY_LAB_VIEW_OVERVIEW = 0,
    KITTY_LAB_VIEW_PLAN,
    KITTY_LAB_VIEW_ACTIONS,
} kitty_lab_view_t;

/* Deterministic native-pixel gallery for exercising the semantic DSCO
 * surfaces without a live model or tool call. */
bool kitty_lab_write_ppm(const char *path, int width, int height, int frame, int frames);
bool kitty_lab_render(FILE *out, int width, int height, int frames, bool animate);

/* Alternate planning fixtures.  The existing functions remain the overview
 * defaults so callers do not need to change; view-specific entry points make
 * hierarchy, dependency readiness, and action timing independently testable. */
bool kitty_lab_write_ppm_view(const char *path, int width, int height, int frame,
                              int frames, kitty_lab_view_t view);
bool kitty_lab_render_view(FILE *out, int width, int height, int frames, bool animate,
                           kitty_lab_view_t view);

/* Opt-in HDR resolve.  The scene is authored in SDR, lifted to linear float,
 * given synthetic highlight headroom, bloomed, tonemapped, and dithered back
 * to the RGB24 the Kitty wire requires.  Off by default: it costs a float
 * buffer plus a mip pyramid per frame and materially reduces zlib ratio on
 * the transmit path, so animated surfaces should measure before enabling. */
void kitty_lab_set_hdr(bool enabled);
bool kitty_lab_hdr_enabled(void);

#endif /* DSCO_KITTY_LAB_H */
