/* src/extension/backend_selftest.c — Backend smoke tests */
#include "extension/numerical_backend.h"
#include <stdio.h>

int backend_selftest_numerical(void) {
    backend_interface_t *b = backend_get("gsl", BACKEND_NUMERICAL);
    if (!b) {
        fprintf(stderr, "[backend_selftest] gsl backend not registered\n");
        return -1;
    }
    numerical_backend_t *nb = (numerical_backend_t *)b;
    double xs[4] = {1.0, 2.0, 3.0, 4.0};
    double ys[4] = {2.0, 4.0, 6.0, 8.0};
    double mean = nb->mean(xs, 4);
    double corr = nb->correlation(xs, ys, 4);
    if (mean < 2.49 || mean > 2.51) return -2;
    if (corr < 0.999) return -3;
    return 0;
}