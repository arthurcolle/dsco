/* src/extension/eigen_backend.c — Eigen Backend Stub */
#include "extension/eigen_backend.h"
#include <stdio.h>

static int eigen_init(void *impl) {
    (void)impl;
    printf("[eigen_backend] Eigen backend loaded (stub)\n");
    return 0;
}

numerical_backend_t numerical_backend_eigen = {.base = {.name = "eigen",
                                                        .category = BACKEND_NUMERICAL,
                                                        .status = BACKEND_STATUS_UNLOADED,
                                                        .impl = NULL,
                                                        .init = eigen_init,
                                                        .shutdown = NULL,
                                                        .version = NULL}};

int eigen_backend_init(void) {
    return backend_register(&numerical_backend_eigen.base);
}