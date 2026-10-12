#include "native_followup_queue.h"

#include <assert.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static native_followup_queue_t queue;
static native_followup_snapshot_t snapshot;

static void test_fifo_and_capacity(void) {
    assert(native_followup_queue_init(&queue));
    char text[32], out[32];
    uint64_t ids[NATIVE_FOLLOWUP_QUEUE_CAP], id = 0;
    assert(native_followup_queue_pop(&queue, out, sizeof(out), NULL) == NATIVE_FOLLOWUP_EMPTY);
    for (int i = 0; i < NATIVE_FOLLOWUP_QUEUE_CAP; ++i) {
        snprintf(text, sizeof(text), "item-%d", i);
        assert(native_followup_queue_push(&queue, text, &ids[i]) == NATIVE_FOLLOWUP_OK);
        assert(ids[i] > (i ? ids[i - 1] : 0));
    }
    assert(native_followup_queue_push(&queue, "overflow", &id) == NATIVE_FOLLOWUP_FULL);
    assert(id == 0);
    assert(native_followup_queue_snapshot(&queue, &snapshot) == NATIVE_FOLLOWUP_OK);
    assert(snapshot.count == NATIVE_FOLLOWUP_QUEUE_CAP);
    uint64_t revision = snapshot.revision;
    assert(native_followup_queue_pop(&queue, out, 2, NULL) == NATIVE_FOLLOWUP_BUFFER_SMALL);
    assert(native_followup_queue_snapshot(&queue, &snapshot) == NATIVE_FOLLOWUP_OK);
    assert(snapshot.revision == revision);
    for (int i = 0; i < NATIVE_FOLLOWUP_QUEUE_CAP; ++i) {
        snprintf(text, sizeof(text), "item-%d", i);
        assert(native_followup_queue_pop(&queue, out, sizeof(out), &id) == NATIVE_FOLLOWUP_OK);
        assert(id == ids[i] && strcmp(text, out) == 0);
    }
    assert(native_followup_queue_push(&queue, "refill", &id) == NATIVE_FOLLOWUP_OK);
    assert(id > ids[NATIVE_FOLLOWUP_QUEUE_CAP - 1]);
    assert(native_followup_queue_pop(&queue, out, sizeof(out), NULL) == NATIVE_FOLLOWUP_OK);
    assert(strcmp(out, "refill") == 0);
    native_followup_queue_destroy(&queue);
}

static void test_edit_delete_and_owned_snapshot(void) {
    assert(native_followup_queue_init(&queue));
    uint64_t a, b, c;
    assert(native_followup_queue_push(&queue, "first", &a) == NATIVE_FOLLOWUP_OK);
    assert(native_followup_queue_push(&queue, "middle", &b) == NATIVE_FOLLOWUP_OK);
    assert(native_followup_queue_push(&queue, "last", &c) == NATIVE_FOLLOWUP_OK);
    assert(native_followup_queue_snapshot(&queue, &snapshot) == NATIVE_FOLLOWUP_OK);
    native_followup_action_t action = {
        .kind = NATIVE_FOLLOWUP_ACTION_REPLACE,
        .id = b,
        .expected_revision = snapshot.entries[1].revision,
        .text = "edited \342\230\203\nsecond line"
    };
    assert(native_followup_queue_apply(&queue, &action) == NATIVE_FOLLOWUP_OK);
    assert(strcmp(snapshot.entries[1].text, "middle") == 0); /* owned, not alias */
    assert(native_followup_queue_apply(&queue, &action) == NATIVE_FOLLOWUP_CONFLICT);
    assert(native_followup_queue_remove(&queue, b, action.expected_revision) == NATIVE_FOLLOWUP_CONFLICT);
    assert(native_followup_queue_remove(&queue, a, snapshot.entries[0].revision) == NATIVE_FOLLOWUP_OK);
    assert(native_followup_queue_snapshot(&queue, &snapshot) == NATIVE_FOLLOWUP_OK);
    assert(snapshot.count == 2 && snapshot.entries[0].id == b && snapshot.entries[1].id == c);
    assert(strcmp(snapshot.entries[0].text, action.text) == 0);
    assert(snapshot.entries[0].revision > action.expected_revision);
    action.kind = NATIVE_FOLLOWUP_ACTION_REMOVE;
    action.expected_revision = snapshot.entries[0].revision;
    assert(native_followup_queue_apply(&queue, &action) == NATIVE_FOLLOWUP_OK);
    assert(native_followup_queue_apply(&queue, &action) == NATIVE_FOLLOWUP_NOT_FOUND);
    char out[32];
    uint64_t id;
    assert(native_followup_queue_pop(&queue, out, sizeof(out), &id) == NATIVE_FOLLOWUP_OK);
    assert(id == c && strcmp(out, "last") == 0);
    assert(native_followup_queue_replace(&queue, c, 0, "late") == NATIVE_FOLLOWUP_NOT_FOUND);
    assert(native_followup_queue_push(&queue, "again", &id) == NATIVE_FOLLOWUP_OK);
    assert(id > c);
    assert(native_followup_queue_remove(&queue, b, 0) == NATIVE_FOLLOWUP_NOT_FOUND);
    assert(native_followup_queue_replace(&queue, id, 0, "unconditional") == NATIVE_FOLLOWUP_OK);
    assert(native_followup_queue_remove(&queue, id, 0) == NATIVE_FOLLOWUP_OK);
    assert(native_followup_queue_snapshot(&queue, &snapshot) == NATIVE_FOLLOWUP_OK);
    assert(snapshot.count == 0 && snapshot.entries[0].id == 0 && snapshot.entries[0].text[0] == 0);
    native_followup_queue_destroy(&queue);
}

static void test_validation_and_limits(void) {
    assert(!native_followup_queue_init(NULL));
    assert(native_followup_queue_init(&queue));
    char *text = malloc(NATIVE_FOLLOWUP_TEXT_CAP + 1);
    char *out = malloc(NATIVE_FOLLOWUP_TEXT_CAP);
    assert(text && out);
    assert(native_followup_queue_push(NULL, "x", NULL) == NATIVE_FOLLOWUP_INVALID);
    assert(native_followup_queue_push(&queue, NULL, NULL) == NATIVE_FOLLOWUP_INVALID);
    assert(native_followup_queue_push(&queue, "", NULL) == NATIVE_FOLLOWUP_INVALID);
    assert(native_followup_queue_pop(&queue, NULL, 1, NULL) == NATIVE_FOLLOWUP_INVALID);
    assert(native_followup_queue_pop(&queue, out, 0, NULL) == NATIVE_FOLLOWUP_INVALID);
    assert(native_followup_queue_snapshot(&queue, NULL) == NATIVE_FOLLOWUP_INVALID);
    assert(native_followup_queue_replace(&queue, 0, 0, "x") == NATIVE_FOLLOWUP_INVALID);
    assert(native_followup_queue_remove(&queue, 0, 0) == NATIVE_FOLLOWUP_INVALID);
    assert(native_followup_queue_apply(&queue, NULL) == NATIVE_FOLLOWUP_INVALID);
    native_followup_action_t action = {.kind = 99, .id = 1};
    assert(native_followup_queue_apply(&queue, &action) == NATIVE_FOLLOWUP_INVALID);
    memset(text, 'x', NATIVE_FOLLOWUP_TEXT_CAP);
    text[NATIVE_FOLLOWUP_TEXT_CAP] = 0;
    assert(native_followup_queue_push(&queue, text, NULL) == NATIVE_FOLLOWUP_TOO_LONG);
    text[NATIVE_FOLLOWUP_TEXT_CAP - 1] = 0;
    uint64_t id;
    assert(native_followup_queue_push(&queue, text, &id) == NATIVE_FOLLOWUP_OK);
    memset(action.text, 'x', sizeof(action.text)); /* bounded unterminated action */
    action.kind = NATIVE_FOLLOWUP_ACTION_REPLACE;
    action.id = id;
    assert(native_followup_queue_apply(&queue, &action) == NATIVE_FOLLOWUP_TOO_LONG);
    assert(native_followup_queue_pop(&queue, out, NATIVE_FOLLOWUP_TEXT_CAP, NULL) == NATIVE_FOLLOWUP_OK);
    assert(strcmp(text, out) == 0);
    free(text);
    free(out);
    /* White-box exhaustion checks: no live threads; never reuse an ID. */
    queue.next_id = UINT64_MAX;
    assert(native_followup_queue_push(&queue, "last id", &id) == NATIVE_FOLLOWUP_OK);
    assert(id == UINT64_MAX);
    assert(native_followup_queue_push(&queue, "wrap", NULL) == NATIVE_FOLLOWUP_EXHAUSTED);
    queue.state.revision = UINT64_MAX;
    assert(native_followup_queue_replace(&queue, id, 0, "wrap") == NATIVE_FOLLOWUP_EXHAUSTED);
    assert(native_followup_queue_remove(&queue, id, 0) == NATIVE_FOLLOWUP_EXHAUSTED);
    char buf[32];
    assert(native_followup_queue_pop(&queue, buf, sizeof(buf), NULL) == NATIVE_FOLLOWUP_EXHAUSTED);
    native_followup_queue_destroy(&queue);
}

#define PRODUCERS 4
#define PER_PRODUCER 300
static atomic_int producers_done;
static atomic_bool consumer_done;
static atomic_int seen[PRODUCERS * PER_PRODUCER];
static atomic_int edits;
static atomic_int deletes;

static void record_consumed(const char *text) {
    int index = -1;
    assert(sscanf(text, "message-%d", &index) == 1);
    assert(index >= 0 && index < PRODUCERS * PER_PRODUCER);
    assert(atomic_fetch_add(&seen[index], 1) == 0);
}

static void *producer(void *arg) {
    int start = *(int *)arg * PER_PRODUCER;
    uint64_t last_id = 0;
    for (int i = 0; i < PER_PRODUCER; ++i) {
        char text[32];
        snprintf(text, sizeof(text), "message-%d", start + i);
        uint64_t id = 0;
        native_followup_result_t result;
        do {
            result = native_followup_queue_push(&queue, text, &id);
            if (result == NATIVE_FOLLOWUP_FULL) sched_yield();
        } while (result == NATIVE_FOLLOWUP_FULL);
        assert(result == NATIVE_FOLLOWUP_OK && id > last_id);
        last_id = id;
    }
    atomic_fetch_add(&producers_done, 1);
    return NULL;
}

static void *consumer(void *unused) {
    (void)unused;
    uint64_t last_id = 0;
    for (;;) {
        /* Check producers before pop so a final push cannot escape draining. */
        bool finished = atomic_load(&producers_done) == PRODUCERS;
        char text[32];
        uint64_t id;
        native_followup_result_t result = native_followup_queue_pop(&queue, text, sizeof(text), &id);
        if (result == NATIVE_FOLLOWUP_EMPTY) {
            if (finished) break;
            sched_yield();
        } else {
            assert(result == NATIVE_FOLLOWUP_OK && id > last_id);
            last_id = id;
            record_consumed(text);
        }
    }
    atomic_store(&consumer_done, true);
    return NULL;
}

static void *editor(void *unused) {
    (void)unused;
    native_followup_snapshot_t *view = malloc(sizeof(*view));
    assert(view);
    uint64_t revision = 0;
    unsigned iteration = 0;
    do {
        assert(native_followup_queue_snapshot(&queue, view) == NATIVE_FOLLOWUP_OK);
        assert(view->count <= NATIVE_FOLLOWUP_QUEUE_CAP && view->revision >= revision);
        revision = view->revision;
        for (size_t i = 0; i < view->count; ++i) {
            assert(view->entries[i].id && view->entries[i].revision <= revision);
            if (i) assert(view->entries[i - 1].id < view->entries[i].id);
        }
        if (view->count) {
            native_followup_entry_t *entry = &view->entries[view->count - 1];
            bool remove = ++iteration % 2 == 0;
            native_followup_result_t result = remove
                ? native_followup_queue_remove(&queue, entry->id, entry->revision)
                : native_followup_queue_replace(&queue, entry->id, entry->revision, entry->text);
            assert(result == NATIVE_FOLLOWUP_OK || result == NATIVE_FOLLOWUP_NOT_FOUND ||
                   result == NATIVE_FOLLOWUP_CONFLICT);
            if (result == NATIVE_FOLLOWUP_OK) {
                if (remove) {
                    record_consumed(entry->text);
                    atomic_fetch_add(&deletes, 1);
                } else atomic_fetch_add(&edits, 1);
            }
        }
        sched_yield();
    } while (!atomic_load(&consumer_done));
    free(view);
    return NULL;
}

static void test_concurrency(void) {
    assert(native_followup_queue_init(&queue));
    pthread_t producers[PRODUCERS], consumer_thread, editor_thread;
    int args[PRODUCERS];
    assert(pthread_create(&editor_thread, NULL, editor, NULL) == 0);
    for (int i = 0; i < PRODUCERS; ++i) {
        args[i] = i;
        assert(pthread_create(&producers[i], NULL, producer, &args[i]) == 0);
    }
    assert(pthread_create(&consumer_thread, NULL, consumer, NULL) == 0);
    for (int i = 0; i < PRODUCERS; ++i) assert(pthread_join(producers[i], NULL) == 0);
    assert(pthread_join(consumer_thread, NULL) == 0);
    assert(pthread_join(editor_thread, NULL) == 0);
    for (int i = 0; i < PRODUCERS * PER_PRODUCER; ++i) assert(atomic_load(&seen[i]) == 1);
    assert(native_followup_queue_snapshot(&queue, &snapshot) == NATIVE_FOLLOWUP_OK);
    assert(snapshot.count == 0);
    printf("concurrency: %d messages accounted once, %d edits, %d deletes\n",
           PRODUCERS * PER_PRODUCER, atomic_load(&edits), atomic_load(&deletes));
    native_followup_queue_destroy(&queue);
}

int main(void) {
    test_fifo_and_capacity();
    test_edit_delete_and_owned_snapshot();
    test_validation_and_limits();
    test_concurrency();
    puts("native_followup_queue: FIFO, capacity, stable IDs, revisions, ownership, validation, concurrency PASS");
    return 0;
}
