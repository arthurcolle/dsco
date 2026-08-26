#ifndef DSCO_AGENT_UI_COMPONENTS_H
#define DSCO_AGENT_UI_COMPONENTS_H

#include "agent_ui_theme.h"
#include "native_ui.h"

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    AGENT_UI_TONE_NEUTRAL = 0,
    AGENT_UI_TONE_ACCENT,
    AGENT_UI_TONE_SUCCESS,
    AGENT_UI_TONE_WARNING,
    AGENT_UI_TONE_DANGER,
} agent_ui_tone_t;

typedef enum {
    AGENT_UI_COMPONENT_SECTION = 1,
    AGENT_UI_COMPONENT_BADGE,
    AGENT_UI_COMPONENT_MESSAGE,
    AGENT_UI_COMPONENT_REASONING,
    AGENT_UI_COMPONENT_TOOL,
    AGENT_UI_COMPONENT_PLAN_STEP,
    AGENT_UI_COMPONENT_AGENT,
    AGENT_UI_COMPONENT_TOPOLOGY,
    AGENT_UI_COMPONENT_ARTIFACT,
    AGENT_UI_COMPONENT_PERMISSION,
    AGENT_UI_COMPONENT_METRIC,
    AGENT_UI_COMPONENT_QUEUE,
    AGENT_UI_COMPONENT_TIMELINE,
    AGENT_UI_COMPONENT_NOTIFICATION,
    AGENT_UI_COMPONENT_COMMAND,
    AGENT_UI_COMPONENT_COMPOSER,
    AGENT_UI_COMPONENT_CODE,
    AGENT_UI_COMPONENT_THEME_SWATCH,
} agent_ui_component_kind_t;

/* Stable component anatomy. Automation and event wiring should target these
 * part keys rather than visible labels. */
typedef enum {
    AGENT_UI_PART_ROOT = 0,
    AGENT_UI_PART_ICON,
    AGENT_UI_PART_EYEBROW,
    AGENT_UI_PART_TITLE,
    AGENT_UI_PART_BODY,
    AGENT_UI_PART_META,
    AGENT_UI_PART_STATE,
    AGENT_UI_PART_PROGRESS,
    AGENT_UI_PART_PRIMARY_ACTION,
    AGENT_UI_PART_SECONDARY_ACTION,
    AGENT_UI_PART_DATA,
    AGENT_UI_PART_DETAIL,
} agent_ui_component_part_t;

typedef struct {
    native_ui_scene_t *scene;
    const agent_ui_theme_t *theme;
    native_ui_density_t density;
} agent_ui_builder_t;

typedef enum {
    AGENT_UI_MESSAGE_SYSTEM = 0,
    AGENT_UI_MESSAGE_USER,
    AGENT_UI_MESSAGE_ASSISTANT,
} agent_ui_message_kind_t;

typedef struct {
    agent_ui_message_kind_t kind;
    const char *author;
    const char *body;
    const char *meta;
    bool streaming;
    bool selected;
} agent_ui_message_model_t;

typedef struct {
    const char *title;
    const char *detail;
    const char *phase;
    float progress;
    bool live;
    bool expanded;
} agent_ui_reasoning_model_t;

typedef struct {
    const char *tool;
    const char *summary;
    const char *detail;
    const char *duration;
    const char *status;
    agent_ui_tone_t tone;
    float progress;
    bool running;
} agent_ui_tool_model_t;

typedef struct {
    int index;
    const char *title;
    const char *detail;
    const char *status;
    agent_ui_tone_t tone;
    bool selected;
    bool actionable;
} agent_ui_plan_step_model_t;

typedef struct {
    const char *name;
    const char *model;
    const char *task;
    const char *status;
    native_ui_agent_state_t state;
    float context_usage;
    bool selected;
} agent_ui_agent_model_t;

typedef struct {
    const char *title;
    const char *route;
    const char *summary;
    int agents;
    int active_agents;
} agent_ui_topology_model_t;

typedef struct {
    const char *title;
    const char *path;
    const char *summary;
    const char *kind;
    const char *action;
    bool actionable;
} agent_ui_artifact_model_t;

typedef struct {
    const char *title;
    const char *detail;
    const char *scope;
    const char *primary_label;
    const char *secondary_label;
    agent_ui_tone_t risk;
} agent_ui_permission_model_t;

typedef struct {
    const char *label;
    const char *value;
    const char *unit;
    const char *trend;
    const char *series;
    agent_ui_tone_t tone;
} agent_ui_metric_model_t;

typedef struct {
    const char *title;
    const char *detail;
    const char *status;
    int position;
    agent_ui_tone_t tone;
    bool selected;
} agent_ui_queue_model_t;

typedef struct {
    const char *time;
    const char *title;
    const char *detail;
    agent_ui_tone_t tone;
} agent_ui_timeline_model_t;

typedef struct {
    const char *title;
    const char *body;
    const char *action;
    agent_ui_tone_t tone;
    bool toast;
} agent_ui_notification_model_t;

typedef struct {
    const char *command;
    const char *description;
    const char *shortcut;
    bool selected;
    bool disabled;
} agent_ui_command_model_t;

typedef struct {
    const char *text;
    const char *placeholder;
    const char *mode;
    const char *submit_label;
    bool focused;
    bool busy;
} agent_ui_composer_component_model_t;

typedef struct {
    const char *language;
    const char *code;
    const char *meta;
    bool copyable;
} agent_ui_code_model_t;

bool agent_ui_builder_init(agent_ui_builder_t *builder,
                           native_ui_scene_t *scene,
                           const agent_ui_theme_t *theme);
uint64_t agent_ui_component_key(agent_ui_component_kind_t kind,
                                uint32_t instance);
uint64_t agent_ui_component_part_key(uint64_t root_key,
                                     agent_ui_component_part_t part);

int agent_ui_add_section_header(agent_ui_builder_t *builder, int parent,
                                uint64_t key, const char *title,
                                const char *caption, const char *meta);
int agent_ui_add_status_badge(agent_ui_builder_t *builder, int parent,
                              uint64_t key, const char *label,
                              agent_ui_tone_t tone);
int agent_ui_add_message(agent_ui_builder_t *builder, int parent, uint64_t key,
                         const agent_ui_message_model_t *model);
int agent_ui_add_reasoning(agent_ui_builder_t *builder, int parent, uint64_t key,
                           const agent_ui_reasoning_model_t *model);
int agent_ui_add_tool(agent_ui_builder_t *builder, int parent, uint64_t key,
                      const agent_ui_tool_model_t *model);
int agent_ui_add_plan_step(agent_ui_builder_t *builder, int parent, uint64_t key,
                           const agent_ui_plan_step_model_t *model);
int agent_ui_add_agent_card(agent_ui_builder_t *builder, int parent, uint64_t key,
                            const agent_ui_agent_model_t *model);
int agent_ui_add_topology(agent_ui_builder_t *builder, int parent, uint64_t key,
                          const agent_ui_topology_model_t *model);
int agent_ui_add_artifact(agent_ui_builder_t *builder, int parent, uint64_t key,
                          const agent_ui_artifact_model_t *model);
int agent_ui_add_permission(agent_ui_builder_t *builder, int parent, uint64_t key,
                            const agent_ui_permission_model_t *model);
int agent_ui_add_metric(agent_ui_builder_t *builder, int parent, uint64_t key,
                        const agent_ui_metric_model_t *model);
int agent_ui_add_queue_item(agent_ui_builder_t *builder, int parent, uint64_t key,
                            const agent_ui_queue_model_t *model);
int agent_ui_add_timeline_event(agent_ui_builder_t *builder, int parent,
                                uint64_t key,
                                const agent_ui_timeline_model_t *model);
int agent_ui_add_notification(agent_ui_builder_t *builder, int parent,
                              uint64_t key,
                              const agent_ui_notification_model_t *model);
int agent_ui_add_command(agent_ui_builder_t *builder, int parent, uint64_t key,
                         const agent_ui_command_model_t *model);
int agent_ui_add_composer(agent_ui_builder_t *builder, int parent, uint64_t key,
                          const agent_ui_composer_component_model_t *model);
int agent_ui_add_code_block(agent_ui_builder_t *builder, int parent, uint64_t key,
                            const agent_ui_code_model_t *model);
int agent_ui_add_theme_swatch(agent_ui_builder_t *builder, int parent,
                              uint64_t key, const agent_ui_theme_t *theme,
                              bool selected);

#endif /* DSCO_AGENT_UI_COMPONENTS_H */
