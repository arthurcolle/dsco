#ifndef DSCO_NATIVE_MASTHEAD_H
#define DSCO_NATIVE_MASTHEAD_H

#include "native_ui.h"

#include <stdbool.h>
#include <stdint.h>

/* Stable identities for the first live session region migrated onto the
 * retained compositor. They are public so accessibility, tests, and future
 * backends can target semantic elements without scraping visible strings. */
#define NATIVE_MASTHEAD_KEY_ROOT       UINT64_C(0x4d4153540001)
#define NATIVE_MASTHEAD_KEY_ACCENT     UINT64_C(0x4d4153540002)
#define NATIVE_MASTHEAD_KEY_SOUL       UINT64_C(0x4d4153540003)
#define NATIVE_MASTHEAD_KEY_IDENTITY   UINT64_C(0x4d4153540004)
#define NATIVE_MASTHEAD_KEY_TITLE      UINT64_C(0x4d4153540005)
#define NATIVE_MASTHEAD_KEY_DETAILS    UINT64_C(0x4d4153540006)
#define NATIVE_MASTHEAD_KEY_MODEL      UINT64_C(0x4d4153540007)
#define NATIVE_MASTHEAD_KEY_METRICS    UINT64_C(0x4d4153540008)
#define NATIVE_MASTHEAD_KEY_RULE       UINT64_C(0x4d4153540009)
#define NATIVE_MASTHEAD_KEY_STATUS     UINT64_C(0x4d415354000a)
#define NATIVE_MASTHEAD_KEY_TURN       UINT64_C(0x4d415354000b)
#define NATIVE_MASTHEAD_KEY_STATE      UINT64_C(0x4d415354000c)

typedef struct {
    const char *title;
    const char *model;
    const char *slot;
    const char *state_label;
    native_ui_agent_state_t state;
    int turn;
    int input_tokens;
    int output_tokens;
    int tools_used;
    int queue_depth;
    int queue_capacity;
    double context_percent;
    double cost_usd;
    bool show_compact_metrics;
} native_masthead_model_t;

/* Builds and lays out a complete retained masthead scene. The scene is
 * caller-owned and contains no pointers into the model after return. */
bool native_masthead_build(native_ui_scene_t *scene, int width, int height,
                           const native_masthead_model_t *model);

#endif /* DSCO_NATIVE_MASTHEAD_H */
