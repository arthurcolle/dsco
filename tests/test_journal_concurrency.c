#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

static bool fail_payload;
static bool wal_only;
/* Force a scheduler handoff at exactly the header/payload boundary. This
 * reproduces the original corruption without relying on rare timing luck. */
static ssize_t fixture_write(int fd, const void *buf, size_t len) {
    if (fail_payload && len != 8) { errno = ENOSPC; return -1; }
    ssize_t n = write(fd, buf, len);
    if (len == 8) {
        struct timespec pause = {.tv_nsec = 1000000};
        nanosleep(&pause, NULL);
    }
    return n;
}
#define write fixture_write
#ifndef DSCO_CHRONICLE_SOURCE
#define DSCO_CHRONICLE_SOURCE "../src/chronicle.c"
#endif
#include DSCO_CHRONICLE_SOURCE
#undef write
#include "../vendor/yyjson.h"

static void *writer(void *arg) {
    long id = (long)arg;
    for (int i = 0; i < 40; i++) {
        char payload[96];
        snprintf(payload, sizeof(payload), "{\"worker\":%ld,\"index\":%d}", id, i);
        assert(chronicle_journal_append("fixture", payload, i == 39));
        if (!wal_only)
            assert(chronicle_event("fixture", NULL, NULL, NULL, "test", "fixture", payload, "metadata"));
    }
    return NULL;
}

int main(void) {
    wal_only = getenv("DSCO_TEST_WAL_ONLY") != NULL;
    char path[] = "/tmp/dsco-journal-concurrency-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    g_chronicle.journal_fd = fd;
    g_chronicle.journal_enabled = true;
    g_chronicle.events_fp = tmpfile();
    assert(g_chronicle.events_fp);
    assert(sqlite3_open(":memory:", &g_chronicle.db) == SQLITE_OK);
    assert(ensure_schema());
    strcpy(g_chronicle.session_id, "11111111-1111-4111-8111-111111111111");
    g_chronicle.ready = true;
    pthread_t threads[8];
    for (long i = 0; i < 8; i++) assert(!pthread_create(&threads[i], NULL, writer, (void *)i));
    for (int i = 0; i < 8; i++) assert(!pthread_join(threads[i], NULL));
    FILE *sink = tmpfile();
    assert(sink);
    assert(journal_print_file(path, sink, true) == 0);
    rewind(sink);
    char line[64];
    assert(fgets(line, sizeof(line), sink) && !strcmp(line, "320 records\n"));
    fclose(sink);

    FILE *fp = fopen(path, "rb");
    assert(fp);
    unsigned char header[8];
    unsigned long long last_seq = 0;
    int frames = 0;
    while (fread(header, 1, 8, fp) == 8) {
        uint32_t len = load_le32(header);
        char *body = malloc((size_t)len + 1);
        assert(body && fread(body, 1, len, fp) == len);
        body[len] = 0;
        yyjson_doc *doc = yyjson_read(body, len, 0);
        assert(doc);
        unsigned long long seq = yyjson_get_uint(yyjson_obj_get(yyjson_doc_get_root(doc), "seq"));
        assert(seq > last_seq);
        last_seq = seq;
        frames++;
        yyjson_doc_free(doc);
        free(body);
    }
    assert(frames == 320);
    fclose(fp);

    /* Each JSONL predecessor must hash the immediately preceding full event,
     * even while WAL and event writers compete for the shared sequence. */
    rewind(g_chronicle.events_fp);
    char *event_line = NULL;
    size_t cap = 0;
    char previous_hash[65] = "";
    int events = 0;
    ssize_t n;
    while ((n = getline(&event_line, &cap, g_chronicle.events_fp)) > 0) {
        yyjson_doc *doc = yyjson_read(event_line, (size_t)n, 0);
        assert(doc);
        assert(yyjson_equals_str(yyjson_obj_get(yyjson_doc_get_root(doc), "prev_event_hash"), previous_hash));
        sha256_hex(event_line, (size_t)n - 1, previous_hash);
        yyjson_doc_free(doc);
        events++;
    }
    assert(events == (wal_only ? 0 : 320));
    assert(g_chronicle.seq == (wal_only ? 320 : 640));
    free(event_line);

    fail_payload = true;
    assert(!chronicle_journal_append("fault", "{}", true));
    struct stat before, after;
    assert(!fstat(fd, &before));
    fail_payload = false;
    assert(!chronicle_journal_append("after-fault", "{}", true));
    assert(!fstat(fd, &after) && before.st_size == after.st_size);
    sink = tmpfile();
    assert(sink && journal_print_file(path, sink, true) == 1);
    fclose(sink);
    fclose(g_chronicle.events_fp);
    sqlite3_close(g_chronicle.db);
    close(fd);
    unlink(path);
    puts("PASS: 320 concurrent WAL frames, 320 chained events, monotonic sequence, poisoned-tail refusal");
    return 0;
}
