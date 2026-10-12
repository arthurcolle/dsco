/* Native voice controller plus the real ANSI/native composer under an owned PTY. */
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE
#endif

#include "native_voice.h"
#include "native_voice_backend.h"
#include "native_composer.h"
#include "pixel_tui.h"
#include "tui.h"
#include "vm.h"

#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int g_cheap_mode;
vm_t g_vm;
volatile int g_interrupted;
double g_cost_budget;

static native_voice_backend_callbacks_t backend_callbacks;
static void *backend_context;
static unsigned backend_generation;
static pthread_t emitter_thread;
static bool emitter_started;
static const char *scenario;
static char spoken_text[256];
static unsigned spoken_generation;
static FILE *receipt;

static const native_ui_node_t *find_node(const native_ui_scene_t *scene, uint64_t key) {
    for (int i = 0; scene && i < scene->count; i++)
        if (scene->nodes[i].key == key)
            return &scene->nodes[i];
    return NULL;
}

static void verify_voice_button_semantics(void) {
    native_ui_scene_t scene;
    native_composer_model_t model = {
        .text = "preserved draft",
        .cursor = 4,
        .active = true,
        .columns = 60,
        .max_rows = NATIVE_UI_COMPOSER_MAX_ROWS,
    };
    assert(native_composer_build(&scene, 800, 84, &model));
    assert(scene.count == 14);
    const native_ui_node_t *button = find_node(&scene, NATIVE_COMPOSER_KEY_VOICE);
    assert(button && button->element == NATIVE_UI_ELEMENT_BADGE);
    assert(button->role == NATIVE_UI_ROLE_NOTIFICATION);
    assert(button->state & NATIVE_UI_STATE_LIVE);
    assert(strcmp(button->text, "Voice off") == 0);
    assert(strstr(button->accessibility_label, "Turn on local native voice"));

    model.voice_active = true;
    model.voice_phase = "listening";
    model.voice_partial = "partial words are presentation only";
    assert(native_composer_build(&scene, 800, 84, &model));
    assert(scene.count == 14);
    button = find_node(&scene, NATIVE_COMPOSER_KEY_VOICE);
    const native_ui_node_t *title = find_node(&scene, NATIVE_COMPOSER_KEY_TITLE);
    const native_ui_node_t *hint = find_node(&scene, NATIVE_COMPOSER_KEY_HINT);
    assert(button && strcmp(button->text, "Voice on") == 0);
    assert(strstr(button->accessibility_label, "Turn off native microphone"));
    assert(title && strcmp(title->text, "Voice · listening") == 0);
    assert(hint && strcmp(hint->text, "partial words are presentation only") == 0);
}

static void *emit_voice(void *unused) {
    (void)unused;
    usleep(strcmp(scenario, "toggle") == 0 ? 500000 : 120000);
    backend_callbacks.state(backend_context, backend_generation,
                            "listening", "On-device fixture is listening.");
    backend_callbacks.partial(backend_context, backend_generation, "spoken work");
    usleep(40000);
    backend_callbacks.final_text(
        backend_context, backend_generation,
        strcmp(scenario, "stop") == 0 ? "VOICE OFF!" :
        strcmp(scenario, "toggle") == 0 ? "stale prompt after toggle" :
        "spoken work request π");
    if (strcmp(scenario, "stop") == 0) {
        fputs("VOICE_LOCAL_STOP\n", receipt);
        fflush(receipt);
    }
    return NULL;
}

bool native_voice_backend_start(const native_voice_backend_callbacks_t *callbacks,
                                void *context, unsigned generation,
                                char *error, size_t error_cap) {
    backend_callbacks = *callbacks;
    backend_context = context;
    backend_generation = generation;
    if (strcmp(scenario, "unavailable") == 0 || strcmp(scenario, "denied") == 0) {
        snprintf(error, error_cap, "%s",
                 strcmp(scenario, "denied") == 0
                     ? "Microphone permission denied by fixture."
                     : "On-device speech recognition is unavailable in fixture.");
        fprintf(receipt, "VOICE_ERROR:%s\n", error);
        fflush(receipt);
        return false;
    }
    if (strcmp(scenario, "toggle") == 0) {
        fputs("VOICE_STARTED\n", receipt);
        fflush(receipt);
    }
    assert(pthread_create(&emitter_thread, NULL, emit_voice, NULL) == 0);
    emitter_started = true;
    return true;
}

void native_voice_backend_stop(unsigned generation) {
    (void)generation;
    if (scenario && strcmp(scenario, "toggle") == 0 && receipt) {
        fputs("VOICE_TOGGLED_OFF\n", receipt);
        fflush(receipt);
    }
}

void native_voice_backend_speak(const char *text, unsigned generation) {
    snprintf(spoken_text, sizeof(spoken_text), "%s", text ? text : "");
    spoken_generation = generation;
}

void native_voice_backend_interrupt(unsigned generation) {
    (void)generation;
}

int main(int argc, char **argv) {
    if (argc != 4)
        return 2;
    bool native = strcmp(argv[1], "native") == 0;
    scenario = argv[3];
    verify_voice_button_semantics();
    receipt = fopen(argv[2], "w");
    assert(receipt);

    setenv("DSCO_PIXEL_TUI", native ? "1" : "0", 1);
    setenv("DSCO_PIXEL_TUI_DPR", "1", 1);
    setenv("DSCO_PIXEL_TUI_ANIMATIONS", "0", 1);
    setenv("DSCO_TUI_ANIMATIONS", "0", 1);
    tui_status_bar_t status;
    tui_status_bar_init(&status, "native voice fixture");
    if (native)
        assert(pixel_tui_session_begin(stderr, "native voice fixture"));

    char line[256];
    assert(tui_composer_read(&status, NULL, line, sizeof(line)));

    if (strcmp(scenario, "final") == 0) {
        /* The real composer polls the controller and returns only the complete
         * utterance, retaining the typed draft for its next invocation. */
        assert(strcmp(line, "spoken work request π") == 0);
        assert(native_voice_active());
        native_voice_speak("assistant reply from same session");
        assert(spoken_generation == backend_generation);
        assert(strcmp(spoken_text, "assistant reply from same session") == 0);
        fprintf(receipt, "VOICE_PROMPT:%s\n", line);
        fflush(receipt);
        native_voice_stop();
        assert(tui_composer_read(&status, NULL, line, sizeof(line)));
    } else {
        /* Spoken stop, keyboard toggle, and startup errors remain local. The
         * first user line returned is the preserved typed draft. */
        assert(strcmp(line, "draft π! preserved") == 0);
        char unexpected[256];
        assert(!native_voice_poll(unexpected, sizeof(unexpected)));
    }
    fprintf(receipt, "DRAFT:%s\n", line);
    fflush(receipt);

    if (emitter_started)
        pthread_join(emitter_thread, NULL);
    if (native)
        pixel_tui_session_end(stderr);
    native_voice_stop();
    tui_cleanup();
    tui_composer_clear_retained_draft();
    pthread_mutex_destroy(&status.mutex);
    fclose(receipt);
    return 0;
}
