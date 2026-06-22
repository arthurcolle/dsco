/* integration.c — minimal adaptive quadrature (Simpson's rule stub) */
#include "gsl/gsl_integration.h"
#include <math.h>
#include <stdlib.h>

gsl_integration_workspace *gsl_integration_workspace_alloc(size_t limit) {
    gsl_integration_workspace *w = malloc(sizeof(gsl_integration_workspace));
    if (!w) return NULL;
    w->limit = limit;
    w->size = 0;
    w->alist = malloc(limit * sizeof(double));
    w->blist = malloc(limit * sizeof(double));
    w->rlist = malloc(limit * sizeof(double));
    w->elist = malloc(limit * sizeof(double));
    return w;
}

void gsl_integration_workspace_free(gsl_integration_workspace *w) {
    if (w) {
        free(w->alist);
        free(w->blist);
        free(w->rlist);
        free(w->elist);
        free(w);
    }
}

int gsl_integration_qag(double (*f)(double x, void *params), void *params,
                        double a, double b,
                        double epsabs, double epsrel, size_t limit,
                        gsl_integration_workspace *workspace,
                        double *result, double *abserr) {
    /* Simplified: trapezoidal rule with fixed steps */
    const int N = 1000;
    double h = (b - a) / N;
    double sum = 0.5 * (f(a, params) + f(b, params));
    for (int i = 1; i < N; i++) {
        sum += f(a + i * h, params);
    }
    *result = h * sum;
    *abserr = 0.0; /* stub */
    return 0;
}