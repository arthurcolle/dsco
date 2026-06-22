/* gsl_chebyshev.h — Chebyshev Approximations */
#ifndef __GSL_CHEBYSHEV_H__
#define __GSL_CHEBYSHEV_H__

typedef struct {
    double *c;
    size_t order;
    size_t a, b;
} gsl_cheb_series;

gsl_cheb_series *gsl_cheb_alloc(const size_t order);
void gsl_cheb_free(gsl_cheb_series *cs);
int gsl_cheb_init(gsl_cheb_series *cs, const gsl_function *func, const double a, const double b);
double gsl_cheb_eval(const gsl_cheb_series *cs, const double x);
int gsl_cheb_eval_err(const gsl_cheb_series *cs, const double x, double *result, double *abserr);
double gsl_cheb_eval_n(const gsl_cheb_series *cs, const size_t n, const double x);
int gsl_cheb_eval_n_err(const gsl_cheb_series *cs, const size_t n, const double x,
                        double *result, double *abserr);
double gsl_cheb_eval_err_mode(const gsl_cheb_series *cs, const double x,
                              gsl_mode_t mode, double *result, double *abserr);

#endif /* __GSL_CHEBYSHEV_H__ */