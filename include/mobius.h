#ifndef DSCO_MOBIUS_H
#define DSCO_MOBIUS_H
#include <stdbool.h>
#include <stdint.h>
/* Fundamental embedding: u in [0,2pi], v in [-half_width,half_width].
 * Seam identification is (2pi,v) == (0,-v), not (0,v). */
void mobius_point(double u, double v, double out[3]);
/* RGB24, white background. Dimensions 32..2048, half_width 0.05..0.8.
 * angle is a finite camera rotation in radians. Caller owns width*height*3 bytes. */
bool mobius_render(uint8_t *rgb, int width, int height,
                   double half_width, double angle);
#endif
