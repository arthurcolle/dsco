#ifndef DSCO_AGENT_PROFILE_H
#define DSCO_AGENT_PROFILE_H

#include <stdbool.h>
#include <stddef.h>

#define AP_MAX_TOOLS       64
#define AP_MAX_GROUPS      16
#define AP_NAME_LEN        64
#define AP_DESC_LEN        256
#define AP_PROMPT_LEN      512
#define AP_MODEL_LEN       128
#define MAX_AGENT_PROFILES 64

/* Tool groups available for filtering */
#define AP_ALL_GROUPS "file_io,git,network,shell,code,crypto,swarm,ast,pipeline,math,search,general,market,prediction,memory"

typedef struct {
    char   name[AP_NAME_LEN];
    char   description[AP_DESC_LEN];
    /* Tool whitelist: tool_count == 0 means use group filter only */
    char   tools[AP_MAX_TOOLS][64];
    int    tool_count;
    /* Group whitelist: group_count == 0 means all groups allowed */
    char   groups[AP_MAX_GROUPS][32];
    int    group_count;
    /* Optional model preference, "" = use session model */
    char   model_hint[AP_MODEL_LEN];
    /* Injected system prompt prefix */
    char   prompt_prefix[AP_PROMPT_LEN];
    /* Budget cap in USD, 0.0 = no cap */
    double budget_usd;
} agent_profile_t;

/* Registry */
void              agent_profiles_init(void);
int               agent_profiles_count(void);
agent_profile_t  *agent_profile_get(int idx);
agent_profile_t  *agent_profile_find(const char *name);
bool              agent_profile_save(const agent_profile_t *p);
bool              agent_profile_delete(const char *name);

/* Persistence: ~/.dsco/agent_profiles.json */
bool              agent_profiles_load(void);
bool              agent_profiles_persist(void);

/* JSON */
int               agent_profile_to_json(const agent_profile_t *p, char *buf, size_t len);
int               agent_profiles_all_json(char *buf, size_t len);
bool              agent_profile_from_json(agent_profile_t *p, const char *json);

/* Active profile — controls tool filter globally */
void                    agent_profile_set_active(const char *name);  /* NULL = disable */
const char             *agent_profile_active_name(void);
const agent_profile_t  *agent_profile_active(void);

/* Returns true if tool is allowed by the active profile.
 * Always true when no profile is active (tool_count == 0 && group_count == 0).
 * group_hint: optional group name (may be NULL), used for group-based filtering. */
bool agent_profile_tool_allowed(const char *tool_name, const char *group_hint);
bool agent_profile_tool_allowed_strict(const char *tool_name, const char *group_hint);

#endif /* DSCO_AGENT_PROFILE_H */
