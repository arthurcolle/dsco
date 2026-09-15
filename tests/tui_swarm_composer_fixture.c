/* PTY fixture for the real composer. No model calls or external windows. */
#include "tui.h"
#include "tui_swarm_dock.h"
#include "vm.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int g_cheap_mode;
vm_t g_vm;
volatile int g_interrupted;
double g_cost_budget;
static atomic_bool stop;

static void *producer(void *unused) {
    (void)unused;
    if (getenv("DSCO_TUI_FIXTURE_STATIC")) return NULL;
    for (int n = 0; !atomic_load(&stop); n++) {
        char line[80];
        int len = snprintf(line, sizeof(line), "fixture stream %d: output stays live\n", n);
        tui_swarm_dock_append(0, line, (size_t)len);
        tui_swarm_dock_update(0, "Inspect runtime behavior", "local fixture",
                              "streaming", (size_t)n * 40, 0);
        if (n == 10) {
            tui_swarm_dock_update(1, "Validate terminal geometry", "local fixture", "done", 420, 0);
            if (getenv("DSCO_TUI_FIXTURE_COMPLETE")) {
                tui_swarm_dock_update(0, NULL, NULL, "done", (size_t)n * 40, 0);
                return NULL;
            }
        }
        struct timespec delay = {.tv_sec = 0, .tv_nsec = 20000000};
        nanosleep(&delay, NULL);
    }
    return NULL;
}

int main(void) {
    setenv("DSCO_PIXEL_TUI", "0", 1);
    tui_swarm_dock_reset();
    tui_swarm_dock_update(0, "Inspect runtime behavior", "local fixture", "running", 0, 0);
    tui_swarm_dock_update(1, "Validate terminal geometry", "local fixture", "running", 0, 0);
    tui_swarm_dock_append(1, "Geometry checks complete.\n", 26);
    tui_status_bar_t status;
    tui_status_bar_init(&status, "local fixture");
    status.animations_enabled = false;
    pthread_t thread;
    if (pthread_create(&thread, NULL, producer, NULL)) return 2;
    char input[512];
    char *result = tui_composer_read(&status, NULL, input, sizeof(input));
    atomic_store(&stop, true);
    pthread_join(thread, NULL);
    tui_cleanup();
    fprintf(stderr, "\nSUBMITTED:%s\n", result ? result : "<cancelled>");
    pthread_mutex_destroy(&status.mutex);
    return result && !strcmp(result, "draft-preserved") ? 0 : 1;
}
