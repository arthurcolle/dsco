#include "native_ui.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

static int clamp_dimension(int value, int minimum, int maximum) {
    if (value < minimum) value = minimum;
    if (maximum >= 0 && value > maximum) value = maximum;
    return value;
}

static bool rect_empty(native_ui_rect_t rect) {
    return rect.width <= 0 || rect.height <= 0;
}

static bool rect_equal(native_ui_rect_t a, native_ui_rect_t b) {
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}

static bool rect_touches(native_ui_rect_t a, native_ui_rect_t b) {
    return a.x <= b.x + b.width && b.x <= a.x + a.width &&
           a.y <= b.y + b.height && b.y <= a.y + a.height;
}

static native_ui_rect_t rect_union(native_ui_rect_t a, native_ui_rect_t b) {
    if (rect_empty(a)) return b;
    if (rect_empty(b)) return a;
    int left = a.x < b.x ? a.x : b.x;
    int top = a.y < b.y ? a.y : b.y;
    int a_right = a.x + a.width;
    int b_right = b.x + b.width;
    int a_bottom = a.y + a.height;
    int b_bottom = b.y + b.height;
    int right = a_right > b_right ? a_right : b_right;
    int bottom = a_bottom > b_bottom ? a_bottom : b_bottom;
    return (native_ui_rect_t){left, top, right - left, bottom - top};
}

static native_ui_rect_t content_rect(const native_ui_node_t *node) {
    native_ui_rect_t rect = node->frame;
    rect.x += node->style.padding.left;
    rect.y += node->style.padding.top;
    rect.width -= node->style.padding.left + node->style.padding.right;
    rect.height -= node->style.padding.top + node->style.padding.bottom;
    if (rect.width < 0) rect.width = 0;
    if (rect.height < 0) rect.height = 0;
    return rect;
}

static bool node_visible(const native_ui_node_t *node) {
    return node && (node->state & NATIVE_UI_STATE_VISIBLE) != 0;
}

static bool node_interactive_in_tree(const native_ui_scene_t *scene, int index) {
    for (int at = index; at >= 0; at = scene->nodes[at].parent) {
        const native_ui_node_t *node = &scene->nodes[at];
        if (!node_visible(node) || !(node->state & NATIVE_UI_STATE_ENABLED) ||
            (node->state & NATIVE_UI_STATE_DISABLED))
            return false;
    }
    return true;
}

static native_ui_constraints_t default_constraints(void) {
    return (native_ui_constraints_t){
        .preferred_width = -1,
        .preferred_height = -1,
        .max_width = -1,
        .max_height = -1,
        .grow = 0,
        .shrink = 1,
    };
}

static native_ui_style_t default_style(native_ui_element_t element) {
    native_ui_flow_t flow = NATIVE_UI_FLOW_NONE;
    if (element == NATIVE_UI_ELEMENT_ROOT || element == NATIVE_UI_ELEMENT_STACK)
        flow = NATIVE_UI_FLOW_COLUMN;
    else if (element == NATIVE_UI_ELEMENT_ROW)
        flow = NATIVE_UI_FLOW_ROW;
    else if (element == NATIVE_UI_ELEMENT_GRID)
        flow = NATIVE_UI_FLOW_GRID;
    else if (element == NATIVE_UI_ELEMENT_OVERLAY)
        flow = NATIVE_UI_FLOW_OVERLAY;
    return (native_ui_style_t){
        .flow = flow,
        .align = NATIVE_UI_ALIGN_STRETCH,
        .justify = NATIVE_UI_ALIGN_START,
        .grid_columns = 1,
        .foreground = NATIVE_UI_COLOR_TEXT,
        .background = NATIVE_UI_COLOR_CLEAR,
        .border = NATIVE_UI_COLOR_CLEAR,
        .type = NATIVE_UI_TYPE_BODY,
        .opacity = 255,
    };
}

void native_ui_scene_init(native_ui_scene_t *scene, int width, int height) {
    if (!scene) return;
    memset(scene, 0, sizeof(*scene));
    if (width < 0) width = 0;
    if (height < 0) height = 0;
    scene->viewport = (native_ui_rect_t){0, 0, width, height};
    scene->density = native_ui_density_for_size(width, height);
    scene->root = 0;
    scene->focused = -1;
    scene->count = 1;
    scene->generation = 1;
    native_ui_node_t *root = &scene->nodes[0];
    root->key = 1;
    root->element = NATIVE_UI_ELEMENT_ROOT;
    root->role = NATIVE_UI_ROLE_AGENT_SHELL;
    root->state = NATIVE_UI_STATE_VISIBLE | NATIVE_UI_STATE_ENABLED;
    root->parent = -1;
    root->first_child = -1;
    root->last_child = -1;
    root->next_sibling = -1;
    root->constraints = default_constraints();
    root->style = default_style(root->element);
    root->frame = scene->viewport;
}

int native_ui_scene_add(native_ui_scene_t *scene, int parent, uint64_t key,
                        native_ui_element_t element, native_ui_role_t role) {
    if (!scene || scene->count >= NATIVE_UI_MAX_NODES || key == 0 ||
        parent < 0 || parent >= scene->count)
        return -1;
    for (int i = 0; i < scene->count; i++)
        if (scene->nodes[i].key == key) return -1;
    int index = scene->count++;
    native_ui_node_t *node = &scene->nodes[index];
    memset(node, 0, sizeof(*node));
    node->key = key;
    node->element = element;
    node->role = role;
    node->state = NATIVE_UI_STATE_VISIBLE | NATIVE_UI_STATE_ENABLED;
    node->parent = parent;
    node->first_child = -1;
    node->last_child = -1;
    node->next_sibling = -1;
    node->constraints = default_constraints();
    node->style = default_style(element);
    native_ui_node_t *parent_node = &scene->nodes[parent];
    if (parent_node->first_child < 0)
        parent_node->first_child = index;
    else
        scene->nodes[parent_node->last_child].next_sibling = index;
    parent_node->last_child = index;
    scene->generation++;
    return index;
}

native_ui_node_t *native_ui_scene_node(native_ui_scene_t *scene, int index) {
    if (!scene || index < 0 || index >= scene->count) return NULL;
    return &scene->nodes[index];
}

void native_ui_node_set_text(native_ui_node_t *node, const char *text) {
    if (!node) return;
    snprintf(node->text, sizeof(node->text), "%s", text ? text : "");
}

void native_ui_node_set_accessibility_label(native_ui_node_t *node, const char *label) {
    if (!node) return;
    snprintf(node->accessibility_label, sizeof(node->accessibility_label),
             "%s", label ? label : "");
}

static int visible_child_count(const native_ui_scene_t *scene, const native_ui_node_t *node) {
    int count = 0;
    for (int child = node->first_child; child >= 0; child = scene->nodes[child].next_sibling)
        if (node_visible(&scene->nodes[child])) count++;
    return count;
}

static void layout_node(native_ui_scene_t *scene, int index);

static void layout_overlay(native_ui_scene_t *scene, native_ui_node_t *node) {
    native_ui_rect_t content = content_rect(node);
    for (int child = node->first_child; child >= 0; child = scene->nodes[child].next_sibling) {
        native_ui_node_t *item = &scene->nodes[child];
        if (!node_visible(item)) continue;
        int width = item->constraints.preferred_width >= 0 ?
                    item->constraints.preferred_width : content.width;
        int height = item->constraints.preferred_height >= 0 ?
                     item->constraints.preferred_height : content.height;
        width = clamp_dimension(width, item->constraints.min_width,
                                item->constraints.max_width);
        height = clamp_dimension(height, item->constraints.min_height,
                                 item->constraints.max_height);
        int x = content.x;
        int y = content.y;
        if (node->style.align == NATIVE_UI_ALIGN_CENTER) x += (content.width - width) / 2;
        else if (node->style.align == NATIVE_UI_ALIGN_END) x += content.width - width;
        if (node->style.justify == NATIVE_UI_ALIGN_CENTER) y += (content.height - height) / 2;
        else if (node->style.justify == NATIVE_UI_ALIGN_END) y += content.height - height;
        item->frame = (native_ui_rect_t){x, y, width, height};
        layout_node(scene, child);
    }
}

static void layout_grid(native_ui_scene_t *scene, native_ui_node_t *node) {
    native_ui_rect_t content = content_rect(node);
    int count = visible_child_count(scene, node);
    if (count == 0) return;
    int columns = node->style.grid_columns > 0 ? node->style.grid_columns : 1;
    if (columns > count) columns = count;
    int rows = (count + columns - 1) / columns;
    int gap = node->style.gap;
    int cell_width = (content.width - gap * (columns - 1)) / columns;
    int cell_height = (content.height - gap * (rows - 1)) / rows;
    if (cell_width < 0) cell_width = 0;
    if (cell_height < 0) cell_height = 0;
    int at = 0;
    for (int child = node->first_child; child >= 0; child = scene->nodes[child].next_sibling) {
        native_ui_node_t *item = &scene->nodes[child];
        if (!node_visible(item)) continue;
        int column = at % columns;
        int row = at / columns;
        item->frame = (native_ui_rect_t){
            content.x + column * (cell_width + gap),
            content.y + row * (cell_height + gap),
            cell_width,
            cell_height,
        };
        layout_node(scene, child);
        at++;
    }
}

static int child_basis(const native_ui_node_t *child, bool row) {
    int preferred = row ? child->constraints.preferred_width :
                          child->constraints.preferred_height;
    int minimum = row ? child->constraints.min_width : child->constraints.min_height;
    int maximum = row ? child->constraints.max_width : child->constraints.max_height;
    return clamp_dimension(preferred >= 0 ? preferred : minimum, minimum, maximum);
}

static void layout_linear(native_ui_scene_t *scene, native_ui_node_t *node, bool row) {
    native_ui_rect_t content = content_rect(node);
    int count = visible_child_count(scene, node);
    if (count == 0) return;
    int available = (row ? content.width : content.height) - node->style.gap * (count - 1);
    if (available < 0) available = 0;
    int used = 0;
    int grow_total = 0;
    int shrink_total = 0;
    for (int child = node->first_child; child >= 0; child = scene->nodes[child].next_sibling) {
        native_ui_node_t *item = &scene->nodes[child];
        if (!node_visible(item)) continue;
        used += child_basis(item, row);
        grow_total += item->constraints.grow;
        shrink_total += item->constraints.shrink;
    }
    int remaining = available - used;
    int cursor = row ? content.x : content.y;
    if (remaining > 0 && grow_total == 0) {
        if (node->style.justify == NATIVE_UI_ALIGN_CENTER) cursor += remaining / 2;
        else if (node->style.justify == NATIVE_UI_ALIGN_END) cursor += remaining;
    }
    int flexible_space = remaining > 0 ? remaining : -remaining;
    int flexible_weight = remaining > 0 ? grow_total : shrink_total;
    for (int child = node->first_child; child >= 0; child = scene->nodes[child].next_sibling) {
        native_ui_node_t *item = &scene->nodes[child];
        if (!node_visible(item)) continue;
        int main_size = child_basis(item, row);
        if (remaining > 0 && grow_total > 0) {
            int weight = item->constraints.grow;
            int share = flexible_weight > 0 ? flexible_space * weight / flexible_weight : 0;
            main_size += share;
            flexible_space -= share;
            flexible_weight -= weight;
        } else if (remaining < 0 && shrink_total > 0) {
            int weight = item->constraints.shrink;
            int share = flexible_weight > 0 ? flexible_space * weight / flexible_weight : 0;
            main_size -= share;
            flexible_space -= share;
            flexible_weight -= weight;
        }
        int main_min = row ? item->constraints.min_width : item->constraints.min_height;
        int main_max = row ? item->constraints.max_width : item->constraints.max_height;
        main_size = clamp_dimension(main_size, main_min, main_max);
        int cross_available = row ? content.height : content.width;
        int preferred_cross = row ? item->constraints.preferred_height :
                                    item->constraints.preferred_width;
        int cross_min = row ? item->constraints.min_height : item->constraints.min_width;
        int cross_max = row ? item->constraints.max_height : item->constraints.max_width;
        int cross_size = node->style.align == NATIVE_UI_ALIGN_STRETCH || preferred_cross < 0 ?
                         cross_available : preferred_cross;
        cross_size = clamp_dimension(cross_size, cross_min, cross_max);
        int cross_origin = row ? content.y : content.x;
        if (node->style.align == NATIVE_UI_ALIGN_CENTER)
            cross_origin += (cross_available - cross_size) / 2;
        else if (node->style.align == NATIVE_UI_ALIGN_END)
            cross_origin += cross_available - cross_size;
        item->frame = row ?
            (native_ui_rect_t){cursor, cross_origin, main_size, cross_size} :
            (native_ui_rect_t){cross_origin, cursor, cross_size, main_size};
        cursor += main_size + node->style.gap;
        layout_node(scene, child);
    }
}

static void layout_node(native_ui_scene_t *scene, int index) {
    native_ui_node_t *node = &scene->nodes[index];
    if (!node_visible(node)) return;
    switch (node->style.flow) {
        case NATIVE_UI_FLOW_ROW: layout_linear(scene, node, true); break;
        case NATIVE_UI_FLOW_COLUMN: layout_linear(scene, node, false); break;
        case NATIVE_UI_FLOW_GRID: layout_grid(scene, node); break;
        case NATIVE_UI_FLOW_OVERLAY: layout_overlay(scene, node); break;
        case NATIVE_UI_FLOW_NONE: break;
    }
}

void native_ui_layout(native_ui_scene_t *scene) {
    if (!scene || scene->count < 1 || scene->root < 0 || scene->root >= scene->count) return;
    scene->nodes[scene->root].frame = scene->viewport;
    layout_node(scene, scene->root);
    scene->generation++;
}

static uint64_t hash_bytes(uint64_t hash, const void *data, size_t size) {
    const unsigned char *bytes = data;
    for (size_t i = 0; i < size; i++) {
        hash ^= bytes[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static uint64_t visual_hash(const native_ui_node_t *node) {
    uint64_t hash = UINT64_C(1469598103934665603);
    hash = hash_bytes(hash, &node->element, sizeof(node->element));
    hash = hash_bytes(hash, &node->role, sizeof(node->role));
    hash = hash_bytes(hash, &node->agent_state, sizeof(node->agent_state));
    hash = hash_bytes(hash, &node->state, sizeof(node->state));
    hash = hash_bytes(hash, &node->style, sizeof(node->style));
    hash = hash_bytes(hash, &node->value, sizeof(node->value));
    hash = hash_bytes(hash, node->text, strlen(node->text));
    return hash;
}

static const native_ui_node_t *find_key(const native_ui_scene_t *scene, uint64_t key) {
    if (!scene) return NULL;
    for (int i = 0; i < scene->count; i++)
        if (scene->nodes[i].key == key) return &scene->nodes[i];
    return NULL;
}

static void damage_add(native_ui_damage_t *damage, native_ui_rect_t rect) {
    if (!damage || rect_empty(rect) || damage->full_repaint) return;
    for (int i = 0; i < damage->count;) {
        if (rect_touches(damage->regions[i], rect)) {
            rect = rect_union(damage->regions[i], rect);
            damage->regions[i] = damage->regions[--damage->count];
            i = 0; /* The larger union may now touch any prior region. */
        } else {
            i++;
        }
    }
    if (damage->count >= NATIVE_UI_MAX_DIRTY_REGIONS) {
        damage->count = 0;
        damage->full_repaint = true;
        return;
    }
    damage->regions[damage->count++] = rect;
}

void native_ui_diff(const native_ui_scene_t *previous, const native_ui_scene_t *current,
                    native_ui_damage_t *damage) {
    if (!damage) return;
    memset(damage, 0, sizeof(*damage));
    if (!current) return;
    if (!previous || !rect_equal(previous->viewport, current->viewport)) {
        damage->full_repaint = true;
        return;
    }
    for (int i = 0; i < current->count; i++) {
        const native_ui_node_t *now = &current->nodes[i];
        const native_ui_node_t *before = find_key(previous, now->key);
        if (!before) {
            damage_add(damage, now->frame);
        } else if (!rect_equal(before->frame, now->frame) ||
                   visual_hash(before) != visual_hash(now)) {
            damage_add(damage, rect_union(before->frame, now->frame));
        }
    }
    for (int i = 0; i < previous->count; i++)
        if (!find_key(current, previous->nodes[i].key))
            damage_add(damage, previous->nodes[i].frame);
}

static bool point_in_rect(native_ui_rect_t rect, int x, int y) {
    return x >= rect.x && y >= rect.y && x < rect.x + rect.width &&
           y < rect.y + rect.height;
}

static int hit_test_node(const native_ui_scene_t *scene, int index, int x, int y) {
    const native_ui_node_t *node = &scene->nodes[index];
    if (!node_visible(node) || !(node->state & NATIVE_UI_STATE_ENABLED) ||
        (node->state & NATIVE_UI_STATE_DISABLED))
        return -1;
    bool inside = point_in_rect(node->frame, x, y);
    if (!inside && (node->state & NATIVE_UI_STATE_CLIPS)) return -1;

    int before_z = INT_MAX;
    int before_index = scene->count;
    for (;;) {
        int candidate = -1;
        int candidate_z = INT_MIN;
        for (int child = node->first_child; child >= 0;
             child = scene->nodes[child].next_sibling) {
            int z = scene->nodes[child].style.z_index;
            if (z > before_z || (z == before_z && child >= before_index)) continue;
            if (candidate < 0 || z > candidate_z || (z == candidate_z && child > candidate)) {
                candidate = child;
                candidate_z = z;
            }
        }
        if (candidate < 0) break;
        int hit = hit_test_node(scene, candidate, x, y);
        if (hit >= 0) return hit;
        before_z = candidate_z;
        before_index = candidate;
    }
    return inside ? index : -1;
}

int native_ui_hit_test(const native_ui_scene_t *scene, int x, int y) {
    if (!scene || scene->root < 0 || scene->root >= scene->count) return -1;
    return hit_test_node(scene, scene->root, x, y);
}

int native_ui_focus_move(native_ui_scene_t *scene, bool backwards) {
    if (!scene || scene->count < 1) return -1;
    int start = scene->focused;
    if (start < 0) start = backwards ? 0 : -1;
    for (int step = 1; step <= scene->count; step++) {
        int index = backwards ? start - step : start + step;
        while (index < 0) index += scene->count;
        index %= scene->count;
        native_ui_node_t *node = &scene->nodes[index];
        if (node_interactive_in_tree(scene, index) &&
            (node->state & NATIVE_UI_STATE_FOCUSABLE)) {
            if (scene->focused >= 0 && scene->focused < scene->count)
                scene->nodes[scene->focused].state &= ~NATIVE_UI_STATE_FOCUSED;
            scene->focused = index;
            node->state |= NATIVE_UI_STATE_FOCUSED;
            scene->generation++;
            return index;
        }
    }
    return -1;
}

bool native_ui_dispatch(native_ui_scene_t *scene, const native_ui_event_t *event) {
    if (!scene || !event) return false;
    int target = -1;
    if (event->type == NATIVE_UI_EVENT_POINTER_MOVE ||
        event->type == NATIVE_UI_EVENT_POINTER_DOWN ||
        event->type == NATIVE_UI_EVENT_POINTER_UP ||
        event->type == NATIVE_UI_EVENT_SCROLL)
        target = native_ui_hit_test(scene, event->x, event->y);
    else
        target = scene->focused >= 0 ? scene->focused : scene->root;
    for (int index = target; index >= 0; index = scene->nodes[index].parent) {
        native_ui_node_t *node = &scene->nodes[index];
        if (node->on_event && node->on_event(scene, node, event, node->event_context))
            return true;
    }
    return false;
}

static void render_node(const native_ui_scene_t *scene, int index,
                        const native_ui_backend_t *backend, void *context) {
    const native_ui_node_t *node = &scene->nodes[index];
    if (!node_visible(node)) return;
    if (backend->fill_rect && node->style.background != NATIVE_UI_COLOR_CLEAR)
        backend->fill_rect(context, node->frame, node->style.background,
                           node->style.opacity, node->style.radius);
    if (backend->stroke_rect && node->style.border != NATIVE_UI_COLOR_CLEAR &&
        node->style.border_width > 0)
        backend->stroke_rect(context, node->frame, node->style.border,
                             node->style.border_width, node->style.radius);
    if ((node->element == NATIVE_UI_ELEMENT_TEXT ||
         node->element == NATIVE_UI_ELEMENT_BADGE ||
         node->element == NATIVE_UI_ELEMENT_INPUT) && backend->draw_text)
        backend->draw_text(context, node->frame, node->text, node->style.type,
                           node->style.foreground, node->style.opacity);
    else if (node->element == NATIVE_UI_ELEMENT_ICON && backend->draw_icon)
        backend->draw_icon(context, node->frame, node->text, node->style.foreground,
                           node->style.opacity);
    else if ((node->element == NATIVE_UI_ELEMENT_CUSTOM ||
              node->element == NATIVE_UI_ELEMENT_METER ||
              node->element == NATIVE_UI_ELEMENT_SPARKLINE ||
              node->element == NATIVE_UI_ELEMENT_IMAGE) && backend->draw_custom)
        backend->draw_custom(context, node);
    bool clipped = (node->state & NATIVE_UI_STATE_CLIPS) && backend->push_clip;
    if (clipped) backend->push_clip(context, node->frame);
    int after_z = INT_MIN;
    int after_index = -1;
    for (;;) {
        int candidate = -1;
        int candidate_z = INT_MAX;
        for (int child = node->first_child; child >= 0;
             child = scene->nodes[child].next_sibling) {
            int z = scene->nodes[child].style.z_index;
            if (z < after_z || (z == after_z && child <= after_index)) continue;
            if (candidate < 0 || z < candidate_z || (z == candidate_z && child < candidate)) {
                candidate = child;
                candidate_z = z;
            }
        }
        if (candidate < 0) break;
        render_node(scene, candidate, backend, context);
        after_z = candidate_z;
        after_index = candidate;
    }
    if (clipped && backend->pop_clip) backend->pop_clip(context);
}

void native_ui_render(const native_ui_scene_t *scene, const native_ui_scene_t *previous,
                      const native_ui_backend_t *backend, void *context) {
    if (!scene || !backend || scene->root < 0 || scene->root >= scene->count) return;
    native_ui_damage_t damage;
    native_ui_diff(previous, scene, &damage);
    if (backend->begin_frame) backend->begin_frame(context, scene->viewport, &damage);
    render_node(scene, scene->root, backend, context);
    if (backend->end_frame) backend->end_frame(context);
}

native_ui_density_t native_ui_density_for_size(int width, int height) {
    if (width < 720 || height < 320) return NATIVE_UI_DENSITY_COMPACT;
    if (width >= 1400 && height >= 440) return NATIVE_UI_DENSITY_EXPANDED;
    return NATIVE_UI_DENSITY_DENSE;
}

native_ui_viewport_metrics_t native_ui_terminal_viewport(int columns, int rows,
                                                         int physical_width,
                                                         int physical_height,
                                                         int requested_scale) {
    native_ui_viewport_metrics_t metrics = {
        .columns = columns > 0 ? columns : 80,
        .rows = rows > 0 ? rows : 24,
        .physical_width = physical_width,
        .physical_height = physical_height,
        .backing_scale = requested_scale >= 1 && requested_scale <= 4 ?
                         requested_scale : 1,
    };
    if (physical_width > 0)
        metrics.physical_cell_width = physical_width / metrics.columns;
    if (physical_height > 0)
        metrics.physical_cell_height = physical_height / metrics.rows;

    if (requested_scale < 1 || requested_scale > 4) {
        int cell_width = metrics.physical_cell_width;
        int cell_height = metrics.physical_cell_height;
        /* Terminal cell dimensions include the user's font size, window zoom,
         * and padding; they are not a reliable display-DPR API. In particular,
         * a 2x Retina panel with a large font was previously misclassified as
         * 3x, causing Kitty to interpolate a too-small framebuffer across the
         * full cell grid. Auto-detection therefore distinguishes only 1x from
         * HiDPI 2x. A real 3x backing grid must use the explicit DPR override
         * until the terminal exposes its backing scale directly. */
        if (cell_height >= 24 || cell_width >= 14)
            metrics.backing_scale = 2;
    }
    metrics.logical_width = physical_width > 0 ?
                            physical_width / metrics.backing_scale :
                            metrics.columns * 10;
    metrics.logical_height = physical_height > 0 ?
                             physical_height / metrics.backing_scale :
                             metrics.rows * 20;
    return metrics;
}

native_ui_agent_shell_layout_t native_ui_agent_shell_layout(int width, int height) {
    if (width < 0) width = 0;
    if (height < 0) height = 0;
    native_ui_agent_shell_layout_t layout = {0};
    layout.density = native_ui_density_for_size(width, height);
    layout.shows_inspector = layout.density == NATIVE_UI_DENSITY_EXPANDED;
    layout.outer_margin = 8;
    layout.inner_padding = 12;
    layout.gap = layout.shows_inspector ? 6 : 0;
    int header_height = height < 300 ? 52 : 56;
    int composer_height = height / 6;
    if (composer_height < 62) composer_height = 62;
    if (composer_height > 78) composer_height = 78;
    if (height < 300) composer_height = 56;
    int transcript_y = header_height + 4;
    int transcript_height = height - transcript_y - composer_height - 4;
    if (transcript_height < 0) transcript_height = 0;
    int inspector_width = 0;
    if (layout.shows_inspector) {
        inspector_width = width / 5;
        if (inspector_width < 190) inspector_width = 190;
        if (inspector_width > 215) inspector_width = 215;
    }
    int content_width = width - layout.outer_margin * 2 - inspector_width - layout.gap;
    if (content_width < 0) content_width = 0;
    layout.header = (native_ui_rect_t){layout.outer_margin, 4,
                                      width - layout.outer_margin * 2,
                                      header_height - 4};
    layout.transcript = (native_ui_rect_t){layout.outer_margin, transcript_y,
                                          content_width, transcript_height};
    layout.inspector = (native_ui_rect_t){layout.outer_margin + content_width + layout.gap,
                                         transcript_y, inspector_width, transcript_height};
    layout.composer = (native_ui_rect_t){layout.outer_margin, height - composer_height,
                                        width - layout.outer_margin * 2,
                                        composer_height - 6};
    if (layout.header.width < 0) layout.header.width = 0;
    if (layout.header.height < 0) layout.header.height = 0;
    if (layout.composer.width < 0) layout.composer.width = 0;
    if (layout.composer.height < 0) layout.composer.height = 0;
    return layout;
}

const char *native_ui_role_name(native_ui_role_t role) {
    static const char *names[] = {
        "none", "agent-shell", "header", "status", "transcript", "message",
        "reasoning-activity", "tool-activity", "agent-topology", "artifact",
        "permission", "plan", "queue", "timeline", "composer", "command-palette",
        "markdown", "math", "code", "notification", "toast", "modal", "scrim",
    };
    if (role < NATIVE_UI_ROLE_NONE || role > NATIVE_UI_ROLE_SCRIM) return "unknown";
    return names[role];
}

const char *native_ui_agent_state_name(native_ui_agent_state_t state) {
    static const char *names[] = {
        "idle", "reasoning", "executing", "responding", "waiting", "blocked", "error",
    };
    if (state < NATIVE_UI_AGENT_IDLE || state > NATIVE_UI_AGENT_ERROR) return "unknown";
    return names[state];
}
