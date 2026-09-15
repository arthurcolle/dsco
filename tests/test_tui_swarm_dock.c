#define _XOPEN_SOURCE 700
#define DSCO_TUI_SWARM_DOCK_TEST 1
#include "tui_swarm_dock.h"

#include <assert.h>
#include <locale.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static tui_swarm_dock_snapshot_t snapshot(int id) {
    tui_swarm_dock_snapshot_t s;
    assert(tui_swarm_dock_snapshot(id, &s));
    return s;
}

static char *render(int cols, int height) {
    char *data = NULL; size_t size = 0;
    FILE *out = open_memstream(&data, &size); assert(out);
    tui_swarm_dock_render(out, 5, cols, height); assert(fclose(out) == 0);
    assert(data); return data;
}

/* Verify that each rendered row occupies exactly the advertised terminal cells.
 * Only the renderer's cursor positioning and SGR are accepted as controls. */
static void check_rows(const char *p, int cols, int rows) {
    int column = 0, seen = 0;
    while (*p) {
        if (*p == '\033') {
            p++; assert(*p++ == '[');
            while ((*p >= '0' && *p <= '9') || *p == ';') p++;
            assert(*p == 'm' || *p == 'H');
            if (*p++ == 'H') { if (seen) assert(column == cols); column = 0; seen++; }
            continue;
        }
        wchar_t wc; mbstate_t st = {0};
        size_t n = mbrtowc(&wc, p, strlen(p), &st);
        assert(n != (size_t)-1 && n != (size_t)-2 && n > 0);
        int width = wcwidth(wc); assert(width >= 0); column += width; p += n;
    }
    assert(column == cols); assert(seen == rows);
}

static void test_lifecycle(void) {
    tui_swarm_dock_reset();
    assert(!tui_swarm_dock_visible()); assert(tui_swarm_dock_height(120, 40, 5) == 0);
    assert(tui_swarm_dock_changed()); assert(!tui_swarm_dock_changed());
    tui_swarm_dock_toggle_focus();
    assert(tui_swarm_dock_focused()); assert(tui_swarm_dock_visible());
    char *out = render(100, 16); assert(strstr(out, "Waiting for swarm workers")); free(out);
    tui_swarm_dock_update(7, "Inspect real source", "local-model", "running", 64, 0.02);
    tui_swarm_dock_append(7, "real output", 11);
    tui_swarm_dock_update(7, NULL, NULL, "completed", 75, 0.03);
    assert(strcmp(snapshot(7).status, "completed") == 0);
    assert(strcmp(snapshot(7).tail, "real output") == 0);
    assert(tui_swarm_dock_key(TUI_SWARM_KEY_ESC, 0));
    assert(!tui_swarm_dock_focused()); assert(tui_swarm_dock_visible());
    assert(!tui_swarm_dock_key(TUI_SWARM_KEY_LEFT, 0));
    tui_swarm_dock_show(false);
    tui_swarm_dock_update(8, "Another worker", "model", "running", 0, 0);
    assert(!tui_swarm_dock_visible());
    tui_swarm_dock_toggle_focus(); assert(tui_swarm_dock_visible());
    tui_swarm_dock_reset(); tui_swarm_dock_update(3, "Start", "model", "running", 0, 0);
    assert(tui_swarm_dock_visible()); assert(!tui_swarm_dock_focused());
}

static void test_id_reuse_and_locale(void) {
    tui_swarm_dock_reset();
    tui_swarm_dock_update(1, "First task", "model", "running", 20, 0);
    tui_swarm_dock_append(1, "old output\033]52;unfinished", strlen("old output\033]52;unfinished"));
    char *out = render(120, 20); free(out);
    tui_swarm_dock_toggle_focus();
    tui_swarm_dock_key(TUI_SWARM_KEY_RIGHT, TUI_SWARM_MOD_SHIFT);
    tui_swarm_dock_snapshot_t before = snapshot(1);
    tui_swarm_dock_update(1, NULL, NULL, "completed", 20, 0);
    tui_swarm_dock_update(1, "Second task", "model", "running", 20, 0);
    tui_swarm_dock_append(1, "new output", 10);
    assert(!strcmp(snapshot(1).tail, "new output"));
    assert(snapshot(1).x == before.x && snapshot(1).width == before.width);
    tui_swarm_dock_update(1, NULL, NULL, "running", 0, 0);
    assert(!strcmp(snapshot(1).tail, ""));
    assert(setenv("DSCO_KITTY_SIGNATURE", "ᴀʀᴛʜᴜʀ\033[2J", 1) == 0);
    assert(setlocale(LC_CTYPE, "C"));
    out = render(100, 16);
    assert(strstr(out, "ᴀʀᴛʜᴜʀ")); assert(strstr(out, "╭"));
    assert(!strstr(out, "\033[2J"));
    assert(!strcmp(setlocale(LC_CTYPE, NULL), "C"));
    free(out); assert(setlocale(LC_CTYPE, "")); unsetenv("DSCO_KITTY_SIGNATURE");
}

static void test_layout_keyboard(void) {
    tui_swarm_dock_reset();
    for (int i = 0; i < 9; i++) tui_swarm_dock_update(i, "Task", "model", "running", 0, 0);
    assert(tui_swarm_dock_height(120, 40, 5) == 20);
    assert(tui_swarm_dock_height(120, 12, 5) == 0);
    assert(tui_swarm_dock_height(120, 20, 8) <= 9);
    char *out = render(120, 20); check_rows(out, 120, 20); free(out);
    tui_swarm_dock_snapshot_t initial = snapshot(0);
    assert(initial.width == 60 && initial.height == 9 && initial.page_size == 4);
    tui_swarm_dock_toggle_focus();
    assert(tui_swarm_dock_key(TUI_SWARM_KEY_RIGHT, TUI_SWARM_MOD_SHIFT));
    assert(snapshot(0).x == initial.x + 2);
    assert(tui_swarm_dock_key(TUI_SWARM_KEY_DOWN, TUI_SWARM_MOD_SHIFT));
    assert(snapshot(0).y == initial.y + 1);
    assert(tui_swarm_dock_key(TUI_SWARM_KEY_RIGHT, TUI_SWARM_MOD_CTRL));
    assert(snapshot(0).width == initial.width + 2);
    for (int i = 0; i < 200; i++) tui_swarm_dock_key(TUI_SWARM_KEY_LEFT, TUI_SWARM_MOD_CTRL);
    assert(snapshot(0).width == 24);
    for (int i = 0; i < 200; i++) tui_swarm_dock_key(TUI_SWARM_KEY_RIGHT, TUI_SWARM_MOD_SHIFT);
    assert(snapshot(0).x + snapshot(0).width == 120);
    tui_swarm_dock_key(TUI_SWARM_KEY_R, 0);
    assert(snapshot(0).x == initial.x && snapshot(0).width == initial.width);
    for (int i = 0; i < 5; i++) tui_swarm_dock_key(TUI_SWARM_KEY_TAB, 0);
    assert(snapshot(0).selected_id == 5 && snapshot(0).page == 1);
    tui_swarm_dock_key(TUI_SWARM_KEY_Z, 0);
    assert(snapshot(0).zoomed); assert(tui_swarm_dock_height(120, 40, 5) == 30);
    out = render(120, 30); check_rows(out, 120, 30); free(out);
    tui_swarm_dock_key(TUI_SWARM_KEY_Z, 0);
    out = render(26, 7); check_rows(out, 26, 7); free(out);
    for (int i = 0; i < 9; i++) {
        tui_swarm_dock_snapshot_t s = snapshot(i);
        assert(s.x >= 0 && s.y >= 1 && s.width + s.x <= 26 && s.height + s.y <= 6);
    }
}

static void test_mouse(void) {
    tui_swarm_dock_reset(); tui_swarm_dock_update(1, "Task", "model", "running", 0, 0);
    char *out = render(120, 20); free(out);
    tui_swarm_dock_snapshot_t before = snapshot(1);
    /* Header local y=1 -> global row=6; all mouse columns are 1-based. */
    assert(tui_swarm_dock_mouse(0, 5, 6, false));
    assert(tui_swarm_dock_mouse(32, 15, 8, false));
    assert(tui_swarm_dock_mouse(0, 15, 8, true));
    tui_swarm_dock_snapshot_t moved = snapshot(1);
    assert(moved.x == before.x + 10 && moved.y == before.y + 2 && moved.focused);
    int corner_col = moved.x + moved.width, corner_row = 5 + moved.y + moved.height - 1;
    assert(tui_swarm_dock_mouse(0, corner_col, corner_row, false));
    assert(tui_swarm_dock_mouse(32, corner_col - 8, corner_row - 2, false));
    tui_swarm_dock_mouse(0, corner_col - 8, corner_row - 2, true);
    assert(snapshot(1).width == moved.width - 8 && snapshot(1).height == moved.height - 2);
    for (int i = 0; i < 30; i++) tui_swarm_dock_append(1, "line\n", 5);
    moved = snapshot(1);
    tui_swarm_dock_mouse(64, moved.x + 3, 5 + moved.y + 2, false);
    assert(snapshot(1).scroll == 3);
    assert(!tui_swarm_dock_mouse(0, 2, 40, false));
    assert(!tui_swarm_dock_focused()); assert(tui_swarm_dock_visible());
}

static void test_stream_sanitization(void) {
    const char *attack = "hello\033[2J world\033]52;c;SECRET\007!\033Ppayload\033\\ done\033(B.";
    const char *expected = "hello world! done.";
    /* Every possible chunk boundary, including after ESC and inside ST. */
    for (size_t split = 0; split <= strlen(attack); split++) {
        tui_swarm_dock_reset();
        tui_swarm_dock_append(1, attack, split);
        tui_swarm_dock_append(1, attack + split, strlen(attack) - split);
        assert(strcmp(snapshot(1).tail, expected) == 0);
    }
    tui_swarm_dock_reset();
    const char *osc_utf8 = "\033]0;Üprivate title\007safe";
    for (size_t i = 0; i < strlen(osc_utf8); i++) tui_swarm_dock_append(1, osc_utf8 + i, 1);
    assert(!strcmp(snapshot(1).tail, "safe"));
    tui_swarm_dock_reset();
    const char *unicode = "中文 café e\xcc\x81 🝮 ";
    for (size_t i = 0; i < strlen(unicode); i++) tui_swarm_dock_append(1, unicode + i, 1);
    assert(strcmp(snapshot(1).tail, unicode) == 0);
    tui_swarm_dock_update(1, "\033]0;evil\007safe task", "m\033[5m", "done\n\033[2J", 0, 0);
    char *out = render(83, 16); check_rows(out, 83, 16);
    assert(!strstr(out, "evil")); assert(!strstr(out, "\033[2J")); free(out);
    const char invalid[] = {'\xc0', '\xaf', '\x07', '\x00', '\x1b', '[', '3', '1', 'm'};
    tui_swarm_dock_append(1, invalid, sizeof(invalid));
    assert(strcmp(snapshot(1).tail, unicode) == 0);
    /* A wide character underneath a moved card must not leave orphan cells. */
    tui_swarm_dock_update(2, "中文中文中文中文", "m", "running", 0, 0);
    out = render(83, 16); free(out);
    tui_swarm_dock_toggle_focus(); tui_swarm_dock_key(TUI_SWARM_KEY_TAB, 0);
    for (int i = 0; i < 15; i++) tui_swarm_dock_key(TUI_SWARM_KEY_LEFT, TUI_SWARM_MOD_SHIFT);
    out = render(83, 16); check_rows(out, 83, 16); free(out);
}

static void *produce(void *arg) {
    int id = *(int *)arg;
    for (int i = 0; i < 200; i++) {
        tui_swarm_dock_update(id, "Concurrent task", "model", "running", (size_t)i, 0.0);
        tui_swarm_dock_append(id, "line 中文\n", strlen("line 中文\n"));
    }
    tui_swarm_dock_update(id, NULL, NULL, "completed", 200, 0);
    return NULL;
}

static void test_bounds_concurrency(void) {
    tui_swarm_dock_reset();
    for (int i = 0; i < 80; i++) tui_swarm_dock_update(i, "Task", "model", "completed", 0, 0);
    assert(snapshot(79).count == 64);
    tui_swarm_dock_snapshot_t s; assert(!tui_swarm_dock_snapshot(0, &s));
    tui_swarm_dock_toggle_focus();
    for (int i = 0; i < 64; i++) tui_swarm_dock_key(TUI_SWARM_KEY_TAB, 0);
    assert(snapshot(79).selected_id == 16);
    char *out = render(120, 20); check_rows(out, 120, 20); free(out);
    for (int i = 0; i < 9000; i++) tui_swarm_dock_append(79, "中", strlen("中"));
    assert(strlen(snapshot(79).tail) < 8192);
    out = render(120, 20); check_rows(out, 120, 20); free(out);
    tui_swarm_dock_reset();
    pthread_t threads[4]; int ids[4] = {0, 1, 2, 3};
    for (int i = 0; i < 4; i++) assert(pthread_create(&threads[i], NULL, produce, &ids[i]) == 0);
    for (int i = 0; i < 25; i++) { out = render(100, 18); free(out); }
    for (int i = 0; i < 4; i++) { pthread_join(threads[i], NULL); assert(strcmp(snapshot(i).status, "completed") == 0); }
    assert(snapshot(0).count == 4);
}

/* Normalize every emitted byte with its active SGR, including inherited row color.
 * Applying deltas must reconstruct the same glyphs/styles as a full snapshot. */
typedef struct { char rows[40][32768]; } test_screen_t;
static int apply_frame(test_screen_t *screen, const char *p) {
    char sgr[128] = "";
    int row = -1, count = 0;
    size_t used = 0;
    while (*p) {
        if (*p == '\033') {
            const char *begin = p;
            assert(*++p == '['); p++;
            while ((*p >= '0' && *p <= '9') || *p == ';') p++;
            if (*p == 'H') {
                int column;
                assert(sscanf(begin, "\033[%d;%dH", &row, &column) == 2);
                assert(row > 0 && row <= 40 && column == 1);
                row--; used = 0; screen->rows[row][0] = 0; count++;
            } else {
                assert(*p == 'm');
                size_t n = (size_t)(p - begin + 1); assert(n < sizeof(sgr));
                memcpy(sgr, begin, n); sgr[n] = 0;
            }
            p++;
        } else {
            assert(row >= 0);
            size_t n = strlen(sgr);
            assert(used + n + 2 < sizeof(screen->rows[row]));
            memcpy(screen->rows[row] + used, sgr, n); used += n;
            screen->rows[row][used++] = *p++;
            screen->rows[row][used] = 0;
        }
    }
    return count;
}

static char *retained(int top, int cols, int rows) {
    char *data = NULL; size_t size = 0;
    FILE *out = open_memstream(&data, &size); assert(out);
    tui_swarm_dock_render_retained(out, top, cols, rows);
    assert(!fclose(out)); assert(data); return data;
}

static void test_retained(void) {
    tui_swarm_dock_reset();
    for (int i = 0; i < 4; i++) {
        tui_swarm_dock_update(i, "Task 中文", "model", "running", 0, 0);
        tui_swarm_dock_append(i, "Initial café output", strlen("Initial café output"));
    }
    test_screen_t *actual = calloc(1, sizeof(*actual));
    test_screen_t *expected = calloc(1, sizeof(*expected));
    assert(actual && expected);
    char *out = retained(5, 100, 18);
    size_t full_bytes = strlen(out);
    assert(apply_frame(actual, out) == 18); free(out);
    /* Polling must not consume the separate paint notification. */
    tui_swarm_dock_changed();
    for (int i = 0; i < 20; i++) { out = retained(5, 100, 18); assert(!*out); free(out); }
    tui_swarm_dock_update(0, "Task 中文", "model", "running", 0, 0);
    assert(!tui_swarm_dock_changed());
    out = retained(5, 100, 18); assert(!*out); free(out);
    for (int step = 0; step < 8; step++) {
        if (step == 0) tui_swarm_dock_append(0, "\nnew 中 stream", strlen("\nnew 中 stream"));
        if (step == 1) tui_swarm_dock_toggle_focus();
        if (step == 2) tui_swarm_dock_key(TUI_SWARM_KEY_RIGHT, TUI_SWARM_MOD_SHIFT);
        if (step == 3) tui_swarm_dock_key(TUI_SWARM_KEY_LEFT, TUI_SWARM_MOD_CTRL);
        if (step == 4) tui_swarm_dock_key(TUI_SWARM_KEY_TAB, 0);
        if (step == 5) tui_swarm_dock_update(1, NULL, NULL, "failed", 0, 0);
        if (step == 6) tui_swarm_dock_key(TUI_SWARM_KEY_Z, 0);
        if (step == 7) tui_swarm_dock_key(TUI_SWARM_KEY_Z, 0);
        assert(tui_swarm_dock_changed());
        out = retained(5, 100, 18);
        int changed_rows = apply_frame(actual, out);
        if (!step) { assert(changed_rows > 0 && changed_rows < 18); assert(strlen(out) < full_bytes / 2); }
        free(out);
        out = render(100, 18); apply_frame(expected, out); free(out);
        assert(!memcmp(actual, expected, sizeof(*actual)));
        /* A snapshot is not a terminal paint and invalidates its relative cache. */
        out = retained(5, 100, 18); assert(apply_frame(actual, out) == 18); free(out);
    }
    tui_swarm_dock_changed();
    tui_swarm_dock_invalidate();
    assert(tui_swarm_dock_changed());
    tui_swarm_dock_invalidate();
    assert(!tui_swarm_dock_changed()); /* no absent-dock repaint loop */
    out = retained(5, 100, 18); assert(apply_frame(actual, out) == 18); free(out);
    out = retained(6, 100, 18); assert(apply_frame(actual, out) == 18); free(out);
    out = retained(6, 101, 19); assert(apply_frame(actual, out) == 19); free(out);
    tui_swarm_dock_show(false);
    out = retained(6, 101, 19); assert(!*out); free(out);
    tui_swarm_dock_show(true);
    out = retained(6, 101, 19); assert(apply_frame(actual, out) == 19); free(out);
    tui_swarm_dock_reset();
    tui_swarm_dock_update(0, "New run", "model", "running", 0, 0);
    out = retained(6, 101, 19); assert(apply_frame(actual, out) == 19); free(out);
    free(actual); free(expected);
}

static void test_auto_arrange_and_progress(void) {
    tui_swarm_dock_reset();
    char note[384];
    assert(!tui_swarm_dock_progress(1, note, sizeof(note)));
    for (int i = 0; i < 4; i++) tui_swarm_dock_update(i, "Inspect source", "model", "running", 0, 0);
    char *out = render(120, 20); free(out);
    out = render(58, 12); free(out);
    out = render(121, 21); free(out);
    tui_swarm_dock_snapshot_t a = snapshot(0), b = snapshot(1), d = snapshot(3);
    assert(a.x == 0 && a.width == 60 && a.height == 9);
    assert(b.x == 60 && b.width == 61);
    assert(d.y == 10 && d.height == 10); /* no lost odd row or column */
    tui_swarm_dock_toggle_focus();
    tui_swarm_dock_key(TUI_SWARM_KEY_RIGHT, TUI_SWARM_MOD_SHIFT);
    int moved = snapshot(0).x;
    out = render(141, 23); free(out);
    assert(snapshot(0).x == moved && snapshot(0).width == 60);
    assert(snapshot(1).x == 70 && snapshot(1).width == 71);
    tui_swarm_dock_key(TUI_SWARM_KEY_R, 0);
    assert(snapshot(0).x == 0 && snapshot(0).width == 70);
    assert(tui_swarm_dock_progress(100, note, sizeof(note)));
    assert(strstr(note, "4 active") && strstr(note, "started") && strstr(note, "Inspect source"));
    out = render(141, 23); assert(strstr(out, "Swarm:")); free(out);
    assert(!tui_swarm_dock_progress(114.999, note, sizeof(note)));
    out = render(141, 23); assert(!strstr(out, "Swarm:")); free(out);
    assert(tui_swarm_dock_progress(115, note, sizeof(note)));
    assert(strstr(note, "no new output"));
    tui_swarm_dock_append(1, "received", 8);
    assert(!tui_swarm_dock_progress(129.99, note, sizeof(note)));
    assert(tui_swarm_dock_progress(130, note, sizeof(note)));
    assert(strstr(note, "output received"));
    for (int i = 0; i < 4; i++) tui_swarm_dock_update(i, NULL, NULL, i == 3 ? "failed" : "done", 0, 0);
    assert(tui_swarm_dock_progress(131, note, sizeof(note)));
    assert(strstr(note, "0 active / 3 finished / 1 failed"));
    assert(!tui_swarm_dock_progress(132, note, sizeof(note)));
    tui_swarm_dock_update(0, NULL, NULL, "running", 0, 0);
    tui_swarm_dock_show(false);
    assert(!tui_swarm_dock_progress(150, note, sizeof(note)));
    tui_swarm_dock_show(true);
    assert(tui_swarm_dock_progress(151, note, sizeof(note)));
    assert(!tui_swarm_dock_progress(152, NULL, 0));
    tui_swarm_dock_reset();
    assert(!tui_swarm_dock_progress(200, note, sizeof(note)));
}

static void test_completed_compaction(void) {
    tui_swarm_dock_reset();
    tui_swarm_dock_update(1, "Review", "model", "running", 0, 0);
    assert(tui_swarm_dock_height(120, 40, 4) == 20);
    tui_swarm_dock_append(1, "retained result", 15);
    tui_swarm_dock_update(1, NULL, NULL, "done", 15, 0);
    assert(tui_swarm_dock_height(120, 40, 4) == 6);
    assert(!strcmp(snapshot(1).tail, "retained result"));
    tui_swarm_dock_toggle_focus();
    assert(tui_swarm_dock_height(120, 40, 4) == 20);
    tui_swarm_dock_key(TUI_SWARM_KEY_ESC, 0);
    assert(tui_swarm_dock_height(120, 40, 4) == 6);
    tui_swarm_dock_update(2, "Attention", "model", "failed", 0, 0);
    assert(tui_swarm_dock_height(120, 40, 4) == 20);
    tui_swarm_dock_update(2, NULL, NULL, "running", 0, 0);
    assert(tui_swarm_dock_height(120, 40, 4) == 20);
    tui_swarm_dock_update(2, NULL, NULL, "completed", 0, 0);
    assert(tui_swarm_dock_height(120, 40, 4) == 6);
    char *out = render(120, 6); free(out);
    tui_swarm_dock_toggle_focus();
    tui_swarm_dock_key(TUI_SWARM_KEY_RIGHT, TUI_SWARM_MOD_SHIFT);
    tui_swarm_dock_key(TUI_SWARM_KEY_ESC, 0);
    assert(tui_swarm_dock_height(120, 40, 4) == 20);
    tui_swarm_dock_toggle_focus();
    tui_swarm_dock_key(TUI_SWARM_KEY_R, 0);
    tui_swarm_dock_key(TUI_SWARM_KEY_Z, 0);
    tui_swarm_dock_key(TUI_SWARM_KEY_ESC, 0);
    assert(tui_swarm_dock_height(120, 40, 4) == 30); /* unfocused zoom remains explicit */
    tui_swarm_dock_toggle_focus();
    tui_swarm_dock_key(TUI_SWARM_KEY_Z, 0);
    tui_swarm_dock_key(TUI_SWARM_KEY_ESC, 0);
    for (int i = 3; i < 8; i++) tui_swarm_dock_update(i, "Off page", "m", "done", 0, 0);
    tui_swarm_dock_update(7, NULL, NULL, "timeout", 0, 0);
    assert(tui_swarm_dock_height(120, 40, 4) == 20); /* failures anywhere retain space */
}

static void test_history_dismissal_and_capacity(void) {
    tui_swarm_dock_reset();
    for (int i = 0; i < 10; i++) {
        tui_swarm_dock_update(i, "finished task", "local", "done", 9, 0);
        tui_swarm_dock_append(i, "old proof", 9);
    }
    tui_swarm_dock_update(10, "live task", "local", "running", 0, 0);
    tui_swarm_dock_update(11, "failure task", "local", "failed", 9, 0);
    tui_swarm_dock_append(11, "bad proof", 9);
    char *out = render(120, 20);
    assert(strstr(out, "live task") && strstr(out, "failure task"));
    assert(!strstr(out, "finished task")); free(out);
    assert(snapshot(10).view_count == 2 && snapshot(10).selected_id == 10);
    assert(snapshot(10).x == 0 && snapshot(11).x == 60);
    tui_swarm_dock_toggle_focus();
    tui_swarm_dock_key(TUI_SWARM_KEY_X, 0);
    assert(!snapshot(10).dismissed); /* Never cancel/hide active work. */
    tui_swarm_dock_key(TUI_SWARM_KEY_TAB, 0);
    assert(snapshot(11).selected_id == 11);
    tui_swarm_dock_key(TUI_SWARM_KEY_X, 0);
    assert(snapshot(11).dismissed && snapshot(11).view_count == 1);
    assert(!strcmp(snapshot(11).status, "failed") && !strcmp(snapshot(11).tail, "bad proof"));
    tui_swarm_dock_update(11, NULL, NULL, "failed", 9, 0); /* duplicate update stays dismissed */
    assert(snapshot(11).dismissed);
    tui_swarm_dock_key(TUI_SWARM_KEY_H, 0);
    assert(snapshot(11).history_view && snapshot(11).view_count == 12);
    tui_swarm_dock_key(TUI_SWARM_KEY_TAB, 0); /* from live 10 to dismissed 11 */
    assert(snapshot(11).selected_id == 11);
    out = render(120, 20);
    assert(strstr(out, "HISTORY") && strstr(out, "dismissed") && strstr(out, "bad proof")); free(out);
    tui_swarm_dock_key(TUI_SWARM_KEY_H, 0);
    out = render(120, 20); free(out);
    assert(snapshot(10).selected_id == 10 && snapshot(10).page == 0);
    /* Mouse hit testing uses visible slots, not retained array indices. */
    tui_swarm_dock_update(11, "retry", NULL, "running", 0, 0);
    assert(!snapshot(11).dismissed && !*snapshot(11).tail);
    out = render(120, 20); free(out);
    assert(tui_swarm_dock_mouse(0, 65, 8, false));
    assert(snapshot(11).selected_id == 11);
    tui_swarm_dock_mouse(0, 65, 8, true);
    tui_swarm_dock_update(10, NULL, NULL, "done", 0, 0);
    char note[384];
    assert(tui_swarm_dock_progress(1, note, sizeof(note)));
    tui_swarm_dock_update(11, NULL, NULL, "killed", 0, 0);
    assert(tui_swarm_dock_progress(2, note, sizeof(note)));
    assert(strstr(note, "0 active"));
    tui_swarm_dock_key(TUI_SWARM_KEY_X, 0);
    tui_swarm_dock_key(TUI_SWARM_KEY_ESC, 0);
    assert(snapshot(11).view_count == 0 && tui_swarm_dock_height(120, 40, 4) == 6);
    out = render(120, 6); assert(strstr(out, "output retained")); free(out);
    tui_swarm_dock_toggle_focus();
    assert(snapshot(11).history_view && snapshot(11).view_count == 12);
    assert(!strcmp(snapshot(0).tail, "old proof"));

    tui_swarm_dock_reset();
    tui_swarm_dock_update(0, "oldest active", "local", "running", 0, 0);
    for (int i = 1; i < 64; i++) tui_swarm_dock_update(i, "history", "local", "done", 0, 0);
    tui_swarm_dock_update(64, "new active", "local", "running", 0, 0);
    tui_swarm_dock_snapshot_t missing;
    assert(tui_swarm_dock_snapshot(0, &missing));
    assert(!tui_swarm_dock_snapshot(1, &missing));
    assert(snapshot(64).count == 64 && snapshot(64).selected_id == 0);
    tui_swarm_dock_toggle_focus();
    tui_swarm_dock_key(TUI_SWARM_KEY_H, 0);
    /* Dismiss a later terminal entry; it must be reclaimed before older history. */
    tui_swarm_dock_key(TUI_SWARM_KEY_TAB, 0);
    tui_swarm_dock_key(TUI_SWARM_KEY_TAB, 0);
    assert(snapshot(3).selected_id == 3);
    tui_swarm_dock_key(TUI_SWARM_KEY_X, 0);
    tui_swarm_dock_update(65, "new active", "local", "running", 0, 0);
    assert(!tui_swarm_dock_snapshot(3, &missing));
    assert(tui_swarm_dock_snapshot(2, &missing) && snapshot(0).selected_id == 0);

    tui_swarm_dock_reset();
    for (int i = 0; i < 64; i++) tui_swarm_dock_update(i, "active", "local", "running", 0, 0);
    tui_swarm_dock_update(64, "overflow", "local", "running", 0, 0);
    tui_swarm_dock_append(64, "overflow", 8);
    assert(!tui_swarm_dock_snapshot(64, &missing));
    for (int i = 0; i < 64; i++) assert(tui_swarm_dock_snapshot(i, &missing));
    assert(snapshot(0).capacity_limited && snapshot(0).count == 64);
    out = render(80, 20); assert(strstr(out, "capacity limit")); free(out);
    assert(tui_swarm_dock_progress(1, note, sizeof(note)) && strstr(note, "Swarm (retained)"));
    tui_swarm_dock_update(12, NULL, NULL, "done", 0, 0);
    tui_swarm_dock_update(64, "overflow admitted", "local", "running", 0, 0);
    assert(tui_swarm_dock_snapshot(64, &missing));
    assert(!tui_swarm_dock_snapshot(12, &missing));
    assert(!strcmp(snapshot(0).status, "running"));
}

static void test_lifecycle_retained_frames(void) {
    tui_swarm_dock_reset();
    for (int i = 0; i < 6; i++) tui_swarm_dock_update(i, "result", "local", "running", 0, 0);
    test_screen_t *actual = calloc(1, sizeof(*actual)), *expected = calloc(1, sizeof(*expected));
    assert(actual && expected);
    char *out = retained(5, 101, 19); apply_frame(actual, out); free(out);
    for (int step = 0; step < 5; step++) {
        if (step == 0) {
            tui_swarm_dock_update(0, NULL, NULL, "done", 0, 0);
            tui_swarm_dock_update(2, NULL, NULL, "done", 0, 0);
        } else if (step == 1) {
            tui_swarm_dock_toggle_focus(); tui_swarm_dock_key(TUI_SWARM_KEY_H, 0);
        } else if (step == 2) {
            tui_swarm_dock_key(TUI_SWARM_KEY_TAB, 0); tui_swarm_dock_key(TUI_SWARM_KEY_X, 0);
            assert(snapshot(2).dismissed);
        } else if (step == 3) tui_swarm_dock_key(TUI_SWARM_KEY_H, 0);
        else tui_swarm_dock_key(TUI_SWARM_KEY_H, 0);
        out = retained(5, 101, 19); apply_frame(actual, out); free(out);
        out = render(101, 19); apply_frame(expected, out); free(out);
        for (int row = 0; row < 40; row++) assert(!strcmp(actual->rows[row], expected->rows[row]));
        /* Snapshot invalidates the cache; re-prime it before the next mutation. */
        out = retained(5, 101, 19); apply_frame(actual, out); free(out);
    }
    free(actual); free(expected);
}

int main(void) {
    test_history_dismissal_and_capacity();
    test_lifecycle_retained_frames();
    test_completed_compaction();
    test_auto_arrange_and_progress();
    test_retained();
    assert(setlocale(LC_CTYPE, "") != NULL);
    test_lifecycle(); test_id_reuse_and_locale(); test_layout_keyboard(); test_mouse();
    test_stream_sanitization(); test_bounds_concurrency();
    puts("tui swarm dock: lifecycle, layout, input, UTF-8, sanitization and concurrency passed");
    return 0;
}
