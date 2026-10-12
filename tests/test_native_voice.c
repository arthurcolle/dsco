#include "native_voice.h"
#include "native_voice_backend.h"

#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static native_voice_backend_callbacks_t observed_callbacks;
static void *observed_context;
static unsigned observed_generation;
static unsigned stopped_generation;
static unsigned spoken_generation;
static unsigned interrupted_generation;
static int starts;
static int stops;
static int speaks;
static int interrupts;
static bool allow_start = true;
static bool stop_during_start;
static const char *start_error = "Microphone permission denied by fixture.";
static char spoken_text[256];
static pthread_mutex_t start_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t start_cond = PTHREAD_COND_INITIALIZER;
static bool block_start;
static bool start_entered;
static bool release_start;

bool native_voice_backend_start(const native_voice_backend_callbacks_t *callbacks,
                                void *context, unsigned generation,
                                char *error, size_t error_cap) {
    starts++;
    observed_callbacks = *callbacks;
    observed_context = context;
    observed_generation = generation;
    pthread_mutex_lock(&start_mutex);
    if (block_start) {
        start_entered = true;
        pthread_cond_broadcast(&start_cond);
        while (!release_start)
            pthread_cond_wait(&start_cond, &start_mutex);
    }
    pthread_mutex_unlock(&start_mutex);
    if (!allow_start) {
        snprintf(error, error_cap, "%s", start_error);
        return false;
    }
    if (stop_during_start) native_voice_stop();
    return true;
}

void native_voice_backend_stop(unsigned generation) {
    stops++;
    stopped_generation = generation;
}

void native_voice_backend_speak(const char *text, unsigned generation) {
    speaks++;
    spoken_generation = generation;
    snprintf(spoken_text, sizeof(spoken_text), "%s", text ? text : "");
}

void native_voice_backend_interrupt(unsigned generation) {
    interrupts++;
    interrupted_generation = generation;
}

static void emit_state(unsigned generation, const char *phase, const char *detail) {
    observed_callbacks.state(observed_context, generation, phase, detail);
}

static void emit_partial(unsigned generation, const char *text) {
    observed_callbacks.partial(observed_context, generation, text);
}

static void emit_final(unsigned generation, const char *text) {
    observed_callbacks.final_text(observed_context, generation, text);
}

static void emit_inactive(unsigned generation, const char *phase, const char *detail) {
    observed_callbacks.inactive(observed_context, generation, phase, detail);
}

static native_voice_state_t snapshot(void) {
    native_voice_state_t state;
    memset(&state, 0, sizeof(state));
    native_voice_snapshot(&state);
    return state;
}

static void expect_no_prompt(void) {
    char text[4096] = "sentinel";
    assert(!native_voice_poll(text, sizeof(text)));
    assert(text[0] == '\0');
}

static void reset_voice(void) {
    native_voice_stop();
    memset(&observed_callbacks, 0, sizeof(observed_callbacks));
    observed_context = NULL;
    observed_generation = 0;
    stopped_generation = 0;
    spoken_generation = 0;
    interrupted_generation = 0;
    starts = stops = speaks = interrupts = 0;
    allow_start = true;
    stop_during_start = false;
    start_error = "Microphone permission denied by fixture.";
    spoken_text[0] = '\0';
    pthread_mutex_lock(&start_mutex);
    block_start = start_entered = release_start = false;
    pthread_mutex_unlock(&start_mutex);
}

static void test_state_transcript_and_forwarding(void) {
    reset_voice();
    native_voice_state_t before = snapshot();
    assert(!before.active && strcmp(before.phase, "off") == 0);

    assert(native_voice_start());
    assert(starts == 1 && observed_generation != 0 && native_voice_active());
    unsigned generation = observed_generation;
    native_voice_state_t starting = snapshot();
    assert(starting.active && strcmp(starting.phase, "starting") == 0);
    assert(starting.revision > before.revision);

    emit_state(generation, "listening", "Speak now.");
    native_voice_state_t listening = snapshot();
    emit_state(generation, "listening", "Speak now.");
    assert(snapshot().revision == listening.revision);
    emit_partial(generation, "draft speech");
    native_voice_state_t hearing = snapshot();
    assert(hearing.active && strcmp(hearing.phase, "hearing") == 0);
    assert(strcmp(hearing.partial, "draft speech") == 0);
    emit_partial(generation, "draft speech");
    assert(snapshot().revision == hearing.revision);

    emit_final(generation, "  spoken\twork\nrequest ");
    native_voice_state_t ready = snapshot();
    assert(ready.active && strcmp(ready.phase, "listening") == 0);
    assert(ready.partial[0] == '\0');
    char text[4096];
    assert(native_voice_poll(text, sizeof(text)));
    assert(strcmp(text, "spoken work request") == 0);
    expect_no_prompt();

    native_voice_speak("assistant answer");
    native_voice_interrupt();
    assert(speaks == 1 && spoken_generation == generation);
    assert(strcmp(spoken_text, "assistant answer") == 0);
    assert(interrupts == 1 && interrupted_generation == generation);

    native_voice_stop();
    assert(!native_voice_active() && stops == 1 && stopped_generation == generation);
    native_voice_speak("must not escape");
    native_voice_interrupt();
    assert(speaks == 1 && interrupts == 1);
}

static void test_bounded_fifo_and_small_destination(void) {
    reset_voice();
    assert(native_voice_start());
    unsigned generation = observed_generation;
    for (int i = 0; i < 9; i++) {
        char text[32];
        snprintf(text, sizeof(text), "utterance-%d", i);
        emit_final(generation, text);
    }

    char text[64];
    native_voice_state_t full = snapshot();
    assert(strcmp(full.phase, "error") == 0);
    assert(strstr(full.detail, "queue full"));
    for (int i = 0; i < 8; i++) {
        assert(native_voice_poll(text, sizeof(text)));
        char expected[32];
        snprintf(expected, sizeof(expected), "utterance-%d", i);
        assert(strcmp(text, expected) == 0);
    }
    expect_no_prompt();

    emit_final(generation, "a transcript longer than this tiny destination");
    char tiny[8];
    assert(!native_voice_poll(tiny, sizeof(tiny)));
    native_voice_state_t error = snapshot();
    assert(strcmp(error.phase, "error") == 0);
    assert(strstr(error.detail, "destination buffer"));
    assert(native_voice_poll(text, sizeof(text)));
    assert(strcmp(text, "a transcript longer than this tiny destination") == 0);

    char overlong[5000];
    memset(overlong, 'x', sizeof(overlong) - 1);
    overlong[sizeof(overlong) - 1] = '\0';
    emit_final(generation, overlong);
    native_voice_state_t too_long = snapshot();
    assert(strcmp(too_long.phase, "error") == 0);
    assert(strstr(too_long.detail, "too long"));
    expect_no_prompt();
    native_voice_stop();
}

static void test_local_stop_commands_and_generation_fence(void) {
    static const char *commands[] = {
        " Stop\tVoice! ", "VOICE OFF...", "Return To Typing?",
    };
    for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
        reset_voice();
        assert(native_voice_start());
        unsigned old_generation = observed_generation;
        emit_final(old_generation, "pending ordinary prompt");
        emit_final(old_generation, commands[i]);

        native_voice_state_t off = snapshot();
        assert(!off.active && strcmp(off.phase, "off") == 0);
        assert(strstr(off.detail, "spoken command"));
        assert(stops == 1 && stopped_generation == old_generation);
        expect_no_prompt();

        unsigned off_revision = off.revision;
        emit_state(old_generation, "listening", "stale state");
        emit_partial(old_generation, "stale partial");
        emit_final(old_generation, "stale final");
        emit_inactive(old_generation, "denied", "stale inactive");
        native_voice_state_t after_stale = snapshot();
        assert(!after_stale.active && after_stale.revision == off_revision);
        assert(after_stale.partial[0] == '\0');
        expect_no_prompt();

        assert(native_voice_start());
        unsigned new_generation = observed_generation;
        assert(new_generation != old_generation);
        emit_final(old_generation, "old generation must remain fenced");
        emit_final(new_generation, "fresh generation accepted");
        char text[128];
        assert(native_voice_poll(text, sizeof(text)));
        assert(strcmp(text, "fresh generation accepted") == 0);
        expect_no_prompt();
        native_voice_stop();
    }
}

static void test_unavailable_and_permission_recovery(void) {
    reset_voice();
    allow_start = false;
    assert(!native_voice_start());
    native_voice_state_t denied = snapshot();
    assert(!denied.active && strcmp(denied.phase, "error") == 0);
    assert(strcmp(denied.detail, "Microphone permission denied by fixture.") == 0);
    native_voice_speak("not active");
    native_voice_interrupt();
    assert(speaks == 0 && interrupts == 0);

    allow_start = true;
    assert(native_voice_start());
    unsigned generation = observed_generation;
    assert(starts == 2 && native_voice_active());
    emit_state(generation, "listening", "Recovered after permission change.");
    native_voice_state_t recovered = snapshot();
    assert(recovered.active && strcmp(recovered.phase, "listening") == 0);
    assert(strstr(recovered.detail, "Recovered"));

    emit_final(generation, "queued prompt must be cleared by backend failure");
    emit_inactive(generation, "unavailable", "Recognizer became unavailable.");
    native_voice_state_t inactive = snapshot();
    assert(!inactive.active && strcmp(inactive.phase, "error") == 0);
    assert(strstr(inactive.detail, "became unavailable"));
    expect_no_prompt();

    assert(native_voice_start());
    assert(observed_generation != generation && starts == 3);
    native_voice_stop();
}

static void *start_voice_thread(void *result) {
    *(bool *)result = native_voice_start();
    return NULL;
}

static void test_stop_wins_start_race(void) {
    reset_voice();
    pthread_mutex_lock(&start_mutex);
    block_start = true;
    pthread_mutex_unlock(&start_mutex);

    bool start_result = true;
    pthread_t thread;
    assert(pthread_create(&thread, NULL, start_voice_thread, &start_result) == 0);
    pthread_mutex_lock(&start_mutex);
    while (!start_entered)
        pthread_cond_wait(&start_cond, &start_mutex);
    unsigned stale_generation = observed_generation;
    pthread_mutex_unlock(&start_mutex);

    native_voice_stop();
    assert(!native_voice_active());
    assert(stops >= 1 && stopped_generation == stale_generation);

    pthread_mutex_lock(&start_mutex);
    release_start = true;
    pthread_cond_broadcast(&start_cond);
    pthread_mutex_unlock(&start_mutex);
    assert(pthread_join(thread, NULL) == 0);

    assert(!start_result);
    assert(!native_voice_active());
    assert(stops >= 2 && stopped_generation == stale_generation);
    emit_state(stale_generation, "listening", "late startup callback");
    emit_final(stale_generation, "late startup prompt");
    native_voice_state_t off = snapshot();
    assert(!off.active && strcmp(off.phase, "off") == 0);
    expect_no_prompt();
}

static void test_stop_during_platform_launch(void) {
    reset_voice();
    stop_during_start = true;
    assert(!native_voice_start());
    assert(!native_voice_active());
    assert(stops == 2); /* early stop plus post-launch stale-owner cleanup */
    emit_final(observed_generation, "must never launch stale work");
    expect_no_prompt();
}

int main(void) {
    test_stop_during_platform_launch();
    test_state_transcript_and_forwarding();
    test_bounded_fifo_and_small_destination();
    test_local_stop_commands_and_generation_fence();
    test_unavailable_and_permission_recovery();
    test_stop_wins_start_race();
    puts("PASS: native voice state, FIFO, local stop, generation fence, TTS, recovery and start/stop race");
    return 0;
}
