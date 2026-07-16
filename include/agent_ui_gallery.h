#ifndef DSCO_AGENT_UI_GALLERY_H
#define DSCO_AGENT_UI_GALLERY_H

#include "agent_ui_theme.h"
#include "native_ui.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

typedef enum {
    AGENT_UI_GALLERY_WORKBENCH = 0,
    AGENT_UI_GALLERY_FOUNDATIONS,
    AGENT_UI_GALLERY_CONVERSATION,
    AGENT_UI_GALLERY_EXECUTION,
    AGENT_UI_GALLERY_ORCHESTRATION,
    AGENT_UI_GALLERY_GOVERNANCE,
    AGENT_UI_GALLERY_THEMES,
    AGENT_UI_GALLERY_PAGE_COUNT,
} agent_ui_gallery_page_t;

typedef struct {
    uint32_t image_id;
    bool active;
    int forced_width;
    int forced_height;
    int forced_scale;
    int physical_width;
    int physical_height;
    int columns;
    int rows;
    int backing_scale;
} agent_ui_gallery_session_t;

const char *agent_ui_gallery_page_name(agent_ui_gallery_page_t page);
bool agent_ui_gallery_build(native_ui_scene_t *scene, int logical_width,
                            int logical_height, agent_ui_gallery_page_t page,
                            const agent_ui_theme_t *theme);
bool agent_ui_gallery_write_ppm(const char *path, int physical_width,
                                int physical_height, int backing_scale,
                                agent_ui_gallery_page_t page,
                                const agent_ui_theme_t *theme);

bool agent_ui_gallery_session_begin(agent_ui_gallery_session_t *session,
                                    FILE *out, int forced_width,
                                    int forced_height, int forced_scale);
bool agent_ui_gallery_session_present(agent_ui_gallery_session_t *session,
                                      FILE *out,
                                      agent_ui_gallery_page_t page,
                                      const agent_ui_theme_t *theme);
bool agent_ui_gallery_session_geometry_changed(agent_ui_gallery_session_t *session,
                                               FILE *out);
void agent_ui_gallery_session_end(agent_ui_gallery_session_t *session,
                                  FILE *out);

#endif /* DSCO_AGENT_UI_GALLERY_H */
