#define _POSIX_C_SOURCE 200809L

#include "kitty_taste_grid.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

static void usage(const char *name) {
    fprintf(stderr,
            "usage: %s [--ppm PATH] [--width N] [--height N] [--no-hold]\n",
            name ? name : "dsco-kitty-tastes");
}

static void wait_for_close_key(void) {
    if (!isatty(STDIN_FILENO)) return;
    struct termios saved, raw;
    if (tcgetattr(STDIN_FILENO, &saved) != 0) {
        (void)getchar();
        return;
    }
    raw = saved;
    raw.c_lflag &= (tcflag_t)~(ICANON | ECHO);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) return;
    unsigned char ch = 0;
    while (read(STDIN_FILENO, &ch, 1) == 1) {
        if (ch == 'q' || ch == 'Q' || ch == 27 || ch == '\r' || ch == '\n')
            break;
    }
    (void)tcsetattr(STDIN_FILENO, TCSANOW, &saved);
}

int main(int argc, char **argv) {
    const char *ppm = NULL;
    int width = 0;
    int height = 0;
    bool hold = true;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            usage(argv[0]);
            return 0;
        } else if (!strcmp(argv[i], "--ppm") && i + 1 < argc) {
            ppm = argv[++i];
        } else if (!strcmp(argv[i], "--width") && i + 1 < argc) {
            width = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--height") && i + 1 < argc) {
            height = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--no-hold")) {
            hold = false;
        } else {
            usage(argv[0]);
            return 2;
        }
    }
    if (ppm) {
        if (width <= 0) width = 1800;
        if (height <= 0) height = 1000;
        return kitty_taste_grid_write_ppm(ppm, width, height) ? 0 : 1;
    }
    if (!kitty_taste_grid_render(stdout, width, height)) {
        fprintf(stderr,
                "dsco-kitty-tastes: Kitty graphics unavailable; use --ppm PATH\n");
        return 1;
    }
    if (hold) wait_for_close_key();
    kitty_taste_grid_clear(stdout);
    return 0;
}
