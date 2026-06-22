#include <stddef.h>
/* gsl_ieee_utils.h — IEEE floating point utilities */
#ifndef __GSL_IEEE_UTILS_H__
#define __GSL_IEEE_UTILS_H__

void gsl_ieee_printf_float(const float *x);
void gsl_ieee_printf_double(const double *x);

void gsl_ieee_fprintf_float(FILE *stream, const float *x);
void gsl_ieee_fprintf_double(FILE *stream, const double *x);

void gsl_ieee_float_to_rep(const float *x, int *sign, int *exponent, int *mantissa);
void gsl_ieee_double_to_rep(const double *x, int *sign, int *exponent, int *mantissa);

#endif /* __GSL_IEEE_UTILS_H__ */