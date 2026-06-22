/* gsl_bspline.h — B-splines */
#ifndef __GSL_BSPLINE_H__
#define __GSL_BSPLINE_H__

#include "gsl_vector.h"
#include "gsl_matrix.h"

typedef struct {
    size_t k;
    size_t nbreak;
    size_t l;
    gsl_vector *knots;
    gsl_vector *deltal;
    gsl_vector *deltar;
    gsl_vector *B;
    gsl_matrix *A;
    gsl_matrix *dB;
} gsl_bspline_workspace;

gsl_bspline_workspace *gsl_bspline_alloc(const size_t k, const size_t nbreak);
void gsl_bspline_free(gsl_bspline_workspace *w);
size_t gsl_bspline_ncoeffs(const gsl_bspline_workspace *w);
size_t gsl_bspline_order(const gsl_bspline_workspace *w);
size_t gsl_bspline_nbreak(const gsl_bspline_workspace *w);
double gsl_bspline_breakpoint(const size_t i, const gsl_bspline_workspace *w);
int gsl_bspline_knots(const gsl_vector *breakpts, gsl_bspline_workspace *w);
int gsl_bspline_knots_uniform(const double a, const double b, gsl_bspline_workspace *w);
int gsl_bspline_eval(const double x, gsl_vector *B, gsl_bspline_workspace *w);
int gsl_bspline_eval_deriv(const double x, const size_t nderiv, gsl_matrix *dB, gsl_bspline_workspace *w);

#endif /* __GSL_BSPLINE_H__ */