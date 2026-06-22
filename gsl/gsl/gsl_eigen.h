/* gsl_eigen.h — Eigensystems */
#ifndef __GSL_EIGEN_H__
#define __GSL_EIGEN_H__

#include "gsl_matrix.h"
#include "gsl_vector.h"

typedef struct {
    size_t size;
    double *d, *sd;
} gsl_eigen_symm_workspace;

gsl_eigen_symm_workspace *gsl_eigen_symm_alloc(size_t n);
void gsl_eigen_symm_free(gsl_eigen_symm_workspace *w);
int gsl_eigen_symm(gsl_matrix *A, gsl_vector *eval, gsl_eigen_symm_workspace *w);

typedef struct {
    size_t size;
    double *d, *sd;
    double *gc, *gs;
} gsl_eigen_symmv_workspace;

gsl_eigen_symmv_workspace *gsl_eigen_symmv_alloc(size_t n);
void gsl_eigen_symmv_free(gsl_eigen_symmv_workspace *w);
int gsl_eigen_symmv(gsl_matrix *A, gsl_vector *eval, gsl_matrix *evec, gsl_eigen_symmv_workspace *w);

typedef struct {
    size_t size;
    double *d, *sd, *tau;
} gsl_eigen_herm_workspace;

gsl_eigen_herm_workspace *gsl_eigen_herm_alloc(size_t n);
void gsl_eigen_herm_free(gsl_eigen_herm_workspace *w);
int gsl_eigen_herm(gsl_matrix_complex *A, gsl_vector *eval, gsl_eigen_herm_workspace *w);

typedef struct {
    size_t size;
    double *d, *sd, *tau;
    double *gc, *gs;
} gsl_eigen_hermv_workspace;

gsl_eigen_hermv_workspace *gsl_eigen_hermv_alloc(size_t n);
void gsl_eigen_hermv_free(gsl_eigen_hermv_workspace *w);
int gsl_eigen_hermv(gsl_matrix_complex *A, gsl_vector *eval, gsl_matrix_complex *evec,
                    gsl_eigen_hermv_workspace *w);

typedef struct {
    size_t size;
    double *diag, *superdiag;
    double *tau;
} gsl_eigen_francis_workspace;

gsl_eigen_francis_workspace *gsl_eigen_francis_alloc(size_t n);
void gsl_eigen_francis_free(gsl_eigen_francis_workspace *w);
void gsl_eigen_francis_T(gsl_eigen_francis_workspace *w);
int gsl_eigen_francis(gsl_matrix *H, gsl_vector_complex *eval, gsl_eigen_francis_workspace *w);
int gsl_eigen_francis_Z(gsl_matrix *H, gsl_vector_complex *eval, gsl_matrix *Z,
                        gsl_eigen_francis_workspace *w);

typedef struct {
    size_t size;
    gsl_matrix *H, *R;
    gsl_vector_complex *tau;
    gsl_vector *diag, *subdiag;
    gsl_eigen_francis_workspace *francis_workspace_p;
} gsl_eigen_nonsymm_workspace;

gsl_eigen_nonsymm_workspace *gsl_eigen_nonsymm_alloc(size_t n);
void gsl_eigen_nonsymm_free(gsl_eigen_nonsymm_workspace *w);
void gsl_eigen_nonsymm_params(int compute_t, int balance, gsl_eigen_nonsymm_workspace *w);
int gsl_eigen_nonsymm(gsl_matrix *A, gsl_vector_complex *eval, gsl_eigen_nonsymm_workspace *w);
int gsl_eigen_nonsymm_Z(gsl_matrix *A, gsl_vector_complex *eval, gsl_matrix *Z,
                        gsl_eigen_nonsymm_workspace *w);

typedef struct {
    size_t size;
    gsl_matrix *H, *R;
    gsl_vector_complex *tau;
    gsl_vector *diag, *subdiag;
    gsl_matrix_complex *Z;
    gsl_eigen_nonsymm_workspace *nonsymm_workspace_p;
} gsl_eigen_nonsymmv_workspace;

gsl_eigen_nonsymmv_workspace *gsl_eigen_nonsymmv_alloc(size_t n);
void gsl_eigen_nonsymmv_free(gsl_eigen_nonsymmv_workspace *w);
void gsl_eigen_nonsymmv_params(int balance, gsl_eigen_nonsymmv_workspace *w);
int gsl_eigen_nonsymmv(gsl_matrix *A, gsl_vector_complex *eval, gsl_matrix_complex *evec,
                       gsl_eigen_nonsymmv_workspace *w);
int gsl_eigen_nonsymmv_Z(gsl_matrix *A, gsl_vector_complex *eval, gsl_matrix_complex *evec,
                         gsl_matrix *Z, gsl_eigen_nonsymmv_workspace *w);

typedef struct {
    size_t size;
    double *diag, *subdiag;
    double *tau;
    size_t k;
    size_t first_row;
    size_t computed;
    gsl_matrix *H;
    gsl_eigen_symmv_workspace *symmv_workspace_p;
} gsl_eigen_gensymm_workspace;

gsl_eigen_gensymm_workspace *gsl_eigen_gensymm_alloc(size_t n);
void gsl_eigen_gensymm_free(gsl_eigen_gensymm_workspace *w);
int gsl_eigen_gensymm(gsl_matrix *A, gsl_matrix *B, gsl_vector *eval, gsl_eigen_gensymm_workspace *w);
int gsl_eigen_gensymm_standardize(gsl_matrix *A, const gsl_matrix *B);

typedef struct {
    size_t size;
    double *diag, *subdiag;
    double *tau;
    size_t k;
    size_t first_row;
    size_t computed;
    gsl_matrix *H;
    gsl_matrix *R;
    gsl_eigen_symmv_workspace *symmv_workspace_p;
} gsl_eigen_gensymmv_workspace;

gsl_eigen_gensymmv_workspace *gsl_eigen_gensymmv_alloc(size_t n);
void gsl_eigen_gensymmv_free(gsl_eigen_gensymmv_workspace *w);
int gsl_eigen_gensymmv(gsl_matrix *A, gsl_matrix *B, gsl_vector *eval, gsl_matrix *evec,
                       gsl_eigen_gensymmv_workspace *w);

typedef struct {
    size_t size;
    gsl_matrix_complex *H;
    gsl_matrix_complex *R;
    gsl_vector_complex *tau;
    size_t k;
    size_t first_row;
    size_t computed;
    gsl_eigen_herm_workspace *herm_workspace_p;
} gsl_eigen_genherm_workspace;

gsl_eigen_genherm_workspace *gsl_eigen_genherm_alloc(size_t n);
void gsl_eigen_genherm_free(gsl_eigen_genherm_workspace *w);
int gsl_eigen_genherm(gsl_matrix_complex *A, gsl_matrix_complex *B, gsl_vector *eval,
                      gsl_eigen_genherm_workspace *w);
int gsl_eigen_genherm_standardize(gsl_matrix_complex *A, const gsl_matrix_complex *B);

typedef struct {
    size_t size;
    gsl_matrix_complex *H;
    gsl_matrix_complex *R;
    gsl_vector_complex *tau;
    size_t k;
    size_t first_row;
    size_t computed;
    gsl_matrix_complex *Z;
    gsl_eigen_hermv_workspace *hermv_workspace_p;
} gsl_eigen_genhermv_workspace;

gsl_eigen_genhermv_workspace *gsl_eigen_genhermv_alloc(size_t n);
void gsl_eigen_genhermv_free(gsl_eigen_genhermv_workspace *w);
int gsl_eigen_genhermv(gsl_matrix_complex *A, gsl_matrix_complex *B, gsl_vector *eval,
                       gsl_matrix_complex *evec, gsl_eigen_genhermv_workspace *w);

typedef struct {
    size_t size;
    gsl_matrix *H;
    gsl_matrix *R;
    gsl_vector *tau;
    size_t k;
    size_t first_row;
    size_t computed;
    gsl_eigen_francis_workspace *francis_workspace_p;
} gsl_eigen_gen_workspace;

gsl_eigen_gen_workspace *gsl_eigen_gen_alloc(size_t n);
void gsl_eigen_gen_free(gsl_eigen_gen_workspace *w);
void gsl_eigen_gen_params(int compute_s, int compute_t, int balance, gsl_eigen_gen_workspace *w);
int gsl_eigen_gen(gsl_matrix *A, gsl_matrix *B, gsl_vector_complex *alpha, gsl_vector *beta,
                  gsl_eigen_gen_workspace *w);
int gsl_eigen_gen_QZ(gsl_matrix *A, gsl_matrix *B, gsl_vector_complex *alpha, gsl_vector *beta,
                     gsl_matrix *Q, gsl_matrix *Z, gsl_eigen_gen_workspace *w);

typedef struct {
    size_t size;
    gsl_matrix *H;
    gsl_matrix *R;
    gsl_vector_complex *tau;
    size_t k;
    size_t first_row;
    size_t computed;
    gsl_matrix *Z;
    gsl_eigen_gen_workspace *gen_workspace_p;
} gsl_eigen_genv_workspace;

gsl_eigen_genv_workspace *gsl_eigen_genv_alloc(size_t n);
void gsl_eigen_genv_free(gsl_eigen_genv_workspace *w);
int gsl_eigen_genv(gsl_matrix *A, gsl_matrix *B, gsl_vector_complex *alpha, gsl_vector *beta,
                   gsl_matrix_complex *evec, gsl_eigen_genv_workspace *w);
int gsl_eigen_genv_QZ(gsl_matrix *A, gsl_matrix *B, gsl_vector_complex *alpha, gsl_vector *beta,
                      gsl_matrix_complex *evec, gsl_matrix *Q, gsl_matrix *Z,
                      gsl_eigen_genv_workspace *w);

#endif /* __GSL_EIGEN_H__ */