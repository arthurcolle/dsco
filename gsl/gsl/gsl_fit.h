#include <stddef.h>
/* gsl_fit.h — linear and nonlinear fitting */
#ifndef __GSL_FIT_H__
#define __GSL_FIT_H__

int gsl_fit_linear(const double x[], size_t xstride,
                   const double y[], size_t ystride,
                   size_t n,
                   double *c0, double *c1,
                   double *cov00, double *cov01, double *cov11,
                   double *chisq);

int gsl_fit_wlinear(const double x[], size_t xstride,
                    const double w[], size_t wstride,
                    const double y[], size_t ystride,
                    size_t n,
                    double *c0, double *c1,
                    double *cov00, double *cov01, double *cov11,
                    double *chisq);

int gsl_fit_linear_est(double x,
                       double c0, double c1,
                       double cov00, double cov01, double cov11,
                       double *y, double *y_err);

int gsl_fit_mul(const double x[], size_t xstride,
                const double y[], size_t ystride,
                size_t n,
                double *c1, double *cov11, double *chisq);

int gsl_fit_wmul(const double x[], size_t xstride,
                 const double w[], size_t wstride,
                 const double y[], size_t ystride,
                 size_t n,
                 double *c1, double *cov11, double *chisq);

int gsl_fit_mul_est(double x,
                    double c1, double cov11,
                    double *y, double *y_err);

#endif /* __GSL_FIT_H__ */