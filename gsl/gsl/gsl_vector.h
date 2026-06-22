#include <stddef.h>
/* gsl_vector.h — minimal vector interface */
#ifndef __GSL_VECTOR_H__
#define __GSL_VECTOR_H__

#include <stdlib.h>

typedef struct {
    size_t size;
    size_t stride;
    double *data;
    void *block;
    int owner;
} gsl_vector;

gsl_vector *gsl_vector_alloc(size_t n);
void gsl_vector_free(gsl_vector *v);
void gsl_vector_set_zero(gsl_vector *v);
void gsl_vector_set_all(gsl_vector *v, double x);

double gsl_vector_get(const gsl_vector *v, size_t i);
void gsl_vector_set(gsl_vector *v, size_t i, double x);

#endif /* __GSL_VECTOR_H__ */