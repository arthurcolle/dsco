/* blas.c — minimal BLAS */
#include "gsl/gsl_blas.h"
#include <string.h>

int gsl_blas_ddot(const gsl_vector *X, const gsl_vector *Y, double *result) {
    if (X->size != Y->size) return -1;
    double sum = 0.0;
    for (size_t i = 0; i < X->size; i++) {
        sum += X->data[i] * Y->data[i];
    }
    *result = sum;
    return 0;
}

int gsl_blas_dgemv(CBLAS_TRANSPOSE_t TransA, double alpha, const gsl_matrix *A,
                   const gsl_vector *X, double beta, gsl_vector *Y) {
    (void)TransA; (void)alpha; (void)beta;
    /* Simplified: Y = A * X */
    if (A->size2 != X->size || A->size1 != Y->size) return -1;
    for (size_t i = 0; i < A->size1; i++) {
        double sum = 0.0;
        for (size_t j = 0; j < A->size2; j++) {
            sum += A->data[i * A->tda + j] * X->data[j];
        }
        Y->data[i] = sum;
    }
    return 0;
}