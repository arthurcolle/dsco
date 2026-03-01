#ifndef DSCO_TOOLS_H
#define DSCO_TOOLS_H

#include <stdbool.h>
#include <stddef.h>

/* Tool definition */
typedef struct {
    const char *name;
    const char *description;
    const char *input_schema_json;
    bool (*execute)(const char *input_json, char *result, size_t result_len);
} tool_def_t;

void             tools_init(void);
const tool_def_t *tools_get_all(int *count);
bool             tools_execute(const char *name, const char *input_json,
                               char *result, size_t result_len);

#endif
