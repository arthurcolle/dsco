/* gsl_rng.h — minimal RNG interface */
#ifndef __GSL_RNG_H__
#define __GSL_RNG_H__

#include <stddef.h>
#include <stdint.h>

typedef struct {
    const char *name;
    size_t     max;
    size_t     min;
    size_t     size;
    void (*set)(void *state, unsigned long int seed);
    unsigned long int (*get)(void *state);
    double (*get_double)(void *state);
} gsl_rng_type;

typedef struct {
    const gsl_rng_type *type;
    void *state;
} gsl_rng;

extern const gsl_rng_type *gsl_rng_mt19937;
extern const gsl_rng_type *gsl_rng_ranlxs0;
extern const gsl_rng_type *gsl_rng_ranlxs1;
extern const gsl_rng_type *gsl_rng_ranlxs2;
extern const gsl_rng_type *gsl_rng_ranlxd1;
extern const gsl_rng_type *gsl_rng_ranlxd2;
extern const gsl_rng_type *gsl_rng_ranlux;
extern const gsl_rng_type *gsl_rng_ranlux64;
extern const gsl_rng_type *gsl_rng_ranmar;
extern const gsl_rng_type *gsl_rng_rand48;
extern const gsl_rng_type *gsl_rng_rand;
extern const gsl_rng_type *gsl_rng_random_bsd;
extern const gsl_rng_type *gsl_rng_random_libc5;
extern const gsl_rng_type *gsl_rng_random_glibc2;
extern const gsl_rng_type *gsl_rng_taus;
extern const gsl_rng_type *gsl_rng_taus2;
extern const gsl_rng_type *gsl_rng_vax;
extern const gsl_rng_type *gsl_rng_transputer;
extern const gsl_rng_type *gsl_rng_randu;
extern const gsl_rng_type *gsl_rng_minstd;
extern const gsl_rng_type *gsl_rng_mrg;
extern const gsl_rng_type *gsl_rng_slatec;
extern const gsl_rng_type *gsl_rng_uni;
extern const gsl_rng_type *gsl_rng_uni32;
extern const gsl_rng_type *gsl_rng_slatec;
extern const gsl_rng_type *gsl_rng_zuf;
extern const gsl_rng_type *gsl_rng_cmrg;
extern const gsl_rng_type *gsl_rng_gfsr4;

gsl_rng *gsl_rng_alloc(const gsl_rng_type *T);
void gsl_rng_free(gsl_rng *r);
void gsl_rng_set(const gsl_rng *r, unsigned long int seed);
unsigned long int gsl_rng_get(const gsl_rng *r);
double gsl_rng_uniform(const gsl_rng *r);
double gsl_rng_uniform_pos(const gsl_rng *r);
unsigned long int gsl_rng_uniform_int(const gsl_rng *r, unsigned long int n);

#endif /* __GSL_RNG_H__ */