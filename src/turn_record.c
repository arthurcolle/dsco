#include "turn_record.h"
#include "json_util.h"
#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <unistd.h>

char *turn_record_encode(const turn_record_t *r, size_t *length) {
    if (length) *length = 0;
    if (!r || !isfinite(r->cost)) {
        errno = EINVAL;
        return NULL;
    }
    jbuf_t b;
    jbuf_init(&b, 512);
    jbuf_appendf(&b, "{\"ts\":%d,\"turn\":%d,\"model\":", r->timestamp, r->turn);
    jbuf_append_json_str(&b, r->model ? r->model : "");
    if (r->usage) {
        jbuf_appendf(&b, ",\"in\":%d,\"out\":%d,\"cache_read\":%d,\"cache_write\":%d",
                     r->usage->input_tokens, r->usage->output_tokens,
                     r->usage->cache_read_input_tokens, r->usage->cache_creation_input_tokens);
    }
    jbuf_appendf(&b, ",\"cost\":%.6f", r->cost);
    if (r->stop_reason) {
        jbuf_append(&b, ",\"stop\":");
        jbuf_append_json_str(&b, r->stop_reason);
    }
    jbuf_append(&b, "}\n");
    if (length) *length = b.len;
    return b.data;
}

static bool append_with_io(int fd, const turn_record_t *record,
                           ssize_t (*write_fn)(int, const void *, size_t),
                           int (*sync_fn)(int)) {
    if (fd < 0 || !write_fn || !sync_fn) {
        errno = fd < 0 ? EBADF : EINVAL;
        return false;
    }
    size_t length;
    char *bytes = turn_record_encode(record, &length);
    if (!bytes) return false;
    size_t offset = 0;
    while (offset < length) {
        ssize_t n = write_fn(fd, bytes + offset, length - offset);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0 || (size_t)n > length - offset) {
            int error = n < 0 ? errno : EIO;
            free(bytes);
            errno = error;
            return false;
        }
        offset += (size_t)n;
    }
    free(bytes);
    int rc;
    do { rc = sync_fn(fd); } while (rc < 0 && errno == EINTR);
    return rc == 0;
}

bool turn_record_append(int fd, const turn_record_t *record) {
    return append_with_io(fd, record, write, fsync);
}

#ifdef DSCO_INTERNAL_TESTS
bool turn_record_append_with_io(int fd, const turn_record_t *record,
                               ssize_t (*write_fn)(int, const void *, size_t),
                               int (*sync_fn)(int)) {
    return append_with_io(fd, record, write_fn, sync_fn);
}
#endif
