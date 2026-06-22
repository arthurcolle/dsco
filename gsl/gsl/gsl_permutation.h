/* gsl_permutation.h — permutations */
#include <stddef.h>
#ifndef __GSL_PERMUTATION_H__
#define __GSL_PERMUTATION_H__

#include <stdlib.h>

typedef struct {
    size_t size;
    size_t *data;
} gsl_permutation;

gsl_permutation *gsl_permutation_alloc(size_t n);
void gsl_permutation_free(gsl_permutation *p);
void gsl_permutation_init(gsl_permutation *p);
int gsl_permutation_memcpy(gsl_permutation *dest, const gsl_permutation *src);

size_t gsl_permutation_get(const gsl_permutation *p, size_t i);
int gsl_permutation_swap(gsl_permutation *p, size_t i, size_t j);

int gsl_permutation_reverse(gsl_permutation *p);
int gsl_permutation_inverse(gsl_permutation *inv, const gsl_permutation *p);

int gsl_permutation_next(gsl_permutation *p);
int gsl_permutation_prev(gsl_permutation *p);

#endif /* __GSL_PERMUTATION_H__ */