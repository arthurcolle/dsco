/* Bounded real raster+Kitty encoding benchmark; terminal sink is caller-owned. */
#include <time.h>
static double fixed_mono;
static int fixture_clock_gettime(clockid_t clock, struct timespec *ts) {
    if (fixed_mono > 0 && clock == CLOCK_MONOTONIC) {
        ts->tv_sec = (time_t)fixed_mono;
        ts->tv_nsec = (long)((fixed_mono - ts->tv_sec) * 1e9);
        return 0;
    }
    return clock_gettime(clock, ts);
}
#define clock_gettime fixture_clock_gettime
#define main transcript_review_main
#include "test_native_transcript_review.c"
#undef main
#undef clock_gettime

int main(void) {
    const double scales[] = {0.65, 1.0, 1.25, 1.3, 1.5, 1.75, 2.0};
    const char *sink_path = getenv("DSCO_COMPOSER_BENCH_SINK");
    FILE *sink = sink_path ? fopen(sink_path, "w") : tmpfile();
    CHECK(sink != NULL);
    if (!sink) return 1;
    /* The production mailbox exposes a burst on its first frame even with
     * animation enabled; no simulated typing clock delays provider output. */
    g_session = (pixel_session_t){.active=true, .current_message=-1,
        .animation_enabled=true, .animation_thread_started=true};
    char burst[1200];
    memset(burst, 'a', 1100);
    strcpy(burst + 1100, "END_VISIBLE_π");
    CHECK(session_stream_mailbox_publish(burst, strlen(burst)));
    CHECK(session_drain_stream_mailbox_locked(sink));
    pixel_message_t *stream = &g_session.messages[g_session.current_message];
    CHECK(!strcmp(message_text(stream), burst));
    CHECK(stream->reveal_len == stream->text_len && !stream->reveal_pending);
    CHECK(!session_reveal_active());
    pixel_visual_line_t lines[32] = {0};
    int visible = wrap_rich_message(stream, 72, lines, 0, 32);
    CHECK(visible > 10 && strstr(lines[visible - 1].runs[0].text, "END_VISIBLE_π"));
    CHECK(session_stream_mailbox_publish("MORE_λ", strlen("MORE_λ")));
    CHECK(session_drain_stream_mailbox_locked(sink));
    CHECK(stream->reveal_len == stream->text_len && !stream->reveal_pending);
    reset_session();
    fixed_mono = 1000.8;
    g_session = (pixel_session_t){.active=true, .width=1120, .height=700,
        .backing_scale=1.3, .surface_width=1456, .surface_height=910,
        .cols=112, .rows=35, .state=PIXEL_TUI_IDLE, .current_message=-1,
        .input_active=true, .animation_enabled=true, .patch_enabled=true,
        .caret_epoch_s=1000.0, .tty_out=sink};
    ui_motion_init(&g_session.motion, false);
    strcpy(g_session.input, "word");
    g_session.input_cursor = 4;
    CHECK(!session_caret_visible(fixed_mono));
    CHECK(session_repaint(sink, true));
    pixel_tui_perf_set_capture(true);
    pixel_tui_perf_reset();
    g_composer_mailbox = (composer_mailbox_t){.pending=true, .active=true, .cursor=5};
    strcpy(g_composer_mailbox.input, "word ");
    atomic_store(&g_composer_mailbox_pending, true);
    CHECK(session_apply_composer_mailbox_locked(sink));
    CHECK(session_caret_visible(fixed_mono));
    CHECK(session_caret_deadline_ms(fixed_mono) == 620);
    CHECK(session_repaint_composer(sink));
    CHECK(pixel_tui_perf_snapshot().wire_bytes > 0);
    CHECK(!session_caret_visible(fixed_mono + 0.7));
    CHECK(session_caret_visible(fixed_mono + 1.1));
    free(g_session.prev_frame);
    free(g_session.patch_buffer);
    reset_session();
    fixed_mono = 0;
    for (size_t i = 0; i < 14; ++i) {
        double scale = scales[i % 7];
        int width = 1120 + (i >= 7), height = 700 + (i >= 7);
        g_session = (pixel_session_t){.active=true, .width=width, .height=height,
            .backing_scale=scale, .surface_width=(int)ceil(width*scale),
            .surface_height=(int)ceil(height*scale), .cols=112, .rows=35,
            .state=PIXEL_TUI_IDLE, .current_message=-1, .input_active=true,
            .patch_enabled=true, .tty_out=sink};
        ui_motion_init(&g_session.motion, true);
        CHECK(session_repaint(sink, true));
        pixel_tui_perf_set_capture(true);
        pixel_tui_perf_reset();
        unsigned fast = 0;
        for (int n = 0; n < 40; ++n) {
            snprintf(g_session.input, sizeof(g_session.input), "typing latency iteration %d", n);
            g_session.input_cursor = strlen(g_session.input);
            if (session_repaint_composer(sink)) ++fast;
            else CHECK(session_repaint(sink, true));
        }
        CHECK(fast == 40);
        pixel_tui_perf_snapshot_t p = pixel_tui_perf_snapshot();
        printf("{\"width\":%d,\"height\":%d,\"scale\":%.2f,\"fast_frames\":%u,\"frame_p50_ms\":%.3f,"
               "\"frame_p95_ms\":%.3f,\"render_p95_ms\":%.3f,\"encode_p95_ms\":%.3f,"
               "\"wire_bytes\":%llu}\n", width, height, scale, fast, p.frame_ms_p50, p.frame_ms_p95,
               p.render_ms_p95, p.encode_ms_p95, (unsigned long long)p.wire_bytes);
        CHECK(session_repaint_composer(sink));
        pixel_tui_perf_snapshot_t unchanged = pixel_tui_perf_snapshot();
        CHECK(unchanged.wire_bytes == p.wire_bytes);
        CHECK(unchanged.identical_frames == p.identical_frames + 1);
        px_canvas_t *full = render_session_frame(g_session.width, g_session.height, scale,
            g_session.surface_width, g_session.surface_height, g_session.model, g_session.state);
        CHECK(full != NULL);
        if (full) {
            size_t bytes=(size_t)full->pixel_width*full->pixel_height*3;
            CHECK(memcmp(g_session.prev_frame, full->pixels, bytes) == 0);
            free_canvas(full);
        }
        free(g_session.prev_frame);
        free(g_session.patch_buffer);
        reset_session();
    }
    fclose(sink);
    printf("native composer performance: %d failures\n", failures);
    return failures != 0;
}
