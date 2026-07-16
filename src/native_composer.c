#include "native_composer.h"

#include <stdio.h>
#include <string.h>

static native_ui_node_t *add_node(native_ui_scene_t *scene, int parent,
                                  uint64_t key, native_ui_element_t element,
                                  native_ui_role_t role) {
    int index = native_ui_scene_add(scene, parent, key, element, role);
    return index >= 0 ? native_ui_scene_node(scene, index) : NULL;
}

static int node_index(const native_ui_scene_t *scene,
                      const native_ui_node_t *node) {
    return scene && node ? (int)(node - scene->nodes) : -1;
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

bool native_composer_build(native_ui_scene_t *scene, int width, int height,
                           const native_composer_model_t *model) {
    if (!scene || !model || width < 160 || height < 56) return false;
    native_ui_scene_init(scene, width, height);
    native_ui_node_t *root = native_ui_scene_node(scene, scene->root);
    if (!root) return false;
    root->key = NATIVE_COMPOSER_KEY_ROOT;
    root->element = NATIVE_UI_ELEMENT_SURFACE;
    root->role = NATIVE_UI_ROLE_COMPOSER;
    root->agent_state = model->agent_state;
    root->style.flow = NATIVE_UI_FLOW_COLUMN;
    root->style.align = NATIVE_UI_ALIGN_STRETCH;
    root->style.padding = (native_ui_insets_t){3, 10, 3, 6};
    root->style.gap = 1;
    root->style.background = NATIVE_UI_COLOR_SURFACE_RAISED;
    root->style.border = NATIVE_UI_COLOR_BORDER;
    root->style.border_width = 1;
    root->style.radius = 9;
    root->style.opacity = 230;
    root->state |= NATIVE_UI_STATE_LIVE;
    native_ui_node_set_accessibility_label(root, "Persistent command composer");

    native_ui_node_t *top = add_node(
        scene, scene->root, NATIVE_COMPOSER_KEY_TOP,
        NATIVE_UI_ELEMENT_ROW, NATIVE_UI_ROLE_STATUS);
    native_ui_node_t *divider = add_node(
        scene, scene->root, NATIVE_COMPOSER_KEY_DIVIDER,
        NATIVE_UI_ELEMENT_RULE, NATIVE_UI_ROLE_STATUS);
    native_ui_node_t *input_row = add_node(
        scene, scene->root, NATIVE_COMPOSER_KEY_INPUT_ROW,
        NATIVE_UI_ELEMENT_ROW, NATIVE_UI_ROLE_COMPOSER);
    native_ui_node_t *footer = add_node(
        scene, scene->root, NATIVE_COMPOSER_KEY_FOOTER,
        NATIVE_UI_ELEMENT_ROW, NATIVE_UI_ROLE_STATUS);
    if (!top || !divider || !input_row || !footer) return false;

    set_fixed_height(top, 14);
    top->style.flow = NATIVE_UI_FLOW_ROW;
    top->style.align = NATIVE_UI_ALIGN_CENTER;
    top->style.gap = 8;
    bool exec = model->exec_kind != 0 &&
                model->exec_text && model->exec_text[0];
    native_ui_node_t *glyph = add_node(
        scene, node_index(scene, top), NATIVE_COMPOSER_KEY_EXEC_GLYPH,
        NATIVE_UI_ELEMENT_CUSTOM, NATIVE_UI_ROLE_STATUS);
    native_ui_node_t *title = add_node(
        scene, node_index(scene, top), NATIVE_COMPOSER_KEY_TITLE,
        NATIVE_UI_ELEMENT_TEXT, NATIVE_UI_ROLE_COMPOSER);
    native_ui_node_t *live = add_node(
        scene, node_index(scene, top), NATIVE_COMPOSER_KEY_LIVE,
        NATIVE_UI_ELEMENT_TEXT, NATIVE_UI_ROLE_QUEUE);
    if (!glyph || !title || !live) return false;
    set_fixed_width(glyph, exec ? 14 : 0);
    glyph->value = model->exec_phase;
    glyph->style.foreground = model->exec_kind == 2 ? NATIVE_UI_COLOR_SUCCESS
        : model->exec_kind == 3 ? NATIVE_UI_COLOR_DANGER
        : NATIVE_UI_COLOR_ACCENT;
    glyph->style.opacity = model->exec_kind == 1 ? 255 : model->exec_flash;
    glyph->state |= NATIVE_UI_STATE_LIVE;
    if (exec) {
        native_ui_node_set_accessibility_label(
            glyph, model->exec_label && model->exec_label[0]
                ? model->exec_label : model->exec_text);
    } else {
        /* Idle collapses the glyph without changing node identity, exactly
         * like the optional clock below: zero layout shift on resolve. */
        glyph->state &= ~NATIVE_UI_STATE_VISIBLE;
    }
    title->constraints.min_width = 40;
    title->constraints.grow = 1;
    title->style.type = NATIVE_UI_TYPE_LABEL;
    title->style.foreground = NATIVE_UI_COLOR_TEXT;
    title->style.opacity = 185;
    if (exec) {
        native_ui_node_set_text(title, model->exec_text);
        title->style.foreground = model->exec_kind == 1 ? NATIVE_UI_COLOR_WARNING
            : model->exec_kind == 2 ? NATIVE_UI_COLOR_SUCCESS
            : NATIVE_UI_COLOR_DANGER;
        title->style.opacity = model->exec_kind == 1
            ? 235 : (uint8_t)(120 + model->exec_flash / 2);
        title->state |= NATIVE_UI_STATE_LIVE;
        native_ui_node_set_accessibility_label(
            title, model->exec_label && model->exec_label[0]
                ? model->exec_label : model->exec_text);
    } else {
        native_ui_node_set_text(title, "COMPOSER");
    }
    char live_label[64];
    snprintf(live_label, sizeof(live_label), "LIVE  /  QUEUE %d/%d",
             model->queue_depth,
             model->queue_capacity > 0 ? model->queue_capacity : 8);
    int live_width = (int)strlen(live_label) * 7;
    if (live_width < 100) live_width = 100;
    set_fixed_width(live, live_width);
    live->style.type = NATIVE_UI_TYPE_LABEL;
    live->style.foreground = model->queue_depth > 0
        ? NATIVE_UI_COLOR_WARNING : NATIVE_UI_COLOR_SUCCESS;
    live->style.opacity = 185;
    native_ui_node_set_text(live, live_label);
    native_ui_node_set_accessibility_label(live, "Queued input status");

    set_fixed_height(divider, 1);
    divider->style.background = NATIVE_UI_COLOR_BORDER;
    divider->style.opacity = 48;

    input_row->constraints.min_height = 14;
    input_row->constraints.grow = 1;
    input_row->style.flow = NATIVE_UI_FLOW_ROW;
    input_row->style.align = NATIVE_UI_ALIGN_STRETCH;
    input_row->style.gap = 6;
    native_ui_node_t *accent = add_node(
        scene, node_index(scene, input_row), NATIVE_COMPOSER_KEY_ACCENT,
        NATIVE_UI_ELEMENT_RULE, NATIVE_UI_ROLE_STATUS);
    native_ui_node_t *prompt = add_node(
        scene, node_index(scene, input_row), NATIVE_COMPOSER_KEY_PROMPT,
        NATIVE_UI_ELEMENT_TEXT, NATIVE_UI_ROLE_COMPOSER);
    native_ui_node_t *input = add_node(
        scene, node_index(scene, input_row), NATIVE_COMPOSER_KEY_INPUT,
        NATIVE_UI_ELEMENT_CUSTOM, NATIVE_UI_ROLE_COMPOSER);
    if (!accent || !prompt || !input) return false;
    set_fixed_width(accent, 2);
    accent->style.background = NATIVE_UI_COLOR_ACCENT;
    accent->style.opacity = model->accent_opacity;
    accent->style.radius = 1;
    set_fixed_width(prompt, 12);
    prompt->style.type = NATIVE_UI_TYPE_TITLE;
    prompt->style.foreground = NATIVE_UI_COLOR_ACCENT;
    prompt->style.opacity = 235;
    native_ui_node_set_text(prompt, "›");
    input->constraints.min_width = 32;
    input->constraints.grow = 1;
    input->agent_state = model->agent_state;
    input->state |= NATIVE_UI_STATE_LIVE;
    if (model->active) input->state |= NATIVE_UI_STATE_FOCUSED;
    input->value = (float)model->cursor;
    native_ui_node_set_text(input, model->text ? model->text : "");
    native_ui_composer_layout_t layout = native_ui_composer_layout(
        model->text, model->cursor,
        model->columns > 0 ? model->columns : 1,
        model->max_rows > 0 ? model->max_rows : NATIVE_UI_COMPOSER_MAX_ROWS);
    char input_label[NATIVE_UI_LABEL_CAP];
    snprintf(input_label, sizeof(input_label),
             "Command input, %d line%s, cursor row %d column %d",
             layout.total_rows, layout.total_rows == 1 ? "" : "s",
             layout.cursor_row + 1, layout.cursor_column + 1);
    native_ui_node_set_accessibility_label(input, input_label);

    /* The CoreText raster backend's label line box is 17 px at 1x. Keep the
     * footer taller than that box so its shortcut legend remains inside the
     * retained surface instead of bleeding into the terminal margin. */
    set_fixed_height(footer, 18);
    footer->style.flow = NATIVE_UI_FLOW_ROW;
    footer->style.align = NATIVE_UI_ALIGN_CENTER;
    footer->style.gap = 8;
    native_ui_node_t *hint = add_node(
        scene, node_index(scene, footer), NATIVE_COMPOSER_KEY_HINT,
        NATIVE_UI_ELEMENT_TEXT, NATIVE_UI_ROLE_STATUS);
    native_ui_node_t *clock = add_node(
        scene, node_index(scene, footer), NATIVE_COMPOSER_KEY_CLOCK,
        NATIVE_UI_ELEMENT_TEXT, NATIVE_UI_ROLE_STATUS);
    if (!hint || !clock) return false;
    hint->constraints.min_width = 30;
    hint->constraints.grow = 1;
    hint->style.type = NATIVE_UI_TYPE_LABEL;
    hint->style.foreground = NATIVE_UI_COLOR_TEXT_MUTED;
    hint->style.opacity = 158;
    native_ui_node_set_text(
        hint, model->compact
            ? "ENTER SEND  /  PGUP PGDN HISTORY"
            : "ENTER SEND  /  OPTION+ENTER NEWLINE  /  PGUP PGDN HISTORY  /  CTRL+C INTERRUPT");
    if (model->clock && *model->clock) {
        set_fixed_width(clock, 42);
        clock->style.type = NATIVE_UI_TYPE_LABEL;
        clock->style.foreground = NATIVE_UI_COLOR_TEXT_MUTED;
        clock->style.opacity = 185;
        native_ui_node_set_text(clock, model->clock);
        native_ui_node_set_accessibility_label(clock, "Local time");
    } else {
        set_fixed_width(clock, 0);
        clock->state &= ~NATIVE_UI_STATE_VISIBLE;
    }

    native_ui_layout(scene);
    return true;
}
