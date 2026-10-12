#include "native_window_layout.h"
#include <limits.h>
#include <string.h>

static int index_of(const native_window_layout_t *layout, uint64_t id) {
    if (!layout || layout->count < 0 || layout->count > NATIVE_WINDOW_LAYOUT_MAX_NODES) return -1;
    for (int i = 0; i < layout->count; i++) if (layout->nodes[i].id == id) return i;
    return -1;
}

/* Check endpoints in a wider type before any int geometry arithmetic. With
 * nonnegative extents the lower endpoint cannot underflow. */
static bool valid_rect(native_ui_rect_t r) {
    return r.width >= 0 && r.height >= 0 &&
        (int64_t)r.x + r.width <= INT_MAX && (int64_t)r.y + r.height <= INT_MAX;
}

static bool valid(const native_window_layout_t *layout) {
    if (!layout || layout->count < 0 || layout->count > NATIVE_WINDOW_LAYOUT_MAX_NODES) return false;
    for (int i = 0; i < layout->count; i++) {
        const native_window_layout_node_t *n = &layout->nodes[i];
        if (!n->id || index_of(layout, n->id) != i ||
            n->kind < NATIVE_WINDOW_LAYOUT_LEAF || n->kind > NATIVE_WINDOW_LAYOUT_GRID ||
            n->axis < NATIVE_WINDOW_LAYOUT_HORIZONTAL || n->axis > NATIVE_WINDOW_LAYOUT_VERTICAL ||
            n->columns < 0 || n->columns > NATIVE_WINDOW_LAYOUT_MAX_NODES ||
            (n->kind != NATIVE_WINDOW_LAYOUT_GRID && n->columns) ||
            n->gap < 0 || n->gap > NATIVE_WINDOW_LAYOUT_MAX_GAP ||
            (n->kind == NATIVE_WINDOW_LAYOUT_LEAF && n->gap) ||
            !valid_rect(n->rect) || !valid_rect(n->clip)) return false;
        if (!i) { if (n->parent_id) return false; }
        else {
            int parent = index_of(layout, n->parent_id);
            if (!n->parent_id || parent < 0 || parent >= i ||
                layout->nodes[parent].kind == NATIVE_WINDOW_LAYOUT_LEAF) return false;
        }
    }
    return true;
}

void native_window_layout_init(native_window_layout_t *layout) {
    if (layout) memset(layout, 0, sizeof(*layout));
}

const native_window_layout_node_t *native_window_layout_find(
    const native_window_layout_t *layout, uint64_t id) {
    int i = index_of(layout, id);
    return i < 0 ? NULL : &layout->nodes[i];
}

bool native_window_layout_add(native_window_layout_t *layout, uint64_t id,
    uint64_t parent_id, native_window_layout_kind_t kind,
    native_window_layout_axis_t axis, int columns, int gap) {
    if (!valid(layout) || layout->count == NATIVE_WINDOW_LAYOUT_MAX_NODES) return false;
    native_window_layout_t next = *layout;
    next.nodes[next.count++] = (native_window_layout_node_t){
        .id = id, .parent_id = parent_id, .kind = kind, .axis = axis,
        .columns = columns, .gap = gap};
    if (!valid(&next)) return false;
    *layout = next;
    return true;
}

static native_ui_rect_t intersect(native_ui_rect_t a, native_ui_rect_t b) {
    int x = a.x > b.x ? a.x : b.x, y = a.y > b.y ? a.y : b.y;
    int right = a.x + a.width < b.x + b.width ? a.x + a.width : b.x + b.width;
    int bottom = a.y + a.height < b.y + b.height ? a.y + a.height : b.y + b.height;
    return (native_ui_rect_t){x, y, right > x ? right - x : 0, bottom > y ? bottom - y : 0};
}

bool native_window_layout_compute(native_window_layout_t *layout,
    native_ui_rect_t frame, native_ui_rect_t viewport) {
    if (!valid(layout) || !valid_rect(frame) || !valid_rect(viewport)) return false;
    native_window_layout_t next = *layout;
    for (int i = 0; i < next.count; i++) {
        native_window_layout_node_t *n = &next.nodes[i];
        if (!i) { n->rect = frame; n->clip = intersect(frame, viewport); continue; }
        const native_window_layout_node_t *p = &next.nodes[index_of(&next, n->parent_id)];
        int siblings = 0, rank = 0;
        for (int j = 1; j < next.count; j++) if (next.nodes[j].parent_id == n->parent_id) {
            if (j < i) rank++;
            siblings++;
        }
        int cols = 1, rows = 1;
        if (p->kind == NATIVE_WINDOW_LAYOUT_GRID) {
            cols = p->columns;
            if (!cols) { cols = 1; while (cols * cols < siblings) cols++; }
            rows = (siblings + cols - 1) / cols;
        } else if (p->axis == NATIVE_WINDOW_LAYOUT_HORIZONTAL) cols = siblings;
        else rows = siblings;
        int col = rank % cols, row = rank / cols;
        native_ui_rect_t b = p->rect;
        int x0 = (int)(b.x + (int64_t)b.width * col / cols);
        int x1 = (int)(b.x + (int64_t)b.width * (col + 1) / cols);
        int y0 = (int)(b.y + (int64_t)b.height * row / rows);
        int y1 = (int)(b.y + (int64_t)b.height * (row + 1) / rows);
        int gx = p->gap < x1 - x0 ? p->gap : x1 - x0;
        int gy = p->gap < y1 - y0 ? p->gap : y1 - y0;
        n->rect = (native_ui_rect_t){x0 + gx / 2, y0 + gy / 2, x1 - x0 - gx, y1 - y0 - gy};
        n->clip = intersect(n->rect, p->clip);
    }
    *layout = next;
    return true;
}
