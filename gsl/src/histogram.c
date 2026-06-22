#include <math.h>
/* histogram.c — minimal histogram */
#include "gsl/gsl_histogram.h"
#include <stdlib.h>
#include <string.h>

gsl_histogram *gsl_histogram_alloc(size_t n) {
    gsl_histogram *h = malloc(sizeof(gsl_histogram));
    if (!h) return NULL;
    h->n = n;
    h->range = malloc((n + 1) * sizeof(double));
    h->bin = calloc(n, sizeof(double));
    return h;
}

void gsl_histogram_free(gsl_histogram *h) {
    if (h) {
        free(h->range);
        free(h->bin);
        free(h);
    }
}

int gsl_histogram_set_ranges_uniform(gsl_histogram *h, double xmin, double xmax) {
    if (xmin >= xmax || h->n == 0) return -1;
    double dx = (xmax - xmin) / h->n;
    for (size_t i = 0; i <= h->n; i++) {
        h->range[i] = xmin + i * dx;
    }
    return 0;
}

int gsl_histogram_increment(gsl_histogram *h, double x) {
    for (size_t i = 0; i < h->n; i++) {
        if (x >= h->range[i] && x < h->range[i + 1]) {
            h->bin[i] += 1.0;
            return 0;
        }
    }
    return -1;
}

double gsl_histogram_get(const gsl_histogram *h, size_t i) {
    return (i < h->n) ? h->bin[i] : 0.0;
}

double gsl_histogram_mean(const gsl_histogram *h) {
    double sum = 0.0, wsum = 0.0;
    for (size_t i = 0; i < h->n; i++) {
        double mid = 0.5 * (h->range[i] + h->range[i + 1]);
        sum += h->bin[i] * mid;
        wsum += h->bin[i];
    }
    return wsum > 0 ? sum / wsum : 0.0;
}

double gsl_histogram_sigma(const gsl_histogram *h) {
    double mean = gsl_histogram_mean(h);
    double sum = 0.0, wsum = 0.0;
    for (size_t i = 0; i < h->n; i++) {
        double mid = 0.5 * (h->range[i] + h->range[i + 1]);
        double d = mid - mean;
        sum += h->bin[i] * d * d;
        wsum += h->bin[i];
    }
    return wsum > 0 ? sqrt(sum / wsum) : 0.0;
}