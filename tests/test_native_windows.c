/* Standalone retained-window model regression: no renderer, terminal, tool,
 * provider, buffer, or filesystem activity. */
#include "native_windows.h"
#include "json_util.h"
#include "../vendor/yyjson.h"
#include <inttypes.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
static char result[128 * 1024];
static const native_ui_rect_t WORK = {20, 30, 1000, 700};

static void check(bool pass, const char *message) {
    checks++;
    if (!pass) { fprintf(stderr, "FAIL %u: %s\n%s\n", checks, message, result); exit(1); }
}

static native_windows_snapshot_t snapshot(void) {
    native_windows_snapshot_t snap;
    memset(&snap, 0, sizeof(snap));
    native_windows_snapshot(&snap);
    return snap;
}

static native_window_t window(uint64_t id) {
    native_windows_snapshot_t snap = snapshot();
    for (int i = 0; i < snap.count; i++) if (snap.windows[i].id == id) return snap.windows[i];
    check(false, "window ID exists in snapshot");
    return (native_window_t){0};
}

static bool rect_equal(native_ui_rect_t a, native_ui_rect_t b) {
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}

static bool inside(native_ui_rect_t r, native_ui_rect_t area) {
    return r.width > 0 && r.height > 0 && r.x >= area.x &&
        r.y >= area.y + NATIVE_WINDOWS_TOOLBAR_HEIGHT &&
        r.x + r.width <= area.x + area.width && r.y + r.height <= area.y + area.height;
}

static bool command(const char *json) {
    bool ok = native_windows_command(json, result, sizeof(result));
    yyjson_doc *doc = yyjson_read(result, strlen(result), 0);
    check(doc && yyjson_is_obj(yyjson_doc_get_root(doc)), "command returns valid JSON object");
    yyjson_val *flag = yyjson_obj_get(yyjson_doc_get_root(doc), "ok");
    if (flag) check(yyjson_is_bool(flag) && yyjson_get_bool(flag) == ok, "JSON ok agrees with command return");
    yyjson_doc_free(doc);
    return ok;
}

static void success(const char *json) { check(command(json), json); }
static void reject(const char *json) { check(!command(json), json); }

static void id_command(const char *action, uint64_t id, const char *extra) {
    char request[2048];
    snprintf(request, sizeof(request), "{\"action\":\"%s\",\"id\":%" PRIu64 "%s}", action, id, extra ? extra : "");
    success(request);
}

static uint64_t open_window(const char *name, const char *kind, int x, int y, int width, int height) {
    char request[1024];
    snprintf(request, sizeof(request), "{\"action\":\"open\",\"title\":\"%s\",\"kind\":\"%s\","
             "\"text\":\"Owned objective\",\"x\":%d,\"y\":%d,\"width\":%d,\"height\":%d}",
             name, kind, x, y, width, height);
    success(request);
    native_windows_snapshot_t snap = snapshot();
    check(snap.count > 0 && snap.focused_id, "opening focuses a retained window");
    check(snap.windows[snap.count - 1].id == snap.focused_id, "focused window is at snapshot front");
    return snap.focused_id;
}

static void reset(void) {
    native_windows_reset();
    native_windows_set_work_area(WORK);
    check(!native_windows_action_pending(), "new session has no pending actions");
}

static void unchanged_windows(native_windows_snapshot_t before) {
    native_windows_snapshot_t after = snapshot();
    check(before.count == after.count && before.focused_id == after.focused_id && before.visible == after.visible,
          "rejected/read-only command leaves window inventory and focus intact");
    for (int i = 0; i < before.count; i++) {
        native_window_t *a = &before.windows[i], *b = &after.windows[i];
        check(a->id == b->id && a->revision == b->revision && rect_equal(a->rect, b->rect) &&
              rect_equal(a->restore_rect, b->restore_rect) && a->kind == b->kind && a->status == b->status &&
              a->zoomed == b->zoomed && a->scroll == b->scroll && !strcmp(a->title, b->title) &&
              !strcmp(a->text, b->text) && !strcmp(a->evidence, b->evidence) && !strcmp(a->next_step, b->next_step),
              "rejected/read-only command does not partially mutate a window");
    }
}

static void test_geometry_focus_lifecycle(void) {
    reset();
    uint64_t first = open_window("First", "note", 0, 0, 16384, 16384);
    native_window_t a = window(first);
    check(inside(a.rect, WORK) && a.rect.width == WORK.width &&
          a.rect.height == WORK.height - NATIVE_WINDOWS_TOOLBAR_HEIGHT,
          "oversized logical geometry fits work area below toolbar");
    id_command("resize", first, ",\"width\":400,\"height\":240");
    id_command("move", first, ",\"x\":100,\"y\":100");
    uint64_t second = open_window("Second", "note", 200, 150, 400, 260);
    check(second > first, "new window gets a fresh increasing ID");
    native_windows_snapshot_t snap = snapshot();
    check(snap.count == 2 && snap.windows[0].id == first && snap.windows[1].id == second,
          "snapshot order is back to front");
    check(native_windows_pointer(0, 250, 200, false), "overlap press is handled");
    native_windows_pointer(0, 250, 200, true);
    check(snapshot().focused_id == second, "overlap hit targets the front window");
    check(native_windows_pointer(0, 120, 200, false), "exposed rear window hit is handled");
    native_windows_pointer(0, 120, 200, true);
    snap = snapshot();
    check(snap.focused_id == first && snap.windows[1].id == first, "click raises rear window without losing its ID");
    id_command("move", first, ",\"x\":16384,\"y\":16384");
    a = window(first); check(inside(a.rect, WORK), "move clamps right/bottom edges");
    id_command("close", first, NULL);
    check(snapshot().count == 1 && snapshot().focused_id == second, "closing focused pane selects surviving front pane");
    uint64_t third = open_window("Third", "note", 90, 100, 360, 200);
    check(third > second && third != first, "closed IDs are never reused in a session");
    char stale[128]; snprintf(stale, sizeof(stale), "{\"action\":\"focus\",\"id\":%" PRIu64 "}", first);
    snap = snapshot(); reject(stale); unchanged_windows(snap);
    id_command("close", second, NULL); id_command("close", third, NULL);
    check(snapshot().count == 0, "close removes only retained windows");
    success("{\"action\":\"show\"}");
    snap = snapshot(); check(snap.visible && snap.count > 0, "show on empty model creates a usable starter panel");
    check(snap.windows[0].id > third, "starter uses a fresh session-local ID");
}

static void test_drag_resize_zoom_and_layout(void) {
    reset();
    uint64_t id = open_window("Drag target", "workflow", 200, 160, 400, 260);
    native_window_t a = window(id);
    int x = a.rect.x + 70, y = a.rect.y + NATIVE_WINDOW_TITLE_HEIGHT / 2;
    check(native_windows_pointer(0, x, y, false), "title press begins drag");
    check(snapshot().captured_id == id, "title press captures exact window");
    check(native_windows_pointer(32, -1000, -1000, false), "captured drag follows pointer outside work area");
    a = window(id); check(a.rect.x == WORK.x && a.rect.y == WORK.y + NATIVE_WINDOWS_TOOLBAR_HEIGHT,
                         "out-of-area title drag clamps top/left");
    native_windows_pointer(0, -1000, -1000, true);
    check(snapshot().captured_id == 0, "out-of-area release clears capture");
    a = window(id);
    check(native_windows_pointer(0, a.rect.x + a.rect.width - 2, a.rect.y + a.rect.height - 2, false),
          "bottom-right resize corner has pointer priority");
    native_windows_pointer(32, -1000, -1000, false);
    a = window(id); check(a.rect.width == 240 && a.rect.height == 160, "corner resize preserves minimum usable geometry");
    native_windows_pointer(0, -1000, -1000, true);
    check(!native_windows_action_pending(), "resize corner never accidentally clicks workflow footer");
    a = window(id);
    native_windows_pointer(0, a.rect.x + a.rect.width - 2, a.rect.y + a.rect.height - 2, false);
    native_windows_pointer(32, 20000, 20000, false);
    native_windows_pointer(0, 20000, 20000, true);
    check(inside(window(id).rect, WORK), "large corner resize fits available work area");
    id_command("resize", id, ",\"width\":420,\"height\":240");
    id_command("move", id, ",\"x\":120,\"y\":130");
    native_ui_rect_t restored = window(id).rect;
    id_command("zoom", id, NULL);
    a = window(id); check(a.zoomed && a.rect.width == WORK.width &&
                         a.rect.height == WORK.height - NATIVE_WINDOWS_TOOLBAR_HEIGHT, "zoom fills usable area");
    id_command("zoom", id, NULL); a = window(id);
    check(!a.zoomed && rect_equal(a.rect, restored), "unzoom restores exact pre-zoom geometry");
    id_command("zoom", id, NULL);
    native_ui_rect_t small = {10, 20, 300, 180}; native_windows_set_work_area(small);
    a = window(id); check(a.zoomed && inside(a.rect, small), "zoom tracks work-area resize");
    id_command("zoom", id, NULL); a = window(id);
    check(!a.zoomed && inside(a.rect, small) && a.rect.height == small.height - NATIVE_WINDOWS_TOOLBAR_HEIGHT,
          "restore after work-area shrink clamps size below nominal minimum");
    native_windows_set_work_area(WORK);
    open_window("Tile 2", "note", 200, 150, 360, 220);
    open_window("Tile 3", "note", 240, 180, 360, 220);
    open_window("Tile 4", "note", 280, 210, 360, 220);
    success("{\"action\":\"tile\"}");
    native_windows_snapshot_t snap = snapshot();
    for (int i = 0; i < snap.count; i++) {
        check(inside(snap.windows[i].rect, WORK), "tiled panel is contained");
        for (int j = i + 1; j < snap.count; j++) {
            native_ui_rect_t p = snap.windows[i].rect, q = snap.windows[j].rect;
            check(p.x + p.width <= q.x || q.x + q.width <= p.x || p.y + p.height <= q.y || q.y + q.height <= p.y,
                  "tile gives panels nonoverlapping logical areas");
        }
    }
    success("{\"action\":\"cascade\"}"); snap = snapshot();
    for (int i = 0; i < snap.count; i++) check(inside(snap.windows[i].rect, WORK), "cascade stays inside work area");
    check(snap.windows[0].rect.x != snap.windows[1].rect.x || snap.windows[0].rect.y != snap.windows[1].rect.y,
          "cascade exposes distinct title positions");
    a = window(snap.focused_id);
    native_windows_pointer(0, a.rect.x + 70, a.rect.y + 10, false);
    native_windows_cancel_gesture(); check(!snapshot().captured_id, "explicit gesture cancellation clears capture");
}

static void assert_tiles_contained(native_ui_rect_t area) {
    native_windows_snapshot_t snap = snapshot();
    for (int i = 0; i < snap.count; i++) {
        native_ui_rect_t a = snap.windows[i].rect;
        check(inside(a, area), "retiled panel stays within resized work area");
        for (int j = i + 1; j < snap.count; j++) {
            native_ui_rect_t b = snap.windows[j].rect;
            check(a.x + a.width <= b.x || b.x + b.width <= a.x ||
                  a.y + a.height <= b.y || b.y + b.height <= a.y,
                  "work-area resize preserves nonoverlapping tiled panels");
        }
    }
}

static uint64_t tiled_fixture(int count) {
    reset();
    uint64_t first = 0;
    for (int i = 0; i < count; i++) {
        char title[32]; snprintf(title, sizeof(title), "Retained tile %d", i);
        uint64_t id = open_window(title, "note", 100, 100, 400, 240);
        if (!i) first = id;
    }
    success("{\"action\":\"tile\"}");
    return first;
}

static void test_retained_layout_on_work_area_resize(void) {
    uint64_t first = tiled_fixture(4);
    uint64_t focused = snapshot().focused_id;
    native_ui_rect_t small = {10, 20, 360, 240};
    native_windows_set_work_area(small);
    assert_tiles_contained(small);
    check(snapshot().count == 4 && snapshot().focused_id == focused && window(first).id == first,
          "retiling preserves window identities and focus");
    check(window(first).rect.width < 240 && window(first).rect.height < 160,
          "small tiled cells fit without manual resize minima causing overlap");
    native_windows_set_work_area(WORK);
    assert_tiles_contained(WORK);
    check(window(first).rect.width > 240 && window(first).rect.height > 160,
          "tiles expand again when work area grows");

    first = tiled_fixture(3);
    native_ui_rect_t before_close = window(first).rect;
    id_command("close", snapshot().focused_id, NULL);
    assert_tiles_contained(WORK);
    check(snapshot().count == 2 && window(first).rect.height > before_close.height,
          "closing a tiled panel redistributes its vacant area to surviving tiles");

    /* Enlarging an area leaves every existing manual rectangle valid, making
     * any unintended automatic layout change directly observable. */
    const char *manual[] = {"move", "resize", "drag", "grip", "zoom", "cascade", "open", "keyboard"};
    native_ui_rect_t larger = {WORK.x, WORK.y, 1300, 900};
    for (size_t i = 0; i < sizeof(manual) / sizeof(manual[0]); i++) {
        first = tiled_fixture(4);
        id_command("focus", first, NULL);
        native_window_t pane = window(first);
        if (!strcmp(manual[i], "move")) id_command("move", first, ",\"x\":40,\"y\":80");
        else if (!strcmp(manual[i], "resize")) id_command("resize", first, ",\"width\":300,\"height\":200");
        else if (!strcmp(manual[i], "drag") || !strcmp(manual[i], "grip")) {
            bool grip = !strcmp(manual[i], "grip");
            int x = pane.rect.x + (grip ? pane.rect.width - 2 : 70);
            int y = pane.rect.y + (grip ? pane.rect.height - 2 : 10);
            check(native_windows_pointer(0, x, y, false), "manual tiled gesture captures panel");
            check(native_windows_pointer(32, x + 20, y + 20, false), "manual tiled gesture moves or resizes panel");
            check(native_windows_pointer(0, x + 20, y + 20, true), "manual tiled gesture releases capture");
        } else if (!strcmp(manual[i], "zoom")) {
            id_command("zoom", first, NULL); id_command("zoom", first, NULL);
        } else if (!strcmp(manual[i], "cascade")) success("{\"action\":\"cascade\"}");
        else if (!strcmp(manual[i], "open")) open_window("Manual new panel", "note", 100, 100, 400, 240);
        else {
            check(native_windows_key(NATIVE_WINDOW_KEY_TOGGLE_FOCUS, 0), "keyboard arrangement takes focus");
            check(native_windows_key(NATIVE_WINDOW_KEY_RIGHT, 0), "keyboard movement exits automatic tiling");
        }
        native_windows_snapshot_t before = snapshot();
        native_windows_set_work_area(larger);
        native_windows_snapshot_t after = snapshot();
        check(after.count == before.count && after.focused_id == before.focused_id,
              "work-area growth preserves manually arranged inventory and focus");
        for (int j = 0; j < before.count; j++) {
            check(after.windows[j].id == before.windows[j].id &&
                  rect_equal(after.windows[j].rect, before.windows[j].rect),
                  "manual move/resize/drag/zoom/cascade/open is not silently retiled");
        }
        check(!native_windows_action_pending(), "layout transitions never enqueue workflow actions");
    }
}

static void test_coarse_pointer_grip_and_enter_handoff(void) {
    const int cells[][2] = {{20, 20}, {32, 32}, {20, 32}};
    for (size_t i = 0; i < sizeof(cells) / sizeof(cells[0]); i++) {
        reset();
        uint64_t id = open_window("Coarse pointer", "workflow", 100, 100, 400, 260);
        native_windows_set_pointer_cell(cells[i][0], cells[i][1]);
        native_ui_rect_t before = window(id).rect;
        /* A terminal reports whole cells. Exercise the full one-cell-plus-one
         * grip boundary, farther from the edge than a fixed 16px target. */
        int x = before.x + before.width - cells[i][0] - 1;
        int y = before.y + before.height - cells[i][1] - 1;
        check(native_windows_pointer(0, x, y, false), "coarse-cell corner press is handled");
        check(snapshot().captured_id == id && native_windows_focused(),
              "corner press captures exact pane and gives it keyboard focus");
        check(native_windows_pointer(32, x + cells[i][0], y + cells[i][1], false),
              "coarse-cell corner remains a reachable resize target");
        native_ui_rect_t after = window(id).rect;
        check(after.width == before.width + cells[i][0] && after.height == before.height + cells[i][1] &&
              after.x == before.x && after.y == before.y, "coarse corner resizes instead of clicking workflow footer");
        check(native_windows_pointer(0, x + cells[i][0], y + cells[i][1], true), "coarse resize releases capture");
        check(!snapshot().captured_id && !native_windows_action_pending(),
              "coarse resize produces no accidental Inspect action");
    }
    reset();
    uint64_t id = open_window("Enter handoff", "workflow", 100, 100, 400, 260);
    const int enters[] = {'\r', '\n'};
    for (size_t i = 0; i < sizeof(enters) / sizeof(enters[0]); i++) {
        native_ui_rect_t pane = window(id).rect;
        check(native_windows_pointer(0, pane.x + 40, pane.y + 70, false), "body click begins panel keyboard focus");
        check(native_windows_pointer(0, pane.x + 40, pane.y + 70, true), "body click releases without workflow action");
        check(native_windows_focused(), "pointer interaction keeps keyboard with the panel");
        native_windows_snapshot_t before = snapshot();
        check(native_windows_key(enters[i], 0), "first Enter is consumed before it can submit the draft");
        check(!native_windows_focused() && native_windows_visible() && !snapshot().captured_id,
              "first Enter returns focus to draft while preserving visible panels");
        unchanged_windows(before);
        check(!native_windows_key(enters[i], 0), "only a subsequent Enter is passed through to the draft");
        check(!native_windows_action_pending(), "Enter focus handoff never fabricates workflow input");
    }
}

static void test_validation_and_workflow(void) {
    reset();
    uint64_t id = open_window("Unchanged", "note", 100, 100, 400, 240);
    const char *invalid[] = {NULL, "", "[]", "{", "{\"action\":\"bogus\"}",
        "{\"action\":\"show\",\"action\":\"hide\"}", "{\"action\":\"open\",\"unexpected\":1}",
        "{\"action\":\"open\",\"kind\":\"bogus\"}", "{\"action\":\"open\",\"status\":\"bogus\"}",
        "{\"action\":\"open\",\"title\":\"No evidence\",\"status\":\"done\"}",
        "{\"action\":\"open\",\"title\":true}", "{\"action\":\"open\",\"text\":\"bad\\u0000text\"}",
        "{\"action\":\"open\",\"width\":0}", "{\"action\":\"open\",\"x\":-1}",
        "{\"action\":\"focus\",\"id\":0}", "{\"action\":\"focus\",\"id\":1.5}",
        "{\"action\":\"events\",\"since\":-1}", "{\"action\":\"continue\"}"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        native_windows_snapshot_t before = snapshot(); reject(invalid[i]); unchanged_windows(before);
        check(!native_windows_action_pending(), "JSON cannot forge a human action");
    }
    char request[1024];
    snprintf(request, sizeof(request), "{\"action\":\"move\",\"id\":%" PRIu64 ",\"x\":300,\"y\":\"wrong\"}", id);
    native_windows_snapshot_t before = snapshot(); reject(request); unchanged_windows(before);
    snprintf(request, sizeof(request), "{\"action\":\"update\",\"id\":%" PRIu64 ",\"title\":\"Partial mutation\",\"status\":\"done\"}", id);
    reject(request); unchanged_windows(before);
    id_command("update", id, ",\"status\":\"running\",\"next_step\":\"Run the focused check\"");
    native_window_t a = window(id);
    check(a.kind == NATIVE_WINDOW_WORKFLOW && a.status == NATIVE_WINDOW_RUNNING,
          "status/next-step update turns a note into a workflow");
    id_command("update", id, ",\"status\":\"done\",\"evidence\":\"Focused check passed\"");
    a = window(id); check(a.status == NATIVE_WINDOW_DONE && !strcmp(a.evidence, "Focused check passed"),
                         "done is accepted with retained concrete evidence");
    before = snapshot();
    snprintf(request, sizeof(request), "{\"action\":\"update\",\"id\":%" PRIu64 ",\"evidence\":\"\"}", id);
    reject(request); unchanged_windows(before);
    check(!native_windows_action_pending(), "workflow status command never enqueues an action");
    before = snapshot(); success("{\"action\":\"list\"}"); unchanged_windows(before);
    success("{\"action\":\"events\"}"); unchanged_windows(before);
    char long_text[NATIVE_WINDOW_TEXT_CAP + 1]; memset(long_text, 'x', sizeof(long_text) - 1); long_text[sizeof(long_text) - 1] = 0;
    jbuf_t wire; jbuf_init(&wire, sizeof(long_text) + 128); jbuf_appendf(&wire, "{\"action\":\"update\",\"id\":%" PRIu64 ",\"text\":", id);
    jbuf_append_json_str(&wire, long_text); jbuf_append_char(&wire, '}');
    reject(wire.data); unchanged_windows(before); jbuf_free(&wire);
}

static void footer_point(native_window_t pane, int part, int *x, int *y) {
    *x = pane.rect.x + (2 * part + 1) * pane.rect.width / 8;
    *y = pane.rect.y + pane.rect.height - NATIVE_WINDOW_FOOTER_HEIGHT + 8;
}

static void click_footer(uint64_t id, int part) {
    native_window_t pane = window(id); int x, y; footer_point(pane, part, &x, &y);
    check(native_windows_pointer(0, x, y, false), "footer press is handled");
    check(native_windows_pointer(0, x, y, true), "footer release is handled");
}

static size_t event_count(uint64_t since) {
    size_t total = 0;
    for (int page = 0; page <= NATIVE_WINDOWS_EVENTS_MAX; page++) {
        char request[128]; snprintf(request, sizeof(request), "{\"action\":\"events\",\"since\":%" PRIu64 "}", since);
        success(request);
        yyjson_doc *doc = yyjson_read(result, strlen(result), 0);
        yyjson_val *root = yyjson_doc_get_root(doc), *events = yyjson_obj_get(root, "events");
        check(yyjson_is_arr(events), "events command returns retained receipts");
        size_t count = yyjson_arr_size(events); check(count <= 16, "event pages have a bounded receipt count");
        uint64_t last = since; size_t i, n; yyjson_val *event;
        yyjson_arr_foreach(events, i, n, event) {
            uint64_t id = yyjson_get_uint(yyjson_obj_get(event, "event_id"));
            check(id > last, "retained receipts follow strictly increasing event IDs after since");
            check(yyjson_is_bool(yyjson_obj_get(event, "delivered")), "receipt delivery state is explicit");
            check(yyjson_equals_str(yyjson_obj_get(event, "source"), "terminal_input"),
                  "receipt provenance identifies terminal input without claiming physical human origin");
            last = id;
        }
        uint64_t next = yyjson_get_uint(yyjson_obj_get(root, "next_since"));
        bool more = yyjson_get_bool(yyjson_obj_get(root, "has_more"));
        check(next == last && (!more || next > since), "event pagination advances without consuming receipts");
        total += count; yyjson_doc_free(doc);
        if (!more) return total;
        since = next;
    }
    check(false, "event pagination terminated"); return 0;
}

static void test_footer_actions_and_retained_events(void) {
    reset();
    uint64_t id = open_window("Workflow action", "workflow", 100, 100, 400, 260);
    id_command("update", id, ",\"next_step\":\"Verify owned fixture\"");
    native_window_t pane = window(id);
    uint64_t first_event = 0, previous = 0;
    for (int part = 0; part < 4; part++) {
        int x, y; footer_point(pane, part, &x, &y);
        check(native_windows_pointer(0, x, y, false), "workflow button accepts pointer press");
        check(!native_windows_action_pending(), "press alone never creates a human receipt");
        check(native_windows_pointer(0, x, y, true), "workflow button accepts release");
        check(native_windows_action_pending(), "matching release queues one human action");
        native_windows_pointer(0, x, y, true);
        native_window_action_t action;
        check(native_windows_pop_action(&action), "pending action is available to event consumer");
        check(action.window_id == id && action.kind == (native_window_action_kind_t)part,
              "four equal footer buttons select Continue/Revise/Retry/Inspect respectively");
        check(action.event_id > previous, "human receipts have increasing event IDs");
        if (!first_event) first_event = action.event_id;
        previous = action.event_id;
        check(!strcmp(action.title, "Workflow action") && !strcmp(action.objective, "Owned objective") &&
              !strcmp(action.next_step, "Verify owned fixture"), "action captures its window's objective and proposed next step");
        native_window_action_t duplicate;
        check(!native_windows_pop_action(&duplicate), "duplicate release cannot enqueue a second action");
        char formatted[4096]; native_windows_format_action(&action, formatted, sizeof(formatted));
        check(strstr(formatted, "Workflow action") && strstr(formatted, "Owned objective"),
              "human action format preserves concrete window context");
        check(strstr(formatted, "automated") && strstr(formatted, "not independent approval"),
              "formatted terminal input does not grant authority based on unverified human provenance");
    }
    check(event_count(0) == 4 && event_count(first_event) == 3 && event_count(previous) == 0,
          "events since filters retained history independently of pending consumption");
    check(!native_windows_action_pending(), "reading consumed receipts does not re-enqueue them");
    int x, y; footer_point(pane, 0, &x, &y);
    native_windows_pointer(0, x, y, false);
    native_windows_pointer(32, WORK.x + WORK.width + 100, WORK.y + WORK.height + 100, false);
    native_windows_pointer(0, WORK.x + WORK.width + 100, WORK.y + WORK.height + 100, true);
    check(!native_windows_action_pending() && !snapshot().captured_id, "release outside cancels footer action and capture");
    native_windows_pointer(0, x, y, false);
    int other_x, other_y; footer_point(pane, 1, &other_x, &other_y);
    native_windows_pointer(0, other_x, other_y, true);
    check(!native_windows_action_pending(), "press one button/release another never activates either");
    for (int side = 0; side < 2; side++) {
        int edge = pane.rect.x + pane.rect.width / 4 - 1 + side;
        native_windows_pointer(0, edge, y, false); native_windows_pointer(0, edge, y, true);
        native_window_action_t action;
        check(native_windows_pop_action(&action) && action.kind == (native_window_action_kind_t)side,
              "quarter-width boundary selects adjacent equal footer regions");
    }
    click_footer(id, NATIVE_WINDOW_INSPECT);
    native_windows_snapshot_t before = snapshot();
    size_t count = event_count(0); unchanged_windows(before);
    yyjson_doc *receipts = yyjson_read(result, strlen(result), 0);
    yyjson_val *events = yyjson_obj_get(yyjson_doc_get_root(receipts), "events");
    yyjson_val *last = yyjson_arr_get_last(events);
    uint64_t pending_event = yyjson_get_uint(yyjson_obj_get(last, "event_id"));
    check(pending_event && yyjson_is_false(yyjson_obj_get(last, "delivered")), "unconsumed human receipt is explicitly undelivered");
    yyjson_doc_free(receipts);
    check(event_count(0) == count && native_windows_action_pending(), "event reads neither consume pending actions nor alter receipts");
    success("{\"action\":\"list\"}"); unchanged_windows(before);
    native_window_action_t pending; check(native_windows_pop_action(&pending) && pending.kind == NATIVE_WINDOW_INSPECT,
                                         "pending human action survives list/events observations");
    check(pending.event_id == pending_event && event_count(pending_event - 1) == 1,
          "consumer receives the exact previously observed retained receipt");
    receipts = yyjson_read(result, strlen(result), 0);
    events = yyjson_obj_get(yyjson_doc_get_root(receipts), "events");
    check(yyjson_is_true(yyjson_obj_get(yyjson_arr_get_first(events), "delivered")), "consumption marks delivery while retaining readable history");
    yyjson_doc_free(receipts);
    check(!native_windows_pop_action(&pending), "observations did not duplicate a pending action");
}

static void test_queue_saturation_and_utf8_action(void) {
    reset();
    uint64_t id = open_window("Queue", "workflow", 100, 100, 400, 260);
    for (int i = 0; i < NATIVE_WINDOWS_EVENTS_MAX; i++) click_footer(id, NATIVE_WINDOW_CONTINUE);
    check(event_count(0) == NATIVE_WINDOWS_EVENTS_MAX, "retained action history reaches its bounded capacity");
    click_footer(id, NATIVE_WINDOW_RETRY);
    check(event_count(0) == NATIVE_WINDOWS_EVENTS_MAX, "saturated pending queue rejects extra actions without false receipt");
    native_window_action_t action; uint64_t previous = 0; int popped = 0;
    while (native_windows_pop_action(&action)) {
        check(action.kind == NATIVE_WINDOW_CONTINUE && action.event_id > previous,
              "saturation preserves queued action order and identities");
        previous = action.event_id; popped++;
    }
    check(popped == NATIVE_WINDOWS_EVENTS_MAX, "exactly 64 accepted actions remain pending at saturation");
    click_footer(id, NATIVE_WINDOW_RETRY);
    check(native_windows_pop_action(&action) && action.kind == NATIVE_WINDOW_RETRY && action.event_id > previous,
          "draining capacity permits a fresh later action");
    check(!native_windows_pop_action(&action), "capacity recovery does not resurrect rejected action");
    jbuf_t wire; jbuf_init(&wire, 2048);
    jbuf_appendf(&wire, "{\"action\":\"update\",\"id\":%" PRIu64 ",\"text\":\"", id);
    for (int i = 0; i < 300; i++) jbuf_append(&wire, "🚀");
    jbuf_append(&wire, "\"}"); success(wire.data); jbuf_free(&wire);
    click_footer(id, NATIVE_WINDOW_INSPECT);
    check(native_windows_pop_action(&action), "long Unicode objective can create a human receipt");
    size_t size = strlen(action.objective);
    check(size > 0 && size < sizeof(action.objective) && size % strlen("🚀") == 0,
          "bounded action objective never splits a UTF-8 glyph");
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_doc_set_root(doc, yyjson_mut_str(doc, action.objective));
    char *encoded = yyjson_mut_write(doc, 0, NULL);
    check(encoded != NULL, "captured Unicode objective remains valid JSON text");
    free(encoded); yyjson_mut_doc_free(doc);
}

static void test_scroll_and_capacity(void) {
    reset();
    uint64_t id = open_window("Scroll", "workflow", 100, 100, 400, 240);
    jbuf_t text; jbuf_init(&text, 8192);
    jbuf_appendf(&text, "{\"action\":\"update\",\"id\":%" PRIu64 ",\"text\":\"", id);
    for (int i = 0; i < 200; i++) jbuf_appendf(&text, "Line %d\\n", i);
    jbuf_append(&text, "\"}"); success(text.data); jbuf_free(&text);
    id_command("scroll", id, ",\"lines\":100000");
    check(window(id).scroll > 0 && window(id).scroll <= 200, "large scroll clamps to retained content");
    id_command("scroll", id, ",\"lines\":-100000"); check(window(id).scroll == 0, "reverse scroll clamps to start");
    native_window_t pane = window(id);
    native_windows_pointer(65, pane.rect.x + 20, pane.rect.y + 50, false);
    check(window(id).scroll > 0, "wheel down scrolls hovered content");
    native_windows_pointer(64, pane.rect.x + 20, pane.rect.y + 50, false);
    check(window(id).scroll == 0, "wheel up reverses bounded scroll");
    check(!native_windows_action_pending(), "scroll events cannot trigger workflow actions");
    id_command("scroll", id, ",\"lines\":100000");
    id_command("update", id, ",\"text\":\"Short\"");
    check(window(id).scroll == 0, "shorter replacement content clamps stale scroll position");
    for (int i = 1; i < NATIVE_WINDOWS_MAX; i++) {
        char name[32]; snprintf(name, sizeof(name), "Capacity %d", i);
        open_window(name, "note", 100, 100, 300, 180);
    }
    native_windows_snapshot_t before = snapshot();
    check(before.count == NATIVE_WINDOWS_MAX, "retained window capacity is available");
    reject("{\"action\":\"open\",\"title\":\"One too many\"}"); unchanged_windows(before);
    id_command("close", id, NULL);
    uint64_t next = open_window("Capacity restored", "note", 100, 100, 300, 180);
    check(next > before.windows[before.count - 1].id, "reused capacity still receives a fresh ID");
}

static void wrapped_rows(const char *text, int columns, const char *const *expected, size_t count) {
    const char *cursor = text, *start = NULL;
    size_t bytes = 0;
    for (size_t i = 0; i < count; i++) {
        const char *before = cursor;
        check(native_window_text_next(&cursor, columns, &start, &bytes), "word-wrap iterator yields expected row");
        check(start >= before && cursor > before && cursor <= text + strlen(text),
              "word-wrap row borrows input and advances within its bounds");
        check(bytes == strlen(expected[i]) && !memcmp(start, expected[i], bytes),
              "word-wrap row preserves expected text and whole UTF-8 codepoints");
    }
    check(!native_window_text_next(&cursor, columns, &start, &bytes),
          "word-wrap iterator stops without an extra phantom row");
}

static void test_word_wrap_and_exact_scroll(void) {
    const char *words[] = {"alpha", "beta gamma"};
    wrapped_rows("alpha  beta gamma", 10, words, 2);
    const char *tabs[] = {"one\t two", "three"};
    wrapped_rows("one\t two   three", 8, tabs, 2);
    const char *long_word[] = {"🚀é猫", "abc", "def"};
    wrapped_rows("🚀é猫abcdef", 3, long_word, 3);
    const char *blank_lines[] = {"", "alpha", "", "beta"};
    wrapped_rows("\nalpha\n\nbeta\n", 5, blank_lines, 4);
    const char *exact[] = {"12345"};
    wrapped_rows("12345", 5, exact, 1);
    wrapped_rows("12345\n", 5, exact, 1);
    wrapped_rows("", 5, NULL, 0);
    const char *narrow[] = {"é", "🚀"};
    wrapped_rows("é🚀", 0, narrow, 2);

    reset();
    uint64_t id = open_window("Exact scroll", "workflow", 100, 100, 240, 160);
    /* This pane has 27 text columns and three visible 18px body rows. */
    id_command("update", id, ",\"text\":\"123456789012345678901234567\\n123456789012345678901234567\\nBody final\"");
    id_command("scroll", id, ",\"lines\":100000");
    check(window(id).scroll == 0, "exact-column lines do not create phantom rows or unnecessary scrolling");
    id_command("update", id, ",\"text\":\"one two three four five six seven\\n\\n123456789012345678901234567\\nBody final\"");
    id_command("scroll", id, ",\"lines\":100000");
    check(window(id).scroll == 2, "wrapped body and explicit blank row leave the final body row reachable");
    id_command("update", id, ",\"next_step\":\"Next first\\nNext final\",\"evidence\":\"Evidence final\"");
    id_command("scroll", id, ",\"lines\":100000");
    check(window(id).scroll == 9,
          "scroll counts all 12 body/heading/blank/evidence rows, exposing final evidence in the last viewport");
    id_command("resize", id, ",\"width\":240,\"height\":196");
    check(window(id).scroll == 7, "five-row resized viewport clamps scroll while keeping the final row reachable");
    id_command("update", id, ",\"evidence\":\"\"");
    check(window(id).scroll == 4, "removing evidence also removes its heading and blank row from scroll bounds");
    id_command("scroll", id, ",\"lines\":-100000");
    check(window(id).scroll == 0 && !native_windows_action_pending(), "exact scrolling returns to start without terminal actions");
}

static void test_maximum_escaped_text_payload(void) {
    reset();
    uint64_t id = open_window("Escaped text", "note", 100, 100, 400, 240);
    jbuf_t wire; jbuf_init(&wire, 64 * 1024);
    jbuf_appendf(&wire, "{\"action\":\"update\",\"id\":%" PRIu64 ",\"text\":\"", id);
    for (int i = 0; i < 8185; i++) jbuf_append(&wire, "\\u0061");
    jbuf_append(&wire, "\\u00e9\\ud83d\\ude80\"}");
    check(wire.len > 32 * 1024 && wire.len < 128 * 1024, "valid escaped text can exceed the previous JSON input budget");
    check(native_windows_validate(wire.data) == NULL, "schema accepts a fully escaped maximum-size UTF-8 text value");
    success(wire.data);
    native_window_t pane = window(id);
    check(strlen(pane.text) == NATIVE_WINDOW_TEXT_CAP - 1 && !strcmp(pane.text + 8185, "é🚀"),
          "8191 decoded bytes retain trailing non-ASCII and surrogate-pair text without truncation");
    native_windows_snapshot_t before = snapshot();
    wire.len -= 2; wire.data[wire.len] = 0; jbuf_append(&wire, "\\u0062\"}");
    reject(wire.data); unchanged_windows(before);
    check(!native_windows_action_pending(), "large text validation never enqueues terminal actions");
    jbuf_free(&wire);
}

static void test_keyboard_and_visibility(void) {
    reset();
    uint64_t first = open_window("Keyboard first", "note", 100, 100, 400, 240);
    uint64_t second = open_window("Keyboard second", "workflow", 200, 180, 400, 240);
    check(!native_windows_focused(), "JSON open leaves typing with the draft by default");
    check(!native_windows_key('x', 0) && snapshot().count == 2, "draft typing cannot accidentally close a panel");
    check(native_windows_key(NATIVE_WINDOW_KEY_TOGGLE_FOCUS, 0) && native_windows_focused(),
          "explicit focus toggle gives keyboard control to panels");
    check(native_windows_key(NATIVE_WINDOW_KEY_TAB, 0) && snapshot().focused_id == first,
          "Tab wraps through stable IDs despite reordered z-order");
    check(native_windows_key(NATIVE_WINDOW_KEY_TAB, NATIVE_WINDOW_MOD_SHIFT) && snapshot().focused_id == second,
          "Shift-Tab reverses stable keyboard order");
    native_window_t before = window(second);
    check(native_windows_key(NATIVE_WINDOW_KEY_RIGHT, 0), "focused arrows are handled");
    native_window_t after = window(second);
    check(after.rect.x > before.rect.x && after.rect.width == before.rect.width, "arrow moves focused window without resizing");
    check(native_windows_key(NATIVE_WINDOW_KEY_DOWN, NATIVE_WINDOW_MOD_SHIFT), "modified arrow resize is handled");
    check(window(second).rect.height > after.rect.height && window(second).rect.y == after.rect.y,
          "Shift-arrow resizes without moving the title");
    check(native_windows_key(NATIVE_WINDOW_KEY_ESCAPE, 0) && !native_windows_focused() && native_windows_visible(),
          "Escape returns keyboard to draft while retaining visible panels");
    success("{\"action\":\"hide\"}");
    check(!native_windows_visible() && !native_windows_focused(), "hide clears visibility and keyboard ownership");
    native_windows_snapshot_t hidden = snapshot();
    native_window_t pane = window(second); int x, y; footer_point(pane, 0, &x, &y);
    check(!native_windows_pointer(0, x, y, false) && !native_windows_pointer(0, x, y, true),
          "hidden panels cannot intercept pointer actions");
    unchanged_windows(hidden);
    success("{\"action\":\"show\"}");
    check(snapshot().count == 2, "show existing workspace does not create another starter");
    check(!native_windows_action_pending(), "layout/focus keyboard commands never forge workflow receipts");
}

typedef struct { atomic_bool stop, failed; atomic_uint observations; } reader_state_t;

static void *snapshot_reader(void *arg) {
    reader_state_t *reader = arg;
    while (!atomic_load(&reader->stop)) {
        native_windows_snapshot_t snap = snapshot();
        bool ok = snap.count == 2;
        for (int i = 0; i < snap.count && i < NATIVE_WINDOWS_MAX; i++)
            ok = ok && snap.windows[i].id > 0 && inside(snap.windows[i].rect, snap.work_area);
        if (!ok) atomic_store(&reader->failed, true);
        atomic_fetch_add(&reader->observations, 1);
    }
    return NULL;
}

static void test_concurrent_snapshot_consistency(void) {
    reset();
    uint64_t id = open_window("Concurrent first", "note", 100, 100, 400, 240);
    open_window("Concurrent second", "workflow", 200, 180, 400, 240);
    reader_state_t reader = {0}; pthread_t threads[2];
    for (int i = 0; i < 2; i++) check(!pthread_create(&threads[i], NULL, snapshot_reader, &reader), "start concurrent snapshot reader");
    for (int i = 0; i < 60; i++) {
        id_command("move", id, i % 2 ? ",\"x\":100,\"y\":100" : ",\"x\":200,\"y\":180");
        id_command("resize", id, i % 2 ? ",\"width\":300,\"height\":180" : ",\"width\":500,\"height\":350");
    }
    atomic_store(&reader.stop, true);
    for (int i = 0; i < 2; i++) check(!pthread_join(threads[i], NULL), "join snapshot reader");
    check(atomic_load(&reader.observations) > 0 && !atomic_load(&reader.failed),
          "snapshots remain coherent while commands mutate logical geometry");
}

int main(void) {
    test_geometry_focus_lifecycle();
    test_drag_resize_zoom_and_layout();
    test_retained_layout_on_work_area_resize();
    test_coarse_pointer_grip_and_enter_handoff();
    test_footer_actions_and_retained_events();
    test_queue_saturation_and_utf8_action();
    test_scroll_and_capacity();
    test_word_wrap_and_exact_scroll();
    test_maximum_escaped_text_payload();
    test_keyboard_and_visibility();
    test_concurrent_snapshot_consistency();
    test_validation_and_workflow();
    printf("native_windows: %u checks passed\n", checks);
    return 0;
}
