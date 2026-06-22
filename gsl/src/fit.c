/* fit.c — minimal linear fitting */
#include "gsl/gsl_fit.h"
#include <math.h>

int gsl_fit_linear(const double x[], size_t xstride,
                   const double y[], size_t ystride,
                   size_t n,
                   double *c0, double *c1,
                   double *cov00, double *cov01, double *cov11,
                   double *chisq) {
    double sum_x = 0.0, sum_y = 0.0, sum_xx = 0.0, sum_xy = 0.0;
    for (size_t i = 0; i < n; i++) {
        double xi = x[i * xstride];
        double yi = y[i * ystride];
        sum_x += xi;
        sum_y += yi;
        sum_xx += xi * xi;
        sum_xy += xi * yi;
    }
    double d = n * sum_xx - sum_x * sum_x;
    *c0 = (sum_xx * sum_y - sum_x * sum_xy) / d;
    *c1 = (n * sum_xy - sum_x * sum_y) / d;

    double sum_res2 = 0.0;
    for (size_t i = 0; i < n; i++) {
        double yi = y[i * ystride];
        double fi = *c0 + *c1 * x[i * xstride];
        double res = yi - fi;
        sum_res2 += res * res;
    }
    *chisq = sum_res2;
    double s2 = sum_res2 / (n - 2);
    *cov00 = s2 * sum_xx / d;
    *cov11 = s2 * n / d;
    *cov01 = -s2 * sum_x / d;
    return 0;
}