/* Deterministic interrupted phase transitions through the real compositor. */
#include <time.h>
static double fixed_mono = 1000.0;
static int fixture_clock_gettime(clockid_t clock, struct timespec *t) {
    if (clock == CLOCK_MONOTONIC) {
        t->tv_sec = (time_t)fixed_mono;
        t->tv_nsec = (long)((fixed_mono - t->tv_sec) * 1e9);
        return 0;
    }
    return clock_gettime(clock, t);
}
#define clock_gettime fixture_clock_gettime
#define main transcript_review_main
#include "test_native_transcript_review.c"
#undef main
#undef clock_gettime

static bool same_color(px_color_t a, px_color_t b) {
    return a.r == b.r && a.g == b.g && a.b == b.b;
}

static int first_body_glyph(const px_canvas_t *c) {
    int begin = s_sel_geom.x + s_sel_geom.role_w;
    for (int x = begin; x < begin + 80; ++x)
        for (int y = s_sel_geom.y + 4; y < s_sel_geom.y + 4 + s_sel_geom.line_h - 3; ++y) {
            px_color_t v = c->pixels[y * c->pixel_width + x];
            if (v.r > 160 && v.g > 160 && v.b > 160) return x;
        }
    return -1;
}

static void check_stable_turn_alignment(void) {
    const int widths[] = {640, 1476, 1920, 2800};
    for (size_t i = 0; i < sizeof(widths) / sizeof(widths[0]); ++i) {
        reset_session();
        g_session.state = PIXEL_TUI_IDLE;
        g_session.current_message = -1;
        ui_motion_init(&g_session.motion, false);
        pixel_message_t *m = session_new_message("ASSISTANT", "");
        CHECK(m && message_text_set_plain(m, "MMMM stable text across turns and activity states."));
        px_canvas_t *c = render_session_frame(widths[i], 830, 1, widths[i], 830,
                                              "fixture", PIXEL_TUI_IDLE);
        CHECK(c != NULL);
        if (!c) continue;
        int x = s_sel_geom.x, w = s_sel_geom.w, role_w = s_sel_geom.role_w;
        int glyph_x = first_body_glyph(c);
        CHECK(glyph_x >= 0);
        const char *artifact_dir = getenv("DSCO_ALIGNMENT_ARTIFACT_DIR");
        char path[1024];
        if (artifact_dir && (widths[i] == 640 || widths[i] == 1476)) {
            snprintf(path, sizeof(path), "%s/alignment-%d-idle.ppm", artifact_dir, widths[i]);
            CHECK(canvas_write_ppm(path, c));
        }
        free_canvas(c);
        fixed_mono += 1.0;
        for (int state = PIXEL_TUI_REASONING; state <= PIXEL_TUI_RESPONDING; ++state) {
            g_session.state = state;
            c = render_session_frame(widths[i], 830, 1, widths[i], 830, "fixture", state);
            CHECK(c != NULL);
            if (!c) continue;
            CHECK(s_sel_geom.x == x && s_sel_geom.w == w && s_sel_geom.role_w == role_w);
            CHECK(first_body_glyph(c) == glyph_x);
            if (artifact_dir && state == PIXEL_TUI_RESPONDING &&
                (widths[i] == 640 || widths[i] == 1476)) {
                snprintf(path, sizeof(path), "%s/alignment-%d-responding.ppm", artifact_dir, widths[i]);
                CHECK(canvas_write_ppm(path, c));
            }
            free_canvas(c);
        }
    }
    reset_session();
}

int main(void) {
    check_stable_turn_alignment();
    FILE *sink = tmpfile();
    CHECK(sink != NULL);
    if (!sink) return 1;
    g_session = (pixel_session_t){.active = true, .state = PIXEL_TUI_IDLE,
        .previous_state = PIXEL_TUI_IDLE, .animation_enabled = true,
        .animation_thread_started = true, .tty_out = sink, .current_message = -1};
    ui_motion_init(&g_session.motion, false);
    px_color_t before = session_animated_accent(g_session.state);
    pixel_tui_session_set_state(sink, PIXEL_TUI_REASONING);
    CHECK(same_color(before, session_animated_accent(g_session.state)));
    fixed_mono += 0.06;
    CHECK(session_transition_progress() > 0.70 && session_transition_progress() < 0.90);
    before = session_animated_accent(g_session.state);
    pixel_tui_session_set_state(sink, PIXEL_TUI_EXECUTING);
    CHECK(same_color(before, session_animated_accent(g_session.state)));
    fixed_mono += 0.04;
    before = session_animated_accent(g_session.state);
    pixel_tui_session_set_state(sink, PIXEL_TUI_RESPONDING);
    CHECK(same_color(before, session_animated_accent(g_session.state)));
    fixed_mono += 0.17;
    CHECK(session_transition_progress() == 1.0);
    CHECK(same_color(session_state_color(PIXEL_TUI_RESPONDING), session_animated_accent(g_session.state)));
    fixed_mono += 0.01;
    pixel_tui_session_set_state(sink, PIXEL_TUI_IDLE);
    fixed_mono += 0.17;
    CHECK(!session_animation_fast());
    /* Repeated HUD publications must not wake or dirty an idle compositor. */
    g_session.stream_repaint_pending = false;
    g_session.structural_repaint_pending = false;
    for (int i = 0; i < 100; ++i) {
        pixel_tui_session_set_model(sink, "", NULL);
        pixel_tui_session_set_usage(sink, -1, -1, -1.0, i, -1);
        pixel_tui_session_set_budget(sink, -1.0, -1.0, -1.0, 0.0);
        pixel_tui_session_set_clock(sink, false);
    }
    CHECK(!g_session.stream_repaint_pending && !g_session.structural_repaint_pending);
    pixel_tui_session_set_turn(sink, 1);
    CHECK(g_session.turn == 1 && g_session.turn_visual_count == 1);
    pixel_tui_session_set_usage(sink, 10, 20, 0.001, 7, 1);
    CHECK(g_session.turn == 1); /* Seven attempts still belong to user turn one. */
    pixel_tui_session_set_turn(sink, 1); /* retry: no new turn history */
    CHECK(g_session.turn_visual_count == 1);
    pixel_tui_session_set_turn(sink, 2);
    CHECK(g_session.turn == 2 && g_session.turn_visual_count == 2);

    CHECK(g_session.stream_repaint_pending && g_session.input_tokens == 10);
    g_session.animation_enabled = false;
    pixel_tui_session_set_state(sink, PIXEL_TUI_EXECUTING);
    CHECK(session_transition_progress() == 1.0);
    CHECK(same_color(session_state_color(PIXEL_TUI_EXECUTING), session_animated_accent(g_session.state)));
    CHECK(!session_animation_fast());
    fclose(sink);
    reset_session();
    printf("native phase motion: %d failures\n", failures);
    return failures != 0;
}
