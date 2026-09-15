/* Minimal sanitizer driver; the Python suite still uses the real DSCO binary
 * to execute and kill a governed MCP tool before inspecting that actual WAL. */
#include "execution_recovery.h"
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    const char *root = getenv("DSCO_RUNS_DIR");
    if (argc != 4 || strcmp(argv[1], "runs") || strcmp(argv[2], "inspect") ||
        !root || !execution_recovery_valid_run_id(argv[3])) return 2;
    char path[4096];
    int n = snprintf(path, sizeof(path), "%s/%s/journal.wal", root, argv[3]);
    if (n < 0 || (size_t)n >= sizeof(path)) return 2;
    return execution_recovery_report(path, argv[3], stdout);
}
