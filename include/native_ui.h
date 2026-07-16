#ifndef DSCO_NATIVE_UI_H
#define DSCO_NATIVE_UI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Backend-neutral retained UI primitives for agent-native surfaces.  The
 * scene owns its labels and uses fixed storage so composing a frame does not
 * allocate.  Backends translate semantic tokens into pixels, cells, DOM, or
 * accessibility nodes. */

#define NATIVE_UI_MAX_NODES 256
#define NATIVE_UI_MAX_DIRTY_REGIONS 32
#define NATIVE_UI_TEXT_CAP 192
#define NATIVE_UI_LABEL_CAP 96
#define NATIVE_UI_COMPOSER_MAX_ROWS 8

typedef struct { int x, y, width, height; } native_ui_rect_t;
typedef struct { int top, right, bottom, left; } native_ui_insets_t;

typedef enum {
    NATIVE_UI_DENSITY_COMPACT = 0,
    NATIVE_UI_DENSITY_DENSE,
    NATIVE_UI_DENSITY_EXPANDED,
} native_ui_density_t;

/* Primitive rendering vocabulary. */
typedef enum {
    NATIVE_UI_ELEMENT_ROOT = 0,
    NATIVE_UI_ELEMENT_SURFACE,
    NATIVE_UI_ELEMENT_STACK,
    NATIVE_UI_ELEMENT_ROW,
    NATIVE_UI_ELEMENT_GRID,
    NATIVE_UI_ELEMENT_OVERLAY,
    NATIVE_UI_ELEMENT_SCROLL,
    NATIVE_UI_ELEMENT_TEXT,
    NATIVE_UI_ELEMENT_ICON,
    NATIVE_UI_ELEMENT_RULE,
    NATIVE_UI_ELEMENT_BADGE,
    NATIVE_UI_ELEMENT_METER,
    NATIVE_UI_ELEMENT_SPARKLINE,
    NATIVE_UI_ELEMENT_INPUT,
    NATIVE_UI_ELEMENT_IMAGE,
    NATIVE_UI_ELEMENT_CUSTOM,
} native_ui_element_t;

/* Meaning exposed to focus, accessibility, automation, and agent backends.
 * A role is deliberately independent from how an element is drawn. */
typedef enum {
    NATIVE_UI_ROLE_NONE = 0,
    NATIVE_UI_ROLE_AGENT_SHELL,
    NATIVE_UI_ROLE_HEADER,
    NATIVE_UI_ROLE_STATUS,
    NATIVE_UI_ROLE_TRANSCRIPT,
    NATIVE_UI_ROLE_MESSAGE,
    NATIVE_UI_ROLE_REASONING_ACTIVITY,
    NATIVE_UI_ROLE_TOOL_ACTIVITY,
    NATIVE_UI_ROLE_AGENT_TOPOLOGY,
    NATIVE_UI_ROLE_ARTIFACT,
    NATIVE_UI_ROLE_PERMISSION,
    NATIVE_UI_ROLE_PLAN,
    NATIVE_UI_ROLE_QUEUE,
    NATIVE_UI_ROLE_TIMELINE,
    NATIVE_UI_ROLE_COMPOSER,
    NATIVE_UI_ROLE_COMMAND_PALETTE,
    NATIVE_UI_ROLE_MARKDOWN,
    NATIVE_UI_ROLE_MATH,
    NATIVE_UI_ROLE_CODE,
    NATIVE_UI_ROLE_NOTIFICATION,
    NATIVE_UI_ROLE_TOAST,
    NATIVE_UI_ROLE_MODAL,
    NATIVE_UI_ROLE_SCRIM,
} native_ui_role_t;

typedef enum {
    NATIVE_UI_AGENT_IDLE = 0,
    NATIVE_UI_AGENT_REASONING,
    NATIVE_UI_AGENT_EXECUTING,
    NATIVE_UI_AGENT_RESPONDING,
    NATIVE_UI_AGENT_WAITING,
    NATIVE_UI_AGENT_BLOCKED,
    NATIVE_UI_AGENT_ERROR,
} native_ui_agent_state_t;

typedef enum {
    NATIVE_UI_FLOW_NONE = 0,
    NATIVE_UI_FLOW_COLUMN,
    NATIVE_UI_FLOW_ROW,
    NATIVE_UI_FLOW_GRID,
    NATIVE_UI_FLOW_OVERLAY,
} native_ui_flow_t;

typedef enum {
    NATIVE_UI_ALIGN_START = 0,
    NATIVE_UI_ALIGN_CENTER,
    NATIVE_UI_ALIGN_END,
    NATIVE_UI_ALIGN_STRETCH,
} native_ui_align_t;

typedef enum {
    NATIVE_UI_COLOR_CLEAR = 0,
    NATIVE_UI_COLOR_CANVAS,
    NATIVE_UI_COLOR_SURFACE,
    NATIVE_UI_COLOR_SURFACE_RAISED,
    NATIVE_UI_COLOR_TEXT,
    NATIVE_UI_COLOR_TEXT_MUTED,
    NATIVE_UI_COLOR_ACCENT,
    NATIVE_UI_COLOR_SUCCESS,
    NATIVE_UI_COLOR_WARNING,
    NATIVE_UI_COLOR_DANGER,
    NATIVE_UI_COLOR_BORDER,
    NATIVE_UI_COLOR_FOCUS,
    NATIVE_UI_COLOR_COUNT,
} native_ui_color_token_t;

typedef enum {
    NATIVE_UI_TYPE_BODY = 0,
    NATIVE_UI_TYPE_LABEL,
    NATIVE_UI_TYPE_TITLE,
    NATIVE_UI_TYPE_CODE,
    NATIVE_UI_TYPE_MATH,
    NATIVE_UI_TYPE_METRIC,
} native_ui_type_token_t;

enum {
    NATIVE_UI_STATE_VISIBLE = 1u << 0,
    NATIVE_UI_STATE_ENABLED = 1u << 1,
    NATIVE_UI_STATE_FOCUSABLE = 1u << 2,
    NATIVE_UI_STATE_FOCUSED = 1u << 3,
    NATIVE_UI_STATE_HOVERED = 1u << 4,
    NATIVE_UI_STATE_PRESSED = 1u << 5,
    NATIVE_UI_STATE_SELECTED = 1u << 6,
    NATIVE_UI_STATE_LIVE = 1u << 7,
    NATIVE_UI_STATE_CLIPS = 1u << 8,
    NATIVE_UI_STATE_DISABLED = 1u << 9,
};

typedef struct {
    int min_width, min_height;
    int preferred_width, preferred_height; /* -1 means intrinsic/flexible. */
    int max_width, max_height;             /* -1 means unbounded. */
    uint16_t grow;
    uint16_t shrink;
} native_ui_constraints_t;

typedef struct {
    native_ui_flow_t flow;
    native_ui_align_t align;
    native_ui_align_t justify;
    native_ui_insets_t padding;
    int gap;
    int grid_columns;
    int z_index;
    native_ui_color_token_t foreground;
    native_ui_color_token_t background;
    native_ui_color_token_t border;
    native_ui_type_token_t type;
    uint8_t opacity;
    uint8_t border_width;
    uint8_t radius;
} native_ui_style_t;

typedef enum {
    NATIVE_UI_EVENT_NONE = 0,
    NATIVE_UI_EVENT_POINTER_MOVE,
    NATIVE_UI_EVENT_POINTER_DOWN,
    NATIVE_UI_EVENT_POINTER_UP,
    NATIVE_UI_EVENT_KEY_DOWN,
    NATIVE_UI_EVENT_TEXT,
    NATIVE_UI_EVENT_SCROLL,
    NATIVE_UI_EVENT_RESIZE,
    NATIVE_UI_EVENT_TICK,
    NATIVE_UI_EVENT_AGENT_STATE,
} native_ui_event_type_t;

typedef struct {
    native_ui_event_type_t type;
    int x, y;
    int key;
    int delta_x, delta_y;
    uint32_t modifiers;
    const char *text;
    double time_seconds;
} native_ui_event_t;

struct native_ui_scene;
struct native_ui_node;
typedef bool (*native_ui_event_handler_t)(struct native_ui_scene *scene,
                                          struct native_ui_node *node,
                                          const native_ui_event_t *event,
                                          void *context);

typedef struct native_ui_node {
    uint64_t key;
    native_ui_element_t element;
    native_ui_role_t role;
    native_ui_agent_state_t agent_state;
    uint32_t state;
    int parent;
    int first_child;
    int last_child;
    int next_sibling;
    native_ui_constraints_t constraints;
    native_ui_style_t style;
    native_ui_rect_t frame;
    float value;
    char text[NATIVE_UI_TEXT_CAP];
    char accessibility_label[NATIVE_UI_LABEL_CAP];
    native_ui_event_handler_t on_event;
    void *event_context;
} native_ui_node_t;

typedef struct native_ui_scene {
    native_ui_node_t nodes[NATIVE_UI_MAX_NODES];
    int count;
    int root;
    int focused;
    native_ui_rect_t viewport;
    native_ui_density_t density;
    uint64_t generation;
} native_ui_scene_t;

typedef struct {
    native_ui_rect_t regions[NATIVE_UI_MAX_DIRTY_REGIONS];
    int count;
    bool full_repaint;
} native_ui_damage_t;

typedef struct {
    native_ui_density_t density;
    bool shows_inspector;
    int outer_margin;
    int inner_padding;
    int gap;
    native_ui_rect_t header;
    native_ui_rect_t transcript;
    native_ui_rect_t inspector;
    native_ui_rect_t composer;
} native_ui_agent_shell_layout_t;

/* Terminal APIs frequently report backing pixels while compositor layout is
 * expressed in logical pixels. Keeping both spaces explicit prevents Retina
 * grids from accidentally selecting desktop breakpoints and half-size type. */
typedef struct {
    int columns, rows;
    int physical_width, physical_height;
    int logical_width, logical_height;
    int physical_cell_width, physical_cell_height;
    int backing_scale;
} native_ui_viewport_metrics_t;

/* Backend-neutral wrapping for the persistent composer. Byte offsets always
 * point into the caller-owned UTF-8 string; a backend can shape each visible
 * row with its own font while sharing cursor/scroll behavior with ANSI. */
typedef struct {
    size_t byte_start;
    size_t byte_end;
} native_ui_composer_row_t;

typedef struct {
    native_ui_composer_row_t rows[NATIVE_UI_COMPOSER_MAX_ROWS];
    int row_count;
    int total_rows;
    int first_row;
    int cursor_row;
    int cursor_column;
} native_ui_composer_layout_t;

/* Rendering stays backend-neutral. Any callback may be NULL. */
typedef struct {
    void (*begin_frame)(void *context, native_ui_rect_t viewport,
                        const native_ui_damage_t *damage);
    void (*push_clip)(void *context, native_ui_rect_t rect);
    void (*pop_clip)(void *context);
    void (*fill_rect)(void *context, native_ui_rect_t rect,
                      native_ui_color_token_t color, uint8_t opacity, uint8_t radius);
    void (*stroke_rect)(void *context, native_ui_rect_t rect,
                        native_ui_color_token_t color, uint8_t width, uint8_t radius);
    void (*draw_text)(void *context, native_ui_rect_t rect, const char *text,
                      native_ui_type_token_t type, native_ui_color_token_t color,
                      uint8_t opacity);
    void (*draw_icon)(void *context, native_ui_rect_t rect, const char *name,
                      native_ui_color_token_t color, uint8_t opacity);
    void (*draw_custom)(void *context, const native_ui_node_t *node);
    void (*end_frame)(void *context);
} native_ui_backend_t;

void native_ui_scene_init(native_ui_scene_t *scene, int width, int height);
int native_ui_scene_add(native_ui_scene_t *scene, int parent, uint64_t key,
                        native_ui_element_t element, native_ui_role_t role);
native_ui_node_t *native_ui_scene_node(native_ui_scene_t *scene, int index);
void native_ui_node_set_text(native_ui_node_t *node, const char *text);
void native_ui_node_set_accessibility_label(native_ui_node_t *node, const char *label);
void native_ui_layout(native_ui_scene_t *scene);
void native_ui_diff(const native_ui_scene_t *previous, const native_ui_scene_t *current,
                    native_ui_damage_t *damage);
int native_ui_hit_test(const native_ui_scene_t *scene, int x, int y);
int native_ui_focus_move(native_ui_scene_t *scene, bool backwards);
bool native_ui_dispatch(native_ui_scene_t *scene, const native_ui_event_t *event);
void native_ui_render(const native_ui_scene_t *scene, const native_ui_scene_t *previous,
                      const native_ui_backend_t *backend, void *context);

/* Compile a declarative JSON UI spec into a laid-out scene (generative UI:
 * agents emit structure as data; the compositor owns layout and rendering).
 * Returns the node count, or -1 on malformed input. Implemented in
 * native_ui_json.c; see that file for the accepted spec shape. */
int native_ui_scene_from_json(native_ui_scene_t *scene, const char *json,
                              int width, int height);

native_ui_density_t native_ui_density_for_size(int width, int height);
native_ui_viewport_metrics_t native_ui_terminal_viewport(int columns, int rows,
                                                         int physical_width,
                                                         int physical_height,
                                                         int requested_scale);
native_ui_agent_shell_layout_t native_ui_agent_shell_layout(int width, int height);
native_ui_composer_layout_t native_ui_composer_layout(const char *text,
                                                      size_t cursor_byte,
                                                      int columns,
                                                      int max_rows);
const char *native_ui_role_name(native_ui_role_t role);
const char *native_ui_agent_state_name(native_ui_agent_state_t state);

#endif /* DSCO_NATIVE_UI_H */
