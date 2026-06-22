#include <stddef.h>
/* gsl_sys.h — system utilities */
#ifndef __GSL_SYS_H__
#define __GSL_SYS_H__

double gsl_log1p(double x);
double gsl_expm1(double x);
double gsl_hypot(double x, double y);
double gsl_hypot3(double x, double y, double z);
double gsl_acosh(double x);
double gsl_asinh(double x);
double gsl_atanh(double x);

int gsl_isnan(double x);
int gsl_isinf(double x);
int gsl_finite(double x);

double gsl_nan(void);
double gsl_posinf(void);
double gsl_neginf(void);

#endif /* __GSL_SYS_H__ */