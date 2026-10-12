#include "native_kinetic.h"
#include <math.h>
#define TAU 6.28318530717958647692
static double sat(double x) { return fmax(0.0, fmin(1.0, x)); }
native_kinetic_sample_t native_kinetic_sample(double x, double phase, double footprint) {
    native_kinetic_sample_t s = {0};
    if (!isfinite(x) || !isfinite(phase) || !isfinite(footprint)) return s;
    x = x - floor(x); phase = phase - floor(phase);
    footprint = sat(fabs(footprint));
    /* Periodic domain warping: coherent flow rather than independent blinking. */
    double warp = x + .065 * sin(TAU * (2*x - phase))
                    + .025 * sin(TAU * (5*x + 2*phase));
    double carrier = .5 + .5*cos(TAU*(warp-phase));
    double braid = .5 + .5*sin(TAU*(3*warp+2*phase));
    /* Analytic footprint attenuation suppresses subpixel interference. */
    double detail = 1.0 / (1.0 + 144.0*footprint*footprint);
    double ridge = fabs(sin(TAU*(6*warp-3*phase)));
    double width = .09 + fmin(.35, footprint*18.0);
    double line = sat(1.0-ridge/width);
    line = line*line*(3.0-2.0*line);
    s.filament = line*detail*carrier;
    s.energy = sat(carrier*carrier*(.22+.58*braid*braid) + .20*s.filament);
    s.spectral = .5+.5*sin(TAU*(warp+phase));
    return s;
}
double native_kinetic_energy(double x, double phase) {
    return native_kinetic_sample(x, phase, 0).energy;
}
