#include <stddef.h>
/* gsl_integration.h — numerical integration */
#ifndef __GSL_INTEGRATION_H__
#define __GSL_INTEGRATION_H__

typedef struct {
    size_t limit;
    size_t size;
    size_t nrmax;
    size_t i;
    size_t maximum;
    size_t *order;
    double *alist;
    double *blist;
    double *rlist;
    double *elist;
    size_t *level;
} gsl_integration_workspace;

gsl_integration_workspace *gsl_integration_workspace_alloc(size_t limit);
void gsl_integration_workspace_free(gsl_integration_workspace *w);

typedef struct {
    double epsabs;
    double epsrel;
    size_t limit;
} gsl_integration_workspace_params;

int gsl_integration_qag(double (*f)(double x, void *params), void *params,
                        double a, double b,
                        double epsabs, double epsrel, size_t limit,
                        gsl_integration_workspace *workspace,
                        double *result, double *abserr);

int gsl_integration_qagi(double (*f)(double x, void *params), void *params,
                         double epsabs, double epsrel, size_t limit,
                         gsl_integration_workspace *workspace,
                         double *result, double *abserr);

int gsl_integration_qagiu(double (*f)(double x, void *params), void *params,
                          double a, double epsabs, double epsrel, size_t limit,
                          gsl_integration_workspace *workspace,
                          double *result, double *abserr);

int gsl_integration_qagil(double (*f)(double x, void *params), void *params,
                          double b, double epsabs, double epsrel, size_t limit,
                          gsl_integration_workspace *workspace,
                          double *result, double *abserr);

#endif /* __GSL_INTEGRATION_H__ */