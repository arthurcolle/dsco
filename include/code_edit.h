#ifndef DSCO_CODE_EDIT_H
#define DSCO_CODE_EDIT_H
#include <stdbool.h>
#include <stddef.h>

/* Governed entry points: callers must dispatch through tools_execute_for_tier. */
bool code_read_execute(const char *input, char *result, size_t size);
bool code_edit_execute(const char *input, char *result, size_t size);
#endif
