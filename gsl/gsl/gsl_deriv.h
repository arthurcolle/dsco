/* gsl_deriv.h — Numerical Differentiation */
#ifndef __GSL_DERIV_H__
#define __GSL_DERIV_H__

#include "gsl_math.h"

int gsl_deriv_central(const gsl_function *f, double x, double h,
                      double *result, double *abserr);
int gsl_deriv_forward(const gsl_function *f, double x, double h,
                      double *result, double *abserr);
int gsl_deriv_backward(const gsl_function *f, double x, double h,
                       double *result, double *abserr);

#endif /* __GSL_DERIV_H__ */