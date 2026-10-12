#ifndef DSCO_NATIVE_WINDOW_LAYOUT_H
#define DSCO_NATIVE_WINDOW_LAYOUT_H

#include "native_ui.h"

#define NATIVE_WINDOW_LAYOUT_MAX_NODES 32
#define NATIVE_WINDOW_LAYOUT_MAX_GAP 64
/* Reserved by the retained-window adapter; ordinary window IDs are leaves. */
#define NATIVE_WINDOW_LAYOUT_ROOT_ID UINT64_MAX

typedef enum {
    NATIVE_WINDOW_LAYOUT_LEAF,
    NATIVE_WINDOW_LAYOUT_SPLIT,
    NATIVE_WINDOW_LAYOUT_GRID
} native_window_layout_kind_t;
typedef enum {
    NATIVE_WINDOW_LAYOUT_HORIZONTAL,
    NATIVE_WINDOW_LAYOUT_VERTICAL
} native_window_layout_axis_t;

typedef struct {
    uint64_t id, parent_id; /* 0 means root; identity is not a storage/z index. */
    native_window_layout_kind_t kind;
    native_window_layout_axis_t axis;
    int columns; /* grid: 0 chooses ceil(sqrt(child_count)) */
    int gap;     /* bounded cell inset, split evenly across the two edges */
    native_ui_rect_t rect, clip; /* half-open; empty geometry is permitted */
} native_window_layout_node_t;
typedef struct {
    int count;
    native_window_layout_node_t nodes[NATIVE_WINDOW_LAYOUT_MAX_NODES];
} native_window_layout_t;

/* Fixed storage, no allocation or UI side effects. Insert one root first, then
 * parents before children. Sibling insertion order is the stable layout order.
 * Splits divide their axis equally; grids are row-major. Leaves cannot parent.
 * All failures leave the caller's layout unchanged. */
void native_window_layout_init(native_window_layout_t *layout);
bool native_window_layout_add(native_window_layout_t *layout, uint64_t id,
    uint64_t parent_id, native_window_layout_kind_t kind,
    native_window_layout_axis_t axis, int columns, int gap);
const native_window_layout_node_t *native_window_layout_find(
    const native_window_layout_t *layout, uint64_t id);
/* Also validates public structs (duplicate IDs, parent order/cycles, bounds).
 * Child rects stay inside parents; clips intersect all ancestor clips and the
 * supplied viewport. Zero-area/tiny frames never produce negative dimensions.
 * Overflowing rectangles are rejected before arithmetic. */
bool native_window_layout_compute(native_window_layout_t *layout,
    native_ui_rect_t frame, native_ui_rect_t viewport);

#endif
