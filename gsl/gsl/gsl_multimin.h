/* gsl_multimin.h — Multidimensional Minimization */
#ifndef __GSL_MULTIMIN_H__
#define __GSL_MULTIMIN_H__

#include "gsl_vector.h"
#include "gsl_matrix.h"

typedef struct {
    size_t n;
    double f;
    gsl_vector *x;
    void *state;
} gsl_multimin_fminimizer;

typedef struct {
    const char *name;
    size_t size;
    int (*alloc)(void *state, size_t n);
    int (*set)(void *state, gsl_multimin_function *f,
               const gsl_vector *x, double *size, const gsl_vector *step_size);
    int (*iterate)(void *state, gsl_multimin_function *f,
                   gsl_vector *x, double *size, double *fval);
    void (*free)(void *state);
} gsl_multimin_fminimizer_type;

extern const gsl_multimin_fminimizer_type *gsl_multimin_fminimizer_nmsimplex;
extern const gsl_multimin_fminimizer_type *gsl_multimin_fminimizer_nmsimplex2;
extern const gsl_multimin_fminimizer_type *gsl_multimin_fminimizer_nmsimplex2rand;

gsl_multimin_fminimizer *gsl_multimin_fminimizer_alloc(const gsl_multimin_fminimizer_type *T, size_t n);
int gsl_multimin_fminimizer_set(gsl_multimin_fminimizer *s, gsl_multimin_function *f,
                                const gsl_vector *x, const gsl_vector *step_size);
void gsl_multimin_fminimizer_free(gsl_multimin_fminimizer *s);
int gsl_multimin_fminimizer_iterate(gsl_multimin_fminimizer *s);
const char *gsl_multimin_fminimizer_name(const gsl_multimin_fminimizer *s);
gsl_vector *gsl_multimin_fminimizer_x(const gsl_multimin_fminimizer *s);
double gsl_multimin_fminimizer_minimum(const gsl_multimin_fminimizer *s);
double gsl_multimin_fminimizer_size(const gsl_multimin_fminimizer *s);
int gsl_multimin_test_size(double size, double epsabs);

typedef struct {
    size_t n;
    double f;
    gsl_vector *x;
    gsl_vector *gradient;
    void *state;
} gsl_multimin_fdfminimizer;

typedef struct {
    const char *name;
    size_t size;
    int (*alloc)(void *state, size_t n);
    int (*set)(void *state, gsl_multimin_function_fdf *fdf,
               const gsl_vector *x, double *f, gsl_vector *gradient, double step_size, double tol);
    int (*iterate)(void *state, gsl_multimin_function_fdf *fdf,
                   gsl_vector *x, double *f, gsl_vector *gradient);
    int (*restart)(void *state);
    void (*free)(void *state);
} gsl_multimin_fdfminimizer_type;

extern const gsl_multimin_fdfminimizer_type *gsl_multimin_fdfminimizer_steepest_descent;
extern const gsl_multimin_fdfminimizer_type *gsl_multimin_fdfminimizer_conjugate_fr;
extern const gsl_multimin_fdfminimizer_type *gsl_multimin_fdfminimizer_conjugate_pr;
extern const gsl_multimin_fdfminimizer_type *gsl_multimin_fdfminimizer_vector_bfgs;
extern const gsl_multimin_fdfminimizer_type *gsl_multimin_fdfminimizer_vector_bfgs2;
extern const gsl_multimin_fdfminimizer_type *gsl_multimin_fdfminimizer_lbfgs;

gsl_multimin_fdfminimizer *gsl_multimin_fdfminimizer_alloc(const gsl_multimin_fdfminimizer_type *T, size_t n);
int gsl_multimin_fdfminimizer_set(gsl_multimin_fdfminimizer *s, gsl_multimin_function_fdf *fdf,
                                  const gsl_vector *x, double step_size, double tol);
void gsl_multimin_fdfminimizer_free(gsl_multimin_fdfminimizer *s);
int gsl_multimin_fdfminimizer_iterate(gsl_multimin_fdfminimizer *s);
int gsl_multimin_fdfminimizer_restart(gsl_multimin_fdfminimizer *s);
const char *gsl_multimin_fdfminimizer_name(const gsl_multimin_fdfminimizer *s);
gsl_vector *gsl_multimin_fdfminimizer_x(const gsl_multimin_fdfminimizer *s);
gsl_vector *gsl_multimin_fdfminimizer_gradient(const gsl_multimin_fdfminimizer *s);
double gsl_multimin_fdfminimizer_minimum(const gsl_multimin_fdfminimizer *s);
int gsl_multimin_test_gradient(const gsl_vector *g, double epsabs);
int gsl_multimin_test_size(double size, double epsabs);

#endif /* __GSL_MULTIMIN_H__ */