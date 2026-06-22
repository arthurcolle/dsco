/* gsl_sum.h — Series Acceleration */
#ifndef __GSL_SUM_H__
#define __GSL_SUM_H__

int gsl_sum_levin_u_alloc(size_t n);
int gsl_sum_levin_u_free(void *w);
int gsl_sum_levin_u_accel(const double *array, const size_t n, void *w, double *sum_accel, double *abserr);
int gsl_sum_levin_u_minmax(const double *array, const size_t n, const size_t min_terms, const size_t max_terms,
                           void *w, double *sum_accel, double *abserr);
int gsl_sum_levin_u_step(const double term, const size_t n, const size_t nmax, void *w, double *sum_accel);

int gsl_sum_levin_utrunc_alloc(size_t n);
int gsl_sum_levin_utrunc_free(void *w);
int gsl_sum_levin_utrunc_accel(const double *array, const size_t n, void *w, double *sum_accel, double *abserr_trunc);
int gsl_sum_levin_utrunc_minmax(const double *array, const size_t n, const size_t min_terms, const size_t max_terms,
                                void *w, double *sum_accel, double *abserr_trunc);
int gsl_sum_levin_utrunc_step(const double term, const size_t n, void *w, double *sum_accel);

#endif /* __GSL_SUM_H__ */