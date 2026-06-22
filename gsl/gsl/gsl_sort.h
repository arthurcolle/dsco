/* gsl_sort.h — sorting */
#include <stddef.h>
#ifndef __GSL_SORT_H__
#define __GSL_SORT_H__

void gsl_sort(double *data, size_t stride, size_t n);
void gsl_sort_vector(double *v);

void gsl_sort_index(size_t *p, const double *data, size_t stride, size_t n);

int gsl_sort_smallest(double *dest, size_t k,
                      const double *src, size_t stride, size_t n);
int gsl_sort_largest(double *dest, size_t k,
                     const double *src, size_t stride, size_t n);

int gsl_sort_smallest_index(size_t *p, size_t k,
                            const double *src, size_t stride, size_t n);
int gsl_sort_largest_index(size_t *p, size_t k,
                           const double *src, size_t stride, size_t n);

#endif /* __GSL_SORT_H__ */