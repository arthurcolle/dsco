#ifndef DSCO_TUI_SWARM_DOCK_H
#define DSCO_TUI_SWARM_DOCK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

/* Producers only update retained state; rendering belongs to the composer. */
void tui_swarm_dock_update(int id, const char *task, const char *model,
                           const char *status, size_t bytes, double cost);
void tui_swarm_dock_append(int id, const char *data, size_t len);
void tui_swarm_dock_reset(void);
int tui_swarm_dock_height(int cols, int rows, int reserved_rows);
void tui_swarm_dock_render(FILE *out, int top, int cols, int height);
/* Single terminal renderer: unchanged frames emit nothing; updates emit changed rows.
 * Invalidate after external clears, scrolls, or unmounts. Geometry changes force full paint. */
void tui_swarm_dock_render_retained(FILE *out, int top, int cols, int height);
void tui_swarm_dock_invalidate(void);
/* Monotonic seconds: start, 15-second active pulse, then one completion notice.
 * Hidden/idle docks stay quiet. Call on the composer thread. */
bool tui_swarm_dock_progress(double now, char *out, size_t cap);
bool tui_swarm_dock_changed(void);
void tui_swarm_dock_toggle_focus(void);
bool tui_swarm_dock_focused(void);
bool tui_swarm_dock_visible(void);
void tui_swarm_dock_show(bool visible);

enum tui_swarm_dock_key {
    TUI_SWARM_KEY_LEFT = 0x2000, TUI_SWARM_KEY_RIGHT,
    TUI_SWARM_KEY_UP, TUI_SWARM_KEY_DOWN, TUI_SWARM_KEY_TAB,
    TUI_SWARM_KEY_ESC, TUI_SWARM_KEY_ENTER, TUI_SWARM_KEY_Z,
    TUI_SWARM_KEY_R, TUI_SWARM_KEY_PAGEUP, TUI_SWARM_KEY_PAGEDOWN,
    TUI_SWARM_KEY_HOME, TUI_SWARM_KEY_END, TUI_SWARM_KEY_H, TUI_SWARM_KEY_X
};
enum tui_swarm_dock_mod {
    TUI_SWARM_MOD_SHIFT = 1, TUI_SWARM_MOD_ALT = 2, TUI_SWARM_MOD_CTRL = 4
};
bool tui_swarm_dock_key(int key, int modifiers);
/* SGR mouse button bits and 1-based terminal coordinates, including motion bit 32. */
bool tui_swarm_dock_mouse(int button, int col, int row, bool released);

#ifdef DSCO_TUI_SWARM_DOCK_TEST
typedef struct {
    int count, selected_id, x, y, width, height, page, page_size, scroll, view_count;
    bool focused, visible, zoomed, history_view, dismissed, capacity_limited;
    char status[48], tail[8192];
} tui_swarm_dock_snapshot_t;
bool tui_swarm_dock_snapshot(int id, tui_swarm_dock_snapshot_t *snapshot);
#endif

#endif
