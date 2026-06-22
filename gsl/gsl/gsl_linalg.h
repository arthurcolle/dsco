/* gsl_linalg.h — Linear Algebra */
#ifndef __GSL_LINALG_H__
#define __GSL_LINALG_H__

#include "gsl_matrix.h"
#include "gsl_vector.h"
#include "gsl_permutation.h"

int gsl_linalg_matmult(const gsl_matrix *A, const gsl_matrix *B, gsl_matrix *C);
int gsl_linalg_matmult_mod(const gsl_matrix *A, gsl_linalg_matrix_mod_t modA,
                           const gsl_matrix *B, gsl_linalg_matrix_mod_t modB, gsl_matrix *C);

int gsl_linalg_LU_decomp(gsl_matrix *A, gsl_permutation *p, int *signum);
int gsl_linalg_LU_solve(const gsl_matrix *LU, const gsl_permutation *p,
                        const gsl_vector *b, gsl_vector *x);
int gsl_linalg_LU_svx(const gsl_matrix *LU, const gsl_permutation *p, gsl_vector *x);
int gsl_linalg_LU_refine(const gsl_matrix *A, const gsl_matrix *LU,
                         const gsl_permutation *p, const gsl_vector *b,
                         gsl_vector *x, gsl_vector *work);
int gsl_linalg_LU_invert(const gsl_matrix *LU, const gsl_permutation *p, gsl_matrix *inverse);
double gsl_linalg_LU_det(gsl_matrix *LU, int signum);
double gsl_linalg_LU_lndet(gsl_matrix *LU);
int gsl_linalg_LU_sgndet(gsl_matrix *lu, int signum);

int gsl_linalg_QR_decomp(gsl_matrix *A, gsl_vector *tau);
int gsl_linalg_QR_solve(const gsl_matrix *QR, const gsl_vector *tau,
                        const gsl_vector *b, gsl_vector *x);
int gsl_linalg_QR_svx(const gsl_matrix *QR, const gsl_vector *tau, gsl_vector *x);
int gsl_linalg_QR_lssolve(const gsl_matrix *QR, const gsl_vector *tau,
                          const gsl_vector *b, gsl_vector *x, gsl_vector *residual);
int gsl_linalg_QR_QRsolve(gsl_matrix *Q, gsl_matrix *R,
                          const gsl_vector *b, gsl_vector *x);
int gsl_linalg_QR_QTvec(const gsl_matrix *QR, const gsl_vector *tau, gsl_vector *v);
int gsl_linalg_QR_Qvec(const gsl_matrix *QR, const gsl_vector *tau, gsl_vector *v);
int gsl_linalg_QR_unpack(const gsl_matrix *QR, const gsl_vector *tau, gsl_matrix *Q, gsl_matrix *R);
int gsl_linalg_QR_QRsolve(gsl_matrix *Q, gsl_matrix *R, const gsl_vector *b, gsl_vector *x);
int gsl_linalg_QR_UR_solve(const gsl_matrix *S, const gsl_matrix *A, const gsl_vector *b, gsl_vector *x);
int gsl_linalg_QR_update(gsl_matrix *Q, gsl_matrix *R, gsl_vector *w, const gsl_vector *v);
int gsl_linalg_QR_QTvec(const gsl_matrix *QR, const gsl_vector *tau, gsl_vector *v);
int gsl_linalg_QR_QTmat(const gsl_matrix *QR, const gsl_vector *tau, gsl_matrix *B);

int gsl_linalg_QRPT_decomp(gsl_matrix *A, gsl_vector *tau, gsl_permutation *p, int *signum, gsl_vector *work);
int gsl_linalg_QRPT_decomp2(const gsl_matrix *A, gsl_matrix *q, gsl_matrix *r, gsl_vector *tau,
                            gsl_permutation *p, int *signum, gsl_vector *work);
int gsl_linalg_QRPT_solve(const gsl_matrix *QR, const gsl_vector *tau, const gsl_permutation *p,
                          const gsl_vector *b, gsl_vector *x);
int gsl_linalg_QRPT_svx(const gsl_matrix *QR, const gsl_vector *tau, const gsl_permutation *p, gsl_vector *x);
int gsl_linalg_QRPT_lssolve(const gsl_matrix *QR, const gsl_vector *tau, const gsl_permutation *p,
                            const gsl_vector *b, gsl_vector *x, gsl_vector *residual);
int gsl_linalg_QRPT_QRsolve(const gsl_matrix *Q, const gsl_matrix *R, const gsl_permutation *p,
                            const gsl_vector *b, gsl_vector *x);
int gsl_linalg_QRPT_update(gsl_matrix *Q, gsl_matrix *R, const gsl_permutation *p, gsl_vector *w, const gsl_vector *v);
int gsl_linalg_QR_UR_lssolve(const gsl_matrix *S, const gsl_matrix *A, const gsl_vector *b, gsl_vector *x, gsl_vector *work);
int gsl_linalg_QR_UR_lssvx(const gsl_matrix *S, const gsl_matrix *A, gsl_vector *x, gsl_vector *work);

int gsl_linalg_SV_decomp(gsl_matrix *A, gsl_matrix *V, gsl_vector *S, gsl_vector *work);
int gsl_linalg_SV_decomp_mod(gsl_matrix *A, gsl_matrix *X, gsl_matrix *V, gsl_vector *S, gsl_vector *work);
int gsl_linalg_SV_decomp_jacobi(gsl_matrix *A, gsl_matrix *Q, gsl_vector *S);
int gsl_linalg_SV_solve(const gsl_matrix *U, const gsl_matrix *Q, const gsl_vector *S,
                        const gsl_vector *b, gsl_vector *x);
int gsl_linalg_SV_lssolve(const gsl_matrix *U, const gsl_matrix *Q, const gsl_vector *S,
                          const gsl_vector *b, gsl_vector *x, gsl_vector *residual);
int gsl_linalg_SV_leverage(const gsl_matrix *U, gsl_vector *h);

int gsl_linalg_cholesky_decomp(gsl_matrix *A);
int gsl_linalg_cholesky_decomp1(gsl_matrix *A);
int gsl_linalg_cholesky_solve(const gsl_matrix *cholesky, const gsl_vector *b, gsl_vector *x);
int gsl_linalg_cholesky_svx(const gsl_matrix *cholesky, gsl_vector *x);
int gsl_linalg_cholesky_invert(gsl_matrix *cholesky);
int gsl_linalg_cholesky_scale(gsl_matrix *d, const gsl_matrix *A);
int gsl_linalg_cholesky_scale_apply(gsl_matrix *A, const gsl_matrix *S);
int gsl_linalg_cholesky_decomp2(gsl_matrix *A, gsl_vector *S);
int gsl_linalg_cholesky_svx2(const gsl_matrix *L, const gsl_vector *S, gsl_vector *x);
int gsl_linalg_cholesky_solve2(const gsl_matrix *L, const gsl_vector *S, const gsl_vector *b, gsl_vector *x);
int gsl_linalg_cholesky_rcond(const gsl_matrix *L, double *rcond, gsl_vector *work);

int gsl_linalg_pcholesky_decomp(gsl_matrix *A, gsl_permutation *p);
int gsl_linalg_pcholesky_solve(const gsl_matrix *pchol, const gsl_permutation *p,
                               const gsl_vector *b, gsl_vector *x);
int gsl_linalg_pcholesky_svx(const gsl_matrix *pchol, const gsl_permutation *p, gsl_vector *x);
int gsl_linalg_pcholesky_decomp2(gsl_matrix *A, gsl_permutation *p, gsl_vector *S);
int gsl_linalg_pcholesky_solve2(const gsl_matrix *pchol, const gsl_permutation *p,
                                const gsl_vector *S, const gsl_vector *b, gsl_vector *x);
int gsl_linalg_pcholesky_svx2(const gsl_matrix *pchol, const gsl_permutation *p,
                              const gsl_vector *S, gsl_vector *x);
int gsl_linalg_pcholesky_invert(const gsl_matrix *pchol, const gsl_permutation *p, gsl_matrix *inverse);
int gsl_linalg_pcholesky_rcond(const gsl_matrix *pchol, const gsl_permutation *p,
                               double *rcond, gsl_vector *work);

int gsl_linalg_mcholesky_decomp(gsl_matrix *A, gsl_permutation *p, gsl_vector *E);
int gsl_linalg_mcholesky_solve(const gsl_matrix *mchol, const gsl_permutation *p,
                               const gsl_vector *b, gsl_vector *x);
int gsl_linalg_mcholesky_svx(const gsl_matrix *mchol, const gsl_permutation *p, gsl_vector *x);
int gsl_linalg_mcholesky_rcond(const gsl_matrix *mchol, const gsl_permutation *p,
                               double *rcond, gsl_vector *work);
int gsl_linalg_mcholesky_invert(const gsl_matrix *mchol, const gsl_permutation *p, gsl_matrix *inverse);

int gsl_linalg_cholesky_band_decomp(gsl_matrix *A);
int gsl_linalg_cholesky_band_solve(const gsl_matrix *L, const gsl_vector *b, gsl_vector *x);
int gsl_linalg_cholesky_band_svx(const gsl_matrix *L, gsl_vector *x);
int gsl_linalg_cholesky_band_invert(const gsl_matrix *L, gsl_matrix *Linv);
int gsl_linalg_cholesky_band_unpack(const gsl_matrix *L, gsl_matrix *L_full);
int gsl_linalg_cholesky_band_scale(gsl_matrix *S, const gsl_matrix *A);
int gsl_linalg_cholesky_band_scale_apply(gsl_matrix *A, const gsl_matrix *S);
int gsl_linalg_cholesky_band_rcond(const gsl_matrix *L, double *rcond, gsl_vector *work);

int gsl_linalg_ldlt_decomp(gsl_matrix *A);
int gsl_linalg_ldlt_solve(const gsl_matrix *LDLT, const gsl_vector *b, gsl_vector *x);
int gsl_linalg_ldlt_svx(const gsl_matrix *LDLT, gsl_vector *x);
int gsl_linalg_ldlt_rcond(const gsl_matrix *LDLT, double *rcond, gsl_vector *work);
int gsl_linalg_ldlt_invert(const gsl_matrix *LDLT, gsl_matrix *Ainv);

int gsl_linalg_ldlt_band_decomp(gsl_matrix *A);
int gsl_linalg_ldlt_band_solve(const gsl_matrix *LDLT, const gsl_vector *b, gsl_vector *x);
int gsl_linalg_ldlt_band_svx(const gsl_matrix *LDLT, gsl_vector *x);
int gsl_linalg_ldlt_band_rcond(const gsl_matrix *LDLT, double *rcond, gsl_vector *work);
int gsl_linalg_ldlt_band_unpack(const gsl_matrix *LDLT, gsl_matrix *L, gsl_vector *D);
int gsl_linalg_ldlt_band_scale(gsl_matrix *S, const gsl_matrix *A);
int gsl_linalg_ldlt_band_scale_apply(gsl_matrix *A, const gsl_matrix *S);

int gsl_linalg_cholesky_decomp_unit(gsl_matrix *A, gsl_vector *D);

int gsl_linalg_complex_cholesky_decomp(gsl_matrix_complex *A);
int gsl_linalg_complex_cholesky_solve(const gsl_matrix_complex *cholesky,
                                      const gsl_vector_complex *b, gsl_vector_complex *x);
int gsl_linalg_complex_cholesky_svx(const gsl_matrix_complex *cholesky, gsl_vector_complex *x);
int gsl_linalg_complex_cholesky_invert(gsl_matrix_complex *cholesky);
int gsl_linalg_complex_cholesky_rcond(const gsl_matrix_complex *L, double *rcond, gsl_vector_complex *work);

int gsl_linalg_hesstri_decomp(gsl_matrix *A, gsl_matrix *B, gsl_matrix *U, gsl_matrix *V, gsl_vector *work);
int gsl_linalg_hesstri_decomp1(gsl_matrix *A, gsl_matrix *B, gsl_matrix *U, gsl_matrix *V, gsl_vector *work);

int gsl_linalg_SV_decomp_jacobi(gsl_matrix *A, gsl_matrix *Q, gsl_vector *S);
int gsl_linalg_SV_decomp_mod(gsl_matrix *A, gsl_matrix *X, gsl_matrix *V, gsl_vector *S, gsl_vector *work);
int gsl_linalg_SV_decomp(gsl_matrix *A, gsl_matrix *V, gsl_vector *S, gsl_vector *work);

int gsl_linalg_balance_matrix(gsl_matrix *A, gsl_vector *D);
int gsl_linalg_balance_accum(gsl_matrix *A, gsl_vector *D);
int gsl_linalg_balance_columns(gsl_matrix *A, gsl_vector *D);

#endif /* __GSL_LINALG_H__ */