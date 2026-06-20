#ifndef DSCO_PROJECT_GRID_H
#define DSCO_PROJECT_GRID_H

#include <stdbool.h>
#include <stdint.h>

/* ──────────────────────────────────────────────────────────────────────────
 *  Deep grid — recursive tile tree, toggleable color modes.
 *
 *  Replaces the old fixed-layout enum (tabs / vsplit / hsplit / 2x2 grid)
 *  with a tmux-style tile tree that supports arbitrary nesting plus a
 *  cycle of presets (1x1 → 1x2 → 2x2 → 2x3 → 3x3 → 4x4).
 *
 *  Toggles:
 *    - zoom (focus one tile full-screen, restore on unzoom)
 *    - color mode (off / state / identity-hue / heatmap / full)
 *    - border style (none / single / double / heavy unicode)
 * ────────────────────────────────────────────────────────────────────────── */

#define DSCO_GRID_MAX_TILES     128
#define DSCO_TILE_MAX_CHILDREN  8

typedef enum {
    DSCO_TILE_LEAF = 0,     /* shows a project */
    DSCO_TILE_HSPLIT,       /* children stacked top→bottom */
    DSCO_TILE_VSPLIT,       /* children laid left→right */
} dsco_tile_kind_t;

typedef enum {
    DSCO_COLOR_MODE_OFF = 0,
    DSCO_COLOR_MODE_STATE,      /* border tinted by run state */
    DSCO_COLOR_MODE_IDENTITY,   /* border tinted by stable per-project hue */
    DSCO_COLOR_MODE_HEATMAP,    /* background tinted by activity rate */
    DSCO_COLOR_MODE_FULL,       /* identity border + heatmap background + state badge */
    DSCO_COLOR_MODE_COUNT,
} dsco_color_mode_t;

typedef enum {
    DSCO_BORDER_NONE = 0,
    DSCO_BORDER_SINGLE,         /* ─ │ ┌ ┐ └ ┘ */
    DSCO_BORDER_DOUBLE,         /* ═ ║ ╔ ╗ ╚ ╝ */
    DSCO_BORDER_HEAVY,          /* ━ ┃ ┏ ┓ ┗ ┛ */
    DSCO_BORDER_DASHED,         /* ╌ ╎ ┌ ┐ └ ┘ */
    DSCO_BORDER_COUNT,
} dsco_border_t;

typedef enum {
    DSCO_THEME_TERMINAL = 0,    /* default — natural identity hues */
    DSCO_THEME_CYBERPUNK,       /* high-sat neon, dark base */
    DSCO_THEME_PAPER,           /* desaturated, light-on-dark grey */
    DSCO_THEME_MONO,            /* greyscale only */
    DSCO_THEME_COUNT,
} dsco_theme_t;

typedef struct {
    int    tile_id;             /* parent tile being entered (filled by layout) */
    int    row0, col0;          /* absolute screen coordinates */
    int    rows, cols;
} dsco_tile_rect_t;

typedef struct {
    dsco_tile_kind_t kind;
    int     project_idx;        /* valid iff kind == LEAF; -1 otherwise */
    int     parent_id;          /* -1 for root */
    int     child_ids[DSCO_TILE_MAX_CHILDREN];
    int     child_count;
    float   weight;             /* proportional size in parent (default 1.0) */
    bool    in_use;
    /* layout cache — populated by render layout pass */
    dsco_tile_rect_t rect;
} dsco_tile_t;

typedef struct {
    dsco_tile_t      tiles[DSCO_GRID_MAX_TILES];
    int              root_id;
    int              focused_id;
    int              zoomed_id;         /* -1 if not zoomed */
    int              preset_idx;        /* current grid-preset cycle position */

    dsco_color_mode_t color_mode;
    dsco_border_t     border;
    dsco_theme_t      theme;
    bool              truecolor;        /* emit 24-bit color escapes */
} dsco_grid_t;

/* ── RGB color value used by render helpers ──────────────────────────── */
typedef struct { uint8_t r, g, b; } dsco_rgb_t;

/* ── Lifecycle ─────────────────────────────────────────────────────────── */
void dsco_grid_init(dsco_grid_t *g);

/* Reset the grid to a fresh `rows`×`cols` arrangement of empty leaves.
 * After this call there are `rows*cols` leaves; populate by setting
 * `project_idx` on each leaf via `dsco_grid_assign_projects`. */
void dsco_grid_set_preset(dsco_grid_t *g, int rows, int cols);

/* Cycle through 1x1, 1x2, 2x2, 2x3, 3x3, 4x4. */
void dsco_grid_cycle_preset(dsco_grid_t *g);

/* Bind the current grid's leaves to project indices in `[0, project_count)`.
 * If there are more leaves than projects, extra leaves get project_idx = -1
 * (rendered as an empty placeholder). */
void dsco_grid_assign_projects(dsco_grid_t *g, int project_count);

/* ── Manipulation ──────────────────────────────────────────────────────── */
int  dsco_grid_split(dsco_grid_t *g, int tile_id, dsco_tile_kind_t dir);
void dsco_grid_zoom_toggle(dsco_grid_t *g);
void dsco_grid_focus(dsco_grid_t *g, int tile_id);

/* Move focus in a direction (dx, dy) — +1/-1 each. Uses cached rects from
 * the last render to find the nearest tile in that direction. */
void dsco_grid_focus_dir(dsco_grid_t *g, int dx, int dy);

/* Focus a tile by its project index. -1 to clear. */
void dsco_grid_focus_project(dsco_grid_t *g, int project_idx);

/* Return the leaf tile_id currently showing `project_idx`, or -1. */
int  dsco_grid_find_leaf(const dsco_grid_t *g, int project_idx);

/* Iterate leaves in document order (depth-first). Calls cb for each leaf. */
typedef void (*dsco_grid_leaf_cb)(const dsco_tile_t *t, void *ctx);
void dsco_grid_foreach_leaf(const dsco_grid_t *g, dsco_grid_leaf_cb cb, void *ctx);

/* ── Visual toggles ────────────────────────────────────────────────────── */
void dsco_grid_cycle_color_mode(dsco_grid_t *g);
void dsco_grid_cycle_border(dsco_grid_t *g);
void dsco_grid_cycle_theme(dsco_grid_t *g);

/* ── Color helpers ─────────────────────────────────────────────────────── */
dsco_rgb_t dsco_grid_project_color(const char *project_id);
dsco_rgb_t dsco_grid_state_color(int state);
dsco_rgb_t dsco_grid_hsl_to_rgb(float h_deg, float s, float l);
dsco_rgb_t dsco_grid_lerp(dsco_rgb_t a, dsco_rgb_t b, float t);

/* ── Layout pass — populates each tile's rect. ──────────────────────────── */
void dsco_grid_layout(dsco_grid_t *g, int row0, int col0, int rows, int cols);

/* ── Border glyph access — returns 6 strings for {h, v, tl, tr, bl, br} ── */
typedef struct {
    const char *h;
    const char *v;
    const char *tl;
    const char *tr;
    const char *bl;
    const char *br;
} dsco_border_glyphs_t;
dsco_border_glyphs_t dsco_grid_border_glyphs(dsco_border_t b);

#endif /* DSCO_PROJECT_GRID_H */
