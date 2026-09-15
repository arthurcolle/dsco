#ifndef DSCO_LINGO_WORKFLOW_H
#define DSCO_LINGO_WORKFLOW_H
#include <stdbool.h>
#include <stddef.h>
extern const char lingo_workflow_schema[];
extern const char lingo_workflow_output_schema[];
bool lingo_workflow_execute(const char *input, char *out, size_t capacity);
#endif
