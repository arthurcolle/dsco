#ifdef __APPLE__
#define _DARWIN_C_SOURCE 1
#endif
#include "event_stream.h"
#include "../vendor/yyjson.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sqlite3.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

typedef struct { int fd; uint64_t first, count; bool failed; } consumer;

static double now_ms(void) {
    struct timespec ts; assert(clock_gettime(CLOCK_MONOTONIC, &ts) == 0);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static int compare_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static void *consume(void *opaque) {
    consumer *c = opaque;
    FILE *in = fdopen(c->fd, "r"); assert(in);
    char *line = NULL; size_t cap = 0; ssize_t n;
    while ((n = getline(&line, &cap, in)) > 0) {
        yyjson_doc *doc = yyjson_read(line, (size_t)n, 0);
        if (!doc) { c->failed = true; continue; }
        yyjson_val *root = yyjson_doc_get_root(doc), *record = yyjson_obj_get(root, "record");
        yyjson_val *seq = yyjson_obj_get(root, "seq"), *schema = yyjson_obj_get(record, "schema");
        uint64_t expected = c->first + c->count;
        if (!yyjson_is_uint(seq) || yyjson_get_uint(seq) != expected || !yyjson_is_obj(record) ||
            !yyjson_is_str(schema) || strcmp(yyjson_get_str(schema), "dsco.event.v1") ||
            !yyjson_is_str(yyjson_obj_get(record, "clock")) ||
            strcmp(yyjson_get_str(yyjson_obj_get(record, "clock")), "CLOCK_MONOTONIC") ||
            !yyjson_is_str(yyjson_obj_get(record, "monotonic_ns")) ||
            !yyjson_obj_get(record, "payload")) c->failed = true;
        c->count++;
        yyjson_doc_free(doc);
    }
    free(line); fclose(in); return NULL;
}

static uint64_t field(const char *json, const char *name) {
    yyjson_doc *doc = yyjson_read(json, strlen(json), 0); assert(doc);
    yyjson_val *value = yyjson_obj_get(yyjson_doc_get_root(doc), name);
    assert(yyjson_is_uint(value));
    uint64_t result = yyjson_get_uint(value); yyjson_doc_free(doc); return result;
}

static bool flag(const char *json, const char *name) {
    yyjson_doc *doc = yyjson_read(json, strlen(json), 0); assert(doc);
    yyjson_val *value = yyjson_obj_get(yyjson_doc_get_root(doc), name);
    assert(yyjson_is_bool(value));
    bool result = yyjson_get_bool(value); yyjson_doc_free(doc); return result;
}

static void database_check(const char *path, uint64_t expected) {
    struct stat st; assert(stat(path, &st) == 0 && (st.st_mode & 0777) == 0600);
    sqlite3 *db = NULL; assert(sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK);
    sqlite3_stmt *q = NULL;
    assert(sqlite3_prepare_v2(db, "SELECT count(*),min(seq),max(seq) FROM events;", -1, &q, NULL) == SQLITE_OK);
    assert(sqlite3_step(q) == SQLITE_ROW && (uint64_t)sqlite3_column_int64(q, 0) == expected &&
           sqlite3_column_int64(q, 1) == 1 && (uint64_t)sqlite3_column_int64(q, 2) == expected);
    sqlite3_finalize(q);
    assert(sqlite3_prepare_v2(db, "PRAGMA integrity_check;", -1, &q, NULL) == SQLITE_OK);
    assert(sqlite3_step(q) == SQLITE_ROW && !strcmp((const char *)sqlite3_column_text(q, 0), "ok"));
    sqlite3_finalize(q); sqlite3_close(db);
    assert(sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE, NULL) == SQLITE_OK);
    assert(sqlite3_exec(db, "UPDATE events SET record='{}' WHERE seq=1;", NULL, NULL, NULL) == SQLITE_CONSTRAINT);
    assert(sqlite3_exec(db, "DELETE FROM events WHERE seq=1;", NULL, NULL, NULL) == SQLITE_CONSTRAINT);
    sqlite3_close(db);
}

int main(int argc, char **argv) {
    assert(argc >= 2);
    if (!strcmp(argv[1], "child")) {
        assert(event_stream_active() && event_stream_healthy());
        for (int i = 0; i < 200; i++) assert(event_stream_emit("fixture", "child.record", "{\"value\":1}"));
        assert(event_stream_checkpoint());
        return 0;
    }
    if (!strcmp(argv[1], "failed_child")) {
        assert(event_stream_active() && !event_stream_healthy());
        assert(!event_stream_emit("fixture", "must.not.exist", "{}"));
        return 0;
    }
    assert(argc == 3);
    const char *mode = argv[1], *path = argv[2];
    int sockets[2]; assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    char rejected[128];
    assert(!event_stream_open(path, sockets[0], rejected, sizeof(rejected)));
    assert(!strcmp(rejected, "nonblocking_socket_required") && !event_stream_active());
    assert(fcntl(sockets[0], F_SETFL, fcntl(sockets[0], F_GETFL) | O_NONBLOCK) == 0);
    int original_flags = fcntl(sockets[0], F_GETFL);
    char error[128], receipt[8192];
    assert(event_stream_open(path, sockets[0], error, sizeof(error)));
    assert(fcntl(sockets[0], F_GETFL) == original_flags);
    assert(event_stream_active() && event_stream_healthy());
    double start = now_ms();
    if (!strcmp(mode, "multiprocess")) {
        consumer c = {.fd = sockets[1], .first = 1}; pthread_t reader;
        assert(pthread_create(&reader, NULL, consume, &c) == 0);
        pid_t children[4];
        for (int i = 0; i < 4; i++) {
            children[i] = fork(); assert(children[i] >= 0);
            if (!children[i]) {
                /* All inherited exporter/mutex state must be inert before exec. */
                if (event_stream_active() || !event_stream_emit("fixture", "must.not.exist", "{}")) _exit(90);
                close(sockets[0]); close(sockets[1]);
                execl(argv[0], argv[0], "child", (char *)NULL); _exit(91);
            }
        }
        double latency[200];
        for (int i = 0; i < 200; i++) {
            double before = now_ms();
            assert(event_stream_emit("fixture", "root.record", i ? "{\"value\":2}" : "not JSON"));
            latency[i] = (now_ms() - before) * 1000.0;
        }
        qsort(latency, 200, sizeof(latency[0]), compare_double);
        for (int i = 0; i < 4; i++) { int status; assert(waitpid(children[i], &status, 0) == children[i]); assert(WIFEXITED(status) && !WEXITSTATUS(status)); }
        double elapsed = now_ms() - start;
        assert(event_stream_close(receipt, sizeof(receipt)));
        close(sockets[0]); pthread_join(reader, NULL);
        assert(!c.failed && c.count == 1000 && field(receipt, "sent_seq") == 1000 && !field(receipt, "pending"));
        database_check(path, 1000);
        printf("{\"mode\":\"multiprocess\",\"passed\":true,\"records\":1000,\"native_processes\":5,\"elapsed_ms\":%.3f,\"events_per_second\":%.3f,\"emit_commit_latency_us\":{\"p50\":%.3f,\"p95\":%.3f,\"max\":%.3f},\"receipt\":%s}\n", elapsed, 1000000.0 / elapsed, latency[99], latency[189], latency[199], receipt);
        return 0;
    }
    if (!strcmp(mode, "disconnected") || !strcmp(mode, "slow")) {
        bool disconnected = !strcmp(mode, "disconnected");
        int small = 4096; assert(setsockopt(sockets[0], SOL_SOCKET, SO_SNDBUF, &small, sizeof(small)) == 0);
        if (disconnected) close(sockets[1]);
        char payload[6000]; memset(payload, 'x', sizeof(payload)-1); payload[sizeof(payload)-1] = 0;
        fprintf(stderr, "fixture: emitting %s\n", mode);
        for (int i = 0; i < 100; i++) assert(event_stream_emit("fixture", "large.record", payload));
        fprintf(stderr, "fixture: closing %s\n", mode);
        double closing = now_ms();
        assert(!event_stream_close(receipt, sizeof(receipt)));
        double close_ms = now_ms() - closing;
        assert(close_ms < 3000 && flag(receipt, "transport_error") && !flag(receipt, "capture_error") && field(receipt, "pending") > 0);
        fprintf(stderr, "fixture: closed %s %.3fms\n", mode, close_ms);
        close(sockets[0]); if (!disconnected) close(sockets[1]);
        database_check(path, 100);
        int replay_sockets[2]; assert(socketpair(AF_UNIX, SOCK_STREAM, 0, replay_sockets) == 0);
        assert(fcntl(replay_sockets[0], F_SETFL, fcntl(replay_sockets[0], F_GETFL) | O_NONBLOCK) == 0);
        consumer c = {.fd = replay_sockets[1], .first = 1}; pthread_t reader;
        assert(pthread_create(&reader, NULL, consume, &c) == 0);
        char replay_receipt[8192];
        fprintf(stderr, "fixture: replaying %s\n", mode);
        assert(event_stream_replay(path, replay_sockets[0], 0, replay_receipt, sizeof(replay_receipt)));
        fprintf(stderr, "fixture: replayed %s\n", mode);
        close(replay_sockets[0]); pthread_join(reader, NULL);
        assert(!c.failed && c.count == 100 && !field(replay_receipt, "pending"));
        assert(socketpair(AF_UNIX, SOCK_STREAM, 0, replay_sockets) == 0);
        assert(fcntl(replay_sockets[0], F_SETFL, fcntl(replay_sockets[0], F_GETFL) | O_NONBLOCK) == 0);
        consumer resumed = {.fd = replay_sockets[1], .first = 41};
        assert(pthread_create(&reader, NULL, consume, &resumed) == 0);
        char resumed_receipt[8192];
        assert(event_stream_replay(path, replay_sockets[0], 40, resumed_receipt, sizeof(resumed_receipt)));
        close(replay_sockets[0]); pthread_join(reader, NULL);
        assert(!resumed.failed && resumed.count == 60 && field(resumed_receipt, "sent_seq") == 100);
        printf("{\"mode\":\"%s\",\"passed\":true,\"records\":100,\"close_ms\":%.3f,\"receipt\":%s,\"replay_receipt\":%s}\n", mode, close_ms, receipt, replay_receipt);
        return 0;
    }
    if (!strcmp(mode, "failure")) {
        assert(event_stream_emit("fixture", "before.failure", "{}"));
        event_stream_fail("fixture_producer_failure");
        assert(!event_stream_healthy() && !event_stream_emit("fixture", "after.failure", "{}"));
        pid_t child = fork(); assert(child >= 0);
        if (!child) { close(sockets[0]); close(sockets[1]); execl(argv[0], argv[0], "failed_child", (char *)NULL); _exit(92); }
        int status; assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
        assert(!event_stream_close(receipt, sizeof(receipt)) && flag(receipt, "capture_error"));
        close(sockets[0]); close(sockets[1]); database_check(path, 1);
        printf("{\"mode\":\"failure\",\"passed\":true,\"receipt\":%s}\n", receipt);
        return 0;
    }
    return 2;
}
