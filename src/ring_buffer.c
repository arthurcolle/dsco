#include "ring_buffer.h"
#include <string.h>

void ring_init(metric_ring_t *r) {
    memset(r, 0, sizeof(*r));
}

void ring_push(metric_ring_t *r, const metric_sample_t *s) {
    size_t idx = (r->head + r->count) % RING_CAPACITY;
    r->samples[idx] = *s;
    if (r->count < RING_CAPACITY) {
        r->count++;
    } else {
        r->head = (r->head + 1) % RING_CAPACITY;
    }
}

size_t ring_count(const metric_ring_t *r) {
    return r->count;
}

const metric_sample_t *ring_get(const metric_ring_t *r, size_t idx) {
    if (idx >= r->count) return NULL;
    size_t actual = (r->head + idx) % RING_CAPACITY;
    return &r->samples[actual];
}
