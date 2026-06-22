#include <stddef.h>
/* gsl_errno.h — minimal error codes */
#ifndef __GSL_ERRNO_H__
#define __GSL_ERRNO_H__

enum {
  GSL_SUCCESS  = 0,
  GSL_FAILURE  = -1,
  GSL_CONTINUE = -2,
  GSL_EDOM     = 1,   /* domain error */
  GSL_ERANGE   = 2,   /* range error */
  GSL_EFAULT   = 3,   /* invalid pointer */
  GSL_EINVAL   = 4,   /* invalid argument */
  GSL_EFAILED  = 5,   /* generic failure */
  GSL_EFACTOR  = 6,
  GSL_ESANITY  = 7,
  GSL_ENOMEM   = 8,
  GSL_EBADFUNC = 9,
  GSL_ERUNAWAY = 10,
  GSL_EMAXITER = 11,
  GSL_EZERODIV = 12,
  GSL_EBADTOL  = 13,
  GSL_ETOL     = 14,
  GSL_EUNDRFLW = 15,
  GSL_EOVRFLW  = 16,
  GSL_ELOSS    = 17,
  GSL_EROUND   = 18,
  GSL_EBADLEN  = 19,
  GSL_EINDEX   = 20,
  GSL_EINVAL   = 21,
  GSL_ETOL     = 14
};

void gsl_error(const char *reason, const char *file, int line, int gsl_errno);
void gsl_stream_printf(const char *label, const char *file, int line, const char *reason);

typedef void gsl_error_handler_t(const char *reason, const char *file, int line, int gsl_errno);
gsl_error_handler_t *gsl_set_error_handler(gsl_error_handler_t *new_handler);
gsl_error_handler_t *gsl_set_error_handler_off(void);

#endif /* __GSL_ERRNO_H__ */