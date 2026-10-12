/* Headless real renderer/model regression; no desktop windows or inference. */
#define main compositor_fixture_main
#include "test_pixel_native_windows.c"
#undef main

int main(void) {
    FILE *out = tmpfile();
    if (!out) return 2;
    native_windows_reset();
    memset(&g_session, 0, sizeof(g_session));
    CHECK(!pixel_tui_session_show_details(out), "non-native session does not open a panel");
    g_session.active = true;
    g_session.width = 800; g_session.height = 600;
    g_session.surface_width = 800; g_session.surface_height = 600;
    g_session.backing_scale = 1; g_session.cols = 100; g_session.rows = 50;
    g_session.current_message = -1; g_session.started_s = monotonic_s() - 23;
    g_session.tty_out = out;
    ui_motion_init(&g_session.motion, true);
    snprintf(g_session.model, sizeof(g_session.model), "fixture-model");
    snprintf(g_session.input, sizeof(g_session.input), "keep this draft π");
    g_session.input_cursor = 5;
    g_session.queue_capacity = 8;
    pixel_tui_session_set_details(out, "Provider: fixture\nSession: owned-test");
    pixel_tui_session_set_usage(out, 1200, 300, 0, 1, 4);
    CHECK(!session_refresh_details(false), "background metrics never opt in");
    sample(); CHECK(snapshot.count == 0 && !snapshot.visible, "startup stays panel-free");
    CHECK(pixel_tui_session_show_details(out), "explicit details request opens panel");
    sample();
    CHECK(snapshot.count == 1 && snapshot.visible && snapshot.windows[0].kind == NATIVE_WINDOW_SESSION,
          "runtime owns one visible read-only details panel");
    uint64_t id = snapshot.windows[0].id;
    CHECK(strstr(snapshot.windows[0].text, "fixture-model") &&
          strstr(snapshot.windows[0].text, "Input tokens: 1200") &&
          strstr(snapshot.windows[0].text, "Provider reported: unknown") &&
          strstr(snapshot.windows[0].text, "Provider: fixture"), "panel has observed metrics and explicit unknown costs");
    CHECK(!strcmp(g_session.input, "keep this draft π") && g_session.input_cursor == 5,
          "opening preserves composer bytes and cursor");
    CHECK(pixel_tui_session_show_details(out), "repeated request succeeds");
    sample(); CHECK(snapshot.count == 1 && snapshot.windows[0].id == id, "repeated request reuses identity");
    px_canvas_t *before = render_session_frame(800, 600, 1, 800, 600, g_session.model, PIXEL_TUI_IDLE);
    pixel_tui_session_set_usage(out, 4321, 987, 0.125, 2, 9);
    pixel_tui_session_set_cost_details(out, 0, 0, 0.125, 1, 2);
    px_canvas_t *after = render_session_frame(800, 600, 1, 800, 600, g_session.model, PIXEL_TUI_IDLE);
    sample();
    CHECK(before && after && changed_rows(before, after, 90, 480) > 0, "changed metrics paint different real panel pixels");
    CHECK(strstr(snapshot.windows[0].text, "Input tokens: 4321") &&
          strstr(snapshot.windows[0].text, "Reference estimate: $0.1250") &&
          strstr(snapshot.windows[0].text, "+ unknown"), "live refresh preserves pricing provenance");
    free_canvas(before); free_canvas(after);
    char request[256], result[2048];
    snprintf(request, sizeof(request), "{\"action\":\"update\",\"id\":%llu,\"text\":\"forged\"}", (unsigned long long)id);
    CHECK(!native_windows_command(request, result, sizeof(result)) && strstr(result, "session_details_read_only"),
          "generic update cannot forge runtime metrics");
    CHECK(command(note), "unrelated note opens");
    sample(); uint64_t focus = snapshot.focused_id;
    native_ui_rect_t rect = snapshot.windows[0].rect;
    CHECK(session_refresh_details(false), "refresh while other panel focused");
    sample(); CHECK(snapshot.focused_id == focus && !memcmp(&rect, &snapshot.windows[0].rect, sizeof(rect)),
          "refresh preserves focus, stacking and geometry");
    CHECK(command("{\"action\":\"hide\"}"), "hide accepted");
    CHECK(session_refresh_details(false) && !native_windows_session_details_visible(), "refresh never unhides workspace");
    CHECK(pixel_tui_session_show_details(out), "explicit request unhides details");
    sample(); CHECK(snapshot.focused_id == id && snapshot.count == 2, "explicit request raises existing panel");
    snprintf(request, sizeof(request), "{\"action\":\"scroll\",\"id\":%llu,\"lines\":3}", (unsigned long long)id);
    CHECK(command(request), "details can scroll");
    sample(); int scroll = snapshot.windows[snapshot.count - 1].scroll;
    session_refresh_details(false); sample();
    CHECK(snapshot.windows[snapshot.count - 1].scroll == scroll, "refresh retains scroll position");
    snprintf(request, sizeof(request), "{\"action\":\"close\",\"id\":%llu}", (unsigned long long)id);
    CHECK(command(request), "details can close");
    CHECK(!session_refresh_details(false), "refresh does not recreate closed panel");
    sample(); CHECK(snapshot.count == 1 && snapshot.windows[0].id == focus, "closing preserves unrelated note");
    for (int i = 1; i < NATIVE_WINDOWS_MAX; i++) CHECK(command(note), "fill available slots");
    CHECK(!pixel_tui_session_show_details(out), "full workspace fails without evicting other panels");
    sample(); CHECK(snapshot.count == NATIVE_WINDOWS_MAX, "capacity failure preserves all panels");
    native_windows_reset();
    CHECK(!session_refresh_details(false), "reset forgets details opt-in");
    char tiny[3] = {'x', 'x', 'Z'};
    native_session_details_t values = {.model = "long model", .elapsed_s = NAN};
    native_session_details_format(&values, tiny, 2);
    CHECK(tiny[1] == 0 && tiny[2] == 'Z', "formatter terminates within provided capacity");
    native_session_details_format(NULL, tiny, sizeof(tiny));
    CHECK(!tiny[0], "null formatter input is safe");
    window_raster_clear();
    g_session.active = false; g_session.tty_out = NULL;
    fclose(out);
    fprintf(stderr, "native_session_details: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
