#ifndef DSCO_JSON_TOOLS_H
#define DSCO_JSON_TOOLS_H
#include <stdbool.h>
#include <stddef.h>
bool tool_json_pointer(const char *input, char *result, size_t rlen);
bool tool_json_diff(const char *input, char *result, size_t rlen);
#endif
