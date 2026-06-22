/* gsl_movstat.h — Moving Window Statistics */
#ifndef __GSL_MOVSTAT_H__
#define __GSL_MOVSTAT_H__

#include "gsl_vector.h"

typedef enum {
    GSL_MOVSTAT_END_PADZERO = 0,
    GSL_MOVSTAT_END_PADVALUE = 1,
    GSL_MOVSTAT_END_TRUNCATE = 2
} gsl_movstat_end_t;

typedef struct {
    size_t n;
    double *work;
} gsl_movstat_accumulator;

typedef struct {
    size_t H;
    size_t J;
    void *state;
} gsl_movstat_function;

int gsl_movstat_mean(const gsl_movstat_end_t endtype, const gsl_vector *x, gsl_vector *y,
                     const gsl_movstat_function *acc);
int gsl_movstat_variance(const gsl_movstat_end_t endtype, const gsl_vector *x, gsl_vector *y,
                         const gsl_movstat_function *acc);
int gsl_movstat_sd(const gsl_movstat_end_t endtype, const gsl_vector *x, gsl_vector *y,
                   const gsl_movstat_function *acc);
int gsl_movstat_median(const gsl_movstat_end_t endtype, const gsl_vector *x, gsl_vector *y,
                       const gsl_movstat_function *acc);
int gsl_movstat_min(const gsl_movstat_end_t endtype, const gsl_vector *x, gsl_vector *y,
                    const gsl_movstat_function *acc);
int gsl_movstat_max(const gsl_movstat_end_t endtype, const gsl_vector *x, gsl_vector *y,
                    const gsl_movstat_function *acc);
int gsl_movstat_minmax(const gsl_movstat_end_t endtype, const gsl_vector *x, gsl_vector *y_min,
                       gsl_vector *y_max, const gsl_movstat_function *acc);
int gsl_movstat_sum(const gsl_movstat_end_t endtype, const gsl_vector *x, gsl_vector *y,
                    const gsl_movstat_function *acc);
int gsl_movstat_qquantile(const gsl_movstat_end_t endtype, const gsl_vector *x, gsl_vector *y,
                          const double p, const gsl_movstat_function *acc);
int gsl_movstat_fill(const gsl_movstat_end_t endtype, const gsl_vector *x, const double pad_value,
                     gsl_vector *y);

#endif /* __GSL_MOVSTAT_H__ */