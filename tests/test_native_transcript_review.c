/* Screenshot regression: exact Markdown spacing and native viewport rasters.
 * No window, provider request, or terminal image protocol is involved. */
#define _DARWIN_C_SOURCE
#include "../src/pixel_tui.c"
#include "vm.h"
#include "critical_stamp.h"
int g_cheap_mode;
vm_t g_vm;
volatile int g_interrupted;
double g_cost_budget;
static int failures;
#define CHECK(x) do { if (!(x)) { ++failures; fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); } } while (0)

static const char *weather =
    "Washington, DC: **84°F (29°C)**, **mostly cloudy**, feeling like **91°F (33°C)**. "
    "Humidity is **74%**, with a south wind around **7 mph**.";

static void reset_session(void) {
    session_messages_free();
    free(g_session.activity_underlay);
    g_session = (pixel_session_t){.current_message=-1};
    visual_cache_invalidate();
}

static void check_text_at_width(const char *markdown, const char *expected, bool code_only,
                                int columns) {
    reset_session();
    pixel_message_t *m = session_new_message("ASSISTANT", "");
    CHECK(m != NULL);
    if (!m) return;
    CHECK(message_text_set_plain(m, markdown));
    pixel_visual_line_t lines[16] = {0};
    int count = wrap_rich_message(m, columns, lines, 0, 16);
    char actual[2048] = {0};
    size_t used = 0;
    for (int i = 0; i < count; ++i) {
        if (visual_line_empty(&lines[i])) continue;
        if (code_only && lines[i].block_style != RICH_STYLE_CODE) continue;
        if (used) actual[used++] = '\n';
        /* Code indentation is retained as layout metadata by rich_text_parse. */
        if (code_only)
            for (int spaces = 0; spaces < lines[i].indent * 2; ++spaces)
                actual[used++] = ' ';
        for (int r = 0; r < lines[i].run_count; ++r) {
            size_t len = strlen(lines[i].runs[r].text);
            CHECK(used + len < sizeof(actual));
            if (used + len >= sizeof(actual)) break;
            memcpy(actual + used, lines[i].runs[r].text, len);
            used += len;
        }
        actual[used] = '\0';
    }
    if (strcmp(actual, expected))
        fprintf(stderr, "expected: [%s]\nactual:   [%s]\n", expected, actual);
    CHECK(strcmp(actual, expected) == 0);
}

static void check_text(const char *markdown, const char *expected, bool code_only) {
    check_text_at_width(markdown, expected, code_only, 512);
}

static void check_source_whitespace(void) {
    reset_session();
    pixel_message_t *m = session_new_message("ASSISTANT", "");
    const char *source = "```python\n\tif ready:\n\t\treturn 1\n\n  # keep this blank line and indent\n```";
    const char *expanded = "```python\n    if ready:\n        return 1\n\n  # keep this blank line and indent\n```";
    CHECK(m && message_text_set_plain(m, source));
    if (!m) return;
    CHECK(!strcmp(message_text(m), expanded));
    check_text(expanded, "    if ready:\n        return 1\n  # keep this blank line and indent", true);
    reset_session();
}

static void check_pixel_wrapping(const char *markdown, const char *expected, int max_px) {
    reset_session();
    pixel_message_t *m = session_new_message("ASSISTANT", "");
    CHECK(m && message_text_set_plain(m, markdown));
    if (!m) return;
    pixel_visual_line_t lines[128] = {0};
    int count = wrap_rich_message_width(m, 80, max_px, lines, 0, 128);
    char actual[4096] = {0};
    size_t used = 0;
    CHECK(count > 1 && count < 128);
    for (int i = 0; i < count; ++i) {
        int width = lines[i].indent * 10;
        for (int j = 0; j < lines[i].run_count; ++j) {
            const pixel_visual_run_t *run = &lines[i].runs[j];
            width += measure_rich_run(run, lines[i].tool_row);
            size_t bytes = strlen(run->text);
            CHECK(used + bytes < sizeof(actual));
            if (used + bytes >= sizeof(actual)) continue;
            memcpy(actual + used, run->text, bytes);
            used += bytes;
            actual[used] = '\0';
        }
        if (width > max_px + 4)
            fprintf(stderr, "pixel wrap row %d: width %d exceeds %d\n", i, width, max_px);
        CHECK(width <= max_px + 4);
    }
    /* These spans have no source spaces at soft-wrap points, so joining
     * lines verifies every UTF-8 byte and punctuation mark without ambiguity. */
    if (strcmp(actual, expected))
        fprintf(stderr, "pixel wrap expected [%s], got [%s]\n", expected, actual);
    CHECK(strcmp(actual, expected) == 0);
}

static void check_rect(native_ui_rect_t r, int width, int height) {
    CHECK(r.x >= 0 && r.y >= 0 && r.width >= 0 && r.height >= 0);
    CHECK(r.x + r.width <= width && r.y + r.height <= height);
}

static void check_startup_card_gutter(void) {
    /* Screenshot-sized SYSTEM line followed by the multiline SESSION card.
     * Exercise the real compositor; clipping used to happen at the 54px role
     * span even with over a thousand pixels of available transcript width. */
    reset_session();
    g_session.width = 1780;
    g_session.height = 847;
    g_session.backing_scale = 1;
    g_session.surface_width = 1780;
    g_session.surface_height = 847;
    g_session.state = PIXEL_TUI_IDLE;
    ui_motion_init(&g_session.motion, true);
    pixel_message_t *m = session_new_message("SYSTEM", "");
    CHECK(m && message_text_set_plain(m,
        "NATIVE KITTY WORKSPACE ONLINE / gpt-6-sol / 320 TOOLS / TYPE A MESSAGE OR /HELP"));
    m = session_new_message("SESSION", "");
    CHECK(m && message_text_set_plain(m,
        "Features  18/40\nTrust  trusted\nBranch  perf/workspace-catalog-chronicle-index\nBudget  $229.73 today / $25.00 session / $100.00 daily"));
    px_canvas_t *canvas = render_session_frame(1780, 847, 1, 1780, 847, "gpt-6-sol", PIXEL_TUI_IDLE);
    CHECK(canvas && s_sel_geom.valid);
    const char *review_path = getenv("DSCO_NATIVE_STARTUP_REVIEW");
    if (canvas && review_path && *review_path) CHECK(canvas_write_ppm(review_path, canvas));
    if (canvas && s_sel_geom.valid) {
        int session_width = font_compat_measure_utf8("SESSION", 11.0f, true);
        int system_width = font_compat_measure_utf8("SYSTEM", 11.0f, true);
        int max_role = session_width > system_width ? session_width : system_width;
        CHECK(s_sel_geom.role_w >= max_role + 20);
        /* Both the role glyphs and the SESSION card's left edge fit on the
         * same row without hiding the final M or N. */
        CHECK(7 + max_role < s_sel_geom.role_w - 5);
        CHECK(s_sel_geom.x + s_sel_geom.role_w < canvas->width);
    }
    free_canvas(canvas);
    /* The reported crop uses enlarged text: recheck the same physical
     * screenshot size at 2x logical zoom, where a 54px gutter was worst. */
    g_session.width = 890;
    g_session.height = 424;
    g_session.backing_scale = 2;
    g_session.surface_width = 1780;
    g_session.surface_height = 848;
    canvas = render_session_frame(890, 424, 2, 1780, 848, "gpt-6-sol", PIXEL_TUI_IDLE);
    CHECK(canvas && s_sel_geom.valid);
    if (canvas && s_sel_geom.valid) {
        int session_width = font_compat_measure_utf8("SESSION", 11.0f, true);
        int system_width = font_compat_measure_utf8("SYSTEM", 11.0f, true);
        int max_role = session_width > system_width ? session_width : system_width;
        CHECK(s_sel_geom.role_w >= max_role + 20);
        CHECK(7 + max_role < s_sel_geom.role_w - 5);
    }
    if (canvas && review_path && *review_path) {
        char scaled_path[1024];
        if (snprintf(scaled_path, sizeof(scaled_path), "%s.2x.ppm", review_path) < (int)sizeof(scaled_path))
            CHECK(canvas_write_ppm(scaled_path, canvas));
    }
    free_canvas(canvas);
    reset_session();
}

static void render_review(int width, int height, int scale) {
    reset_session();
    g_session.width = width;
    g_session.height = height;
    g_session.backing_scale = scale;
    g_session.surface_width = width * scale;
    g_session.surface_height = height * scale;
    g_session.cols = width / 8;
    g_session.rows = height / 16;
    g_session.state = PIXEL_TUI_IDLE;
    g_session.input_active = true;
    ui_motion_init(&g_session.motion, true);
    pixel_message_t *m = session_new_message("SESSION", "");
    CHECK(m && message_text_set_plain(m, "Features  18/40\nTrust  trusted\nBudget  $25.00 session"));
    m = session_new_message("USER", "");
    CHECK(m && message_text_set_plain(m, "What's the weather in Washington, DC?"));
    if (m) m->streaming = false;
    m = session_new_message("ASSISTANT", "");
    CHECK(m && message_text_set_plain(m, weather));
    if (m) m->streaming = false;
    m = session_new_message("ASSISTANT", "");
    CHECK(m && message_text_set_plain(m,
        "| Condition | Value |\n|---|---|\n| Temperature | 17°C |\n| Wind | NNW 21 mph |"));
    native_ui_agent_shell_layout_t shell = native_ui_agent_shell_layout(width, height);
    check_rect(shell.header, width, height);
    check_rect(shell.transcript, width, height);
    check_rect(shell.composer, width, height);
    CHECK(shell.header.y + shell.header.height <= shell.transcript.y);
    CHECK(shell.transcript.y + shell.transcript.height <= shell.composer.y);
    CHECK(shell.transcript.height >= height / 2);
    px_canvas_t *canvas = render_session_frame(width, height, scale, width * scale,
                                              height * scale, "openai-codex/gpt-5.4", PIXEL_TUI_IDLE);
    CHECK(canvas != NULL);
    if (canvas) {
        CHECK(s_sel_geom.valid && s_sel_geom.x == shell.reading_column.x &&
              s_sel_geom.w == shell.reading_column.width);
        CHECK(canvas->pixel_width == width * scale);
        CHECK(canvas->pixel_height == height * scale);
        if (getenv("DSCO_KEEP_NATIVE_ARTIFACTS")) {
            char path[160];
            snprintf(path, sizeof(path), "/tmp/dsco-native-transcript-%dx%d-%dx.ppm", width, height, scale);
            CHECK(canvas_write_ppm(path, canvas));
            printf("native review artifact: %s\n", path);
        }
        free_canvas(canvas);
    }
}

static pixel_message_t *coding_message(const char *role, const char *text) {
    pixel_message_t *m = session_new_message(role, "");
    CHECK(m && message_text_set_plain(m, text));
    if (m) m->streaming = false;
    return m;
}

static void coding_tool(const char *command, const char *result, bool ok, float elapsed_ms) {
    pixel_message_t *m = coding_message("TOOL", result);
    if (!m) return;
    m->tool_row = true;
    m->tool_status = ok ? 1 : 0;
    m->tool_elapsed_ms = elapsed_ms;
    m->tool_total_bytes = (uint32_t)strlen(result);
    snprintf(m->tool_name, sizeof(m->tool_name), "bash");
    snprintf(m->detail, sizeof(m->detail), "%s", command);
}

/* Actual user display preference: 1920x1080 physical, zoom 0.65, DPR 2.
 * This stable coding conversation exercises prose, Markdown, commands,
 * traceback indentation, tool status, and the follow-up composer together. */
static void render_coding_review(void) {
    reset_session();
    const double scale = 0.65 * 2.0;
    const int width = (int)floor(1920 / scale), height = (int)floor(1080 / scale);
    g_session.width = width;
    g_session.height = height;
    g_session.backing_scale = scale;
    g_session.surface_width = 1920;
    g_session.surface_height = 1080;
    g_session.cols = 192;
    g_session.rows = 54;
    g_session.turn = 15;
    g_session.started_s = monotonic_s() - 37;
    g_session.state = getenv("DSCO_NATIVE_BEAUTY_SIDEBAR") ? PIXEL_TUI_EXECUTING : PIXEL_TUI_RESPONDING;
    g_session.input_active = true;
    ui_motion_init(&g_session.motion, true);
    snprintf(g_session.input, sizeof(g_session.input), "Run the concurrency tests again, then show me the diff.");
    g_session.input_cursor = strlen(g_session.input);
    coding_message("USER", "Fix the duplicate completion event in the lineage store. Preserve the existing lease and retry behavior, and verify the real test suite.");
    coding_message("ASSISTANT", "The failure is in the completion transaction: two workers can acknowledge the same lease before its final state is recorded. I’m checking that path and adding coverage for a repeated completion.");
    coding_tool("python3 -m unittest discover -s tests -v",
        "test_complete_is_atomic (test_store.StoreTests) ... ok\n"
        "test_duplicate_completion_is_idempotent (test_store.StoreTests) ... FAIL\n"
        "\nFAIL: test_duplicate_completion_is_idempotent\n"
        "Traceback (most recent call last):\n"
        "  File \"tests/test_store.py\", line 184, in test_duplicate_completion_is_idempotent\n"
        "    self.assertEqual(len(events), 1)\n"
        "AssertionError: 2 != 1\n"
        "\nRan 29 tests in 0.428s\nFAILED (failures=1)", false, 428);
    coding_message("ASSISTANT", "The repeated call currently appends a second event. I’m moving the **completed-state check inside the transaction**, before any event is written. A retry will return the original result.");
    coding_tool("apply_patch lineage/store.py tests/test_store.py",
        "Updated lineage/store.py and tests/test_store.py.\n"
        "Added regression coverage for duplicate completion and concurrent senders.", true, 8);
    coding_message("ASSISTANT", "The completion path now returns the stored receipt on a retry:\n\n"
        "```python\n"
        "if lease.status == \"completed\":\n"
        "    return lease.receipt\n"
        "```\n\n"
        "Both the state check and event write remain in the same transaction.");
    coding_tool("python3 -m unittest discover -s tests -v",
        "test_duplicate_completion_is_idempotent ... ok\n"
        "test_concurrent_senders_deduplicate ... ok\n"
        "Ran 31 tests in 0.461s — OK", true, 461);
    pixel_message_t *last = coding_message("ASSISTANT", "**All 31 tests pass.** Duplicate completions now return the existing receipt without adding another event. Lease renewal, expiry, and retry behavior remain covered.");
    if (last) last->streaming = true;
    px_canvas_t *canvas = render_session_frame(width, height, scale, 1920, 1080,
                                              "gpt-6-astra", g_session.state);
    CHECK(canvas != NULL);
    if (canvas) {
        CHECK(canvas->pixel_width == 1920 && canvas->pixel_height == 1080);
        const char *path = getenv("DSCO_NATIVE_BEAUTY_ARTIFACT");
        if (path && *path) {
            CHECK(canvas_write_ppm(path, canvas));
            printf("native coding artifact: %s\n", path);
        }
        free_canvas(canvas);
    }
}

static void check_presentation_features(void) {
    reset_session();
    pixel_message_t *m = coding_message("ASSISTANT", "| Name | Status |\n| --- | --- |\n| One | Ready |\n");
    pixel_visual_line_t lines[64] = {0};
    int count = wrap_rich_message(m, 80, lines, 0, 64);
    bool card = false;
    for (int i = 0; i < count; i++) if (lines[i].block_style == RICH_STYLE_TABLE) card = true;
    CHECK(card);
    check_text("```c\n   odd_indent();\n```", "   odd_indent();", true);
    m = coding_message("TOOL", "compact");
    m->tool_row = true; m->tool_status = 1;
    snprintf(m->tool_name, sizeof(m->tool_name), "fixture");
    m->tool_expanded = strdup("--- a\n+++ b\n@@ -1 +1 @@\n-old\n+new");
    count = wrap_rich_message(m, 80, lines, 0, 64);
    CHECK(count == 2);
    g_session.transcript_expanded = true;
    count = wrap_rich_message(m, 80, lines, 0, 64);
    CHECK(count > 2);
    bool added = false, removed = false;
    for (int i = 0; i < count; i++) {
        if (lines[i].block_style == RICH_STYLE_DIFF_ADD) added = true;
        if (lines[i].block_style == RICH_STYLE_DIFF_REMOVE) removed = true;
    }
    CHECK(added && removed);
    CHECK(!strcmp(message_text(m), "compact"));
    reset_session();
}

static void check_critical_native_card(void) {
    reset_session();
    g_session.width = g_session.surface_width = 920;
    g_session.height = g_session.surface_height = 780;
    g_session.backing_scale = 1;
    px_canvas_t *before = render_session_frame(920, 780, 1, 920, 780, "fixture", g_session.state);
    CHECK(before != NULL);
    FILE *sink = tmpfile();
    CHECK(sink != NULL);
    if (!before || !sink) { free_canvas(before); if (sink) fclose(sink); return; }
    char stamp[1024];
    critical_stamp_format(stamp, sizeof(stamp), "provider rejected", "turn incomplete",
                          "inspect pending effects before retrying");
    g_session.active = true;
    pixel_tui_session_add_message(sink, "ERROR", stamp);
    CHECK(g_session.message_count == 1);
    pixel_message_t *message = &g_session.messages[g_session.message_start];
    CHECK(!strcmp(message->role, "ERROR"));
    CHECK(strstr(message_text(message), "[!! CRITICAL ERROR !!]") != NULL);
    CHECK(strstr(message_text(message), "Error: provider rejected") != NULL);
    CHECK(strstr(message_text(message), "Impact: turn incomplete") != NULL);
    CHECK(strstr(message_text(message), "Next: inspect pending effects before retrying") != NULL);
    px_canvas_t *after = render_session_frame(920, 780, 1, 920, 780, "fixture", g_session.state);
    CHECK(after != NULL);
    if (after) {
        size_t bytes = (size_t)before->pixel_width * (size_t)before->pixel_height * 4;
        CHECK(memcmp(before->pixels, after->pixels, bytes) != 0);
        free_canvas(after);
    }
    free_canvas(before);
    fclose(sink);
    reset_session();
}

static void check_native_table_grid(void) {
    reset_session();
    pixel_message_t *m = session_new_message("ASSISTANT", "");
    const char *sample = "Before\n\n| Tool | State |\n|---|---|\n| weather | ready |\n\nAfter";
    CHECK(m && message_text_set_plain(m, sample));
    if (!m) return;
    char source[256];
    snprintf(source, sizeof(source), "%s", message_text(m));
    pixel_visual_line_t lines[32] = {0};
    int count = wrap_rich_message_width(m, 80, 640, lines, 0, 32);
    CHECK(count >= 4);
    int header = -1;
    for (int i = 0; i < count; i++)
        if (lines[i].table_header) { header = i; break; }
    CHECK(header > 0 && header + 1 < count);
    if (header > 0 && header + 1 < count) {
        char raw[512];
        CHECK(lines[header].table_columns == 2 && lines[header + 1].table_columns == 2);
        sel_line_bytes(&lines[header + 1], raw, sizeof(raw));
        CHECK(!strcmp(raw, "weather | ready"));
        CHECK(!lines[count - 1].table_columns); /* prose after the grid */
        s_sel_geom = (typeof(s_sel_geom)){.valid = true, .w = 800, .role_w = 65};
        CHECK(sel_prefix_width(&lines[header + 1], 12) >
              sel_prefix_width(&lines[header + 1], 3));
    }
    count = wrap_rich_message_width(m, 50, 175, lines, 0, 32);
    for (int i = 0; i < count; i++) CHECK(!lines[i].table_columns);
    CHECK(!strcmp(message_text(m), source)); /* source stays copyable */
    reset_session();
}

int main(void) {
    check_startup_card_gutter();
    check_source_whitespace();
    check_native_table_grid();
    check_critical_native_card();
    check_presentation_features();
    check_text(weather, "Washington, DC: 84°F (29°C), mostly cloudy, feeling like 91°F (33°C). Humidity is 74%, with a south wind around 7 mph.", false);
    check_text("pre**fix**ed and `inline`, **bold**.", "prefixed and inline, bold.", false);
    check_text("**left** **right**", "left right", false);
    check_text_at_width("123 **word**.", "123\nword.", false, 8);
    check_text_at_width("a pre**fix**ed", "a\nprefixed", false, 8);
    check_text("```c\n  if (ready) {\n    run();\n  }\n```", "  if (ready) {\n    run();\n  }", true);
    const int widths[] = {96, 160, 260};
    for (size_t i = 0; i < sizeof(widths) / sizeof(widths[0]); ++i) {
        check_pixel_wrapping("WWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWW",
                             "WWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWW", widths[i]);
        check_pixel_wrapping("你好世界你好世界你好世界**你好世界**你好世界你好世界你好世界你好世界",
                             "你好世界你好世界你好世界你好世界你好世界你好世界你好世界你好世界", widths[i]);
        check_pixel_wrapping("# WWWWWWWWWWWWWW世界世界\n\nprose**WWWWWWWWWWWW**end`codeWWWWWWWW`.",
                             "WWWWWWWWWWWWWW世界世界proseWWWWWWWWWWWWendcodeWWWWWWWW.", widths[i]);
        check_pixel_wrapping("```\n    WWWWWWWWWWWWWWWWWWWWWWWW();\n    返回值返回值返回值返回值返回值();\n```",
                             "    WWWWWWWWWWWWWWWWWWWWWWWW();    返回值返回值返回值返回值返回值();", widths[i]);
    }
    render_review(920, 780, 1);
    render_review(460, 390, 2);
    render_review(1120, 700, 1);
    render_review(3440, 1374, 1);
    render_coding_review();
    reset_session();
    printf("native transcript review: %d failures\n", failures);
    return failures != 0;
}
