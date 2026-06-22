#include <stddef.h>
/* gsl_monte.h — Monte Carlo integration */
#ifndef __GSL_MONTE_H__
#define __GSL_MONTE_H__

#include "gsl_rng.h"

typedef struct {
    size_t dim;
    double *xmin;
    double *xmax;
    double *delx;
    double volume;
} gsl_monte_function;

int gsl_monte_plain_integrate(const gsl_monte_function *f,
                              const double xl[], const double xu[],
                              const size_t dim, const size_t calls,
                              gsl_rng *r,
                              double *result, double *abserr);

typedef struct {
    size_t dim;
    double *xmin;
    double *xmax;
    double *delx;
    double volume;
    double *x;
    int *ix;
} gsl_monte_vegas_state;

gsl_monte_vegas_state *gsl_monte_vegas_alloc(size_t dim);
void gsl_monte_vegas_free(gsl_monte_vegas_state *state);

int gsl_monte_vegas_integrate(const gsl_monte_function *f,
                              const double xl[], const double xu[],
                              size_t dim, size_t calls,
                              gsl_rng *r, gsl_monte_vegas_state *state,
                              double *result, double *abserr);

#endif /* __GSL_MONTE_H__ */