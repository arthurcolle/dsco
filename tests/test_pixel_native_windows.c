/* Actual renderer + retained model. Default: owned in-memory state and tmpfile;
 * --live: explicitly launched fixture TTY only. Link all test objects except
 * pixel_tui.o. No inference, credentials, buffer reads, or external processes. */
#define _DARWIN_C_SOURCE
#include "../src/pixel_tui.c"
#include "vm.h"
#include <signal.h>
#include <termios.h>
#include <limits.h>

int g_cheap_mode;
vm_t g_vm;
volatile int g_interrupted;
double g_cost_budget;
static int checks, failures;
#define CHECK(v, label) do { ++checks; if (!(v)) { ++failures; \
    fprintf(stderr, "FAIL: %s (%d)\n", label, __LINE__); } } while (0)

static bool command(const char *json) {
    char result[2048];
    return native_windows_command(json, result, sizeof(result));
}
static native_windows_snapshot_t snapshot;
static void sample(void) { native_windows_snapshot(&snapshot); }
static const char *workflow = "{\"action\":\"open\",\"kind\":\"workflow\","
    "\"title\":\"Make the next step concrete\",\"text\":\"Keep an outcome, the evidence and the next action visible while DSCO works.\\n\\nDrag the title. Resize the lower-right corner. Your draft stays below.\","
    "\"next_step\":\"Inspect the current result, then choose the next action.\","
    "\"evidence\":\"Owned visual fixture; no external work has been performed.\","
    "\"x\":24,\"y\":100,\"width\":460,\"height\":310}";
static const char *note = "{\"action\":\"open\",\"title\":\"Working notes\","
    "\"text\":\"Astra π 🦉\\n\\nA second retained window.\\nEach panel owns its scroll position.\\nClose a view without deleting its buffer.\","
    "\"x\":410,\"y\":145,\"width\":290,\"height\":225}";

static int cell_x(int x) { return x * g_session.cols / g_session.width + 1; }
static int cell_y(int y) { return y * g_session.rows / g_session.height + 1; }

static void check_wrapped(const char *text, int columns, const char *const *expected, int count) {
    const char *cursor = text, *start;
    size_t bytes;
    int rows = 0;
    bool matches = true;
    while (native_window_text_next(&cursor, columns, &start, &bytes)) {
        if (rows >= count || strlen(expected[rows]) != bytes || memcmp(start, expected[rows], bytes))
            matches = false;
        if (++rows > 1000) { matches = false; break; }
    }
    CHECK(matches && rows == count, "word wrapping preserves expected whitespace and UTF-8 line boundaries");
    int width = columns * 8 + 24, height = (count + 1) * 18;
    px_canvas_t *actual = canvas_acquire_device(width, height, width, height, 1);
    px_canvas_t *wanted = canvas_acquire_device(width, height, width, height, 1);
    if (actual && wanted) {
        fill_rect(actual, 0, 0, width, height, C_PANEL, 1);
        fill_rect(wanted, 0, 0, width, height, C_PANEL, 1);
        int rendered_rows = 0;
        int full = session_window_text(actual, text, 0, height, &rendered_rows, 0, C_TEXT);
        for (int i = 0; i < count; ++i)
            draw_text_ellipsis(wanted, 12, i * 18, 1, expected[i], C_TEXT, 0.94, width - 24);
        CHECK(!full && rendered_rows == count &&
              !memcmp(actual->pixels, wanted->pixels, (size_t)width * height * sizeof(px_color_t)),
              "painted text rows match word-wrap output exactly");
    } else CHECK(false, "word-wrap fixture canvases allocate");
    free_canvas(actual); free_canvas(wanted);
}

static size_t changed_rows(const px_canvas_t *a, const px_canvas_t *b, int top, int bottom) {
    if (!a || !b) return SIZE_MAX;
    size_t changed = 0;
    for (int y = top * a->backing_scale; y < bottom * a->backing_scale; ++y)
        for (int x = 0; x < a->pixel_width; ++x)
            changed += memcmp(&a->pixels[(size_t)y * a->pixel_width + x],
                              &b->pixels[(size_t)y * b->pixel_width + x], sizeof(px_color_t)) != 0;
    return changed;
}

static bool clean_workspace_gaps(const px_canvas_t *c, native_ui_rect_t area) {
    if (!c) return false;
    sample();
    size_t checked = 0;
    px_color_t background = C_BG_TOP;
    /* The composer is drawn later and casts its existing soft shadow into the
     * bottom six logical pixels; that shadow is not transcript leakage. */
    for (int y = area.y + NATIVE_WINDOWS_TOOLBAR_HEIGHT; y < area.y + area.height - 6; ++y)
        for (int x = area.x; x < area.x + area.width; ++x) {
            bool panel = false;
            for (int i = 0; i < snapshot.count; ++i) {
                native_ui_rect_t r = snapshot.windows[i].rect;
                if (x >= r.x && x < r.x + r.width && y >= r.y && y < r.y + r.height)
                    panel = true;
            }
            if (panel) continue;
            for (int sy = 0; sy < c->backing_scale; ++sy)
                for (int sx = 0; sx < c->backing_scale; ++sx) {
                    const px_color_t *pixel = &c->pixels[
                        (size_t)(y * c->backing_scale + sy) * c->pixel_width + x * c->backing_scale + sx];
                    if (memcmp(pixel, &background, sizeof(background))) {
                        fprintf(stderr, "workspace gap (%d,%d): got %u,%u,%u expected %u,%u,%u\n",
                                x, y, pixel->r, pixel->g, pixel->b, background.r, background.g, background.b);
                        return false;
                    }
                    ++checked;
                }
        }
    return checked > 0;
}

static int run_tests(const char *artifact) {
    native_windows_reset();
    memset(&g_session, 0, sizeof(g_session));
    g_session.active = true;
    g_session.width = 800; g_session.height = 600;
    g_session.surface_width = 1600; g_session.surface_height = 1200;
    g_session.backing_scale = 2; g_session.cols = 100; g_session.rows = 50;
    g_session.current_message = -1; g_session.state = PIXEL_TUI_IDLE;
    g_session.saved_stdout_fd = g_session.saved_stderr_fd = g_session.devnull_fd = -1;
    g_session.capture_read_fd = g_session.capture_write_fd = -1;
    snprintf(g_session.input, sizeof(g_session.input), "Composer stays usable: Astra π 🦉");
    g_session.input_cursor = strlen(g_session.input);
    ui_motion_init(&g_session.motion, true);
    check_wrapped("Astra π 🦉 remains usable", 10,
                  (const char *[]) {"Astra π 🦉", "remains", "usable"}, 3);
    check_wrapped("status remains agent-reported", 14,
                  (const char *[]) {"status remains", "agent-reported"}, 2);
    check_wrapped("prefix abcdefghijkl suffix", 6,
                  (const char *[]) {"prefix", "abcdef", "ghijkl", "suffix"}, 4);
    check_wrapped("alpha  beta gamma", 11,
                  (const char *[]) {"alpha  beta", "gamma"}, 2);
    check_wrapped("alpha\n\nbeta\n", 12,
                  (const char *[]) {"alpha", "", "beta"}, 3);
    check_wrapped("🦉π🦉π🦉", 2, (const char *[]) {"🦉π", "🦉π", "🦉"}, 3);
    check_wrapped("", 2, NULL, 0);
    px_canvas_t *base = render_session_frame(800, 600, 2, 1600, 1200, "owned window fixture", PIXEL_TUI_IDLE);
    CHECK(base != NULL, "base framebuffer renders");
    CHECK(command(workflow) && command(note), "two retained panels open");
    px_canvas_t *panels = render_session_frame(800, 600, 2, 1600, 1200, "owned window fixture", PIXEL_TUI_IDLE);
    CHECK(panels != NULL, "native panels render on the same framebuffer");
    native_ui_rect_t area = session_windows_work_area(800, 600, PIXEL_TUI_IDLE);
    size_t outside_changes = 0, inside_changes = 0;
    if (base && panels) {
        for (int y = 0; y < 1200; ++y) for (int x = 0; x < 1600; ++x) {
            if (!memcmp(&base->pixels[(size_t)y * 1600 + x], &panels->pixels[(size_t)y * 1600 + x], 3)) continue;
            bool inside = x >= area.x * 2 && x < (area.x + area.width) * 2 &&
                          y >= area.y * 2 && y < (area.y + area.height) * 2;
            if (inside) ++inside_changes; else ++outside_changes;
        }
        CHECK(inside_changes > 10000, "visible panel pixels replace the transcript region");
        CHECK(outside_changes == 0, "panels leave every masthead/composer pixel unchanged at Retina scale");
        if (artifact) CHECK(canvas_write_ppm(artifact, panels), "owned panel preview artifact written");
        CHECK(clean_workspace_gaps(panels, area), "free workspace space has no transcript pixels");
        for (int level = PIXEL_TUI_NOTICE_INFO; level <= PIXEL_TUI_NOTICE_ACTIVITY; ++level) {
            g_session.notices[0] = (pixel_notice_t){.used = true, .level = level,
                .created_s = monotonic_s(), .text = "Owned notice must not cover workflow controls"};
            px_canvas_t *notice = render_session_frame(800, 600, 2, 1600, 1200, "owned window fixture", PIXEL_TUI_IDLE);
            CHECK(changed_rows(panels, notice, area.y + NATIVE_WINDOWS_TOOLBAR_HEIGHT, 600) == 0,
                  "nonmodal notice leaves all panel and composer pixels unchanged");
            size_t toolbar_changes = changed_rows(panels, notice, area.y, area.y + NATIVE_WINDOWS_TOOLBAR_HEIGHT);
            CHECK(level == PIXEL_TUI_NOTICE_WARNING || level == PIXEL_TUI_NOTICE_ERROR ?
                  toolbar_changes > 0 : toolbar_changes == 0,
                  "action feedback outranks informational notices but warnings and errors remain visible");
            free_canvas(notice);
        }
        memset(g_session.notices, 0, sizeof(g_session.notices));
        sample();
        native_window_t action_window = snapshot.windows[0];
        int action_x = action_window.rect.x + action_window.rect.width / 8;
        int action_y = action_window.rect.y + action_window.rect.height - NATIVE_WINDOW_FOOTER_HEIGHT / 2;
        CHECK(native_windows_pointer(0, action_x, action_y, false) &&
              native_windows_pointer(0, action_x, action_y, true) && native_windows_action_pending(),
              "owned workflow click queues one action for toolbar acknowledgement");
        for (int delivered = 0; delivered < 2; ++delivered) {
            if (delivered) {
                native_window_action_t action;
                CHECK(native_windows_pop_action(&action) && !native_windows_action_pending(),
                      "owned workflow action is received exactly once");
            }
            sample();
            CHECK(strstr(snapshot.feedback, delivered ? "DSCO received" : "requested for") != NULL,
                  "toolbar model publishes queued and received acknowledgements");
            px_canvas_t *ack = render_session_frame(800, 600, 2, 1600, 1200, "owned window fixture", PIXEL_TUI_IDLE);
            g_session.notices[0] = (pixel_notice_t){.used = true, .level = PIXEL_TUI_NOTICE_SUCCESS,
                .created_s = monotonic_s() - 1, .text = "dsco — response complete"};
            px_canvas_t *stale = render_session_frame(800, 600, 2, 1600, 1200, "owned window fixture", PIXEL_TUI_IDLE);
            CHECK(changed_rows(ack, stale, area.y, area.y + NATIVE_WINDOWS_TOOLBAR_HEIGHT) == 0,
                  "older completion cannot obscure the queued or received click acknowledgement");
            free_canvas(ack); free_canvas(stale);
            memset(g_session.notices, 0, sizeof(g_session.notices));
        }
        /* Count using the actual painter with all rows skipped, then compare
         * the model's final scroll position with its drawable row capacity. */
        int painted_rows = 0;
        px_canvas_t *measure = canvas_acquire_device(action_window.rect.width, 20, action_window.rect.width, 20, 1);
        CHECK(measure != NULL, "owned text measurement canvas allocates");
        if (measure) {
            session_window_text(measure, action_window.text, 0, 0, &painted_rows, 100000, C_TEXT);
            session_window_text(measure, "\nNEXT STEP", 0, 0, &painted_rows, 100000, C_TEXT);
            session_window_text(measure, action_window.next_step, 0, 0, &painted_rows, 100000, C_TEXT);
            session_window_text(measure, "\nEVIDENCE", 0, 0, &painted_rows, 100000, C_TEXT);
            session_window_text(measure, action_window.evidence, 0, 0, &painted_rows, 100000, C_TEXT);
            char scroll_command[128];
            snprintf(scroll_command, sizeof(scroll_command), "{\"action\":\"scroll\",\"id\":%llu,\"lines\":100000}",
                     (unsigned long long)action_window.id);
            CHECK(command(scroll_command), "model scrolls to the actual final wrapped row");
            sample();
            int available = (action_window.rect.height - NATIVE_WINDOW_TITLE_HEIGHT - 38 - NATIVE_WINDOW_FOOTER_HEIGHT) / 18;
            int expected = painted_rows > available ? painted_rows - available : 0;
            for (int i = 0; i < snapshot.count; ++i) if (snapshot.windows[i].id == action_window.id)
                CHECK(snapshot.windows[i].scroll == expected, "model scroll extent matches painter rows including section headings");
            free_canvas(measure);
        }
        px_canvas_t *overlay_base = render_session_frame(800, 600, 2, 1600, 1200, "owned window fixture", PIXEL_TUI_IDLE);
        g_session.composer_menu.kind = PIXEL_TUI_MENU_COMMANDS;
        g_session.composer_menu.count = 1;
        snprintf(g_session.composer_menu.items[0].label, sizeof(g_session.composer_menu.items[0].label), "Owned menu");
        px_canvas_t *overlay = render_session_frame(800, 600, 2, 1600, 1200, "owned window fixture", PIXEL_TUI_IDLE);
        CHECK(changed_rows(overlay_base, overlay, area.y + NATIVE_WINDOWS_TOOLBAR_HEIGHT, area.y + area.height) > 0,
              "composer menu retains its layer above the workspace");
        free_canvas(overlay); memset(&g_session.composer_menu, 0, sizeof(g_session.composer_menu));
        g_session.modal.active = true; g_session.modal.kind = PIXEL_TUI_MODAL_PERMISSION;
        snprintf(g_session.modal.title, sizeof(g_session.modal.title), "Owned permission fixture");
        overlay = render_session_frame(800, 600, 2, 1600, 1200, "owned window fixture", PIXEL_TUI_IDLE);
        CHECK(changed_rows(overlay_base, overlay, area.y + NATIVE_WINDOWS_TOOLBAR_HEIGHT, 600) > 10000,
              "critical modal retains its layer above workspace and composer");
        free_canvas(overlay); free_canvas(overlay_base); memset(&g_session.modal, 0, sizeof(g_session.modal));
        CHECK(command("{\"action\":\"hide\"}"), "workspace can hide for transcript restoration");
        px_canvas_t *hidden = render_session_frame(800, 600, 2, 1600, 1200, "owned window fixture", PIXEL_TUI_IDLE);
        CHECK(changed_rows(base, hidden, 0, 600) == 0, "Hide restores the original transcript framebuffer");
        free_canvas(hidden);
        CHECK(command("{\"action\":\"show\"}") && command("{\"action\":\"tile\"}"), "workspace tiles for gutter regression");
        px_canvas_t *tiled = render_session_frame(800, 600, 2, 1600, 1200, "owned window fixture", PIXEL_TUI_IDLE);
        CHECK(clean_workspace_gaps(tiled, area), "four-pixel tile gutters contain only the workspace background");
        free_canvas(tiled);
        native_windows_reset(); native_windows_set_work_area(area);
        CHECK(command(workflow) && command(note), "restore retained panel geometry after layering checks");
    }
    free_canvas(base); free_canvas(panels);
    sample();
    CHECK(snapshot.count == 2 && snapshot.visible, "model remains retained after rendering");
    native_window_t first = snapshot.windows[0];
    FILE *out = tmpfile(); CHECK(out != NULL, "owned output stream created");
    if (!out) return 1;
    int cx = cell_x(first.rect.x + 60), cy = cell_y(first.rect.y + 12);
    CHECK(pixel_tui_session_pointer(out, 0, cx, cy, false), "title press routes through cell-to-logical wrapper");
    sample(); CHECK(snapshot.captured_id == first.id, "pressed title captures its stable window ID");
    CHECK(native_windows_focused(), "title press gives the selected panel keyboard focus");
    CHECK(pixel_tui_session_pointer(out, 32, cx + 3, cy + 2, false), "button motion routes a drag");
    CHECK(pixel_tui_session_pointer(out, 0, cx + 3, cy + 2, true), "release completes captured drag");
    sample();
    CHECK(snapshot.captured_id == 0 && snapshot.windows[snapshot.count - 1].id == first.id,
          "drag preserves original window and raises it above overlap");
    CHECK(snapshot.windows[snapshot.count - 1].rect.x != first.rect.x, "drag changes retained geometry");
    CHECK(!pixel_tui_session_pointer(out, 0, INT_MAX, INT_MAX, false), "oversized cell coordinates rejected");
    CHECK(pixel_tui_session_window_key(out, NATIVE_WINDOW_KEY_ESCAPE, 0), "pointer-selected panel can return focus to draft");
    CHECK(pixel_tui_session_window_key(out, NATIVE_WINDOW_KEY_TOGGLE_FOCUS, 0), "keyboard focus reaches native manager");
    CHECK(native_windows_focused(), "panel keyboard focus becomes active");
    CHECK(pixel_tui_session_window_key(out, NATIVE_WINDOW_KEY_ESCAPE, 0), "Escape returns to composer");
    CHECK(!native_windows_focused(), "Escape releases panel keyboard focus");
    CHECK(!strcmp(g_session.input, "Composer stays usable: Astra π 🦉"), "pointer/key controls never mutate draft text");
    g_session.modal.active = true;
    CHECK(pixel_tui_session_pointer(out, 0, cx, cy, false), "modal consumes pointer packets without fallback behind it");
    g_session.modal.active = false;

    g_session.scene_json = strdup("{}"); g_session.scene_image_id = 12345;
    g_session.scene_col = g_session.scene_row = 1; g_session.scene_cols = g_session.scene_rows = 4;
    g_session.scene_placed = true; g_session.scene_dirty = false;
    session_place_scene(out, false);
    CHECK(!g_session.scene_placed && g_session.scene_json, "managed windows hide placement without losing prior scene");
    CHECK(command("{\"action\":\"hide\"}"), "workspace hides");
    session_place_scene(out, false);
    CHECK(g_session.scene_placed && g_session.scene_json, "hidden windows restore the prior scene placement");
    session_release_scene(out);
    CHECK(command("{\"action\":\"show\"}"), "workspace reopens");
    snprintf(g_session.input, sizeof(g_session.input), "one\ntwo\nthree\nfour\nfive\nsix");
    panels = render_session_frame(800, 600, 2, 1600, 1200, "owned window fixture", PIXEL_TUI_IDLE);
    free_canvas(panels); sample();
    session_deck_geometry_t deck = session_deck_geometry(800, 600, PIXEL_TUI_IDLE);
    for (int i = 0; i < snapshot.count; ++i)
        CHECK(snapshot.windows[i].rect.y + snapshot.windows[i].rect.height <= deck.deck_y - 6,
              "multiline composer growth clamps every retained window above the deck");
    native_windows_reset(); session_messages_free(); free(g_session.prev_frame); free(g_session.patch_buffer);
    memset(&g_session, 0, sizeof(g_session)); fclose(out);
    printf("native window renderer: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}

static volatile sig_atomic_t live_stop;
static void stop_live(int sig) { (void)sig; live_stop = 1; }
static void live_status(void) {
    const char *path = getenv("DSCO_NATIVE_WINDOWS_FIXTURE_STATUS");
    if (!path || !*path) return;
    char *result = malloc(180000); if (!result) return;
    if (!native_windows_command("{\"action\":\"list\"}", result, 180000)) { free(result); return; }
    jbuf_t status; jbuf_init(&status, strlen(result) + 256);
    size_t result_len = strlen(result);
    if (result_len && result[result_len - 1] == '}') result[--result_len] = '\0';
    jbuf_append(&status, result);
    session_lock();
    jbuf_appendf(&status, ",\"fixture\":{\"cols\":%d,\"rows\":%d,\"width\":%d,\"height\":%d,\"phase\":%d,\"menu\":%d,\"draft\":",
                 g_session.cols, g_session.rows, g_session.width, g_session.height,
                 g_session.state, g_session.composer_menu.kind);
    jbuf_append_json_str(&status, g_session.input);
    session_unlock();
    jbuf_append(&status, "},\"human_events\":");
    if (native_windows_command("{\"action\":\"events\"}", result, 180000))
        jbuf_append(&status, result);
    else jbuf_append(&status, "{}");
    jbuf_append(&status, "}");
    char tmp[4096];
    if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp)) { free(result); jbuf_free(&status); return; }
    FILE *f = fopen(tmp, "w");
    if (f) { fputs(status.data, f); fclose(f); rename(tmp, path); }
    free(result); jbuf_free(&status);
}

static int run_live(void) {
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) return 2;
    struct termios saved, raw;
    if (tcgetattr(STDIN_FILENO, &saved) != 0) return 2;
    raw = saved; cfmakeraw(&raw); tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    signal(SIGTERM, stop_live); signal(SIGINT, stop_live);
    setenv("DSCO_PIXEL_TUI", "1", 1);
    if (!pixel_tui_session_begin(stdout, "owned native windows fixture")) {
        tcsetattr(STDIN_FILENO, TCSANOW, &saved); return 2;
    }
    command(workflow); command(note); pixel_tui_session_windows_changed(stdout);
    const char *draft = "Draft remains usable: Astra π 🦉";
    pixel_tui_session_set_input(stdout, draft, strlen(draft), true);
    int escape = 0; char seq[96]; size_t len = 0; bool menu = false;
    double deadline = monotonic_s() + 300.0, next_status = 0;
    while (!live_stop && monotonic_s() < deadline) {
        if (monotonic_s() >= next_status) { live_status(); next_status = monotonic_s() + 0.2; }
        struct pollfd p = {.fd = STDIN_FILENO, .events = POLLIN};
        if (poll(&p, 1, 50) <= 0) {
            if (escape == 1) {
                pixel_tui_session_window_key(stdout, NATIVE_WINDOW_KEY_ESCAPE, 0);
                escape = 0;
            }
            continue;
        }
        unsigned char byte;
        if (read(STDIN_FILENO, &byte, 1) != 1) break;
        if (escape == 1) {
            if (byte == '[') { escape = 2; len = 0; }
            else if (byte == ']' || byte == '_' || byte == 'P') escape = 3;
            else escape = 0;
            continue;
        }
        if (escape == 3) { if (byte == 7) escape = 0; else if (byte == 27) escape = 4; continue; }
        if (escape == 4) { escape = byte == '\\' ? 0 : 3; continue; }
        if (escape == 2) {
            if (len + 1 < sizeof(seq)) seq[len++] = (char)byte;
            else { escape = 0; continue; }
            if (byte < 0x40 || byte > 0x7e) continue;
            seq[len] = '\0'; escape = 0;
            if (seq[0] == '<' && (byte == 'm' || byte == 'M')) {
                char *end; long b = strtol(seq + 1, &end, 10);
                if (*end != ';' || b < 0 || b > INT_MAX) continue;
                long x = strtol(end + 1, &end, 10); if (*end != ';' || x < 1 || x > INT_MAX) continue;
                long y = strtol(end + 1, &end, 10); if ((*end != 'm' && *end != 'M') || y < 1 || y > INT_MAX) continue;
                pixel_tui_session_pointer(stdout, (int)b, (int)x, (int)y, byte == 'm');
            } else {
                int key = byte == 'A' ? NATIVE_WINDOW_KEY_UP : byte == 'B' ? NATIVE_WINDOW_KEY_DOWN :
                          byte == 'C' ? NATIVE_WINDOW_KEY_RIGHT : byte == 'D' ? NATIVE_WINDOW_KEY_LEFT : 0;
                if (key) pixel_tui_session_window_key(stdout, key, 0);
            }
            continue;
        }
        if (byte == 27) { escape = 1; continue; }
        if (byte == 'q' || byte == 3) break;
        if (byte == 7) pixel_tui_session_window_key(stdout, NATIVE_WINDOW_KEY_TOGGLE_FOCUS, 0);
        else if (byte == '\t') pixel_tui_session_window_key(stdout, NATIVE_WINDOW_KEY_TAB, 0);
        else if (byte == 'm') {
            menu = !menu;
            pixel_tui_menu_item_t items[] = {{.label = "Owned menu item", .detail = "Panels remain underneath"}};
            pixel_tui_session_set_composer(stdout, draft, strlen(draft), true,
                menu ? PIXEL_TUI_MENU_COMMANDS : PIXEL_TUI_MENU_NONE, items, menu ? 1 : 0, 0);
        } else if (byte == 'p') pixel_tui_session_set_state(stdout, PIXEL_TUI_REASONING);
        else pixel_tui_session_window_key(stdout, byte, 0);
    }
    live_status(); pixel_tui_session_end(stdout);
    tcsetattr(STDIN_FILENO, TCSANOW, &saved);
    return 0;
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--live")) return run_live();
    return run_tests(argc > 1 ? argv[1] : NULL);
}
