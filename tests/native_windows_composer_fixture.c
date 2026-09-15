/* Real native compositor + real composer, owned PTY only; no provider/tool
 * execution. The producer models an in-flight stream's safe action boundary. */
#include "tui.h"
#include "pixel_tui.h"
#include "native_windows.h"
#include "json_util.h"
#include "vm.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

int g_cheap_mode;
vm_t g_vm;
volatile int g_interrupted;
double g_cost_budget;
static atomic_bool stop, boundary_waiting;
static atomic_int phase, returned, actions, failures;
static _Atomic uint64_t first_event, second_event;
static const char *status_path;
static char submitted[512];

static void status_write(void) {
    native_windows_snapshot_t snapshot;
    native_windows_snapshot(&snapshot);
    jbuf_t b; jbuf_init(&b, 1024);
    jbuf_appendf(&b, "{\"phase\":%d,\"returned\":%d,\"actions\":%d,\"failures\":%d,"
        "\"boundary_waiting\":%s,\"first_event\":%llu,\"second_event\":%llu,"
        "\"focused\":%s,\"focused_id\":%llu,\"captured_id\":%llu,\"windows\":[",
        atomic_load(&phase), atomic_load(&returned), atomic_load(&actions), atomic_load(&failures),
        atomic_load(&boundary_waiting) ? "true" : "false",
        (unsigned long long)atomic_load(&first_event), (unsigned long long)atomic_load(&second_event),
        snapshot.keyboard_focus ? "true" : "false", (unsigned long long)snapshot.focused_id,
        (unsigned long long)snapshot.captured_id);
    for (int i = 0; i < snapshot.count; i++) {
        native_window_t *w = &snapshot.windows[i];
        jbuf_appendf(&b, "%s{\"id\":%llu,\"x\":%d,\"y\":%d,\"width\":%d,\"height\":%d,\"scroll\":%d,\"zoomed\":%s}",
            i ? "," : "", (unsigned long long)w->id, w->rect.x, w->rect.y,
            w->rect.width, w->rect.height, w->scroll, w->zoomed ? "true" : "false");
    }
    jbuf_append(&b, "],\"submitted\":");
    jbuf_append_json_str(&b, atomic_load(&phase) == 3 ? submitted : "");
    jbuf_append(&b, "}");
    char temporary[4096];
    snprintf(temporary, sizeof(temporary), "%s.tmp", status_path);
    FILE *f = fopen(temporary, "w");
    if (!f || fwrite(b.data, 1, b.len, f) != b.len) atomic_fetch_add(&failures, 1);
    if (f && fclose(f)) atomic_fetch_add(&failures, 1);
    if (rename(temporary, status_path)) atomic_fetch_add(&failures, 1);
    jbuf_free(&b);
}

static void receive_action(bool second) {
    native_window_action_t action, extra;
    if (!native_windows_pop_action(&action)) { atomic_fetch_add(&failures, 1); return; }
    char intent[4096]; native_windows_format_action(&action, intent, sizeof(intent));
    if (!intent[0] || intent[0] == '/' || native_windows_pop_action(&extra))
        atomic_fetch_add(&failures, 1);
    atomic_fetch_add(&actions, 1);
    if (second) atomic_store(&second_event, action.event_id);
    else atomic_store(&first_event, action.event_id);
}

static void *producer(void *unused) {
    (void)unused;
    int ticks = 0, pending_ticks = 0;
    while (!atomic_load(&stop)) {
        if (atomic_load(&phase) == 1 && native_windows_action_pending()) {
            atomic_store(&boundary_waiting, true);
            if (++pending_ticks == 12) {
                receive_action(true);
                tui_composer_preserve_on_interrupt(true);
                tui_composer_signal_interrupt();
            }
        }
        if ((++ticks % 10) == 0 && atomic_load(&phase) == 1)
            pixel_tui_session_swarm_update(stderr, 91, "running", "Owned stream fixture", "fixture", (size_t)ticks, 0);
        status_write();
        struct timespec delay = {.tv_nsec = 20000000}; nanosleep(&delay, NULL);
    }
    return NULL;
}

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    status_path = argv[1];
    setenv("DSCO_PIXEL_TUI", "1", 1);
    setenv("DSCO_PIXEL_TUI_DPR", "1", 1);
    setenv("DSCO_PIXEL_TUI_ANIMATIONS", "0", 1);
    setenv("DSCO_TUI_ANIMATIONS", "0", 1);
    native_windows_reset();
    tui_composer_clear_retained_draft();
    tui_status_bar_t status;
    tui_status_bar_init(&status, "owned input fixture");
    status.animations_enabled = false;
    if (!pixel_tui_session_begin(stderr, "owned input fixture")) return 2;
    char result[4096];
    if (!native_windows_command("{\"action\":\"open\",\"kind\":\"workflow\",\"title\":\"Owned workflow\",\"text\":\"Draft preservation test\\nline2\\nline3\\nline4\\nline5\\nline6\\nline7\\nline8\\nline9\\nline10\\nline11\\nline12\\nline13\\nline14\\nline15\\nline16\\nline17\\nline18\\nline19\\nline20\",\"next_step\":\"Continue the owned fixture\",\"x\":40,\"y\":140,\"width\":420,\"height\":220}", result, sizeof(result))) return 2;
    if (!native_windows_command("{\"action\":\"open\",\"title\":\"Owned note\",\"text\":\"No model request\",\"x\":500,\"y\":140,\"width\":300,\"height\":180}", result, sizeof(result))) return 2;
    native_windows_key(NATIVE_WINDOW_KEY_ESCAPE, 0);
    pixel_tui_session_windows_changed(stderr);
    pthread_t worker;
    if (pthread_create(&worker, NULL, producer, NULL)) return 2;
    char input[512];
    tui_composer_set_action_wakeup(true);
    char *line = tui_composer_read(&status, NULL, input, sizeof(input));
    atomic_fetch_add(&returned, 1);
    if (!line || input[0]) atomic_fetch_add(&failures, 1);
    receive_action(false);
    tui_composer_set_action_wakeup(false);
    atomic_store(&phase, 1);
    line = tui_composer_read(&status, NULL, input, sizeof(input));
    atomic_fetch_add(&returned, 1);
    if (!line || input[0] || atomic_load(&actions) != 2) atomic_fetch_add(&failures, 1);
    tui_composer_preserve_on_interrupt(false);
    tui_composer_set_action_wakeup(true);
    atomic_store(&phase, 2);
    line = tui_composer_read(&status, NULL, input, sizeof(input));
    atomic_fetch_add(&returned, 1);
    if (!line || strcmp(input, "draft π-left!preserved\nsecond")) atomic_fetch_add(&failures, 1);
    if (line) snprintf(submitted, sizeof(submitted), "%s", input);
    atomic_store(&stop, true);
    pthread_join(worker, NULL);
    atomic_store(&phase, 3);
    status_write();
    pixel_tui_session_end(stderr);
    tui_composer_clear_retained_draft();
    tui_cleanup();
    pthread_mutex_destroy(&status.mutex);
    return atomic_load(&failures) ? 1 : 0;
}
