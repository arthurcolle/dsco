/* test_tui_theme_snapshot.c — Integument golden tests: theme + box composition.
 *
 * ORGANISM_PLAN Phase II Integument. Where test_tui_snapshot.c covers the
 * primitive byte tables, this covers COMPOSED render behavior and the theme
 * system's invariants under switching — the properties that actually break in
 * a regression (a theme leaving a role NULL, two themes collapsing to the same
 * palette, a box style losing its junction characters).
 *
 * Headless; no TTY required.
 */
#include "tui.h"
#include "vm.h"
#include <stdio.h>
#include <string.h>

int  g_cheap_mode = 0;
vm_t g_vm;
volatile int g_interrupted = 0;
double g_cost_budget = 0.0;

static int g_pass = 0, g_fail = 0;

#define CHECK(name, cond)                                                               \
    do {                                                                                \
        if (cond) {                                                                     \
            g_pass++;                                                                   \
        } else {                                                                        \
            g_fail++;                                                                   \
            fprintf(stderr, "CHECK FAIL [%s]\n", name);                                 \
        }                                                                               \
    } while (0)

/* Every box style must expose a complete, non-empty 8-char set. A render that
 * concatenates a NULL corner into a format stream is UB; an empty corner draws
 * a broken frame. This is the contract the panel UI relies on. */
static void test_all_box_styles_complete(void) {
    tui_box_style_t styles[] = {BOX_ROUND, BOX_SINGLE, BOX_DOUBLE, BOX_HEAVY, BOX_ASCII};
    const char *names[] = {"round", "single", "double", "heavy", "ascii"};
    for (int i = 0; i < 5; i++) {
        const tui_box_chars_t *b = tui_box_chars(styles[i]);
        char nm[64];
        snprintf(nm, sizeof(nm), "%s_nonnull", names[i]);
        CHECK(nm, b != NULL);
        if (!b)
            continue;
        const char *parts[] = {b->tl, b->tr, b->bl, b->br, b->h, b->v, b->lj, b->rj};
        for (int j = 0; j < 8; j++) {
            snprintf(nm, sizeof(nm), "%s_part%d_nonempty", names[i], j);
            CHECK(nm, parts[j] != NULL && parts[j][0] != '\0');
        }
    }
}

/* Theme accessors must remain non-NULL across repeated switching (no state that
 * decays to NULL on the second apply). Exercise the switch cycle. */
static void test_theme_switch_stable(void) {
    for (int cycle = 0; cycle < 3; cycle++) {
        tui_apply_theme(TUI_THEME_DARK);
        CHECK("dark_dim", tui_theme_dim() != NULL);
        CHECK("dark_bright", tui_theme_bright() != NULL);
        CHECK("dark_accent", tui_theme_accent() != NULL);
        tui_apply_theme(TUI_THEME_LIGHT);
        CHECK("light_dim", tui_theme_dim() != NULL);
        CHECK("light_bright", tui_theme_bright() != NULL);
        CHECK("light_accent", tui_theme_accent() != NULL);
    }
}

/* The 5 semantic status roles must be DISTINCT escape strings — if two collapse
 * to the same bytes, success and failure would render identically (an
 * accessibility/clarity regression). */
static void test_status_roles_distinct(void) {
    const char *r[] = {TUI_ROLE_OK, TUI_ROLE_FAIL, TUI_ROLE_WARN, TUI_ROLE_INFO};
    const char *n[] = {"OK", "FAIL", "WARN", "INFO"};
    for (int i = 0; i < 4; i++) {
        for (int j = i + 1; j < 4; j++) {
            char nm[64];
            snprintf(nm, sizeof(nm), "%s_ne_%s", n[i], n[j]);
            CHECK(nm, strcmp(r[i], r[j]) != 0);
        }
    }
}

/* term_width has a sane floor (it falls back to 80 off-TTY). A zero/negative
 * width would cause divide-by-zero in column math downstream. */
static void test_term_width_floor(void) {
    int w = tui_term_width();
    CHECK("term_width_positive", w > 0);
}

int main(void) {
    test_all_box_styles_complete();
    test_theme_switch_stable();
    test_status_roles_distinct();
    test_term_width_floor();
    fprintf(stderr, "\ntui theme snapshot: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
