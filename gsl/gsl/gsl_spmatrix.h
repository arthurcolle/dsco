/* gsl_spmatrix.h — Sparse Matrices */
#ifndef __GSL_SPMATRIX_H__
#define __GSL_SPMATRIX_H__

#include <stdlib.h>

typedef struct {
    size_t size1;
    size_t size2;
    size_t *i;
    size_t *p;
    double *data;
    size_t nz;
    size_t nzmax;
} gsl_spmatrix;

gsl_spmatrix *gsl_spmatrix_alloc(size_t n1, size_t n2);
void gsl_spmatrix_free(gsl_spmatrix *m);
int gsl_spmatrix_set_zero(gsl_spmatrix *m);
int gsl_spmatrix_set(gsl_spmatrix *m, size_t i, size_t j, double x);
double gsl_spmatrix_get(const gsl_spmatrix *m, size_t i, size_t j);
int gsl_spmatrix_scale(gsl_spmatrix *m, double x);
int gsl_spmatrix_add(gsl_spmatrix *c, const gsl_spmatrix *a, const gsl_spmatrix *b);
int gsl_spmatrix_scale_rows(gsl_spmatrix *m, const gsl_vector *x);
int gsl_spmatrix_scale_columns(gsl_spmatrix *m, const gsl_vector *x);

#endif /* __GSL_SPMATRIX_H__ */