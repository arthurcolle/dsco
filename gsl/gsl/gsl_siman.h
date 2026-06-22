/* gsl_siman.h — Simulated Annealing */
#ifndef __GSL_SIMAN_H__
#define __GSL_SIMAN_H__

typedef struct {
    int N_TRIES;
    int ITERS_FIXED_T;
    double STEP_SIZE;
    double K, T_INITIAL, MU_T, T_MIN;
} gsl_siman_params_t;

void gsl_siman_solve(const gsl_rng *r, void *x0_p, void (*efunc)(void *, double *),
                     void (*take_step)(const gsl_rng *, void *, double),
                     void (*copyfunc)(void *, void *), void (*printfunc)(void *),
                     size_t element_size, gsl_siman_params_t params);

#endif /* __GSL_SIMAN_H__ */