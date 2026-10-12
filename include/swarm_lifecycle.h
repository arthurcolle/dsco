#ifndef DSCO_SWARM_LIFECYCLE_H
#define DSCO_SWARM_LIFECYCLE_H

#include "swarm.h"
#include "json_util.h"

typedef struct {
    int requested;
    int forced;
    int remaining;
    int poll_error;
    double elapsed_ms;
} swarm_cancel_report_t;

typedef void (*swarm_cancel_event_cb)(const swarm_child_t *child, bool forced, void *ctx);

/* Internal primitives: callers must hold the swarm progress guard and enter
 * through the governed tool path. Neither function grants authority. */
swarm_cancel_report_t swarm_cancel_child(swarm_t *s, int child_id, int grace_ms,
    swarm_stream_cb stream, void *stream_ctx, swarm_cancel_event_cb event, void *event_ctx);
swarm_cancel_report_t swarm_cancel_group(swarm_t *s, int group_id, int grace_ms,
    swarm_stream_cb stream, void *stream_ctx, swarm_cancel_event_cb event, void *event_ctx);

/* Persist an immutable run receipt and a compatible child-slot alias. */
bool swarm_result_persist(swarm_child_t *child);
void swarm_child_lifecycle_append(jbuf_t *b, const swarm_child_t *child);
void swarm_group_lifecycle_append(jbuf_t *b, const swarm_t *s, int group_id);

#endif
