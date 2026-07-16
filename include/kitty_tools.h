#ifndef DSCO_KITTY_TOOLS_H
#define DSCO_KITTY_TOOLS_H

#include <stdbool.h>
#include <stddef.h>

/* Governed agent-facing Kitty surfaces. */
bool tool_kitty_remote(const char *input_json, char *result, size_t result_len);
bool tool_kitten(const char *input_json, char *result, size_t result_len);

#endif /* DSCO_KITTY_TOOLS_H */
