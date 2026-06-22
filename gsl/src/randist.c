/* randist.c — minimal distribution sampling */
#include "gsl/gsl_randist.h"
#include "gsl/gsl_rng.h"
#include <math.h>

double gsl_ran_gaussian(const gsl_rng *r, double sigma) {
    /* Box-Muller */
    double u1 = gsl_rng_uniform(r);
    double u2 = gsl_rng_uniform(r);
    return sigma * sqrt(-2.0 * log(u1)) * cos(2.0 * M_PI * u2);
}

double gsl_ran_gaussian_pdf(double x, double sigma) {
    double u = x / sigma;
    return (1.0 / (sigma * sqrt(2.0 * M_PI))) * exp(-0.5 * u * u);
}

double gsl_ran_exponential(const gsl_rng *r, double mu) {
    return -mu * log(gsl_rng_uniform(r));
}

double gsl_ran_exponential_pdf(double x, double mu) {
    if (x < 0) return 0.0;
    return (1.0 / mu) * exp(-x / mu);
}

double gsl_ran_gamma(const gsl_rng *r, double a, double b) {
    /* Marsaglia & Tsang method (simplified) */
    if (a < 1.0) {
        return gsl_ran_gamma(r, a + 1.0, b) * pow(gsl_rng_uniform(r), 1.0 / a);
    }
    double d = a - 1.0 / 3.0;
    double c = 1.0 / sqrt(9.0 * d);
    for (;;) {
        double x = gsl_ran_gaussian(r, 1.0);
        double v = 1.0 + c * x;
        if (v <= 0.0) continue;
        v = v * v * v;
        double u = gsl_rng_uniform(r);
        if (u < 1.0 - 0.0331 * x * x * x * x) return b * d * v;
        if (log(u) < 0.5 * x * x + d * (1.0 - v + log(v))) return b * d * v;
    }
}

double gsl_ran_poisson(const gsl_rng *r, double mu) {
    if (mu < 10.0) {
        double L = exp(-mu);
        int k = 0;
        double p = 1.0;
        do {
            k++;
            p *= gsl_rng_uniform(r);
        } while (p > L);
        return k - 1;
    }
    /* For larger mu, use normal approximation */
    double x = gsl_ran_gaussian(r, sqrt(mu)) + mu;
    return x > 0 ? (unsigned int)x : 0;
}

double gsl_ran_binomial(const gsl_rng *r, double p, unsigned int n) {
    unsigned int k = 0;
    for (unsigned int i = 0; i < n; i++) {
        if (gsl_rng_uniform(r) < p) k++;
    }
    return k;
}