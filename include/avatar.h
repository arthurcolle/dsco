#ifndef DSCO_AVATAR_H
#define DSCO_AVATAR_H

#include <stddef.h>
#include <stdbool.h>

/* ── Procedural 3-D avatar faces ────────────────────────────────────────────
 * A face is an SDF scene (ellipsoid head + eyes/iris/pupils, nose, mouth, ears,
 * brows, hair, optional glasses) ray-marched onto the Braille subpixel grid,
 * shaded per-material (skin / eye-white / iris / hair / lip ramps) rather than
 * by a single luminance ramp. Every parameter — head shape, eye spacing, skin
 * tone, hair style/color, iris color — derives deterministically from a name
 * via FNV-1a + mulberry32, so the same name always yields the same face (same
 * convention as the pets sprites).
 *
 * Mirrors fractal.c / anim.c: a still string renderer and a live rotating loop,
 * both driven by the same JSON spec, both reusing anim.c's frame primitives. */

/* True if `kind` names this renderer ("face" / "avatar"). */
bool avatar_is_kind(const char *kind);

/* Render one still frame for the spec into `out` (NUL-terminated, truncated to
 * `cap`). Returns bytes written (snprintf semantics) or -1.
 * Spec keys: name (seed), width, height, color, title, and optional explicit
 *   overrides skin (0-4), hair (style 0-4), haircol (0-4), iris (0-3),
 *   glasses (bool), yaw, pitch, dist. */
int avatar_plot(char *out, size_t cap, const char *json);

/* Run the animated loop: the head turns side to side and blinks. Honors
 * gens/fps/color from the spec. Returns frames shown, or -1. */
int avatar_anim(const char *json);

#endif /* DSCO_AVATAR_H */
