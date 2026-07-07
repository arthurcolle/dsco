#ifndef DSCO_COMMAND_PLANE_H
#define DSCO_COMMAND_PLANE_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* ═══════════════════════════════════════════════════════════════════════════
 * Command Plane: durable local command/idempotency store
 *
 * Design intent
 *   - Every externally-triggered side-effectful command gets a stable command_id.
 *   - Callers may provide an idempotency_key. Replays with the same key return
 *     the first recorded outcome instead of executing the side effect again.
 *   - The local store is append/transition oriented: pending -> running ->
 *     succeeded|failed|canceled. A command may be retried only when policy allows
 *     and the prior terminal state is not a success for the same idempotency key.
 *
 * Local persistence
 *   - SQLite database at $DSCO_COMMAND_PLANE_PATH or
 *     ~/.dsco/command_plane/commands.sqlite3.
 *   - WAL + FULL synchronous by default: command records are small; durability is
 *     more valuable than marginal write latency here.
 *   - `idempotency_key` is UNIQUE when non-empty. Empty/NULL keys are allowed but
 *     do not deduplicate.
 *
 * Minimal lifecycle
 *   1. command_plane_open(&cp, NULL)
 *   2. command_plane_begin(&cp, &cmd, "tool:x", idem_key, request_hash)
 *   3. execute side effect only if begin returns COMMAND_PLANE_BEGIN_NEW
 *   4. command_plane_complete/fail records the outcome
 *   5. command_plane_close(&cp)
 *
 * This module is intentionally small: it establishes the durable substrate and
 * replay contract without wiring command dispatch yet.
 * ═══════════════════════════════════════════════════════════════════════════ */

#define COMMAND_PLANE_ID_LEN       64
#define COMMAND_PLANE_KEY_LEN      128
#define COMMAND_PLANE_KIND_LEN     64
#define COMMAND_PLANE_HASH_LEN     128
#define COMMAND_PLANE_STATUS_LEN   24
#define COMMAND_PLANE_RESULT_LEN   2048
#define COMMAND_PLANE_ERROR_LEN    512
#define COMMAND_PLANE_PATH_LEN     512

typedef enum {
    COMMAND_PLANE_OK = 0,
    COMMAND_PLANE_ERR = -1,
    COMMAND_PLANE_NOT_FOUND = 1,
} command_plane_rc_t;

typedef enum {
    COMMAND_PLANE_BEGIN_NEW = 0,
    COMMAND_PLANE_BEGIN_REPLAY = 1,
    COMMAND_PLANE_BEGIN_CONFLICT = 2,
    COMMAND_PLANE_BEGIN_ERROR = -1,
} command_plane_begin_t;

typedef enum {
    COMMAND_PLANE_STATUS_PENDING = 0,
    COMMAND_PLANE_STATUS_RUNNING = 1,
    COMMAND_PLANE_STATUS_SUCCEEDED = 2,
    COMMAND_PLANE_STATUS_FAILED = 3,
    COMMAND_PLANE_STATUS_CANCELED = 4,
} command_plane_status_t;

typedef struct {
    char id[COMMAND_PLANE_ID_LEN];
    char kind[COMMAND_PLANE_KIND_LEN];
    char idempotency_key[COMMAND_PLANE_KEY_LEN];
    char request_hash[COMMAND_PLANE_HASH_LEN];
    command_plane_status_t status;
    int attempt_count;
    int64_t created_at_unix;
    int64_t updated_at_unix;
    char result_json[COMMAND_PLANE_RESULT_LEN];
    char error[COMMAND_PLANE_ERROR_LEN];
} command_record_t;

typedef struct {
    void *sqlite; /* sqlite3*, kept opaque to avoid leaking sqlite headers. */
    char path[COMMAND_PLANE_PATH_LEN];
    bool initialized;
} command_plane_t;

int command_plane_open(command_plane_t *cp, const char *path);
void command_plane_close(command_plane_t *cp);

command_plane_begin_t command_plane_begin(command_plane_t *cp,
                                          command_record_t *out,
                                          const char *kind,
                                          const char *idempotency_key,
                                          const char *request_hash);

int command_plane_get_by_id(command_plane_t *cp,
                            const char *command_id,
                            command_record_t *out);

int command_plane_get_by_idempotency_key(command_plane_t *cp,
                                         const char *idempotency_key,
                                         command_record_t *out);

int command_plane_complete(command_plane_t *cp, const char *command_id,
                           const char *result_json);
int command_plane_fail(command_plane_t *cp, const char *command_id,
                       const char *error);

const char *command_plane_status_name(command_plane_status_t status);
command_plane_status_t command_plane_status_from_name(const char *status);

#endif /* DSCO_COMMAND_PLANE_H */
