#ifndef DSCO_LINGO_H
#define DSCO_LINGO_H
#include <stdbool.h>
#include <stddef.h>
extern const char lingo_tool_schema[];
bool lingo_execute(const char *input, char *result, size_t capacity);
int lingo_cli(int argc, char **argv);
#endif
