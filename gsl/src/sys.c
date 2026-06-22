/* sys.c — system utilities */
#include "gsl/gsl_sys.h"
#include <math.h>
#include <float.h>

double gsl_log1p(double x) {
    if (fabs(x) < 1e-4) return x - 0.5 * x * x;
    return log(1.0 + x);
}

double gsl_expm1(double x) {
    if (fabs(x) < 1e-4) return x + 0.5 * x * x;
    return exp(x) - 1.0;
}

double gsl_hypot(double x, double y) {
    return hypot(x, y);
}

double gsl_hypot3(double x, double y, double z) {
    return sqrt(x * x + y * y + z * z);
}

int gsl_isnan(double x) {
    return isnan(x);
}

int gsl_isinf(double x) {
    return isinf(x);
}

int gsl_finite(double x) {
    return isfinite(x);
}