#ifndef DSCO_MATRIX_INFERENCE_H
#define DSCO_MATRIX_INFERENCE_H

#include <stdbool.h>

/* Native operator surface for the dedicated Matrix inference appliance. */
int matrix_inference_cli(int argc, char **argv);

/* Kept public for the focused protocol regression in tests/test.c. */
bool matrix_inference_canary_response_ok(const char *response);

#endif
