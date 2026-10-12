/* Headless transport fault injection using owned temporary descriptors only.
 * Buffered mode fails at fflush; unbuffered mode fails during the Kitty write.
 * No terminal, window, provider, or application session is touched. */
#define _DARWIN_C_SOURCE
#include <time.h>
static double fixed_mono;
static int fixture_clock_gettime(clockid_t clock, struct timespec *ts) {
    if (clock == CLOCK_MONOTONIC) {
        ts->tv_sec = (time_t)fixed_mono;
        ts->tv_nsec = (long)((fixed_mono - ts->tv_sec) * 1e9);
        return 0;
    }
    return clock_gettime(clock, ts);
}
#define clock_gettime fixture_clock_gettime
#define main compositor_fixture_main
#include "test_pixel_native_windows.c"
#undef main
#undef clock_gettime
#include <fcntl.h>

/* lane: 0 = background, 1 = composer, 2 = live activity. */
static void transport_case(bool patch, bool buffered, int lane) {
    fixed_mono = 1000;
    native_windows_reset();
    memset(&g_session, 0, sizeof(g_session));
    g_session.active = true;
    g_session.width = g_session.surface_width = 800;
    g_session.height = g_session.surface_height = 600;
    g_session.backing_scale = 1;
    g_session.cols = 100; g_session.rows = 50;
    g_session.current_message = -1;
    g_session.state = PIXEL_TUI_IDLE;
    g_session.patch_enabled = true;
    snprintf(g_session.model, sizeof(g_session.model), "transport recovery fixture");
    snprintf(g_session.input, sizeof(g_session.input), "before");
    ui_motion_init(&g_session.motion, lane != 2);
    if (lane == 2) {
        g_session.state = PIXEL_TUI_EXECUTING;
        g_session.animation_enabled = true;
        g_session.animation_interval_ms = 17;
        g_session.tool_visuals[0] = (pixel_tool_visual_t){.used = true, .sequence = 1,
            .status = PIXEL_OP_RUNNING, .started_s = 998};
        strcpy(g_session.tool_visuals[0].name, "bash");
        strcpy(g_session.tool_visuals[0].preview, "owned fixture");
    }
    pixel_tui_perf_reset();
    pixel_tui_perf_set_capture(true);

    FILE *out = tmpfile();
    char *buffer = buffered ? malloc(8U * 1024U * 1024U) : NULL;
    CHECK(out && (!buffered || buffer), "owned transport fixture allocates");
    if (!out || (buffered && !buffer)) {
        if (out) fclose(out);
        free(buffer);
        return;
    }
    CHECK(setvbuf(out, buffer, buffered ? _IOFBF : _IONBF,
                  buffered ? 8U * 1024U * 1024U : 0) == 0, "transport buffering configured");
    CHECK(session_repaint(out, true), "initial authoritative frame succeeds");
    uint32_t resident_id = g_session.prev_frame_image;
    size_t frame_bytes = (size_t)g_session.prev_frame_width * g_session.prev_frame_height * 3U;
    uint32_t baseline_crc = crc32(0, g_session.prev_frame, (uInt)frame_bytes);
    CHECK(resident_id && frame_bytes, "successful frame establishes resident baseline");
    snprintf(g_session.input, sizeof(g_session.input), "after");
    g_session.input_cursor = strlen(g_session.input);
    if (!patch) g_session.patch_streak = SESSION_PATCH_STREAK_LIMIT;

    int original = dup(fileno(out));
    int readonly = open("/dev/null", O_RDONLY);
    CHECK(original >= 0 && readonly >= 0, "owned descriptors available for fault injection");
    if (original < 0 || readonly < 0) {
        if (original >= 0) close(original);
        if (readonly >= 0) close(readonly);
        goto cleanup;
    }
    CHECK(dup2(readonly, fileno(out)) >= 0, "write descriptor replaced by read-only sink");
    close(readonly);
    fixed_mono += 0.1;
    bool sent = lane == 1 ? session_repaint_composer(out) :
                lane == 2 ? session_repaint_activity(out) : session_repaint(out, true);
    CHECK(!sent && ferror(out), "injected output failure is reported, not swallowed");
    CHECK(g_session.prev_frame_image == 0 &&
              g_session.patch_streak >= SESSION_PATCH_STREAK_LIMIT,
          "failed transport invalidates every incremental/identical fast path");
    CHECK(crc32(0, g_session.prev_frame, (uInt)frame_bytes) == baseline_crc,
          "failed bytes are never committed as the resident pixel baseline");
    CHECK(!session_composer_patch_possible(), "caret cannot patch an uncertain resident image");
    CHECK(dup2(original, fileno(out)) >= 0, "original transport descriptor restored");
    close(original);
    clearerr(out); /* Explicit transport owner recovery, not renderer error masking. */
    long before = ftell(out);
    CHECK(session_repaint(out, true), "first repaint after recovery succeeds");
    CHECK(ftell(out) > before, "recovery transmits bytes rather than skipping an identical frame");
    CHECK(g_session.prev_frame_image && g_session.prev_frame_image != resident_id &&
              g_session.prev_frame_image == g_session.image_ids[g_session.state] &&
              g_session.patch_streak == 0,
          "recovery establishes a fresh authoritative full frame");
    pixel_tui_perf_snapshot_t perf = pixel_tui_perf_snapshot();
    CHECK(perf.full_frames == 2, "recovery is counted as full transport, not a patch or skip");
    before = ftell(out);
    CHECK(session_repaint(out, true) && ftell(out) == before,
          "healthy identical frames still avoid transmission");
    fprintf(stderr, "transport case: %s %s %s checked\n", patch ? "patch" : "full",
            buffered ? "flush-failure" : "write-failure", lane == 1 ? "composer" : lane == 2 ? "activity" : "background");
cleanup:
    fclose(out); free(buffer);
    free(g_session.prev_frame); free(g_session.patch_buffer); free(g_session.activity_underlay);
    session_messages_free();
    memset(&g_session, 0, sizeof(g_session));
    pixel_tui_perf_set_capture(false);
}

#ifdef __APPLE__
typedef struct { bool reject_placement, rejected; size_t bytes; } placement_sink_t;
static int placement_write(void *cookie, const char *data, int len) {
    placement_sink_t *sink = cookie;
    if (sink->reject_placement && memmem(data, (size_t)len, "\033_Ga=p,", 7)) {
        sink->rejected = true;
        errno = EIO;
        return -1;
    }
    sink->bytes += (size_t)len;
    return len;
}

/* A placement fprintf can fail AFTER the encoder succeeds. With unbuffered
 * output, fflush can still return zero while ferror remains latched. */
static void placement_failure(void) {
    fixed_mono = 1000;
    g_session = (pixel_session_t){.active = true, .width = 800, .height = 600,
        .surface_width = 800, .surface_height = 600, .backing_scale = 1,
        .cols = 100, .rows = 50, .current_message = -1, .patch_enabled = true};
    ui_motion_init(&g_session.motion, true);
    placement_sink_t sink = {0};
    FILE *out = funopen(&sink, NULL, placement_write, NULL, NULL);
    CHECK(out != NULL, "owned placement fault sink opens");
    if (!out) return;
    setvbuf(out, NULL, _IONBF, 0);
    CHECK(session_repaint(out, true), "placement fixture establishes baseline");
    uint32_t resident_id = g_session.prev_frame_image;
    strcpy(g_session.input, "new frame");
    g_session.patch_streak = SESSION_PATCH_STREAK_LIMIT;
    sink.reject_placement = true;
    CHECK(!session_repaint(out, true) && sink.rejected && ferror(out),
          "post-encode placement write failure is reported despite successful fflush");
    CHECK(!g_session.prev_frame_image, "failed placement cannot certify resident pixels");
    sink.reject_placement = false;
    clearerr(out);
    size_t before = sink.bytes;
    CHECK(session_repaint(out, true) && sink.bytes > before &&
              g_session.prev_frame_image && g_session.prev_frame_image != resident_id,
          "failed placement recovers with a new authoritative full frame");
    fclose(out);
    free(g_session.prev_frame); free(g_session.patch_buffer); free(g_session.activity_underlay);
    session_messages_free();
    memset(&g_session, 0, sizeof(g_session));
}
#endif

int main(void) {
    unsetenv("DSCO_PIXEL_TUI_SESSION_SNAPSHOT");
    for (int buffered = 0; buffered < 2; ++buffered) {
        transport_case(false, buffered, 0);
        transport_case(true, buffered, 0);
        transport_case(true, buffered, 1);
        transport_case(true, buffered, 2);
    }
#ifdef __APPLE__
    placement_failure();
#endif
    printf("native transport recovery: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
