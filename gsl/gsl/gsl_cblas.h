/* gsl_cblas.h — CBLAS Interface */
#ifndef __GSL_CBLAS_H__
#define __GSL_CBLAS_H__

typedef enum { CblasRowMajor = 101, CblasColMajor = 102 } CBLAS_ORDER;
typedef enum { CblasNoTrans = 111, CblasTrans = 112, CblasConjTrans = 113 } CBLAS_TRANSPOSE;
typedef enum { CblasUpper = 121, CblasLower = 122 } CBLAS_UPLO;
typedef enum { CblasNonUnit = 131, CblasUnit = 132 } CBLAS_DIAG;
typedef enum { CblasLeft = 141, CblasRight = 142 } CBLAS_SIDE;

void cblas_dgemv(CBLAS_ORDER order, CBLAS_TRANSPOSE TransA, int M, int N,
                 double alpha, const double *A, int lda, const double *X, int incX,
                 double beta, double *Y, int incY);
void cblas_dgemm(CBLAS_ORDER order, CBLAS_TRANSPOSE TransA, CBLAS_TRANSPOSE TransB,
                 int M, int N, int K, double alpha, const double *A, int lda,
                 const double *B, int ldb, double beta, double *C, int ldc);
double cblas_ddot(int N, const double *X, int incX, const double *Y, int incY);

#endif /* __GSL_CBLAS_H__ */