#ifndef DSCO_INTEGRATION_FABRIC_H
#define DSCO_INTEGRATION_FABRIC_H

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    DSCO_INTEGRATION_SCOPE_UNKNOWN = 0,
    DSCO_INTEGRATION_SCOPE_PUBLIC_APP,
    DSCO_INTEGRATION_SCOPE_PRIVATE_SERVICE,
    DSCO_INTEGRATION_SCOPE_INFRASTRUCTURE,
    DSCO_INTEGRATION_SCOPE_AGENTIC,
} dsco_integration_scope_t;

typedef enum {
    DSCO_INTEGRATION_ACTION_NONE                  = 0u,
    DSCO_INTEGRATION_ACTION_READ                  = 1u << 0,
    DSCO_INTEGRATION_ACTION_WRITE                 = 1u << 1,
    DSCO_INTEGRATION_ACTION_SEND                  = 1u << 2,
    DSCO_INTEGRATION_ACTION_DELETE                = 1u << 3,
    DSCO_INTEGRATION_ACTION_ADMIN                 = 1u << 4,
    DSCO_INTEGRATION_ACTION_UNTRUSTED_CONTENT     = 1u << 5,
    DSCO_INTEGRATION_ACTION_REQUIRES_CONFIRMATION = 1u << 6,
    DSCO_INTEGRATION_ACTION_INTERACTIVE           = 1u << 7,
} dsco_integration_action_t;

typedef struct {
    const char *id;
    const char *display_name;
    dsco_integration_scope_t scope;
    unsigned default_actions;
    bool codex_app;
    bool private_layer;
} dsco_integration_profile_t;

const dsco_integration_profile_t *dsco_integration_profiles(size_t *count);
const dsco_integration_profile_t *dsco_integration_profile_for_server(const char *server_name);
const dsco_integration_profile_t *dsco_integration_profile_for_tool(const char *tool_name);
unsigned dsco_integration_actions_for_catalog_labels(bool retrievable, bool sync,
                                                    bool consequential, bool interactive);
unsigned dsco_integration_actions_for_tool(const char *tool_name);
bool dsco_integration_requires_confirmation(const char *tool_name);
bool dsco_integration_action_has(unsigned actions, dsco_integration_action_t action);
const char *dsco_integration_scope_name(dsco_integration_scope_t scope);

#endif /* DSCO_INTEGRATION_FABRIC_H */
