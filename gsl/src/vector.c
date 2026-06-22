/* vector.c — minimal vector */
#include "gsl/gsl_vector.h"
#include <stdlib.h>
#include <string.h>

gsl_vector *gsl_vector_alloc(size_t n) {
    gsl_vector *v = malloc(sizeof(gsl_vector));
    if (!v) return NULL;
    v->size = n;
    v->stride = 1;
    v->data = calloc(n, sizeof(double));
    v->block = NULL;
    v->owner = 1;
    return v;
}

void gsl_vector_free(gsl_vector *v) {
    if (v) {
        free(v->data);
        free(v);
    }
}

void gsl_vector_set_zero(gsl_vector *v) {
    memset(v->data, 0, v->size * sizeof(double));
}

void gsl_vector_set_all(gsl_vector *v, double x) {
    for (size_t i = 0; i < v->size; i++) v->data[i] = x;
}

double gsl_vector_get(const gsl_vector *v, size_t i) {
    return (i < v->size) ? v->data[i * v->stride] : 0.0;
}

void gsl_vector_set(gsl_vector *v, size_t i, double x) {
    if (i < v->size) v->data[i * v->stride] = x;
}