#ifndef DSCO_CALLBACKS_H
#define DSCO_CALLBACKS_H

#include <stdbool.h>

typedef struct {
    bool enabled;
    char url[1024];
    char events[512];
    char mode[32];
    int max_payload_bytes;
} callback_policy_t;

bool callback_policy_from_env(callback_policy_t *out);
bool callback_event_matches(const callback_policy_t *policy, const char *event_name);
bool callback_outbox_enqueue(const callback_policy_t *policy,
                             const char *run_id,
                             const char *event_name,
                             const char *event_json);
int callbacks_cli(int argc, char **argv);

#endif
