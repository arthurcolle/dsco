#ifndef DSCO_KITTY_BANNER_H
#define DSCO_KITTY_BANNER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

/* Distributed Systems wordmark rendered as a native-pixel Kitty animation.
 * Frames are transmitted once via the graphics protocol's animation keys
 * (a=t / a=f) and started with a=a,s=3, so the terminal owns the loop: the
 * banner keeps animating pixel-by-pixel after the shell moves on (and after
 * the emitting process exits), while the placement scrolls with the
 * transcript like ordinary output. */

bool kitty_banner_available(FILE *out);

/* Drop the cached device cell-pixel metrics so the next render re-queries the
 * terminal (CSI 16t/14t). Call on SIGWINCH: a window resize can accompany a
 * font-size or display-DPI change, which changes the pixels-per-cell the
 * banner canvas must be sized against. Cheap and thread-safe to call from the
 * resize path; the actual re-query happens lazily on the next render. */
void kitty_banner_invalidate_geometry(void);

/* Delete every banner image this process transmitted (placements and pixel
 * data), stopping any server-side animation loop left running. No-op when no
 * pixel-native banner was rendered, so it is safe on every exit path. */
void kitty_banner_clear(FILE *out);

/* Freeze any resident banner animation on its root frame, keeping the
 * placement visible. A server-side loop that keeps running while the session
 * scrolls and reflows repaints its frames at stale grid positions (torn
 * rows, image blitted over text) and can wedge kitty's renderer outright
 * when the OS window is resized. Call when the interactive session starts
 * doing real work; idempotent and a no-op when nothing is resident. */
void kitty_banner_settle(FILE *out);

/* Laser-wipe any resident banner out of the terminal: a white-hot beam
 * sweeps the wordmark away (server-side animation paths append and step
 * dissolve frames on the resident image id, so the effect plays wherever
 * the placement has scrolled; the layered renderer powers its planes down
 * top-to-bottom), then the images are deleted. Blocks ~500ms while the
 * effect plays. Returns 1 if an effect ran, 0 when nothing was resident.
 * Idempotent; also disarms the exit-time kitty_banner_clear. */
int kitty_banner_dissolve(FILE *out);

/* Emit the animated wordmark at the current cursor position. Returns the
 * number of terminal rows occupied (cursor ends on the line below), or 0
 * when the terminal is unsuitable or transmission failed. */
int kitty_banner_render(FILE *out);

/* Layered variant: the scene decomposed into independent images (deep
 * particles, grid, packet streams, wordmark, specular gloss) stacked with
 * placement z-indexes over one cell anchor, each plane flipping at its own
 * tempo. Animation is client-driven over base graphics commands (per-tick
 * a=T retransmission under stable image ids) so it plays on any terminal
 * with kitty graphics support, independent of the animation extension and
 * its frame-storage quotas. Plays `loops` wordmark cycles then settles.
 * Same return contract as kitty_banner_render. */
int kitty_banner_render_layers(FILE *out, int loops);

/* Best pixel-native renderer for the detected terminal: the persistent
 * server-side animation on real kitty, one client-driven layered cycle on
 * every other kitty-graphics terminal (iTerm2 ≥ 3.6, ghostty, wezterm — the
 * protocol is probed on the tty, not assumed from env). Returns rows
 * occupied, 0 when pixel graphics are unavailable. */
int kitty_banner_render_auto(FILE *out);

/* Artifact dump for the layered variant: every layer alone mid-loop plus
 * software composites of the full stack at several instants, as PPMs. */
bool kitty_banner_write_layers_ppm(const char *dir);

/* Deterministic artifact path for visual tests: composite animation frame
 * `frame` (of `frames`) over a dark backdrop and write it as a PPM. */
bool kitty_banner_write_ppm(const char *path, int frame, int frames);

/* Render the same DSCO wordmark scene used by the startup splash into a caller-
 * owned RGBA buffer. This is the backend-neutral seam used by the native
 * compositor's persistent upper-left chrome; no Kitty placement is created.
 * `stride` is bytes per output row and must be at least width * 4. */
bool kitty_banner_render_rgba(unsigned char *rgba, int width, int height,
                              size_t stride, int frame, int frames);

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
