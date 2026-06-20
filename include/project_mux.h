#ifndef DSCO_PROJECT_MUX_H
#define DSCO_PROJECT_MUX_H

#include "project.h"
#include "project_grid.h"

/* ──────────────────────────────────────────────────────────────────────────
 *  Multiplexer — runs many Projects in parallel, one TUI screen.
 *
 *  Layout is a recursive tile tree (`dsco_grid_t`); the old fixed enum is
 *  gone. Visual cues are layered: identity-hue borders, state badges,
 *  activity heatmap. All toggleable at runtime.
 *
 *  Keys (prefix = Ctrl+B by default):
 *    c       new project           x   close active
 *    1..9    focus by tab number   n/p next/prev project
 *    h/j/k/l navigate cells (vim)  z   zoom focused cell
 *    s       split horizontal      v   split vertical
 *    G       cycle grid preset (1×1 → 1×2 → 2×2 → 2×3 → 3×3 → 3×4 → 4×4)
 *    C       cycle color mode (off / state / identity / heatmap / full)
 *    B       cycle border style (none / single / double / heavy / dashed)
 *    d       detach                ?   help
 * ────────────────────────────────────────────────────────────────────────── */

typedef struct dsco_mux {
    dsco_project_t  *projects[DSCO_PROJECT_MAX];
    int              count;
    int              focused;      /* index into projects[] — mirrors grid focus */

    dsco_grid_t      grid;
    bool             running;
    bool             needs_redraw;

    /* terminal state */
    int              term_rows;
    int              term_cols;
    char             prefix_pending;   /* != 0 while waiting for second key after Ctrl+B */

    /* per-project drain threads write a byte here to wake the render loop;
     * the main loop polls this fd + stdin only. */
    int              wake_fd_r;
    int              wake_fd_w;

    /* api key forwarded to workers */
    char             api_key[256];
} dsco_mux_t;

/* Top-level entry. Boots the multiplexer with an initial project rooted at
 * `initial_root` (or NULL to start empty). Returns when user detaches or all
 * projects exit. */
int dsco_mux_run(const char *api_key, const char *initial_root);

/* Implementation surfaces — also called from project.c via weak symbols. */
int dsco_mux_spawn_worker(dsco_project_t *p, const char *api_key);
int dsco_mux_kill_worker(dsco_project_t *p);

#endif /* DSCO_PROJECT_MUX_H */
