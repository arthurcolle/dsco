/* Fractional display zoom, logical hit coordinates, and 1080p raster proof. */
#define _DARWIN_C_SOURCE
#include "../src/pixel_tui.c"
#include "vm.h"
int g_cheap_mode;
vm_t g_vm;
volatile int g_interrupted;
double g_cost_budget;
static int failures;
#define CHECK(x) do { if (!(x)) { failures++; fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); } } while (0)
int main(void) {
    unsetenv("DSCO_PIXEL_TUI_DPR");
    unsetenv("DSCO_PIXEL_TUI_ZOOM");
    double zoom;
    CHECK(native_display_zoom_parse("1.25", &zoom) && zoom == 1.25);
    CHECK(native_display_zoom_parse("150%", &zoom) && zoom == 1.5);
    CHECK(native_display_zoom_parse("auto", &zoom) && zoom == 0);
    CHECK(native_display_zoom_parse("0.75", &zoom) && zoom == 0.75);
    CHECK(native_display_zoom_parse("0.05", &zoom) && zoom == 0.05);
    CHECK(native_display_zoom_parse("5%", &zoom) && zoom == 0.05);
    CHECK(native_display_zoom_parse("10", &zoom) && zoom == 10);
    const char *invalid[] = {"nan", "inf", "0", "1.25junk", "-1"};
    for (size_t i = 0; i < sizeof(invalid)/sizeof(invalid[0]); i++)
        CHECK(!native_display_zoom_parse(invalid[i], &zoom));
    CHECK(render_logical_scale(192, 54, 1920, 1080) == 1.25);
    CHECK(render_logical_scale(192, 54, 3840, 2160) == 2.0);
    setenv("DSCO_PIXEL_TUI_DPR", "1", 1);
    CHECK(render_logical_scale(192, 54, 1920, 1080) == 1.0);
    unsetenv("DSCO_PIXEL_TUI_DPR");
    double levels[] = {0.05, 0.75, 1.0, 1.25, 1.5, 1.75, 2.0, 3.0, 5.0};
    for (size_t i = 0; i < sizeof(levels)/sizeof(levels[0]); i++) {
        s_display_zoom_override = levels[i];
        int w, h, sw, sh;
        double scale;
        session_render_geometry(192, 54, 1920, 1080, &w, &h, &scale, &sw, &sh);
        CHECK(abs(w - (int)(1920 / levels[i])) <= 1);
        CHECK(abs(h - (int)(1080 / levels[i])) <= 1);
        CHECK(abs(sw * 1080 - sh * 1920) <= 1920 * scale);
        CHECK(sw == 1920 && sh == 1080);
        CHECK(fabs(scale - levels[i]) < 1e-9);
        native_ui_agent_shell_layout_t shell = native_ui_agent_shell_layout(w, h);
        CHECK(shell.composer.y + shell.composer.height <= h);
        CHECK(shell.transcript.height > 60);
        g_session = (pixel_session_t){.width=w, .height=h, .backing_scale=scale,
            .surface_width=sw, .surface_height=sh, .cols=192, .rows=54,
            .state=PIXEL_TUI_EXECUTING, .current_message=-1, .input_active=true};
        ui_motion_init(&g_session.motion, true);
        strcpy(g_session.input, "Keep working while I review the tool results.");
        g_session.input_cursor = strlen(g_session.input);
        pixel_message_t *m = session_new_message("USER", "");
        message_text_set_plain(m, "Improve the external 1080p display and the tool-calling experience.");
        m = session_new_message("ASSISTANT", "");
        message_text_set_plain(m, "## Readable at this zoom\nThe transcript, input, and tool activity share one logical viewport.\n\n- Fractional sizing without a jump to 200%\n- Tool results complete the correct call\n- Your draft remains ready while tools run\n\n```c\ncomplete_tool(operation_id, result);\n```");
        g_session.tool_visuals[0] = (pixel_tool_visual_t){.used=true,.sequence=1,
            .status=PIXEL_OP_RUNNING,.started_s=monotonic_s()-2.0};
        strcpy(g_session.tool_visuals[0].name, "bash");
        strcpy(g_session.tool_visuals[0].preview, "make test_native_compositor test_pixel_geometry");
        px_canvas_t *c = render_session_frame(w, h, scale, sw, sh, "native display review", PIXEL_TUI_EXECUTING);
        CHECK(c != NULL);
        if (c && getenv("DSCO_KEEP_NATIVE_ARTIFACTS")) {
            char path[128];
            snprintf(path, sizeof(path), "/tmp/dsco-1080p-zoom-%d.ppm", (int)(levels[i]*100));
            CHECK(canvas_write_ppm(path, c));
        }
        if (c) free_canvas(c);
        session_messages_free();
        free(g_session.activity_underlay);
        g_session = (pixel_session_t){0};
        visual_cache_invalidate();
    }
    s_display_zoom_override = -1;
    printf("native zoom: %d failures\n", failures);
    return failures != 0;
}
