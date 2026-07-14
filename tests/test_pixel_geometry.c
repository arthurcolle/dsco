/* P1.1 / P2.3 of TUI_IMPROVEMENT_PLAN.md — viewport metrics, DPR heuristics,
 * density breakpoints, and shell-layout invariants for the pixel compositor.
 *
 * Everything here exercises the public native_ui API (no tty, no kitty, no
 * statics), so it runs headless and deterministic. Threshold constants match
 * native_ui_terminal_viewport() as of 2026-07-14:
 *   backing 3x when cell_h >= 38 or cell_w >= 27
 *   backing 2x when cell_h >= 24 or cell_w >= 14
 *   otherwise 1x; explicit requested_scale 1..4 always wins.
 */

#include "native_ui.h"
#include "vm.h"

#include <stdio.h>
#include <string.h>

/* Normally defined in main.c (excluded from headless test binaries). Provide
 * them so the TUI test object graph links standalone — same pattern as
 * tests/test_tui_snapshot.c. */
int  g_cheap_mode = 0;
vm_t g_vm;
volatile int g_interrupted = 0;
double g_cost_budget = 0.0;

static int g_pass = 0, g_fail = 0;

#define CHECK(cond, ...) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
           fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } \
} while (0)

/* Build physical dimensions that produce an exact integer cell size. */
static native_ui_viewport_metrics_t vp(int cols, int rows,
                                       int cell_w, int cell_h,
                                       int requested) {
    return native_ui_terminal_viewport(cols, rows,
                                       cell_w > 0 ? cols * cell_w : 0,
                                       cell_h > 0 ? rows * cell_h : 0,
                                       requested);
}

static void test_defaults_and_fallbacks(void) {
    /* Zero/negative geometry falls back to 80x24 and cols*10 / rows*20. */
    native_ui_viewport_metrics_t m = native_ui_terminal_viewport(0, 0, 0, 0, 0);
    CHECK(m.columns == 80 && m.rows == 24, "default grid, got %dx%d", m.columns, m.rows);
    CHECK(m.backing_scale == 1, "no pixels => 1x, got %d", m.backing_scale);
    CHECK(m.logical_width == 800 && m.logical_height == 480,
          "fallback logical 800x480, got %dx%d", m.logical_width, m.logical_height);

    m = native_ui_terminal_viewport(-3, -1, -100, -100, 0);
    CHECK(m.columns == 80 && m.rows == 24, "negative grid clamped");
    CHECK(m.backing_scale == 1, "negative pixels => 1x, got %d", m.backing_scale);
}

static void test_requested_scale_override(void) {
    /* Explicit 1..4 wins over any heuristic; out-of-range falls back. */
    for (int s = 1; s <= 4; s++) {
        native_ui_viewport_metrics_t m = vp(80, 24, 31, 58, s);
        CHECK(m.backing_scale == s, "requested %d honored, got %d", s, m.backing_scale);
    }
    native_ui_viewport_metrics_t m = vp(80, 24, 8, 16, 0);
    CHECK(m.backing_scale == 1, "requested 0 => heuristic 1x, got %d", m.backing_scale);
    m = vp(80, 24, 8, 16, 5);
    CHECK(m.backing_scale == 1, "requested 5 => heuristic 1x, got %d", m.backing_scale);
    m = vp(80, 24, 8, 16, -2);
    CHECK(m.backing_scale == 1, "requested -2 => heuristic 1x, got %d", m.backing_scale);
}

static void test_heuristic_boundaries(void) {
    /* cell_h boundaries with cell_w held safely at 1x (10px). */
    CHECK(vp(80, 24, 10, 23, 0).backing_scale == 1, "cell_h 23 => 1x");
    CHECK(vp(80, 24, 10, 24, 0).backing_scale == 2, "cell_h 24 => 2x");
    CHECK(vp(80, 24, 10, 37, 0).backing_scale == 2, "cell_h 37 => 2x");
    CHECK(vp(80, 24, 10, 38, 0).backing_scale == 3, "cell_h 38 => 3x");

    /* cell_w boundaries with cell_h held safely at 1x (18px). */
    CHECK(vp(80, 24, 13, 18, 0).backing_scale == 1, "cell_w 13 => 1x");
    CHECK(vp(80, 24, 14, 18, 0).backing_scale == 2, "cell_w 14 => 2x");
    CHECK(vp(80, 24, 26, 18, 0).backing_scale == 2, "cell_w 26 => 2x");
    CHECK(vp(80, 24, 27, 18, 0).backing_scale == 3, "cell_w 27 => 3x");

    /* Either axis alone is sufficient to escalate. */
    CHECK(vp(80, 24, 8, 40, 0).backing_scale == 3, "tall cell alone => 3x");
    CHECK(vp(80, 24, 30, 16, 0).backing_scale == 3, "wide cell alone => 3x");
}

static void test_logical_division(void) {
    native_ui_viewport_metrics_t m = vp(100, 50, 20, 40, 0); /* 2000x2000 phys */
    CHECK(m.backing_scale == 3, "20x40 cell => 3x, got %d", m.backing_scale);
    CHECK(m.logical_width == 2000 / 3 && m.logical_height == 2000 / 3,
          "logical = physical/backing, got %dx%d", m.logical_width, m.logical_height);
    CHECK(m.physical_cell_width == 20 && m.physical_cell_height == 40,
          "cell metrics preserved, got %dx%d",
          m.physical_cell_width, m.physical_cell_height);

    m = vp(120, 40, 16, 32, 0); /* classic Retina 2x-ish cell */
    CHECK(m.backing_scale == 2, "16x32 cell => 2x, got %d", m.backing_scale);
    CHECK(m.logical_width == 120 * 16 / 2 && m.logical_height == 40 * 32 / 2,
          "2x logical halves physical");
}

static void test_observed_regression_case(void) {
    /* Documented live geometry from the 2026-07-14 primary-monitor-disconnect
     * incident: kitty on the built-in Retina panel reported 75x29 cells over
     * 2325x1682 physical px (cell 31x58). The physical backing was 2x with a
     * large terminal font; the current thresholds classify it as 3x. This
     * check pins today's behavior so any retune is a conscious decision, and
     * the comment preserves the tuning question. */
    native_ui_viewport_metrics_t m =
        native_ui_terminal_viewport(75, 29, 2325, 1682, 0);
    CHECK(m.physical_cell_width == 31 && m.physical_cell_height == 58,
          "incident cell 31x58, got %dx%d",
          m.physical_cell_width, m.physical_cell_height);
    CHECK(m.backing_scale == 3,
          "incident geometry currently classifies 3x, got %d (threshold retune?)",
          m.backing_scale);
    CHECK(m.logical_width == 2325 / 3 && m.logical_height == 1682 / 3,
          "incident logical %dx%d", m.logical_width, m.logical_height);
    /* Whatever the scale decision, the outcome must stay in the sane band the
     * session clamps to: never the raw physical size, never microscopic. */
    CHECK(m.logical_width >= 320 && m.logical_width < 2325,
          "logical width sane, got %d", m.logical_width);
}

static void test_density_breakpoints(void) {
    CHECK(native_ui_density_for_size(719, 400) == NATIVE_UI_DENSITY_COMPACT,
          "w<720 compact");
    CHECK(native_ui_density_for_size(900, 319) == NATIVE_UI_DENSITY_COMPACT,
          "h<320 compact");
    CHECK(native_ui_density_for_size(720, 320) == NATIVE_UI_DENSITY_DENSE,
          "720x320 dense");
    CHECK(native_ui_density_for_size(1399, 600) == NATIVE_UI_DENSITY_DENSE,
          "1399 wide dense");
    CHECK(native_ui_density_for_size(1400, 439) == NATIVE_UI_DENSITY_DENSE,
          "short expanded-width dense");
    CHECK(native_ui_density_for_size(1400, 440) == NATIVE_UI_DENSITY_EXPANDED,
          "1400x440 expanded");
}

static void check_rect_nonneg(native_ui_rect_t r, const char *name) {
    CHECK(r.width >= 0 && r.height >= 0, "%s non-negative, got %dx%d",
          name, r.width, r.height);
}

static void test_shell_layout_invariants(void) {
    static const struct { int w, h; } sizes[] = {
        {0, 0}, {100, 100}, {640, 360}, {720, 320},
        {1162, 841}, {1400, 440}, {1920, 1080},
    };
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        native_ui_agent_shell_layout_t l =
            native_ui_agent_shell_layout(sizes[i].w, sizes[i].h);
        check_rect_nonneg(l.header, "header");
        check_rect_nonneg(l.transcript, "transcript");
        check_rect_nonneg(l.inspector, "inspector");
        check_rect_nonneg(l.composer, "composer");
        CHECK(l.shows_inspector == (l.density == NATIVE_UI_DENSITY_EXPANDED),
              "inspector iff expanded at %dx%d", sizes[i].w, sizes[i].h);
        if (l.shows_inspector)
            CHECK(l.inspector.width >= 190 && l.inspector.width <= 215,
                  "inspector width band, got %d", l.inspector.width);
        /* Composer anchored at the bottom, transcript above it. */
        if (sizes[i].h >= 320)
            CHECK(l.transcript.y + l.transcript.height <= l.composer.y,
                  "transcript above composer at %dx%d", sizes[i].w, sizes[i].h);
    }
}

int main(void) {
    test_defaults_and_fallbacks();
    test_requested_scale_override();
    test_heuristic_boundaries();
    test_logical_division();
    test_observed_regression_case();
    test_density_breakpoints();
    test_shell_layout_invariants();
    printf("pixel geometry: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
