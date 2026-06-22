/* ── shadeexpr.h: a tiny infix expression language for live "shadercode" ──────
 * Compile a math expression once, then evaluate it millions of times per frame
 * with zero allocation. Variable names resolve to fixed slot indices at compile
 * time so eval is a flat stack-machine walk over the caller-filled slots[].
 *
 * Used by fractal.c to let the user type a raw distance-estimator / shading
 * formula and hot-reload it without recompiling the binary.
 *
 * Grammar (C-like, all double):
 *   expr   := term (('+'|'-') term)*
 *   term   := pow  (('*'|'/'|'%') pow)*
 *   pow    := unary ('^' unary)*           // right-assoc, ^ = powf
 *   unary  := ('-'|'+')? atom
 *   atom   := number | ident | call | '(' expr ')'
 *   call   := ident '(' expr (',' expr)* ')'
 * Identifiers are variable slots (below) or constants pi, e, tau, phi.
 * Functions: sin cos tan asin acos atan atan2 exp log log2 sqrt cbrt abs floor
 *   ceil round frac sign mod min max clamp mix step smoothstep pow hypot
 *   length(x,y,z) dist(ax,ay,az,bx,by,bz) sphere(x,y,z,r) box(x,y,z,bx,by,bz)
 *   torus(x,y,z,R,r) noise(x,y,z) fbm(x,y,z)
 */
#ifndef DSCO_SHADEEXPR_H
#define DSCO_SHADEEXPR_H

#include <stddef.h>

/* Fixed variable slots the caller fills before each eval. Keep in sync with the
 * name table in shadeexpr.c. SE_SLOT_COUNT is the array length to allocate. */
enum {
    SE_X = 0,  /* current sample point x (object space)            */
    SE_Y,      /* y                                                */
    SE_Z,      /* z                                                */
    SE_R,      /* radius = length(x,y,z) (precomputed convenience) */
    SE_T,      /* time / animation phase                           */
    SE_P,      /* "power" / primary user knob                      */
    SE_Q,      /* secondary user knob                              */
    SE_S,      /* tertiary user knob                               */
    SE_I,      /* iteration index when used inside a fold          */
    SE_U,      /* spare / screen-space u                           */
    SE_V,      /* spare / screen-space v                           */
    SE_W,      /* spare / 4th-dim slice                            */
    SE_SLOT_COUNT
};

typedef struct shadeexpr shadeexpr_t;

/* Compile `src`. On error returns NULL and writes a human-readable message
 * (with a caret/offset hint when possible) into errbuf. errbuf may be NULL. */
shadeexpr_t *shadeexpr_compile(const char *src, char *errbuf, size_t errcap);

/* Evaluate against caller-filled slots (length must be >= SE_SLOT_COUNT).
 * Allocation-free and reentrant; safe to call from a tight per-pixel loop.
 * NaN/Inf results are sanitized to a large finite value by the caller. */
double shadeexpr_eval(const shadeexpr_t *e, const double *slots);

/* Source text the expression was compiled from (for the controls HUD). */
const char *shadeexpr_source(const shadeexpr_t *e);

void shadeexpr_free(shadeexpr_t *e);

#endif /* DSCO_SHADEEXPR_H */
