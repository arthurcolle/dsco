/* src/extension/fftw_backend.c — FFTW Backend Stub */
#include "extension/fftw_backend.h"
#include <stdio.h>

static int fftw_init(void *impl) {
    (void)impl;
    printf("[fftw_backend] FFTW backend loaded (stub)\n");
    return 0;
}

numerical_backend_t numerical_backend_fftw = {.base = {.name = "fftw",
                                                       .category = BACKEND_NUMERICAL,
                                                       .status = BACKEND_STATUS_UNLOADED,
                                                       .impl = NULL,
                                                       .init = fftw_init,
                                                       .shutdown = NULL,
                                                       .version = NULL}};

int fftw_backend_init(void) {
    return backend_register(&numerical_backend_fftw.base);
}