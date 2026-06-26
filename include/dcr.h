#ifndef DSCO_DCR_H
#define DSCO_DCR_H

#include "provider_profiles.h"
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    char key[128];
    char value[512];
    char source[128];
    char source_url[512];
    double confidence;
    bool validated;
} dcr_atom_t;

typedef struct {
    provider_profile_t profile;
    char name[64];
    char display_name[128];
    char description[512];
    char base_url[512];
    char models_url[512];
    char transport_base_url[512];
    char env_vars[PROVIDER_PROFILE_MAX_ENV_VARS][64];
    char aliases[PROVIDER_PROFILE_MAX_ALIASES][64];
    char default_model[128];
    char default_aux_model[128];

    char wire_api[64];
    long stream_idle_timeout_ms;
    int stream_max_retries;
    int request_max_retries;
    bool idempotent_retries;
    char provenance_source[128];
    char provenance_url[512];
    double confidence;
    bool loaded;
} dcr_provider_t;

typedef struct {
    char id[128];
    char provider[64];
    char display_name[128];
    char wire_model[128];
    char aliases[8][128];
    size_t alias_count;
    int context_window;
    int max_output_tokens;
    bool supports_reasoning;
    char reasoning_efforts[8][32];
    size_t reasoning_effort_count;
    char effort_alias_from[8][32];
    char effort_alias_to[8][32];
    size_t effort_alias_count;
    char provenance_source[128];
    double confidence;
    bool loaded;
} dcr_model_t;

void dcr_init(void);
void dcr_reload(void);
bool dcr_is_loaded(void);

size_t dcr_provider_count(void);
const dcr_provider_t *dcr_provider_at(size_t idx);
const dcr_provider_t *dcr_provider_find(const char *name_or_alias);
const provider_profile_t *dcr_provider_profile_find(const char *name_or_alias);
const char *dcr_provider_primary_env_var(const dcr_provider_t *p);
bool dcr_provider_has_env_var(const dcr_provider_t *p, const char *env_var);
const char *dcr_provider_wire_api(const char *provider);
long dcr_provider_stream_idle_timeout_ms(const char *provider, long fallback_ms);
int dcr_provider_stream_max_retries(const char *provider, int fallback);
int dcr_provider_request_max_retries(const char *provider, int fallback);

size_t dcr_model_count(void);
const dcr_model_t *dcr_model_at(size_t idx);
const dcr_model_t *dcr_model_find(const char *id_or_alias);
const char *dcr_reasoning_effort_normalize(const char *provider, const char *model, const char *effort,
                                           char *out, size_t out_len);

int dcr_cli(int argc, char **argv);

#endif /* DSCO_DCR_H */
