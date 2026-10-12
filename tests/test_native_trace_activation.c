/* Reuse the headless compositor fixture. Native tracing and explicit capture
 * share pixel_tui_perf_enabled(); activate capture between start and finish. */
#define main compositor_fixture_main
#include "test_pixel_native_windows.c"
#undef main
int main(void) {
    unsetenv("DSCO_PIXEL_TUI_PERF");
    pixel_tui_perf_reset();pixel_tui_perf_set_capture(false);
    double absent_start=pixel_tui_perf_enabled()?monotonic_s()*1000.0:0.0;
    pixel_tui_perf_set_capture(true);
    pixel_tui_frame_sample_t sample={.kind=PIXEL_TUI_FRAME_FULL};
    perf_finish_frame(&sample,absent_start);
    pixel_tui_perf_snapshot_t missing=pixel_tui_perf_snapshot();
    sample=(pixel_tui_frame_sample_t){.kind=PIXEL_TUI_FRAME_FULL};
    perf_finish_frame(&sample,monotonic_s()*1000.0-2.0);
    pixel_tui_perf_snapshot_t valid=pixel_tui_perf_snapshot();
    pixel_tui_perf_set_capture(false);
    fprintf(stderr,"missing-start samples=%llu; total=%llu; max-frame=%.3f ms\n",(unsigned long long)missing.frames,(unsigned long long)valid.frames,valid.frame_ms_max);
    CHECK(missing.frames==0,"mid-frame activation cannot fabricate a frame latency");
    CHECK(valid.frames==1 && valid.frame_ms_total>=1.0 && valid.frame_ms_max<1000,"next measured frame has plausible latency");
    if(!failures)puts("native_trace_activation: 2 timing invariants passed");
    return failures?1:0;
}
