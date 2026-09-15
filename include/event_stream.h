#ifndef DSCO_EVENT_STREAM_H
#define DSCO_EVENT_STREAM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Ordered, durable native event outbox; transport is a replayable projection.
 * open creates a new absolute database path exclusively with mode 0600 and
 * duplicates an already-nonblocking connected AF_UNIX SOCK_STREAM descriptor
 * with CLOEXEC. It never
 * changes the caller descriptor's status flags or shuts down the shared socket.
 * Native exec children join through DSCO_EVENT_STREAM_DB, each with its own
 * SQLite connection. A fork child is inert until exec; no Lua callbacks occur.
 */
bool event_stream_open(const char *dbpath, int socket_fd, char *err, size_t err_cap);
bool event_stream_active(void);
/* Disabled capture succeeds as a no-op. Enabled capture failure is sticky.
 * Valid JSON payloads retain their structure; invalid JSON becomes {text:...}.
 * Each committed record is <=32 MiB. source/event are bounded to 128 bytes.
 */
bool event_stream_emit(const char *source, const char *event, const char *payload_json);
bool event_stream_healthy(void);
/* Producers must report encoding/instrumentation failures before emit. */
void event_stream_fail(const char *reason);
bool event_stream_checkpoint(void);
/* Stop exporter with a two-second transport drain window and retain backlog.
 * Receipt sent_seq means fully written to the socket, never consumer ACK.
 * Closing observation neither kills workers nor deletes retained events.
 */
bool event_stream_close(char *receipt, size_t receipt_cap);
/* Replay the fixed committed high-water mark observed at invocation. Socket
 * validation matches open; a two-second no-progress deadline retains backlog.
 * after_seq is exclusive. The database is never mutated by replay.
 */
bool event_stream_replay(const char *dbpath, int socket_fd, uint64_t after_seq,
                         char *receipt, size_t receipt_cap);

#endif
