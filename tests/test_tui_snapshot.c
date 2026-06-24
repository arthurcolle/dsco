/* test_tui_snapshot.c — Integument golden/snapshot tests for the TUI surface.
 *
 * ORGANISM_PLAN Phase I.2 / II Integument: the TUI is the product's face. These
 * are ANSI-normalized golden tests over the PURE, deterministic parts of the
 * render layer — box-drawing tables and the semantic-role/theme system — so a
 * regression in the skin is caught before it reaches a user (or a demo).
 *
 * "Golden" here means: assert the exact bytes a render primitive produces.
 * No TTY required; these run headless in CI.
 *
 * Build (standalone):
 *   cc -Iinclude tests/test_tui_snapshot.c src/tui.c <deps> -o /tmp/ttui && /tmp/ttui
 * Or via the project test harness.
 */
#include "tui.h"
#include "vm.h"
#include <stdio.h>
#include <string.h>

/* Normally defined in main.c (the CLI entry, excluded from this headless test
 * binary). Provide them here so the TUI/agent object graph links standalone. */
int  g_cheap_mode = 0;
vm_t g_vm;

static int g_pass = 0, g_fail = 0;

#define SNAP(name, got, want)                                                          \
    do {                                                                               \
        if (strcmp((got), (want)) == 0) {                                              \
            g_pass++;                                                                  \
        } else {                                                                       \
            g_fail++;                                                                  \
            fprintf(stderr, "SNAPSHOT FAIL [%s]\n  got : '%s'\n  want: '%s'\n", name,  \
                    (got), (want));                                                    \
        }                                                                              \
    } while (0)

#define CHECK(name, cond)                                                              \
    do {                                                                               \
        if (cond) {                                                                    \
            g_pass++;                                                                  \
        } else {                                                                       \
            g_fail++;                                                                  \
            fprintf(stderr, "CHECK FAIL [%s]\n", name);                                \
        }                                                                              \
    } while (0)

/* Golden: the rounded box corner set is stable and is what the panel UI draws. */
static void test_box_chars_round(void) {
    const tui_box_chars_t *b = tui_box_chars(BOX_ROUND);
    CHECK("box_round_nonnull", b != NULL);
    if (!b)
        return;
    SNAP("box_round_tl", b->tl, "\xe2\x95\xad"); /* ╭ */
    SNAP("box_round_tr", b->tr, "\xe2\x95\xae"); /* ╮ */
    SNAP("box_round_bl", b->bl, "\xe2\x95\xb0"); /* ╰ */
    SNAP("box_round_br", b->br, "\xe2\x95\xaf"); /* ╯ */
    SNAP("box_round_h", b->h, "\xe2\x94\x80");   /* ─ */
    SNAP("box_round_v", b->v, "\xe2\x94\x82");   /* │ */
}

/* Golden: the ASCII fallback box is pure ASCII (degrades safely on dumb terms). */
static void test_box_chars_ascii(void) {
    const tui_box_chars_t *b = tui_box_chars(BOX_ASCII);
    CHECK("box_ascii_nonnull", b != NULL);
    if (!b)
        return;
    SNAP("box_ascii_tl", b->tl, "+");
    SNAP("box_ascii_h", b->h, "-");
    SNAP("box_ascii_v", b->v, "|");
}

/* Golden: heavy + double box sets are distinct from round (no aliasing bug). */
static void test_box_styles_distinct(void) {
    const tui_box_chars_t *r = tui_box_chars(BOX_ROUND);
    const tui_box_chars_t *d = tui_box_chars(BOX_DOUBLE);
    const tui_box_chars_t *h = tui_box_chars(BOX_HEAVY);
    CHECK("round_vs_double_tl", r && d && strcmp(r->tl, d->tl) != 0);
    CHECK("round_vs_heavy_tl", r && h && strcmp(r->tl, h->tl) != 0);
    CHECK("double_vs_heavy_tl", d && h && strcmp(d->tl, h->tl) != 0);
}

/* Contract: theme accessors never return NULL — call sites concatenate them
 * directly into fprintf format streams; a NULL would be UB. */
static void test_theme_accessors_nonnull(void) {
    tui_apply_theme(TUI_THEME_DARK);
    CHECK("theme_dim_nonnull", tui_theme_dim() != NULL);
    CHECK("theme_bright_nonnull", tui_theme_bright() != NULL);
    CHECK("theme_accent_nonnull", tui_theme_accent() != NULL);
    tui_apply_theme(TUI_THEME_LIGHT);
    CHECK("theme_dim_nonnull_light", tui_theme_dim() != NULL);
    CHECK("theme_accent_nonnull_light", tui_theme_accent() != NULL);
}

/* Golden: semantic status roles are non-empty ANSI escapes (the Integument
 * meaning-coded palette). They must each begin with ESC '[' . */
static void test_status_roles_are_ansi(void) {
    const char *roles[] = {TUI_ROLE_OK, TUI_ROLE_FAIL, TUI_ROLE_WARN, TUI_ROLE_INFO, TUI_ROLE_MUTED};
    const char *names[] = {"OK", "FAIL", "WARN", "INFO", "MUTED"};
    for (int i = 0; i < 5; i++) {
        CHECK(names[i], roles[i] != NULL && roles[i][0] == '\033' && roles[i][1] == '[');
    }
}

int main(void) {
    test_box_chars_round();
    test_box_chars_ascii();
    test_box_styles_distinct();
    test_theme_accessors_nonnull();
    test_status_roles_are_ansi();

    fprintf(stderr, "\ntui snapshot: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
