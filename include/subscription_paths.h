#ifndef DSCO_SUBSCRIPTION_PATHS_H
#define DSCO_SUBSCRIPTION_PATHS_H

#include <stdbool.h>
#include <stddef.h>

#define DSCO_SUBSCRIPTION_PATH_ARGV_MAX 64
#define DSCO_SUBSCRIPTION_PATH_MAX (1u << 16)

typedef struct {
    char *id;
    char *lane_id;
    char *provider;
    char *product;
    char *principal_profile;
    char *transport;
    char *billing;
    char *auth_mode;
    char *model;
    char *endpoint;
    char *executable;
    char *authenticated;
    char *source;
    char **argv;
    size_t argc;
    bool prompt_on_stdin;
    bool enabled;
    bool qualified;
    bool requested_automatic_eligibility;
} dsco_subscription_path_t;

typedef struct {
    dsco_subscription_path_t *paths;
    size_t count;
    size_t capacity;
    char **errors;
    size_t error_count;
} dsco_subscription_path_registry_t;

/* Load zero or more strict v1 manifests from the default config locations and
 * DSCO_SUBSCRIPTION_PATHS. Catalog inclusion is deliberately independent of
 * installation, authentication, qualification, and automatic scheduling. */
bool dsco_subscription_paths_load(dsco_subscription_path_registry_t *registry);
void dsco_subscription_paths_free(dsco_subscription_path_registry_t *registry);

const dsco_subscription_path_t *dsco_subscription_path_find(
    const dsco_subscription_path_registry_t *registry, const char *selector);

bool dsco_subscription_path_resolve_executable(const dsco_subscription_path_t *path,
                                               char *out, size_t out_len);
bool dsco_subscription_path_ready(const dsco_subscription_path_t *path,
                                  char *reason, size_t reason_len);
bool dsco_subscription_path_automatic_eligible(const dsco_subscription_path_t *path);

/* Build direct argv without a shell. Placeholders must occupy a complete
 * manifest argument and are limited to {prompt}, {model}, and {cwd}. */
bool dsco_subscription_path_build_argv(const dsco_subscription_path_t *path,
                                       const char *resolved_binary,
                                       const char *prompt, const char *model,
                                       const char *cwd, char **argv,
                                       size_t argv_cap, size_t *argc_out,
                                       char *error, size_t error_len);
void dsco_subscription_path_argv_free(char **argv, size_t argc);

#endif
