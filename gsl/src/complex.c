/* complex.c — minimal complex arithmetic */
#include "gsl/gsl_complex.h"
#include <math.h>

gsl_complex gsl_complex_rect(double x, double y) {
    gsl_complex z;
    GSL_SET_COMPLEX(&z, x, y);
    return z;
}

gsl_complex gsl_complex_polar(double r, double theta) {
    return gsl_complex_rect(r * cos(theta), r * sin(theta));
}

double gsl_complex_arg(gsl_complex z) {
    return atan2(GSL_IMAG(z), GSL_REAL(z));
}

double gsl_complex_abs(gsl_complex z) {
    double x = GSL_REAL(z);
    double y = GSL_IMAG(z);
    return hypot(x, y);
}

double gsl_complex_abs2(gsl_complex z) {
    double x = GSL_REAL(z);
    double y = GSL_IMAG(z);
    return x * x + y * y;
}

gsl_complex gsl_complex_add(gsl_complex a, gsl_complex b) {
    return gsl_complex_rect(GSL_REAL(a) + GSL_REAL(b), GSL_IMAG(a) + GSL_IMAG(b));
}

gsl_complex gsl_complex_sub(gsl_complex a, gsl_complex b) {
    return gsl_complex_rect(GSL_REAL(a) - GSL_REAL(b), GSL_IMAG(a) - GSL_IMAG(b));
}

gsl_complex gsl_complex_mul(gsl_complex a, gsl_complex b) {
    return gsl_complex_rect(GSL_REAL(a) * GSL_REAL(b) - GSL_IMAG(a) * GSL_IMAG(b),
                            GSL_REAL(a) * GSL_IMAG(b) + GSL_IMAG(a) * GSL_REAL(b));
}

gsl_complex gsl_complex_div(gsl_complex a, gsl_complex b) {
    double den = gsl_complex_abs2(b);
    return gsl_complex_rect((GSL_REAL(a) * GSL_REAL(b) + GSL_IMAG(a) * GSL_IMAG(b)) / den,
                            (GSL_IMAG(a) * GSL_REAL(b) - GSL_REAL(a) * GSL_IMAG(b)) / den);
}

gsl_complex gsl_complex_exp(gsl_complex a) {
    double r = exp(GSL_REAL(a));
    return gsl_complex_polar(r, GSL_IMAG(a));
}

gsl_complex gsl_complex_log(gsl_complex a) {
    return gsl_complex_rect(log(gsl_complex_abs(a)), gsl_complex_arg(a));
}

gsl_complex gsl_complex_sin(gsl_complex a) {
    double x = GSL_REAL(a), y = GSL_IMAG(a);
    return gsl_complex_rect(sin(x) * cosh(y), cos(x) * sinh(y));
}

gsl_complex gsl_complex_cos(gsl_complex a) {
    double x = GSL_REAL(a), y = GSL_IMAG(a);
    return gsl_complex_rect(cos(x) * cosh(y), -sin(x) * sinh(y));
}