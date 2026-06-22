/* extension/numerical_backend.h — Numerical Backend Interface */
#ifndef DSCO_EXTENSION_NUMERICAL_BACKEND_H
#define DSCO_EXTENSION_NUMERICAL_BACKEND_H

#include "backend.h"
#include "gsl/gsl_vector.h"
#include "gsl/gsl_matrix.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Numerical backend vtable */
typedef struct numerical_backend {
    backend_interface_t base;

    /* Linear algebra */
    int (*matrix_alloc)(size_t rows, size_t cols, gsl_matrix **out);
    int (*matrix_free)(gsl_matrix *m);
    int (*gemm)(const gsl_matrix *A, const gsl_matrix *B, gsl_matrix *C,
                double alpha, double beta);

    /* FFT */
    int (*fft_complex_forward)(double *data, size_t n);

    /* Statistics */
    double (*mean)(const double *data, size_t n);
    double (*variance)(const double *data, size_t n);
    double (*correlation)(const double *x, const double *y, size_t n);

    /* Distributions */
    double (*gaussian_sample)(double mu, double sigma);
    double (*gaussian_pdf)(double x, double mu, double sigma);
    double (*gaussian_cdf)(double x, double mu, double sigma);

    /* Optimization */
    int (*optimize)(double (*f)(const double *x, size_t n),
                    double *x, size_t n, int max_iter);

    /* Special functions */
    double (*bessel_J0)(double x);
    double (*gamma)(double x);
    double (*erf)(double x);
} numerical_backend_t;

/* Default GSL implementation */
extern numerical_backend_t numerical_backend_gsl;

/* Registration */
int numerical_backend_register(numerical_backend_t *impl);

#ifdef __cplusplus
}
#endif
#endif /* DSCO_EXTENSION_NUMERICAL_BACKEND_H */