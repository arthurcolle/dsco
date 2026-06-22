/* rng.c — minimal RNG implementation (mt19937 stub) */
#include "gsl/gsl_rng.h"
#include <stdlib.h>
#include <string.h>

static const gsl_rng_type mt19937_type = {
    "mt19937",
    0xffffffffUL,
    0,
    sizeof(uint32_t) * 624,
    NULL, /* set */
    NULL, /* get */
    NULL  /* get_double */
};

const gsl_rng_type *gsl_rng_mt19937 = &mt19937_type;

gsl_rng *gsl_rng_alloc(const gsl_rng_type *T) {
    gsl_rng *r = malloc(sizeof(gsl_rng));
    if (!r) return NULL;
    r->type = T;
    r->state = calloc(1, T->size);
    return r;
}

void gsl_rng_free(gsl_rng *r) {
    if (r) {
        free(r->state);
        free(r);
    }
}

void gsl_rng_set(const gsl_rng *r, unsigned long int seed) {
    (void)r; (void)seed;
    /* Stub: real implementation would seed MT19937 */
}

unsigned long int gsl_rng_get(const gsl_rng *r) {
    (void)r;
    return (unsigned long int)rand();
}

double gsl_rng_uniform(const gsl_rng *r) {
    return gsl_rng_get(r) / (double)r->type->max;
}

double gsl_rng_uniform_pos(const gsl_rng *r) {
    double u = gsl_rng_uniform(r);
    return u == 0.0 ? 1.0 / (double)r->type->max : u;
}

unsigned long int gsl_rng_uniform_int(const gsl_rng *r, unsigned long int n) {
    return gsl_rng_get(r) % n;
}