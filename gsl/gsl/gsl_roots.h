/* gsl_roots.h — One Dimensional Root-Finding */
#ifndef __GSL_ROOTS_H__
#define __GSL_ROOTS_H__

#include "gsl_math.h"

typedef struct {
    const char *name;
    size_t size;
    int (*set)(void *state, gsl_function *f, double *root, double x_lower, double x_upper);
    int (*iterate)(void *state, gsl_function *f, double *root, double *x_lower, double *x_upper);
} gsl_root_fsolver_type;

typedef struct {
    const gsl_root_fsolver_type *type;
    gsl_function *function;
    double root;
    double x_lower;
    double x_upper;
    void *state;
} gsl_root_fsolver;

gsl_root_fsolver *gsl_root_fsolver_alloc(const gsl_root_fsolver_type *T);
void gsl_root_fsolver_free(gsl_root_fsolver *s);
int gsl_root_fsolver_set(gsl_root_fsolver *s, gsl_function *f, double x_lower, double x_upper);
int gsl_root_fsolver_iterate(gsl_root_fsolver *s);
const char *gsl_root_fsolver_name(const gsl_root_fsolver *s);
double gsl_root_fsolver_root(const gsl_root_fsolver *s);
double gsl_root_fsolver_x_lower(const gsl_root_fsolver *s);
double gsl_root_fsolver_x_upper(const gsl_root_fsolver *s);

typedef struct {
    const char *name;
    size_t size;
    int (*set)(void *state, gsl_function_fdf *fdf, double *root);
    int (*iterate)(void *state, gsl_function_fdf *fdf, double *root);
} gsl_root_fdfsolver_type;

typedef struct {
    const gsl_root_fdfsolver_type *type;
    gsl_function_fdf *fdf;
    double root;
    void *state;
} gsl_root_fdfsolver;

gsl_root_fdfsolver *gsl_root_fdfsolver_alloc(const gsl_root_fdfsolver_type *T);
void gsl_root_fdfsolver_free(gsl_root_fdfsolver *s);
int gsl_root_fdfsolver_set(gsl_root_fdfsolver *s, gsl_function_fdf *fdf, double root);
int gsl_root_fdfsolver_iterate(gsl_root_fdfsolver *s);
const char *gsl_root_fdfsolver_name(const gsl_root_fdfsolver *s);
double gsl_root_fdfsolver_root(const gsl_root_fdfsolver *s);

int gsl_root_test_interval(double x_lower, double x_upper, double epsabs, double epsrel);
int gsl_root_test_residual(double f, double epsabs);
int gsl_root_test_delta(double x1, double x0, double epsabs, double epsrel);

extern const gsl_root_fsolver_type *gsl_root_fsolver_bisection;
extern const gsl_root_fsolver_type *gsl_root_fsolver_brent;
extern const gsl_root_fsolver_type *gsl_root_fsolver_falsepos;
extern const gsl_root_fdfsolver_type *gsl_root_fdfsolver_newton;
extern const gsl_root_fdfsolver_type *gsl_root_fdfsolver_secant;
extern const gsl_root_fdfsolver_type *gsl_root_fdfsolver_steffenson;

#endif /* __GSL_ROOTS_H__ */