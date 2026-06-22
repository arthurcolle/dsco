#include <stddef.h>
/* gsl_cdf.h — cumulative distribution functions */
#ifndef __GSL_CDF_H__
#define __GSL_CDF_H__

double gsl_cdf_gaussian_P(double x, double sigma);
double gsl_cdf_gaussian_Q(double x, double sigma);
double gsl_cdf_gaussian_Pinv(double P, double sigma);
double gsl_cdf_gaussian_Qinv(double Q, double sigma);

double gsl_cdf_ugaussian_P(double x);
double gsl_cdf_ugaussian_Q(double x);
double gsl_cdf_ugaussian_Pinv(double P);
double gsl_cdf_ugaussian_Qinv(double Q);

double gsl_cdf_exponential_P(double x, double mu);
double gsl_cdf_exponential_Q(double x, double mu);
double gsl_cdf_exponential_Pinv(double P, double mu);
double gsl_cdf_exponential_Qinv(double Q, double mu);

double gsl_cdf_laplace_P(double x, double a);
double gsl_cdf_laplace_Q(double x, double a);
double gsl_cdf_laplace_Pinv(double P, double a);
double gsl_cdf_laplace_Qinv(double Q, double a);

double gsl_cdf_cauchy_P(double x, double a);
double gsl_cdf_cauchy_Q(double x, double a);
double gsl_cdf_cauchy_Pinv(double P, double a);
double gsl_cdf_cauchy_Qinv(double Q, double a);

double gsl_cdf_rayleigh_P(double x, double sigma);
double gsl_cdf_rayleigh_Q(double x, double sigma);
double gsl_cdf_rayleigh_Pinv(double P, double sigma);
double gsl_cdf_rayleigh_Qinv(double Q, double sigma);

double gsl_cdf_gamma_P(double x, double a, double b);
double gsl_cdf_gamma_Q(double x, double a, double b);
double gsl_cdf_gamma_Pinv(double P, double a, double b);
double gsl_cdf_gamma_Qinv(double Q, double a, double b);

double gsl_cdf_beta_P(double x, double a, double b);
double gsl_cdf_beta_Q(double x, double a, double b);
double gsl_cdf_beta_Pinv(double P, double a, double b);
double gsl_cdf_beta_Qinv(double Q, double a, double b);

double gsl_cdf_lognormal_P(double x, double zeta, double sigma);
double gsl_cdf_lognormal_Q(double x, double zeta, double sigma);
double gsl_cdf_lognormal_Pinv(double P, double zeta, double sigma);
double gsl_cdf_lognormal_Qinv(double Q, double zeta, double sigma);

double gsl_cdf_chisq_P(double x, double nu);
double gsl_cdf_chisq_Q(double x, double nu);
double gsl_cdf_chisq_Pinv(double P, double nu);
double gsl_cdf_chisq_Qinv(double Q, double nu);

double gsl_cdf_fdist_P(double x, double nu1, double nu2);
double gsl_cdf_fdist_Q(double x, double nu1, double nu2);
double gsl_cdf_fdist_Pinv(double P, double nu1, double nu2);
double gsl_cdf_fdist_Qinv(double Q, double nu1, double nu2);

double gsl_cdf_tdist_P(double x, double nu);
double gsl_cdf_tdist_Q(double x, double nu);
double gsl_cdf_tdist_Pinv(double P, double nu);
double gsl_cdf_tdist_Qinv(double Q, double nu);

double gsl_cdf_weibull_P(double x, double a, double b);
double gsl_cdf_weibull_Q(double x, double a, double b);
double gsl_cdf_weibull_Pinv(double P, double a, double b);
double gsl_cdf_weibull_Qinv(double Q, double a, double b);

double gsl_cdf_logistic_P(double x, double a);
double gsl_cdf_logistic_Q(double x, double a);
double gsl_cdf_logistic_Pinv(double P, double a);
double gsl_cdf_logistic_Qinv(double Q, double a);

double gsl_cdf_pareto_P(double x, double a, double b);
double gsl_cdf_pareto_Q(double x, double a, double b);
double gsl_cdf_pareto_Pinv(double P, double a, double b);
double gsl_cdf_pareto_Qinv(double Q, double a, double b);

double gsl_cdf_binomial_P(unsigned int k, double p, unsigned int n);
double gsl_cdf_binomial_Q(unsigned int k, double p, unsigned int n);

double gsl_cdf_poisson_P(unsigned int k, double mu);
double gsl_cdf_poisson_Q(unsigned int k, double mu);

double gsl_cdf_negative_binomial_P(unsigned int k, double p, double n);
double gsl_cdf_negative_binomial_Q(unsigned int k, double p, double n);

double gsl_cdf_geometric_P(unsigned int k, double p);
double gsl_cdf_geometric_Q(unsigned int k, double p);

#endif /* __GSL_CDF_H__ */