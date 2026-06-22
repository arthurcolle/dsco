/* sort.c — minimal sorting */
#include "gsl/gsl_sort.h"
#include <stdlib.h>

static int cmp_double(const void *a, const void *b) {
    double da = *(const double *)a;
    double db = *(const double *)b;
    return (da > db) - (da < db);
}

void gsl_sort(double *data, size_t stride, size_t n) {
    if (stride == 1) {
        qsort(data, n, sizeof(double), cmp_double);
    } else {
        /* Strided sort: copy, sort, copy back */
        double *tmp = malloc(n * sizeof(double));
        for (size_t i = 0; i < n; i++) tmp[i] = data[i * stride];
        qsort(tmp, n, sizeof(double), cmp_double);
        for (size_t i = 0; i < n; i++) data[i * stride] = tmp[i];
        free(tmp);
    }
}

void gsl_sort_index(size_t *p, const double *data, size_t stride, size_t n) {
    /* Simple selection sort for indices */
    for (size_t i = 0; i < n; i++) p[i] = i;
    for (size_t i = 0; i < n - 1; i++) {
        size_t min = i;
        for (size_t j = i + 1; j < n; j++) {
            if (data[p[j] * stride] < data[p[min] * stride]) min = j;
        }
        size_t tmp = p[i];
        p[i] = p[min];
        p[min] = tmp;
    }
}