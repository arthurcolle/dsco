/* specfunc.c — minimal special functions */
#include "gsl/gsl_specfunc.h"
#include <math.h>

/* Bessel function of the first kind, order 0 */
double gsl_sf_bessel_J0(double x) {
    return j0(x);
}

/* Gamma function */
double gsl_sf_gamma(double x) {
    return tgamma(x);
}

/* Regularized lower incomplete gamma P(a,x) */
double gsl_sf_gamma_inc_P(double a, double x) {
    (void)a; (void)x;
    return 0.0; /* stub */
}

/* Regularized upper incomplete gamma Q(a,x) */
double gsl_sf_gamma_inc_Q(double a, double x) {
    (void)a; (void)x;
    return 1.0; /* stub */
}
