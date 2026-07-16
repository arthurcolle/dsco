#include "agent_ui_components.h"

#include <stdio.h>
#include <string.h>

#define INTERNAL_SLOT_BASE 32U

static bool builder_ok(const agent_ui_builder_t *builder, int parent) {
    return builder && builder->scene && builder->theme && parent >= 0 &&
           parent < builder->scene->count;
}

static uint64_t slot_key(uint64_t root_key, unsigned slot) {
    uint64_t mixed = UINT64_C(0x9e3779b97f4a7c15) +
                     (uint64_t)(slot + 1U) * UINT64_C(0x100000001b3);
    uint64_t key = root_key ^ mixed;
    return key ? key : mixed;
}

uint64_t agent_ui_component_key(agent_ui_component_kind_t kind,
                                uint32_t instance) {
    return UINT64_C(0xa600000000000000) |
           ((uint64_t)((unsigned)kind & 0xffU) << 40) |
           ((uint64_t)instance << 8) | UINT64_C(0x5a);
}

uint64_t agent_ui_component_part_key(uint64_t root_key,
                                     agent_ui_component_part_t part) {
    return part == AGENT_UI_PART_ROOT ? root_key : slot_key(root_key, (unsigned)part);
}

static int add_raw(agent_ui_builder_t *builder, int parent, uint64_t key,
                   native_ui_element_t element, native_ui_role_t role) {
    if (!builder_ok(builder, parent)) return -1;
    return native_ui_scene_add(builder->scene, parent, key, element, role);
}

static native_ui_node_t *node_at(agent_ui_builder_t *builder, int index) {
    return builder && builder->scene ? native_ui_scene_node(builder->scene, index) : NULL;
}

static int add_part(agent_ui_builder_t *builder, int parent, uint64_t root_key,
                    agent_ui_component_part_t part,
                    native_ui_element_t element, native_ui_role_t role) {
    return add_raw(builder, parent, agent_ui_component_part_key(root_key, part),
                   element, role);
}

static int add_internal(agent_ui_builder_t *builder, int parent, uint64_t root_key,
                        unsigned slot, native_ui_element_t element,
                        native_ui_role_t role) {
    return add_raw(builder, parent, slot_key(root_key, INTERNAL_SLOT_BASE + slot),
                   element, role);
}

static void fixed_width(native_ui_node_t *node, int width) {
    if (!node) return;
    if (width < 0) width = 0;
    node->constraints.min_width = width;
    node->constraints.preferred_width = width;
    node->constraints.max_width = width;
    node->constraints.shrink = 0;
}

static void fixed_height(native_ui_node_t *node, int height) {
    if (!node) return;
    if (height < 0) height = 0;
    node->constraints.min_height = height;
    node->constraints.preferred_height = height;
    node->constraints.max_height = height;
    node->constraints.shrink = 0;
}

static int density_height(const agent_ui_builder_t *builder, int dense) {
    if (!builder) return dense;
    if (builder->density == NATIVE_UI_DENSITY_COMPACT)
        return dense * 4 / 5;
    if (builder->density == NATIVE_UI_DENSITY_EXPANDED)
        return dense * 6 / 5;
    return dense;
}

static native_ui_color_token_t tone_color(agent_ui_tone_t tone) {
    switch (tone) {
        case AGENT_UI_TONE_ACCENT: return NATIVE_UI_COLOR_ACCENT;
        case AGENT_UI_TONE_SUCCESS: return NATIVE_UI_COLOR_SUCCESS;
        case AGENT_UI_TONE_WARNING: return NATIVE_UI_COLOR_WARNING;
        case AGENT_UI_TONE_DANGER: return NATIVE_UI_COLOR_DANGER;
        case AGENT_UI_TONE_NEUTRAL: default: return NATIVE_UI_COLOR_TEXT_MUTED;
    }
}

static void set_text(native_ui_node_t *node, const char *text,
                     native_ui_type_token_t type,
                     native_ui_color_token_t color, uint8_t opacity) {
    if (!node) return;
    native_ui_node_set_text(node, text ? text : "");
    node->style.type = type;
    node->style.foreground = color;
    node->style.opacity = opacity;
}

static void set_a11y(native_ui_node_t *node, const char *prefix,
                     const char *value) {
    if (!node) return;
    char label[NATIVE_UI_LABEL_CAP];
    if (value && *value)
        snprintf(label, sizeof(label), "%s: %s", prefix ? prefix : "Item", value);
    else
        snprintf(label, sizeof(label), "%s", prefix ? prefix : "Item");
    native_ui_node_set_accessibility_label(node, label);
}

static void style_card(agent_ui_builder_t *builder, native_ui_node_t *node,
                       native_ui_flow_t flow, bool raised) {
    if (!builder || !node) return;
    node->style.flow = flow;
    node->style.align = NATIVE_UI_ALIGN_STRETCH;
    node->style.padding = (native_ui_insets_t){
        builder->theme->spacing.md, builder->theme->spacing.md,
        builder->theme->spacing.md, builder->theme->spacing.md,
    };
    node->style.gap = builder->theme->spacing.sm;
    node->style.background = raised ? NATIVE_UI_COLOR_SURFACE_RAISED
                                    : NATIVE_UI_COLOR_SURFACE;
    node->style.border = NATIVE_UI_COLOR_BORDER;
    node->style.border_width = 1;
    node->style.radius = (uint8_t)builder->theme->radius.md;
    node->style.opacity = 245;
    node->state |= NATIVE_UI_STATE_CLIPS;
}

static void mark_selected(native_ui_node_t *node, bool selected) {
    if (!node || !selected) return;
    node->state |= NATIVE_UI_STATE_SELECTED;
    node->style.border = NATIVE_UI_COLOR_FOCUS;
    node->style.border_width = 2;
}

static int label_width(const char *label, int minimum, int maximum) {
    int width = (int)strlen(label ? label : "") * 7 + 18;
    if (width < minimum) width = minimum;
    if (maximum > 0 && width > maximum) width = maximum;
    return width;
}

static int add_badge_node(agent_ui_builder_t *builder, int parent, uint64_t key,
                          const char *label, agent_ui_tone_t tone,
                          native_ui_role_t role, bool focusable) {
    int index = add_raw(builder, parent, key, NATIVE_UI_ELEMENT_BADGE, role);
    native_ui_node_t *node = node_at(builder, index);
    if (!node) return -1;
    fixed_width(node, label_width(label, 42, 132));
    fixed_height(node, density_height(builder, 22));
    set_text(node, label, NATIVE_UI_TYPE_LABEL,
             tone == AGENT_UI_TONE_NEUTRAL ? NATIVE_UI_COLOR_TEXT
                                           : tone_color(tone), 238);
    node->style.background = NATIVE_UI_COLOR_SURFACE_RAISED;
    node->style.border = tone_color(tone);
    node->style.border_width = 1;
    node->style.radius = (uint8_t)builder->theme->radius.pill;
    if (focusable) node->state |= NATIVE_UI_STATE_FOCUSABLE;
    set_a11y(node, focusable ? "Action" : "Status", label);
    return index;
}

bool agent_ui_builder_init(agent_ui_builder_t *builder,
                           native_ui_scene_t *scene,
                           const agent_ui_theme_t *theme) {
    if (!builder || !scene || scene->count < 1) return false;
    *builder = (agent_ui_builder_t){
        .scene = scene,
        .theme = theme ? theme : agent_ui_theme_default(),
        .density = scene->density,
    };
    return true;
}

int agent_ui_add_section_header(agent_ui_builder_t *builder, int parent,
                                uint64_t key, const char *title,
                                const char *caption, const char *meta) {
    int root = add_raw(builder, parent, key, NATIVE_UI_ELEMENT_ROW,
                       NATIVE_UI_ROLE_HEADER);
    native_ui_node_t *root_node = node_at(builder, root);
    if (!root_node) return -1;
    fixed_height(root_node, density_height(builder, 42));
    root_node->style.flow = NATIVE_UI_FLOW_ROW;
    root_node->style.align = NATIVE_UI_ALIGN_CENTER;
    root_node->style.gap = builder->theme->spacing.md;

    int stack = add_internal(builder, root, key, 0, NATIVE_UI_ELEMENT_STACK,
                             NATIVE_UI_ROLE_HEADER);
    native_ui_node_t *stack_node = node_at(builder, stack);
    if (!stack_node) return -1;
    stack_node->constraints.grow = 1;
    stack_node->style.flow = NATIVE_UI_FLOW_COLUMN;
    stack_node->style.gap = 1;
    int title_index = add_part(builder, stack, key, AGENT_UI_PART_TITLE,
                               NATIVE_UI_ELEMENT_TEXT, NATIVE_UI_ROLE_HEADER);
    int caption_index = add_part(builder, stack, key, AGENT_UI_PART_BODY,
                                 NATIVE_UI_ELEMENT_TEXT, NATIVE_UI_ROLE_HEADER);
    fixed_height(node_at(builder, title_index), density_height(builder, 23));
    fixed_height(node_at(builder, caption_index), density_height(builder, 16));
    set_text(node_at(builder, title_index), title, NATIVE_UI_TYPE_TITLE,
             NATIVE_UI_COLOR_TEXT, 248);
    set_text(node_at(builder, caption_index), caption, NATIVE_UI_TYPE_LABEL,
             NATIVE_UI_COLOR_TEXT_MUTED, 190);
    if (!caption || !*caption) node_at(builder, caption_index)->state &= ~NATIVE_UI_STATE_VISIBLE;
    int meta_index = add_part(builder, root, key, AGENT_UI_PART_META,
                              NATIVE_UI_ELEMENT_TEXT, NATIVE_UI_ROLE_STATUS);
    native_ui_node_t *meta_node = node_at(builder, meta_index);
    fixed_width(meta_node, label_width(meta, 56, 180));
    set_text(meta_node, meta, NATIVE_UI_TYPE_LABEL, NATIVE_UI_COLOR_ACCENT, 220);
    if (!meta || !*meta) meta_node->state &= ~NATIVE_UI_STATE_VISIBLE;
    set_a11y(root_node, "Section", title);
    return root;
}

int agent_ui_add_status_badge(agent_ui_builder_t *builder, int parent,
                              uint64_t key, const char *label,
                              agent_ui_tone_t tone) {
    return add_badge_node(builder, parent, key, label, tone,
                          NATIVE_UI_ROLE_STATUS, false);
}

int agent_ui_add_message(agent_ui_builder_t *builder, int parent, uint64_t key,
                         const agent_ui_message_model_t *model) {
    if (!model) return -1;
    int root = add_raw(builder, parent, key, NATIVE_UI_ELEMENT_SURFACE,
                       NATIVE_UI_ROLE_MESSAGE);
    native_ui_node_t *root_node = node_at(builder, root);
    if (!root_node) return -1;
    style_card(builder, root_node, NATIVE_UI_FLOW_COLUMN,
               model->kind == AGENT_UI_MESSAGE_ASSISTANT);
    fixed_height(root_node, density_height(builder, model->streaming ? 102 : 88));
    mark_selected(root_node, model->selected);
    root_node->state |= NATIVE_UI_STATE_LIVE;

    int header = add_internal(builder, root, key, 0, NATIVE_UI_ELEMENT_ROW,
                              NATIVE_UI_ROLE_MESSAGE);
    native_ui_node_t *header_node = node_at(builder, header);
    fixed_height(header_node, density_height(builder, 22));
    header_node->style.flow = NATIVE_UI_FLOW_ROW;
    header_node->style.align = NATIVE_UI_ALIGN_CENTER;
    header_node->style.gap = builder->theme->spacing.sm;
    int icon = add_part(builder, header, key, AGENT_UI_PART_ICON,
                        NATIVE_UI_ELEMENT_ICON, NATIVE_UI_ROLE_MESSAGE);
    native_ui_node_t *icon_node = node_at(builder, icon);
    fixed_width(icon_node, 18);
    set_text(icon_node,
             model->kind == AGENT_UI_MESSAGE_USER ? "user" :
             model->kind == AGENT_UI_MESSAGE_SYSTEM ? "system" : "assistant",
             NATIVE_UI_TYPE_LABEL,
             model->kind == AGENT_UI_MESSAGE_USER ? NATIVE_UI_COLOR_ACCENT :
             model->kind == AGENT_UI_MESSAGE_SYSTEM ? NATIVE_UI_COLOR_WARNING :
                                                     NATIVE_UI_COLOR_SUCCESS, 235);
    int author = add_part(builder, header, key, AGENT_UI_PART_TITLE,
                          NATIVE_UI_ELEMENT_TEXT, NATIVE_UI_ROLE_MESSAGE);
    native_ui_node_t *author_node = node_at(builder, author);
    author_node->constraints.grow = 1;
    set_text(author_node, model->author, NATIVE_UI_TYPE_LABEL,
             NATIVE_UI_COLOR_TEXT, 235);
    int meta = add_part(builder, header, key, AGENT_UI_PART_META,
                        NATIVE_UI_ELEMENT_TEXT, NATIVE_UI_ROLE_TIMELINE);
    native_ui_node_t *meta_node = node_at(builder, meta);
    fixed_width(meta_node, label_width(model->meta, 46, 150));
    set_text(meta_node, model->meta, NATIVE_UI_TYPE_LABEL,
             NATIVE_UI_COLOR_TEXT_MUTED, 178);
    int body = add_part(builder, root, key, AGENT_UI_PART_BODY,
                        NATIVE_UI_ELEMENT_TEXT, NATIVE_UI_ROLE_MESSAGE);
    native_ui_node_t *body_node = node_at(builder, body);
    body_node->constraints.grow = 1;
    set_text(body_node, model->body, NATIVE_UI_TYPE_BODY,
             NATIVE_UI_COLOR_TEXT, 245);
    if (model->streaming) {
        int state = add_part(builder, root, key, AGENT_UI_PART_STATE,
                             NATIVE_UI_ELEMENT_TEXT, NATIVE_UI_ROLE_STATUS);
        fixed_height(node_at(builder, state), density_height(builder, 16));
        set_text(node_at(builder, state), "RESPONDING · LIVE",
                 NATIVE_UI_TYPE_LABEL, NATIVE_UI_COLOR_ACCENT, 210);
    }
    set_a11y(root_node, "Message", model->author);
    return root;
}

int agent_ui_add_reasoning(agent_ui_builder_t *builder, int parent, uint64_t key,
                           const agent_ui_reasoning_model_t *model) {
    if (!model) return -1;
    int root = add_raw(builder, parent, key, NATIVE_UI_ELEMENT_SURFACE,
                       NATIVE_UI_ROLE_REASONING_ACTIVITY);
    native_ui_node_t *root_node = node_at(builder, root);
    if (!root_node) return -1;
    style_card(builder, root_node, NATIVE_UI_FLOW_COLUMN, false);
    fixed_height(root_node, density_height(builder, model->expanded ? 118 : 94));
    root_node->agent_state = NATIVE_UI_AGENT_REASONING;
    if (model->live) root_node->state |= NATIVE_UI_STATE_LIVE;

    int header = add_internal(builder, root, key, 0, NATIVE_UI_ELEMENT_ROW,
                              NATIVE_UI_ROLE_REASONING_ACTIVITY);
    fixed_height(node_at(builder, header), density_height(builder, 22));
    node_at(builder, header)->style.flow = NATIVE_UI_FLOW_ROW;
    node_at(builder, header)->style.align = NATIVE_UI_ALIGN_CENTER;
    node_at(builder, header)->style.gap = builder->theme->spacing.sm;
    int activity = add_part(builder, header, key, AGENT_UI_PART_ICON,
                            NATIVE_UI_ELEMENT_CUSTOM,
                            NATIVE_UI_ROLE_REASONING_ACTIVITY);
    fixed_width(node_at(builder, activity), 30);
    native_ui_node_set_text(node_at(builder, activity), "activity-pulse");
    int title = add_part(builder, header, key, AGENT_UI_PART_TITLE,
                         NATIVE_UI_ELEMENT_TEXT,
                         NATIVE_UI_ROLE_REASONING_ACTIVITY);
    node_at(builder, title)->constraints.grow = 1;
    set_text(node_at(builder, title), model->title, NATIVE_UI_TYPE_TITLE,
             NATIVE_UI_COLOR_TEXT, 240);
    add_badge_node(builder, header,
                   agent_ui_component_part_key(key, AGENT_UI_PART_STATE),
                   model->phase, model->live ? AGENT_UI_TONE_ACCENT : AGENT_UI_TONE_NEUTRAL,
                   NATIVE_UI_ROLE_STATUS, false);
    int detail = add_part(builder, root, key, AGENT_UI_PART_DETAIL,
                          NATIVE_UI_ELEMENT_TEXT,
                          NATIVE_UI_ROLE_REASONING_ACTIVITY);
    set_text(node_at(builder, detail), model->detail, NATIVE_UI_TYPE_BODY,
             NATIVE_UI_COLOR_TEXT_MUTED, 205);
    node_at(builder, detail)->constraints.grow = 1;
    int meter = add_part(builder, root, key, AGENT_UI_PART_PROGRESS,
                         NATIVE_UI_ELEMENT_METER, NATIVE_UI_ROLE_STATUS);
    native_ui_node_t *meter_node = node_at(builder, meter);
    fixed_height(meter_node, 12);
    meter_node->value = model->progress;
    meter_node->style.foreground = NATIVE_UI_COLOR_ACCENT;
    set_a11y(root_node, "Reasoning activity", model->title);
    return root;
}

int agent_ui_add_tool(agent_ui_builder_t *builder, int parent, uint64_t key,
                      const agent_ui_tool_model_t *model) {
    if (!model) return -1;
    int root = add_raw(builder, parent, key, NATIVE_UI_ELEMENT_SURFACE,
                       NATIVE_UI_ROLE_TOOL_ACTIVITY);
    native_ui_node_t *root_node = node_at(builder, root);
    if (!root_node) return -1;
    style_card(builder, root_node, NATIVE_UI_FLOW_COLUMN, true);
    fixed_height(root_node, density_height(builder, 126));
    root_node->agent_state = model->running ? NATIVE_UI_AGENT_EXECUTING
                                            : NATIVE_UI_AGENT_IDLE;
    if (model->running) root_node->state |= NATIVE_UI_STATE_LIVE;

    int header = add_internal(builder, root, key, 0, NATIVE_UI_ELEMENT_ROW,
                              NATIVE_UI_ROLE_TOOL_ACTIVITY);
    fixed_height(node_at(builder, header), density_height(builder, 24));
    node_at(builder, header)->style.flow = NATIVE_UI_FLOW_ROW;
    node_at(builder, header)->style.align = NATIVE_UI_ALIGN_CENTER;
    node_at(builder, header)->style.gap = builder->theme->spacing.sm;
    int icon = add_part(builder, header, key, AGENT_UI_PART_ICON,
                        NATIVE_UI_ELEMENT_ICON, NATIVE_UI_ROLE_TOOL_ACTIVITY);
    fixed_width(node_at(builder, icon), 20);
    set_text(node_at(builder, icon), "tool", NATIVE_UI_TYPE_LABEL,
             tone_color(model->tone), 240);
    int title = add_part(builder, header, key, AGENT_UI_PART_TITLE,
                         NATIVE_UI_ELEMENT_TEXT, NATIVE_UI_ROLE_TOOL_ACTIVITY);
    node_at(builder, title)->constraints.grow = 1;
    set_text(node_at(builder, title), model->tool, NATIVE_UI_TYPE_TITLE,
             NATIVE_UI_COLOR_TEXT, 245);
    add_badge_node(builder, header,
                   agent_ui_component_part_key(key, AGENT_UI_PART_STATE),
                   model->status, model->tone, NATIVE_UI_ROLE_STATUS, false);
    int summary = add_part(builder, root, key, AGENT_UI_PART_BODY,
                           NATIVE_UI_ELEMENT_TEXT, NATIVE_UI_ROLE_TOOL_ACTIVITY);
    set_text(node_at(builder, summary), model->summary, NATIVE_UI_TYPE_BODY,
             NATIVE_UI_COLOR_TEXT, 228);
    int detail = add_part(builder, root, key, AGENT_UI_PART_DETAIL,
                          NATIVE_UI_ELEMENT_TEXT, NATIVE_UI_ROLE_CODE);
    set_text(node_at(builder, detail), model->detail, NATIVE_UI_TYPE_CODE,
             NATIVE_UI_COLOR_TEXT_MUTED, 205);
    node_at(builder, detail)->constraints.grow = 1;
    int footer = add_internal(builder, root, key, 1, NATIVE_UI_ELEMENT_ROW,
                              NATIVE_UI_ROLE_STATUS);
    fixed_height(node_at(builder, footer), 16);
    node_at(builder, footer)->style.flow = NATIVE_UI_FLOW_ROW;
    int meter = add_part(builder, footer, key, AGENT_UI_PART_PROGRESS,
                         NATIVE_UI_ELEMENT_METER, NATIVE_UI_ROLE_STATUS);
    node_at(builder, meter)->constraints.grow = 1;
    node_at(builder, meter)->value = model->progress;
    node_at(builder, meter)->style.foreground = tone_color(model->tone);
    int duration = add_part(builder, footer, key, AGENT_UI_PART_META,
                            NATIVE_UI_ELEMENT_TEXT, NATIVE_UI_ROLE_TIMELINE);
    fixed_width(node_at(builder, duration), label_width(model->duration, 42, 100));
    set_text(node_at(builder, duration), model->duration, NATIVE_UI_TYPE_LABEL,
             NATIVE_UI_COLOR_TEXT_MUTED, 190);
    set_a11y(root_node, "Tool activity", model->tool);
    return root;
}

int agent_ui_add_plan_step(agent_ui_builder_t *builder, int parent, uint64_t key,
                           const agent_ui_plan_step_model_t *model) {
    if (!model) return -1;
    int root = add_raw(builder, parent, key, NATIVE_UI_ELEMENT_SURFACE,
                       NATIVE_UI_ROLE_PLAN);
    native_ui_node_t *root_node = node_at(builder, root);
    if (!root_node) return -1;
    style_card(builder, root_node, NATIVE_UI_FLOW_ROW, model->selected);
    fixed_height(root_node, density_height(builder, 66));
    root_node->style.align = NATIVE_UI_ALIGN_CENTER;
    mark_selected(root_node, model->selected);
    if (model->actionable) root_node->state |= NATIVE_UI_STATE_FOCUSABLE;
    int number = add_part(builder, root, key, AGENT_UI_PART_ICON,
                          NATIVE_UI_ELEMENT_BADGE, NATIVE_UI_ROLE_PLAN);
    char number_text[16];
    snprintf(number_text, sizeof(number_text), "%02d", model->index);
    fixed_width(node_at(builder, number), 34);
    set_text(node_at(builder, number), number_text, NATIVE_UI_TYPE_LABEL,
             tone_color(model->tone), 235);
    node_at(builder, number)->style.background = NATIVE_UI_COLOR_SURFACE_RAISED;
    node_at(builder, number)->style.radius = (uint8_t)builder->theme->radius.sm;
    int stack = add_internal(builder, root, key, 0, NATIVE_UI_ELEMENT_STACK,
                             NATIVE_UI_ROLE_PLAN);
    node_at(builder, stack)->constraints.grow = 1;
    node_at(builder, stack)->style.flow = NATIVE_UI_FLOW_COLUMN;
    node_at(builder, stack)->style.gap = 1;
    int title = add_part(builder, stack, key, AGENT_UI_PART_TITLE,
                         NATIVE_UI_ELEMENT_TEXT, NATIVE_UI_ROLE_PLAN);
    fixed_height(node_at(builder, title), density_height(builder, 23));
    set_text(node_at(builder, title), model->title, NATIVE_UI_TYPE_BODY,
             NATIVE_UI_COLOR_TEXT, 240);
    int detail = add_part(builder, stack, key, AGENT_UI_PART_DETAIL,
                          NATIVE_UI_ELEMENT_TEXT, NATIVE_UI_ROLE_PLAN);
    fixed_height(node_at(builder, detail), density_height(builder, 17));
    set_text(node_at(builder, detail), model->detail, NATIVE_UI_TYPE_LABEL,
             NATIVE_UI_COLOR_TEXT_MUTED, 190);
    add_badge_node(builder, root,
                   agent_ui_component_part_key(key, AGENT_UI_PART_STATE),
                   model->status, model->tone, NATIVE_UI_ROLE_STATUS, false);
    set_a11y(root_node, "Plan step", model->title);
    return root;
}

int agent_ui_add_agent_card(agent_ui_builder_t *builder, int parent, uint64_t key,
                            const agent_ui_agent_model_t *model) {
    if (!model) return -1;
    int root = add_raw(builder, parent, key, NATIVE_UI_ELEMENT_SURFACE,
                       NATIVE_UI_ROLE_AGENT_TOPOLOGY);
    native_ui_node_t *root_node = node_at(builder, root);
    if (!root_node) return -1;
    style_card(builder, root_node, NATIVE_UI_FLOW_COLUMN, true);
    fixed_height(root_node, density_height(builder, 130));
    root_node->agent_state = model->state;
    root_node->state |= NATIVE_UI_STATE_FOCUSABLE;
    mark_selected(root_node, model->selected);
    int header = add_internal(builder, root, key, 0, NATIVE_UI_ELEMENT_ROW,
                              NATIVE_UI_ROLE_AGENT_TOPOLOGY);
    fixed_height(node_at(builder, header), 24);
    node_at(builder, header)->style.flow = NATIVE_UI_FLOW_ROW;
    node_at(builder, header)->style.align = NATIVE_UI_ALIGN_CENTER;
    int icon = add_part(builder, header, key, AGENT_UI_PART_ICON,
                        NATIVE_UI_ELEMENT_ICON, NATIVE_UI_ROLE_AGENT_TOPOLOGY);
    fixed_width(node_at(builder, icon), 22);
    set_text(node_at(builder, icon), "agent", NATIVE_UI_TYPE_LABEL,
             NATIVE_UI_COLOR_ACCENT, 240);
    int title = add_part(builder, header, key, AGENT_UI_PART_TITLE,
                         NATIVE_UI_ELEMENT_TEXT, NATIVE_UI_ROLE_AGENT_TOPOLOGY);
    node_at(builder, title)->constraints.grow = 1;
    set_text(node_at(builder, title), model->name, NATIVE_UI_TYPE_TITLE,
             NATIVE_UI_COLOR_TEXT, 245);
    agent_ui_tone_t state_tone = model->state == NATIVE_UI_AGENT_ERROR ||
                                 model->state == NATIVE_UI_AGENT_BLOCKED
                                 ? AGENT_UI_TONE_DANGER :
                                 model->state == NATIVE_UI_AGENT_IDLE
                                 ? AGENT_UI_TONE_NEUTRAL : AGENT_UI_TONE_SUCCESS;
    add_badge_node(builder, header,
                   agent_ui_component_part_key(key, AGENT_UI_PART_STATE),
                   model->status, state_tone, NATIVE_UI_ROLE_STATUS, false);
    int model_node = add_part(builder, root, key, AGENT_UI_PART_META,
                              NATIVE_UI_ELEMENT_TEXT, NATIVE_UI_ROLE_STATUS);
    set_text(node_at(builder, model_node), model->model, NATIVE_UI_TYPE_LABEL,
             NATIVE_UI_COLOR_ACCENT, 205);
    int task = add_part(builder, root, key, AGENT_UI_PART_BODY,
                        NATIVE_UI_ELEMENT_TEXT, NATIVE_UI_ROLE_AGENT_TOPOLOGY);
    node_at(builder, task)->constraints.grow = 1;
    set_text(node_at(builder, task), model->task, NATIVE_UI_TYPE_BODY,
             NATIVE_UI_COLOR_TEXT_MUTED, 210);
    int meter = add_part(builder, root, key, AGENT_UI_PART_PROGRESS,
                         NATIVE_UI_ELEMENT_METER, NATIVE_UI_ROLE_STATUS);
    fixed_height(node_at(builder, meter), 12);
    node_at(builder, meter)->value = model->context_usage;
    node_at(builder, meter)->style.foreground = NATIVE_UI_COLOR_ACCENT;
    set_a11y(root_node, "Agent", model->name);
    return root;
}

int agent_ui_add_topology(agent_ui_builder_t *builder, int parent, uint64_t key,
                          const agent_ui_topology_model_t *model) {
    if (!model) return -1;
    int root = add_raw(builder, parent, key, NATIVE_UI_ELEMENT_SURFACE,
                       NATIVE_UI_ROLE_AGENT_TOPOLOGY);
    native_ui_node_t *root_node = node_at(builder, root);
    if (!root_node) return -1;
    style_card(builder, root_node, NATIVE_UI_FLOW_COLUMN, false);
    fixed_height(root_node, density_height(builder, 178));
    int header = add_internal(builder, root, key, 0, NATIVE_UI_ELEMENT_ROW,
                              NATIVE_UI_ROLE_AGENT_TOPOLOGY);
    fixed_height(node_at(builder, header), 24);
    node_at(builder, header)->style.flow = NATIVE_UI_FLOW_ROW;
    int title = add_part(builder, header, key, AGENT_UI_PART_TITLE,
                         NATIVE_UI_ELEMENT_TEXT, NATIVE_UI_ROLE_AGENT_TOPOLOGY);
    node_at(builder, title)->constraints.grow = 1;
    set_text(node_at(builder, title), model->title, NATIVE_UI_TYPE_TITLE,
             NATIVE_UI_COLOR_TEXT, 245);
    int meta = add_part(builder, header, key, AGENT_UI_PART_META,
                        NATIVE_UI_ELEMENT_TEXT, NATIVE_UI_ROLE_STATUS);
    char count[48];
    snprintf(count, sizeof(count), "%d / %d ACTIVE", model->active_agents, model->agents);
    fixed_width(node_at(builder, meta), 96);
    set_text(node_at(builder, meta), count, NATIVE_UI_TYPE_LABEL,
             NATIVE_UI_COLOR_SUCCESS, 220);
    int graph = add_part(builder, root, key, AGENT_UI_PART_DATA,
                         NATIVE_UI_ELEMENT_CUSTOM, NATIVE_UI_ROLE_AGENT_TOPOLOGY);
    node_at(builder, graph)->constraints.grow = 1;
    char graph_tag[NATIVE_UI_TEXT_CAP];
    snprintf(graph_tag, sizeof(graph_tag), "topology:%d:%d:%s", model->agents,
             model->active_agents, model->route ? model->route : "direct");
    native_ui_node_set_text(node_at(builder, graph), graph_tag);
    int detail = add_part(builder, root, key, AGENT_UI_PART_DETAIL,
                          NATIVE_UI_ELEMENT_TEXT, NATIVE_UI_ROLE_AGENT_TOPOLOGY);
    fixed_height(node_at(builder, detail), 18);
    set_text(node_at(builder, detail), model->summary, NATIVE_UI_TYPE_LABEL,
             NATIVE_UI_COLOR_TEXT_MUTED, 190);
    set_a11y(root_node, "Agent topology", model->title);
    return root;
}

int agent_ui_add_artifact(agent_ui_builder_t *builder, int parent, uint64_t key,
                          const agent_ui_artifact_model_t *model) {
    if (!model) return -1;
    int root = add_raw(builder, parent, key, NATIVE_UI_ELEMENT_SURFACE,
                       NATIVE_UI_ROLE_ARTIFACT);
    native_ui_node_t *root_node = node_at(builder, root);
    if (!root_node) return -1;
    style_card(builder, root_node, NATIVE_UI_FLOW_ROW, true);
    fixed_height(root_node, density_height(builder, 92));
    root_node->style.align = NATIVE_UI_ALIGN_CENTER;
    if (model->actionable) root_node->state |= NATIVE_UI_STATE_FOCUSABLE;
    int icon = add_part(builder, root, key, AGENT_UI_PART_ICON,
                        NATIVE_UI_ELEMENT_ICON, NATIVE_UI_ROLE_ARTIFACT);
    fixed_width(node_at(builder, icon), 30);
    set_text(node_at(builder, icon), model->kind, NATIVE_UI_TYPE_LABEL,
             NATIVE_UI_COLOR_ACCENT, 230);
    int stack = add_internal(builder, root, key, 0, NATIVE_UI_ELEMENT_STACK,
                             NATIVE_UI_ROLE_ARTIFACT);
    node_at(builder, stack)->constraints.grow = 1;
    node_at(builder, stack)->style.flow = NATIVE_UI_FLOW_COLUMN;
    node_at(builder, stack)->style.gap = 1;
    int title = add_part(builder, stack, key, AGENT_UI_PART_TITLE,
                         NATIVE_UI_ELEMENT_TEXT, NATIVE_UI_ROLE_ARTIFACT);
    fixed_height(node_at(builder, title), density_height(builder, 24));
    set_text(node_at(builder, title), model->title, NATIVE_UI_TYPE_TITLE,
             NATIVE_UI_COLOR_TEXT, 240);
    int path = add_part(builder, stack, key, AGENT_UI_PART_META,
                        NATIVE_UI_ELEMENT_TEXT, NATIVE_UI_ROLE_CODE);
    fixed_height(node_at(builder, path), density_height(builder, 18));
    set_text(node_at(builder, path), model->path, NATIVE_UI_TYPE_CODE,
             NATIVE_UI_COLOR_TEXT_MUTED, 200);
    int body = add_part(builder, stack, key, AGENT_UI_PART_BODY,
                        NATIVE_UI_ELEMENT_TEXT, NATIVE_UI_ROLE_ARTIFACT);
    fixed_height(node_at(builder, body), density_height(builder, 18));
    set_text(node_at(builder, body), model->summary, NATIVE_UI_TYPE_LABEL,
             NATIVE_UI_COLOR_TEXT_MUTED, 185);
    if (model->action && *model->action)
        add_badge_node(builder, root,
                       agent_ui_component_part_key(key, AGENT_UI_PART_PRIMARY_ACTION),
                       model->action, AGENT_UI_TONE_ACCENT,
                       NATIVE_UI_ROLE_ARTIFACT, model->actionable);
    set_a11y(root_node, "Artifact", model->title);
    return root;
}

int agent_ui_add_permission(agent_ui_builder_t *builder, int parent, uint64_t key,
                            const agent_ui_permission_model_t *model) {
    if (!model) return -1;
    int root = add_raw(builder, parent, key, NATIVE_UI_ELEMENT_SURFACE,
                       NATIVE_UI_ROLE_PERMISSION);
    native_ui_node_t *root_node = node_at(builder, root);
    if (!root_node) return -1;
    style_card(builder, root_node, NATIVE_UI_FLOW_COLUMN, true);
    fixed_height(root_node, density_height(builder, 150));
    root_node->style.border = tone_color(model->risk);
    int title = add_part(builder, root, key, AGENT_UI_PART_TITLE,
                         NATIVE_UI_ELEMENT_TEXT, NATIVE_UI_ROLE_PERMISSION);
    fixed_height(node_at(builder, title), 24);
    set_text(node_at(builder, title), model->title, NATIVE_UI_TYPE_TITLE,
             tone_color(model->risk), 245);
    int detail = add_part(builder, root, key, AGENT_UI_PART_BODY,
                          NATIVE_UI_ELEMENT_TEXT, NATIVE_UI_ROLE_PERMISSION);
    node_at(builder, detail)->constraints.grow = 1;
    set_text(node_at(builder, detail), model->detail, NATIVE_UI_TYPE_BODY,
             NATIVE_UI_COLOR_TEXT, 230);
    int scope = add_part(builder, root, key, AGENT_UI_PART_META,
                         NATIVE_UI_ELEMENT_TEXT, NATIVE_UI_ROLE_CODE);
    fixed_height(node_at(builder, scope), 20);
    set_text(node_at(builder, scope), model->scope, NATIVE_UI_TYPE_CODE,
             NATIVE_UI_COLOR_TEXT_MUTED, 205);
    int actions = add_internal(builder, root, key, 0, NATIVE_UI_ELEMENT_ROW,
                               NATIVE_UI_ROLE_PERMISSION);
    fixed_height(node_at(builder, actions), density_height(builder, 28));
    node_at(builder, actions)->style.flow = NATIVE_UI_FLOW_ROW;
    node_at(builder, actions)->style.justify = NATIVE_UI_ALIGN_END;
    node_at(builder, actions)->style.gap = builder->theme->spacing.sm;
    int spacer = add_internal(builder, actions, key, 1, NATIVE_UI_ELEMENT_STACK,
                              NATIVE_UI_ROLE_NONE);
    node_at(builder, spacer)->constraints.grow = 1;
    add_badge_node(builder, actions,
                   agent_ui_component_part_key(key, AGENT_UI_PART_SECONDARY_ACTION),
                   model->secondary_label ? model->secondary_label : "Deny",
                   AGENT_UI_TONE_NEUTRAL, NATIVE_UI_ROLE_PERMISSION, true);
    add_badge_node(builder, actions,
                   agent_ui_component_part_key(key, AGENT_UI_PART_PRIMARY_ACTION),
                   model->primary_label ? model->primary_label : "Allow",
                   model->risk, NATIVE_UI_ROLE_PERMISSION, true);
    set_a11y(root_node, "Permission request", model->title);
    return root;
}

int agent_ui_add_metric(agent_ui_builder_t *builder, int parent, uint64_t key,
                        const agent_ui_metric_model_t *model) {
    if (!model) return -1;
    int root = add_raw(builder, parent, key, NATIVE_UI_ELEMENT_SURFACE,
                       NATIVE_UI_ROLE_STATUS);
    native_ui_node_t *root_node = node_at(builder, root);
    if (!root_node) return -1;
    style_card(builder, root_node, NATIVE_UI_FLOW_COLUMN, false);
    fixed_height(root_node, density_height(builder, 112));
    int header = add_internal(builder, root, key, 0, NATIVE_UI_ELEMENT_ROW,
                              NATIVE_UI_ROLE_STATUS);
    fixed_height(node_at(builder, header), 18);
    node_at(builder, header)->style.flow = NATIVE_UI_FLOW_ROW;
    int label = add_part(builder, header, key, AGENT_UI_PART_TITLE,
                         NATIVE_UI_ELEMENT_TEXT, NATIVE_UI_ROLE_STATUS);
    node_at(builder, label)->constraints.grow = 1;
    set_text(node_at(builder, label), model->label, NATIVE_UI_TYPE_LABEL,
             NATIVE_UI_COLOR_TEXT_MUTED, 205);
    int trend = add_part(builder, header, key, AGENT_UI_PART_STATE,
                         NATIVE_UI_ELEMENT_TEXT, NATIVE_UI_ROLE_STATUS);
    fixed_width(node_at(builder, trend), label_width(model->trend, 38, 90));
    set_text(node_at(builder, trend), model->trend, NATIVE_UI_TYPE_LABEL,
             tone_color(model->tone), 225);
    int value_row = add_internal(builder, root, key, 1, NATIVE_UI_ELEMENT_ROW,
                                 NATIVE_UI_ROLE_STATUS);
    fixed_height(node_at(builder, value_row), 30);
    node_at(builder, value_row)->style.flow = NATIVE_UI_FLOW_ROW;
    node_at(builder, value_row)->style.align = NATIVE_UI_ALIGN_END;
    int value = add_part(builder, value_row, key, AGENT_UI_PART_DATA,
                         NATIVE_UI_ELEMENT_TEXT, NATIVE_UI_ROLE_STATUS);
    node_at(builder, value)->constraints.grow = 1;
    set_text(node_at(builder, value), model->value, NATIVE_UI_TYPE_METRIC,
             NATIVE_UI_COLOR_TEXT, 250);
    int unit = add_part(builder, value_row, key, AGENT_UI_PART_META,
                        NATIVE_UI_ELEMENT_TEXT, NATIVE_UI_ROLE_STATUS);
    fixed_width(node_at(builder, unit), label_width(model->unit, 28, 80));
    set_text(node_at(builder, unit), model->unit, NATIVE_UI_TYPE_LABEL,
             NATIVE_UI_COLOR_TEXT_MUTED, 190);
    int spark = add_part(builder, root, key, AGENT_UI_PART_PROGRESS,
                         NATIVE_UI_ELEMENT_SPARKLINE, NATIVE_UI_ROLE_TIMELINE);
    node_at(builder, spark)->constraints.grow = 1;
    native_ui_node_set_text(node_at(builder, spark), model->series ? model->series : "0 0");
    node_at(builder, spark)->style.foreground = tone_color(model->tone);
    node_at(builder, spark)->style.opacity = 225;
    set_a11y(root_node, "Metric", model->label);
    return root;
}

int agent_ui_add_queue_item(agent_ui_builder_t *builder, int parent, uint64_t key,
                            const agent_ui_queue_model_t *model) {
    if (!model) return -1;
    int root = add_raw(builder, parent, key, NATIVE_UI_ELEMENT_SURFACE,
                       NATIVE_UI_ROLE_QUEUE);
    native_ui_node_t *root_node = node_at(builder, root);
    if (!root_node) return -1;
    style_card(builder, root_node, NATIVE_UI_FLOW_ROW, model->selected);
    fixed_height(root_node, density_height(builder, 64));
    root_node->style.align = NATIVE_UI_ALIGN_CENTER;
    root_node->state |= NATIVE_UI_STATE_FOCUSABLE;
    mark_selected(root_node, model->selected);
    char position[16];
    snprintf(position, sizeof(position), "Q%02d", model->position);
    int icon = add_part(builder, root, key, AGENT_UI_PART_ICON,
                        NATIVE_UI_ELEMENT_BADGE, NATIVE_UI_ROLE_QUEUE);
    fixed_width(node_at(builder, icon), 42);
    set_text(node_at(builder, icon), position, NATIVE_UI_TYPE_LABEL,
             NATIVE_UI_COLOR_ACCENT, 225);
    node_at(builder, icon)->style.background = NATIVE_UI_COLOR_SURFACE_RAISED;
    node_at(builder, icon)->style.radius = (uint8_t)builder->theme->radius.sm;
    int stack = add_internal(builder, root, key, 0, NATIVE_UI_ELEMENT_STACK,
                             NATIVE_UI_ROLE_QUEUE);
    node_at(builder, stack)->constraints.grow = 1;
    node_at(builder, stack)->style.flow = NATIVE_UI_FLOW_COLUMN;
    int title = add_part(builder, stack, key, AGENT_UI_PART_TITLE,
                         NATIVE_UI_ELEMENT_TEXT, NATIVE_UI_ROLE_QUEUE);
    fixed_height(node_at(builder, title), density_height(builder, 22));
    set_text(node_at(builder, title), model->title, NATIVE_UI_TYPE_BODY,
             NATIVE_UI_COLOR_TEXT, 235);
    int detail = add_part(builder, stack, key, AGENT_UI_PART_DETAIL,
                          NATIVE_UI_ELEMENT_TEXT, NATIVE_UI_ROLE_QUEUE);
    fixed_height(node_at(builder, detail), density_height(builder, 17));
    set_text(node_at(builder, detail), model->detail, NATIVE_UI_TYPE_LABEL,
             NATIVE_UI_COLOR_TEXT_MUTED, 185);
    add_badge_node(builder, root,
                   agent_ui_component_part_key(key, AGENT_UI_PART_STATE),
                   model->status, model->tone, NATIVE_UI_ROLE_STATUS, false);
    set_a11y(root_node, "Queue item", model->title);
    return root;
}

int agent_ui_add_timeline_event(agent_ui_builder_t *builder, int parent,
                                uint64_t key,
                                const agent_ui_timeline_model_t *model) {
    if (!model) return -1;
    int root = add_raw(builder, parent, key, NATIVE_UI_ELEMENT_ROW,
                       NATIVE_UI_ROLE_TIMELINE);
    native_ui_node_t *root_node = node_at(builder, root);
    if (!root_node) return -1;
    fixed_height(root_node, density_height(builder, 56));
    root_node->style.flow = NATIVE_UI_FLOW_ROW;
    root_node->style.align = NATIVE_UI_ALIGN_CENTER;
    root_node->style.gap = builder->theme->spacing.md;
    int time = add_part(builder, root, key, AGENT_UI_PART_META,
                        NATIVE_UI_ELEMENT_TEXT, NATIVE_UI_ROLE_TIMELINE);
    fixed_width(node_at(builder, time), 56);
    set_text(node_at(builder, time), model->time, NATIVE_UI_TYPE_LABEL,
             NATIVE_UI_COLOR_TEXT_MUTED, 190);
    int icon = add_part(builder, root, key, AGENT_UI_PART_ICON,
                        NATIVE_UI_ELEMENT_ICON, NATIVE_UI_ROLE_TIMELINE);
    fixed_width(node_at(builder, icon), 18);
    set_text(node_at(builder, icon), "event", NATIVE_UI_TYPE_LABEL,
             tone_color(model->tone), 235);
    int stack = add_internal(builder, root, key, 0, NATIVE_UI_ELEMENT_STACK,
                             NATIVE_UI_ROLE_TIMELINE);
    node_at(builder, stack)->constraints.grow = 1;
    node_at(builder, stack)->style.flow = NATIVE_UI_FLOW_COLUMN;
    int title = add_part(builder, stack, key, AGENT_UI_PART_TITLE,
                         NATIVE_UI_ELEMENT_TEXT, NATIVE_UI_ROLE_TIMELINE);
    fixed_height(node_at(builder, title), density_height(builder, 22));
    set_text(node_at(builder, title), model->title, NATIVE_UI_TYPE_BODY,
             NATIVE_UI_COLOR_TEXT, 230);
    int detail = add_part(builder, stack, key, AGENT_UI_PART_DETAIL,
                          NATIVE_UI_ELEMENT_TEXT, NATIVE_UI_ROLE_TIMELINE);
    fixed_height(node_at(builder, detail), density_height(builder, 17));
    set_text(node_at(builder, detail), model->detail, NATIVE_UI_TYPE_LABEL,
             NATIVE_UI_COLOR_TEXT_MUTED, 180);
    set_a11y(root_node, "Timeline event", model->title);
    return root;
}

int agent_ui_add_notification(agent_ui_builder_t *builder, int parent,
                              uint64_t key,
                              const agent_ui_notification_model_t *model) {
    if (!model) return -1;
    int root = add_raw(builder, parent, key, NATIVE_UI_ELEMENT_SURFACE,
                       model->toast ? NATIVE_UI_ROLE_TOAST : NATIVE_UI_ROLE_NOTIFICATION);
    native_ui_node_t *root_node = node_at(builder, root);
    if (!root_node) return -1;
    style_card(builder, root_node, NATIVE_UI_FLOW_ROW, true);
    fixed_height(root_node, density_height(builder, model->toast ? 76 : 92));
    root_node->style.align = NATIVE_UI_ALIGN_CENTER;
    root_node->style.border = tone_color(model->tone);
    root_node->state |= NATIVE_UI_STATE_LIVE;
    int icon = add_part(builder, root, key, AGENT_UI_PART_ICON,
                        NATIVE_UI_ELEMENT_ICON, root_node->role);
    fixed_width(node_at(builder, icon), 28);
    set_text(node_at(builder, icon), "notice", NATIVE_UI_TYPE_LABEL,
             tone_color(model->tone), 245);
    int stack = add_internal(builder, root, key, 0, NATIVE_UI_ELEMENT_STACK,
                             root_node->role);
    node_at(builder, stack)->constraints.grow = 1;
    node_at(builder, stack)->style.flow = NATIVE_UI_FLOW_COLUMN;
    int title = add_part(builder, stack, key, AGENT_UI_PART_TITLE,
                         NATIVE_UI_ELEMENT_TEXT, root_node->role);
    fixed_height(node_at(builder, title), density_height(builder, 24));
    set_text(node_at(builder, title), model->title, NATIVE_UI_TYPE_TITLE,
             NATIVE_UI_COLOR_TEXT, 240);
    int body = add_part(builder, stack, key, AGENT_UI_PART_BODY,
                        NATIVE_UI_ELEMENT_TEXT, root_node->role);
    fixed_height(node_at(builder, body), density_height(builder, 18));
    set_text(node_at(builder, body), model->body, NATIVE_UI_TYPE_LABEL,
             NATIVE_UI_COLOR_TEXT_MUTED, 195);
    if (model->action && *model->action)
        add_badge_node(builder, root,
                       agent_ui_component_part_key(key, AGENT_UI_PART_PRIMARY_ACTION),
                       model->action, model->tone, root_node->role, true);
    set_a11y(root_node, model->toast ? "Toast" : "Notification", model->title);
    return root;
}

int agent_ui_add_command(agent_ui_builder_t *builder, int parent, uint64_t key,
                         const agent_ui_command_model_t *model) {
    if (!model) return -1;
    int root = add_raw(builder, parent, key, NATIVE_UI_ELEMENT_SURFACE,
                       NATIVE_UI_ROLE_COMMAND_PALETTE);
    native_ui_node_t *root_node = node_at(builder, root);
    if (!root_node) return -1;
    style_card(builder, root_node, NATIVE_UI_FLOW_ROW, model->selected);
    fixed_height(root_node, density_height(builder, 62));
    root_node->style.align = NATIVE_UI_ALIGN_CENTER;
    root_node->state |= NATIVE_UI_STATE_FOCUSABLE;
    if (model->disabled) {
        root_node->state |= NATIVE_UI_STATE_DISABLED;
        root_node->style.opacity = 125;
    }
    mark_selected(root_node, model->selected);
    int icon = add_part(builder, root, key, AGENT_UI_PART_ICON,
                        NATIVE_UI_ELEMENT_ICON, NATIVE_UI_ROLE_COMMAND_PALETTE);
    fixed_width(node_at(builder, icon), 24);
    set_text(node_at(builder, icon), "command", NATIVE_UI_TYPE_LABEL,
             model->selected ? NATIVE_UI_COLOR_ACCENT : NATIVE_UI_COLOR_TEXT_MUTED, 225);
    int stack = add_internal(builder, root, key, 0, NATIVE_UI_ELEMENT_STACK,
                             NATIVE_UI_ROLE_COMMAND_PALETTE);
    node_at(builder, stack)->constraints.grow = 1;
    node_at(builder, stack)->style.flow = NATIVE_UI_FLOW_COLUMN;
    int title = add_part(builder, stack, key, AGENT_UI_PART_TITLE,
                         NATIVE_UI_ELEMENT_TEXT, NATIVE_UI_ROLE_COMMAND_PALETTE);
    fixed_height(node_at(builder, title), density_height(builder, 22));
    set_text(node_at(builder, title), model->command, NATIVE_UI_TYPE_BODY,
             NATIVE_UI_COLOR_TEXT, 235);
    int detail = add_part(builder, stack, key, AGENT_UI_PART_DETAIL,
                          NATIVE_UI_ELEMENT_TEXT, NATIVE_UI_ROLE_COMMAND_PALETTE);
    fixed_height(node_at(builder, detail), density_height(builder, 17));
    set_text(node_at(builder, detail), model->description, NATIVE_UI_TYPE_LABEL,
             NATIVE_UI_COLOR_TEXT_MUTED, 180);
    add_badge_node(builder, root,
                   agent_ui_component_part_key(key, AGENT_UI_PART_META),
                   model->shortcut, AGENT_UI_TONE_NEUTRAL,
                   NATIVE_UI_ROLE_COMMAND_PALETTE, false);
    set_a11y(root_node, "Command", model->command);
    return root;
}

int agent_ui_add_composer(agent_ui_builder_t *builder, int parent, uint64_t key,
                          const agent_ui_composer_component_model_t *model) {
    if (!model) return -1;
    int root = add_raw(builder, parent, key, NATIVE_UI_ELEMENT_SURFACE,
                       NATIVE_UI_ROLE_COMPOSER);
    native_ui_node_t *root_node = node_at(builder, root);
    if (!root_node) return -1;
    style_card(builder, root_node, NATIVE_UI_FLOW_ROW, true);
    fixed_height(root_node, density_height(builder, 62));
    root_node->style.align = NATIVE_UI_ALIGN_CENTER;
    root_node->state |= NATIVE_UI_STATE_LIVE;
    if (model->focused) {
        root_node->style.border = NATIVE_UI_COLOR_FOCUS;
        root_node->style.border_width = 2;
    }
    int mode = add_part(builder, root, key, AGENT_UI_PART_EYEBROW,
                        NATIVE_UI_ELEMENT_BADGE, NATIVE_UI_ROLE_COMPOSER);
    fixed_width(node_at(builder, mode), label_width(model->mode, 44, 92));
    set_text(node_at(builder, mode), model->mode, NATIVE_UI_TYPE_LABEL,
             NATIVE_UI_COLOR_ACCENT, 225);
    node_at(builder, mode)->style.background = NATIVE_UI_COLOR_SURFACE_RAISED;
    node_at(builder, mode)->style.radius = (uint8_t)builder->theme->radius.sm;
    int input = add_part(builder, root, key, AGENT_UI_PART_BODY,
                         NATIVE_UI_ELEMENT_INPUT, NATIVE_UI_ROLE_COMPOSER);
    native_ui_node_t *input_node = node_at(builder, input);
    input_node->constraints.grow = 1;
    input_node->state |= NATIVE_UI_STATE_FOCUSABLE;
    if (model->focused) input_node->state |= NATIVE_UI_STATE_FOCUSED;
    const char *content = model->text && *model->text ? model->text : model->placeholder;
    set_text(input_node, content, NATIVE_UI_TYPE_BODY,
             model->text && *model->text ? NATIVE_UI_COLOR_TEXT
                                         : NATIVE_UI_COLOR_TEXT_MUTED,
             model->text && *model->text ? 240 : 170);
    add_badge_node(builder, root,
                   agent_ui_component_part_key(key, AGENT_UI_PART_PRIMARY_ACTION),
                   model->busy ? "STOP" : model->submit_label,
                   model->busy ? AGENT_UI_TONE_DANGER : AGENT_UI_TONE_ACCENT,
                   NATIVE_UI_ROLE_COMPOSER, true);
    set_a11y(root_node, "Command composer", content);
    return root;
}

int agent_ui_add_code_block(agent_ui_builder_t *builder, int parent, uint64_t key,
                            const agent_ui_code_model_t *model) {
    if (!model) return -1;
    int root = add_raw(builder, parent, key, NATIVE_UI_ELEMENT_SURFACE,
                       NATIVE_UI_ROLE_CODE);
    native_ui_node_t *root_node = node_at(builder, root);
    if (!root_node) return -1;
    style_card(builder, root_node, NATIVE_UI_FLOW_COLUMN, false);
    fixed_height(root_node, density_height(builder, 112));
    int header = add_internal(builder, root, key, 0, NATIVE_UI_ELEMENT_ROW,
                              NATIVE_UI_ROLE_CODE);
    fixed_height(node_at(builder, header), 22);
    node_at(builder, header)->style.flow = NATIVE_UI_FLOW_ROW;
    int language = add_part(builder, header, key, AGENT_UI_PART_TITLE,
                            NATIVE_UI_ELEMENT_TEXT, NATIVE_UI_ROLE_CODE);
    node_at(builder, language)->constraints.grow = 1;
    set_text(node_at(builder, language), model->language, NATIVE_UI_TYPE_LABEL,
             NATIVE_UI_COLOR_WARNING, 225);
    int meta = add_part(builder, header, key, AGENT_UI_PART_META,
                        NATIVE_UI_ELEMENT_TEXT, NATIVE_UI_ROLE_CODE);
    fixed_width(node_at(builder, meta), label_width(model->meta, 48, 120));
    set_text(node_at(builder, meta), model->meta, NATIVE_UI_TYPE_LABEL,
             NATIVE_UI_COLOR_TEXT_MUTED, 180);
    if (model->copyable)
        add_badge_node(builder, header,
                       agent_ui_component_part_key(key, AGENT_UI_PART_PRIMARY_ACTION),
                       "COPY", AGENT_UI_TONE_NEUTRAL, NATIVE_UI_ROLE_CODE, true);
    int code = add_part(builder, root, key, AGENT_UI_PART_BODY,
                        NATIVE_UI_ELEMENT_TEXT, NATIVE_UI_ROLE_CODE);
    node_at(builder, code)->constraints.grow = 1;
    set_text(node_at(builder, code), model->code, NATIVE_UI_TYPE_CODE,
             NATIVE_UI_COLOR_TEXT, 230);
    set_a11y(root_node, "Code block", model->language);
    return root;
}

int agent_ui_add_theme_swatch(agent_ui_builder_t *builder, int parent,
                              uint64_t key, const agent_ui_theme_t *theme,
                              bool selected) {
    if (!theme) return -1;
    int root = add_raw(builder, parent, key, NATIVE_UI_ELEMENT_CUSTOM,
                       NATIVE_UI_ROLE_COMMAND_PALETTE);
    native_ui_node_t *root_node = node_at(builder, root);
    if (!root_node) return -1;
    root_node->state |= NATIVE_UI_STATE_FOCUSABLE | NATIVE_UI_STATE_CLIPS;
    if (selected) root_node->state |= NATIVE_UI_STATE_SELECTED;
    char tag[NATIVE_UI_TEXT_CAP];
    snprintf(tag, sizeof(tag), "theme-swatch:%s:%d", theme->id, selected ? 1 : 0);
    native_ui_node_set_text(root_node, tag);
    set_a11y(root_node, "Theme", theme->name);
    return root;
}
