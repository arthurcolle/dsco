#ifndef DSCO_NATIVE_FOLLOWUP_QUEUE_H
#define DSCO_NATIVE_FOLLOWUP_QUEUE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <pthread.h>

#define NATIVE_FOLLOWUP_QUEUE_CAP 8
#define NATIVE_FOLLOWUP_TEXT_CAP 65536

typedef struct {
    uint64_t id;
    uint64_t revision;
    char text[NATIVE_FOLLOWUP_TEXT_CAP];
} native_followup_entry_t;

/* Owned copy; ~512 KiB. Allocate off small composer-thread stacks. */
typedef struct {
    uint64_t revision;
    size_t count;
    native_followup_entry_t entries[NATIVE_FOLLOWUP_QUEUE_CAP];
} native_followup_snapshot_t;

typedef enum {
    NATIVE_FOLLOWUP_ACTION_REPLACE = 1,
    NATIVE_FOLLOWUP_ACTION_REMOVE = 2
} native_followup_action_kind_t;

typedef struct {
    native_followup_action_kind_t kind;
    uint64_t id;
    uint64_t expected_revision; /* Entry revision; zero means unconditional. */
    char text[NATIVE_FOLLOWUP_TEXT_CAP];
} native_followup_action_t;

typedef enum {
    NATIVE_FOLLOWUP_OK = 0,
    NATIVE_FOLLOWUP_EMPTY,
    NATIVE_FOLLOWUP_FULL,
    NATIVE_FOLLOWUP_NOT_FOUND,
    NATIVE_FOLLOWUP_CONFLICT,
    NATIVE_FOLLOWUP_INVALID,
    NATIVE_FOLLOWUP_TOO_LONG,
    NATIVE_FOLLOWUP_BUFFER_SMALL,
    NATIVE_FOLLOWUP_EXHAUSTED
} native_followup_result_t;

/* Private state: do not copy a live queue or access fields without its mutex.
 * Initialize before publishing; destroy only after all users have joined.
 * IDs are never reused during one initialized queue lifetime. */
typedef struct {
    pthread_mutex_t mutex;
    uint64_t next_id;
    native_followup_snapshot_t state;
} native_followup_queue_t;

bool native_followup_queue_init(native_followup_queue_t *q);
void native_followup_queue_destroy(native_followup_queue_t *q);
native_followup_result_t native_followup_queue_push(native_followup_queue_t *q,
                                                   const char *text, uint64_t *id);
/* Too-small output is rejected without consuming the item; id is optional. */
native_followup_result_t native_followup_queue_pop(native_followup_queue_t *q,
                                                  char *out, size_t cap, uint64_t *id);
native_followup_result_t native_followup_queue_snapshot(native_followup_queue_t *q,
                                                       native_followup_snapshot_t *out);
native_followup_result_t native_followup_queue_replace(native_followup_queue_t *q,
                                                       uint64_t id, uint64_t expected_revision,
                                                       const char *text);
native_followup_result_t native_followup_queue_remove(native_followup_queue_t *q,
                                                      uint64_t id, uint64_t expected_revision);
native_followup_result_t native_followup_queue_apply(native_followup_queue_t *q,
                                                     const native_followup_action_t *action);

#endif
