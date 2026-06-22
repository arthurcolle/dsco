/* gsl_spblas.h — Sparse BLAS */
#ifndef __GSL_SPBLAS_H__
#define __GSL_SPBLAS_H__

#include "gsl_spmatrix.h"
#include "gsl_vector.h"

int gsl_spblas_dgemv(const double alpha, const gsl_spmatrix *A, const gsl_vector *x,
                     const double beta, gsl_vector *y);
int gsl_spblas_dgemm(const double alpha, const gsl_spmatrix *A, const gsl_spmatrix *B,
                     const double beta, gsl_spmatrix *C);

#endif /* __GSL_SPBLAS_H__ */