#ifndef DSCO_ANIM_H
#define DSCO_ANIM_H

#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <signal.h>
#include "tui.h"

/* ── Terminal cell animations ──────────────────────────────────────────────
 * Direct-to-terminal animated renderers built on the same Braille subpixel
 * grid as plot.c (2×4 dots per character cell, so a W×H cell canvas drives a
 * 2W×4H simulation grid). Each renderer runs its own frame loop, repainting
 * in place via cursor-home, and honors SIGINT for a clean teardown that
 * restores the cursor and leaves the final frame on screen.
 *
 * These are meant for the *direct* output path (a `--anim` subcommand or a
 * raw tool exec), NOT the agent tool-result path — animation needs real time
 * and a TTY, neither of which survives a JSON round-trip. */

typedef struct {
    const char *kind;     /* "life" | "rule" | "attractor" | "rain" */
    const char *title;    /* optional heading printed above the canvas */
    int    width;         /* canvas width in character cells (0 = auto) */
    int    height;        /* canvas height in character rows (0 = auto) */
    long   gens;          /* frames to render; <= 0 = run until interrupted */
    int    fps;           /* frames per second (default 18) */
    bool   color;         /* emit ANSI color (density/age ramp) */
    bool   wrap;          /* toroidal edges (life) */
    unsigned seed;        /* RNG seed for random fills (0 = fixed default) */
    const char *pattern;  /* life: random|gliders|gun|pulsar|pentomino|acorn */
    double density;       /* life: initial random fill fraction (0..1) */
    int    rule;          /* elementary CA rule number (0..255, default 110) */
} anim_opts_t;

/* Defaults: auto size, 18 fps, color+wrap on, random life at 0.30 density. */
anim_opts_t anim_opts_default(void);

/* Each runs a render loop on stdout and returns the number of frames shown,
 * or -1 on error. They block until `gens` frames elapse or SIGINT arrives. */
int anim_life(const anim_opts_t *o);       /* Conway's Game of Life */
int anim_rule(const anim_opts_t *o);       /* scrolling 1-D elementary CA */
int anim_attractor(const anim_opts_t *o);  /* morphing De Jong attractor */
int anim_rain(const anim_opts_t *o);       /* falling-glyph "digital rain" */

/* Parse a JSON spec ({"kind":"life","width":80,...}) and route to the right
 * renderer. Unknown/empty kind defaults to "life". Returns frames or -1. */
int anim_dispatch(const char *json);

/* ── Shared frame-loop primitives (used by sibling renderers, e.g. fractal.c) ──
 * A renderer that wants the same teardown/timing/painting as the built-ins
 * brackets its loop with anim_begin()/anim_end(), sleeps a frame with
 * anim_tick(), and paints with anim_paint(). */
typedef struct { struct sigaction old_int; } anim_term_t;

void anim_begin(anim_term_t *t);        /* hide cursor, clear, trap SIGINT */
void anim_end(anim_term_t *t);          /* restore cursor + SIGINT handler */
bool anim_tick(int fps);                /* sleep one frame; false if interrupted */
bool anim_interrupted(void);            /* SIGINT seen since anim_begin() */
long anim_frame_budget(long gens);      /* gens, or -1 on TTY / 240 off-TTY */
void anim_resolve_size(int width, int height, int *W, int *H);

/* Paint a Braille canvas to `out`. If cell_color is non-NULL it holds one
 * 256-color index per character cell (negative = blank/dead field); otherwise
 * each cell is tinted by its live-subpixel popcount on the viridis ramp. */
void anim_paint(FILE *out, const tui_braille_t *bc, bool color,
                const int *cell_color);

#endif /* DSCO_ANIM_H */
