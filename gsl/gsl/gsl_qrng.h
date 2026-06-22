/* gsl_qrng.h — Quasi-Random Sequences */
#ifndef __GSL_QRNG_H__
#define __GSL_QRNG_H__

#include <stdlib.h>

typedef enum {
    GSL_QRNG_NIEDERRREITER_2,
    GSL_QRNG_SOBOL,
    GSL_QRNG_HALTON,
    GSL_QRNG_REVERSEHALTON
} gsl_qrng_type;

typedef struct {
    const gsl_qrng_type *type;
    unsigned int dimension;
    size_t size;
    void *state;
} gsl_qrng;

gsl_qrng *gsl_qrng_alloc(const gsl_qrng_type *T, unsigned int dimension);
void gsl_qrng_free(gsl_qrng *q);
void gsl_qrng_init(gsl_qrng *q);
int gsl_qrng_get(const gsl_qrng *q, double x[]);
const char *gsl_qrng_name(const gsl_qrng *q);
size_t gsl_qrng_size(const gsl_qrng *q);
void *gsl_qrng_state(const gsl_qrng *q);

extern const gsl_qrng_type *gsl_qrng_niederreiter_2;
extern const gsl_qrng_type *gsl_qrng_sobol;
extern const gsl_qrng_type *gsl_qrng_halton;
extern const gsl_qrng_type *gsl_qrng_reversehalton;

#endif /* __GSL_QRNG_H__ */