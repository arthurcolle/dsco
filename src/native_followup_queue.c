#include "native_followup_queue.h"

#include <string.h>

static native_followup_result_t text_length(const char *text, size_t *len) {
    if (!text || !text[0]) return NATIVE_FOLLOWUP_INVALID;
    for (*len = 0; *len < NATIVE_FOLLOWUP_TEXT_CAP; ++*len)
        if (!text[*len]) return NATIVE_FOLLOWUP_OK;
    return NATIVE_FOLLOWUP_TOO_LONG;
}

bool native_followup_queue_init(native_followup_queue_t *q) {
    if (!q) return false;
    memset(q, 0, sizeof(*q));
    q->next_id = 1;
    return pthread_mutex_init(&q->mutex, NULL) == 0;
}

void native_followup_queue_destroy(native_followup_queue_t *q) {
    if (q) pthread_mutex_destroy(&q->mutex);
}

native_followup_result_t native_followup_queue_push(native_followup_queue_t *q,
                                                   const char *text, uint64_t *id) {
    if (!q) return NATIVE_FOLLOWUP_INVALID;
    size_t len;
    native_followup_result_t result = text_length(text, &len);
    if (result != NATIVE_FOLLOWUP_OK) return result;
    pthread_mutex_lock(&q->mutex);
    if (q->state.count == NATIVE_FOLLOWUP_QUEUE_CAP) {
        result = NATIVE_FOLLOWUP_FULL;
    } else if (!q->next_id || q->state.revision == UINT64_MAX) {
        result = NATIVE_FOLLOWUP_EXHAUSTED;
    } else {
        native_followup_entry_t *entry = &q->state.entries[q->state.count++];
        entry->id = q->next_id++;
        entry->revision = ++q->state.revision;
        memcpy(entry->text, text, len + 1);
        if (id) *id = entry->id;
    }
    pthread_mutex_unlock(&q->mutex);
    return result;
}

/* Caller holds the mutex. Compaction preserves FIFO and all surviving IDs. */
static void erase_entry(native_followup_queue_t *q, size_t index) {
    size_t following = q->state.count - index - 1;
    if (following)
        memmove(&q->state.entries[index], &q->state.entries[index + 1],
                following * sizeof(q->state.entries[0]));
    memset(&q->state.entries[--q->state.count], 0, sizeof(q->state.entries[0]));
    ++q->state.revision;
}

native_followup_result_t native_followup_queue_pop(native_followup_queue_t *q,
                                                  char *out, size_t cap, uint64_t *id) {
    if (!q || !out || !cap) return NATIVE_FOLLOWUP_INVALID;
    pthread_mutex_lock(&q->mutex);
    native_followup_result_t result = NATIVE_FOLLOWUP_OK;
    if (!q->state.count) {
        result = NATIVE_FOLLOWUP_EMPTY;
    } else if (strlen(q->state.entries[0].text) >= cap) {
        result = NATIVE_FOLLOWUP_BUFFER_SMALL;
    } else if (q->state.revision == UINT64_MAX) {
        result = NATIVE_FOLLOWUP_EXHAUSTED;
    } else {
        native_followup_entry_t *entry = &q->state.entries[0];
        memcpy(out, entry->text, strlen(entry->text) + 1);
        if (id) *id = entry->id;
        erase_entry(q, 0);
    }
    pthread_mutex_unlock(&q->mutex);
    return result;
}

native_followup_result_t native_followup_queue_snapshot(native_followup_queue_t *q,
                                                       native_followup_snapshot_t *out) {
    if (!q || !out) return NATIVE_FOLLOWUP_INVALID;
    pthread_mutex_lock(&q->mutex);
    memcpy(out, &q->state, sizeof(*out));
    pthread_mutex_unlock(&q->mutex);
    return NATIVE_FOLLOWUP_OK;
}

static native_followup_result_t modify_entry(native_followup_queue_t *q,
                                             uint64_t id, uint64_t expected_revision,
                                             const char *text, size_t len) {
    pthread_mutex_lock(&q->mutex);
    native_followup_result_t result = NATIVE_FOLLOWUP_NOT_FOUND;
    for (size_t i = 0; i < q->state.count; ++i) {
        native_followup_entry_t *entry = &q->state.entries[i];
        if (entry->id != id) continue;
        if (expected_revision && entry->revision != expected_revision) {
            result = NATIVE_FOLLOWUP_CONFLICT;
        } else if (q->state.revision == UINT64_MAX) {
            result = NATIVE_FOLLOWUP_EXHAUSTED;
        } else {
            if (text) {
                memset(entry->text, 0, sizeof(entry->text));
                memcpy(entry->text, text, len + 1);
                entry->revision = ++q->state.revision;
            } else {
                erase_entry(q, i);
            }
            result = NATIVE_FOLLOWUP_OK;
        }
        break;
    }
    pthread_mutex_unlock(&q->mutex);
    return result;
}

native_followup_result_t native_followup_queue_replace(native_followup_queue_t *q,
                                                       uint64_t id, uint64_t expected_revision,
                                                       const char *text) {
    if (!q || !id) return NATIVE_FOLLOWUP_INVALID;
    size_t len;
    native_followup_result_t result = text_length(text, &len);
    if (result != NATIVE_FOLLOWUP_OK) return result;
    return modify_entry(q, id, expected_revision, text, len);
}

native_followup_result_t native_followup_queue_remove(native_followup_queue_t *q,
                                                      uint64_t id, uint64_t expected_revision) {
    if (!q || !id) return NATIVE_FOLLOWUP_INVALID;
    return modify_entry(q, id, expected_revision, NULL, 0);
}

native_followup_result_t native_followup_queue_apply(native_followup_queue_t *q,
                                                     const native_followup_action_t *action) {
    if (!q || !action) return NATIVE_FOLLOWUP_INVALID;
    switch (action->kind) {
        case NATIVE_FOLLOWUP_ACTION_REPLACE:
            return native_followup_queue_replace(q, action->id, action->expected_revision,
                                                 action->text);
        case NATIVE_FOLLOWUP_ACTION_REMOVE:
            return native_followup_queue_remove(q, action->id, action->expected_revision);
        default:
            return NATIVE_FOLLOWUP_INVALID;
    }
}
