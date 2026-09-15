/* Single-scene lifetime regression. Include the implementation to inspect
 * private ownership without exporting test controls in the production API.
 * Link with TUI_TEST_LIB_OBJS excluding pixel_tui.o. The only terminal here is
 * an owned, invisible PTY with a draining reader; no app/window is launched. */
#define _DARWIN_C_SOURCE
#include "../src/pixel_tui.c"
#include "vm.h"
#include <zlib.h>
#include <signal.h>
#include <termios.h>
#ifdef __APPLE__
#include <util.h>
#else
#include <pty.h>
#endif

int g_cheap_mode = 0;
vm_t g_vm;
volatile int g_interrupted = 0;
double g_cost_budget = 0.0;

static int checks, failures;
#define CHECK(expr, label) do { checks++; if (!(expr)) { failures++; \
    fprintf(stderr, "FAIL: %s (%d)\n", label, __LINE__); } } while (0)

typedef struct { int fd; _Atomic bool stop; size_t bytes; } drain_t;
static void *drain_pty(void *arg) {
    drain_t *drain = arg;
    char bytes[8192];
    while (!atomic_load(&drain->stop)) {
        struct pollfd ready = {.fd = drain->fd, .events = POLLIN};
        if (poll(&ready, 1, 50) > 0 && (ready.revents & POLLIN)) {
            ssize_t count = read(drain->fd, bytes, sizeof(bytes));
            if (count > 0) drain->bytes += (size_t)count;
        }
    }
    return NULL;
}

static void check_owned_scene(const char *json, const char *label) {
    CHECK(g_session.scene_json && !strcmp(g_session.scene_json, json), label);
    CHECK(g_session.scene_image_id != 0, "retained scene owns uploaded image");
    CHECK(g_session.overlay_image_id == 0, "scene does not occupy transient overlay slot");
}

static bool wait_background_after(double after, int menu_count) {
    double deadline = monotonic_s() + 1.5;
    while (monotonic_s() < deadline) {
        session_lock();
        bool ready = g_session.last_paint_s > after &&
                     !g_session.structural_repaint_pending &&
                     !g_session.composer_repaint_pending &&
                     (menu_count < 0 || s_patched_menu_count == menu_count);
        session_unlock();
        if (ready) return true;
        struct timespec tick = {.tv_nsec = 5000000}; nanosleep(&tick, NULL);
    }
    return false;
}

static void test_tool_result_soft_wrap(void) {
    /* The reported screenshot cut the archive key in a single-line JSON
     * result. All retained preview bytes must survive narrower viewports. */
    static const char *const results[] = {
        "{\"changed\":true,\"turns_evicted\":1,\"archive_failures\":0,\"keep_recent\":2,"
        "\"estimated_tokens_before\":33259,\"estimated_tokens_after\":31455,"
        "\"last_archive_key\":\"ck:tool:73a6e4720114d96cf74af10ef38864819c9fda2c9f0123456789abcdef0123456789\","
        "\"retrieval\":\"context_recall\",\"semantic_summary\":false}",
        "Unicode boundary: 雪 café 🌈 λ 雪 café 🌈 λ 雪 café 🌈 λ\n\nLAST_VISIBLE_RESULT"
    };
    const int columns[] = {12, 40, 80, 200};
    pixel_visual_line_t *lines = calloc(128, sizeof(*lines));
    CHECK(lines != NULL, "tool wrap line fixture allocates");
    if (!lines) return;
    for (size_t r = 0; r < sizeof(results) / sizeof(*results); r++) {
        pixel_message_t message = {.tool_row = true, .tool_status = 1, .sequence = 1};
        snprintf(message.role, sizeof(message.role), "TOOL");
        snprintf(message.tool_name, sizeof(message.tool_name), "context_evict");
        CHECK(message_text_set_plain(&message, results[r]), "tool result fixture retained");
        char expected[1024];
        size_t expected_len = 0;
        for (const char *p = results[r]; *p; p++)
            if (*p != '\n') expected[expected_len++] = *p;
        expected[expected_len] = '\0';
        for (size_t w = 0; w < sizeof(columns) / sizeof(*columns); w++) {
            int count = wrap_tool_message(&message, columns[w], lines, 0, 128);
            char actual[1024] = "";
            size_t used = 0;
            CHECK(count > 2, "long retained tool result wraps into continuation rows");
            for (int i = 1; i < count; i++) {
                CHECK(lines[i].tool_row && !lines[i].first && lines[i].indent == 1,
                      "continuation retains tool rail and indentation");
                CHECK(lines[i].char_count <= columns[w] - 2,
                      "continuation fits the available text columns");
                for (int ri = 0; ri < lines[i].run_count; ri++) {
                    size_t bytes = strlen(lines[i].runs[ri].text);
                    CHECK(used + bytes < sizeof(actual), "wrapped text fits fixture capture");
                    if (used + bytes >= sizeof(actual)) continue;
                    memcpy(actual + used, lines[i].runs[ri].text, bytes);
                    used += bytes;
                    actual[used] = '\0';
                }
            }
            CHECK(!strcmp(actual, expected),
                  "complete archive key, Unicode and result tail survive soft wrapping");
            CHECK(!lines[count - 1].streaming, "completed wrapped result has no live cursor");
        }
        message_text_clear(&message);
    }
    free(lines);
}

static volatile sig_atomic_t live_stop;
static void stop_live(int sig) { (void)sig; live_stop = 1; }

/* Kitty mouse packets and terminal acknowledgments are not key commands. */
static int live_command_byte(int *state, unsigned char byte) {
    enum { NORMAL, ESCAPE, CSI, STRING, STRING_ESCAPE, SS3 };
    switch (*state) {
        case NORMAL:
            if (byte == 0x1b) { *state = ESCAPE; return 0; }
            return byte >= 0x20 && byte != 0x7f ? byte : 0;
        case ESCAPE:
            if (byte == '[') *state = CSI;
            else if (byte == 'O') *state = SS3;
            else if (byte == '_' || byte == ']' || byte == 'P' || byte == '^' || byte == 'X')
                *state = STRING;
            else *state = NORMAL;
            return 0;
        case CSI:
            if (byte >= 0x40 && byte <= 0x7e) *state = NORMAL;
            return 0;
        case STRING:
            if (byte == 7) *state = NORMAL;
            else if (byte == 0x1b) *state = STRING_ESCAPE;
            return 0;
        case STRING_ESCAPE:
            *state = byte == '\\' ? NORMAL : byte == 0x1b ? STRING_ESCAPE : STRING;
            return 0;
        case SS3:
        default:
            *state = NORMAL;
            return 0;
    }
}

static void live_status(const char *input_hex) {
    const char *path = getenv("DSCO_PIXEL_SCENE_FIXTURE_STATUS");
    if (!path || !*path) return;
    jbuf_t b; jbuf_init(&b, 1024);
    session_lock();
    jbuf_appendf(&b, "{\"pid\":%ld,\"scene\":%s,\"scene_id\":%u,\"placed\":%s,\"dirty\":%s,"
                 "\"base_id\":%u,\"cols\":%d,\"rows\":%d,\"width\":%d,\"height\":%d,"
                 "\"scene_col\":%d,\"scene_row\":%d,\"scene_cols\":%d,\"scene_rows\":%d,"
                 "\"phase\":%d,\"menu\":%d,\"input\":",
        (long)getpid(), g_session.scene_json ? "true" : "false", g_session.scene_image_id,
        g_session.scene_placed ? "true" : "false", g_session.scene_dirty ? "true" : "false",
        g_session.image_ids[g_session.state], g_session.cols, g_session.rows,
        g_session.width, g_session.height, g_session.scene_col, g_session.scene_row,
        g_session.scene_cols, g_session.scene_rows, g_session.state, g_session.composer_menu.kind);
    jbuf_append_json_str(&b, g_session.input);
    jbuf_append(&b, ",\"input_hex\":"); jbuf_append_json_str(&b, input_hex); jbuf_append(&b, "}");
    session_unlock();
    char temporary[4096];
    int n = snprintf(temporary, sizeof(temporary), "%s.tmp.%ld", path, (long)getpid());
    if (n > 0 && (size_t)n < sizeof(temporary)) {
        FILE *f = fopen(temporary, "w");
        if (f) {
            bool wrote = fwrite(b.data, 1, b.len, f) == b.len;
            if (fclose(f) != 0) wrote = false;
            if (!wrote || rename(temporary, path) != 0) (void)unlink(temporary);
        }
    }
    jbuf_free(&b);
}

/* Explicit opt-in owned Kitty fixture for screenshot/layering verification.
 * m toggles a temporary menu; t changes phase; q exits. Resize the fixture's
 * own Kitty window externally to exercise the real geometry watcher. */
static int run_live_fixture(void) {
    if (!isatty(STDIN_FILENO) || !isatty(STDERR_FILENO)) {
        fputs("--live requires its own Kitty terminal\n", stderr);
        return 2;
    }
    struct termios saved, raw;
    if (tcgetattr(STDIN_FILENO, &saved) != 0) return 2;
    setenv("DSCO_PIXEL_TUI", "1", 1); /* This fixture process only. */
    fputs("\033]2;DSCO retained scene fixture\007", stderr);
    if (!pixel_tui_session_begin(stderr, "owned lifetime fixture")) return 2;
    const char *spec = "{\"element\":\"stack\",\"style\":{\"bg\":\"surface\",\"pad\":20,\"gap\":16},"
        "\"children\":[{\"element\":\"text\",\"text\":\"RETAINED SCENE / OWNED FIXTURE\",\"size\":{\"h\":36}},"
        "{\"element\":\"text\",\"text\":\"This panel survives draft edits, tool phases and window resizing.\",\"size\":{\"h\":32}},"
        "{\"element\":\"text\",\"text\":\"m: menu priority   t: phase change   q: close fixture\",\"size\":{\"h\":32}}]}";
    if (pixel_tui_render_scene_json(stderr, spec) <= 0) {
        pixel_tui_session_end(stderr); return 2;
    }
    pixel_tui_session_set_input(stderr, "Draft remains usable: Astra π 🦉", strlen("Draft remains usable: Astra π 🦉"), true);
    uint64_t operation = pixel_tui_session_tool_begin(stderr, "owned_fixture_probe", "{}");
    pixel_tui_session_tool_end(stderr, operation, "owned_fixture_probe", true, 0.01, "complete");
    raw = saved; raw.c_lflag &= (tcflag_t)~(ICANON | ECHO);
    raw.c_cc[VMIN] = 1; raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) {
        pixel_tui_session_end(stderr); return 2;
    }
    signal(SIGTERM, stop_live); signal(SIGINT, stop_live);
    bool menu = false;
    unsigned phase = 0;
    char input_hex[2049] = {0};
    size_t input_hex_len = 0;
    int escape_state = 0;
    double deadline = monotonic_s() + 300.0;
    while (!live_stop && monotonic_s() < deadline) {
        live_status(input_hex);
        struct pollfd input = {.fd = STDIN_FILENO, .events = POLLIN};
        if (poll(&input, 1, 250) <= 0) continue;
        if (input.revents & (POLLHUP | POLLERR)) break;
        unsigned char key;
        if (read(STDIN_FILENO, &key, 1) != 1) break;
        if (input_hex_len + 2 < sizeof(input_hex)) {
            snprintf(input_hex + input_hex_len, 3, "%02x", key); input_hex_len += 2;
        }
        int command = live_command_byte(&escape_state, key);
        if (command == 'q') break;
        if (!command) continue;
        if (command == 'm') {
            menu = !menu;
            pixel_tui_menu_item_t item = {.label = "OWNED MENU HAS PRIORITY", .detail = "m restores retained scene"};
            pixel_tui_session_set_composer(stderr, menu ? "/fixture" : "Draft remains usable",
                menu ? 8 : 20, true, menu ? PIXEL_TUI_MENU_COMMANDS : PIXEL_TUI_MENU_NONE,
                menu ? &item : NULL, menu ? 1 : 0, 0);
        } else if (command == 't') {
            pixel_tui_session_set_state(stderr, (pixel_tui_state_t)(++phase % 4));
        }
    }
    pixel_tui_session_end(stderr);
    (void)tcsetattr(STDIN_FILENO, TCSANOW, &saved);
    puts("owned retained-scene fixture closed");
    return 0;
}

static void test_animation_responsiveness(void) {
    ui_motion_t motion;
    ui_motion_init(&motion, false);
    ui_motion_set(&motion, 1, 0, 1.0, 0.18, UI_MOTION_EASE_OUT, 10.0);
    CHECK(!ui_motion_active(&motion, 10.0), "unseeded resting target does not request frames");
    ui_motion_snap(&motion, 1, 0, 0.0);
    ui_motion_set(&motion, 1, 0, 1.0, 0.18, UI_MOTION_EASE_OUT, 10.0);
    double mid = ui_motion_value(&motion, 1, 0, 10.09, 0.0);
    CHECK(mid > 0.8 && mid < 1.0, "entrance is mostly visible at 90ms");
    for (int i = 1; i < 18; i++)
        ui_motion_set(&motion, 1, 0, 1.0, 0.18, UI_MOTION_EASE_OUT, 10.0 + i * 0.01);
    CHECK(!ui_motion_active(&motion, 10.181), "repeated target preserves 180ms completion");
    CHECK(ui_motion_value(&motion, 1, 0, 10.181, 0.0) == 1.0, "repeated target reaches endpoint");
    ui_motion_set(&motion, 1, 0, 0.0, 0.18, UI_MOTION_EASE_OUT, 11.0);
    CHECK(ui_motion_active(&motion, 11.01), "changed target still animates");
    ui_motion_set(&motion, 1, 0, 0.0, 0.0, UI_MOTION_EASE_OUT, 11.01);
    CHECK(!ui_motion_active(&motion, 11.01), "explicit immediate target snaps");
    ui_motion_snap(&motion, 1, 0, 0.0);
    ui_motion_set(&motion, 1, 0, 1.0, 0.18, UI_MOTION_SPRING, 12.0);
    double before = ui_motion_value(&motion, 1, 0, 12.09, 0.0);
    ui_motion_set(&motion, 1, 0, 1.0, 0.18, UI_MOTION_SPRING, 12.09);
    CHECK(fabs(ui_motion_value(&motion, 1, 0, 12.09, 0.0) - before) < 1e-9,
          "identical spring target retains position");
    CHECK(motion.tracks[0].start_s == 12.0, "identical spring target retains timeline");
    motion.reduced = true;
    ui_motion_set(&motion, 1, 0, 1.0, 0.18, UI_MOTION_SPRING, 12.10);
    CHECK(!ui_motion_active(&motion, 12.10), "reduced motion overrides identical target");
    int saved_interval = g_session.animation_interval_ms;
    double saved_cost = g_session.background_frame_cost_ema_ms;
    bool saved_input = atomic_load(&g_composer_input_active);
    g_session.animation_interval_ms = 17;
    g_session.background_frame_cost_ema_ms = 0.0;
    atomic_store(&g_composer_input_active, false);
    CHECK(fabs(session_repaint_interval_s() - 0.017) < 1e-9, "healthy frames target near 60Hz");
    g_session.background_frame_cost_ema_ms = 70.0;
    CHECK(fabs(session_repaint_interval_s() - 0.1) < 1e-9, "expensive frames retain duty backoff");
    atomic_store(&g_composer_input_active, true);
    CHECK(fabs(session_repaint_interval_s() - 0.2) < 1e-9, "typing retains tighter background budget");
    CHECK(session_composer_repaint_interval_s() == 0.008, "typing keeps independent 8ms clock");
    g_session.animation_interval_ms = saved_interval;
    g_session.background_frame_cost_ema_ms = saved_cost;
    atomic_store(&g_composer_input_active, saved_input);
}

int main(int argc, char **argv) {
    test_animation_responsiveness();
    test_tool_result_soft_wrap();
    if (argc == 2 && !strcmp(argv[1], "--live")) return run_live_fixture();
    int parser_state = 0, commands = 0;
    const char *packets = "\033[<0;25;7M\033[<0;25;7m\033_Gi=123;image missing qmt\033\\"
                          "\033]title has mqt\007\033Oq\033[?64;1;2c";
    for (const unsigned char *p = (const unsigned char *)packets; *p; p++)
        if (live_command_byte(&parser_state, *p)) commands++;
    CHECK(commands == 0 && parser_state == 0, "fixture ignores mouse and terminal response packets");
    CHECK(live_command_byte(&parser_state, 'm') == 'm' &&
          live_command_byte(&parser_state, 't') == 't' &&
          live_command_byte(&parser_state, 'q') == 'q', "fixture preserves deliberate key commands");
    int master = -1, slave = -1;
    struct winsize size = {.ws_col = 80, .ws_row = 24, .ws_xpixel = 800, .ws_ypixel = 480};
    if (openpty(&master, &slave, NULL, NULL, &size) != 0) return 2;
    FILE *out = fdopen(slave, "w");
    if (!out) { close(master); close(slave); return 2; }
    setvbuf(out, NULL, _IONBF, 0);
    drain_t drain = {.fd = master};
    pthread_t reader;
    if (pthread_create(&reader, NULL, drain_pty, &drain) != 0) {
        fclose(out); close(master); return 2;
    }
    g_session = (pixel_session_t){
        .active = true, .cols = 80, .rows = 24, .width = 800, .height = 480,
        .surface_width = 800, .surface_height = 480, .backing_scale = 1,
        .current_message = -1, .capture_message = -1,
        .capture_read_fd = -1, .capture_write_fd = -1, .devnull_fd = -1,
        .saved_stdout_fd = dup(STDOUT_FILENO), .saved_stderr_fd = dup(STDERR_FILENO),
        .tty_out = out, .animation_interval_ms = 33, .patch_enabled = true,
    };
    ui_motion_init(&g_session.motion, true);
    atomic_store(&g_session_active_fast, true);
    atomic_store(&g_composer_accepting_input, true);
    const char *scene = "{\"element\":\"stack\",\"children\":["
                        "{\"element\":\"text\",\"text\":\"retained scene A\",\"size\":{\"h\":32}}]}";
    char *caller_json = strdup(scene);
    CHECK(pixel_tui_render_scene_json(out, caller_json) > 0, "initial retained scene upload");
    memset(caller_json, 'x', strlen(caller_json)); free(caller_json);
    check_owned_scene(scene, "scene copied caller-owned JSON");
    CHECK(g_session.scene_placed, "scene initially visible");
    uint32_t image = g_session.scene_image_id;

    pixel_tui_session_set_input(out, "draft", 5, true);
    check_owned_scene(scene, "typing preserves scene");
    CHECK(g_session.scene_image_id == image, "ordinary typing reuses scene image");
    pixel_tui_session_set_state(out, PIXEL_TUI_REASONING);
    pixel_tui_session_begin_message(out, "ASSISTANT", "fixture");
    pixel_tui_session_append_text(out, "owned fixture output");
    uint64_t operation = pixel_tui_session_tool_begin(out, "fixture", "{}");
    pixel_tui_session_tool_end(out, operation, "fixture", true, 0.001, "done");
    pixel_tui_session_set_turn(out, 2);
    check_owned_scene(scene, "phase, transcript, tool and turn preserve scene");

    pixel_tui_menu_item_t item = {.label = "fixture menu", .detail = "owned"};
    pixel_tui_session_set_composer(out, "/", 1, true, PIXEL_TUI_MENU_COMMANDS, &item, 1, 0);
    CHECK(!g_session.scene_placed && g_session.scene_json, "menu hides but retains scene");
    pixel_tui_session_set_input(out, "draft", 5, true);
    CHECK(g_session.scene_placed, "closing menu restores scene");
    pixel_tui_session_show_modal(out, PIXEL_TUI_MODAL_MENU, "fixture", "owned", &item, 1, 0, "done");
    CHECK(!g_session.scene_placed && g_session.scene_json, "modal hides but retains scene");
    pixel_tui_session_clear_modal(out);
    CHECK(g_session.scene_placed, "closing modal restores scene");

    session_lock();
    g_session.overlay_image_id = 0x1234U;
    session_place_scene(out, false);
    CHECK(!g_session.scene_placed, "transient inspector takes precedence");
    session_clear_overlay(out);
    session_unlock();
    CHECK(g_session.scene_placed, "clearing transient overlay restores retained scene");

    image = g_session.scene_image_id;
    size.ws_col = 100; size.ws_row = 30; size.ws_xpixel = 1000; size.ws_ypixel = 600;
    CHECK(ioctl(slave, TIOCSWINSZ, &size) == 0, "resize owned PTY");
    pixel_tui_session_refresh(out);
    check_owned_scene(scene, "resize preserves scene JSON");
    CHECK(g_session.scene_image_id != image && g_session.scene_placed,
          "resize rebuilds and reanchors scene image");
    session_deck_geometry_t deck = session_deck_geometry(g_session.width, g_session.height,
                                                        g_session.state);
    CHECK((g_session.scene_row - 1 + g_session.scene_rows) * g_session.height /
              g_session.rows <= deck.composer.y,
          "scene placement cannot overlap composer");

    image = g_session.scene_image_id;
    CHECK(pixel_tui_render_scene_json(out, "{invalid") == 0, "invalid replacement rejected");
    check_owned_scene(scene, "invalid replacement preserves previous scene");
    CHECK(image == g_session.scene_image_id && g_session.scene_placed,
          "invalid replacement keeps previous image placement");
    char *huge = malloc(PIXEL_TUI_SCENE_JSON_MAX + 2U);
    memset(huge, ' ', PIXEL_TUI_SCENE_JSON_MAX + 1U);
    huge[PIXEL_TUI_SCENE_JSON_MAX + 1U] = '\0';
    CHECK(pixel_tui_render_scene_json(out, huge) == 0, "oversized scene rejected");
    free(huge);
    check_owned_scene(scene, "oversized replacement preserves scene");

    pixel_tui_session_suspend_terminal(out);
    CHECK(!g_session.scene_placed && g_session.scene_json, "TTY handoff hides scene only");
    CHECK(pixel_tui_render_scene_json(out, scene) == 0, "no scene writes into borrowed terminal");
    pixel_tui_session_resume_terminal(out);
    session_restore_stdio(); /* Keep this test process diagnostics visible. */
    CHECK(g_session.scene_placed && g_session.scene_image_id != image,
          "TTY resume rebuilds retained scene");

    const char *replacement = "{\"element\":\"text\",\"text\":\"scene B\"}";
    CHECK(pixel_tui_render_scene_json(out, replacement) > 0, "explicit replacement succeeds");
    check_owned_scene(replacement, "replacement owns new JSON");
    char result[512];
    CHECK(!tool_ui_render("{\"action\":\"close\",\"action\":\"render\"}", result, sizeof(result)),
          "duplicate action rejected before mutation");
    CHECK(!tool_ui_render("{\"action\":\"close\\u0000suffix\"}", result, sizeof(result)),
          "NUL action rejected before mutation");
    CHECK(!tool_ui_render("{\"action\":null}", result, sizeof(result)), "nonstring action rejected");
    check_owned_scene(replacement, "ambiguous action cannot dismiss scene");
    char tiny[8];
    CHECK(!tool_ui_render("{\"action\":\"close\"}", tiny, sizeof(tiny)) && !tiny[0],
          "undersized close result rejected without truncated JSON");
    check_owned_scene(replacement, "undersized close result cannot dismiss scene");
    char artifact_dir[] = "/tmp/dsco-pixel-scene-artifact-XXXXXX";
    CHECK(mkdtemp(artifact_dir) != NULL, "owned artifact fixture directory");
    char artifact_path[256];
    snprintf(artifact_path, sizeof(artifact_path), "%s/quote\"and\\slash.ppm", artifact_dir);
    jbuf_t request;
    jbuf_init(&request, 512);
    jbuf_append(&request, "{\"spec\":"); jbuf_append(&request, replacement);
    jbuf_append(&request, ",\"width\":320,\"ppm_path\":");
    jbuf_append_json_str(&request, artifact_path); jbuf_append(&request, "}");
    CHECK(tool_ui_render(request.data, result, sizeof(result)), "quoted artifact path render succeeds");
    yyjson_doc *response = yyjson_read(result, strlen(result), 0);
    yyjson_val *ppm = response ? yyjson_obj_get(yyjson_doc_get_root(response), "ppm") : NULL;
    CHECK(ppm && yyjson_equals_str(ppm, artifact_path), "artifact path response round-trips exact quotes and backslash");
    yyjson_doc_free(response);
    char limited[64];
    CHECK(!tool_ui_render(request.data, limited, sizeof(limited)) &&
          json_is_valid_container(limited) && strstr(limited, "result_too_small"),
          "artifact response overflow returns false with complete error JSON");
    jbuf_free(&request);
    CHECK(!pixel_tui_write_scene_ppm(artifact_path, replacement, INT32_MAX, 0),
          "headless width bounded before canvas allocation");
    CHECK(!pixel_tui_write_scene_ppm(artifact_path, replacement, 320, INT32_MAX),
          "headless height bounded before canvas allocation");
    CHECK(!tool_ui_render("{\"spec\":{\"element\":\"text\"},\"width\":2147483647}", result, sizeof(result)),
          "tool width bounded before mutation or canvas allocation");
    CHECK(unlink(artifact_path) == 0 && rmdir(artifact_dir) == 0, "owned artifact fixture removed");
    CHECK(tool_ui_render("{\"action\":\"close\"}", result, sizeof(result)) &&
          strstr(result, "\"closed\":true"), "tool close is callable without spec");
    CHECK(!g_session.scene_json && !g_session.scene_image_id, "close releases JSON and image");
    CHECK(tool_ui_render("{\"action\":\"close\"}", result, sizeof(result)) &&
          strstr(result, "\"closed\":false"), "close is idempotent");

    CHECK(pixel_tui_render_scene_json(out, scene) > 0, "scene before suspended close");
    pixel_tui_session_suspend_terminal(out);
    CHECK(pixel_tui_clear_scene(out), "close while suspended");
    pixel_tui_session_resume_terminal(out);
    session_restore_stdio();
    CHECK(!g_session.scene_json && !g_session.scene_image_id && !g_session.scene_placed,
          "suspended close cannot resurrect scene on resume");
    CHECK(pixel_tui_render_scene_json(out, scene) > 0, "scene before shutdown");
    pixel_tui_session_set_input(out, "active compositor", 17, true);
    session_lock();
    double before_phase = monotonic_s();
    uLong before_phase_pixels = crc32(0, g_session.prev_frame,
        (uInt)((size_t)g_session.prev_frame_width * g_session.prev_frame_height * 3U));
    session_animation_start();
    session_unlock();
    pixel_tui_session_set_state(out, PIXEL_TUI_EXECUTING);
    CHECK(wait_background_after(before_phase, -1), "active editor cannot starve phase publication");
    session_lock();
    CHECK(crc32(0, g_session.prev_frame,
              (uInt)((size_t)g_session.prev_frame_width * g_session.prev_frame_height * 3U)) !=
              before_phase_pixels &&
          g_session.prev_frame_image == g_session.image_ids[PIXEL_TUI_EXECUTING] &&
          g_session.input_active, "new phase pixels published while editor remains active (patch or replacement)");
    g_session.transition_started_s = 0.0; /* Stable colors for raster parity below. */
    double before_menu = monotonic_s();
    session_unlock();
    pixel_tui_menu_item_t live_item = {.label = "MENU ORIGINAL", .detail = "owned fixture"};
    pixel_tui_session_set_composer(out, "/first", 6, true, PIXEL_TUI_MENU_COMMANDS, &live_item, 1, 0);
    CHECK(wait_background_after(before_menu, 1), "active editor publishes initial menu frame");
    session_lock();
    double before_menu_patch = g_session.last_composer_paint_s;
    session_unlock();
    live_item.label = "MENU UPDATED";
    pixel_tui_session_set_composer(out, "/second", 7, true, PIXEL_TUI_MENU_COMMANDS, &live_item, 1, 0);
    double patch_deadline = monotonic_s() + 1.5;
    bool patched = false;
    while (monotonic_s() < patch_deadline) {
        session_lock();
        patched = !g_session.composer_repaint_pending &&
                  g_session.last_composer_paint_s > before_menu_patch;
        session_unlock();
        if (patched) break;
        struct timespec tick = {.tv_nsec = 5000000}; nanosleep(&tick, NULL);
    }
    CHECK(patched, "same-shape menu updates use responsive composer lane");
    session_lock();
    px_canvas_t *reference = render_session_frame(g_session.width, g_session.height,
        g_session.backing_scale, g_session.surface_width, g_session.surface_height,
        g_session.model, g_session.state);
    deck = session_deck_geometry(g_session.width, g_session.height, g_session.state);
    int menu_top = deck.deck_y - 27 - 20 - 6;
    int bright = 0, matched = 0;
    if (reference && g_session.prev_frame) {
        const px_color_t *actual = (const px_color_t *)g_session.prev_frame;
        for (int y = menu_top + 5; y < deck.deck_y - 8; y++) {
            for (int x = deck.composer.x + 8; x < deck.composer.x + 300; x++) {
                size_t at = (size_t)y * (size_t)reference->pixel_width + (size_t)x;
                px_color_t expected = reference->pixels[at], got = actual[at];
                if ((int)expected.r + expected.g + expected.b > 360) {
                    bright++;
                    if ((int)got.r + got.g + got.b > 300) matched++;
                }
            }
        }
    }
    CHECK(bright > 20 && matched * 10 >= bright * 9,
          "menu patch retains visible text at full-render menu coordinates");
    free_canvas(reference);
    /* No mailbox, key, wheel, resize or structural event follows this reveal.
     * Its own damage must publish while the editor remains focused. */
    pixel_message_t *reveal_message = session_new_message("ASSISTANT", NULL);
    CHECK(message_text_set_plain(reveal_message,
          "Transcript progress must paint without mouse input. "
          "Retained reveal damage must survive the drained stream mailbox."),
          "reveal-only message fixture");
    reveal_message->reveal_len = 0;
    reveal_message->reveal_pending = true;
    reveal_message->streaming = false;
    g_session.animation_enabled = true;
    g_session.input_active = true;
    g_session.stream_repaint_pending = false;
    g_session.structural_repaint_pending = false;
    g_session.stream_repaint_pending_since_s = 0.0;
    double before_reveal = monotonic_s();
    session_animation_wake();
    session_unlock();
    CHECK(wait_background_after(before_reveal, -1),
          "focused editor publishes reveal-only transcript without mouse events");
    session_lock();
    CHECK(reveal_message->reveal_len > 0 && g_session.input_active,
          "reveal progressed without stealing composer focus");
    session_unlock();
    pixel_tui_session_end(out); /* Owns and closes the slave FILE. */
    CHECK(!g_session.scene_json && !g_session.scene_image_id && !g_session.active,
          "session shutdown releases scene ownership");
    atomic_store(&drain.stop, true);
    pthread_join(reader, NULL);
    close(master);
    CHECK(drain.bytes > 1000, "real graphics bytes traversed only the owned PTY");
    printf("pixel scene lifetime: %d checks, %d failures; %zu captured PTY bytes\n",
           checks, failures, drain.bytes);
    return failures ? 1 : 0;
}
