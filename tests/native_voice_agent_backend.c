/* Link-time replacement for microphone/speaker hardware. NEVER linked into
 * dsco: only build/dsco_native_voice_fixture accepts these private pipe FDs. */
#define _POSIX_C_SOURCE 200809L
#include "native_voice_backend.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static native_voice_backend_callbacks_t callbacks;
static void *context;
static unsigned generation;
static bool active, started;
static int input_fd = -1, report_fd = -1;

static void report(const char *kind, const char *text) {
    char line[4096];
    int n = snprintf(line, sizeof(line), "%s %s\n", kind, text ? text : "");
    if (report_fd >= 0 && n > 0)
        (void)write(report_fd, line, (size_t)n < sizeof(line) ? (size_t)n : sizeof(line) - 1);
}

static void dispatch(char *line) {
    pthread_mutex_lock(&lock);
    native_voice_backend_callbacks_t cb = callbacks;
    void *ctx = context;
    unsigned gen = generation;
    bool live = active;
    pthread_mutex_unlock(&lock);
    if (!live) return;
    if (!strncmp(line, "FINAL ", 6)) cb.final_text(ctx, gen, line + 6);
    else if (!strncmp(line, "PARTIAL ", 8)) cb.partial(ctx, gen, line + 8);
    else if (!strncmp(line, "ERROR ", 6)) cb.inactive(ctx, gen, "error", line + 6);
    report("DELIVERED", line);
}

static void *reader(void *unused) {
    (void)unused;
    char line[4096];
    size_t used = 0;
    for (;;) {
        char c;
        if (read(input_fd, &c, 1) != 1) break;
        if (c == '\n') { line[used] = '\0'; dispatch(line); used = 0; }
        else if (used + 1 < sizeof(line)) line[used++] = c;
    }
    return NULL;
}

bool native_voice_backend_start(const native_voice_backend_callbacks_t *cb,
                                void *ctx, unsigned gen, char *error, size_t cap) {
    pthread_mutex_lock(&lock);
    callbacks = *cb; context = ctx; generation = gen; active = true;
    if (!started) {
        const char *input = getenv("DSCO_NATIVE_VOICE_TEST_IN_FD");
        const char *output = getenv("DSCO_NATIVE_VOICE_TEST_REPORT_FD");
        input_fd = input ? atoi(input) : -1;
        report_fd = output ? atoi(output) : -1;
        pthread_t thread;
        if (input_fd < 0 || report_fd < 0 || pthread_create(&thread, NULL, reader, NULL)) {
            active = false;
            snprintf(error, cap, "test fixture pipes unavailable");
            pthread_mutex_unlock(&lock);
            return false;
        }
        pthread_detach(thread); started = true;
    }
    pthread_mutex_unlock(&lock);
    cb->state(ctx, gen, "listening", "Local deterministic native speech fixture");
    report("READY", "");
    return true;
}

void native_voice_backend_stop(unsigned gen) {
    pthread_mutex_lock(&lock);
    if (generation == gen) active = false;
    pthread_mutex_unlock(&lock);
    report("OFF", "");
}

void native_voice_backend_speak(const char *text, unsigned gen) {
    pthread_mutex_lock(&lock);
    bool live = active && generation == gen;
    pthread_mutex_unlock(&lock);
    if (live) report("SPEAK", text);
}

void native_voice_backend_interrupt(unsigned gen) {
    (void)gen;
    report("INTERRUPT", "");
}
