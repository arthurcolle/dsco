#ifndef DSCO_LOCAL_SMART_ROUTER_H
#define DSCO_LOCAL_SMART_ROUTER_H

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    DSCO_LOCAL_LANE_FAST = 0,
    DSCO_LOCAL_LANE_SMART = 1,
} dsco_local_lane_t;

typedef struct {
    dsco_local_lane_t lane;
    int score;
    int reasoning_budget;
    const char *reason;
    bool forced;
} dsco_local_decision_t;

const char *dsco_local_lane_name(dsco_local_lane_t lane);

/* Classify an OpenAI-compatible chat request. `override_lane` accepts
 * "fast", "smart", or NULL. `smart_threshold` is the last-user-message byte
 * threshold above which the smart lane wins even without a keyword hit. */
dsco_local_lane_t dsco_local_smart_classify(const char *body, size_t body_len,
                                            const char *override_lane,
                                            size_t smart_threshold);

/* Produce the full scored decision used by the router. Returned reason strings
 * have static lifetime. A score of 4 or greater selects the smart lane unless
 * an explicit header/model override forces a lane. */
dsco_local_decision_t dsco_local_smart_decide(const char *body, size_t body_len,
                                               const char *override_lane,
                                               size_t smart_threshold);

/* Return a malloc-owned copy with cache_prompt, enable_thinking, and a bounded
 * reasoning-budget default injected. Explicit caller values always win. */
char *dsco_local_smart_patch_body(const char *body, dsco_local_lane_t lane);

/* Patch using the decision's adaptive reasoning budget. */
char *dsco_local_smart_patch_decision(const char *body,
                                      const dsco_local_decision_t *decision);

#endif /* DSCO_LOCAL_SMART_ROUTER_H */
