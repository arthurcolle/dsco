/* Headless regression for the crowded native compositor screenshot. */
#define main transcript_review_main
#include "test_native_transcript_review.c"
#undef main
#include "tui.h"

static void check_native_chrome(void) {
    reset_session();
    FILE *capture = tmpfile();
    CHECK(capture != NULL);
    if (!capture) return;
    fflush(stderr);
    int saved = dup(STDERR_FILENO);
    CHECK(saved >= 0);
    if (saved < 0) { fclose(capture); return; }
    CHECK(dup2(fileno(capture), STDERR_FILENO) >= 0);
    g_session.active = true;
    atomic_store(&g_session_active_fast, true);
    tui_section_divider(20, 1, 1.59, "test", 0);
    tui_section_divider_ex(20, 1, 1, 1, 1.59, "test", 100, 25, "main");
    tui_transition_divider();
    tui_dag_t dag;
    tui_dag_init(&dag);
    tui_dag_add_node(&dag, "read_file");
    tui_dag_add_node(&dag, "write_file");
    tui_dag_add_edge(&dag, 0, 1);
    tui_dag_render(&dag);
    tui_async_spinner_t spinner;
    tui_async_spinner_start(&spinner, "invoke_tool", TUI_TOOL_OTHER);
    usleep(20000);
    tui_async_spinner_stop(&spinner, true, "worker result", 16, NULL);
    const char *names[] = {"read_file", "write_file"};
    tui_batch_spinner_t batch;
    tui_batch_spinner_start(&batch, names, 2);
    tui_batch_spinner_complete(&batch, 0, true, "read complete", 16);
    tui_batch_spinner_complete(&batch, 1, false, "write failed", 20);
    usleep(20000);
    tui_batch_spinner_stop(&batch);
    tui_batch_summary(&batch, "[in:10 out:20]");
    fflush(stderr);
    long native_bytes = ftell(capture);
    g_session.active = false;
    atomic_store(&g_session_active_fast, false);
    tui_section_divider(20, 1, 1.59, "test", 0);
    fflush(stderr);
    long terminal_bytes = ftell(capture);
    CHECK(dup2(saved, STDERR_FILENO) >= 0);
    close(saved);
    fclose(capture);
    CHECK(native_bytes == 0);
    CHECK(terminal_bytes > native_bytes);
}

static void render_busy(int width, int height, int scale) {
    reset_session();
    g_session.width = width;
    g_session.height = height;
    g_session.backing_scale = scale;
    g_session.surface_width = width * scale;
    g_session.surface_height = height * scale;
    g_session.cols = width / 8;
    g_session.rows = height / 16;
    g_session.state = PIXEL_TUI_EXECUTING;
    g_session.input_active = true;
    ui_motion_init(&g_session.motion, true);
    pixel_message_t *m = session_new_message("USER", "");
    CHECK(m && message_text_set_plain(m, "Review the native compositor and fix the rendering issues."));
    m = session_new_message("ASSISTANT", "");
    CHECK(m && message_text_set_plain(m,
        "The worker is checking **transcript rendering**. Full diagnostics remain in the worker log; "
        "conversation and tool results stay here."));
    const char *names[] = {"read_file", "bash", "write_file"};
    const char *results[] = {
        "src/font_compat.c · native font selection",
        "Typography and viewport checks passed. No desktop windows opened.",
        "Updated the compositor. Running focused regression checks."
    };
    for (int i = 0; i < 3; ++i) {
        m = session_new_message("TOOL", "");
        CHECK(m != NULL);
        if (!m) continue;
        m->tool_row = true;
        m->tool_status = 1;
        m->tool_elapsed_ms = 16 + 200 * i;
        snprintf(m->tool_name, sizeof(m->tool_name), "%s", names[i]);
        CHECK(message_text_set_plain(m, results[i]));
    }
    m = session_new_message("ASSISTANT", "");
    CHECK(m && message_text_set_plain(m,
        "## Rendering checks\n\n"
        "- Normal-width type with room between lines.\n"
        "- Worker telemetry does not become assistant dialogue.\n"
        "- Terminal status dividers do not cross the native canvas.\n\n"
        "Existing drafts and background work remain untouched."));
    px_canvas_t *canvas = render_session_frame(width, height, scale, width * scale,
                                               height * scale, "gpt-6-astra", PIXEL_TUI_EXECUTING);
    CHECK(canvas != NULL);
    if (canvas) {
        const char *dir = getenv("DSCO_NATIVE_REVIEW_DIR");
        if (dir && *dir) {
            char path[1024];
            snprintf(path, sizeof(path), "%s/busy-%dx%d-%dx.ppm", dir, width, height, scale);
            CHECK(canvas_write_ppm(path, canvas));
            printf("native readability artifact: %s\n", path);
        }
        free_canvas(canvas);
    }
}

int main(void) {
    unsetenv("DSCO_PIXEL_FONT");
    unsetenv("DSCO_PIXEL_TUI_ZOOM");
    check_native_chrome();
#ifdef __APPLE__
    /* A normal-width 14pt monospace face, not the previous condensed default. */
    CHECK(font_compat_measure_utf8("MMMMMMMMMM", 14.0f, false) >= 80);
#endif
    CHECK(session_transcript_line_height() > font_compat_line_height(SESSION_TRANSCRIPT_BODY_SIZE, false));
    CHECK(session_transcript_line_height() > font_compat_prose_line_height(14.0f, false));
    render_busy(1080, 920, 1);
    render_busy(540, 460, 2);
    render_busy(800, 620, 1);
    reset_session();
    printf("native readability: %d failures\n", failures);
    return failures != 0;
}
