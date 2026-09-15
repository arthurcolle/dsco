#ifndef DSCO_LINGO_AUTOBOT_H
#define DSCO_LINGO_AUTOBOT_H

#include <stdbool.h>
#include <stddef.h>

extern const char lingo_autobot_schema[];
extern const char lingo_autobot_output_schema[];
bool lingo_autobot_execute(const char *input, char *result, size_t capacity);

#endif
