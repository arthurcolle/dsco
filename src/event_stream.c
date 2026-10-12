#ifdef __APPLE__
#define _DARWIN_C_SOURCE 1
#endif
#include "event_stream.h"
#include "../vendor/yyjson.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sqlite3.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define RECORD_LIMIT (32u * 1024u * 1024u)
#define DRAIN_NS 2000000000ULL
#define FORMAT "dsco.event.v1"

/* The producer connection belongs to exactly one process. The root exporter
 * has a separate read-only connection and never holds mu during socket I/O.
 * The owning PID is claimed before touching mu: inherited mutexes are inert. */
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static _Atomic pid_t owner_pid;
static _Atomic bool bad_capture, bad_transport, stopping;
static _Atomic uint64_t sent_seq, stop_at;
static bool initialized, closed, root, writer_started;
static sqlite3 *db;
static pthread_t writer;
static int output_fd = -1;
static pid_t root_pid;
static char path[PATH_MAX], marker[PATH_MAX];

static uint64_t monotonic_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) return 0;
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void error_text(char *out, size_t cap, const char *value) {
    if (out && cap) snprintf(out, cap, "%s", value);
}

static bool inherited(void) {
    pid_t owner = atomic_load(&owner_pid);
    return owner && owner != getpid();
}

static bool claim_owner(void) {
    pid_t zero = 0, current = getpid();
    return atomic_compare_exchange_strong(&owner_pid, &zero, current) || zero == current;
}

static bool valid_path(const char *candidate) {
    return candidate && candidate[0] == '/' &&
           strnlen(candidate, PATH_MAX) < PATH_MAX - 7;
}

static bool existing_file(const char *candidate) {
    struct stat st;
    return valid_path(candidate) && lstat(candidate, &st) == 0 && S_ISREG(st.st_mode) &&
           st.st_uid == geteuid() && (st.st_mode & 077) == 0;
}

static bool sql(sqlite3 *connection, const char *statement) {
    return sqlite3_exec(connection, statement, NULL, NULL, NULL) == SQLITE_OK;
}

static bool scalar(sqlite3 *connection, const char *query, uint64_t *out) {
    sqlite3_stmt *statement = NULL;
    bool ok = sqlite3_prepare_v2(connection, query, -1, &statement, NULL) == SQLITE_OK &&
              sqlite3_step(statement) == SQLITE_ROW &&
              sqlite3_column_type(statement, 0) == SQLITE_INTEGER &&
              sqlite3_column_int64(statement, 0) >= 0;
    if (ok) *out = (uint64_t)sqlite3_column_int64(statement, 0);
    sqlite3_finalize(statement);
    return ok;
}

static void fail_locked(const char *reason) {
    atomic_store(&bad_capture, true);
    /* The sidecar covers failures which make the SQLite health row unwritable.
     * It contains only a fixed diagnostic code, never event payloads. */
    if (marker[0]) {
        int fd = open(marker, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd >= 0) {
            size_t len = strlen(reason), at = 0;
            while (at < len) {
                ssize_t n = write(fd, reason + at, len - at);
                if (n > 0) at += (size_t)n;
                else if (n < 0 && errno == EINTR) continue;
                else break;
            }
            (void)fsync(fd);
            close(fd);
        }
    }
    if (db) (void)sql(db, "UPDATE health SET capture_error=1 WHERE id=1;");
}

static bool health_locked(void) {
    if (atomic_load(&bad_capture) || !db) return false;
    struct stat st;
    if (lstat(marker, &st) == 0 || errno != ENOENT) {
        atomic_store(&bad_capture, true);
        return false;
    }
    uint64_t unhealthy = 0;
    if (!scalar(db, "SELECT capture_error FROM health WHERE id=1;", &unhealthy)) {
        fail_locked("health_read_failed");
        return false;
    }
    if (unhealthy) atomic_store(&bad_capture, true);
    return unhealthy == 0;
}

static bool connect_existing_locked(const char *candidate) {
    if (!valid_path(candidate)) {
        atomic_store(&bad_capture, true);
        return false;
    }
    snprintf(path, sizeof(path), "%s", candidate);
    snprintf(marker, sizeof(marker), "%s.error", candidate);
    if (!existing_file(candidate) ||
        sqlite3_open_v2(candidate, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX,
                        NULL) != SQLITE_OK) {
        fail_locked("outbox_open_failed");
        return false;
    }
    sqlite3_busy_timeout(db, 5000);
    uint64_t value = 0;
    if (!sql(db, "PRAGMA synchronous=FULL;") ||
        !scalar(db, "SELECT root_pid FROM stream_metadata WHERE id=1 AND format_version=1;", &value) ||
        value == 0 || value > INT_MAX) {
        fail_locked("outbox_metadata_invalid");
        return false;
    }
    root_pid = (pid_t)value;
    return health_locked();
}

/* Returns with mu held for an enabled stream, including a failed one. */
static bool lock_active(void) {
    if (inherited()) return false;
    const char *configured = getenv("DSCO_EVENT_STREAM_DB");
    if (!atomic_load(&owner_pid) && (!configured || !configured[0])) return false;
    if (!claim_owner()) return false;
    pthread_mutex_lock(&mu);
    if (closed) { pthread_mutex_unlock(&mu); return false; }
    if (!initialized) {
        if (!configured || !configured[0]) { pthread_mutex_unlock(&mu); return false; }
        initialized = true;
        (void)connect_existing_locked(configured);
    }
    return true;
}

bool event_stream_active(void) {
    if (!lock_active()) return false;
    pthread_mutex_unlock(&mu);
    return true;
}

bool event_stream_healthy(void) {
    if (!lock_active()) return true;
    bool ok = health_locked();
    pthread_mutex_unlock(&mu);
    return ok;
}

void event_stream_fail(const char *reason) {
    if (!lock_active()) return;
    const char *code = reason && reason[0] && strnlen(reason, 129) <= 128
                           ? reason : "producer_capture_failed";
    fail_locked(code);
    pthread_mutex_unlock(&mu);
}

static int duplicate_socket(int fd, char *err, size_t cap) {
    int type = 0;
    socklen_t length = sizeof(type);
    struct sockaddr_storage peer, local;
    socklen_t peer_len = sizeof(peer), local_len = sizeof(local);
    int descriptor_flags = fd >= 0 ? fcntl(fd, F_GETFL) : -1;
    if (fd < 0 || getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &length) ||
        type != SOCK_STREAM || getpeername(fd, (struct sockaddr *)&peer, &peer_len) ||
        getsockname(fd, (struct sockaddr *)&local, &local_len) ||
        peer.ss_family != AF_UNIX || local.ss_family != AF_UNIX) {
        error_text(err, cap, "connected_unix_stream_socket_required");
        return -1;
    }
    if (descriptor_flags < 0 || !(descriptor_flags & O_NONBLOCK)) {
        error_text(err, cap, "nonblocking_socket_required");
        return -1;
    }
    int owned = fcntl(fd, F_DUPFD_CLOEXEC, 3);
    if (owned < 0) error_text(err, cap, "socket_duplicate_failed");
    return owned;
}

static char *make_record(const char *source, const char *event, const char *payload) {
    if (!source || !source[0] || strnlen(source, 129) > 128 ||
        !event || !event[0] || strnlen(event, 129) > 128) return NULL;
    size_t length = payload ? strnlen(payload, RECORD_LIMIT + 1u) : 0;
    if (length > RECORD_LIMIT) return NULL;
    yyjson_doc *parsed = length ? yyjson_read(payload, length, 0) : NULL;
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (!doc) { yyjson_doc_free(parsed); return NULL; }
    yyjson_mut_val *record = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, record);
    bool ok = record && yyjson_mut_obj_add_str(doc, record, "schema", FORMAT) &&
        yyjson_mut_obj_add_int(doc, record, "pid", getpid()) &&
        yyjson_mut_obj_add_int(doc, record, "ppid", getppid()) &&
        yyjson_mut_obj_add_int(doc, record, "rootpid", root_pid);
    char clock[32];
    snprintf(clock, sizeof(clock), "%llu", (unsigned long long)monotonic_ns());
    ok = ok && yyjson_mut_obj_add_str(doc, record, "clock", "CLOCK_MONOTONIC") &&
        yyjson_mut_obj_add_str(doc, record, "monotonic_ns", clock) &&
        yyjson_mut_obj_add_str(doc, record, "source", source) &&
        yyjson_mut_obj_add_str(doc, record, "event", event);
    yyjson_mut_val *value = parsed ? yyjson_val_mut_copy(doc, yyjson_doc_get_root(parsed))
                                   : yyjson_mut_obj(doc);
    if (!parsed && value)
        ok = ok && yyjson_mut_obj_add_str(doc, value, "text", payload ? payload : "");
    ok = ok && value && yyjson_mut_obj_add_val(doc, record, "payload", value);
    size_t bytes = 0;
    char *result = ok ? yyjson_mut_write(doc, 0, &bytes) : NULL;
    if (bytes > RECORD_LIMIT) { free(result); result = NULL; }
    yyjson_mut_doc_free(doc);
    yyjson_doc_free(parsed);
    return result;
}

bool event_stream_emit(const char *source, const char *event, const char *payload_json) {
    if (!lock_active()) return true;
    bool ok = health_locked();
    char *record = ok ? make_record(source, event, payload_json) : NULL;
    sqlite3_stmt *statement = NULL;
    if (ok && !record) { fail_locked("event_record_invalid_or_too_large"); ok = false; }
    if (ok) {
        ok = sqlite3_prepare_v2(db, "INSERT INTO events(record) VALUES(?1);", -1,
                                &statement, NULL) == SQLITE_OK &&
             sqlite3_bind_text(statement, 1, record, -1, SQLITE_STATIC) == SQLITE_OK &&
             sqlite3_step(statement) == SQLITE_DONE;
        sqlite3_finalize(statement);
        if (!ok) fail_locked("event_commit_failed");
    }
    free(record);
    if (ok) pthread_cond_signal(&changed);
    pthread_mutex_unlock(&mu);
    return ok;
}

static sqlite3 *reader_open(const char *candidate) {
    sqlite3 *reader = NULL;
    if (!existing_file(candidate) || sqlite3_open_v2(candidate, &reader,
            SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX, NULL) != SQLITE_OK) {
        if (reader) sqlite3_close(reader);
        return NULL;
    }
    sqlite3_busy_timeout(reader, 100);
    return reader;
}

/* Copies only one bounded row; no SQLite read transaction remains open while
 * waiting for a slow socket. seq is the only delivery cursor. */
static int next_row(sqlite3 *reader, uint64_t after, uint64_t through,
                     uint64_t *sequence, char **record) {
    sqlite3_stmt *statement = NULL;
    int rc = sqlite3_prepare_v2(reader,
        "SELECT seq,record FROM events WHERE seq>?1 AND seq<=?2 ORDER BY seq LIMIT 1;",
        -1, &statement, NULL);
    if (rc != SQLITE_OK) return -1;
    sqlite3_bind_int64(statement, 1, (sqlite3_int64)after);
    sqlite3_bind_int64(statement, 2, (sqlite3_int64)through);
    rc = sqlite3_step(statement);
    int result = 0;
    if (rc == SQLITE_ROW) {
        sqlite3_int64 id = sqlite3_column_int64(statement, 0);
        int length = sqlite3_column_bytes(statement, 1);
        const char *text = (const char *)sqlite3_column_text(statement, 1);
        if (id <= 0 || length <= 0 || (unsigned)length > RECORD_LIMIT || !text ||
            (size_t)length != strlen(text)) result = -1;
        else {
            *record = malloc((size_t)length + 1);
            if (!*record) result = -1;
            else { memcpy(*record, text, (size_t)length + 1); *sequence = (uint64_t)id; result = 1; }
        }
    } else if (rc != SQLITE_DONE) result = -1;
    sqlite3_finalize(statement);
    return result;
}

/* SIGPIPE is blocked only on the exporter thread, or temporarily by replay.
 * MSG_DONTWAIT changes this send only, never the caller's socket flags. */
static bool send_part(int fd, const char *data, size_t length, bool live, uint64_t *idle) {
    size_t at = 0;
    while (at < length) {
        uint64_t now = monotonic_ns();
        if ((live && atomic_load(&stopping) && now >= atomic_load(&stop_at)) ||
            (!live && now >= *idle)) return false;
        int flags = MSG_DONTWAIT;
#ifdef MSG_NOSIGNAL
        flags |= MSG_NOSIGNAL;
#endif
        size_t chunk = length - at;
        if (chunk > 65536) chunk = 65536;
        ssize_t n = send(fd, data + at, chunk, flags);
        if (n > 0) { at += (size_t)n; if (!live) *idle = monotonic_ns() + DRAIN_NS; continue; }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd wait = {.fd = fd, .events = POLLOUT};
            int rc = poll(&wait, 1, 25);
            if (rc < 0 && errno == EINTR) continue;
            if (rc < 0 || (rc > 0 && (wait.revents & (POLLERR | POLLHUP | POLLNVAL)))) return false;
            continue;
        }
        return false;
    }
    return true;
}

static bool send_row(int fd, uint64_t sequence, const char *record, bool live,
                       uint64_t *idle) {
    char prefix[64];
    int n = snprintf(prefix, sizeof(prefix), "{\"seq\":%llu,\"record\":",
                     (unsigned long long)sequence);
    return n > 0 && (size_t)n < sizeof(prefix) &&
        send_part(fd, prefix, (size_t)n, live, idle) &&
        send_part(fd, record, strlen(record), live, idle) &&
        send_part(fd, "}\n", 2, live, idle);
}

static void wait_for_event(void) {
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_nsec += 1000000;
    if (deadline.tv_nsec >= 1000000000L) { deadline.tv_sec++; deadline.tv_nsec -= 1000000000L; }
    pthread_mutex_lock(&mu);
    if (!atomic_load(&stopping)) pthread_cond_timedwait(&changed, &mu, &deadline);
    pthread_mutex_unlock(&mu);
}

static void *export_events(void *unused) {
    (void)unused;
    sigset_t blocked;
    sigemptyset(&blocked); sigaddset(&blocked, SIGPIPE);
    (void)pthread_sigmask(SIG_BLOCK, &blocked, NULL);
    sqlite3 *reader = reader_open(path);
    if (!reader) { atomic_store(&bad_transport, true); return NULL; }
    while (!atomic_load(&stopping) || monotonic_ns() < atomic_load(&stop_at)) {
        uint64_t sequence = 0, idle = 0;
        char *record = NULL;
        int found = next_row(reader, atomic_load(&sent_seq), INT64_MAX, &sequence, &record);
        if (found < 0) { atomic_store(&bad_transport, true); break; }
        if (!found) {
            if (atomic_load(&stopping)) break;
            wait_for_event();
            continue;
        }
        bool ok = send_row(output_fd, sequence, record, true, &idle);
        free(record);
        if (!ok) { atomic_store(&bad_transport, true); break; }
        atomic_store(&sent_seq, sequence);
    }
    sqlite3_close(reader);
    return NULL;
}

bool event_stream_open(const char *dbpath, int socket_fd, char *err, size_t err_cap) {
    error_text(err, err_cap, "");
    if (inherited() || !claim_owner()) {
        error_text(err, err_cap, "fork_child_requires_exec"); return false;
    }
    pthread_mutex_lock(&mu);
    if (initialized) { error_text(err, err_cap, "event_stream_already_configured"); pthread_mutex_unlock(&mu); return false; }
    if (!valid_path(dbpath)) {
        error_text(err, err_cap, "absolute_new_database_path_required"); pthread_mutex_unlock(&mu); return false;
    }
    int socket = duplicate_socket(socket_fd, err, err_cap);
    if (socket < 0) { pthread_mutex_unlock(&mu); return false; }
    int fresh = open(dbpath, O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fresh < 0) {
        close(socket); error_text(err, err_cap, "new_database_creation_failed"); pthread_mutex_unlock(&mu); return false;
    }
    close(fresh);
    initialized = root = true;
    root_pid = getpid();
    snprintf(path, sizeof(path), "%s", dbpath);
    snprintf(marker, sizeof(marker), "%s.error", dbpath);
    bool ok = sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX,
                              NULL) == SQLITE_OK;
    if (ok) {
        sqlite3_busy_timeout(db, 5000);
        ok = sql(db, "PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL;"
            "CREATE TABLE stream_metadata(id INTEGER PRIMARY KEY CHECK(id=1),format_version INTEGER NOT NULL,root_pid INTEGER NOT NULL);"
            "CREATE TABLE health(id INTEGER PRIMARY KEY CHECK(id=1),capture_error INTEGER NOT NULL CHECK(capture_error IN (0,1)));"
            "INSERT INTO health VALUES(1,0);"
            "CREATE TABLE events(seq INTEGER PRIMARY KEY AUTOINCREMENT,record TEXT NOT NULL CHECK(length(CAST(record AS BLOB))<=33554432));"
            "CREATE TRIGGER events_no_update BEFORE UPDATE ON events BEGIN SELECT RAISE(ABORT,'append_only_events'); END;"
            "CREATE TRIGGER events_no_delete BEFORE DELETE ON events BEGIN SELECT RAISE(ABORT,'append_only_events'); END;");
    }
    char insert[128];
    snprintf(insert, sizeof(insert), "INSERT INTO stream_metadata VALUES(1,1,%ld);", (long)root_pid);
    if (ok) ok = sql(db, insert);
    if (ok) ok = setenv("DSCO_EVENT_STREAM_DB", path, 1) == 0;
    output_fd = socket;
    if (ok) { writer_started = pthread_create(&writer, NULL, export_events, NULL) == 0; ok = writer_started; }
    if (!ok) {
        fail_locked("outbox_initialization_failed");
        close(output_fd); output_fd = -1;
        if (db) { sqlite3_close(db); db = NULL; }
        error_text(err, err_cap, "outbox_initialization_failed");
    }
    pthread_mutex_unlock(&mu);
    return ok;
}

bool event_stream_checkpoint(void) {
    if (!lock_active()) return true;
    bool ok = health_locked();
    if (ok) {
        int frames = 0, copied = 0;
        int rc = sqlite3_wal_checkpoint_v2(db, NULL, SQLITE_CHECKPOINT_PASSIVE, &frames, &copied);
        /* FULL synchronous commits already persist WAL. A reader retaining WAL
         * frames does not make those committed records less durable. */
        ok = rc == SQLITE_OK || rc == SQLITE_BUSY;
        if (!ok) fail_locked("outbox_checkpoint_failed");
    }
    pthread_mutex_unlock(&mu);
    return ok;
}

static bool receipt_json(char *out, size_t cap, const char *database, uint64_t sent,
                          uint64_t last, uint64_t pending, bool capture, bool transport,
                          bool replay) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (!doc) return false;
    yyjson_mut_val *result = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, result);
    bool ok = result && yyjson_mut_obj_add_str(doc, result, "schema", "dsco.event_stream.receipt.v1") &&
        yyjson_mut_obj_add_str(doc, result, "db_path", database ? database : "") &&
        yyjson_mut_obj_add_uint(doc, result, "sent_seq", sent) &&
        yyjson_mut_obj_add_uint(doc, result, "last_seq", last) &&
        yyjson_mut_obj_add_uint(doc, result, "pending", pending) &&
        yyjson_mut_obj_add_bool(doc, result, "capture_error", capture) &&
        yyjson_mut_obj_add_bool(doc, result, "transport_error", transport) &&
        yyjson_mut_obj_add_bool(doc, result, "replay", replay) &&
        yyjson_mut_obj_add_str(doc, result, "sent_meaning", "complete_socket_write_not_consumer_ack");
    size_t length = 0;
    char *json = ok ? yyjson_mut_write(doc, 0, &length) : NULL;
    ok = json && out && cap > length;
    if (ok) memcpy(out, json, length + 1);
    free(json);
    yyjson_mut_doc_free(doc);
    return ok;
}

static bool pending_rows(sqlite3 *connection, uint64_t after, uint64_t *last, uint64_t *pending) {
    char query[128];
    snprintf(query, sizeof(query), "SELECT count(*) FROM events WHERE seq>%llu;", (unsigned long long)after);
    return scalar(connection, "SELECT COALESCE(max(seq),0) FROM events;", last) &&
           scalar(connection, query, pending);
}

bool event_stream_close(char *receipt, size_t receipt_cap) {
    if (!lock_active()) return receipt_json(receipt, receipt_cap, "", 0, 0, 0, false, false, false);
    atomic_store(&stop_at, monotonic_ns() + DRAIN_NS);
    atomic_store(&stopping, true);
    pthread_cond_signal(&changed);
    bool join = root && writer_started;
    pthread_mutex_unlock(&mu);
    if (join) pthread_join(writer, NULL);
    (void)event_stream_checkpoint();
    pthread_mutex_lock(&mu);
    writer_started = false;
    uint64_t last = 0, pending = 0, sent = atomic_load(&sent_seq);
    if (!db || !pending_rows(db, sent, &last, &pending)) atomic_store(&bad_capture, true);
    bool capture = atomic_load(&bad_capture), transport = atomic_load(&bad_transport);
    bool formatted = receipt_json(receipt, receipt_cap, path, sent, last, pending, capture, transport, false);
    if (output_fd >= 0) { close(output_fd); output_fd = -1; }
    if (db) { sqlite3_close(db); db = NULL; }
    closed = true;
    pthread_mutex_unlock(&mu);
    return formatted && !capture && !transport && pending == 0;
}

bool event_stream_replay(const char *dbpath, int socket_fd, uint64_t after_seq,
                         char *receipt, size_t receipt_cap) {
    if (after_seq > INT64_MAX) return false;
    char ignored[96];
    int socket = duplicate_socket(socket_fd, ignored, sizeof(ignored));
    if (socket < 0) return false;
    sqlite3 *reader = reader_open(dbpath);
    uint64_t last = 0, pending = 0, sent = after_seq;
    bool capture = !reader || !pending_rows(reader, after_seq, &last, &pending);
    uint64_t unhealthy = 0;
    if (!capture && (!scalar(reader, "SELECT capture_error FROM health WHERE id=1;", &unhealthy) || unhealthy)) capture = true;
    char error_marker[PATH_MAX];
    if (valid_path(dbpath)) {
        snprintf(error_marker, sizeof(error_marker), "%s.error", dbpath);
        struct stat st;
        if (lstat(error_marker, &st) == 0 || errno != ENOENT) capture = true;
    }
    sigset_t blocked, previous, previously_pending;
    sigemptyset(&blocked); sigaddset(&blocked, SIGPIPE);
    sigpending(&previously_pending);
    int masked = pthread_sigmask(SIG_BLOCK, &blocked, &previous);
    bool transport = masked != 0;
    uint64_t idle = monotonic_ns() + DRAIN_NS;
    /* Valid committed records remain replayable even when later capture failed. */
    while (reader && !transport && sent < last) {
        char *record = NULL;
        uint64_t sequence = 0;
        int found = next_row(reader, sent, last, &sequence, &record);
        if (found != 1) { transport = true; break; }
        transport = !send_row(socket, sequence, record, false, &idle);
        free(record);
        if (!transport) sent = sequence;
    }
    if (!masked) {
        /* Consume only a SIGPIPE generated by these sends, preserving one that
         * was already pending before replay temporarily blocked the signal. */
        if (!sigismember(&previously_pending, SIGPIPE)) {
            sigset_t pending_signals;
            if (sigpending(&pending_signals) == 0 && sigismember(&pending_signals, SIGPIPE)) {
                int signal_number = 0;
                (void)sigwait(&blocked, &signal_number);
            }
        }
        (void)pthread_sigmask(SIG_SETMASK, &previous, NULL);
    }
    close(socket);
    if (reader) {
        char query[160];
        snprintf(query, sizeof(query), "SELECT count(*) FROM events WHERE seq>%llu AND seq<=%llu;",
                 (unsigned long long)sent, (unsigned long long)last);
        if (!scalar(reader, query, &pending)) capture = true;
        sqlite3_close(reader);
    }
    bool formatted = receipt_json(receipt, receipt_cap, dbpath, sent, last, pending,
                                   capture, transport, true);
    return formatted && !capture && !transport && sent >= last;
}
