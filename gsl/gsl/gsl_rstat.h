/* gsl_rstat.h — Running Statistics */
#ifndef __GSL_RSTAT_H__
#define __GSL_RSTAT_H__

typedef struct {
    size_t n;
    double mean;
    double M2;
    double M3;
    double M4;
    double median;
    double *work;
} gsl_rstat_workspace;

gsl_rstat_workspace *gsl_rstat_alloc(void);
void gsl_rstat_free(gsl_rstat_workspace *w);
int gsl_rstat_add(double x, gsl_rstat_workspace *w);
double gsl_rstat_n(const gsl_rstat_workspace *w);
double gsl_rstat_mean(const gsl_rstat_workspace *w);
double gsl_rstat_variance(const gsl_rstat_workspace *w);
double gsl_rstat_sd(const gsl_rstat_workspace *w);
double gsl_rstat_rms(const gsl_rstat_workspace *w);
double gsl_rstat_sd_mean(const gsl_rstat_workspace *w);
double gsl_rstat_median(const gsl_rstat_workspace *w);
double gsl_rstat_skew(const gsl_rstat_workspace *w);
double gsl_rstat_kurtosis(const gsl_rstat_workspace *w);
double gsl_rstat_min(const gsl_rstat_workspace *w);
double gsl_rstat_max(const gsl_rstat_workspace *w);
int gsl_rstat_reset(gsl_rstat_workspace *w);

typedef struct {
    size_t n;
    double *work;
} gsl_rstat_quantile_workspace;

gsl_rstat_quantile_workspace *gsl_rstat_quantile_alloc(const double p);
void gsl_rstat_quantile_free(gsl_rstat_quantile_workspace *w);
int gsl_rstat_quantile_reset(gsl_rstat_quantile_workspace *w);
int gsl_rstat_quantile_add(double x, gsl_rstat_quantile_workspace *w);
double gsl_rstat_quantile_get(const gsl_rstat_quantile_workspace *w);

#endif /* __GSL_RSTAT_H__ */