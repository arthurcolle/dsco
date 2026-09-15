#ifndef DSCO_GRAPHSUB_OPERATOR_H
#define DSCO_GRAPHSUB_OPERATOR_H

#include <stdbool.h>
#include <stddef.h>

extern const char graphsub_operator_schema[];
extern const char graphsub_operator_output_schema[];

/* Native, bounded, live GraphSub browse operations. Tool callers must dispatch
 * through tools_execute_for_tier(); this module does not grant capabilities.
 * Result must have room for the complete JSON envelope (128 bytes minimum for
 * errors). False means an error; no partial success response is emitted. */
bool graphsub_operator_execute(const char *input, char *result, size_t capacity);

#endif
