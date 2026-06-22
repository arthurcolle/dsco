/* gsl_fft.h — Fast Fourier Transform */
#ifndef __GSL_FFT_H__
#define __GSL_FFT_H__

#include <stddef.h>
#include "gsl_complex.h"

#ifndef GSL_COMPLEX_PACKED
#define GSL_COMPLEX_PACKED gsl_complex_packed
typedef double gsl_complex_packed;
#endif

int gsl_fft_complex_radix2_forward(gsl_complex_packed_array data, size_t stride, size_t n);
int gsl_fft_complex_radix2_backward(gsl_complex_packed_array data, size_t stride, size_t n);
int gsl_fft_complex_radix2_inverse(gsl_complex_packed_array data, size_t stride, size_t n);
int gsl_fft_complex_radix2_transform(gsl_complex_packed_array data, size_t stride, size_t n, int sign);

int gsl_fft_complex_radix2_dif_forward(gsl_complex_packed_array data, size_t stride, size_t n);
int gsl_fft_complex_radix2_dif_backward(gsl_complex_packed_array data, size_t stride, size_t n);
int gsl_fft_complex_radix2_dif_inverse(gsl_complex_packed_array data, size_t stride, size_t n);
int gsl_fft_complex_radix2_dif_transform(gsl_complex_packed_array data, size_t stride, size_t n, int sign);

int gsl_fft_complex_bitreverse_order(gsl_complex_packed_array data, size_t stride, size_t n, size_t n_used);

typedef struct {
    size_t n;
    size_t nf;
    size_t factor[64];
    gsl_complex *twiddle[64];
    gsl_complex *trig;
} gsl_fft_complex_wavetable;

typedef struct {
    size_t n;
    double *scratch;
} gsl_fft_complex_workspace;

gsl_fft_complex_wavetable *gsl_fft_complex_wavetable_alloc(size_t n);
void gsl_fft_complex_wavetable_free(gsl_fft_complex_wavetable *wavetable);
gsl_fft_complex_workspace *gsl_fft_complex_workspace_alloc(size_t n);
void gsl_fft_complex_workspace_free(gsl_fft_complex_workspace *workspace);

int gsl_fft_complex_forward(gsl_complex_packed_array data, size_t stride, size_t n,
                            const gsl_fft_complex_wavetable *wavetable,
                            gsl_fft_complex_workspace *work);
int gsl_fft_complex_backward(gsl_complex_packed_array data, size_t stride, size_t n,
                             const gsl_fft_complex_wavetable *wavetable,
                             gsl_fft_complex_workspace *work);
int gsl_fft_complex_inverse(gsl_complex_packed_array data, size_t stride, size_t n,
                            const gsl_fft_complex_wavetable *wavetable,
                            gsl_fft_complex_workspace *work);
int gsl_fft_complex_transform(gsl_complex_packed_array data, size_t stride, size_t n,
                              const gsl_fft_complex_wavetable *wavetable,
                              gsl_fft_complex_workspace *work, int sign);

int gsl_fft_real_radix2_transform(double data[], size_t stride, size_t n);
int gsl_fft_halfcomplex_radix2_inverse(double data[], size_t stride, size_t n);
int gsl_fft_halfcomplex_radix2_backward(double data[], size_t stride, size_t n);
int gsl_fft_halfcomplex_radix2_transform(double data[], size_t stride, size_t n);

int gsl_fft_real_radix2_pack(const double real_float[], gsl_complex_packed_array complex_packed_float,
                             size_t float_stride, size_t n);
int gsl_fft_real_radix2_unpack(const gsl_complex_packed_array complex_packed_float, double real_float[],
                               size_t float_stride, size_t n);

typedef struct {
    size_t n;
    size_t nf;
    size_t factor[64];
    gsl_complex *twiddle[64];
    gsl_complex *trig;
} gsl_fft_real_wavetable;
typedef gsl_fft_real_wavetable gsl_fft_halfcomplex_wavetable;

typedef struct {
    size_t n;
    double *scratch;
} gsl_fft_real_workspace;

gsl_fft_real_wavetable *gsl_fft_real_wavetable_alloc(size_t n);
void gsl_fft_real_wavetable_free(gsl_fft_real_wavetable *wavetable);
gsl_fft_halfcomplex_wavetable *gsl_fft_halfcomplex_wavetable_alloc(size_t n);
void gsl_fft_halfcomplex_wavetable_free(gsl_fft_halfcomplex_wavetable *wavetable);
gsl_fft_real_workspace *gsl_fft_real_workspace_alloc(size_t n);
void gsl_fft_real_workspace_free(gsl_fft_real_workspace *workspace);

int gsl_fft_real_transform(double data[], size_t stride, size_t n,
                           const gsl_fft_real_wavetable *wavetable,
                           gsl_fft_real_workspace *work);
int gsl_fft_halfcomplex_transform(double data[], size_t stride, size_t n,
                                  const gsl_fft_halfcomplex_wavetable *wavetable,
                                  gsl_fft_real_workspace *work);
int gsl_fft_real_inverse(double data[], size_t stride, size_t n,
                         const gsl_fft_halfcomplex_wavetable *wavetable,
                         gsl_fft_real_workspace *work);
int gsl_fft_halfcomplex_inverse(double data[], size_t stride, size_t n,
                                const gsl_fft_halfcomplex_wavetable *wavetable,
                                gsl_fft_real_workspace *work);
int gsl_fft_real_unpack(const double real_float[], gsl_complex_packed_array complex_packed_float,
                        size_t float_stride, size_t n);
int gsl_fft_halfcomplex_unpack(const double halfcomplex_float[], gsl_complex_packed_array complex_packed_float,
                               size_t float_stride, size_t n);

#endif /* __GSL_FFT_H__ */