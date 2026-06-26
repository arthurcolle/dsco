/* src/extension/numerical_gsl.c — GSL Numerical Backend Implementation */
#include "extension/numerical_backend.h"
#include "gsl/gsl_vector.h"
#include "gsl/gsl_matrix.h"
#include "gsl/gsl_blas.h"
#include "gsl/gsl_statistics.h"
#include "gsl/gsl_randist.h"
#include "gsl/gsl_cdf.h"
#include "gsl/gsl_specfunc.h"
#include "crypto.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

static int gsl_matrix_alloc_impl(size_t rows, size_t cols, gsl_matrix **out) {
    *out = gsl_matrix_alloc(rows, cols);
    return *out ? 0 : -1;
}

static int gsl_matrix_free_impl(gsl_matrix *m) {
    if (m)
        gsl_matrix_free(m);
    return 0;
}

static int gsl_gemm_impl(const gsl_matrix *A, const gsl_matrix *B, gsl_matrix *C, double alpha,
                         double beta) {
    return gsl_blas_dgemm(CblasNoTrans, CblasNoTrans, alpha, A, B, beta, C);
}

static double gsl_mean_impl(const double *data, size_t n) {
    return gsl_stats_mean(data, 1, n);
}

static double gsl_variance_impl(const double *data, size_t n) {
    return gsl_stats_variance(data, 1, n);
}

static double gsl_correlation_impl(const double *x, const double *y, size_t n) {
    return gsl_stats_correlation(x, 1, y, 1, n);
}

static double gsl_gaussian_sample_impl(double mu, double sigma) {
    /* Hardened: use /dev/urandom-backed bytes instead of libc rand(). */
    uint32_t words[2];
    if (!crypto_random_bytes((uint8_t *)words, sizeof(words)))
        return mu;
    double u1 = words[0] / 4294967296.0; /* [0,1) */
    double u2 = words[1] / 4294967296.0;
    if (u1 <= 0.0)
        u1 = 1.0 / 4294967296.0; /* avoid log(0) */
    double z = sqrt(-2.0 * log(u1)) * cos(2.0 * M_PI * u2);
    return mu + sigma * z;
}

static double gsl_gaussian_pdf_impl(double x, double mu, double sigma) {
    return gsl_ran_gaussian_pdf(x - mu, sigma);
}

static double gsl_gaussian_cdf_impl(double x, double mu, double sigma) {
    return gsl_cdf_gaussian_P(x - mu, sigma);
}

static double gsl_bessel_J0_impl(double x) {
    return gsl_sf_bessel_J0(x);
}

static double gsl_gamma_impl(double x) {
    return gsl_sf_gamma(x);
}

static double gsl_erf_impl(double x) {
    return erf(x); /* system erf is fine */
}

numerical_backend_t numerical_backend_gsl = {.base = {.name = "gsl",
                                                      .category = BACKEND_NUMERICAL,
                                                      .status = BACKEND_STATUS_LOADED,
                                                      .impl = NULL,
                                                      .init = NULL,
                                                      .shutdown = NULL,
                                                      .version = NULL},
                                             .matrix_alloc = gsl_matrix_alloc_impl,
                                             .matrix_free = gsl_matrix_free_impl,
                                             .gemm = gsl_gemm_impl,
                                             .mean = gsl_mean_impl,
                                             .variance = gsl_variance_impl,
                                             .correlation = gsl_correlation_impl,
                                             .gaussian_sample = gsl_gaussian_sample_impl,
                                             .gaussian_pdf = gsl_gaussian_pdf_impl,
                                             .gaussian_cdf = gsl_gaussian_cdf_impl,
                                             .bessel_J0 = gsl_bessel_J0_impl,
                                             .gamma = gsl_gamma_impl,
                                             .erf = gsl_erf_impl};

int numerical_backend_register(numerical_backend_t *impl) {
    return backend_register(&impl->base);
}