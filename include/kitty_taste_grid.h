#ifndef DSCO_KITTY_TASTE_GRID_H
#define DSCO_KITTY_TASTE_GRID_H

#include <stdbool.h>
#include <stdio.h>

/* Eight deliberately different DSCO visual directions rendered into one
 * deterministic 4x2 RGB comparison board.  The same pixels can be written
 * headlessly or uploaded directly through the Kitty graphics protocol. */
bool kitty_taste_grid_write_ppm(const char *path, int width, int height);
bool kitty_taste_grid_render(FILE *out, int width, int height);
void kitty_taste_grid_clear(FILE *out);

#endif /* DSCO_KITTY_TASTE_GRID_H */
