#include "kitty_lab.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(const char *name) {
    fprintf(stderr,
            "usage: %s [--view overview|plan|actions] [--ppm PATH] "
            "[--width N] [--height N] [--frames N] [--animate]\n",
            name ? name : "dsco-kitty-lab");
}

static bool parse_view(const char *value, kitty_lab_view_t *view) {
    if (!value || !view) return false;
    if (!strcmp(value, "overview")) *view = KITTY_LAB_VIEW_OVERVIEW;
    else if (!strcmp(value, "plan") || !strcmp(value, "planning"))
        *view = KITTY_LAB_VIEW_PLAN;
    else if (!strcmp(value, "actions") || !strcmp(value, "action"))
        *view = KITTY_LAB_VIEW_ACTIONS;
    else return false;
    return true;
}

int main(int argc, char **argv) {
    const char *ppm = NULL;
    int width = 960, height = 540, frames = 12;
    bool animate = false;
    kitty_lab_view_t view = KITTY_LAB_VIEW_OVERVIEW;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            usage(argv[0]);
            return 0;
        } else if (!strcmp(argv[i], "--view") && i + 1 < argc) {
            if (!parse_view(argv[++i], &view)) {
                usage(argv[0]);
                return 2;
            }
        } else if (!strcmp(argv[i], "--ppm") && i + 1 < argc) {
            ppm = argv[++i];
        } else if (!strcmp(argv[i], "--width") && i + 1 < argc) {
            width = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--height") && i + 1 < argc) {
            height = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--frames") && i + 1 < argc) {
            frames = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--animate")) {
            animate = true;
        } else {
            usage(argv[0]);
            return 2;
        }
    }
    if (ppm)
        return kitty_lab_write_ppm_view(ppm, width, height, 0, frames, view) ? 0 : 1;
    if (!kitty_lab_render_view(stdout, width, height, frames, animate, view)) {
        fprintf(stderr, "dsco-kitty-lab: Kitty graphics unavailable; use --ppm PATH\n");
        return 1;
    }
    return 0;
}
