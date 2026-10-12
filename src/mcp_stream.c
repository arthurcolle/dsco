#include "mcp_stream.h"
#include "mcp_response.h"
#include "../vendor/yyjson.h"
#include <pthread.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define STREAM_LIMIT (16u * 1024u * 1024u)
struct mcp_stream {
    pthread_mutex_t lock;
    char *data;
    size_t len, offset, capacity;
};
mcp_stream_t *mcp_stream_new(void) {
    mcp_stream_t *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    int rc = pthread_mutex_init(&s->lock, NULL);
    if (rc) { free(s); errno = rc; return NULL; }
    return s;
}
void mcp_stream_free(mcp_stream_t *s) {
    if (!s) return;
    pthread_mutex_destroy(&s->lock);
    free(s->data); free(s);
}
bool mcp_stream_lock(mcp_stream_t *s) {
    if (!s) { errno = EINVAL; return false; }
    int rc = pthread_mutex_lock(&s->lock);
    if (rc) errno = rc;
    return rc == 0;
}
void mcp_stream_unlock(mcp_stream_t *s) { if (s) pthread_mutex_unlock(&s->lock); }
size_t mcp_stream_pending(const mcp_stream_t *s) { return s ? s->len - s->offset : 0; }
bool mcp_stream_feed(mcp_stream_t *s, const char *data, size_t len) {
    if (!s || (!data && len)) { errno = EINVAL; return false; }
    size_t pending = mcp_stream_pending(s);
    if (len > STREAM_LIMIT || pending > STREAM_LIMIT - len) { errno = EOVERFLOW; return false; }
    if (len && memchr(data, 0, len)) { errno = EPROTO; return false; }
    if (s->offset) {
        if (pending) memmove(s->data, s->data + s->offset, pending);
        s->len = pending; s->offset = 0;
    }
    size_t required = pending + len;
    if (required > s->capacity) {
        size_t capacity = s->capacity ? s->capacity : 4096;
        while (capacity < required) capacity *= 2;
        char *buffer = realloc(s->data, capacity);
        if (!buffer) return false;
        s->data = buffer; s->capacity = capacity;
    }
    if (len) memcpy(s->data + s->len, data, len);
    s->len += len;
    return true;
}
char *mcp_stream_take(mcp_stream_t *s, const char *request) {
    if (!s || !request) return NULL;
    while (s->offset < s->len) {
        const char *start = s->data + s->offset;
        size_t remaining = s->len - s->offset;
        const char *newline = memchr(start, '\n', remaining);
        size_t width = newline ? (size_t)(newline - start) : remaining;
        if (!newline) {
            /* Read boundaries have no protocol meaning. Only consume the
             * newline-free compatibility case after full JSON validation. */
            yyjson_doc *doc = yyjson_read(start, width, 0);
            if (!doc) return NULL;
            bool object = yyjson_is_obj(yyjson_doc_get_root(doc));
            yyjson_doc_free(doc);
            if (!object) return NULL;
        }
        char *matched = mcp_response_match(start, width, request);
        s->offset += width + (newline ? 1 : 0);
        if (matched) return matched;
    }
    return NULL;
}
