#ifndef DSCO_LINGO_CHIMERA_H
#define DSCO_LINGO_CHIMERA_H
#include <stdbool.h>
#include <stddef.h>
extern const char lingo_chimera_schema[];
extern const char lingo_chimera_output_schema[];
bool lingo_chimera_execute(const char *input, char *result, size_t capacity);
extern const char lingo_chimera_execute_schema[];
extern const char lingo_chimera_execute_output_schema[];
bool lingo_chimera_complete(const char *input, char *result, size_t capacity);
#endif
