#include <stddef.h>
/* gsl_test.h — test framework */
#ifndef __GSL_TEST_H__
#define __GSL_TEST_H__

void gsl_test(int status, const char *test_description, ...);
void gsl_test_rel(double result, double expected, double relative_error,
                  const char *test_description, ...);
void gsl_test_abs(double result, double expected, double absolute_error,
                  const char *test_description, ...);
void gsl_test_factor(double result, double expected, double factor,
                     const char *test_description, ...);
void gsl_test_int(int result, int expected, const char *test_description, ...);
void gsl_test_str(const char *result, const char *expected,
                  const char *test_description, ...);
void gsl_test_verbose(int verbose);

#endif /* __GSL_TEST_H__ */