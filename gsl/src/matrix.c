/* matrix.c — minimal matrix */
#include "gsl/gsl_matrix.h"
#include <stdlib.h>
#include <string.h>

gsl_matrix *gsl_matrix_alloc(size_t n1, size_t n2) {
    gsl_matrix *m = malloc(sizeof(gsl_matrix));
    if (!m) return NULL;
    m->size1 = n1;
    m->size2 = n2;
    m->tda = n2;
    m->data = calloc(n1 * n2, sizeof(double));
    m->block = NULL;
    m->owner = 1;
    return m;
}

void gsl_matrix_free(gsl_matrix *m) {
    if (m) {
        free(m->data);
        free(m);
    }
}

void gsl_matrix_set_zero(gsl_matrix *m) {
    memset(m->data, 0, m->size1 * m->size2 * sizeof(double));
}

void gsl_matrix_set_identity(gsl_matrix *m) {
    gsl_matrix_set_zero(m);
    size_t n = m->size1 < m->size2 ? m->size1 : m->size2;
    for (size_t i = 0; i < n; i++) {
        m->data[i * m->tda + i] = 1.0;
    }
}

double gsl_matrix_get(const gsl_matrix *m, size_t i, size_t j) {
    return (i < m->size1 && j < m->size2) ? m->data[i * m->tda + j] : 0.0;
}

void gsl_matrix_set(gsl_matrix *m, size_t i, size_t j, double x) {
    if (i < m->size1 && j < m->size2) {
        m->data[i * m->tda + j] = x;
    }
}