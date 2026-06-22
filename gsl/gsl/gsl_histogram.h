/* gsl_histogram.h — histograms */
#include <stddef.h>
#ifndef __GSL_HISTOGRAM_H__
#define __GSL_HISTOGRAM_H__

#include <stdlib.h>

typedef struct {
    size_t n;
    double *range;
    double *bin;
} gsl_histogram;

gsl_histogram *gsl_histogram_alloc(size_t n);
void gsl_histogram_free(gsl_histogram *h);
int gsl_histogram_set_ranges(gsl_histogram *h, const double range[], size_t size);
int gsl_histogram_set_ranges_uniform(gsl_histogram *h, double xmin, double xmax);

int gsl_histogram_increment(gsl_histogram *h, double x);
int gsl_histogram_accumulate(gsl_histogram *h, double x, double weight);

double gsl_histogram_get(const gsl_histogram *h, size_t i);
int gsl_histogram_get_range(const gsl_histogram *h, size_t i, double *lower, double *upper);

double gsl_histogram_max(const gsl_histogram *h);
double gsl_histogram_min(const gsl_histogram *h);
size_t gsl_histogram_bins(const gsl_histogram *h);

void gsl_histogram_reset(gsl_histogram *h);

double gsl_histogram_mean(const gsl_histogram *h);
double gsl_histogram_sigma(const gsl_histogram *h);
double gsl_histogram_sum(const gsl_histogram *h);

int gsl_histogram_find(const gsl_histogram *h, double x, size_t *i);

int gsl_histogram_fprintf(FILE *stream, const gsl_histogram *h,
                          const char *range_format, const char *bin_format);
int gsl_histogram_fscanf(FILE *stream, gsl_histogram *h);
int gsl_histogram_fwrite(FILE *stream, const gsl_histogram *h);
int gsl_histogram_fread(FILE *stream, gsl_histogram *h);

#endif /* __GSL_HISTOGRAM_H__ */