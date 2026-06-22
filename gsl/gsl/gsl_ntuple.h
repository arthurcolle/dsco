/* gsl_ntuple.h — N-tuple Data Storage */
#ifndef __GSL_NTUPLE_H__
#define __GSL_NTUPLE_H__

#include <stdio.h>

typedef struct {
    FILE *file;
    void *ntuple_data;
    size_t size;
} gsl_ntuple;

typedef struct {
    int (*function)(void *ntuple_data, void *params);
    void *params;
} gsl_ntuple_select_fn;

typedef struct {
    double (*function)(void *ntuple_data, void *params);
    void *params;
} gsl_ntuple_value_fn;

gsl_ntuple *gsl_ntuple_create(char *filename, void *ntuple_data, size_t size);
gsl_ntuple *gsl_ntuple_open(char *filename, void *ntuple_data, size_t size);
int gsl_ntuple_write(gsl_ntuple *ntuple);
int gsl_ntuple_read(gsl_ntuple *ntuple);
void gsl_ntuple_close(gsl_ntuple *ntuple);

int gsl_ntuple_project(void *h, gsl_ntuple *ntuple, gsl_ntuple_value_fn *value_func,
                       gsl_ntuple_select_fn *select_func);

#endif /* __GSL_NTUPLE_H__ */