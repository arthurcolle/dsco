/* statistics.c — minimal descriptive statistics */
#include "gsl/gsl_statistics.h"
#include <math.h>

double gsl_stats_mean(const double data[], size_t stride, size_t n) {
    double sum = 0.0;
    for (size_t i = 0; i < n; i++) sum += data[i * stride];
    return sum / n;
}

double gsl_stats_variance(const double data[], size_t stride, size_t n) {
    double mean = gsl_stats_mean(data, stride, n);
    double sum = 0.0;
    for (size_t i = 0; i < n; i++) {
        double d = data[i * stride] - mean;
        sum += d * d;
    }
    return sum / (n - 1);
}

double gsl_stats_sd(const double data[], size_t stride, size_t n) {
    return sqrt(gsl_stats_variance(data, stride, n));
}

double gsl_stats_covariance(const double data1[], size_t stride1,
                            const double data2[], size_t stride2, size_t n) {
    double mean1 = gsl_stats_mean(data1, stride1, n);
    double mean2 = gsl_stats_mean(data2, stride2, n);
    double sum = 0.0;
    for (size_t i = 0; i < n; i++) {
        sum += (data1[i * stride1] - mean1) * (data2[i * stride2] - mean2);
    }
    return sum / (n - 1);
}

double gsl_stats_correlation(const double data1[], size_t stride1,
                             const double data2[], size_t stride2, size_t n) {
    double cov = gsl_stats_covariance(data1, stride1, data2, stride2, n);
    double sd1 = gsl_stats_sd(data1, stride1, n);
    double sd2 = gsl_stats_sd(data2, stride2, n);
    return cov / (sd1 * sd2);
}

double gsl_stats_skew(const double data[], size_t stride, size_t n) {
    double mean = gsl_stats_mean(data, stride, n);
    double sd = gsl_stats_sd(data, stride, n);
    double sum = 0.0;
    for (size_t i = 0; i < n; i++) {
        double d = (data[i * stride] - mean) / sd;
        sum += d * d * d;
    }
    return sum / n;
}

double gsl_stats_kurtosis(const double data[], size_t stride, size_t n) {
    double mean = gsl_stats_mean(data, stride, n);
    double sd = gsl_stats_sd(data, stride, n);
    double sum = 0.0;
    for (size_t i = 0; i < n; i++) {
        double d = (data[i * stride] - mean) / sd;
        sum += d * d * d * d;
    }
    return sum / n - 3.0;
}

double gsl_stats_median_from_sorted_data(const double sorted_data[], size_t stride, size_t n) {
    if (n % 2 == 1) {
        return sorted_data[((n - 1) / 2) * stride];
    } else {
        return 0.5 * (sorted_data[(n / 2 - 1) * stride] + sorted_data[(n / 2) * stride]);
    }
}

double gsl_stats_quantile_from_sorted_data(const double sorted_data[], size_t stride, size_t n, double f) {
    double idx = f * (n - 1);
    size_t i = (size_t)idx;
    double frac = idx - i;
    if (i + 1 < n) {
        return (1.0 - frac) * sorted_data[i * stride] + frac * sorted_data[(i + 1) * stride];
    }
    return sorted_data[i * stride];
}