#include "project_grid.h"
#include "crypto.h" /* fnv1a_32 — consolidated */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ──────────────────────────────────────────────────────────────────────────
 *  Tile pool allocation
 * ────────────────────────────────────────────────────────────────────────── */

static int alloc_tile(dsco_grid_t *g) {
    for (int i = 0; i < DSCO_GRID_MAX_TILES; i++) {
        if (!g->tiles[i].in_use) {
            dsco_tile_t *t = &g->tiles[i];
            memset(t, 0, sizeof(*t));
            t->kind = DSCO_TILE_LEAF;
            t->project_idx = -1;
            t->parent_id = -1;
            t->weight = 1.0f;
            t->in_use = true;
            return i;
        }
    }
    return -1;
}

static __attribute__((unused)) void free_tile_recursive(dsco_grid_t *g, int id) {
    if (id < 0 || id >= DSCO_GRID_MAX_TILES)
        return;
    dsco_tile_t *t = &g->tiles[id];
    if (!t->in_use)
        return;
    for (int i = 0; i < t->child_count; i++)
        free_tile_recursive(g, t->child_ids[i]);
    t->in_use = false;
    t->child_count = 0;
    t->project_idx = -1;
}

/* ──────────────────────────────────────────────────────────────────────────
 *  Color math — HSL → RGB, project identity hash, state colors
 * ────────────────────────────────────────────────────────────────────────── */

/* FNV-1a — consolidated into fnv1a_32() in crypto.h */
#define fnv1a(s) fnv1a_32(s)

static float clamp01(float x) {
    return x < 0 ? 0 : (x > 1 ? 1 : x);
}

dsco_rgb_t dsco_grid_hsl_to_rgb(float h_deg, float s, float l) {
    /* h in [0,360), s and l in [0,1] */
    s = clamp01(s);
    l = clamp01(l);
    float c = (1.0f - fabsf(2.0f * l - 1.0f)) * s;
    float hp = h_deg / 60.0f;
    float x = c * (1.0f - fabsf(fmodf(hp, 2.0f) - 1.0f));
    float r1 = 0, g1 = 0, b1 = 0;
    if (hp < 1) {
        r1 = c;
        g1 = x;
    } else if (hp < 2) {
        r1 = x;
        g1 = c;
    } else if (hp < 3) {
        g1 = c;
        b1 = x;
    } else if (hp < 4) {
        g1 = x;
        b1 = c;
    } else if (hp < 5) {
        r1 = x;
        b1 = c;
    } else {
        r1 = c;
        b1 = x;
    }
    float m = l - c * 0.5f;
    dsco_rgb_t out = {
        (uint8_t)roundf(clamp01(r1 + m) * 255.0f),
        (uint8_t)roundf(clamp01(g1 + m) * 255.0f),
        (uint8_t)roundf(clamp01(b1 + m) * 255.0f),
    };
    return out;
}

/* Per-theme HSL parameters keyed by theme — caller still passes hue from id hash */
static void theme_hsl(dsco_theme_t th, float hue_in, float *s, float *l, float *hue_out) {
    *hue_out = hue_in;
    switch (th) {
        case DSCO_THEME_CYBERPUNK:
            /* lock to neon hues: cyan / magenta / yellow / hot pink */
            {
                float bands[] = {180, 300, 60, 330};
                *hue_out = bands[((int)hue_in / 90) & 3];
            }
            *s = 0.95f;
            *l = 0.60f;
            break;
        case DSCO_THEME_PAPER:
            *s = 0.30f;
            *l = 0.55f;
            break;
        case DSCO_THEME_MONO:
            *hue_out = 0;
            *s = 0.0f;
            *l = 0.55f;
            break;
        case DSCO_THEME_TERMINAL:
        default:
            /* avoid the muddy yellow-green band */
            if (hue_in > 50 && hue_in < 90)
                *hue_out = hue_in + 50;
            *s = 0.62f;
            *l = 0.62f;
            break;
    }
}

/* Theme used when no grid is passed (e.g. tab strip) — keep a module global. */
static dsco_theme_t g_active_theme = DSCO_THEME_TERMINAL;

dsco_rgb_t dsco_grid_project_color(const char *project_id) {
    if (!project_id || !*project_id) {
        dsco_rgb_t grey = {128, 128, 128};
        return grey;
    }
    uint32_t h = fnv1a(project_id);
    float hue_in = (float)(h % 360);
    float s, l, hue_out;
    theme_hsl(g_active_theme, hue_in, &s, &l, &hue_out);
    return dsco_grid_hsl_to_rgb(hue_out, s, l);
}

void dsco_grid_cycle_theme(dsco_grid_t *g) {
    g->theme = (dsco_theme_t)((g->theme + 1) % DSCO_THEME_COUNT);
    g_active_theme = g->theme;
}

dsco_rgb_t dsco_grid_state_color(int state) {
    /* keep in sync with dsco_project_state_t */
    switch (state) {
        case 0: {
            dsco_rgb_t c = {120, 120, 130};
            return c;
        } /* IDLE — slate */
        case 1: {
            dsco_rgb_t c = {72, 200, 130};
            return c;
        } /* RUNNING — green */
        case 2: {
            dsco_rgb_t c = {235, 190, 80};
            return c;
        } /* PAUSED — amber */
        case 3: {
            dsco_rgb_t c = {235, 80, 90};
            return c;
        } /* QUARANTINED — red */
        case 4: {
            dsco_rgb_t c = {90, 90, 100};
            return c;
        } /* CLOSED — dim */
        case 5: {
            dsco_rgb_t c = {210, 70, 70};
            return c;
        } /* DEAD — dark red */
    }
    dsco_rgb_t fallback = {180, 180, 180};
    return fallback;
}

dsco_rgb_t dsco_grid_lerp(dsco_rgb_t a, dsco_rgb_t b, float t) {
    t = clamp01(t);
    dsco_rgb_t out = {
        (uint8_t)(a.r + (b.r - a.r) * t),
        (uint8_t)(a.g + (b.g - a.g) * t),
        (uint8_t)(a.b + (b.b - a.b) * t),
    };
    return out;
}

/* ──────────────────────────────────────────────────────────────────────────
 *  Border glyphs
 * ────────────────────────────────────────────────────────────────────────── */

dsco_border_glyphs_t dsco_grid_border_glyphs(dsco_border_t b) {
    dsco_border_glyphs_t g;
    switch (b) {
        case DSCO_BORDER_SINGLE:
            g.h = "─";
            g.v = "│";
            g.tl = "┌";
            g.tr = "┐";
            g.bl = "└";
            g.br = "┘";
            break;
        case DSCO_BORDER_DOUBLE:
            g.h = "═";
            g.v = "║";
            g.tl = "╔";
            g.tr = "╗";
            g.bl = "╚";
            g.br = "╝";
            break;
        case DSCO_BORDER_HEAVY:
            g.h = "━";
            g.v = "┃";
            g.tl = "┏";
            g.tr = "┓";
            g.bl = "┗";
            g.br = "┛";
            break;
        case DSCO_BORDER_DASHED:
            g.h = "╌";
            g.v = "╎";
            g.tl = "┌";
            g.tr = "┐";
            g.bl = "└";
            g.br = "┘";
            break;
        case DSCO_BORDER_NONE:
        default:
            g.h = " ";
            g.v = " ";
            g.tl = " ";
            g.tr = " ";
            g.bl = " ";
            g.br = " ";
            break;
    }
    return g;
}

/* ──────────────────────────────────────────────────────────────────────────
 *  Lifecycle / presets
 * ────────────────────────────────────────────────────────────────────────── */

void dsco_grid_init(dsco_grid_t *g) {
    memset(g, 0, sizeof(*g));
    g->root_id = -1;
    g->focused_id = -1;
    g->zoomed_id = -1;
    g->preset_idx = 0;
    g->color_mode = DSCO_COLOR_MODE_IDENTITY;
    g->border = DSCO_BORDER_SINGLE;
    g->theme = DSCO_THEME_TERMINAL;
    g_active_theme = g->theme;
    g->truecolor = true;
    dsco_grid_set_preset(g, 1, 1);
}

void dsco_grid_set_preset(dsco_grid_t *g, int rows, int cols) {
    if (rows < 1)
        rows = 1;
    if (cols < 1)
        cols = 1;
    /* free everything */
    for (int i = 0; i < DSCO_GRID_MAX_TILES; i++)
        g->tiles[i].in_use = false;
    g->root_id = -1;
    g->focused_id = -1;
    g->zoomed_id = -1;

    if (rows == 1 && cols == 1) {
        int leaf = alloc_tile(g);
        g->root_id = leaf;
        g->focused_id = leaf;
        return;
    }

    /* Build: VSPLIT of `cols` columns, each column an HSPLIT of `rows` rows.
     * (rows=1 → just VSPLIT; cols=1 → just HSPLIT.) */
    int root;
    if (cols == 1) {
        root = alloc_tile(g);
        g->tiles[root].kind = DSCO_TILE_HSPLIT;
        g->tiles[root].project_idx = -1;
        for (int r = 0; r < rows && g->tiles[root].child_count < DSCO_TILE_MAX_CHILDREN; r++) {
            int leaf = alloc_tile(g);
            g->tiles[leaf].parent_id = root;
            g->tiles[root].child_ids[g->tiles[root].child_count++] = leaf;
        }
    } else if (rows == 1) {
        root = alloc_tile(g);
        g->tiles[root].kind = DSCO_TILE_VSPLIT;
        g->tiles[root].project_idx = -1;
        for (int c = 0; c < cols && g->tiles[root].child_count < DSCO_TILE_MAX_CHILDREN; c++) {
            int leaf = alloc_tile(g);
            g->tiles[leaf].parent_id = root;
            g->tiles[root].child_ids[g->tiles[root].child_count++] = leaf;
        }
    } else {
        root = alloc_tile(g);
        g->tiles[root].kind = DSCO_TILE_VSPLIT;
        for (int c = 0; c < cols && g->tiles[root].child_count < DSCO_TILE_MAX_CHILDREN; c++) {
            int col = alloc_tile(g);
            g->tiles[col].parent_id = root;
            g->tiles[col].kind = DSCO_TILE_HSPLIT;
            for (int r = 0; r < rows && g->tiles[col].child_count < DSCO_TILE_MAX_CHILDREN; r++) {
                int leaf = alloc_tile(g);
                g->tiles[leaf].parent_id = col;
                g->tiles[col].child_ids[g->tiles[col].child_count++] = leaf;
            }
            g->tiles[root].child_ids[g->tiles[root].child_count++] = col;
        }
    }
    g->root_id = root;
    /* focus first leaf */
    for (int i = 0; i < DSCO_GRID_MAX_TILES; i++) {
        if (g->tiles[i].in_use && g->tiles[i].kind == DSCO_TILE_LEAF) {
            g->focused_id = i;
            break;
        }
    }
}

static const int k_preset_table[][2] = {
    {1, 1}, {1, 2}, {2, 2}, {2, 3}, {3, 3}, {3, 4}, {4, 4},
};
#define K_PRESET_COUNT ((int)(sizeof(k_preset_table) / sizeof(k_preset_table[0])))

void dsco_grid_cycle_preset(dsco_grid_t *g) {
    g->preset_idx = (g->preset_idx + 1) % K_PRESET_COUNT;
    dsco_grid_set_preset(g, k_preset_table[g->preset_idx][0], k_preset_table[g->preset_idx][1]);
}

/* ──────────────────────────────────────────────────────────────────────────
 *  Leaf iteration + project assignment
 * ────────────────────────────────────────────────────────────────────────── */

static void leaves_dfs(const dsco_grid_t *g, int id, int *leaves, int *count, int cap) {
    if (id < 0 || id >= DSCO_GRID_MAX_TILES || *count >= cap)
        return;
    const dsco_tile_t *t = &g->tiles[id];
    if (!t->in_use)
        return;
    if (t->kind == DSCO_TILE_LEAF) {
        leaves[(*count)++] = id;
        return;
    }
    for (int i = 0; i < t->child_count; i++)
        leaves_dfs(g, t->child_ids[i], leaves, count, cap);
}

void dsco_grid_assign_projects(dsco_grid_t *g, int project_count) {
    int leaves[DSCO_GRID_MAX_TILES];
    int n = 0;
    leaves_dfs(g, g->root_id, leaves, &n, DSCO_GRID_MAX_TILES);
    for (int i = 0; i < n; i++) {
        g->tiles[leaves[i]].project_idx = (i < project_count) ? i : -1;
    }
    if (g->focused_id < 0 && n > 0)
        g->focused_id = leaves[0];
}

void dsco_grid_foreach_leaf(const dsco_grid_t *g, dsco_grid_leaf_cb cb, void *ctx) {
    int leaves[DSCO_GRID_MAX_TILES];
    int n = 0;
    leaves_dfs(g, g->root_id, leaves, &n, DSCO_GRID_MAX_TILES);
    for (int i = 0; i < n; i++)
        cb(&g->tiles[leaves[i]], ctx);
}

int dsco_grid_find_leaf(const dsco_grid_t *g, int project_idx) {
    int leaves[DSCO_GRID_MAX_TILES];
    int n = 0;
    leaves_dfs(g, g->root_id, leaves, &n, DSCO_GRID_MAX_TILES);
    for (int i = 0; i < n; i++) {
        if (g->tiles[leaves[i]].project_idx == project_idx)
            return leaves[i];
    }
    return -1;
}

void dsco_grid_focus(dsco_grid_t *g, int tile_id) {
    if (tile_id < 0 || tile_id >= DSCO_GRID_MAX_TILES)
        return;
    if (!g->tiles[tile_id].in_use)
        return;
    if (g->tiles[tile_id].kind != DSCO_TILE_LEAF)
        return;
    g->focused_id = tile_id;
}

void dsco_grid_focus_project(dsco_grid_t *g, int project_idx) {
    int tid = dsco_grid_find_leaf(g, project_idx);
    if (tid >= 0)
        g->focused_id = tid;
}

/* ──────────────────────────────────────────────────────────────────────────
 *  Splits & zoom
 * ────────────────────────────────────────────────────────────────────────── */

int dsco_grid_split(dsco_grid_t *g, int tile_id, dsco_tile_kind_t dir) {
    if (tile_id < 0 || tile_id >= DSCO_GRID_MAX_TILES)
        return -1;
    dsco_tile_t *t = &g->tiles[tile_id];
    if (!t->in_use || t->kind != DSCO_TILE_LEAF)
        return -1;
    if (dir != DSCO_TILE_HSPLIT && dir != DSCO_TILE_VSPLIT)
        return -1;

    /* convert this leaf into a split with two children: itself (renamed) +
     * a fresh empty leaf. We do this by allocating a NEW leaf to inherit
     * the project, then transforming `t` into a split with both kids. */
    int new_leaf = alloc_tile(g);
    int new_sibling = alloc_tile(g);
    if (new_leaf < 0 || new_sibling < 0)
        return -1;

    g->tiles[new_leaf].parent_id = tile_id;
    g->tiles[new_leaf].project_idx = t->project_idx;
    g->tiles[new_sibling].parent_id = tile_id;

    t->kind = dir;
    t->project_idx = -1;
    t->child_count = 2;
    t->child_ids[0] = new_leaf;
    t->child_ids[1] = new_sibling;

    g->focused_id = new_sibling;
    return new_sibling;
}

void dsco_grid_zoom_toggle(dsco_grid_t *g) {
    if (g->zoomed_id >= 0) {
        g->zoomed_id = -1;
        return;
    }
    if (g->focused_id >= 0 && g->tiles[g->focused_id].in_use &&
        g->tiles[g->focused_id].kind == DSCO_TILE_LEAF) {
        g->zoomed_id = g->focused_id;
    }
}

/* ──────────────────────────────────────────────────────────────────────────
 *  Directional focus (vim h/j/k/l) — uses cached rects from last render
 * ────────────────────────────────────────────────────────────────────────── */

void dsco_grid_focus_dir(dsco_grid_t *g, int dx, int dy) {
    if (g->focused_id < 0)
        return;
    const dsco_tile_t *cur = &g->tiles[g->focused_id];
    int cr = cur->rect.row0 + cur->rect.rows / 2;
    int cc = cur->rect.col0 + cur->rect.cols / 2;

    int best_id = -1;
    long best_score = INT64_MAX;
    for (int i = 0; i < DSCO_GRID_MAX_TILES; i++) {
        if (!g->tiles[i].in_use || g->tiles[i].kind != DSCO_TILE_LEAF)
            continue;
        if (i == g->focused_id)
            continue;
        const dsco_tile_t *t = &g->tiles[i];
        int tr = t->rect.row0 + t->rect.rows / 2;
        int tc = t->rect.col0 + t->rect.cols / 2;
        int dr = tr - cr;
        int dc = tc - cc;
        /* must be in the requested direction */
        if (dx > 0 && dc <= 0)
            continue;
        if (dx < 0 && dc >= 0)
            continue;
        if (dy > 0 && dr <= 0)
            continue;
        if (dy < 0 && dr >= 0)
            continue;
        /* manhattan-ish, biased toward the requested axis */
        long score;
        if (dx != 0)
            score = (long)labs(dc) * 1 + (long)labs(dr) * 4;
        else
            score = (long)labs(dr) * 1 + (long)labs(dc) * 4;
        if (score < best_score) {
            best_score = score;
            best_id = i;
        }
    }
    if (best_id >= 0)
        g->focused_id = best_id;
}

/* ──────────────────────────────────────────────────────────────────────────
 *  Toggles
 * ────────────────────────────────────────────────────────────────────────── */

void dsco_grid_cycle_color_mode(dsco_grid_t *g) {
    g->color_mode = (dsco_color_mode_t)((g->color_mode + 1) % DSCO_COLOR_MODE_COUNT);
}

void dsco_grid_cycle_border(dsco_grid_t *g) {
    g->border = (dsco_border_t)((g->border + 1) % DSCO_BORDER_COUNT);
}

/* ──────────────────────────────────────────────────────────────────────────
 *  Layout — recursively assign rects to every tile
 * ────────────────────────────────────────────────────────────────────────── */

static void layout_recursive(dsco_grid_t *g, int id, int r0, int c0, int rows, int cols);

void dsco_grid_layout(dsco_grid_t *g, int row0, int col0, int rows, int cols) {
    if (g->zoomed_id >= 0 && g->tiles[g->zoomed_id].in_use) {
        /* zoomed: only the focused leaf is visible at full size */
        dsco_tile_t *t = &g->tiles[g->zoomed_id];
        t->rect.row0 = row0;
        t->rect.col0 = col0;
        t->rect.rows = rows;
        t->rect.cols = cols;
        return;
    }
    layout_recursive(g, g->root_id, row0, col0, rows, cols);
}

static void layout_recursive(dsco_grid_t *g, int id, int r0, int c0, int rows, int cols) {
    if (id < 0 || id >= DSCO_GRID_MAX_TILES)
        return;
    dsco_tile_t *t = &g->tiles[id];
    if (!t->in_use)
        return;
    t->rect.row0 = r0;
    t->rect.col0 = c0;
    t->rect.rows = rows;
    t->rect.cols = cols;
    if (t->kind == DSCO_TILE_LEAF || t->child_count == 0)
        return;

    float total_w = 0.0f;
    for (int i = 0; i < t->child_count; i++) {
        float w = g->tiles[t->child_ids[i]].weight;
        if (w <= 0)
            w = 1.0f;
        total_w += w;
    }
    if (total_w == 0.0f)
        total_w = (float)t->child_count;

    int used = 0;
    for (int i = 0; i < t->child_count; i++) {
        dsco_tile_t *c = &g->tiles[t->child_ids[i]];
        float w = c->weight > 0 ? c->weight : 1.0f;
        if (t->kind == DSCO_TILE_VSPLIT) {
            int cw = (i == t->child_count - 1) ? cols - used : (int)((float)cols * w / total_w);
            layout_recursive(g, t->child_ids[i], r0, c0 + used, rows, cw);
            used += cw;
        } else { /* HSPLIT */
            int ch = (i == t->child_count - 1) ? rows - used : (int)((float)rows * w / total_w);
            layout_recursive(g, t->child_ids[i], r0 + used, c0, ch, cols);
            used += ch;
        }
    }
}
