#include "turn_record.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char captured[4096];
static size_t captured_len;
static int mode, writes, syncs;
static ssize_t fault_write(int fd, const void *bytes, size_t n) {
    (void)fd;
    ++writes;
    if (mode == 1 && writes == 1) { errno = EINTR; return -1; }
    if (mode == 2 && writes == 2) { errno = ENOSPC; return -1; }
    if (mode == 3) return 0;
    if (n > 7) n = 7;
    assert(captured_len + n < sizeof(captured));
    memcpy(captured + captured_len, bytes, n);
    captured_len += n;
    return (ssize_t)n;
}
static int fault_sync(int fd) {
    (void)fd;
    ++syncs;
    if (mode == 1 && syncs == 1) { errno = EINTR; return -1; }
    if (mode == 4) { errno = EIO; return -1; }
    return 0;
}
static void reset(int m) {
    mode = m; writes = syncs = 0; captured_len = 0;
    memset(captured, 0, sizeof(captured));
}
static void golden(const turn_record_t *r, const char *expected) {
    size_t n = 99;
    char *bytes = turn_record_encode(r, &n);
    assert(bytes && n == strlen(expected) && !strcmp(bytes, expected));
    free(bytes);
}
int main(void) {
    usage_t usage = { .input_tokens = 12, .output_tokens = 3,
        .cache_read_input_tokens = 4, .cache_creation_input_tokens = 5 };
    turn_record_t r = { .timestamp = 1700000000, .turn = 2, .model = "model",
        .usage = &usage, .cost = 0.0123456, .stop_reason = "end_turn" };
    golden(&r, "{\"ts\":1700000000,\"turn\":2,\"model\":\"model\",\"in\":12,\"out\":3,\"cache_read\":4,\"cache_write\":5,\"cost\":0.012346,\"stop\":\"end_turn\"}\n");
    r = (turn_record_t){0};
    const char *empty = "{\"ts\":0,\"turn\":0,\"model\":\"\",\"cost\":0.000000}\n";
    golden(&r, empty);
    r.model = "quote\" slash\\ line\n\t\001 snow\xe2\x98\x83";
    r.stop_reason = "";
    r.cost = -0.0;
    golden(&r, "{\"ts\":0,\"turn\":0,\"model\":\"quote\\\" slash\\\\ line\\n\\t\\u0001 snow\xe2\x98\x83\",\"cost\":-0.000000,\"stop\":\"\"}\n");
    r = (turn_record_t){ .timestamp = INT_MAX, .turn = INT_MIN };
    golden(&r, "{\"ts\":2147483647,\"turn\":-2147483648,\"model\":\"\",\"cost\":0.000000}\n");
    char long_model[4097]; memset(long_model, 'x', sizeof(long_model)-1); long_model[4096] = 0;
    r.model = long_model;
    size_t length;
    char *large = turn_record_encode(&r, &length);
    assert(large && length > 4096 && strstr(large, long_model)); free(large);
    r.cost = NAN; length = 99;
    assert(!turn_record_encode(&r, &length) && errno == EINVAL && length == 0);
    r.cost = INFINITY;
    assert(!turn_record_encode(&r, NULL) && errno == EINVAL);
    assert(!turn_record_encode(NULL, NULL) && errno == EINVAL);
    r = (turn_record_t){0};
    for (int m = 0; m <= 4; ++m) {
        reset(m);
        bool ok = turn_record_append_with_io(7, &r, fault_write, fault_sync);
        if (m <= 1) {
            assert(ok && !strcmp(captured, empty));
            assert(syncs == (m == 1 ? 2 : 1));
        } else if (m == 2) {
            assert(!ok && errno == ENOSPC && syncs == 0 && captured_len == 7);
        } else if (m == 3) {
            assert(!ok && errno == EIO && syncs == 0 && captured_len == 0);
        } else {
            assert(!ok && errno == EIO && syncs == 1 && !strcmp(captured, empty));
        }
    }
    reset(0);
    assert(!turn_record_append(-1, &r) && errno == EBADF);
    assert(!turn_record_append_with_io(7, &r, NULL, fault_sync) && errno == EINVAL);
    assert(writes == 0 && syncs == 0);
    char path[] = "/tmp/dsco-turn-record-XXXXXX";
    int fd = mkstemp(path); assert(fd >= 0); unlink(path);
    assert(turn_record_append(fd, &r));
    assert(turn_record_append(fd, &r));
    assert(lseek(fd, 0, SEEK_SET) == 0);
    char actual[1024] = {0};
    assert(read(fd, actual, sizeof(actual)-1) == (ssize_t)(2 * strlen(empty)));
    char expected[1024]; snprintf(expected, sizeof(expected), "%s%s", empty, empty);
    assert(!strcmp(actual, expected)); close(fd);
    puts("turn_record: golden wire fixtures, ownership, boundaries, real append and injected I/O faults PASS");
    return 0;
}
