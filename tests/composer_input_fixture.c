/* Real ANSI/native composer under an owned PTY; no provider or tool calls. */
#include "tui.h"
#include "pixel_tui.h"
#include "vm.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int g_cheap_mode;
vm_t g_vm;
volatile int g_interrupted;
double g_cost_budget;

static void interrupt_composer(int signal_number) {
    (void)signal_number;
    tui_composer_signal_interrupt();
}

int main(int argc, char **argv) {
    if (argc < 3 || argc > 4) return 2;
    bool native = strcmp(argv[1], "native") == 0;
    bool handoff = argc == 4 && strcmp(argv[3], "handoff") == 0;
    FILE *results = fopen(argv[2], "w");
    if (!results) return 2;
    setenv("DSCO_PIXEL_TUI", native ? "1" : "0", 1);
    setenv("DSCO_PIXEL_TUI_DPR", "1", 1);
    setenv("DSCO_PIXEL_TUI_ANIMATIONS", "0", 1);
    setenv("DSCO_TUI_ANIMATIONS", "0", 1);
    tui_status_bar_t status;
    tui_status_bar_init(&status, "owned input fixture");
    status.animations_enabled = false;
    if (native && !pixel_tui_session_begin(stderr, "owned input fixture")) return 2;
    struct sigaction handler = {.sa_handler = interrupt_composer, .sa_flags = SA_RESTART};
    sigemptyset(&handler.sa_mask);
    sigaction(SIGUSR1, &handler, NULL);
    for (int i = 0; i < 2; ++i) {
        tui_composer_preserve_on_interrupt(handoff && i == 0);
        char input[128];
        char *line = tui_composer_read(&status, NULL, input, sizeof(input));
        fprintf(results, "%s\n", line ? line : "<cancelled>");
        fflush(results);
        if (!line) break;
    }
    if (native) pixel_tui_session_end(stderr);
    tui_composer_clear_retained_draft();
    tui_cleanup();
    pthread_mutex_destroy(&status.mutex);
    fclose(results);
    return 0;
}
