/* Fault injection: a reactor failure cannot manufacture successful reap. */
#include "swarm_lifecycle.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

static int term_calls, force_calls, poll_calls;
bool swarm_active_test(const swarm_t *s, int id) {
    return id >= 0 && id < s->child_count && (s->active.words[id / 64] & (1ULL << (id % 64)));
}
swarm_child_t *swarm_get(swarm_t *s, int id) {
    return id >= 0 && id < s->child_count ? &s->children[id] : NULL;
}
bool swarm_kill(swarm_t *s, int id) {
    if (s->children[id].cancel_requested) return false;
    ++term_calls; s->children[id].cancel_requested = true;
    return true;
}
bool swarm_force_kill(swarm_t *s, int id) {
    ++force_calls; s->children[id].cancel_forced = true;
    return true;
}
int swarm_poll_stream(swarm_t *s, int ms, swarm_stream_cb cb, void *ctx) {
    (void)s; (void)ms; (void)cb; (void)ctx;
    ++poll_calls; errno = EIO; return -1;
}

int main(void) {
    swarm_t *s = calloc(1, sizeof(*s)); assert(s);
    s->child_count = s->group_count = 1;
    s->active.words[0] = 1; s->active.count = 1;
    s->groups[0].child_count = 1; s->groups[0].child_ids[0] = 0;
    swarm_cancel_report_t r = swarm_cancel_group(s, 0, 500, NULL, NULL, NULL, NULL);
    assert(r.requested == 1 && r.forced == 1 && r.remaining == 1 && r.poll_error == EIO);
    assert(term_calls == 1 && force_calls == 1 && poll_calls == 2 && !s->children[0].reaped);
    jbuf_t b; jbuf_init(&b, 512); jbuf_append(&b, "{\"id\":0");
    swarm_group_lifecycle_append(&b, s, 0); jbuf_append(&b, "}");
    char *lifecycle = json_get_raw(b.data, "lifecycle"); assert(lifecycle);
    assert(!json_get_bool(lifecycle, "shutdown_complete", true));
    assert(json_get_int(lifecycle, "live_processes", 0) == 1);
    free(lifecycle); jbuf_free(&b);
    r = swarm_cancel_child(s, 99, 0, NULL, NULL, NULL, NULL);
    assert(r.poll_error == EINVAL && term_calls == 1 && force_calls == 1);
    free(s);
    puts("PASS: reactor failure escalates once, reports pending cleanup, and preserves unreaped state");
    return 0;
}
