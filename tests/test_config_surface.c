#include "config_surface.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(condition, message)                                                                  \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "FAIL: %s (line %d)\n", (message), __LINE__);                          \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

enum {
    TEST_MIN_WORKERS = 1,
    TEST_MAX_WORKERS = sizeof(uint64_t) * CHAR_BIT,
};

static const dsco_config_spec_t TEST_SPECS[] = {
    {
        .name = "workers",
        .environment = "DSCO_TEST_CONFIG_WORKERS",
        .description = "Concurrent workers.",
        .kind = DSCO_CONFIG_SIZE,
        .required = true,
        .has_minimum = true,
        .has_maximum = true,
        .minimum.size = TEST_MIN_WORKERS,
        .maximum.size = TEST_MAX_WORKERS,
    },
    {
        .name = "enabled",
        .environment = "DSCO_TEST_CONFIG_ENABLED",
        .description = "Enable the path.",
        .kind = DSCO_CONFIG_BOOL,
        .has_default = true,
        .default_value.boolean = true,
    },
    DSCO_CONFIG_STRING_SPEC("label", "DSCO_TEST_CONFIG_LABEL", "Operator label."),
};

static void test_resolution_precedence(void) {
    char error[BUFSIZ];
    dsco_config_resolved_t resolved;
    char workers[sizeof(size_t) * CHAR_BIT + 1];
    snprintf(workers, sizeof(workers), "%zu", sizeof(uint64_t));

    unsetenv("DSCO_TEST_CONFIG_WORKERS");
    CHECK(!dsco_config_resolve(&TEST_SPECS[0], NULL, &resolved, error, sizeof(error)) &&
              strstr(error, "required"),
          "required value fails when every source is absent");

    setenv("DSCO_TEST_CONFIG_WORKERS", workers, 1);
    CHECK(dsco_config_resolve(&TEST_SPECS[0], NULL, &resolved, error, sizeof(error)) &&
              resolved.is_set && resolved.source == DSCO_CONFIG_FROM_ENVIRONMENT &&
              resolved.value.size == sizeof(uint64_t),
          "environment value resolves with provenance");

    snprintf(workers, sizeof(workers), "%zu", sizeof(uint32_t));
    CHECK(dsco_config_resolve(&TEST_SPECS[0], workers, &resolved, error, sizeof(error)) &&
              resolved.source == DSCO_CONFIG_FROM_EXPLICIT &&
              resolved.value.size == sizeof(uint32_t),
          "explicit value outranks environment");

    CHECK(dsco_config_resolve(&TEST_SPECS[1], NULL, &resolved, error, sizeof(error)) &&
              resolved.source == DSCO_CONFIG_FROM_DEFAULT && resolved.value.boolean,
          "declared default remains observable as a source");

    unsetenv("DSCO_TEST_CONFIG_WORKERS");
}

static void test_invalid_values_fail_without_clamping(void) {
    char error[BUFSIZ];
    dsco_config_resolved_t resolved;
    char outside[sizeof(size_t) * CHAR_BIT + 1];
    snprintf(outside, sizeof(outside), "%zu", (size_t)TEST_MAX_WORKERS + TEST_MIN_WORKERS);
    CHECK(!dsco_config_resolve(&TEST_SPECS[0], outside, &resolved, error, sizeof(error)) &&
              strstr(error, "outside"),
          "out-of-range explicit values fail instead of clamping");
    CHECK(!dsco_config_resolve(&TEST_SPECS[0], "workers", &resolved, error, sizeof(error)) &&
              strstr(error, "must be"),
          "malformed values fail instead of falling back");
}

static void test_machine_schema(void) {
    size_t needed =
        dsco_config_format_schema(TEST_SPECS, sizeof(TEST_SPECS) / sizeof(TEST_SPECS[0]), NULL, 0);
    CHECK(needed != SIZE_MAX && needed > 0, "schema sizing succeeds");
    char *schema = malloc(needed + 1);
    CHECK(schema != NULL, "schema output allocates at its exact measured size");
    if (!schema)
        return;
    CHECK(dsco_config_format_schema(TEST_SPECS, sizeof(TEST_SPECS) / sizeof(TEST_SPECS[0]), schema,
                                    needed + 1) == needed,
          "schema formatting is deterministic");
    CHECK(strstr(schema, "\"workers\"") &&
              strstr(schema, "\"x-environment\":\"DSCO_TEST_CONFIG_WORKERS\"") &&
              strstr(schema, "\"required\":[\"workers\"]"),
          "schema exposes model-facing names, environment controls, and requirements");
    char maximum_field[sizeof(size_t) * CHAR_BIT + sizeof("\"maximum\":")];
    snprintf(maximum_field, sizeof(maximum_field), "\"maximum\":%u", TEST_MAX_WORKERS);
    CHECK(strstr(schema, maximum_field) && strstr(schema, "\"default\":true"),
          "schema exposes declared constraints without invented limits");
    free(schema);
}

int main(void) {
    test_resolution_precedence();
    test_invalid_values_fail_without_clamping();
    test_machine_schema();
    if (failures) {
        fprintf(stderr, "%d configuration surface test(s) failed\n", failures);
        return EXIT_FAILURE;
    }
    puts("configuration surface: all tests passed");
    return EXIT_SUCCESS;
}
