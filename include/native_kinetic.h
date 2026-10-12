#ifndef DSCO_NATIVE_KINETIC_H
#define DSCO_NATIVE_KINETIC_H
/* Allocation-free procedural shader; footprint is normalized pixel width. */
typedef struct { double energy, filament, spectral; } native_kinetic_sample_t;
native_kinetic_sample_t native_kinetic_sample(double position, double phase, double footprint);
double native_kinetic_energy(double position, double phase);
#endif
