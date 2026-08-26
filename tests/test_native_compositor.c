#include "compositor_parity.h"
#include "compositor_stream_bench.h"
#include "native_composer.h"
#include "native_masthead.h"
#include "pixel_tui.h"
#include "rich_text.h"
#include "pixel_tui_perf.h"
#include "px_backend.h"
#include "ui_motion.h"
#include "vm.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Normally supplied by main.c, which is intentionally excluded from the
 * headless compositor test binary. */
int g_cheap_mode = 0;
vm_t g_vm;
volatile int g_interrupted = 0;
double g_cost_budget = 0.0;

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(condition, ...)                                                                      \
    do {                                                                                           \
        if (condition) {                                                                           \
            g_pass++;                                                                              \
        } else {                                                                                   \
            g_fail++;                                                                              \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                                   \
            fprintf(stderr, __VA_ARGS__);                                                          \
            fputc('\n', stderr);                                                                   \
        }                                                                                          \
    } while (0)

static const native_ui_node_t *find_key(const native_ui_scene_t *scene, uint64_t key) {
    if (!scene)
        return NULL;
    for (int i = 0; i < scene->count; i++)
        if (scene->nodes[i].key == key)
            return &scene->nodes[i];
    return NULL;
}

static bool frame_inside(native_ui_rect_t frame, native_ui_rect_t viewport) {
    return frame.x >= viewport.x && frame.y >= viewport.y && frame.width >= 0 &&
           frame.height >= 0 && frame.x + frame.width <= viewport.x + viewport.width &&
           frame.y + frame.height <= viewport.y + viewport.height;
}

static native_masthead_model_t fixture_model(void) {
    return (native_masthead_model_t){
        .title = "DSCO / AGENT WORKSPACE",
        .model = "openai/gpt-5.6-luna",
        .slot = "native",
        .state_label = "executing",
        .state = NATIVE_UI_AGENT_EXECUTING,
        .turn = 7,
        .input_tokens = 1234,
        .output_tokens = 567,
        .tools_used = 9,
        .queue_depth = 2,
        .queue_capacity = 8,
        .context_percent = 41.0,
        .cost_usd = 0.1275,
        .show_compact_metrics = true,
    };
}

static void test_masthead_semantics_and_layout(void) {
    native_ui_scene_t scene;
    native_masthead_model_t model = fixture_model();
    CHECK(native_masthead_build(&scene, 1120, 52, &model), "dense masthead should build");
    CHECK(scene.count == 12, "masthead should have 12 stable nodes, got %d", scene.count);
    CHECK(scene.nodes[scene.root].key == NATIVE_MASTHEAD_KEY_ROOT &&
              scene.nodes[scene.root].role == NATIVE_UI_ROLE_HEADER,
          "root should retain masthead identity and header role");

    const native_ui_node_t *title = find_key(&scene, NATIVE_MASTHEAD_KEY_TITLE);
    const native_ui_node_t *model_node = find_key(&scene, NATIVE_MASTHEAD_KEY_MODEL);
    const native_ui_node_t *metrics = find_key(&scene, NATIVE_MASTHEAD_KEY_METRICS);
    const native_ui_node_t *state = find_key(&scene, NATIVE_MASTHEAD_KEY_STATE);
    const native_ui_node_t *soul = find_key(&scene, NATIVE_MASTHEAD_KEY_SOUL);
    CHECK(title && !strcmp(title->text, "DSCO / AGENT WORKSPACE"),
          "title should be authoritative retained text");
    CHECK(model_node && strstr(model_node->text, "gpt-5.6-luna") &&
              strstr(model_node->text, "native"),
          "model and slot should share one semantic identity line");
    CHECK(metrics && strstr(metrics->text, "CTX 41%") && strstr(metrics->text, "9 TOOLS"),
          "dense resource metrics should be retained");
    CHECK(state && !strcmp(state->text, "EXECUTING") && state->element == NATIVE_UI_ELEMENT_BADGE,
          "agent lifecycle should be a semantic badge");
    CHECK(soul && soul->element == NATIVE_UI_ELEMENT_CUSTOM &&
              soul->agent_state == NATIVE_UI_AGENT_EXECUTING,
          "soul mark should retain lifecycle semantics");

    bool unique = true;
    for (int i = 0; i < scene.count; i++) {
        if ((scene.nodes[i].state & NATIVE_UI_STATE_VISIBLE) &&
            !frame_inside(scene.nodes[i].frame, scene.viewport))
            unique = false;
        for (int j = i + 1; j < scene.count; j++)
            if (scene.nodes[i].key == scene.nodes[j].key)
                unique = false;
    }
    CHECK(unique, "visible masthead frames and stable keys should be valid");
    CHECK(state && state->accessibility_label[0] && soul && soul->accessibility_label[0],
          "commandable status needs accessibility labels");
}

static void test_masthead_density_and_damage(void) {
    native_ui_scene_t compact;
    native_masthead_model_t model = fixture_model();
    CHECK(!native_masthead_build(&compact, 159, 52, &model) &&
              !native_masthead_build(&compact, 420, 43, &model),
          "masthead should reject geometry that cannot preserve its regions");
    model.turn = 0;
    model.show_compact_metrics = false;
    CHECK(native_masthead_build(&compact, 420, 48, &model), "compact masthead should build");
    const native_ui_node_t *metrics = find_key(&compact, NATIVE_MASTHEAD_KEY_METRICS);
    const native_ui_node_t *turn = find_key(&compact, NATIVE_MASTHEAD_KEY_TURN);
    CHECK(metrics && !(metrics->state & NATIVE_UI_STATE_VISIBLE),
          "inspector layouts should suppress duplicate compact metrics");
    CHECK(turn && !(turn->state & NATIVE_UI_STATE_VISIBLE),
          "turn zero should not reserve visible status text");

    native_ui_scene_t before;
    native_ui_scene_t after;
    model = fixture_model();
    CHECK(native_masthead_build(&before, 1120, 52, &model), "damage fixture should build");
    model.input_tokens += 111;
    model.output_tokens += 17;
    model.cost_usd += 0.25;
    CHECK(native_masthead_build(&after, 1120, 52, &model), "mutated damage fixture should build");
    native_ui_damage_t damage;
    native_ui_diff(&before, &after, &damage);
    const native_ui_node_t *after_metrics = find_key(&after, NATIVE_MASTHEAD_KEY_METRICS);
    CHECK(!damage.full_repaint && damage.count == 1,
          "metric mutation should produce one semantic dirty region, got %d", damage.count);
    CHECK(after_metrics && damage.count == 1 && damage.regions[0].x == after_metrics->frame.x &&
              damage.regions[0].width == after_metrics->frame.width,
          "damage should stay on the retained metrics node");
}

static native_composer_model_t composer_fixture(void) {
    return (native_composer_model_t){
        .text = "inspect queue\nthen continue",
        .cursor = 18,
        .active = true,
        .agent_state = NATIVE_UI_AGENT_EXECUTING,
        .columns = 72,
        .max_rows = NATIVE_UI_COMPOSER_MAX_ROWS,
        .queue_depth = 2,
        .queue_capacity = 8,
        .clock = "10:42",
        .compact = false,
        .accent_opacity = 224,
    };
}

static void test_composer_semantics_and_damage(void) {
    native_ui_scene_t scene;
    native_composer_model_t model = composer_fixture();
    CHECK(native_composer_build(&scene, 1120, 84, &model), "dense retained composer should build");
    CHECK(scene.count == 12, "composer should have 12 stable nodes, got %d", scene.count);
    CHECK(scene.nodes[scene.root].key == NATIVE_COMPOSER_KEY_ROOT &&
              scene.nodes[scene.root].role == NATIVE_UI_ROLE_COMPOSER &&
              (scene.nodes[scene.root].state & NATIVE_UI_STATE_LIVE),
          "composer root should expose its persistent live-region identity");

    const native_ui_node_t *input = find_key(&scene, NATIVE_COMPOSER_KEY_INPUT);
    const native_ui_node_t *queue = find_key(&scene, NATIVE_COMPOSER_KEY_LIVE);
    const native_ui_node_t *clock = find_key(&scene, NATIVE_COMPOSER_KEY_CLOCK);
    CHECK(input && input->element == NATIVE_UI_ELEMENT_CUSTOM &&
              input->role == NATIVE_UI_ROLE_COMPOSER && (input->state & NATIVE_UI_STATE_FOCUSED),
          "shared editor should be hosted by the focused compositor node");
    CHECK(input && strstr(input->text, "inspect queue") &&
              strstr(input->accessibility_label, "2 lines") &&
              strstr(input->accessibility_label, "cursor row 2"),
          "multiline text and cursor position should survive semantically");
    CHECK(queue && strstr(queue->text, "QUEUE 2/8") &&
              queue->style.foreground == NATIVE_UI_COLOR_WARNING,
          "queued input should project into the retained status region");
    CHECK(clock && (clock->state & NATIVE_UI_STATE_VISIBLE) && !strcmp(clock->text, "10:42"),
          "composer clock should remain a stable semantic node");

    bool valid = true;
    for (int i = 0; i < scene.count; i++) {
        if ((scene.nodes[i].state & NATIVE_UI_STATE_VISIBLE) &&
            !frame_inside(scene.nodes[i].frame, scene.viewport))
            valid = false;
        for (int j = i + 1; j < scene.count; j++)
            if (scene.nodes[i].key == scene.nodes[j].key)
                valid = false;
    }
    CHECK(valid, "composer frames and retained keys should remain valid");

    native_ui_scene_t before = scene;
    model.text = "inspect queue\nthen continue now";
    model.cursor = strlen(model.text);
    native_ui_scene_t after;
    CHECK(native_composer_build(&after, 1120, 84, &model), "edited composer scene should build");
    native_ui_damage_t damage;
    native_ui_diff(&before, &after, &damage);
    const native_ui_node_t *after_input = find_key(&after, NATIVE_COMPOSER_KEY_INPUT);
    CHECK(!damage.full_repaint && damage.count == 1,
          "editor mutation should dirty one retained region, got %d", damage.count);
    CHECK(after_input && damage.count == 1 && damage.regions[0].x == after_input->frame.x &&
              damage.regions[0].width == after_input->frame.width,
          "editor damage should stay bounded to the custom input node");

    native_ui_scene_t compact;
    model = composer_fixture();
    model.compact = true;
    model.clock = NULL;
    CHECK(native_composer_build(&compact, 420, 56, &model),
          "compact composer should preserve the shared editor");
    const native_ui_node_t *hint = find_key(&compact, NATIVE_COMPOSER_KEY_HINT);
    clock = find_key(&compact, NATIVE_COMPOSER_KEY_CLOCK);
    CHECK(hint && strstr(hint->text, "PGUP PGDN") && !strstr(hint->text, "OPTION+ENTER"),
          "compact hint should retain essential editing controls");
    CHECK(clock && !(clock->state & NATIVE_UI_STATE_VISIBLE),
          "missing clock should collapse without changing node identity");
    CHECK(!native_composer_build(&compact, 159, 84, &model) &&
              !native_composer_build(&compact, 420, 55, &model),
          "composer should reject layouts that cannot preserve its regions");
}

typedef struct {
    int begins;
    int fills;
    int strokes;
    int texts;
    int lines;
    int circles;
    int customs;
    int clips;
    px_backend_color_t last_fill;
} fake_surface_t;

static void fake_begin(void *surface, native_ui_rect_t viewport, const native_ui_damage_t *damage) {
    fake_surface_t *fake = surface;
    if (viewport.width > 0 && damage)
        fake->begins++;
}

static void fake_push_clip(void *surface, native_ui_rect_t rect) {
    fake_surface_t *fake = surface;
    if (rect.width > 0)
        fake->clips++;
}

static void fake_pop_clip(void *surface) {
    (void)surface;
}

static void fake_fill(void *surface, native_ui_rect_t rect, px_backend_color_t color,
                      uint8_t opacity, uint8_t radius, bool raised) {
    fake_surface_t *fake = surface;
    (void)opacity;
    (void)radius;
    (void)raised;
    if (rect.width > 0 && rect.height > 0)
        fake->fills++;
    fake->last_fill = color;
}

static void fake_stroke(void *surface, native_ui_rect_t rect, px_backend_color_t color,
                        uint8_t opacity, uint8_t width, uint8_t radius) {
    fake_surface_t *fake = surface;
    (void)color;
    (void)opacity;
    (void)width;
    (void)radius;
    if (rect.width > 0)
        fake->strokes++;
}

static void fake_text(void *surface, native_ui_rect_t rect, const char *text,
                      native_ui_type_token_t type, px_backend_color_t color, uint8_t opacity) {
    fake_surface_t *fake = surface;
    (void)type;
    (void)color;
    (void)opacity;
    if (rect.width > 0 && text && *text)
        fake->texts++;
}

static void fake_line(void *surface, int x0, int y0, int x1, int y1, px_backend_color_t color,
                      uint8_t opacity) {
    fake_surface_t *fake = surface;
    (void)x0;
    (void)y0;
    (void)x1;
    (void)y1;
    (void)color;
    (void)opacity;
    fake->lines++;
}

static void fake_circle(void *surface, int cx, int cy, int radius, px_backend_color_t color,
                        uint8_t opacity) {
    fake_surface_t *fake = surface;
    (void)cx;
    (void)cy;
    (void)color;
    (void)opacity;
    if (radius > 0)
        fake->circles++;
}

static void fake_custom(void *surface, const native_ui_node_t *node,
                        const px_backend_palette_t *palette) {
    fake_surface_t *fake = surface;
    (void)palette;
    if (node)
        fake->customs++;
}

static void test_pixel_backend_contract(void) {
    native_ui_scene_t scene;
    native_ui_scene_init(&scene, 320, 120);
    native_ui_node_t *root = native_ui_scene_node(&scene, scene.root);
    root->style.flow = NATIVE_UI_FLOW_COLUMN;
    root->style.background = NATIVE_UI_COLOR_SURFACE;
    root->style.border = NATIVE_UI_COLOR_BORDER;
    root->style.border_width = 1;
    root->style.padding = (native_ui_insets_t){4, 4, 4, 4};
    root->style.gap = 4;
    root->state |= NATIVE_UI_STATE_CLIPS;

    int meter_index = native_ui_scene_add(&scene, scene.root, 101, NATIVE_UI_ELEMENT_METER,
                                          NATIVE_UI_ROLE_STATUS);
    int spark_index = native_ui_scene_add(&scene, scene.root, 102, NATIVE_UI_ELEMENT_SPARKLINE,
                                          NATIVE_UI_ROLE_TIMELINE);
    int custom_index = native_ui_scene_add(&scene, scene.root, 103, NATIVE_UI_ELEMENT_CUSTOM,
                                           NATIVE_UI_ROLE_AGENT_TOPOLOGY);
    CHECK(meter_index > 0 && spark_index > 0 && custom_index > 0,
          "backend fixture nodes should be admitted");
    scene.nodes[meter_index].constraints.preferred_height = 20;
    scene.nodes[meter_index].value = 0.5f;
    scene.nodes[meter_index].style.foreground = NATIVE_UI_COLOR_WARNING;
    scene.nodes[spark_index].constraints.preferred_height = 38;
    native_ui_node_set_text(&scene.nodes[spark_index], "1 4 2 8 5");
    scene.nodes[spark_index].style.foreground = NATIVE_UI_COLOR_ACCENT;
    scene.nodes[custom_index].constraints.grow = 1;
    native_ui_node_set_text(&scene.nodes[custom_index], "transport-extension");
    native_ui_layout(&scene);

    px_backend_palette_t palette = {0};
    for (int i = 0; i < NATIVE_UI_COLOR_COUNT; i++)
        palette.colors[i] = (px_backend_color_t){(uint8_t)i, (uint8_t)(i + 1), (uint8_t)(i + 2)};
    fake_surface_t fake = {0};
    px_backend_ops_t ops = {
        .begin_frame = fake_begin,
        .push_clip = fake_push_clip,
        .pop_clip = fake_pop_clip,
        .fill_rect = fake_fill,
        .stroke_rect = fake_stroke,
        .draw_text = fake_text,
        .draw_line = fake_line,
        .fill_circle = fake_circle,
        .draw_custom = fake_custom,
    };
    px_backend_t backend;
    px_backend_init(&backend, &fake, &palette, &ops);
    native_ui_render(&scene, NULL, px_backend_native_vtable(), &backend);
    const native_ui_damage_t *damage = px_backend_last_damage(&backend);
    CHECK(fake.begins == 1 && damage && damage->full_repaint,
          "backend should receive and retain frame damage");
    CHECK(fake.fills >= 3 && fake.strokes == 1,
          "surface and meter primitives should map to raster operations");
    CHECK(fake.lines == 4 && fake.circles == 1,
          "sparkline should map to segments plus a latest-point marker");
    CHECK(fake.customs == 1 && fake.clips == 1,
          "transport extensions and clip ownership should survive traversal");
    px_backend_color_t warning = px_backend_resolve_color(&backend, NATIVE_UI_COLOR_WARNING);
    CHECK(warning.r == NATIVE_UI_COLOR_WARNING && warning.g == NATIVE_UI_COLOR_WARNING + 1,
          "semantic tokens should resolve through the supplied palette");
    px_backend_color_t missing = px_backend_resolve_color(NULL, NATIVE_UI_COLOR_ACCENT);
    CHECK(missing.r == 0 && missing.g == 0 && missing.b == 0,
          "missing backend should resolve to a safe zero color");
}

static uint64_t file_hash(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file)
        return 0;
    uint64_t hash = UINT64_C(1469598103934665603);
    unsigned char bytes[4096];
    size_t count;
    while ((count = fread(bytes, 1, sizeof(bytes), file)) > 0) {
        for (size_t i = 0; i < count; i++) {
            hash ^= bytes[i];
            hash *= UINT64_C(1099511628211);
        }
    }
    fclose(file);
    return hash;
}

static bool ppm_dimensions(const char *path, int expected_width, int expected_height) {
    FILE *file = fopen(path, "rb");
    if (!file)
        return false;
    char magic[3] = {0};
    int width = 0, height = 0, max_value = 0;
    int read = fscanf(file, "%2s %d %d %d", magic, &width, &height, &max_value);
    fclose(file);
    return read == 4 && !strcmp(magic, "P6") && width == expected_width &&
           height == expected_height && max_value == 255;
}

static void test_headless_session_artifacts(void) {
    bool keep = getenv("DSCO_KEEP_NATIVE_ARTIFACTS") != NULL;
    const struct {
        int width, height;
    } sizes[] = {
        {640, 360},
        {1120, 700},
        {1600, 900},
    };
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        char path[160];
        if (keep)
            snprintf(path, sizeof(path), "/tmp/dsco-native-session-%dx%d.ppm", sizes[i].width,
                     sizes[i].height);
        else
            snprintf(path, sizeof(path), "/tmp/dsco-native-session-%ld-%zu.ppm", (long)getpid(), i);
        bool wrote = pixel_tui_write_session_ppm(path, sizes[i].width, sizes[i].height,
                                                 "openai/gpt-5.6-luna", PIXEL_TUI_IDLE);
        struct stat st = {0};
        CHECK(wrote && stat(path, &st) == 0 && st.st_size > 1024,
              "session artifact %dx%d should contain RGB pixels", sizes[i].width, sizes[i].height);
        CHECK(ppm_dimensions(path, sizes[i].width, sizes[i].height),
              "session artifact should preserve requested geometry");
        if (!keep)
            unlink(path);
    }

    uint64_t hashes[4] = {0};
    for (int state = PIXEL_TUI_IDLE; state <= PIXEL_TUI_RESPONDING; state++) {
        char path[160];
        if (keep)
            snprintf(path, sizeof(path), "/tmp/dsco-native-state-%d.ppm", state);
        else
            snprintf(path, sizeof(path), "/tmp/dsco-native-state-%ld-%d.ppm", (long)getpid(),
                     state);
        CHECK(
            pixel_tui_write_session_ppm(path, 800, 480, "state-fixture", (pixel_tui_state_t)state),
            "each lifecycle state should render headlessly");
        hashes[state] = file_hash(path);
        if (!keep)
            unlink(path);
    }
    bool distinct = true;
    for (int i = 0; i < 4; i++)
        for (int j = i + 1; j < 4; j++)
            if (!hashes[i] || hashes[i] == hashes[j])
                distinct = false;
    CHECK(distinct, "all four lifecycle states should produce distinct surfaces");
    CHECK(!pixel_tui_write_session_ppm(NULL, 800, 480, "bad", PIXEL_TUI_IDLE) &&
              !pixel_tui_write_session_ppm("/tmp/no.ppm", 100, 100, "bad", PIXEL_TUI_IDLE),
          "headless artifact API should reject invalid requests");
}

static void test_populated_fixture_density(void) {
    static const pixel_tui_fixture_message_t messages[] = {
        {.role = "USER", .text = "Explain the repaint pipeline and preserve both renderers."},
        {.role = "ASSISTANT",
         .text =
             "## Pipeline\n\nRetain layout, raster once, diff bounded tiles, then encode "
             "only changed rectangles. Keep Markdown, code, tools, and composer state visible."},
        {.role = "TOOL",
         .detail = "make test_pixel",
         .text = "native compositor tests passed\nkitty transport tests passed"},
    };
    pixel_tui_fixture_t fixture = {
        .model = "openai-codex/gpt-5.6-luna",
        .slot_name = "native subscription",
        .state = PIXEL_TUI_RESPONDING,
        .messages = messages,
        .message_count = 3,
        .input = "Measure the next frame.",
        .input_cursor = 23,
        .turn = 4,
        .input_tokens = 1200,
        .output_tokens = 400,
        .tools_used = 2,
        .context_percent = 31.0,
    };
    char path[160];
    snprintf(path, sizeof(path), "/tmp/dsco-native-fixture-%ld.ppm", (long)getpid());
    pixel_tui_density_metrics_t density = {0};
    CHECK(pixel_tui_write_fixture_ppm(path, 1120, 700, &fixture, &density),
          "populated fixture should render through the real session compositor");
    CHECK(density.transcript_width > 800 && density.transcript_height > 400,
          "responding layout should prioritize transcript area (%dx%d)", density.transcript_width,
          density.transcript_height);
    CHECK(density.wrap_columns >= 100 && density.line_capacity >= 25,
          "dense viewport should preserve review-scale text capacity (%d cols, %d lines)",
          density.wrap_columns, density.line_capacity);
    CHECK(density.visible_messages == 3 && density.visible_chars > 120 &&
              density.source_chars >= density.visible_chars,
          "density accounting should cover the populated semantic fixture "
          "(messages=%d visible=%zu source=%zu)",
          density.visible_messages, density.visible_chars, density.source_chars);
    unlink(path);
}

static void test_long_stream_fixture_retention(void) {
    static const char fragment[] =
        "- **retained** response chunk uses `code` and a [link](https://example.test).\n";
    const size_t target = 60U * 1024U;
    char *text = malloc(target + 1U);
    CHECK(text != NULL, "long-response fixture storage should allocate");
    if (!text)
        return;
    size_t used = 0;
    while (used + sizeof(fragment) - 1U <= target) {
        memcpy(text + used, fragment, sizeof(fragment) - 1U);
        used += sizeof(fragment) - 1U;
    }
    text[used] = '\0';
    pixel_tui_fixture_message_t message = {
        .role = "ASSISTANT",
        .detail = "long deterministic stream",
        .text = text,
    };
    pixel_tui_fixture_t fixture = {
        .model = "openai-codex/gpt-5.6-luna",
        .slot_name = "native subscription",
        .state = PIXEL_TUI_RESPONDING,
        .messages = &message,
        .message_count = 1,
        .turn = 1,
    };
    char path[160];
    snprintf(path, sizeof(path), "/tmp/dsco-native-long-%ld.ppm", (long)getpid());
    pixel_tui_density_metrics_t density = {0};
    CHECK(pixel_tui_write_fixture_ppm(path, 1120, 700, &fixture, &density),
          "60 KiB rich response should render through the native compositor");
    CHECK(density.source_chars >= used - 1U,
          "long response should retain its full source (%zu of %zu chars)", density.source_chars,
          used);
    CHECK(density.wrapped_lines > 256,
          "long response should cross the former 256-line ceiling (%d lines)",
          density.wrapped_lines);
    CHECK(density.visible_lines == density.line_capacity && density.visible_chars > 256,
          "long response tail should fill the visible transcript (%d lines, %zu chars)",
          density.visible_lines, density.visible_chars);
    unlink(path);
    free(text);
}

static void test_frame_telemetry_aggregation(void) {
    const char *old = getenv("DSCO_PIXEL_TUI_PERF");
    char saved[256];
    bool had_old = old != NULL;
    if (had_old)
        snprintf(saved, sizeof(saved), "%s", old);
    setenv("DSCO_PIXEL_TUI_PERF", "1", 1);
    pixel_tui_perf_reset();
    pixel_tui_perf_note_stream_request(false);
    pixel_tui_perf_note_stream_request(true);
    pixel_tui_perf_note_throttled();
    pixel_tui_perf_note_scheduler_wake(1.5, 33.0, 33.0, true);
    pixel_tui_perf_note_scheduler_wake(40.0, 90.0, 33.0, true);
    pixel_tui_perf_note_producer_wait(4.1);
    pixel_tui_perf_note_producer_wait(9.1);
    pixel_tui_frame_sample_t sample = {
        .kind = PIXEL_TUI_FRAME_PATCH,
        .has_queue = true,
        .has_render = true,
        .has_diff = true,
        .has_encode = true,
        .has_upload = true,
        .damage_rects = 2,
        .chunks = 3,
        .raw_bytes = 1200,
        .packed_bytes = 300,
        .encoded_bytes = 400,
        .wire_bytes = 470,
        .retained_bytes = 8192,
        .transient_bytes = 2048,
        .queue_ms = 17.2,
        .render_ms = 2.0,
        .diff_ms = 0.2,
        .encode_ms = 0.5,
        .upload_ms = 0.8,
        .flush_ms = 0.1,
        .frame_ms = 3.1,
    };
    pixel_tui_perf_record(&sample);
    pixel_tui_perf_snapshot_t snapshot = pixel_tui_perf_snapshot();
    CHECK(snapshot.stream_requests == 2 && snapshot.coalesced_requests == 1 &&
              snapshot.throttled_repaints == 1,
          "telemetry should count scheduled, coalesced, and throttled work");
    CHECK(snapshot.scheduler_wakeups == 2 && snapshot.scheduler_renders == 2 &&
              snapshot.scheduler_deadline_misses == 1 && snapshot.scheduler_long_gaps == 1 &&
              snapshot.scheduler_lateness_ms_max == 40.0 && snapshot.frame_gap_ms_max == 90.0,
          "telemetry should expose scheduler misses and animation gaps");
    CHECK(snapshot.frames == 1 && snapshot.patch_frames == 1 && snapshot.damage_rects == 2 &&
              snapshot.wire_bytes == 470,
          "telemetry should aggregate frame kind, damage, and transport bytes");
    CHECK(snapshot.peak_retained_bytes == 8192 && snapshot.peak_transient_bytes == 2048 &&
              snapshot.frame_ms_max == 3.1,
          "telemetry should retain allocation and latency peaks");
    CHECK(snapshot.queue_samples == 1 && snapshot.queue_ms_p50 == 17.25 &&
              snapshot.queue_ms_p95 == 17.25 && snapshot.frame_ms_p99 == 3.25 &&
              snapshot.producer_wait_samples == 2 && snapshot.producer_wait_ms_p50 == 4.25 &&
              snapshot.producer_wait_ms_p99 == 9.25,
          "bounded histograms should expose queue and frame tail percentiles");
    FILE *json = tmpfile();
    CHECK(json && pixel_tui_perf_write_json(json), "telemetry should expose machine-readable JSON");
    if (json) {
        rewind(json);
        char output[4096] = {0};
        size_t n = fread(output, 1, sizeof(output) - 1, json);
        output[n] = '\0';
        CHECK(strstr(output, "dsco.pixel_compositor_perf.v1") &&
                  strstr(output, "\"deadline_misses\":1") &&
                  strstr(output, "\"long_frame_gaps\":1") && strstr(output, "\"wire_bytes\":470") &&
                  strstr(output, "\"producer_wait\":{\"avg\":6.600") &&
                  strstr(output, "\"queue\":{\"avg\":17.200,\"p50\":17.250"),
              "telemetry JSON should carry schema, percentiles, and exact wire count");
        fclose(json);
    }

    compositor_stream_bench_config_t bench = {
        .chunks = 64,
        .interval_us = 1000,
        .model = "bench-model",
    };
    FILE *bench_json = tmpfile();
    CHECK(bench_json &&
              compositor_stream_bench_write_json(bench_json, &bench, 4096, 128.0, &snapshot),
          "stream benchmark summary should be testable without a TTY");
    if (bench_json) {
        rewind(bench_json);
        char output[4096] = {0};
        size_t n = fread(output, 1, sizeof(output) - 1, bench_json);
        output[n] = '\0';
        CHECK(strstr(output, "dsco.compositor_stream_bench.v1") &&
                  strstr(output, "\"chunks\":64") && strstr(output, "\"p99\":17.250"),
              "stream benchmark JSON should retain workload and tail latency");
        fclose(bench_json);
    }
    if (had_old)
        setenv("DSCO_PIXEL_TUI_PERF", saved, 1);
    else
        unsetenv("DSCO_PIXEL_TUI_PERF");
}

static void test_dual_compositor_parity_corpus(void) {
    char directory[160];
    snprintf(directory, sizeof(directory), "/tmp/dsco-compositor-parity-%ld", (long)getpid());
    CHECK(compositor_parity_write(directory, NULL) == 0,
          "dual-compositor parity corpus should generate headlessly");
    const char *names[] = {
        "native.ppm", "legacy.ansi", "legacy.txt", "fixture.json", "metrics.json", "README.md",
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        char path[256];
        snprintf(path, sizeof(path), "%s/%s", directory, names[i]);
        struct stat st = {0};
        CHECK(stat(path, &st) == 0 && st.st_size > 32, "parity artifact %s should be populated",
              names[i]);
    }
    char native_path[256];
    snprintf(native_path, sizeof(native_path), "%s/native.ppm", directory);
    uint64_t first_hash = file_hash(native_path);
    CHECK(compositor_parity_write(directory, NULL) == 0,
          "parity corpus should regenerate in place");
    CHECK(first_hash != 0 && file_hash(native_path) == first_hash,
          "native parity screenshot should be byte-deterministic");
    char metrics_path[256];
    snprintf(metrics_path, sizeof(metrics_path), "%s/metrics.json", directory);
    FILE *metrics = fopen(metrics_path, "r");
    char metrics_json[4096] = {0};
    size_t metrics_len = metrics ? fread(metrics_json, 1, sizeof(metrics_json) - 1, metrics) : 0;
    if (metrics)
        fclose(metrics);
    metrics_json[metrics_len] = '\0';
    const char *ratio_key = strstr(metrics_json, "\"native_to_established_visible_char_ratio\":");
    double ratio = ratio_key ? strtod(strchr(ratio_key, ':') + 1, NULL) : 0.0;
    CHECK(ratio >= 0.95,
          "native transcript should retain at least 95%% of established visible "
          "text on the parity corpus (%.3f)",
          ratio);
    CHECK(strstr(metrics_json, "\"visible_messages\": 5") != NULL,
          "native parity viewport should preserve all fixture messages");
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        char path[256];
        snprintf(path, sizeof(path), "%s/%s", directory, names[i]);
        unlink(path);
    }
    rmdir(directory);
}

/* ── Capture parser: terminal overwrite semantics ─────────────────────── */

typedef struct {
    char lines[32][256];
    int count;
} capture_sink_t;

static bool capture_sink_publish(const char *line, size_t len, void *ctx) {
    capture_sink_t *sink = (capture_sink_t *)ctx;
    if (sink->count < 32) {
        size_t n = len < sizeof(sink->lines[0]) - 1 ? len : sizeof(sink->lines[0]) - 1;
        memcpy(sink->lines[sink->count], line, n);
        sink->lines[sink->count][n] = '\0';
        sink->count++;
    }
    return true;
}

static int capture_run(const char *stream, capture_sink_t *sink) {
    memset(sink, 0, sizeof(*sink));
    pixel_capture_parser_t parser;
    pixel_capture_parser_init(&parser);
    /* Feed one byte at a time so chunk boundaries can't hide state bugs. */
    for (const char *p = stream; *p; p++)
        pixel_capture_parser_feed(&parser, p, 1, capture_sink_publish, sink);
    pixel_capture_parser_finish(&parser, capture_sink_publish, sink);
    return sink->count;
}

/* ── Inline live-operation cards ─────────────────────────────────────── */

static unsigned char *ppm_load(const char *path, int *w_out, int *h_out) {
    FILE *file = fopen(path, "rb");
    if (!file)
        return NULL;
    char magic[3] = {0};
    int w = 0, h = 0, max_value = 0;
    if (fscanf(file, "%2s %d %d %d", magic, &w, &h, &max_value) != 4 || strcmp(magic, "P6") ||
        max_value != 255 || w < 1 || h < 1) {
        fclose(file);
        return NULL;
    }
    (void)fgetc(file); /* single whitespace terminating the header */
    size_t n = (size_t)w * (size_t)h * 3U;
    unsigned char *pixels = malloc(n);
    if (!pixels || fread(pixels, 1, n, file) != n) {
        free(pixels);
        fclose(file);
        return NULL;
    }
    fclose(file);
    *w_out = w;
    *h_out = h;
    return pixels;
}

/* Count saturated pixels in a band, split into warm (amber-ish, r > b) and
 * cool (cyan-ish) channels — the live-op card accent rails. The threshold
 * sits below the default theme's accent chroma (48, ~40 after the 0.80 rail
 * alpha over the panel) but above every neutral surface/dim/text tone. */
static void band_saturation(const unsigned char *pixels, int w, int h, int x0, int y0, int x1,
                            int y1, int *warm_out, int *cool_out) {
    int warm = 0, cool = 0;
    if (x1 > w)
        x1 = w;
    if (y1 > h)
        y1 = h;
    for (int y = y0 > 0 ? y0 : 0; y < y1; y++) {
        for (int x = x0 > 0 ? x0 : 0; x < x1; x++) {
            const unsigned char *p = pixels + ((size_t)y * (size_t)w + (size_t)x) * 3U;
            int mx = p[0] > p[1] ? p[0] : p[1];
            if (p[2] > mx)
                mx = p[2];
            int mn = p[0] < p[1] ? p[0] : p[1];
            if (p[2] < mn)
                mn = p[2];
            if (mx - mn < 32)
                continue;
            if (p[0] > p[2])
                warm++;
            else
                cool++;
        }
    }
    *warm_out = warm;
    *cool_out = cool;
}

static void test_tool_preview_extraction(void) {
    char out[128];
    pixel_tui_tool_preview_extract("bash", "{\"command\":\"make -j8\"}", out, sizeof(out));
    CHECK(!strcmp(out, "make -j8"), "command value should be extracted, got '%s'", out);
    pixel_tui_tool_preview_extract("read_file", "{\"limit\":5, \"file_path\": \"/tmp/a.c\"}", out,
                                   sizeof(out));
    CHECK(!strcmp(out, "/tmp/a.c"), "file_path value should be extracted, got '%s'", out);
    pixel_tui_tool_preview_extract("bash", "{\"command\":\"echo \\\"hi\\\" there\"}", out,
                                   sizeof(out));
    CHECK(!strcmp(out, "echo \"hi\" there"), "escaped quotes should unescape, got '%s'", out);
    pixel_tui_tool_preview_extract("web_search", "{\"query\":\"kitty graphics\"}", out,
                                   sizeof(out));
    CHECK(!strcmp(out, "kitty graphics"), "query value should be extracted, got '%s'", out);
    pixel_tui_tool_preview_extract("bash", "not json at all", out, sizeof(out));
    CHECK(!strcmp(out, "not json at all"), "malformed JSON should fall back to raw text, got '%s'",
          out);
    pixel_tui_tool_preview_extract("bash", NULL, out, sizeof(out));
    CHECK(out[0] == '\0', "NULL input should yield an empty preview");
}

static void test_tool_result_views(void) {
    CHECK(pixel_tui_tool_view_parse(NULL) == PIXEL_TUI_TOOL_VIEW_RESULTS &&
              pixel_tui_tool_view_parse("compact") == PIXEL_TUI_TOOL_VIEW_RESULTS,
          "native tool view should default to outcome-dense results");
    CHECK(pixel_tui_tool_view_parse("calls") == PIXEL_TUI_TOOL_VIEW_CALLS &&
              pixel_tui_tool_view_parse("verbose") == PIXEL_TUI_TOOL_VIEW_FULL,
          "calls and full tool views should parse explicitly");
    CHECK(!strcmp(pixel_tui_tool_view_name(PIXEL_TUI_TOOL_VIEW_RESULTS), "results") &&
              !strcmp(pixel_tui_tool_view_name(PIXEL_TUI_TOOL_VIEW_CALLS), "calls") &&
              !strcmp(pixel_tui_tool_view_name(PIXEL_TUI_TOOL_VIEW_FULL), "full"),
          "tool view names should be stable for diagnostics");

    const char *result = "first useful result\nsecond line\nthird line";
    char preview[256];
    uint32_t tail = 0, bytes = 0;
    pixel_tui_tool_result_preview(result, PIXEL_TUI_TOOL_VIEW_RESULTS, preview, sizeof(preview),
                                  &tail, &bytes);
    CHECK(!strcmp(preview, "first useful result") && tail == 2 && bytes == strlen(result),
          "results view should retain one useful line plus hidden-tail metadata");

    pixel_tui_tool_result_preview(result, PIXEL_TUI_TOOL_VIEW_CALLS, preview, sizeof(preview),
                                  &tail, &bytes);
    CHECK(preview[0] == '\0' && tail == 0 && bytes == strlen(result),
          "calls view should expose status/size without result text");

    pixel_tui_tool_result_preview(result, PIXEL_TUI_TOOL_VIEW_FULL, preview, sizeof(preview), &tail,
                                  &bytes);
    CHECK(!strcmp(preview, result) && tail == 0 && bytes == strlen(result),
          "full view should preserve a bounded multiline result");
}

static void test_live_op_presence_motion(void) {
    /* The tool_end cadence track: snap to 1.0, ease to 0.0 over the dwell +
     * collapse window (0.55s + 0.75s). Presence itself is a pure function of
     * resolved_s at draw time; this track only holds the compositor at
     * animation cadence for the same window. */
    ui_motion_t motion;
    ui_motion_init(&motion, false);
    const uint64_t key = UINT64_C(0x54000000) + 7;
    const uint16_t presence_prop = 0;
    double now = 100.0;
    ui_motion_snap(&motion, key, presence_prop, 1.0);
    ui_motion_set(&motion, key, presence_prop, 0.0, 0.55 + 0.75, UI_MOTION_EASE_OUT, now);
    double at_end = ui_motion_value(&motion, key, presence_prop, now, 0.0);
    double mid = ui_motion_value(&motion, key, presence_prop, now + 0.6, 0.0);
    double settled = ui_motion_value(&motion, key, presence_prop, now + 1.5, 0.0);
    CHECK(at_end > 0.98, "cadence track should be fully live the instant the tool ends (%.3f)",
          at_end);
    CHECK(mid > 0.02 && mid < 0.98, "cadence track should be mid-flight at 0.6s (%.3f)", mid);
    CHECK(settled < 0.02, "cadence track should settle shortly after the collapse (%.3f)", settled);
    CHECK(ui_motion_active(&motion, now + 0.9),
          "compositor must stay at animation cadence through the collapse");
}

static void test_live_op_deck_density_and_pixels(void) {
    bool keep = getenv("DSCO_KEEP_NATIVE_ARTIFACTS") != NULL;
    enum { MSGS = 40 };
    static char bodies[MSGS][96];
    static pixel_tui_fixture_message_t messages[MSGS];
    for (int i = 0; i < MSGS; i++) {
        snprintf(bodies[i], sizeof(bodies[i]),
                 "transcript line %02d probing wrapped capacity under the live deck", i);
        messages[i] = (pixel_tui_fixture_message_t){.role = (i % 2) ? "ASSISTANT" : "USER",
                                                    .text = bodies[i]};
    }
    static const pixel_tui_fixture_tool_t running2[] = {
        {.name = "sandbox_run", .preview = "make -j8 test", .status = 0, .elapsed_s = 3.2},
        {.name = "read_file", .preview = "src/pixel_tui.c", .status = 0, .elapsed_s = 0.7},
    };
    static const pixel_tui_fixture_tool_t running5[] = {
        {.name = "sandbox_run", .preview = "make test", .status = 0, .elapsed_s = 5.0},
        {.name = "read_file", .preview = "src/a.c", .status = 0, .elapsed_s = 4.0},
        {.name = "grep_files", .preview = "MOTION_KEY", .status = 0, .elapsed_s = 3.0},
        {.name = "web_fetch", .preview = "https://kitty", .status = 0, .elapsed_s = 2.0},
        {.name = "write_file", .preview = "src/b.c", .status = 0, .elapsed_s = 1.0},
    };
    pixel_tui_fixture_t fixture = {
        .model = "openai-codex/gpt-5.6-luna",
        .slot_name = "native",
        .state = PIXEL_TUI_EXECUTING,
        .messages = messages,
        .message_count = MSGS,
        .turn = 4,
        .tools_used = 2,
    };
    char path[6][192];
    for (int i = 0; i < 6; i++)
        snprintf(path[i], sizeof(path[i]), "/tmp/dsco-live-ops-%ld-%d.ppm", (long)getpid(), i);

    pixel_tui_density_metrics_t base = {0}, live = {0}, many = {0}, done = {0};
    CHECK(pixel_tui_write_fixture_ppm(path[0], 1120, 700, &fixture, &base),
          "deep fixture without tools should render");
    fixture.tools = running2;
    fixture.tool_count = 2;
    CHECK(pixel_tui_write_fixture_ppm(path[1], 1120, 700, &fixture, &live),
          "deep fixture with 2 running tools should render");
    CHECK(live.visible_lines < base.visible_lines,
          "running ops must reserve the transcript tail (%d -> %d)", base.visible_lines,
          live.visible_lines);
    CHECK(live.line_capacity < base.line_capacity, "deck must shrink line capacity (%d -> %d)",
          base.line_capacity, live.line_capacity);

    fixture.tools = running5;
    fixture.tool_count = 5;
    CHECK(pixel_tui_write_fixture_ppm(path[2], 1120, 700, &fixture, &many),
          "overflowing fixture with 5 running tools should render");
    CHECK(many.visible_lines >= 3, "deck height clamp must preserve transcript lines (visible=%d)",
          many.visible_lines);
    CHECK(many.visible_lines <= live.visible_lines,
          "overflow chip should cost at least as much as two cards (%d vs %d)", many.visible_lines,
          live.visible_lines);

    /* Finished tools carry zero presence in reduced-motion fixtures: the
     * deck releases the tail and the durable TOOL row owns the story. */
    static const pixel_tui_fixture_tool_t done1[] = {
        {.name = "sandbox_run", .preview = "make test", .status = 1, .elapsed_s = 2.5},
    };
    fixture.tools = done1;
    fixture.tool_count = 1;
    CHECK(pixel_tui_write_fixture_ppm(path[3], 1120, 700, &fixture, &done),
          "fixture with a finished tool should render");
    CHECK(done.visible_lines == base.visible_lines,
          "finished cards must release the transcript tail (%d vs %d)", done.visible_lines,
          base.visible_lines);

    /* Pixel proof: with an empty transcript, saturated warm (amber rail:
     * sandbox_run) and cool (cyan rail: read_file) channels in the deck band
     * can only come from live-op cards. */
    fixture.messages = NULL;
    fixture.message_count = 0;
    fixture.tools = NULL;
    fixture.tool_count = 0;
    CHECK(pixel_tui_write_fixture_ppm(path[4], 1120, 700, &fixture, NULL),
          "empty fixture should render");
    fixture.tools = running2;
    fixture.tool_count = 2;
    CHECK(pixel_tui_write_fixture_ppm(path[5], 1120, 700, &fixture, NULL),
          "empty fixture with running tools should render");
    int w = 0, h = 0, w2 = 0, h2 = 0;
    unsigned char *empty_px = ppm_load(path[4], &w, &h);
    unsigned char *cards_px = ppm_load(path[5], &w2, &h2);
    CHECK(empty_px && cards_px && w == 1120 && h == 700 && w2 == w && h2 == h,
          "band fixtures should load as full RGB rasters");
    if (empty_px && cards_px) {
        int base_warm = 0, base_cool = 0, warm = 0, cool = 0;
        band_saturation(empty_px, w, h, 40, 150, 700, 600, &base_warm, &base_cool);
        band_saturation(cards_px, w, h, 40, 150, 700, 600, &warm, &cool);
        CHECK(warm - base_warm >= 30,
              "amber accent channel should land in the deck band (%d -> %d)", base_warm, warm);
        CHECK(cool - base_cool >= 30, "cyan accent channel should land in the deck band (%d -> %d)",
              base_cool, cool);
    }
    free(empty_px);
    free(cards_px);
    for (int i = 0; i < 6; i++)
        unlink(path[i]);

    if (keep) {
        /* Stable design artifact: a populated session with three live ops. */
        static const pixel_tui_fixture_message_t artifact_messages[] = {
            {.role = "USER", .text = "Profile the compositor and patch the hot path."},
            {.role = "THINKING", .text = "Need the frame timings first, then the diff bounds."},
            {.role = "ASSISTANT",
             .text = "Running the perf capture now; patching the damage clamp while the "
                     "bench streams so both land in one turn."},
            {.role = "TOOL",
             .detail = "read_file src/pixel_tui.c",
             .text = "4,096 lines / retained-mode session renderer"},
            {.role = "ASSISTANT",
             .text = "The patcher already ships bounded rects; extending the clamp to the "
                     "deck band next."},
        };
        static const pixel_tui_fixture_tool_t artifact_tools[] = {
            {.name = "sandbox_run",
             .preview = "make bench-compositor -j8",
             .status = 0,
             .elapsed_s = 4.6},
            {.name = "grep_files",
             .preview = "session_animation_fast",
             .status = 0,
             .elapsed_s = 1.8},
            {.name = "web_fetch",
             .preview = "https://sw.kovidgoyal.net/kitty/graphics-protocol",
             .status = 0,
             .elapsed_s = 0.4},
        };
        pixel_tui_fixture_t artifact = {
            .model = "openai-codex/gpt-5.6-luna",
            .slot_name = "native",
            .state = PIXEL_TUI_EXECUTING,
            .messages = artifact_messages,
            .message_count = 5,
            .input = "keep the patch rects bounded",
            .input_cursor = 12,
            .input_active = true,
            .turn = 7,
            .input_tokens = 3200,
            .output_tokens = 940,
            .tools_used = 11,
            .cost_usd = 0.42,
            .context_percent = 37.0,
            .tools = artifact_tools,
            .tool_count = 3,
        };
        CHECK(pixel_tui_write_fixture_ppm("/tmp/dsco-live-ops-1120x700.ppm", 1120, 700, &artifact,
                                          NULL),
              "live-ops design artifact should render");
    }
}

static void test_capture_parser_overwrite_semantics(void) {
    capture_sink_t sink;

    /* Async spinner: each frame is ESC[2K CR <row> with no newline; stop
     * erases the frame and prints a durable completion line. Only the
     * completion line may reach the transcript. */
    capture_run("\033[2K\r  \033[1mGrep\033[0m... 1.2s"
                "\033[2K\r  \033[1mGrep\033[0m... 1.3s"
                "\033[2K\r\n  ▌ tool_response Grep · 1.4s\n",
                &sink);
    CHECK(sink.count == 1, "spinner frames must not publish (got %d lines)", sink.count);
    CHECK(sink.count == 1 && strstr(sink.lines[0], "tool_response Grep"),
          "completion line should survive spinner suppression");

    /* Batch spinner: cursor-up then per-row ESC[2K CR <row> LF, repeated
     * every frame; the durable summary follows on a plain line. */
    capture_run("\033[2A"
                "\033[2K\r| bash $ grep foo (14.7s)\n"
                "\033[2K\r| bash $ printf bar (14.7s)\n"
                "\033[2A"
                "\033[2K\r/ bash $ grep foo (14.8s)\n"
                "\033[2K\r/ bash $ printf bar (14.8s)\n"
                "  ✓ 2 tools (42ms avg, 120ms max)\n",
                &sink);
    CHECK(sink.count == 1, "batch repaint frames must not publish (got %d lines)", sink.count);
    CHECK(sink.count == 1 && strstr(sink.lines[0], "2 tools"),
          "batch summary should survive repaint suppression");

    /* CRLF endings are plain line terminators, not overwrites. */
    capture_run("hello\r\nworld\r\n", &sink);
    CHECK(sink.count == 2 && !strcmp(sink.lines[0], "hello") && !strcmp(sink.lines[1], "world"),
          "CRLF lines should publish normally (got %d)", sink.count);

    /* Bare-CR progress collapses to its final newline-terminated state. */
    capture_run("10%\r50%\r100% done\n", &sink);
    CHECK(sink.count == 1 && !strcmp(sink.lines[0], "100% done"),
          "CR progress should publish only the final state (got %d: '%s')", sink.count,
          sink.count ? sink.lines[0] : "");

    /* SGR color has no overwrite meaning and must not suppress content. */
    capture_run("\033[1mbold\033[0m line\n", &sink);
    CHECK(sink.count == 1 && !strcmp(sink.lines[0], "bold line"),
          "SGR-styled lines should publish");

    /* An unterminated durable tail publishes on finish. */
    capture_run("no newline at end", &sink);
    CHECK(sink.count == 1 && !strcmp(sink.lines[0], "no newline at end"),
          "durable tail should flush at end of stream");
}

static size_t rich_emphasis_tokens(const rich_token_t *tokens, size_t count, char *joined,
                                   size_t cap) {
    size_t emphasized = 0;
    if (joined && cap)
        joined[0] = '\0';
    for (size_t i = 0; i < count; i++) {
        if (tokens[i].type != RICH_TOKEN_TEXT)
            continue;
        if (tokens[i].style == RICH_STYLE_EMPHASIS)
            emphasized++;
        if (joined) {
            size_t used = strlen(joined);
            snprintf(joined + used, cap - used, "%s", tokens[i].text);
        }
    }
    return emphasized;
}

static void test_rich_text_math_semantics(void) {
    rich_token_t tokens[128];
    size_t n =
        rich_text_parse("Euler: $e^{i\\pi}+1=0$\n$$\\frac{a+b}{c}=\\sqrt{d}\n$$\n", tokens, 128);
    bool inline_ok = false, display_ok = false, raw_delimiter = false;
    for (size_t i = 0; i < n; i++) {
        if (tokens[i].type != RICH_TOKEN_TEXT)
            continue;
        if (strstr(tokens[i].text, "$$"))
            raw_delimiter = true;
        if (tokens[i].style == RICH_STYLE_MATH && strstr(tokens[i].text, "e") &&
            strstr(tokens[i].text, "π"))
            inline_ok = true;
        if (tokens[i].style == RICH_STYLE_MATH_DISPLAY && strstr(tokens[i].text, "(a+b)⁄(c)=√(d)"))
            display_ok = true;
    }
    CHECK(inline_ok, "inline LaTeX should become typographic math tokens");
    CHECK(display_ok, "multiline $$ blocks should emit display math content");
    CHECK(!raw_delimiter, "display delimiters must never leak into transcript tokens");

    n = rich_text_parse("\\[\\mathbb{R} \\to \mathbb{R}\\]", tokens, 128);
    bool bracket_display = false;
    for (size_t i = 0; i < n; i++)
        if (tokens[i].type == RICH_TOKEN_TEXT && tokens[i].style == RICH_STYLE_MATH_DISPLAY &&
            strstr(tokens[i].text, "ℝ") && strstr(tokens[i].text, "→"))
            bracket_display = true;
    CHECK(bracket_display, "\\[...\\] should render as a display equation");
}

static void test_rich_text_identifier_and_list_semantics(void) {
    rich_token_t tokens[256];
    char joined[512];

    /* Intra-word underscores are identifiers, not emphasis. */
    size_t n = rich_text_parse("call tui_composer_set_slash_commands now", tokens, 256);
    size_t emphasized = rich_emphasis_tokens(tokens, n, joined, sizeof(joined));
    CHECK(emphasized == 0 && strstr(joined, "tui_composer_set_slash_commands"),
          "snake_case identifiers should render literally (%zu emphasis runs)", emphasized);

    /* Real emphasis still parses. */
    n = rich_text_parse("this is *important* to know", tokens, 256);
    bool found = false;
    for (size_t i = 0; i < n; i++)
        if (tokens[i].type == RICH_TOKEN_TEXT && tokens[i].style == RICH_STYLE_EMPHASIS &&
            !strcmp(tokens[i].text, "important"))
            found = true;
    CHECK(found, "*word* should still emit an emphasis run");

    /* Regression: transcript tokens with underscores must survive verbatim and
     * never open an italic run (the "markdown clobbers tool_response" bug).
     * Each case pairs an all-literal expectation with a zero-emphasis check. */
    static const char *literal_underscore_cases[] = {
        "TOOL  tool_response ok",
        "src/provider.c codex_exec_build_request / codex_exec_stream",
        "s_slash_commands stays literal",
        "DSCO_TOOL_TIMEOUT_bash and MAX_TOOL_RESULT",
        "/tmp/dsco_resize_test/zoom_burst_009.png",
        "read a_b then c_d then e_f end",
        "one_two three_four five_six",
        "_internal and __dunder__ names survive",
    };
    for (size_t c = 0; c < sizeof(literal_underscore_cases) / sizeof(literal_underscore_cases[0]);
         c++) {
        const char *in = literal_underscore_cases[c];
        n = rich_text_parse(in, tokens, 256);
        emphasized = rich_emphasis_tokens(tokens, n, joined, sizeof(joined));
        CHECK(emphasized == 0 && strstr(joined, in),
              "underscore identifiers must render literally: '%s' "
              "(%zu emphasis runs, joined='%s')",
              in, emphasized, joined);
    }

    /* A single underscore-flanked word between spaces IS still emphasis, so the
     * flanking rule does not over-correct and kill legitimate _italic_. */
    n = rich_text_parse("an _emphasized_ word", tokens, 256);
    found = false;
    for (size_t i = 0; i < n; i++)
        if (tokens[i].type == RICH_TOKEN_TEXT && tokens[i].style == RICH_STYLE_EMPHASIS &&
            !strcmp(tokens[i].text, "emphasized"))
            found = true;
    CHECK(found, "_word_ between spaces should still emphasize");

    /* Mixed line: identifier survives, real emphasis still fires. */
    n = rich_text_parse("call foo_bar and *see* baz_qux now", tokens, 256);
    emphasized = rich_emphasis_tokens(tokens, n, joined, sizeof(joined));
    CHECK(emphasized == 1 && strstr(joined, "foo_bar") && strstr(joined, "baz_qux"),
          "mixed identifiers+emphasis: idents literal, *see* emphasized "
          "(%zu emphasis runs, joined='%s')",
          emphasized, joined);

    /* List marker and item text share one visual line: the marker starts the
     * block and the item body must not open a second block. */
    n = rich_text_parse("- alpha beta\n- gamma\n", tokens, 256);
    int markers = 0, stray_blocks = 0;
    for (size_t i = 0; i < n; i++) {
        if (tokens[i].type != RICH_TOKEN_TEXT)
            continue;
        if (tokens[i].style == RICH_STYLE_LIST_MARKER)
            markers++;
        else if (tokens[i].block_start)
            stray_blocks++;
    }
    CHECK(markers == 2 && stray_blocks == 0,
          "list items should not split marker and text (%d markers, %d strays)", markers,
          stray_blocks);
}

int main(void) {
    test_masthead_semantics_and_layout();
    test_masthead_density_and_damage();
    test_composer_semantics_and_damage();
    test_pixel_backend_contract();
    test_headless_session_artifacts();
    test_populated_fixture_density();
    test_long_stream_fixture_retention();
    test_frame_telemetry_aggregation();
    test_dual_compositor_parity_corpus();
    test_rich_text_identifier_and_list_semantics();
    test_rich_text_math_semantics();
    test_tool_preview_extraction();
    test_tool_result_views();
    test_live_op_presence_motion();
    test_live_op_deck_density_and_pixels();
    test_capture_parser_overwrite_semantics();
    printf("native compositor: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
