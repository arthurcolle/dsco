#ifndef DSCO_FRACTAL_H
#define DSCO_FRACTAL_H

#include <stddef.h>
#include <stdbool.h>

/* ── 3-D / 4-D distance-estimated fractals on the Braille grid ──────────────
 * Ray-marches signed-distance fractals (Mandelbulb, quaternion Julia, Mandelbox)
 * and projects them onto the same 2×4 Braille subpixel canvas the rest of the
 * viz stack uses. Shading is recovered from a binary dot grid two ways at once:
 *   • ordered (Bayer) dithering of the lit subpixels encodes luminance spatially
 *   • each character cell is tinted on a palette by its mean luminance
 * so a monochrome dot field reads as a lit 3-D surface.
 *
 * The "4-D" piece is the quaternion Julia set: its constant c is a quaternion
 * and the 3-D render is a slice at a chosen 4th coordinate (`wslice`). Sweeping
 * wslice and rotating c through 4-space is the actual 4-D exploration.
 *
 * Two entry points, mirroring plot.c / anim.c:
 *   fractal_plot  — render ONE still frame into a string (tool-result safe)
 *   fractal_anim  — run a live rotating/morphing loop on the terminal
 * Both take the same JSON spec. */

/* True if `kind` names a fractal this module renders (mandelbulb, quaternion/
 * julia4d, mandelbox, …) — lets anim.c/plot.c route without knowing the list. */
bool fractal_is_kind(const char *kind);

/* Render a single still frame for the JSON spec into `out` (NUL-terminated,
 * truncated to `cap`). Returns bytes written (snprintf semantics) or -1.
 * Spec keys: kind, width, height, color, iters, power, palette, title,
 *   cx,cy,cz,cw (quaternion constant), wslice, scale, yaw, pitch, dist, fov. */
int fractal_plot(char *out, size_t cap, const char *json);

/* Run the animated loop (rotating camera; for quaternion, sweeping the 4-D
 * slice and rotating c). Honors gens/fps/color from the spec. Returns the
 * number of frames shown, or -1 on error. */
int fractal_anim(const char *json);

#endif /* DSCO_FRACTAL_H */
