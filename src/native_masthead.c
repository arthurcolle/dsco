#include "native_masthead.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static native_ui_node_t *add_node(native_ui_scene_t *scene, int parent,
                                  uint64_t key, native_ui_element_t element,
                                  native_ui_role_t role) {
    int index = native_ui_scene_add(scene, parent, key, element, role);
    return index >= 0 ? native_ui_scene_node(scene, index) : NULL;
}

static void set_fixed_width(native_ui_node_t *node, int width) {
    if (!node) return;
    if (width < 0) width = 0;
    node->constraints.min_width = width;
    node->constraints.preferred_width = width;
    node->constraints.max_width = width;
    node->constraints.shrink = 0;
}

static void set_fixed_height(native_ui_node_t *node, int height) {
    if (!node) return;
    if (height < 0) height = 0;
    node->constraints.min_height = height;
    node->constraints.preferred_height = height;
    node->constraints.max_height = height;
    node->constraints.shrink = 0;
}

static void sentence_label(char *dst, size_t cap, const char *value) {
    if (!dst || cap == 0) return;
    if (!value) value = "idle";
    size_t at = 0;
    while (value[at] && at + 1 < cap) {
        dst[at] = (char)(at == 0 ? toupper((unsigned char)value[at]) : tolower((unsigned char)value[at]));
        at++;
    }
    dst[at] = '\0';
}

bool native_masthead_build(native_ui_scene_t *scene, int width, int height,
                           const native_masthead_model_t *model) {
    if (!scene || !model || width < 160 || height < 44) return false;
    native_ui_scene_init(scene, width, height);
    native_ui_node_t *root = native_ui_scene_node(scene, scene->root);
    if (!root) return false;
    root->key = NATIVE_MASTHEAD_KEY_ROOT;
    root->element = NATIVE_UI_ELEMENT_SURFACE;
    root->role = NATIVE_UI_ROLE_HEADER;
    root->agent_state = model->state;
    root->style.flow = NATIVE_UI_FLOW_ROW;
    root->style.align = NATIVE_UI_ALIGN_STRETCH;
    root->style.padding = (native_ui_insets_t){2, 8, 2, 6};
    root->style.gap = 8;
    root->style.background = NATIVE_UI_COLOR_SURFACE_RAISED;
    root->style.border = NATIVE_UI_COLOR_BORDER;
    root->style.border_width = 0;
    root->style.radius = 6;
    root->style.opacity = 240;
    native_ui_node_set_accessibility_label(root, "DSCO agent workspace masthead");

    native_ui_node_t *accent = add_node(
        scene, scene->root, NATIVE_MASTHEAD_KEY_ACCENT,
        NATIVE_UI_ELEMENT_RULE, NATIVE_UI_ROLE_STATUS);
    native_ui_node_t *soul = add_node(
        scene, scene->root, NATIVE_MASTHEAD_KEY_SOUL,
        NATIVE_UI_ELEMENT_CUSTOM, NATIVE_UI_ROLE_AGENT_TOPOLOGY);
    native_ui_node_t *identity = add_node(
        scene, scene->root, NATIVE_MASTHEAD_KEY_IDENTITY,
        NATIVE_UI_ELEMENT_STACK, NATIVE_UI_ROLE_HEADER);
    native_ui_node_t *status = add_node(
        scene, scene->root, NATIVE_MASTHEAD_KEY_STATUS,
        NATIVE_UI_ELEMENT_ROW, NATIVE_UI_ROLE_STATUS);
    if (!accent || !soul || !identity || !status) return false;

    set_fixed_width(accent, 2);
    accent->style.background = NATIVE_UI_COLOR_ACCENT;
    accent->style.opacity = 0;
    accent->style.radius = 1;

    set_fixed_width(soul, height < 48 ? 28 : 34);
    soul->agent_state = model->state;
    native_ui_node_set_text(soul, "dsco-soul");
    native_ui_node_set_accessibility_label(
        soul, "Overmind Soul activity and governed tool state");

    identity->constraints.min_width = 32;
    identity->constraints.grow = 1;
    identity->style.flow = NATIVE_UI_FLOW_COLUMN;
    identity->style.align = NATIVE_UI_ALIGN_STRETCH;
    identity->style.gap = 1;

    native_ui_node_t *title = add_node(
        scene, (int)(identity - scene->nodes), NATIVE_MASTHEAD_KEY_TITLE,
        NATIVE_UI_ELEMENT_TEXT, NATIVE_UI_ROLE_HEADER);
    native_ui_node_t *details = add_node(
        scene, (int)(identity - scene->nodes), NATIVE_MASTHEAD_KEY_DETAILS,
        NATIVE_UI_ELEMENT_ROW, NATIVE_UI_ROLE_STATUS);
    native_ui_node_t *rule = add_node(
        scene, (int)(identity - scene->nodes), NATIVE_MASTHEAD_KEY_RULE,
        NATIVE_UI_ELEMENT_RULE, NATIVE_UI_ROLE_STATUS);
    if (!title || !details || !rule) return false;

    set_fixed_height(title, 20);
    title->style.type = NATIVE_UI_TYPE_TITLE;
    title->style.foreground = NATIVE_UI_COLOR_TEXT;
    title->style.opacity = 235;
    native_ui_node_set_text(title,
        model->title && *model->title ? model->title : "DSCO / AGENT WORKSPACE");
    native_ui_node_set_accessibility_label(title, "DSCO agent workspace");

    details->constraints.min_height = 17;
    details->constraints.preferred_height = 17;
    details->constraints.max_height = 18;
    details->constraints.grow = 1;
    details->style.flow = NATIVE_UI_FLOW_ROW;
    details->style.align = NATIVE_UI_ALIGN_CENTER;
    details->style.gap = 8;

    native_ui_node_t *model_node = add_node(
        scene, (int)(details - scene->nodes), NATIVE_MASTHEAD_KEY_MODEL,
        NATIVE_UI_ELEMENT_TEXT, NATIVE_UI_ROLE_STATUS);
    native_ui_node_t *metrics = add_node(
        scene, (int)(details - scene->nodes), NATIVE_MASTHEAD_KEY_METRICS,
        NATIVE_UI_ELEMENT_TEXT, NATIVE_UI_ROLE_STATUS);
    if (!model_node || !metrics) return false;
    model_node->constraints.min_width = 24;
    model_node->constraints.grow = 1;
    model_node->style.type = NATIVE_UI_TYPE_LABEL;
    model_node->style.foreground = NATIVE_UI_COLOR_TEXT_MUTED;
    model_node->style.opacity = 210;
    char model_label[NATIVE_UI_TEXT_CAP];
    if (model->slot && *model->slot)
        snprintf(model_label, sizeof(model_label), "%s  /  %s",
                 model->model && *model->model ? model->model : "model",
                 model->slot);
    else
        snprintf(model_label, sizeof(model_label), "%s",
                 model->model && *model->model ? model->model : "model");
    native_ui_node_set_text(model_node, model_label);
    native_ui_node_set_accessibility_label(model_node, "Active model and workspace slot");

    char metric_label[NATIVE_UI_TEXT_CAP];
    snprintf(metric_label, sizeof(metric_label),
             "IN %d  OUT %d  /  CTX %.0f%%  /  VALUE $%.4f%s  /  Q %d/%d  /  %d TOOLS",
             model->input_tokens, model->output_tokens, model->context_percent,
             model->cost_usd, model->unpriced_responses > 0 ? "+?" : "", model->queue_depth,
             model->queue_capacity > 0 ? model->queue_capacity : 8,
             model->tools_used);
    native_ui_node_set_text(metrics, metric_label);
    /* Preserve the model line at enlarged/narrow viewport sizes. Detailed
     * accounting remains available in the inspector and /cost. */
    /* Detailed accounting has a dedicated inspector and /cost view. It is
     * retained semantically here, but should not crowd the model identity. */
    bool show_metrics = false;
    metrics->constraints.min_width = show_metrics ? 100 : 0;
    metrics->constraints.preferred_width = show_metrics ? 310 : 0;
    metrics->constraints.max_width = show_metrics ? 360 : 0;
    metrics->constraints.shrink = 1;
    metrics->style.type = NATIVE_UI_TYPE_LABEL;
    metrics->style.foreground = NATIVE_UI_COLOR_TEXT_MUTED;
    metrics->style.opacity = show_metrics ? 220 : 0;
    if (!show_metrics)
        metrics->state &= ~NATIVE_UI_STATE_VISIBLE;
    native_ui_node_set_accessibility_label(metrics, "Session resource metrics");

    set_fixed_height(rule, 1);
    rule->style.background = NATIVE_UI_COLOR_ACCENT;
    rule->style.opacity = 0;
    /* Two complete text line boxes fit the compact 44px header. The
     * decorative rule must not push the model label through its border. */
    if (height < 50)
        rule->state &= ~NATIVE_UI_STATE_VISIBLE;

    char state_label[32];
    sentence_label(state_label, sizeof(state_label),
                    model->state_label && *model->state_label
                        ? model->state_label
                        : native_ui_agent_state_name(model->state));
    int state_width = (int)strlen(state_label) * 7 + 18;
    if (state_width < 58) state_width = 58;
    if (state_width > 104) state_width = 104;
    int turn_width = model->turn > 0 && width >= 620 ? 68 : 0;
    set_fixed_width(status, state_width + turn_width + (turn_width ? 6 : 0));
    status->style.flow = NATIVE_UI_FLOW_ROW;
    status->style.align = NATIVE_UI_ALIGN_CENTER;
    status->style.gap = 6;

    native_ui_node_t *turn = add_node(
        scene, (int)(status - scene->nodes), NATIVE_MASTHEAD_KEY_TURN,
        NATIVE_UI_ELEMENT_TEXT, NATIVE_UI_ROLE_TIMELINE);
    native_ui_node_t *state = add_node(
        scene, (int)(status - scene->nodes), NATIVE_MASTHEAD_KEY_STATE,
        NATIVE_UI_ELEMENT_BADGE, NATIVE_UI_ROLE_STATUS);
    if (!turn || !state) return false;
    set_fixed_width(turn, turn_width);
    turn->style.type = NATIVE_UI_TYPE_LABEL;
    turn->style.foreground = NATIVE_UI_COLOR_TEXT_MUTED;
    turn->style.opacity = 185;
    if (turn_width > 0) {
        char turn_label[32];
        snprintf(turn_label, sizeof(turn_label), "Turn %d", model->turn);
        native_ui_node_set_text(turn, turn_label);
    } else {
        turn->state &= ~NATIVE_UI_STATE_VISIBLE;
    }
    native_ui_node_set_accessibility_label(turn, "Current conversation turn");

    set_fixed_width(state, state_width);
    state->constraints.preferred_height = 20;
    state->constraints.max_height = 22;
    state->agent_state = model->state;
    state->style.type = NATIVE_UI_TYPE_LABEL;
    state->style.foreground = model->state == NATIVE_UI_AGENT_IDLE
        ? NATIVE_UI_COLOR_TEXT_MUTED : NATIVE_UI_COLOR_ACCENT;
    state->style.background = NATIVE_UI_COLOR_SURFACE;
    state->style.border = NATIVE_UI_COLOR_BORDER;
    state->style.border_width = 1;
    state->style.radius = 5;
    state->style.padding = (native_ui_insets_t){0, 8, 0, 8};
    state->style.opacity = 210;
    native_ui_node_set_text(state, state_label);
    native_ui_node_set_accessibility_label(state, "Current agent lifecycle state");

    native_ui_layout(scene);
    return true;
}
