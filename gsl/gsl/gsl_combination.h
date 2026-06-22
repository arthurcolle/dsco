/* gsl_combination.h — combinations */
#include <stddef.h>
#ifndef __GSL_COMBINATION_H__
#define __GSL_COMBINATION_H__

#include <stdlib.h>

typedef struct {
    size_t n;
    size_t k;
    size_t *data;
} gsl_combination;

gsl_combination *gsl_combination_alloc(size_t n, size_t k);
void gsl_combination_free(gsl_combination *c);
void gsl_combination_init_first(gsl_combination *c);
void gsl_combination_init_last(gsl_combination *c);

int gsl_combination_memcpy(gsl_combination *dest, const gsl_combination *src);

size_t gsl_combination_get(const gsl_combination *c, size_t i);
int gsl_combination_swap(gsl_combination *c, size_t i, size_t j);

int gsl_combination_next(gsl_combination *c);
int gsl_combination_prev(gsl_combination *c);

#endif /* __GSL_COMBINATION_H__ */