#include <stddef.h>
/* gsl_odeiv2.h — ODE solvers */
#ifndef __GSL_ODEIV2_H__
#define __GSL_ODEIV2_H__

typedef struct {
    int (*function)(double t, const double y[], double dydt[], void *params);
    int (*jacobian)(double t, const double y[], double *dfdy, double dfdt[], void *params);
    size_t dimension;
    void *params;
} gsl_odeiv2_system;

typedef struct gsl_odeiv2_driver_struct gsl_odeiv2_driver;

gsl_odeiv2_driver *gsl_odeiv2_driver_alloc_y_new(const gsl_odeiv2_system *sys,
                                                  const gsl_odeiv2_step_type *T,
                                                  double hstart, double epsabs, double epsrel);
void gsl_odeiv2_driver_free(gsl_odeiv2_driver *d);

int gsl_odeiv2_driver_apply(gsl_odeiv2_driver *d, double *t,
                            double t1, double y[]);

int gsl_odeiv2_driver_apply_fixed_step(gsl_odeiv2_driver *d, double *t,
                                       double h, size_t n, double y[]);

int gsl_odeiv2_driver_set_hmin(gsl_odeiv2_driver *d, double hmin);
int gsl_odeiv2_driver_set_hmax(gsl_odeiv2_driver *d, double hmax);
int gsl_odeiv2_driver_set_nmax(gsl_odeiv2_driver *d, size_t nmax);

#endif /* __GSL_ODEIV2_H__ */