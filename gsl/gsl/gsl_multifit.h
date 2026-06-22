#include <stddef.h>
/* gsl_multifit.h — linear least squares fitting */
#ifndef __GSL_MULTIFIT_H__
#define __GSL_MULTIFIT_H__

#include "gsl_matrix.h"
#include "gsl_vector.h"

int gsl_multifit_linear(const gsl_matrix *X, const gsl_vector *y,
                        gsl_vector *c, gsl_matrix *cov,
                        double *chisq, gsl_matrix *work);

int gsl_multifit_wlinear(const gsl_matrix *X, const gsl_vector *w,
                         const gsl_vector *y,
                         gsl_vector *c, gsl_matrix *cov,
                         double *chisq, gsl_matrix *work);

int gsl_multifit_linear_est(const gsl_vector *x, const gsl_vector *c,
                            const gsl_matrix *cov, double *y, double *y_err);

int gsl_multifit_linear_residuals(const gsl_matrix *X, const gsl_vector *y,
                                  const gsl_vector *c, gsl_vector *r);

#endif /* __GSL_MULTIFIT_H__ */