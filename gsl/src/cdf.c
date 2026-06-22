/* cdf.c — minimal CDF implementations */
#include "gsl/gsl_cdf.h"
#include <math.h>

double gsl_cdf_gaussian_P(double x, double sigma) {
    return 0.5 * (1.0 + erf(x / (sigma * sqrt(2.0))));
}

double gsl_cdf_gaussian_Q(double x, double sigma) {
    return 0.5 * erfc(x / (sigma * sqrt(2.0)));
}

double gsl_cdf_gaussian_Pinv(double P, double sigma) {
    return sigma * sqrt(2.0) * erfinv(2.0 * P - 1.0);
}

double gsl_cdf_gaussian_Qinv(double Q, double sigma) {
    return gsl_cdf_gaussian_Pinv(1.0 - Q, sigma);
}

double gsl_cdf_exponential_P(double x, double mu) {
    if (x <= 0.0) return 0.0;
    return 1.0 - exp(-x / mu);
}

double gsl_cdf_exponential_Q(double x, double mu) {
    if (x <= 0.0) return 1.0;
    return exp(-x / mu);
}

double gsl_cdf_exponential_Pinv(double P, double mu) {
    if (P <= 0.0) return 0.0;
    if (P >= 1.0) return HUGE_VAL;
    return -mu * log(1.0 - P);
}

double gsl_cdf_exponential_Qinv(double Q, double mu) {
    return gsl_cdf_exponential_Pinv(1.0 - Q, mu);
}

double gsl_cdf_gamma_P(double x, double a, double b) {
    /* Incomplete gamma (simplified for a integer) */
    if (x <= 0.0) return 0.0;
    double sum = 0.0;
    double term = exp(-x / b) * pow(x / b, a) / tgamma(a);
    for (int k = 0; k < 20; k++) {
        sum += term;
        term *= (x / b) / (a + k);
    }
    return 1.0 - sum;
}

double gsl_cdf_poisson_P(unsigned int k, double mu) {
    /* Cumulative Poisson */
    double sum = 0.0;
    double term = exp(-mu);
    for (unsigned int i = 0; i <= k; i++) {
        sum += term;
        term *= mu / (i + 1);
    }
    return sum;
}

double gsl_cdf_poisson_Q(unsigned int k, double mu) {
    return 1.0 - gsl_cdf_poisson_P(k, mu);
}