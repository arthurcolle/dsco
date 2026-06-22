#include <stddef.h>
/* gsl_specfunc.h — special functions */
#ifndef __GSL_SPECFUNC_H__
#define __GSL_SPECFUNC_H__

double gsl_sf_airy_Ai(double x, int mode);
double gsl_sf_airy_Bi(double x, int mode);
double gsl_sf_airy_Ai_scaled(double x, int mode);
double gsl_sf_airy_Bi_scaled(double x, int mode);

double gsl_sf_bessel_J0(double x);
double gsl_sf_bessel_J1(double x);
double gsl_sf_bessel_Jn(int n, double x);
double gsl_sf_bessel_Y0(double x);
double gsl_sf_bessel_Y1(double x);
double gsl_sf_bessel_Yn(int n, double x);
double gsl_sf_bessel_I0(double x);
double gsl_sf_bessel_I1(double x);
double gsl_sf_bessel_In(int n, double x);
double gsl_sf_bessel_K0(double x);
double gsl_sf_bessel_K1(double x);
double gsl_sf_bessel_Kn(int n, double x);

double gsl_sf_legendre_P1(double x);
double gsl_sf_legendre_P2(double x);
double gsl_sf_legendre_P3(double x);
double gsl_sf_legendre_Pl(int l, double x);
double gsl_sf_legendre_Pl_array(int lmax, double x, double result_array[]);

double gsl_sf_gamma(double x);
double gsl_sf_lngamma(double x);
double gsl_sf_gamma_inc_P(double a, double x);
double gsl_sf_gamma_inc_Q(double a, double x);

double gsl_sf_psi(double x);
double gsl_sf_psi_1piy(double y);
double gsl_sf_psi_n(int n, double x);

double gsl_sf_zeta(double s);
double gsl_sf_zeta_m1(double s);
double gsl_sf_hzeta(double s, double q);

double gsl_sf_eta(double s);

#endif /* __GSL_SPECFUNC_H__ */