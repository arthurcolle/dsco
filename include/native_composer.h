#ifndef DSCO_NATIVE_COMPOSER_H
#define DSCO_NATIVE_COMPOSER_H

#include "native_ui.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NATIVE_COMPOSER_KEY_ROOT       UINT64_C(0x434f4d500001)
#define NATIVE_COMPOSER_KEY_TOP        UINT64_C(0x434f4d500002)
#define NATIVE_COMPOSER_KEY_TITLE      UINT64_C(0x434f4d500003)
#define NATIVE_COMPOSER_KEY_LIVE       UINT64_C(0x434f4d500004)
#define NATIVE_COMPOSER_KEY_DIVIDER    UINT64_C(0x434f4d500005)
#define NATIVE_COMPOSER_KEY_INPUT_ROW  UINT64_C(0x434f4d500006)
#define NATIVE_COMPOSER_KEY_ACCENT     UINT64_C(0x434f4d500007)
#define NATIVE_COMPOSER_KEY_PROMPT     UINT64_C(0x434f4d500008)
#define NATIVE_COMPOSER_KEY_INPUT      UINT64_C(0x434f4d500009)
#define NATIVE_COMPOSER_KEY_FOOTER     UINT64_C(0x434f4d50000a)
#define NATIVE_COMPOSER_KEY_HINT       UINT64_C(0x434f4d50000b)
#define NATIVE_COMPOSER_KEY_CLOCK      UINT64_C(0x434f4d50000c)
#define NATIVE_COMPOSER_KEY_EXEC_GLYPH UINT64_C(0x434f4d50000d)

typedef struct {
    const char *text;
    size_t cursor;
    bool active;
    native_ui_agent_state_t agent_state;
    int columns;
    int max_rows;
    int queue_depth;
    int queue_capacity;
    const char *clock;
    bool compact;
    uint8_t accent_opacity;
    /* Exec ticker: ephemeral tool liveness projected onto the top strip.
     * The host composes the text; the composer only owns presentation. */
    const char *exec_text;   /* composed ticker, NULL/empty when idle */
    const char *exec_label;  /* accessibility narration, optional */
    uint8_t exec_kind;       /* 0 idle, 1 running, 2 done-flash, 3 error-flash */
    float   exec_phase;      /* 0..1 spinner phase */
    uint8_t exec_flash;      /* 0..255 resolve intensity */
} native_composer_model_t;

/* Builds the retained presentation for the shared cell editor. Editing,
 * history, paste, and submission remain authoritative in tui.c; this scene
 * owns compositor layout, semantics, damage, and accessibility. */
bool native_composer_build(native_ui_scene_t *scene, int width, int height,
                           const native_composer_model_t *model);

#endif /* DSCO_NATIVE_COMPOSER_H */
