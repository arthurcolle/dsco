#include <stddef.h>
/* gsl_wavelet.h — wavelets */
#ifndef __GSL_WAVELET_H__
#define __GSL_WAVELET_H__

typedef struct {
    const char *name;
    int (*init)(const double **h1, const double **g1,
                const double **h2, const double **g2, size_t *n,
                size_t *k, size_t *offset);
} gsl_wavelet_type;

typedef struct {
    const gsl_wavelet_type *type;
    const double *h1;
    const double *g1;
    const double *h2;
    const double *g2;
    size_t n;
    size_t k;
    size_t offset;
} gsl_wavelet;

typedef struct {
    double *scratch;
    size_t n;
} gsl_wavelet_workspace;

gsl_wavelet *gsl_wavelet_alloc(const gsl_wavelet_type *T, size_t k);
void gsl_wavelet_free(gsl_wavelet *w);

int gsl_wavelet_transform(const gsl_wavelet *w,
                          double data[], size_t stride, size_t n,
                          gsl_wavelet_direction dir, gsl_wavelet_workspace *work);

int gsl_wavelet_transform_forward(const gsl_wavelet *w,
                                  double data[], size_t stride, size_t n,
                                  gsl_wavelet_workspace *work);

int gsl_wavelet_transform_inverse(const gsl_wavelet *w,
                                  double data[], size_t stride, size_t n,
                                  gsl_wavelet_workspace *work);

#endif /* __GSL_WAVELET_H__ */