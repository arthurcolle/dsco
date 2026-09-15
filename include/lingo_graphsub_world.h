#ifndef DSCO_LINGO_GRAPHSUB_WORLD_H
#define DSCO_LINGO_GRAPHSUB_WORLD_H
#include <stdbool.h>
#include <stddef.h>
extern const char lingo_graphsub_world_schema[];
extern const char lingo_graphsub_world_output_schema[];
/* Caller must use tools_execute_for_tier; publication is never retried. */
bool lingo_graphsub_world_execute(const char *input, char *result, size_t capacity);
#endif
