/* gsl_poly.h — Polynomials */
#ifndef __GSL_POLY_H__
#define __GSL_POLY_H__

int gsl_poly_eval(const double c[], const int len, const double x);
int gsl_poly_dd_init(double dd[], const double xa[], const double ya[], size_t size);
int gsl_poly_dd_taylor(double c[], double xp, const double dd[], const double xa[], size_t size, double *w);
int gsl_poly_dd_hermite_init(double dd[], double za[], const double xa[], const double ya[],
                             const double dya[], const size_t size);

int gsl_poly_solve_quadratic(double a, double b, double c, double *x0, double *x1);
int gsl_poly_complex_solve_quadratic(double a, double b, double c, gsl_complex *z0, gsl_complex *z1);

int gsl_poly_solve_cubic(double a, double b, double c, double *x0, double *x1, double *x2);
int gsl_poly_complex_solve_cubic(double a, double b, double c, gsl_complex *z0, gsl_complex *z1, gsl_complex *z2);

typedef struct {
    size_t nc;
    double *matrix;
} gsl_poly_complex_workspace;

gsl_poly_complex_workspace *gsl_poly_complex_workspace_alloc(size_t n);
void gsl_poly_complex_workspace_free(gsl_poly_complex_workspace *w);
int gsl_poly_complex_solve(const double *a, size_t n, gsl_poly_complex_workspace *w, gsl_complex *z);

#endif /* __GSL_POLY_H__ */