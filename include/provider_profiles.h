#ifndef DSCO_PROVIDER_PROFILES_H
#define DSCO_PROVIDER_PROFILES_H

#include <stdbool.h>
#include <stddef.h>

#define PROVIDER_PROFILE_MAX_ALIASES   8
#define PROVIDER_PROFILE_MAX_ENV_VARS  8

typedef enum {
    PROVIDER_API_CHAT_COMPLETIONS = 0,
    PROVIDER_API_ANTHROPIC_MESSAGES,
    PROVIDER_API_CODEX_RESPONSES,
    PROVIDER_API_BEDROCK_CONVERSE,
    PROVIDER_API_ACP,
    PROVIDER_API_EXTERNAL_PROCESS,
} provider_api_mode_t;

typedef enum {
    PROVIDER_AUTH_API_KEY = 0,
    PROVIDER_AUTH_OAUTH_DEVICE_CODE,
    PROVIDER_AUTH_OAUTH_EXTERNAL,
    PROVIDER_AUTH_COPILOT,
    PROVIDER_AUTH_AWS_SDK,
    PROVIDER_AUTH_EXTERNAL_PROCESS,
    PROVIDER_AUTH_NONE,
} provider_auth_type_t;

typedef enum {
    PROVIDER_TRANSPORT_NONE = 0,
    PROVIDER_TRANSPORT_OPENAI_CHAT,
    PROVIDER_TRANSPORT_ANTHROPIC_MESSAGES,
    PROVIDER_TRANSPORT_CODEX_RESPONSES,
    PROVIDER_TRANSPORT_BEDROCK_CONVERSE,
    PROVIDER_TRANSPORT_ACP,
    PROVIDER_TRANSPORT_EXTERNAL_PROCESS,
} provider_transport_kind_t;

enum {
    PROVIDER_CAP_TOOLS                = 1u << 0,
    PROVIDER_CAP_MULTITURN            = 1u << 1,
    PROVIDER_CAP_STREAMING            = 1u << 2,
    PROVIDER_CAP_VISION               = 1u << 3,
    PROVIDER_CAP_VISION_TOOL_MESSAGES = 1u << 4,
    PROVIDER_CAP_REASONING            = 1u << 5,
    PROVIDER_CAP_JSON                 = 1u << 6,
    PROVIDER_CAP_PROMPT_CACHE         = 1u << 7,
    PROVIDER_CAP_HEALTH_CHECK         = 1u << 8,
};

typedef struct {
    const char *name;
    const char *display_name;
    const char *description;

    provider_api_mode_t api_mode;
    provider_auth_type_t auth_type;
    provider_transport_kind_t transport;

    /* Provider/catalog base URL. This can differ from the DSCO transport base. */
    const char *base_url;
    const char *models_url;

    /* Root used by the current DSCO transport, if implemented. For
     * PROVIDER_TRANSPORT_OPENAI_CHAT this is the URL before /chat/completions. */
    const char *transport_base_url;

    const char *env_vars[PROVIDER_PROFILE_MAX_ENV_VARS];
    const char *aliases[PROVIDER_PROFILE_MAX_ALIASES];

    const char *default_model;
    const char *default_aux_model;
    unsigned    caps;
} provider_profile_t;

size_t provider_profile_count(void);
const provider_profile_t *provider_profile_at(size_t index);
const provider_profile_t *provider_profile_find(const char *name_or_alias);
const char *provider_profile_canonical_name(const char *name_or_alias);

const char *provider_api_mode_name(provider_api_mode_t mode);
const char *provider_auth_type_name(provider_auth_type_t auth_type);
const char *provider_transport_kind_name(provider_transport_kind_t transport);

const char *provider_profile_primary_env_var(const provider_profile_t *profile);
bool provider_profile_has_env_var(const provider_profile_t *profile,
                                  const char *env_var);
bool provider_profile_has_alias(const provider_profile_t *profile,
                                const char *alias);
bool provider_profile_transport_supported(const provider_profile_t *profile);

#endif /* DSCO_PROVIDER_PROFILES_H */
