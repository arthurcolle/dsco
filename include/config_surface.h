#ifndef DSCO_CONFIG_SURFACE_H
#define DSCO_CONFIG_SURFACE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    DSCO_CONFIG_BOOL = 0,
    DSCO_CONFIG_SIZE,
    DSCO_CONFIG_U64,
    DSCO_CONFIG_DOUBLE,
    DSCO_CONFIG_STRING,
} dsco_config_kind_t;

typedef union {
    bool boolean;
    size_t size;
    uint64_t u64;
    double number;
    const char *string;
} dsco_config_value_t;

typedef struct {
    const char *name;
    const char *environment;
    const char *description;
    dsco_config_kind_t kind;
    bool required;
    bool has_default;
    bool has_minimum;
    bool has_maximum;
    dsco_config_value_t default_value;
    dsco_config_value_t minimum;
    dsco_config_value_t maximum;
} dsco_config_spec_t;

typedef enum {
    DSCO_CONFIG_UNSET = 0,
    DSCO_CONFIG_FROM_DEFAULT,
    DSCO_CONFIG_FROM_ENVIRONMENT,
    DSCO_CONFIG_FROM_EXPLICIT,
} dsco_config_source_t;

typedef struct {
    bool is_set;
    dsco_config_source_t source;
    dsco_config_value_t value;
} dsco_config_resolved_t;

#define DSCO_CONFIG_BOOL_SPEC(NAME, ENVIRONMENT, DESCRIPTION)                                      \
    {.name = (NAME),                                                                               \
     .environment = (ENVIRONMENT),                                                                 \
     .description = (DESCRIPTION),                                                                 \
     .kind = DSCO_CONFIG_BOOL}
#define DSCO_CONFIG_SIZE_SPEC(NAME, ENVIRONMENT, DESCRIPTION)                                      \
    {.name = (NAME),                                                                               \
     .environment = (ENVIRONMENT),                                                                 \
     .description = (DESCRIPTION),                                                                 \
     .kind = DSCO_CONFIG_SIZE}
#define DSCO_CONFIG_U64_SPEC(NAME, ENVIRONMENT, DESCRIPTION)                                       \
    {.name = (NAME),                                                                               \
     .environment = (ENVIRONMENT),                                                                 \
     .description = (DESCRIPTION),                                                                 \
     .kind = DSCO_CONFIG_U64}
#define DSCO_CONFIG_DOUBLE_SPEC(NAME, ENVIRONMENT, DESCRIPTION)                                    \
    {.name = (NAME),                                                                               \
     .environment = (ENVIRONMENT),                                                                 \
     .description = (DESCRIPTION),                                                                 \
     .kind = DSCO_CONFIG_DOUBLE}
#define DSCO_CONFIG_STRING_SPEC(NAME, ENVIRONMENT, DESCRIPTION)                                    \
    {.name = (NAME),                                                                               \
     .environment = (ENVIRONMENT),                                                                 \
     .description = (DESCRIPTION),                                                                 \
     .kind = DSCO_CONFIG_STRING}

const char *dsco_config_kind_name(dsco_config_kind_t kind);
const char *dsco_config_source_name(dsco_config_source_t source);

/* Precedence: explicit text > non-empty environment value > declared default
 * > unset. Invalid configured values fail; they are never silently clamped or
 * replaced by a default. Strings in the resolved value borrow their source. */
bool dsco_config_resolve(const dsco_config_spec_t *spec, const char *explicit_text,
                         dsco_config_resolved_t *out, char *error, size_t error_capacity);

/* Emit one JSON Schema object for model/operator configuration. Returns the
 * required byte count excluding NUL; SIZE_MAX reports an invalid specification
 * or allocation failure. A NULL/zero buffer performs an exact sizing pass. */
size_t dsco_config_format_schema(const dsco_config_spec_t *specs, size_t count, char *buffer,
                                 size_t capacity);

#endif /* DSCO_CONFIG_SURFACE_H */
