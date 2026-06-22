#include <stddef.h>
/* gsl_randist.h — random number distributions */
#ifndef __GSL_RANDIST_H__
#define __GSL_RANDIST_H__

#include "gsl_rng.h"

double gsl_ran_gaussian(const gsl_rng *r, double sigma);
double gsl_ran_gaussian_pdf(double x, double sigma);
double gsl_ran_gaussian_ziggurat(const gsl_rng *r, double sigma);
double gsl_ran_gaussian_ratio_method(const gsl_rng *r, double sigma);

double gsl_ran_ugaussian(const gsl_rng *r);
double gsl_ran_ugaussian_pdf(double x);
double gsl_ran_ugaussian_tail(const gsl_rng *r, double a);

double gsl_ran_exponential(const gsl_rng *r, double mu);
double gsl_ran_exponential_pdf(double x, double mu);

double gsl_ran_laplace(const gsl_rng *r, double a);
double gsl_ran_laplace_pdf(double x, double a);

double gsl_ran_cauchy(const gsl_rng *r, double a);
double gsl_ran_cauchy_pdf(double x, double a);

double gsl_ran_rayleigh(const gsl_rng *r, double sigma);
double gsl_ran_rayleigh_pdf(double x, double sigma);

double gsl_ran_gamma(const gsl_rng *r, double a, double b);
double gsl_ran_gamma_pdf(double x, double a, double b);

double gsl_ran_beta(const gsl_rng *r, double a, double b);
double gsl_ran_beta_pdf(double x, double a, double b);

double gsl_ran_lognormal(const gsl_rng *r, double zeta, double sigma);
double gsl_ran_lognormal_pdf(double x, double zeta, double sigma);

double gsl_ran_chisq(const gsl_rng *r, double nu);
double gsl_ran_chisq_pdf(double x, double nu);

double gsl_ran_fdist(const gsl_rng *r, double nu1, double nu2);
double gsl_ran_fdist_pdf(double x, double nu1, double nu2);

double gsl_ran_tdist(const gsl_rng *r, double nu);
double gsl_ran_tdist_pdf(double x, double nu);

double gsl_ran_weibull(const gsl_rng *r, double a, double b);
double gsl_ran_weibull_pdf(double x, double a, double b);

double gsl_ran_logistic(const gsl_rng *r, double a);
double gsl_ran_logistic_pdf(double x, double a);

double gsl_ran_pareto(const gsl_rng *r, double a, double b);
double gsl_ran_pareto_pdf(double x, double a, double b);

double gsl_ran_bernoulli(const gsl_rng *r, double p);
double gsl_ran_bernoulli_pdf(unsigned int k, double p);

double gsl_ran_binomial(const gsl_rng *r, double p, unsigned int n);
double gsl_ran_binomial_pdf(unsigned int k, double p, unsigned int n);

double gsl_ran_poisson(const gsl_rng *r, double mu);
double gsl_ran_poisson_pdf(unsigned int k, double mu);

double gsl_ran_negative_binomial(const gsl_rng *r, double p, double n);
double gsl_ran_negative_binomial_pdf(unsigned int k, double p, double n);

double gsl_ran_geometric(const gsl_rng *r, double p);
double gsl_ran_geometric_pdf(unsigned int k, double p);

double gsl_ran_hypergeometric(const gsl_rng *r, unsigned int n1, unsigned int n2, unsigned int t);
double gsl_ran_hypergeometric_pdf(unsigned int k, unsigned int n1, unsigned int n2, unsigned int t);

double gsl_ran_logarithmic(const gsl_rng *r, double p);
double gsl_ran_logarithmic_pdf(unsigned int k, double p);

#endif /* __GSL_RANDIST_H__ */