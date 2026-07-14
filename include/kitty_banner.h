#ifndef DSCO_KITTY_BANNER_H
#define DSCO_KITTY_BANNER_H

#include <stdbool.h>
#include <stdio.h>

/* Distributed Systems wordmark rendered as a native-pixel Kitty animation.
 * Frames are transmitted once via the graphics protocol's animation keys
 * (a=t / a=f) and started with a=a,s=3, so the terminal owns the loop: the
 * banner keeps animating pixel-by-pixel after the shell moves on (and after
 * the emitting process exits), while the placement scrolls with the
 * transcript like ordinary output. */

bool kitty_banner_available(FILE *out);

/* Emit the animated wordmark at the current cursor position. Returns the
 * number of terminal rows occupied (cursor ends on the line below), or 0
 * when the terminal is unsuitable or transmission failed. */
int kitty_banner_render(FILE *out);

/* Layered variant: the scene decomposed into independent images (deep
 * particles, grid, packet streams, wordmark, specular gloss) stacked with
 * placement z-indexes over one cell anchor. Each layer runs its own
 * server-side animation loop at its own tempo, so the terminal composites
 * five live pixel planes. Same return contract as kitty_banner_render. */
int kitty_banner_render_layers(FILE *out);

/* Artifact dump for the layered variant: every layer alone mid-loop plus
 * software composites of the full stack at several instants, as PPMs. */
bool kitty_banner_write_layers_ppm(const char *dir);

/* Deterministic artifact path for visual tests: composite animation frame
 * `frame` (of `frames`) over a dark backdrop and write it as a PPM. */
bool kitty_banner_write_ppm(const char *path, int frame, int frames);

/* Portable finite-loop renderer. On graphics-protocol terminals it is
 * pixel-native: frames are transmitted once and the loop is driven by
 * stepping the placement's current frame, so `loops` cycles play at full
 * canvas resolution and end on a static frame. Elsewhere the frames are
 * rasterised to Unicode sextant mosaics (2×3 truecolor/256 sub-pixels per
 * cell) or legacy ▀ half blocks (1×2), redrawn in place.
 * DSCO_BANNER_CELLS=pixel|sextant|half pins a mode. Returns the number of
 * rows occupied (cursor ends below), 0 on failure. */
int dsco_banner_render_cells(FILE *out, int loops);

#endif /* DSCO_KITTY_BANNER_H */
