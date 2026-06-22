/* gsl_interp.h — Interpolation */
#ifndef __GSL_INTERP_H__
#define __GSL_INTERP_H__

#include <stdlib.h>

typedef enum {
    GSL_INTERP_LINEAR,
    GSL_INTERP_POLYNOMIAL,
    GSL_INTERP_CSPLINE,
    GSL_INTERP_CSPLINE_PERIODIC,
    GSL_INTERP_AKIMA,
    GSL_INTERP_AKIMA_PERIODIC,
    GSL_INTERP_STEPPED
} gsl_interp_type;

typedef struct {
    const gsl_interp_type *type;
    double xmin, xmax;
    size_t size;
    void *state;
} gsl_interp;

typedef struct {
    const char *name;
    unsigned int min_size;
    void *(*alloc)(size_t size);
    int (*init)(void *, const double xa[], const double ya[], size_t size);
    int (*eval)(const void *, const double xa[], const double ya[], size_t size,
                double x, gsl_interp_accel *a, double *y);
    int (*eval_deriv)(const void *, const double xa[], const double ya[], size_t size,
                      double x, gsl_interp_accel *a, double *dydx);
    int (*eval_deriv2)(const void *, const double xa[], const double ya[], size_t size,
                       double x, gsl_interp_accel *a, double *d2ydx2);
    int (*eval_integ)(const void *, const double xa[], const double ya[], size_t size,
                      gsl_interp_accel *a, double a, double b, double *result);
    void (*free)(void *);
} gsl_interp_type;

gsl_interp *gsl_interp_alloc(const gsl_interp_type *T, size_t size);
void gsl_interp_free(gsl_interp *interp);
int gsl_interp_init(gsl_interp *interp, const double xa[], const double ya[], size_t size);

const char *gsl_interp_name(const gsl_interp *interp);
unsigned int gsl_interp_min_size(const gsl_interp *interp);
unsigned int gsl_interp_type_min_size(const gsl_interp_type *T);

int gsl_interp_eval(const gsl_interp *interp, const double xa[], const double ya[], double x,
                    gsl_interp_accel *a, double *y);
int gsl_interp_eval_e(const gsl_interp *interp, const double xa[], const double ya[], double x,
                      gsl_interp_accel *a, double *y);
double gsl_interp_eval_safe(const gsl_interp *interp, const double xa[], const double ya[], double x,
                            gsl_interp_accel *a);
int gsl_interp_eval_deriv(const gsl_interp *interp, const double xa[], const double ya[], double x,
                          gsl_interp_accel *a, double *dydx);
int gsl_interp_eval_deriv_e(const gsl_interp *interp, const double xa[], const double ya[], double x,
                            gsl_interp_accel *a, double *dydx);
int gsl_interp_eval_deriv2(const gsl_interp *interp, const double xa[], const double ya[], double x,
                           gsl_interp_accel *a, double *d2ydx2);
int gsl_interp_eval_deriv2_e(const gsl_interp *interp, const double xa[], const double ya[], double x,
                             gsl_interp_accel *a, double *d2ydx2);
int gsl_interp_eval_integ(const gsl_interp *interp, const double xa[], const double ya[], double a, double b,
                          gsl_interp_accel *acc, double *result);
int gsl_interp_eval_integ_e(const gsl_interp *interp, const double xa[], const double ya[], double a, double b,
                            gsl_interp_accel *acc, double *result);

typedef struct {
    size_t cache;
    size_t miss;
    size_t limit;
} gsl_interp_accel;

gsl_interp_accel *gsl_interp_accel_alloc(void);
int gsl_interp_accel_reset(gsl_interp_accel *a);
void gsl_interp_accel_free(gsl_interp_accel *a);
size_t gsl_interp_accel_find(gsl_interp_accel *a, const double x_array[], size_t size, double x);

extern const gsl_interp_type *gsl_interp_linear;
extern const gsl_interp_type *gsl_interp_polynomial;
extern const gsl_interp_type *gsl_interp_cspline;
extern const gsl_interp_type *gsl_interp_cspline_periodic;
extern const gsl_interp_type *gsl_interp_akima;
extern const gsl_interp_type *gsl_interp_akima_periodic;
extern const gsl_interp_type *gsl_interp_steplen;

#endif /* __GSL_INTERP_H__ */