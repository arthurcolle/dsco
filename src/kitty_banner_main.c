/* dsco-banner: emit the animated Distributed Systems wordmark and exit.
 * The terminal keeps looping the animation (see src/kitty_banner.c).
 *
 *   dsco-banner                  render into the current Kitty window (falls
 *                                back to ANSI half-block cells elsewhere)
 *   dsco-banner --layers [N]     z-stacked variant: five pixel planes flipped
 *                                client-side at independent tempos, N cycles
 *   dsco-banner --cells [N]      finite renderer, N loops: pixel-native on
 *                                Kitty-protocol terminals, sextant/half-block
 *                                mosaics elsewhere (DSCO_BANNER_CELLS pins)
 *   dsco-banner --ppm DIR        dump every animation frame as PPM
 *   dsco-banner --ppm-layers DIR dump per-layer + composite PPMs */

#include "kitty_banner.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc >= 2 && strcmp(argv[1], "--layers") == 0) {
        int loops = argc >= 3 ? atoi(argv[2]) : 3;
        if (kitty_banner_available(stdout) &&
            kitty_banner_render_layers(stdout, loops))
            return 0;
        fprintf(stderr,
                "dsco-banner: layered render needs the Kitty graphics "
                "protocol (kitty/ghostty/wezterm)\n");
        return 1;
    }
    if (argc >= 3 && strcmp(argv[1], "--ppm-layers") == 0) {
        if (!kitty_banner_write_layers_ppm(argv[2])) {
            fprintf(stderr, "dsco-banner: failed writing layer PPMs to %s\n",
                    argv[2]);
            return 1;
        }
        printf("wrote layer + composite PPMs to %s\n", argv[2]);
        return 0;
    }
    if (argc >= 3 && strcmp(argv[1], "--ppm") == 0) {
        char path[1024];
        for (int f = 0; f < 24; f++) {
            snprintf(path, sizeof(path), "%s/banner_%02d.ppm", argv[2], f);
            if (!kitty_banner_write_ppm(path, f, 24)) {
                fprintf(stderr, "dsco-banner: failed writing %s\n", path);
                return 1;
            }
        }
        printf("wrote 24 frames to %s\n", argv[2]);
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "--cells") == 0) {
        int loops = argc >= 3 ? atoi(argv[2]) : 3;
        if (dsco_banner_render_cells(stdout, loops) <= 0) {
            fprintf(stderr, "dsco-banner: cell render failed (not a tty?)\n");
            return 1;
        }
        return 0;
    }
    if (kitty_banner_render_auto(stdout) > 0)
        return 0;
    if (dsco_banner_render_cells(stdout, 3) > 0)
        return 0;
    fprintf(stderr, "dsco-banner: render failed (not a tty?)\n");
    return 1;
}
