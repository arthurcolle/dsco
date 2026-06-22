/* gsl_statistics.h — descriptive statistics */
#include <stddef.h>
#ifndef __GSL_STATISTICS_H__
#define __GSL_STATISTICS_H__

double gsl_stats_mean(const double data[], size_t stride, size_t n);
double gsl_stats_variance(const double data[], size_t stride, size_t n);
double gsl_stats_sd(const double data[], size_t stride, size_t n);
double gsl_stats_variance_with_fixed_mean(const double data[], size_t stride, size_t n, double mean);
double gsl_stats_sd_with_fixed_mean(const double data[], size_t stride, size_t n, double mean);
double gsl_stats_tss(const double data[], size_t stride, size_t n);
double gsl_stats_tss_m(const double data[], size_t stride, size_t n, double mean);

double gsl_stats_absdev(const double data[], size_t stride, size_t n);
double gsl_stats_skew(const double data[], size_t stride, size_t n);
double gsl_stats_kurtosis(const double data[], size_t stride, size_t n);
double gsl_stats_lag1_autocorrelation(const double data[], size_t stride, size_t n);

double gsl_stats_covariance(const double data1[], size_t stride1,
                            const double data2[], size_t stride2, size_t n);
double gsl_stats_correlation(const double data1[], size_t stride1,
                             const double data2[], size_t stride2, size_t n);
double gsl_stats_spearman(const double data1[], size_t stride1,
                          const double data2[], size_t stride2, size_t n,
                          double work[]);

double gsl_stats_max(const double data[], size_t stride, size_t n);
double gsl_stats_min(const double data[], size_t stride, size_t n);
void   gsl_stats_minmax(double *min, double *max, const double data[], size_t stride, size_t n);

size_t gsl_stats_max_index(const double data[], size_t stride, size_t n);
size_t gsl_stats_min_index(const double data[], size_t stride, size_t n);
void   gsl_stats_minmax_index(size_t *min_index, size_t *max_index,
                              const double data[], size_t stride, size_t n);

double gsl_stats_median_from_sorted_data(const double sorted_data[], size_t stride, size_t n);
double gsl_stats_quantile_from_sorted_data(const double sorted_data[], size_t stride, size_t n, double f);

double gsl_stats_trmean_from_sorted_data(double trim, const double sorted_data[], size_t stride, size_t n);
double gsl_stats_gastwirth_from_sorted_data(const double sorted_data[], size_t stride, size_t n);

double gsl_stats_mad(const double data[], size_t stride, size_t n, double work[]);
double gsl_stats_mad0(const double data[], size_t stride, size_t n, double work[]);

double gsl_stats_sn_from_sorted_data(const double sorted_data[], size_t stride, size_t n, double work[]);
double gsl_stats_qn_from_sorted_data(const double sorted_data[], size_t stride, size_t n, double work[], int work_int[]);

#endif /* __GSL_STATISTICS_H__ */