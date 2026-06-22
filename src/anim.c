/* ── anim.c: direct-to-terminal cell animations ───────────────────────────
 * Conway's Game of Life and friends, rendered on the Braille subpixel grid
 * (2×4 dots/cell) so a modest W×H character canvas drives a 2W×4H simulation.
 * Each renderer owns a frame loop that repaints in place via cursor-home and
 * tints cells on a viridis density ramp. SIGINT stops cleanly, restores the
 * cursor, and leaves the last frame on screen.
 *
 * These live on the *direct* output path (the `--anim` subcommand): animation
 * needs real wall-clock time and a TTY, neither of which survives the agent's
 * JSON tool-result round-trip — which is exactly why a static `plot` came back
 * monochrome and frozen. */

#include "anim.h"
#include "tui.h"
#include "json_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <stdint.h>

#include "fractal.h"

/* ── frame-loop control ──────────────────────────────────────────────────── */
static volatile sig_atomic_t g_anim_stop = 0;
static void anim_on_sigint(int sig) { (void)sig; g_anim_stop = 1; }

/* deterministic xorshift32 — no libc RNG state, fully reproducible per seed */
static uint32_t xs_state = 0x2545F491u;
static void     xs_seed(uint32_t s) { xs_state = s ? s : 0x2545F491u; }
static uint32_t xs_next(void) {
    uint32_t x = xs_state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return xs_state = x;
}
static double xs_unit(void) { return (xs_next() >> 8) * (1.0 / 16777216.0); }

/* viridis-ish 256-color ramp, t in [0,1] → deep blue → green → yellow.
 * Same anchor indices the plot.c attractor uses, so the two read as one tool. */
static int heat256(double t) {
    static const int ramp[] = { 17,18,19,20,26,32,38,44,49,48,47,46,82,118,154,190,220,226 };
    const int n = (int)(sizeof ramp / sizeof ramp[0]);
    if (t < 0) t = 0; if (t > 1) t = 1;
    int i = (int)(t * (n - 1) + 0.5);
    if (i < 0) i = 0; if (i >= n) i = n - 1;
    return ramp[i];
}

/* ── option plumbing ─────────────────────────────────────────────────────── */
anim_opts_t anim_opts_default(void) {
    anim_opts_t o = {0};
    o.kind = "life"; o.width = 0; o.height = 0; o.gens = 0; o.fps = 18;
    o.color = true; o.wrap = true; o.seed = 0; o.pattern = "random";
    o.density = 0.30; o.rule = 110;
    return o;
}

/* Resolve auto size from the terminal, leaving room for header + cursor. */
void anim_resolve_size(int width, int height, int *W, int *H) {
    int w = width, h = height;
    if (w <= 0) { int tw = tui_term_width();  w = (tw > 4 ? tw : 80) - 1; }
    if (h <= 0) { int th = tui_term_height(); h = (th > 6 ? th : 24) - 3; }
    if (w < 8)  w = 8;   if (w > 240) w = 240;
    if (h < 4)  h = 4;   if (h > 120) h = 120;
    *W = w; *H = h;
}
static void anim_size(const anim_opts_t *o, int *W, int *H) {
    anim_resolve_size(o->width, o->height, W, H);
}

/* Frames to run: <=0 means "until Ctrl-C" on a TTY, but bounded off-TTY so a
 * redirect or a non-interactive caller can't spew forever. */
long anim_frame_budget(long gens) {
    if (gens > 0) return gens;
    return isatty(STDOUT_FILENO) ? -1 : 240;
}

bool anim_interrupted(void) { return g_anim_stop != 0; }

/* ── terminal setup / teardown ───────────────────────────────────────────── */
void anim_begin(anim_term_t *t) {
    g_anim_stop = 0;
    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_handler = anim_on_sigint; sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, &t->old_int);
    fputs("\033[?25l\033[2J\033[H", stdout);   /* hide cursor, clear, home */
    fflush(stdout);
}
void anim_end(anim_term_t *t) {
    fputs("\033[0m\033[?25h\n", stdout);       /* reset, show cursor, drop below */
    fflush(stdout);
    sigaction(SIGINT, &t->old_int, NULL);
}

/* Sleep one frame, returning false if interrupted mid-sleep. */
bool anim_tick(int fps) {
    if (fps < 1) fps = 1; if (fps > 120) fps = 120;
    useconds_t us = (useconds_t)(1000000 / fps);
    if (usleep(us) != 0 && g_anim_stop) return false;
    return !g_anim_stop;
}

/* ── colored Braille blit ────────────────────────────────────────────────── */
/* Paint `bc` to `out`, one terminal row per cell-row, each row clear-to-EOL.
 * If `cell_color` is given it supplies a 256-color index per character cell
 * (negative = blank/dead field); otherwise each cell is tinted by its live
 * subpixel popcount on the viridis ramp. The caller homes the cursor and draws
 * any header rows first. */
void anim_paint(FILE *out, const tui_braille_t *bc, bool color,
                const int *cell_color) {
    char *mb = NULL; size_t ms = 0;
    FILE *f = open_memstream(&mb, &ms);
    if (f) { tui_braille_render(bc, f, NULL); fclose(f); }
    const int W = bc->w_cells;
    int row = 0; char *line = mb;
    while (line && *line && row < bc->h_cells) {
        char *nl = strchr(line, '\n'); if (nl) *nl = '\0';
        char *p = line; int col = 0;
        while (*p && col < W) {
            int glen = 1; unsigned char lead = (unsigned char)*p;
            if (lead >= 0xF0) glen = 4; else if (lead >= 0xE0) glen = 3;
            else if (lead >= 0xC0) glen = 2;
            if (color) {
                if (cell_color) {
                    int idx = cell_color[row * W + col];
                    if (idx >= 0) fprintf(out, "\033[38;5;%dm", idx);
                    else          fputs("\033[38;5;234m", out);
                } else {
                    int pc = __builtin_popcount((unsigned)bc->cells[row * W + col]);
                    if (pc) fprintf(out, "\033[38;5;%dm", heat256(pc / 8.0));
                    else    fputs("\033[38;5;234m", out);   /* faint dead-field */
                }
            }
            fwrite(p, 1, (size_t)glen, out); p += glen; col++;
        }
        if (color) fputs("\033[0m", out);
        fputs("\033[K\n", out);
        row++;
        if (!nl) break;
        line = nl + 1;
    }
    free(mb);
}

/* Built-in renderers paint to stdout with the popcount ramp. */
static void anim_blit(const tui_braille_t *bc, bool color) {
    anim_paint(stdout, bc, color, NULL);
}

static void anim_header(const char *title, const char *line) {
    fputs("\033[H", stdout);
    if (title && title[0]) printf("\033[1m%s\033[0m\033[K\n", title);
    if (line) printf("\033[2m%s\033[0m\033[K\n", line);
}

/* ════════════════════════════════════════════════════════════════════════
 * Conway's Game of Life (B3/S23)
 * ════════════════════════════════════════════════════════════════════════ */

static void life_seed_random(unsigned char *g, int gw, int gh, double density) {
    for (int i = 0; i < gw * gh; i++) g[i] = (xs_unit() < density) ? 1 : 0;
}

/* place a (x,y) cell list centered-ish at (ox,oy) with bounds checks */
static void life_stamp(unsigned char *g, int gw, int gh,
                       const int (*cells)[2], int n, int ox, int oy) {
    for (int i = 0; i < n; i++) {
        int x = ox + cells[i][0], y = oy + cells[i][1];
        if (x >= 0 && x < gw && y >= 0 && y < gh) g[y * gw + x] = 1;
    }
}

static void life_seed_pattern(unsigned char *g, int gw, int gh,
                              const char *pat, double density) {
    memset(g, 0, (size_t)gw * gh);
    if (!pat || !strcmp(pat, "random")) { life_seed_random(g, gw, gh, density); return; }

    static const int glider[][2]    = {{1,0},{2,1},{0,2},{1,2},{2,2}};
    static const int rpent[][2]      = {{1,0},{2,0},{0,1},{1,1},{1,2}};
    static const int acorn[][2]      = {{1,0},{3,1},{0,2},{1,2},{4,2},{5,2},{6,2}};
    static const int gun[][2] = {
        {0,4},{0,5},{1,4},{1,5},
        {10,4},{10,5},{10,6},{11,3},{11,7},{12,2},{12,8},{13,2},{13,8},
        {14,5},{15,3},{15,7},{16,4},{16,5},{16,6},{17,5},
        {20,2},{20,3},{20,4},{21,2},{21,3},{21,4},{22,1},{22,5},
        {24,0},{24,1},{24,5},{24,6},{34,2},{34,3},{35,2},{35,3}
    };

    if (!strcmp(pat, "gun")) {
        life_stamp(g, gw, gh, gun, (int)(sizeof gun / sizeof gun[0]), 2, 2);
    } else if (!strcmp(pat, "pentomino") || !strcmp(pat, "rpentomino")) {
        life_stamp(g, gw, gh, rpent, 5, gw / 2 - 1, gh / 2 - 1);
    } else if (!strcmp(pat, "acorn")) {
        life_stamp(g, gw, gh, acorn, 7, gw / 2 - 3, gh / 2 - 1);
    } else if (!strcmp(pat, "gliders")) {
        for (int gy = 2; gy < gh - 4; gy += 8)
            for (int gx = 2; gx < gw - 4; gx += 8)
                if (xs_unit() < 0.5) life_stamp(g, gw, gh, glider, 5, gx, gy);
    } else {
        life_seed_random(g, gw, gh, density);   /* unknown → random */
    }
}

/* Patterns that grow without bound (emitters / chaotic seeds) default to the
 * unbounded universe so they don't self-destruct against a toroidal edge. */
static bool life_pattern_unbounded(const char *p) {
    return p && (!strcmp(p, "gun") || !strcmp(p, "acorn") ||
                 !strcmp(p, "pentomino") || !strcmp(p, "rpentomino") ||
                 !strcmp(p, "gliders"));
}

/* Dead "apron" (in grid cells) kept around the visible window in the unbounded
 * universe so emitters fire past the viewport and are absorbed out of sight
 * instead of wrapping back to collide with their source. */
#define LIFE_APRON 16

/* Copy a viewport-sized seed into the larger sim grid at (ox,oy), zeroing the
 * rest. Keeps pattern placement (gun top-left, r-pentomino centered) relative
 * to the visible window rather than the apron-padded universe. */
static void life_embed(unsigned char *sim, int sw, int sh,
                       const unsigned char *src, int vw, int vh, int ox, int oy) {
    memset(sim, 0, (size_t)sw * sh);
    for (int y = 0; y < vh; y++)
        for (int x = 0; x < vw; x++)
            if (src[y * vw + x]) sim[(y + oy) * sw + (x + ox)] = 1;
}

/* Absorbing boundary: clear the outermost ring each step so gliders reaching
 * the hard edge dissipate cleanly instead of leaving a reflected remnant. */
static void life_absorb_border(unsigned char *g, int sw, int sh) {
    for (int x = 0; x < sw; x++) { g[x] = 0; g[(sh - 1) * sw + x] = 0; }
    for (int y = 0; y < sh; y++) { g[y * sw] = 0; g[y * sw + (sw - 1)] = 0; }
}

static long life_step(const unsigned char *cur, unsigned char *nxt,
                      int gw, int gh, bool wrap) {
    long pop = 0;
    for (int y = 0; y < gh; y++) {
        for (int x = 0; x < gw; x++) {
            int n = 0;
            for (int dy = -1; dy <= 1; dy++) {
                for (int dx = -1; dx <= 1; dx++) {
                    if (!dx && !dy) continue;
                    int nx = x + dx, ny = y + dy;
                    if (wrap) { nx = (nx + gw) % gw; ny = (ny + gh) % gh; }
                    else if (nx < 0 || nx >= gw || ny < 0 || ny >= gh) continue;
                    n += cur[ny * gw + nx];
                }
            }
            unsigned char alive = cur[y * gw + x];
            unsigned char live  = (alive && (n == 2 || n == 3)) || (!alive && n == 3);
            nxt[y * gw + x] = live;
            pop += live;
        }
    }
    return pop;
}

int anim_life(const anim_opts_t *o) {
    int W, H; anim_size(o, &W, &H);
    const int vw = W * 2, vh = H * 4;            /* visible window grid */
    xs_seed(o->seed ? o->seed : (uint32_t)(time(NULL) ^ (long)getpid()));

    /* With wrap off we run an *unbounded* universe: a grid padded by a dead
     * apron with absorbing edges, so patterns grow and emit past the window
     * without toroidal wrap feeding back on themselves. The window is a fixed
     * sub-rectangle offset by the apron M. */
    const int M  = o->wrap ? 0 : LIFE_APRON;
    const int sw = vw + 2 * M, sh = vh + 2 * M;  /* full sim grid */

    unsigned char *cur  = calloc((size_t)sw * sh, 1);
    unsigned char *nxt  = calloc((size_t)sw * sh, 1);
    unsigned char *seed = calloc((size_t)vw * vh, 1);
    if (!cur || !nxt || !seed) { free(cur); free(nxt); free(seed); return -1; }

    life_seed_pattern(seed, vw, vh, o->pattern, o->density);
    life_embed(cur, sw, sh, seed, vw, vh, M, M);

    tui_braille_t bc; tui_braille_init(&bc, vw, vh);
    const long budget = anim_frame_budget(o->gens);

    anim_term_t term; anim_begin(&term);
    long frames = 0, pop = 0, stale = 0, last_pop = -1;
    for (; budget < 0 || frames < budget; frames++) {
        if (g_anim_stop) break;

        tui_braille_clear(&bc);
        for (int y = 0; y < vh; y++)
            for (int x = 0; x < vw; x++)
                if (cur[(y + M) * sw + (x + M)]) tui_braille_set(&bc, x, y);

        char hdr[160];
        snprintf(hdr, sizeof hdr, "gen %-6ld  pop %-6ld  %dx%d cells  %s%s  ^C to stop",
                 frames, pop, vw, vh, o->pattern ? o->pattern : "random",
                 o->wrap ? "" : " ∞");
        anim_header(o->title ? o->title : "Conway's Game of Life", hdr);
        anim_blit(&bc, o->color);
        fflush(stdout);

        pop = life_step(cur, nxt, sw, sh, o->wrap);
        if (!o->wrap) life_absorb_border(nxt, sw, sh);
        unsigned char *tmp = cur; cur = nxt; nxt = tmp;

        /* auto-reseed on extinction or a long stable/oscillating plateau so an
         * unattended run keeps producing motion instead of freezing. */
        if (pop == last_pop) stale++; else stale = 0;
        last_pop = pop;
        if (pop == 0 || stale > 220) {
            life_seed_pattern(seed, vw, vh, o->pattern, o->density);
            life_embed(cur, sw, sh, seed, vw, vh, M, M);
            stale = 0; last_pop = -1;
        }
        if (!anim_tick(o->fps)) break;
    }
    anim_end(&term);

    tui_braille_free(&bc);
    free(cur); free(nxt); free(seed);
    return (int)frames;
}

/* ════════════════════════════════════════════════════════════════════════
 * Elementary 1-D cellular automaton (Wolfram rule N), scrolling upward
 * ════════════════════════════════════════════════════════════════════════ */
int anim_rule(const anim_opts_t *o) {
    int W, H; anim_size(o, &W, &H);
    const int gw = W * 2, gh = H * 4;
    xs_seed(o->seed ? o->seed : (uint32_t)(time(NULL) ^ (long)getpid()));
    const int rule = o->rule & 0xFF;

    /* history buffer, newest row at the bottom (index gh-1) */
    unsigned char *hist = calloc((size_t)gw * gh, 1);
    unsigned char *row  = calloc((size_t)gw, 1);
    if (!hist || !row) { free(hist); free(row); return -1; }
    row[gw / 2] = 1;                              /* single seed cell */

    tui_braille_t bc; tui_braille_init(&bc, gw, gh);
    const long budget = anim_frame_budget(o->gens);

    anim_term_t term; anim_begin(&term);
    long frames = 0;
    for (; budget < 0 || frames < budget; frames++) {
        if (g_anim_stop) break;

        /* scroll history up, drop new row in at the bottom */
        memmove(hist, hist + gw, (size_t)gw * (gh - 1));
        memcpy(hist + (size_t)gw * (gh - 1), row, (size_t)gw);

        tui_braille_clear(&bc);
        for (int y = 0; y < gh; y++)
            for (int x = 0; x < gw; x++)
                if (hist[y * gw + x]) tui_braille_set(&bc, x, y);

        char hdr[160];
        snprintf(hdr, sizeof hdr, "rule %-3d  gen %-6ld  %d wide  ^C to stop",
                 rule, frames, gw);
        anim_header(o->title ? o->title : "Elementary cellular automaton", hdr);
        anim_blit(&bc, o->color);
        fflush(stdout);

        /* compute next generation (toroidal edges) */
        unsigned char *nrow = calloc((size_t)gw, 1);
        if (!nrow) break;
        for (int x = 0; x < gw; x++) {
            int l = row[(x - 1 + gw) % gw], c = row[x], r = row[(x + 1) % gw];
            int idx = (l << 2) | (c << 1) | r;
            nrow[x] = (rule >> idx) & 1;
        }
        free(row); row = nrow;

        if (!anim_tick(o->fps)) break;
    }
    anim_end(&term);

    tui_braille_free(&bc);
    free(hist); free(row);
    return (int)frames;
}

/* ════════════════════════════════════════════════════════════════════════
 * Morphing De Jong attractor — params breathe sinusoidally over time
 * ════════════════════════════════════════════════════════════════════════ */
int anim_attractor(const anim_opts_t *o) {
    int W, H; anim_size(o, &W, &H);
    const int gw = W * 2, gh = H * 4;
    const long iters = 120000;                   /* per-frame trace budget */
    const long burn = 500;

    tui_braille_t bc; tui_braille_init(&bc, gw, gh);
    const long budget = anim_frame_budget(o->gens);

    anim_term_t term; anim_begin(&term);
    long frames = 0;
    for (; budget < 0 || frames < budget; frames++) {
        if (g_anim_stop) break;
        double ph = frames * 0.035;
        double a = -2.0 + 0.6 * sin(ph),       b = -2.0 + 0.6 * cos(ph * 0.9);
        double c = -1.2 + 0.5 * sin(ph * 1.3),  d =  2.0 + 0.4 * cos(ph * 0.7);

        /* pass 1: bounding box */
        double x = 0.1, y = 0.1, xmn = 1e30, xmx = -1e30, ymn = 1e30, ymx = -1e30;
        for (long i = 0; i < iters; i++) {
            double nx = sin(a * y) - cos(b * x), ny = sin(c * x) - cos(d * y);
            x = nx; y = ny;
            if (i < burn) continue;
            if (x < xmn) xmn = x; if (x > xmx) xmx = x;
            if (y < ymn) ymn = y; if (y > ymx) ymx = y;
        }
        double xr = xmx - xmn, yr = ymx - ymn;
        if (xr < 1e-9) xr = 1; if (yr < 1e-9) yr = 1;
        xmn -= xr * 0.03; ymn -= yr * 0.03; xr *= 1.06; yr *= 1.06;

        /* pass 2: trace dots */
        tui_braille_clear(&bc);
        x = 0.1; y = 0.1;
        for (long i = 0; i < iters; i++) {
            double nx = sin(a * y) - cos(b * x), ny = sin(c * x) - cos(d * y);
            x = nx; y = ny;
            if (i < burn) continue;
            int px = (int)((x - xmn) / xr * (gw - 1) + 0.5);
            int py = (int)((1.0 - (y - ymn) / yr) * (gh - 1) + 0.5);
            if (px >= 0 && px < gw && py >= 0 && py < gh) tui_braille_set(&bc, px, py);
        }

        char hdr[160];
        snprintf(hdr, sizeof hdr, "a=%+.2f b=%+.2f c=%+.2f d=%+.2f  frame %-5ld  ^C to stop",
                 a, b, c, d, frames);
        anim_header(o->title ? o->title : "De Jong attractor (morphing)", hdr);
        anim_blit(&bc, o->color);
        fflush(stdout);
        if (!anim_tick(o->fps)) break;
    }
    anim_end(&term);

    tui_braille_free(&bc);
    return (int)frames;
}

/* ════════════════════════════════════════════════════════════════════════
 * "Digital rain" — falling glyph columns with fading green trails
 * ════════════════════════════════════════════════════════════════════════ */
int anim_rain(const anim_opts_t *o) {
    int W, H; anim_size(o, &W, &H);
    xs_seed(o->seed ? o->seed : (uint32_t)(time(NULL) ^ (long)getpid()));

    /* per-column drop head (row, fractional), speed, and trail length */
    double *head = malloc(sizeof(double) * W);
    double *spd  = malloc(sizeof(double) * W);
    int    *len  = malloc(sizeof(int) * W);
    char   *cells = malloc((size_t)W * H);       /* persistent glyph per cell */
    if (!head || !spd || !len || !cells) { free(head); free(spd); free(len); free(cells); return -1; }

    static const char glyphs[] = "01234567890ABCDEFGHJKLMNPQRZ#%&@$<>*+=-/\\|";
    const int ng = (int)(sizeof glyphs - 1);
    for (int c = 0; c < W; c++) {
        head[c] = -(xs_unit() * H);
        spd[c]  = 0.3 + xs_unit() * 0.9;
        len[c]  = 4 + (int)(xs_unit() * (H * 0.6));
    }
    for (int i = 0; i < W * H; i++) cells[i] = glyphs[xs_next() % ng];

    const long budget = anim_frame_budget(o->gens);
    anim_term_t term; anim_begin(&term);
    long frames = 0;
    for (; budget < 0 || frames < budget; frames++) {
        if (g_anim_stop) break;
        anim_header(o->title ? o->title : "Digital rain", "^C to stop");

        for (int r = 0; r < H; r++) {
            for (int c = 0; c < W; c++) {
                int h = (int)head[c];
                int dist = h - r;                /* 0 at head, grows up the trail */
                char g = cells[r * W + c];
                if (dist == 0 && h >= 0 && h < H) {
                    if (o->color) fputs("\033[38;5;231m", stdout);   /* bright head */
                    fputc(g, stdout);
                } else if (dist > 0 && dist < len[c]) {
                    double t = 1.0 - (double)dist / len[c];
                    if (o->color) {
                        int shade = 22 + (int)(t * 14);              /* greens 22..36 */
                        printf("\033[38;5;%dm", shade);
                    }
                    fputc(g, stdout);
                } else {
                    fputc(' ', stdout);
                }
            }
            if (o->color) fputs("\033[0m", stdout);
            fputs("\033[K\n", stdout);
        }
        fflush(stdout);

        /* advance drops; occasionally rescramble a glyph for shimmer */
        for (int c = 0; c < W; c++) {
            head[c] += spd[c];
            if (head[c] - len[c] > H) {           /* fully off-screen → respawn */
                head[c] = -(xs_unit() * H * 0.5);
                spd[c]  = 0.3 + xs_unit() * 0.9;
                len[c]  = 4 + (int)(xs_unit() * (H * 0.6));
            }
            if (xs_unit() < 0.15) {
                int r = xs_next() % H;
                cells[r * W + c] = glyphs[xs_next() % ng];
            }
        }
        if (!anim_tick(o->fps)) break;
    }
    anim_end(&term);

    free(head); free(spd); free(len); free(cells);
    return (int)frames;
}

/* ── JSON dispatch ───────────────────────────────────────────────────────── */
int anim_dispatch(const char *json) {
    anim_opts_t o = anim_opts_default();
    char *kind = json ? json_get_str(json, "kind") : NULL;
    char *pat  = json ? json_get_str(json, "pattern") : NULL;
    char *titl = json ? json_get_str(json, "title") : NULL;
    if (kind) o.kind = kind;
    if (pat)  o.pattern = pat;
    if (titl) o.title = titl;
    /* Emitter / unbounded-growth patterns get an open universe by default so
     * they don't wrap into themselves; an explicit "wrap" below still wins. */
    if (life_pattern_unbounded(o.pattern)) o.wrap = false;
    if (json) {
        o.width   = json_get_int(json, "width", o.width);
        o.height  = json_get_int(json, "height", o.height);
        o.gens    = json_get_int(json, "gens", (int)o.gens);
        o.fps     = json_get_int(json, "fps", o.fps);
        o.color   = json_get_bool(json, "color", o.color);
        o.wrap    = json_get_bool(json, "wrap", o.wrap);
        o.seed    = (unsigned)json_get_int(json, "seed", (int)o.seed);
        o.density = json_get_double(json, "density", o.density);
        o.rule    = json_get_int(json, "rule", o.rule);
    }

    int rc;
    const char *k = o.kind ? o.kind : "life";
    if      (!strcmp(k, "rule") || !strcmp(k, "ca"))            rc = anim_rule(&o);
    else if (!strcmp(k, "attractor") || !strcmp(k, "dejong"))  rc = anim_attractor(&o);
    else if (!strcmp(k, "rain") || !strcmp(k, "matrix"))       rc = anim_rain(&o);
    else if (fractal_is_kind(k))                               rc = fractal_anim(json);
    else                                                       rc = anim_life(&o);

    free(kind); free(pat); free(titl);
    return rc;
}
