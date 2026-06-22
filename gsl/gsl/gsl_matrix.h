#include <stddef.h>
/* gsl_matrix.h — minimal matrix interface */
#ifndef __GSL_MATRIX_H__
#define __GSL_MATRIX_H__

#include <stdlib.h>

typedef struct {
    size_t size1;
    size_t size2;
    size_t tda;
    double *data;
    void *block;
    int owner;
} gsl_matrix;

gsl_matrix *gsl_matrix_alloc(size_t n1, size_t n2);
void gsl_matrix_free(gsl_matrix *m);
void gsl_matrix_set_zero(gsl_matrix *m);
void gsl_matrix_set_identity(gsl_matrix *m);
void gsl_matrix_set_all(gsl_matrix *m, double x);

double gsl_matrix_get(const gsl_matrix *m, size_t i, size_t j);
void gsl_matrix_set(gsl_matrix *m, size_t i, size_t j, double x);

#endif /* __GSL_MATRIX_H__ */