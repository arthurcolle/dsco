#include <stddef.h>
/* gsl_complex.h — complex numbers */
#ifndef __GSL_COMPLEX_H__
#define __GSL_COMPLEX_H__

typedef struct {
    double dat[2];
} gsl_complex;

#define GSL_REAL(z) ((z).dat[0])
#define GSL_IMAG(z) ((z).dat[1])
#define GSL_SET_COMPLEX(zp,x,y) do { (zp)->dat[0]=(x); (zp)->dat[1]=(y); } while(0)
#define GSL_SET_REAL(zp,x) do { (zp)->dat[0]=(x); } while(0)
#define GSL_SET_IMAG(zp,y) do { (zp)->dat[1]=(y); } while(0)

gsl_complex gsl_complex_rect(double x, double y);
gsl_complex gsl_complex_polar(double r, double theta);

double gsl_complex_arg(gsl_complex z);
double gsl_complex_abs(gsl_complex z);
double gsl_complex_abs2(gsl_complex z);
double gsl_complex_logabs(gsl_complex z);

gsl_complex gsl_complex_add(gsl_complex a, gsl_complex b);
gsl_complex gsl_complex_sub(gsl_complex a, gsl_complex b);
gsl_complex gsl_complex_mul(gsl_complex a, gsl_complex b);
gsl_complex gsl_complex_div(gsl_complex a, gsl_complex b);

gsl_complex gsl_complex_add_real(gsl_complex a, double x);
gsl_complex gsl_complex_sub_real(gsl_complex a, double x);
gsl_complex gsl_complex_mul_real(gsl_complex a, double x);
gsl_complex gsl_complex_div_real(gsl_complex a, double x);

gsl_complex gsl_complex_add_imag(gsl_complex a, double y);
gsl_complex gsl_complex_sub_imag(gsl_complex a, double y);
gsl_complex gsl_complex_mul_imag(gsl_complex a, double y);
gsl_complex gsl_complex_div_imag(gsl_complex a, double y);

gsl_complex gsl_complex_conjugate(gsl_complex z);
gsl_complex gsl_complex_inverse(gsl_complex a);
gsl_complex gsl_complex_negative(gsl_complex a);

gsl_complex gsl_complex_sqrt(gsl_complex z);
gsl_complex gsl_complex_sqrt_real(double x);
gsl_complex gsl_complex_pow(gsl_complex a, gsl_complex b);
gsl_complex gsl_complex_pow_real(gsl_complex a, double b);

gsl_complex gsl_complex_exp(gsl_complex a);
gsl_complex gsl_complex_log(gsl_complex a);
gsl_complex gsl_complex_log10(gsl_complex a);
gsl_complex gsl_complex_log_b(gsl_complex a, gsl_complex b);

gsl_complex gsl_complex_sin(gsl_complex a);
gsl_complex gsl_complex_cos(gsl_complex a);
gsl_complex gsl_complex_tan(gsl_complex a);
gsl_complex gsl_complex_sec(gsl_complex a);
gsl_complex gsl_complex_csc(gsl_complex a);
gsl_complex gsl_complex_cot(gsl_complex a);

gsl_complex gsl_complex_arcsin(gsl_complex a);
gsl_complex gsl_complex_arccos(gsl_complex a);
gsl_complex gsl_complex_arctan(gsl_complex a);
gsl_complex gsl_complex_arcsec(gsl_complex a);
gsl_complex gsl_complex_arccsc(gsl_complex a);
gsl_complex gsl_complex_arccot(gsl_complex a);

gsl_complex gsl_complex_sinh(gsl_complex a);
gsl_complex gsl_complex_cosh(gsl_complex a);
gsl_complex gsl_complex_tanh(gsl_complex a);
gsl_complex gsl_complex_sech(gsl_complex a);
gsl_complex gsl_complex_csch(gsl_complex a);
gsl_complex gsl_complex_coth(gsl_complex a);

gsl_complex gsl_complex_arcsinh(gsl_complex a);
gsl_complex gsl_complex_arccosh(gsl_complex a);
gsl_complex gsl_complex_arctanh(gsl_complex a);
gsl_complex gsl_complex_arcsech(gsl_complex a);
gsl_complex gsl_complex_arccsch(gsl_complex a);
gsl_complex gsl_complex_arccoth(gsl_complex a);

#endif /* __GSL_COMPLEX_H__ */