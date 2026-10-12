#define _XOPEN_SOURCE 700
#include "tui_swarm_dock.h"

#include <limits.h>
#include <locale.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define DOCK_WORKERS 64
#define DOCK_HISTORY 8192
#define DOCK_MAX_COLS 1024
#define DOCK_MAX_ROWS 256

typedef struct {
    int id, x, y, w, h, scroll;
    unsigned long raised;
    bool placed, manual, dismissed;
    char task[256], model[96], status[48], history[DOCK_HISTORY];
    size_t history_len, bytes;
    double cost;
    unsigned char escape, string_utf8, utf8[4], utf8_len, utf8_need;
} dock_worker_t;

typedef struct {
    dock_worker_t workers[DOCK_WORKERS];
    int count, selected, cols, height, top, page_size;
    int view[DOCK_WORKERS], view_count;
    bool history_view, capacity_limited;
    bool visible, hidden_by_user, focused, dirty, paint_dirty, zoom;
    int drag, drag_kind, drag_col, drag_row, drag_x, drag_y, drag_w, drag_h;
    unsigned long serial, output_serial, progress_output;
    double progress_at, progress_until;
    char progress_notice[384];
    bool progress_armed;
} dock_state_t;

static dock_state_t dock = {.selected = -1, .drag = -1, .page_size = 1};
static void invalidate_frame(void);
static pthread_mutex_t dock_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t locale_once = PTHREAD_ONCE_INIT;
static locale_t utf8_locale;
static void init_locale(void) {
    utf8_locale = newlocale(LC_CTYPE_MASK, "en_US.UTF-8", (locale_t)0);
    if (!utf8_locale) utf8_locale = newlocale(LC_CTYPE_MASK, "C.UTF-8", (locale_t)0);
}
static int min_i(int a, int b) { return a < b ? a : b; }
static int max_i(int a, int b) { return a > b ? a : b; }
static int clamp_i(int n, int lo, int hi) { return max_i(lo, min_i(n, hi)); }

static void history_push(dock_worker_t *w, const char *p, size_t n) {
    if (!n) return;
    if (n >= sizeof(w->history)) { p += n - sizeof(w->history) + 1; n = sizeof(w->history) - 1; }
    if (w->history_len + n >= sizeof(w->history)) {
        size_t discard = w->history_len + n - sizeof(w->history) + 1;
        /* Drop a block rather than shifting 8 KiB for each arriving byte. */
        if (discard < sizeof(w->history) / 4)
            discard = (size_t)min_i((int)w->history_len, (int)sizeof(w->history) / 4);
        /* Never leave a partial UTF-8 sequence at the beginning. */
        while (discard < w->history_len && ((unsigned char)w->history[discard] & 0xc0) == 0x80) discard++;
        memmove(w->history, w->history + discard, w->history_len - discard);
        w->history_len -= discard;
    }
    memcpy(w->history + w->history_len, p, n);
    w->history_len += n;
    w->history[w->history_len] = '\0';
}

/* Incremental parser: escape strings and split CSI/OSC never reach the terminal.
 * State is per worker, so interleaved streams cannot complete one another's escapes. */
static void sanitized_append(dock_worker_t *w, const char *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)data[i];
        if (w->escape) {
            /* A C1 ST byte embedded in UTF-8 inside OSC/DCS is not a terminator. */
            if (w->escape == 3 || w->escape == 4) {
                if (w->string_utf8 && (c & 0xc0) == 0x80) { w->string_utf8--; continue; }
                w->string_utf8 = 0;
                if (c >= 0xc2 && c <= 0xf4) {
                    w->string_utf8 = c < 0xe0 ? 1 : c < 0xf0 ? 2 : 3;
                    continue;
                }
            }
            if (w->escape == 1) {
                if (c == '[') w->escape = 2;
                else if (c == ']') w->escape = 3;
                else if (c == 'P' || c == '^' || c == '_' || c == 'X') w->escape = 4;
                else if (c >= 0x20 && c <= 0x2f) w->escape = 5;
                else w->escape = c == 0x1b ? 1 : 0;
            } else if (w->escape == 2) {
                if (c >= 0x40 && c <= 0x7e) w->escape = 0;
                else if (c == 0x1b) w->escape = 1;
            } else if (w->escape == 5) {
                if (c >= 0x30 && c <= 0x7e) w->escape = 0;
                else if (c == 0x1b) w->escape = 1;
            } else if (w->escape == 6 || w->escape == 7) {
                if (c == '\\' || c == 0x9c || (w->escape == 6 && c == 7)) w->escape = 0;
                else if (c != 0x1b) {
                    w->escape -= 3;
                    if (c >= 0xc2 && c <= 0xf4) w->string_utf8 = c < 0xe0 ? 1 : c < 0xf0 ? 2 : 3;
                }
            } else if (c == 0x9c || (w->escape == 3 && c == 7)) w->escape = 0;
            else if (c == 0x1b) w->escape += 3;
            continue;
        }
        if (w->utf8_need) {
            if ((c & 0xc0) == 0x80) {
                w->utf8[w->utf8_len++] = c;
                if (w->utf8_len == w->utf8_need) {
                    uint32_t cp = w->utf8[0] & (0x7f >> w->utf8_need);
                    for (unsigned j = 1; j < w->utf8_need; j++) cp = (cp << 6) | (w->utf8[j] & 0x3f);
                    bool valid = (w->utf8_need == 2 ? cp >= 0x80 : w->utf8_need == 3 ? cp >= 0x800 : cp >= 0x10000)
                                 && cp <= 0x10ffff && !(cp >= 0xd800 && cp <= 0xdfff);
                    if (valid && !(cp >= 0x80 && cp <= 0x9f) && cp != 0x2028 && cp != 0x2029)
                        history_push(w, (const char *)w->utf8, w->utf8_need);
                    w->utf8_len = w->utf8_need = 0;
                }
                continue;
            }
            w->utf8_len = w->utf8_need = 0;
            /* Reprocess the unexpected byte, particularly ESC. */
        }
        if (c == 0x1b) w->escape = 1;
        else if (c == 0x9b) w->escape = 2;
        else if (c == 0x9d) w->escape = 3;
        else if (c == 0x90 || c == 0x98 || c == 0x9e || c == 0x9f) w->escape = 4;
        else if (c == '\n' || c == '\r') history_push(w, "\n", 1);
        else if (c == '\t') history_push(w, "  ", 2);
        else if (c >= 0x20 && c < 0x7f) history_push(w, (const char *)&data[i], 1);
        else if (c >= 0xc2 && c <= 0xf4) {
            w->utf8[0] = c; w->utf8_len = 1;
            w->utf8_need = c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
        }
    }
}

static bool sanitized_label(char *out, size_t cap, const char *input) {
    if (!input) return false;
    dock_worker_t tmp = {0};
    sanitized_append(&tmp, input, strlen(input));
    for (size_t i = 0; i < tmp.history_len; i++) if (tmp.history[i] == '\n') tmp.history[i] = ' ';
    size_t n = tmp.history_len < cap - 1 ? tmp.history_len : cap - 1;
    while (n && ((unsigned char)tmp.history[n] & 0xc0) == 0x80) n--;
    bool changed = strlen(out) != n || memcmp(out, tmp.history, n);
    memcpy(out, tmp.history, n); out[n] = '\0';
    return changed;
}

static bool terminal_status(const char *s);
static bool success_status(const char *s) {
    return !strcmp(s, "done") || !strcmp(s, "completed");
}

/* Projection only: roll-up/dismissal never deletes worker output or run records. */
static void rebuild_view(void) {
    dock.view_count = 0;
    bool selected_visible = false;
    for (int i = 0; i < dock.count; i++) {
        dock_worker_t *w = &dock.workers[i];
        if (!dock.history_view && (w->dismissed || (success_status(w->status) && !w->manual))) continue;
        dock.view[dock.view_count++] = i;
        selected_visible |= dock.selected == i;
    }
    if (!selected_visible) {
        dock.selected = dock.view_count ? dock.view[0] : -1;
        dock.drag = -1;
    }
}

static int selected_slot(void) {
    for (int i = 0; i < dock.view_count; i++) if (dock.view[i] == dock.selected) return i;
    return 0;
}

static int worker_index(int id, bool create) {
    for (int i = 0; i < dock.count; i++) if (dock.workers[i].id == id) return i;
    if (!create) return -1;
    int i = dock.count;
    if (i == DOCK_WORKERS) {
        /* Reclaim dismissed terminal entries first, then oldest terminal history.
         * Never evict an active card for historical or newly arriving workers. */
        int victim = -1;
        for (int pass = 0; pass < 2 && victim < 0; pass++)
            for (int j = 0; j < dock.count; j++)
                if (terminal_status(dock.workers[j].status) &&
                    (pass || dock.workers[j].dismissed)) { victim = j; break; }
        if (victim < 0) {
            if (!dock.capacity_limited) dock.dirty = dock.paint_dirty = true;
            dock.capacity_limited = true;
            return -1;
        }
        memmove(dock.workers + victim, dock.workers + victim + 1,
                sizeof(dock.workers[0]) * (DOCK_WORKERS - victim - 1));
        i--;
        if (dock.selected == victim) dock.selected = -1;
        else if (dock.selected > victim) dock.selected--;
        dock.drag = -1;
    } else dock.count++;
    dock.workers[i] = (dock_worker_t){.id = id, .raised = ++dock.serial};
    strcpy(dock.workers[i].status, "starting");
    if (dock.selected < 0) dock.selected = i;
    if (!dock.hidden_by_user) dock.visible = true;
    dock.dirty = dock.paint_dirty = true;
    rebuild_view();
    return i;
}

static bool terminal_status(const char *s) {
    return s && (!strcmp(s, "completed") || !strcmp(s, "done") || !strcmp(s, "failed") ||
                 !strcmp(s, "error") || !strcmp(s, "cancelled") || !strcmp(s, "canceled") ||
                 !strcmp(s, "stopped") || !strcmp(s, "killed") || !strcmp(s, "timeout"));
}

void tui_swarm_dock_update(int id, const char *task, const char *model,
                           const char *status, size_t bytes, double cost) {
    pthread_mutex_lock(&dock_mutex);
    int index = worker_index(id, true);
    if (index < 0) { pthread_mutex_unlock(&dock_mutex); return; }
    dock_worker_t *w = &dock.workers[index];
    bool changed = false;
    if (bytes < w->bytes || (terminal_status(w->status) && status && !terminal_status(status))) {
        changed = true;
        w->dismissed = false;
        w->history_len = 0; w->history[0] = '\0'; w->scroll = 0;
        w->escape = w->string_utf8 = w->utf8_len = w->utf8_need = 0;
    }
    changed |= sanitized_label(w->task, sizeof(w->task), task);
    changed |= sanitized_label(w->model, sizeof(w->model), model);
    changed |= sanitized_label(w->status, sizeof(w->status), status);
    double clean_cost = isfinite(cost) && cost > 0 ? cost : 0;
    changed |= w->bytes != bytes || w->cost != clean_cost;
    w->bytes = bytes; w->cost = clean_cost;
    if (changed) {
        rebuild_view();
        dock.dirty = dock.paint_dirty = true;
    }
    pthread_mutex_unlock(&dock_mutex);
}

void tui_swarm_dock_append(int id, const char *data, size_t len) {
    if (!data || !len) return;
    pthread_mutex_lock(&dock_mutex);
    int index = worker_index(id, true);
    if (index < 0) { pthread_mutex_unlock(&dock_mutex); return; }
    dock_worker_t *w = &dock.workers[index];
    sanitized_append(w, data, len);
    dock.output_serial++;
    dock.dirty = dock.paint_dirty = true;
    pthread_mutex_unlock(&dock_mutex);
}

void tui_swarm_dock_reset(void) {
    pthread_mutex_lock(&dock_mutex);
    invalidate_frame();
    dock = (dock_state_t){.selected = -1, .drag = -1, .page_size = 1, .dirty = true, .paint_dirty = true};
    pthread_mutex_unlock(&dock_mutex);
}

/* Poll on the composer thread; no producer writes, timers, or model calls.
 * Counts describe observed lifecycle state, not inferred forward progress. */
bool tui_swarm_dock_progress(double now, char *out, size_t cap) {
    if (!out || !cap || !isfinite(now) || now < 0) return false;
    out[0] = '\0';
    pthread_mutex_lock(&dock_mutex);
    if (dock.progress_notice[0] && now >= dock.progress_until) {
        dock.progress_notice[0] = '\0';
        dock.dirty = dock.paint_dirty = true;
    }
    int active = 0, finished = 0, failed = 0, first = -1;
    for (int i = 0; i < dock.count; i++) {
        const char *status = dock.workers[i].status;
        if (!terminal_status(status)) { active++; if (first < 0) first = i; }
        else if (!strcmp(status, "failed") || !strcmp(status, "error") || !strcmp(status, "timeout")) failed++;
        else finished++;
    }
    if (!dock.visible || (!active && !dock.progress_armed)) {
        dock.progress_armed = false;
        pthread_mutex_unlock(&dock_mutex); return false;
    }
    if (active && dock.progress_armed && now >= dock.progress_at && now - dock.progress_at < 15.0) {
        pthread_mutex_unlock(&dock_mutex); return false;
    }
    const char *activity = !active ? "finished" : !dock.progress_armed ? "started" :
        dock.output_serial != dock.progress_output ? "output received" : "no new output";
    if (dock.selected >= 0 && !terminal_status(dock.workers[dock.selected].status)) first = dock.selected;
    snprintf(out, cap, "Swarm%s: %d active / %d finished / %d failed · %s%s%s",
             dock.capacity_limited ? " (retained)" : "", active, finished, failed, activity, first >= 0 ? " · " : "",
             first >= 0 ? dock.workers[first].task : "");
    dock.progress_armed = active > 0;
    dock.progress_at = now; dock.progress_output = dock.output_serial;
    snprintf(dock.progress_notice, sizeof(dock.progress_notice), "%s", out);
    dock.progress_until = now + 7.0;
    dock.dirty = dock.paint_dirty = true;
    pthread_mutex_unlock(&dock_mutex);
    return true;
}

bool tui_swarm_dock_changed(void) {
    pthread_mutex_lock(&dock_mutex);
    bool result = dock.dirty; dock.dirty = false;
    pthread_mutex_unlock(&dock_mutex);
    return result;
}

bool tui_swarm_dock_focused(void) {
    pthread_mutex_lock(&dock_mutex); bool result = dock.focused;
    pthread_mutex_unlock(&dock_mutex); return result;
}

bool tui_swarm_dock_visible(void) {
    pthread_mutex_lock(&dock_mutex); bool result = dock.visible;
    pthread_mutex_unlock(&dock_mutex); return result;
}

void tui_swarm_dock_show(bool visible) {
    pthread_mutex_lock(&dock_mutex);
    if (dock.visible != visible) invalidate_frame();
    dock.visible = visible; dock.hidden_by_user = !visible;
    if (!visible) { dock.focused = false; dock.drag = -1; }
    dock.dirty = dock.paint_dirty = true;
    pthread_mutex_unlock(&dock_mutex);
}

void tui_swarm_dock_toggle_focus(void) {
    pthread_mutex_lock(&dock_mutex);
    dock.focused = !dock.focused; dock.visible = true; dock.hidden_by_user = false;
    if (dock.focused && !dock.view_count && dock.count) {
        dock.history_view = true;
        rebuild_view();
    }
    dock.dirty = dock.paint_dirty = true;
    pthread_mutex_unlock(&dock_mutex);
}

int tui_swarm_dock_height(int cols, int rows, int reserved_rows) {
    pthread_mutex_lock(&dock_mutex);
    int available = rows - max_i(0, reserved_rows) - 3;
    int result = 0;
    if (dock.visible && cols >= 20 && available >= 5) {
        int desired = dock.zoom ? rows * 3 / 4 : rows / 2;
        bool compact = dock.count > 0;
        for (int i = 0; i < dock.count; i++) {
            dock_worker_t *w = &dock.workers[i];
            if (!w->dismissed && (!success_status(w->status) || w->manual)) compact = false;
        }
        /* Failures keep space until explicitly dismissed. Focus/zoom/manual
         * geometry preserve inspection, including the retained history view. */
        if (compact && !dock.focused && !dock.zoom) desired = 6;
        result = min_i(available, max_i(5, min_i(desired, DOCK_MAX_ROWS)));
    }
    pthread_mutex_unlock(&dock_mutex);
    return result;
}

static void clamp_worker(dock_worker_t *w) {
    int width = max_i(1, dock.cols), height = max_i(1, dock.height - 2);
    w->w = clamp_i(w->w, min_i(24, width), width);
    w->h = clamp_i(w->h, min_i(5, height), height);
    w->x = clamp_i(w->x, 0, width - w->w);
    w->y = clamp_i(w->y, 1, height - w->h + 1);
}

static void arrange(bool all) {
    int usable = max_i(1, dock.height - 2);
    int columns = dock.cols >= 70 ? 2 : 1;
    int lines = usable >= 10 ? 2 : 1;
    dock.page_size = columns * lines;
    for (int i = 0; i < dock.view_count; i++) {
        dock_worker_t *w = &dock.workers[dock.view[i]];
        if (all || !w->placed || !w->manual) {
            int slot = i % dock.page_size;
            int col = slot % columns, row = slot / columns;
            w->x = col * dock.cols / columns;
            w->y = 1 + row * usable / lines;
            w->w = (col + 1) * dock.cols / columns - w->x;
            w->h = (row + 1) * usable / lines - (w->y - 1);
            w->placed = true;
            w->manual = false;
        }
        clamp_worker(w);
    }
}

static void select_worker(int i) {
    if (dock.view_count == 0) return;
    dock.selected = dock.view[(i % dock.view_count + dock.view_count) % dock.view_count];
    dock.workers[dock.selected].raised = ++dock.serial;
    dock.dirty = dock.paint_dirty = true;
}

bool tui_swarm_dock_key(int key, int modifiers) {
    pthread_mutex_lock(&dock_mutex);
    if (!dock.focused || !dock.visible) { pthread_mutex_unlock(&dock_mutex); return false; }
    bool handled = true;
    dock_worker_t *w = dock.selected >= 0 ? &dock.workers[dock.selected] : NULL;
    int dx = key == TUI_SWARM_KEY_LEFT ? -1 : key == TUI_SWARM_KEY_RIGHT ? 1 : 0;
    int dy = key == TUI_SWARM_KEY_UP ? -1 : key == TUI_SWARM_KEY_DOWN ? 1 : 0;
    if (key == TUI_SWARM_KEY_ESC || key == 27) { dock.focused = false; dock.drag = -1; }
    else if (key == TUI_SWARM_KEY_TAB || key == '\t') select_worker(selected_slot() + ((modifiers & TUI_SWARM_MOD_SHIFT) ? -1 : 1));
    else if (key == TUI_SWARM_KEY_Z || key == 'z' || key == TUI_SWARM_KEY_ENTER || key == '\r') dock.zoom = !dock.zoom;
    else if (key == TUI_SWARM_KEY_R || key == 'r') {
        dock.zoom = false;
        for (int i = 0; i < dock.count; i++) dock.workers[i].manual = false;
        rebuild_view(); arrange(true);
    }
    else if (key == TUI_SWARM_KEY_H || key == 'h') {
        dock.history_view = !dock.history_view;
        dock.zoom = false; dock.drag = -1;
        rebuild_view();
    }
    else if (key == TUI_SWARM_KEY_X || key == 'x') {
        /* Dismiss is UI-only and terminal-only; it cannot cancel a worker. */
        if (w && terminal_status(w->status)) {
            w->dismissed = true;
            dock.history_view = false; dock.zoom = false; dock.drag = -1;
            rebuild_view();
        }
    }
    else if (key == TUI_SWARM_KEY_PAGEUP && w) w->scroll = min_i(DOCK_HISTORY, w->scroll + max_i(1, w->h - 4));
    else if (key == TUI_SWARM_KEY_PAGEDOWN && w) w->scroll = max_i(0, w->scroll - max_i(1, w->h - 4));
    else if (key == TUI_SWARM_KEY_HOME && w) w->scroll = DOCK_HISTORY;
    else if (key == TUI_SWARM_KEY_END && w) w->scroll = 0;
    else if (dx || dy) {
        if (w && (modifiers & TUI_SWARM_MOD_CTRL)) { w->manual = true; w->w += dx * 2; w->h += dy; clamp_worker(w); }
        else if (w && (modifiers & TUI_SWARM_MOD_SHIFT)) { w->manual = true; w->x += dx * 2; w->y += dy; clamp_worker(w); }
        else select_worker(selected_slot() + (dx ? dx : dy * (dock.cols >= 70 ? 2 : 1)));
    } else handled = false;
    if (handled) dock.dirty = dock.paint_dirty = true;
    pthread_mutex_unlock(&dock_mutex);
    return handled;
}

static void effective_rect(const dock_worker_t *w, int *x, int *y, int *width, int *height) {
    *x = dock.zoom ? 0 : w->x; *y = dock.zoom ? 1 : w->y;
    *width = dock.zoom ? dock.cols : w->w;
    *height = dock.zoom ? dock.height - 2 : w->h;
}

bool tui_swarm_dock_mouse(int button, int col, int row, bool released) {
    pthread_mutex_lock(&dock_mutex);
    int x = col - 1, y = row - dock.top;
    bool inside = dock.visible && x >= 0 && x < dock.cols && y >= 0 && y < dock.height;
    if (released) {
        bool handled = dock.drag >= 0 || inside;
        dock.drag = -1;
        pthread_mutex_unlock(&dock_mutex); return handled;
    }
    if ((button & 32) && dock.drag >= 0) {
        dock_worker_t *w = &dock.workers[dock.drag];
        w->manual = true;
        if (dock.drag_kind == 1) { w->x = dock.drag_x + col - dock.drag_col; w->y = dock.drag_y + row - dock.drag_row; }
        else { w->w = dock.drag_w + col - dock.drag_col; w->h = dock.drag_h + row - dock.drag_row; }
        clamp_worker(w); dock.dirty = dock.paint_dirty = true;
        pthread_mutex_unlock(&dock_mutex); return true;
    }
    if (!inside) {
        if (!(button & (32 | 64))) { dock.focused = false; dock.dirty = dock.paint_dirty = true; }
        pthread_mutex_unlock(&dock_mutex); return false;
    }
    if (button & 32) { pthread_mutex_unlock(&dock_mutex); return true; }
    int first = (selected_slot() / dock.page_size) * dock.page_size;
    int hit = -1, hit_slot = -1; unsigned long raised = 0;
    for (int slot = first; slot < min_i(dock.view_count, first + dock.page_size); slot++) {
        int i = dock.view[slot];
        if (dock.zoom && i != dock.selected) continue;
        int wx, wy, ww, wh; effective_rect(&dock.workers[i], &wx, &wy, &ww, &wh);
        if (x >= wx && x < wx + ww && y >= wy && y < wy + wh && dock.workers[i].raised >= raised) {
            hit = i; hit_slot = slot; raised = dock.workers[i].raised;
        }
    }
    if (hit >= 0) {
        select_worker(hit_slot);
        dock_worker_t *w = &dock.workers[hit];
        if (button & 64) w->scroll = clamp_i(w->scroll + ((button & 1) ? -3 : 3), 0, DOCK_HISTORY);
        else if ((button & 3) == 0) {
            dock.focused = true;
            int wx, wy, ww, wh; effective_rect(w, &wx, &wy, &ww, &wh);
            if (!dock.zoom && (y == wy || (x >= wx + ww - 2 && y == wy + wh - 1))) {
                dock.drag = hit; dock.drag_kind = y == wy ? 1 : 2;
                dock.drag_col = col; dock.drag_row = row;
                dock.drag_x = w->x; dock.drag_y = w->y; dock.drag_w = w->w; dock.drag_h = w->h;
            }
        }
    } else if (!(button & 64)) dock.focused = true;
    dock.dirty = dock.paint_dirty = true;
    pthread_mutex_unlock(&dock_mutex);
    return true;
}

typedef struct { char text[24]; unsigned char color, continuation; } dock_cell_t;
typedef struct { dock_cell_t *cells; int cols, rows; } dock_canvas_t;
/* Only the composer uses this terminal-relative cache. Snapshot rendering stays full. */
static dock_canvas_t last_frame;
static int last_top;
static bool frame_valid;

static void invalidate_frame(void) { frame_valid = false; }

void tui_swarm_dock_invalidate(void) {
    pthread_mutex_lock(&dock_mutex);
    /* An absent dock must not schedule another repaint on every composer paint. */
    dock.dirty |= frame_valid;
    dock.paint_dirty = true;
    invalidate_frame();
    pthread_mutex_unlock(&dock_mutex);
}

enum { SURFACE, MUTED, ICE, COPPER, BRIGHT, GREEN, RED };
static const char *palette[] = {
    "\033[0;38;2;166;183;197;48;2;15;22;30m",
    "\033[0;38;2;111;133;150;48;2;15;22;30m",
    "\033[0;38;2;140;211;230;48;2;15;22;30m",
    "\033[1;38;2;220;155;104;48;2;15;22;30m",
    "\033[1;38;2;225;239;244;48;2;15;22;30m",
    "\033[0;38;2;136;207;171;48;2;15;22;30m",
    "\033[1;38;2;235;128;132;48;2;15;22;30m"
};

static dock_cell_t *cell_at(dock_canvas_t *c, int x, int y) {
    return x >= 0 && x < c->cols && y >= 0 && y < c->rows ? &c->cells[y * c->cols + x] : NULL;
}

/* Clearing either half of a wide glyph clears the pair before an overlapping card is drawn. */
static void clear_cell(dock_canvas_t *c, int x, int y) {
    dock_cell_t *p = cell_at(c, x, y); if (!p) return;
    if (p->continuation && x > 0) {
        dock_cell_t *prev = cell_at(c, x - 1, y); strcpy(prev->text, " "); prev->continuation = 0;
    }
    dock_cell_t *next = cell_at(c, x + 1, y);
    if (next && next->continuation) { strcpy(next->text, " "); next->continuation = 0; }
    strcpy(p->text, " "); p->continuation = 0; p->color = SURFACE;
}

static int canvas_text(dock_canvas_t *c, int x, int y, int limit, const char *s, int color) {
    int start = x, end = min_i(c->cols, x + max_i(0, limit));
    const char *text_end = s + strlen(s);
    while (s < text_end && x < end) {
        wchar_t wc; mbstate_t st = {0}; size_t n = mbrtowc(&wc, s, (size_t)(text_end - s), &st);
        if (n == (size_t)-1 || n == (size_t)-2) { n = 1; wc = '?'; }
        else if (!n) break;
        int width = wcwidth(wc);
        if (width < 0) { s += n; continue; }
        if (!width) {
            int previous = x - 1;
            dock_cell_t *p = cell_at(c, previous, y);
            if (p && p->continuation) p = cell_at(c, --previous, y);
            if (p && previous >= start && strlen(p->text) + n < sizeof(p->text)) strncat(p->text, s, n);
            s += n; continue;
        }
        if (width > 2 || x + width > end) break;
        clear_cell(c, x, y); if (width == 2) clear_cell(c, x + 1, y);
        dock_cell_t *p = cell_at(c, x, y);
        if (p) {
            if (wc == '?' && n == 1 && (unsigned char)*s >= 0x80) strcpy(p->text, "?");
            else { memcpy(p->text, s, n); p->text[n] = '\0'; }
            p->color = (unsigned char)color;
        }
        if (width == 2 && (p = cell_at(c, x + 1, y))) { p->text[0] = '\0'; p->continuation = 1; p->color = (unsigned char)color; }
        x += width; s += n;
    }
    return x - start;
}

static void line(dock_canvas_t *c, int x, int y, int count, const char *glyph, int color) {
    for (int i = 0; i < count; i++) canvas_text(c, x + i, y, 1, glyph, color);
}

/* Find visual line starts, honoring terminal columns rather than UTF-8 bytes. */
static int wrap_starts(const char *s, int width, size_t *starts, int cap) {
    int count = 1, col = 0; size_t at = 0, length = strlen(s); starts[0] = 0;
    while (at < length && count < cap) {
        if (s[at] == '\n') { starts[count++] = ++at; col = 0; continue; }
        wchar_t wc; mbstate_t st = {0}; size_t n = mbrtowc(&wc, s + at, length - at, &st);
        if (n == (size_t)-1 || n == (size_t)-2 || !n) { n = 1; wc = '?'; }
        int w = max_i(0, wcwidth(wc));
        if (w > 0 && col + w > width && col > 0) { starts[count++] = at; col = 0; }
        col += w; at += n;
    }
    return count;
}

static void draw_worker(dock_canvas_t *c, dock_worker_t *w, bool selected) {
    int x, y, width, height; effective_rect(w, &x, &y, &width, &height);
    if (width < 2 || height < 2) return;
    int border = selected ? (dock.focused ? COPPER : ICE) : MUTED;
    for (int j = y; j < y + height; j++) for (int i = x; i < x + width; i++) clear_cell(c, i, j);
    line(c, x + 1, y, width - 2, "─", border);
    line(c, x + 1, y + height - 1, width - 2, "─", border);
    canvas_text(c, x, y, 1, "╭", border); canvas_text(c, x + width - 1, y, 1, "╮", border);
    canvas_text(c, x, y + height - 1, 1, "╰", border); canvas_text(c, x + width - 1, y + height - 1, 1, "◢", border);
    for (int j = y + 1; j < y + height - 1; j++) {
        canvas_text(c, x, j, 1, "│", border); canvas_text(c, x + width - 1, j, 1, "│", border);
    }
    static const char *glyphs[] = {"⍼", "⧉", "⌬", "⦇", "🝮", "⧖"};
    char label[512];
    snprintf(label, sizeof(label), " %s %d · %s%s ", glyphs[(unsigned)w->id % 6], w->id, w->status,
             w->dismissed ? " · dismissed" : "");
    int status_color = border;
    if (!strcmp(w->status, "done") || !strcmp(w->status, "completed")) status_color = GREEN;
    else if (!strcmp(w->status, "failed") || !strcmp(w->status, "error") ||
             !strcmp(w->status, "timeout")) status_color = RED;
    canvas_text(c, x + 2, y, width - 4, label, status_color);
    if (height >= 4) {
        snprintf(label, sizeof(label), "%s · %zu B · $%.4f", w->model[0] ? w->model : "model pending", w->bytes, w->cost);
        canvas_text(c, x + 2, y + 1, width - 4, label, MUTED);
    }
    if (height >= 5) canvas_text(c, x + 2, y + 2, width - 4, w->task[0] ? w->task : "Waiting for worker output…", BRIGHT);
    int content = height - 4, content_width = width - 4;
    if (content > 0 && content_width > 0) {
        size_t starts[DOCK_HISTORY]; int count = wrap_starts(w->history, content_width, starts, DOCK_HISTORY);
        int max_scroll = max_i(0, count - content); w->scroll = min_i(w->scroll, max_scroll);
        int first = max_i(0, count - content - w->scroll);
        for (int j = 0; j < content && first + j < count; j++) {
            size_t begin = starts[first + j];
            size_t end = first + j + 1 < count ? starts[first + j + 1] : w->history_len;
            while (end > begin && w->history[end - 1] == '\n') end--;
            char text[DOCK_HISTORY]; size_t n = min_i((int)(end - begin), DOCK_HISTORY - 1);
            memcpy(text, w->history + begin, n); text[n] = '\0';
            canvas_text(c, x + 2, y + 3 + j, content_width, text, SURFACE);
        }
        if (w->scroll) {
            snprintf(label, sizeof(label), " ↑ %d lines ", w->scroll);
            canvas_text(c, x + 2, y + height - 1, width - 4, label, COPPER);
        }
    }
}

static void render_frame(FILE *out, int top, int cols, int height, bool retained) {
    if (!out || top < 1 || cols < 1 || height < 1) return;
    pthread_mutex_lock(&dock_mutex);
    if (!dock.visible) { pthread_mutex_unlock(&dock_mutex); return; }
    cols = min_i(cols, DOCK_MAX_COLS); height = min_i(height, DOCK_MAX_ROWS);
    bool full = !retained || !frame_valid || last_top != top ||
                last_frame.cols != cols || last_frame.rows != height;
    if (!full && !dock.paint_dirty) { pthread_mutex_unlock(&dock_mutex); return; }
    pthread_once(&locale_once, init_locale);
    locale_t previous_locale = utf8_locale ? uselocale(utf8_locale) : (locale_t)0;
    dock.top = top; dock.cols = min_i(cols, DOCK_MAX_COLS); dock.height = min_i(height, DOCK_MAX_ROWS);
    arrange(false);
    dock_canvas_t c = {.cols = dock.cols, .rows = dock.height};
    c.cells = calloc((size_t)c.cols * c.rows, sizeof(*c.cells));
    if (!c.cells) {
        if (previous_locale) uselocale(previous_locale);
        pthread_mutex_unlock(&dock_mutex); return;
    }
    for (int i = 0; i < c.cols * c.rows; i++) strcpy(c.cells[i].text, " ");
    char title[512], signature[160] = "dsco";
    const char *configured_signature = getenv("DSCO_KITTY_SIGNATURE");
    if (configured_signature && *configured_signature) sanitized_label(signature, sizeof(signature), configured_signature);
    int page = selected_slot() / dock.page_size;
    int finished = 0, dismissed = 0;
    for (int i = 0; i < dock.count; i++) {
        finished += terminal_status(dock.workers[i].status);
        dismissed += dock.workers[i].dismissed;
    }
    snprintf(title, sizeof(title), " ⧉ %s  /  SWARM%s%s%s · %d shown / %d retained · %d/%d",
             signature, dock.capacity_limited ? " · capacity limit" : "",
             dock.focused ? " · FOCUSED" : "", dock.history_view ? " · HISTORY" : "",
             dock.view_count, dock.count, page + 1, max_i(1, (dock.view_count + dock.page_size - 1) / dock.page_size));
    canvas_text(&c, 0, 0, c.cols, title, ICE);
    if (dock.count == 0) {
        canvas_text(&c, 2, min_i(2, c.rows - 1), c.cols - 4, "Waiting for swarm workers", BRIGHT);
        if (c.rows > 4) canvas_text(&c, 2, 3, c.cols - 4, "Real workers appear here and remain after completion.", MUTED);
    } else if (!dock.view_count) {
        snprintf(title, sizeof(title), "%d finished · %d dismissed · output retained", finished, dismissed);
        canvas_text(&c, 2, min_i(2, c.rows - 1), c.cols - 4, title, GREEN);
        if (c.rows > 4) canvas_text(&c, 2, 3, c.cols - 4, dock.focused
            ? "H to inspect history; no execution records deleted."
            : "Ctrl+G to inspect history; no execution records deleted.", MUTED);
    } else {
        int order[DOCK_WORKERS], n = 0;
        for (int slot = page * dock.page_size; slot < min_i(dock.view_count, (page + 1) * dock.page_size); slot++) {
            int i = dock.view[slot];
            if (!dock.zoom || i == dock.selected) order[n++] = i;
        }
        for (int i = 1; i < n; i++) {
            int t = order[i], j = i;
            while (j > 0 && dock.workers[order[j - 1]].raised > dock.workers[t].raised) { order[j] = order[j - 1]; j--; }
            order[j] = t;
        }
        /* Selected card always paints last, even while producers create peers. */
        for (int i = 0; i < n; i++) if (order[i] != dock.selected) draw_worker(&c, &dock.workers[order[i]], false);
        if (dock.selected >= 0) draw_worker(&c, &dock.workers[dock.selected], true);
    }
    const char *help = dock.progress_notice[0] ? dock.progress_notice : dock.focused
        ? (c.cols < 80 ? " h history · x dismiss · Esc input"
                       : " Tab next · h history/live · x dismiss done · z zoom · r arrange · Esc input")
        : (c.cols < 70 ? " Ctrl+G swarm · wheel scroll"
                       : " Ctrl+G swarm · drag title to move · drag ◢ to resize · wheel scroll");
    canvas_text(&c, 0, c.rows - 1, c.cols, help, dock.focused ? COPPER : MUTED);
    char *frame = NULL; size_t frame_len = 0;
    FILE *frame_out = open_memstream(&frame, &frame_len);
    if (!frame_out) {
        free(c.cells);
        if (previous_locale) uselocale(previous_locale);
        pthread_mutex_unlock(&dock_mutex); return;
    }
    int previous = -1;
    bool emitted = false;
    for (int y = 0; y < c.rows; y++) {
        /* Whole-row deltas preserve wide glyphs and erase vacated card geometry. */
        if (!full && !memcmp(c.cells + y * c.cols, last_frame.cells + y * c.cols,
                             (size_t)c.cols * sizeof(*c.cells))) continue;
        emitted = true;
        fprintf(frame_out, "\033[%d;1H", top + y);
        for (int x = 0; x < c.cols; x++) {
            dock_cell_t *p = &c.cells[y * c.cols + x];
            if (p->continuation) continue;
            if (previous != p->color) { fputs(palette[p->color], frame_out); previous = p->color; }
            fputs(p->text, frame_out);
        }
    }
    if (emitted) fputs("\033[0m", frame_out);
    int frame_error = ferror(frame_out);
    if (fclose(frame_out)) frame_error = 1;
    if (retained && !frame_error) {
        free(last_frame.cells);
        last_frame = c; last_top = top; frame_valid = true;
        dock.paint_dirty = false;
    } else {
        free(c.cells);
        /* Snapshot calls can change layout; do not let a later delta use stale geometry. */
        invalidate_frame();
    }
    if (previous_locale) uselocale(previous_locale);
    pthread_mutex_unlock(&dock_mutex);
    /* The composer owns the terminal lock. A slow terminal never holds up producers. */
    if (frame) {
        if (frame_error || fwrite(frame, 1, frame_len, out) != frame_len || ferror(out))
            tui_swarm_dock_invalidate();
        free(frame);
    }
}

void tui_swarm_dock_render(FILE *out, int top, int cols, int height) {
    render_frame(out, top, cols, height, false);
}

void tui_swarm_dock_render_retained(FILE *out, int top, int cols, int height) {
    render_frame(out, top, cols, height, true);
}

#ifdef DSCO_TUI_SWARM_DOCK_TEST
bool tui_swarm_dock_snapshot(int id, tui_swarm_dock_snapshot_t *s) {
    if (!s) return false;
    pthread_mutex_lock(&dock_mutex);
    memset(s, 0, sizeof(*s));
    s->count = dock.count; s->selected_id = dock.selected >= 0 ? dock.workers[dock.selected].id : -1;
    s->focused = dock.focused; s->visible = dock.visible; s->zoomed = dock.zoom;
    s->page_size = dock.page_size; s->page = selected_slot() / dock.page_size;
    s->view_count = dock.view_count; s->history_view = dock.history_view;
    s->capacity_limited = dock.capacity_limited;
    int i = worker_index(id, false);
    if (i >= 0) {
        dock_worker_t *w = &dock.workers[i];
        s->dismissed = w->dismissed;
        s->x = w->x; s->y = w->y; s->width = w->w; s->height = w->h; s->scroll = w->scroll;
        memcpy(s->status, w->status, sizeof(s->status)); memcpy(s->tail, w->history, sizeof(s->tail));
    }
    pthread_mutex_unlock(&dock_mutex); return i >= 0;
}
#endif
