/* extension/fftw_backend.h — FFTW Backend (Skeleton) */
#ifndef DSCO_EXTENSION_FFTW_BACKEND_H
#define DSCO_EXTENSION_FFTW_BACKEND_H

#include "numerical_backend.h"

#ifdef __cplusplus
extern "C" {
#endif

extern numerical_backend_t numerical_backend_fftw;

int fftw_backend_init(void);

#ifdef __cplusplus
}
#endif
#endif /* DSCO_EXTENSION_FFTW_BACKEND_H */