#include "native_window_layout.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long checks;
#define CHECK(expr) do { \
    checks++; \
    if (!(expr)) { \
        fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); \
        exit(1); \
    } \
} while (0)

static native_ui_rect_t rect(int x, int y, int w, int h) {
    return (native_ui_rect_t){x, y, w, h};
}

static void same_rect(native_ui_rect_t a, native_ui_rect_t b) {
    CHECK(a.x == b.x && a.y == b.y &&
          a.width == b.width && a.height == b.height);
}

static void add(native_window_layout_t *l, uint64_t id, uint64_t parent,
                native_window_layout_kind_t kind,
                native_window_layout_axis_t axis, int columns, int gap) {
    CHECK(native_window_layout_add(l, id, parent, kind, axis, columns, gap));
}

static void leaf(native_window_layout_t *l, uint64_t id, uint64_t parent) {
    add(l, id, parent, NATIVE_WINDOW_LAYOUT_LEAF,
        NATIVE_WINDOW_LAYOUT_HORIZONTAL, 0, 0);
}

static void rejected_add(native_window_layout_t *l, uint64_t id, uint64_t parent,
                         native_window_layout_kind_t kind,
                         native_window_layout_axis_t axis, int columns, int gap) {
    unsigned char before[sizeof(*l)];
    memcpy(before, l, sizeof(*l));
    CHECK(!native_window_layout_add(l, id, parent, kind, axis, columns, gap));
    CHECK(memcmp(before, l, sizeof(*l)) == 0);
}

static void rejected_compute(native_window_layout_t *l,
                             native_ui_rect_t frame, native_ui_rect_t viewport) {
    unsigned char before[sizeof(*l)];
    memcpy(before, l, sizeof(*l));
    CHECK(!native_window_layout_compute(l, frame, viewport));
    CHECK(memcmp(before, l, sizeof(*l)) == 0);
}

static void test_init_and_add(void) {
    native_window_layout_t l;
    native_window_layout_init(NULL);
    CHECK(native_window_layout_find(NULL, 1) == NULL);
    CHECK(!native_window_layout_add(NULL, 1, 0, NATIVE_WINDOW_LAYOUT_LEAF,
                                   NATIVE_WINDOW_LAYOUT_HORIZONTAL, 0, 0));
    CHECK(!native_window_layout_compute(NULL, rect(0, 0, 1, 1), rect(0, 0, 1, 1)));
    memset(&l, 0xa5, sizeof(l));
    native_window_layout_init(&l);
    CHECK(l.count == 0);
    CHECK(native_window_layout_find(&l, 1) == NULL);
    CHECK(native_window_layout_compute(&l, rect(0, 0, 0, 0), rect(0, 0, 0, 0)));
    rejected_add(&l, 0, 0, NATIVE_WINDOW_LAYOUT_LEAF, NATIVE_WINDOW_LAYOUT_HORIZONTAL, 0, 0);
    rejected_add(&l, 1, 9, NATIVE_WINDOW_LAYOUT_LEAF, NATIVE_WINDOW_LAYOUT_HORIZONTAL, 0, 0);
    leaf(&l, 400, 0);
    CHECK(native_window_layout_find(&l, 400) == &l.nodes[0]);
    CHECK(native_window_layout_find(&l, 0) == NULL);
    CHECK(native_window_layout_find(&l, 1) == NULL);
    rejected_add(&l, 401, 400, NATIVE_WINDOW_LAYOUT_LEAF, NATIVE_WINDOW_LAYOUT_HORIZONTAL, 0, 0);
    rejected_add(&l, 402, 0, NATIVE_WINDOW_LAYOUT_LEAF, NATIVE_WINDOW_LAYOUT_HORIZONTAL, 0, 0);
    CHECK(native_window_layout_compute(&l, rect(-20, -10, 50, 40), rect(0, 0, 12, 8)));
    same_rect(l.nodes[0].rect, rect(-20, -10, 50, 40));
    same_rect(l.nodes[0].clip, rect(0, 0, 12, 8));

    native_window_layout_init(&l);
    add(&l, NATIVE_WINDOW_LAYOUT_ROOT_ID, 0, NATIVE_WINDOW_LAYOUT_SPLIT,
        NATIVE_WINDOW_LAYOUT_HORIZONTAL, 0, NATIVE_WINDOW_LAYOUT_MAX_GAP);
    rejected_add(&l, NATIVE_WINDOW_LAYOUT_ROOT_ID, NATIVE_WINDOW_LAYOUT_ROOT_ID,
                 NATIVE_WINDOW_LAYOUT_LEAF, NATIVE_WINDOW_LAYOUT_HORIZONTAL, 0, 0);
    rejected_add(&l, 1, 999, NATIVE_WINDOW_LAYOUT_LEAF, NATIVE_WINDOW_LAYOUT_HORIZONTAL, 0, 0);
    rejected_add(&l, 1, 1, NATIVE_WINDOW_LAYOUT_SPLIT, NATIVE_WINDOW_LAYOUT_HORIZONTAL, 0, 0);
    for (int i = 1; i < NATIVE_WINDOW_LAYOUT_MAX_NODES; i++)
        leaf(&l, (uint64_t)i, NATIVE_WINDOW_LAYOUT_ROOT_ID);
    CHECK(l.count == NATIVE_WINDOW_LAYOUT_MAX_NODES);
    rejected_add(&l, 100, NATIVE_WINDOW_LAYOUT_ROOT_ID,
                 NATIVE_WINDOW_LAYOUT_LEAF, NATIVE_WINDOW_LAYOUT_HORIZONTAL, 0, 0);
    CHECK(native_window_layout_compute(&l, rect(0, 0, 1, 1), rect(0, 0, 1, 1)));
    for (int i = 1; i < l.count; i++) {
        CHECK(l.nodes[i].rect.width == 0 && l.nodes[i].rect.height == 0);
        CHECK(l.nodes[i].rect.x >= 0 && l.nodes[i].rect.x <= 1);
    }
}

static void test_nested_geometry(void) {
    native_window_layout_t l;
    native_window_layout_init(&l);
    add(&l, 90, 0, NATIVE_WINDOW_LAYOUT_SPLIT, NATIVE_WINDOW_LAYOUT_HORIZONTAL, 0, 3);
    add(&l, 8, 90, NATIVE_WINDOW_LAYOUT_GRID, NATIVE_WINDOW_LAYOUT_VERTICAL, 2, 1);
    leaf(&l, 71, 8);
    /* A later sibling follows an earlier sibling's descendant in storage. */
    add(&l, 4, 90, NATIVE_WINDOW_LAYOUT_SPLIT, NATIVE_WINDOW_LAYOUT_VERTICAL, 0, 0);
    leaf(&l, 3, 8);
    leaf(&l, 60, 4);
    leaf(&l, 20, 8);
    leaf(&l, 1, 4);
    CHECK(native_window_layout_compute(&l, rect(10, 20, 103, 63), rect(30, 30, 65, 40)));
    same_rect(l.nodes[0].rect, rect(10, 20, 103, 63));
    same_rect(l.nodes[1].rect, rect(11, 21, 48, 60));
    same_rect(l.nodes[3].rect, rect(62, 21, 49, 60));
    same_rect(l.nodes[2].rect, rect(11, 21, 23, 29));
    same_rect(l.nodes[4].rect, rect(35, 21, 23, 29));
    same_rect(l.nodes[6].rect, rect(11, 51, 23, 29));
    same_rect(l.nodes[5].rect, rect(62, 21, 49, 30));
    same_rect(l.nodes[7].rect, rect(62, 51, 49, 30));
    same_rect(l.nodes[2].clip, rect(30, 30, 4, 20));
    same_rect(l.nodes[6].clip, rect(30, 51, 4, 19));
    same_rect(l.nodes[7].clip, rect(62, 51, 33, 19));
    for (int i = 0; i < l.count; i++)
        CHECK(native_window_layout_find(&l, l.nodes[i].id) == &l.nodes[i]);
    unsigned char before[sizeof(l)];
    memcpy(before, &l, sizeof(l));
    CHECK(native_window_layout_compute(&l, rect(10, 20, 103, 63), rect(30, 30, 65, 40)));
    CHECK(memcmp(before, &l, sizeof(l)) == 0);
    CHECK(native_window_layout_compute(&l, rect(10, 20, 103, 63), rect(500, 500, 1, 1)));
    for (int i = 0; i < l.count; i++)
        CHECK(l.nodes[i].clip.width == 0 && l.nodes[i].clip.height == 0);
}

static void test_grids_and_depth(void) {
    native_window_layout_t l;
    native_window_layout_init(&l);
    add(&l, 1, 0, NATIVE_WINDOW_LAYOUT_GRID, NATIVE_WINDOW_LAYOUT_HORIZONTAL, 0, 0);
    for (uint64_t i = 2; i <= 6; i++) leaf(&l, i, 1);
    CHECK(native_window_layout_compute(&l, rect(-5, -7, 11, 7), rect(-5, -7, 11, 7)));
    same_rect(l.nodes[1].rect, rect(-5, -7, 3, 3));
    same_rect(l.nodes[2].rect, rect(-2, -7, 4, 3));
    same_rect(l.nodes[3].rect, rect(2, -7, 4, 3));
    same_rect(l.nodes[4].rect, rect(-5, -4, 3, 4));
    same_rect(l.nodes[5].rect, rect(-2, -4, 4, 4));
    l.nodes[0].columns = NATIVE_WINDOW_LAYOUT_MAX_NODES;
    CHECK(native_window_layout_compute(&l, rect(0, 0, 320, 8), rect(0, 0, 320, 8)));
    same_rect(l.nodes[5].rect, rect(40, 0, 10, 8));

    native_window_layout_init(&l);
    for (int i = 0; i < NATIVE_WINDOW_LAYOUT_MAX_NODES; i++)
        add(&l, (uint64_t)i + 1, (uint64_t)i,
            i == NATIVE_WINDOW_LAYOUT_MAX_NODES - 1 ? NATIVE_WINDOW_LAYOUT_LEAF :
            (i % 2 ? NATIVE_WINDOW_LAYOUT_GRID : NATIVE_WINDOW_LAYOUT_SPLIT),
            i % 2 ? NATIVE_WINDOW_LAYOUT_VERTICAL : NATIVE_WINDOW_LAYOUT_HORIZONTAL,
            0, 0);
    CHECK(native_window_layout_compute(&l, rect(INT_MIN, -100, INT_MAX, 300), rect(-50, -5, 20, 10)));
    for (int i = 0; i < l.count; i++) {
        same_rect(l.nodes[i].rect, rect(INT_MIN, -100, INT_MAX, 300));
        same_rect(l.nodes[i].clip, rect(-50, -5, 20, 10));
    }
}

static void test_validation(void) {
    native_window_layout_t base, bad;
    native_ui_rect_t frame = rect(0, 0, 100, 100);
    native_window_layout_init(&base);
    add(&base, 1, 0, NATIVE_WINDOW_LAYOUT_SPLIT, NATIVE_WINDOW_LAYOUT_HORIZONTAL, 0, 0);
    add(&base, 2, 1, NATIVE_WINDOW_LAYOUT_GRID, NATIVE_WINDOW_LAYOUT_VERTICAL, 2, 0);
    leaf(&base, 3, 2);
    CHECK(native_window_layout_compute(&base, frame, frame));
    /* Test both mutators against malformed public storage, byte-for-byte. */
#define BAD(change) do { \
    memcpy(&bad, &base, sizeof(bad)); \
    change; \
    rejected_compute(&bad, frame, frame); \
    rejected_add(&bad, 50, 1, NATIVE_WINDOW_LAYOUT_LEAF, NATIVE_WINDOW_LAYOUT_HORIZONTAL, 0, 0); \
} while (0)
    BAD(bad.count = -1);
    CHECK(native_window_layout_find(&bad, 1) == NULL);
    BAD(bad.count = NATIVE_WINDOW_LAYOUT_MAX_NODES + 1);
    CHECK(native_window_layout_find(&bad, 1) == NULL);
    BAD(bad.count = INT_MAX);
    BAD(bad.nodes[2].id = 0);
    BAD(bad.nodes[2].id = 1);
    BAD(bad.nodes[0].parent_id = 2);
    BAD(bad.nodes[1].parent_id = 0);
    BAD(bad.nodes[1].parent_id = 99);
    BAD(bad.nodes[1].parent_id = 2);
    BAD(bad.nodes[1].parent_id = 3);
    BAD(bad.nodes[0].kind = NATIVE_WINDOW_LAYOUT_LEAF);
    BAD(bad.nodes[2].kind = (native_window_layout_kind_t)-1);
    BAD(bad.nodes[2].kind = (native_window_layout_kind_t)3);
    BAD(bad.nodes[2].axis = (native_window_layout_axis_t)-1);
    BAD(bad.nodes[2].axis = (native_window_layout_axis_t)2);
    BAD(bad.nodes[1].columns = -1);
    BAD(bad.nodes[1].columns = NATIVE_WINDOW_LAYOUT_MAX_NODES + 1);
    BAD(bad.nodes[1].columns = INT_MAX);
    BAD(bad.nodes[0].columns = 1);
    BAD(bad.nodes[2].columns = 1);
    BAD(bad.nodes[1].gap = -1);
    BAD(bad.nodes[1].gap = NATIVE_WINDOW_LAYOUT_MAX_GAP + 1);
    BAD(bad.nodes[1].gap = INT_MAX);
    BAD(bad.nodes[2].gap = 1);
    BAD(bad.nodes[2].rect.width = -1);
    BAD(bad.nodes[2].clip.height = -1);
    BAD(bad.nodes[2].rect = rect(INT_MAX, 0, 1, 1));
    BAD(bad.nodes[2].clip = rect(0, INT_MAX, 1, 1));
#undef BAD
    const native_ui_rect_t invalid[] = {
        {0, 0, -1, 1}, {0, 0, 1, -1}, {INT_MAX, 0, 1, 1},
        {0, INT_MAX, 1, 1}, {1, 0, INT_MAX, 1}, {0, 1, 1, INT_MAX}
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        rejected_compute(&base, invalid[i], frame);
        rejected_compute(&base, frame, invalid[i]);
    }
    for (int k = -1; k <= 3; k += 4)
        rejected_add(&base, 5, 1, (native_window_layout_kind_t)k, NATIVE_WINDOW_LAYOUT_HORIZONTAL, 0, 0);
    rejected_add(&base, 5, 1, NATIVE_WINDOW_LAYOUT_GRID, (native_window_layout_axis_t)2, 0, 0);
    rejected_add(&base, 5, 1, NATIVE_WINDOW_LAYOUT_GRID, NATIVE_WINDOW_LAYOUT_HORIZONTAL, INT_MAX, 0);
    rejected_add(&base, 5, 1, NATIVE_WINDOW_LAYOUT_GRID, NATIVE_WINDOW_LAYOUT_HORIZONTAL, 0, INT_MAX);
    CHECK(native_window_layout_compute(&base, rect(INT_MAX, INT_MAX, 0, 0),
                                       rect(INT_MIN, INT_MIN, INT_MAX, INT_MAX)));
    CHECK(native_window_layout_compute(&base, rect(INT_MIN, INT_MIN, INT_MAX, INT_MAX),
                                       rect(INT_MAX, INT_MAX, 0, 0)));
}

static uint32_t seed = UINT32_C(0x39812abc);
static unsigned random_below(unsigned n) {
    seed = seed * UINT32_C(1664525) + UINT32_C(1013904223);
    return seed % n;
}

/* Use wider endpoints and all ancestors, independently of the implementation's
 * immediate-parent clip recurrence. Every generated nonempty clip must agree. */
static void verify_geometry(const native_window_layout_t *l, native_ui_rect_t viewport) {
    for (int i = 0; i < l->count; i++) {
        const native_window_layout_node_t *n = &l->nodes[i];
        CHECK(n->rect.width >= 0 && n->rect.height >= 0);
        int64_t left = n->rect.x, top = n->rect.y;
        int64_t right = left + n->rect.width, bottom = top + n->rect.height;
        CHECK(right <= INT_MAX && bottom <= INT_MAX);
        if (i) {
            const native_window_layout_node_t *p = native_window_layout_find(l, n->parent_id);
            CHECK(p != NULL);
            CHECK(left >= p->rect.x && top >= p->rect.y);
            CHECK(right <= (int64_t)p->rect.x + p->rect.width);
            CHECK(bottom <= (int64_t)p->rect.y + p->rect.height);
        }
        const native_window_layout_node_t *a = n;
        do {
            if (a->rect.x > left) left = a->rect.x;
            if (a->rect.y > top) top = a->rect.y;
            if ((int64_t)a->rect.x + a->rect.width < right) right = (int64_t)a->rect.x + a->rect.width;
            if ((int64_t)a->rect.y + a->rect.height < bottom) bottom = (int64_t)a->rect.y + a->rect.height;
            a = native_window_layout_find(l, a->parent_id);
        } while (a);
        if (viewport.x > left) left = viewport.x;
        if (viewport.y > top) top = viewport.y;
        if ((int64_t)viewport.x + viewport.width < right) right = (int64_t)viewport.x + viewport.width;
        if ((int64_t)viewport.y + viewport.height < bottom) bottom = (int64_t)viewport.y + viewport.height;
        same_rect(n->clip, rect((int)left, (int)top,
                               right > left ? (int)(right - left) : 0,
                               bottom > top ? (int)(bottom - top) : 0));
    }
}

static void test_generated_trees(void) {
    const native_ui_rect_t frames[] = {
        {0, 0, 0, 0}, {0, 0, 1, 1}, {-4, -8, 2, 3}, {-100, -100, 307, 109},
        {INT_MIN, INT_MIN, INT_MAX, INT_MAX}, {0, 0, INT_MAX, INT_MAX},
        {INT_MAX - 15, INT_MAX - 16, 15, 16}, {INT_MAX, INT_MAX, 0, 0}
    };
    const native_ui_rect_t viewports[] = {
        {-10, -20, 40, 60}, {INT_MIN, INT_MIN, INT_MAX, INT_MAX},
        {INT_MAX, INT_MAX, 0, 0}, {0, 0, INT_MAX, INT_MAX}, {0, 0, 0, 0}
    };
    for (int iteration = 0; iteration < 200; iteration++) {
        native_window_layout_t l;
        native_window_layout_init(&l);
        int count = 1 + (int)random_below(NATIVE_WINDOW_LAYOUT_MAX_NODES);
        for (int i = 0; i < count; i++) {
            int parent = 0;
            if (i) do { parent = (int)random_below((unsigned)i); }
                   while (l.nodes[parent].kind == NATIVE_WINDOW_LAYOUT_LEAF);
            native_window_layout_kind_t kind = i ? (native_window_layout_kind_t)random_below(3) :
                (random_below(2) ? NATIVE_WINDOW_LAYOUT_GRID : NATIVE_WINDOW_LAYOUT_SPLIT);
            add(&l, (uint64_t)i + 100, i ? l.nodes[parent].id : 0, kind,
                (native_window_layout_axis_t)random_below(2),
                kind == NATIVE_WINDOW_LAYOUT_GRID ? (int)random_below(NATIVE_WINDOW_LAYOUT_MAX_NODES + 1) : 0,
                kind == NATIVE_WINDOW_LAYOUT_LEAF ? 0 : (int)random_below(NATIVE_WINDOW_LAYOUT_MAX_GAP + 1));
        }
        for (size_t f = 0; f < sizeof(frames) / sizeof(frames[0]); f++) {
            for (size_t v = 0; v < sizeof(viewports) / sizeof(viewports[0]); v++) {
                CHECK(native_window_layout_compute(&l, frames[f], viewports[v]));
                same_rect(l.nodes[0].rect, frames[f]);
                verify_geometry(&l, viewports[v]);
            }
        }
    }
}

int main(void) {
    test_init_and_add();
    test_nested_geometry();
    test_grids_and_depth();
    test_validation();
    test_generated_trees();
    printf("native_window_layout: %lu checks passed (including 8000 generated layouts)\n", checks);
    return 0;
}
