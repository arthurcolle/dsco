#include <stddef.h>
/* gsl_blas.h — minimal BLAS interface */
#ifndef __GSL_BLAS_H__
#define __GSL_BLAS_H__

#include "gsl_matrix.h"
#include "gsl_vector.h"
#include "gsl_cblas.h"

typedef CBLAS_TRANSPOSE CBLAS_TRANSPOSE_t;

int gsl_blas_ddot(const gsl_vector *X, const gsl_vector *Y, double *result);
int gsl_blas_dgemv(CBLAS_TRANSPOSE_t TransA, double alpha, const gsl_matrix *A,
                   const gsl_vector *X, double beta, gsl_vector *Y);
int gsl_blas_dgemm(CBLAS_TRANSPOSE_t TransA, CBLAS_TRANSPOSE_t TransB,
                   double alpha, const gsl_matrix *A, const gsl_matrix *B,
                   double beta, gsl_matrix *C);

#endif /* __GSL_BLAS_H__ */