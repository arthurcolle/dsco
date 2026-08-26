#include <sys/types.h>
#ifndef DSCO_RING_BUFFER_H
#define DSCO_RING_BUFFER_H

#include <stdint.h>
#include <stddef.h>

#define RING_CAPACITY 3600  /* 1 hour at 1Hz, or 5 min at 12Hz */

typedef struct {
    uint64_t ts;
    pid_t child;
    uint64_t rss;
    uint64_t peak_rss;
    int pressure;
} metric_sample_t;

typedef struct {
    metric_sample_t samples[RING_CAPACITY];
    size_t head;
    size_t count;
} metric_ring_t;

void ring_init(metric_ring_t *r);
void ring_push(metric_ring_t *r, const metric_sample_t *s);
size_t ring_count(const metric_ring_t *r);
const metric_sample_t *ring_get(const metric_ring_t *r, size_t idx);

#endif
