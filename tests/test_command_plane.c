#include "command_plane.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int fails = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); fails++; } \
    else { printf("ok:   %s\n", msg); } \
} while (0)

static void tmp_db_path(char *buf, size_t len) {
    snprintf(buf, len, "/tmp/dsco-command-plane-test-%ld-%d.sqlite3", (long)getpid(), rand());
    unlink(buf);
}

int main(void) {
    srand(1);
    char path[256];
    tmp_db_path(path, sizeof(path));

    command_plane_t cp;
    memset(&cp, 0, sizeof(cp));
    CHECK(command_plane_open(&cp, path) == COMMAND_PLANE_OK, "command plane init");

    command_record_t rec1, rec2;
    memset(&rec1, 0, sizeof(rec1));
    memset(&rec2, 0, sizeof(rec2));

    command_plane_begin_t br = command_plane_begin(&cp, &rec1, "demo", "idem-1", "hash-a");
    CHECK(br == COMMAND_PLANE_BEGIN_NEW, "first idempotent begin is new");
    CHECK(rec1.status == COMMAND_PLANE_STATUS_RUNNING, "new command starts running");

    br = command_plane_begin(&cp, &rec2, "demo", "idem-1", "hash-a");
    CHECK(br == COMMAND_PLANE_BEGIN_REPLAY, "same key/hash replays");
    CHECK(strcmp(rec1.id, rec2.id) == 0, "replay returns original command id");

    memset(&rec2, 0, sizeof(rec2));
    br = command_plane_begin(&cp, &rec2, "demo", "idem-1", "hash-b");
    CHECK(br == COMMAND_PLANE_BEGIN_CONFLICT, "same idempotency key with different hash conflicts");

    CHECK(command_plane_complete(&cp, rec1.id, "{\"ok\":true}") == COMMAND_PLANE_OK,
          "running command can complete");
    CHECK(command_plane_fail(&cp, rec1.id, "late failure") == COMMAND_PLANE_NOT_FOUND,
          "completed command cannot be failed");

    command_record_t rec3;
    memset(&rec3, 0, sizeof(rec3));
    br = command_plane_begin(&cp, &rec3, "demo", "idem-2", "hash-c");
    CHECK(br == COMMAND_PLANE_BEGIN_NEW, "second command begins");
    CHECK(command_plane_fail(&cp, rec3.id, "boom") == COMMAND_PLANE_OK,
          "running command can fail");
    CHECK(command_plane_complete(&cp, rec3.id, "{}") == COMMAND_PLANE_NOT_FOUND,
          "failed command cannot complete");

    command_plane_close(&cp);
    unlink(path);

    if (fails == 0)
        printf("\nALL COMMAND-PLANE TESTS PASSED\n");
    else
        printf("\n%d TEST(S) FAILED\n", fails);
    return fails == 0 ? 0 : 1;
}
