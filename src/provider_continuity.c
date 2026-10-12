#include "provider_continuity.h"
#include "subscription_bench.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>

bool provider_continuity_enabled(void) {
    const char *v = getenv("DSCO_CROSS_PROVIDER_FALLBACK");
    return v && (!strcmp(v, "1") || !strcasecmp(v, "true"));
}

const char *provider_continuity_provider(const char *model) {
    if (!model || !provider_continuity_enabled()) return NULL;
    for (size_t i = 0; i < dsco_subscription_lane_count(); i++) {
        const dsco_subscription_lane_spec_t *lane = dsco_subscription_lane_at(i);
        if (lane && !strcmp(model, lane->model)) return lane->provider;
    }
    return NULL;
}

int provider_continuity_models(const char *current_provider, char models[][128], int cap) {
    if (!provider_continuity_enabled() || !models || cap <= 0) return 0;
    int count = 0;
    for (size_t i = 0; i < dsco_subscription_lane_count() && count < cap; i++) {
        const dsco_subscription_lane_spec_t *lane = dsco_subscription_lane_at(i);
        if (!lane || (current_provider && !strcmp(lane->provider, current_provider))) continue;
        const char *allow = getenv("DSCO_CONTINUITY_PROVIDERS");
        if (allow && *allow) {
            bool found = false;
            const char *at = allow;
            while (*at) {
                const char *end = strchr(at, ',');
                size_t n = end ? (size_t)(end - at) : strlen(at);
                if (n == strlen(lane->provider) && !strncmp(at, lane->provider, n)) found = true;
                if (!end) break;
                at = end + 1;
            }
            if (!found) continue;
        }
        char auth[64], endpoint[512], reason[96];
        if (!dsco_subscription_lane_native_ready(lane, NULL, auth, sizeof(auth),
                                                endpoint, sizeof(endpoint), reason, sizeof(reason))) continue;
        snprintf(models[count++], 128, "%s", lane->model);
    }
    return count;
}
