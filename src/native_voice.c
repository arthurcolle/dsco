#include "native_voice.h"
#include "native_voice_backend.h"

#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#define NATIVE_VOICE_QUEUE_CAP 8
#define NATIVE_VOICE_UTTERANCE_CAP 4096

typedef struct {
    pthread_mutex_t mutex;
    native_voice_state_t state;
    unsigned generation;
    char utterances[NATIVE_VOICE_QUEUE_CAP][NATIVE_VOICE_UTTERANCE_CAP];
    unsigned head;
    unsigned count;
} native_voice_manager_t;

static native_voice_manager_t s_voice = {
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .state = {
        .active = false,
        .revision = 1,
        .phase = "off",
        .detail = "Voice is off.",
        .partial = "",
    },
};

static void copy_text(char *dst, size_t cap, const char *src) {
    if (!dst || cap == 0)
        return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    size_t n = strlen(src);
    if (n >= cap) {
        n = cap - 1;
        /* Never leave a truncated UTF-8 codepoint at the end. */
        while (n > 0 && (((unsigned char)src[n] & 0xc0u) == 0x80u))
            n--;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static void clear_queue_locked(void) {
    s_voice.head = 0;
    s_voice.count = 0;
    memset(s_voice.utterances, 0, sizeof(s_voice.utterances));
}

static void set_state_locked(const char *phase, const char *detail) {
    char next_phase[sizeof(s_voice.state.phase)];
    char next_detail[sizeof(s_voice.state.detail)];
    copy_text(next_phase, sizeof(next_phase), phase);
    copy_text(next_detail, sizeof(next_detail), detail);
    if (strcmp(next_phase, s_voice.state.phase) == 0 &&
        strcmp(next_detail, s_voice.state.detail) == 0)
        return;
    memcpy(s_voice.state.phase, next_phase, sizeof(next_phase));
    memcpy(s_voice.state.detail, next_detail, sizeof(next_detail));
    s_voice.state.revision++;
}

static bool current_locked(unsigned generation) {
    return s_voice.state.active && generation == s_voice.generation;
}

static void backend_state(void *context, unsigned generation,
                          const char *phase, const char *detail) {
    (void)context;
    pthread_mutex_lock(&s_voice.mutex);
    if (current_locked(generation))
        set_state_locked(phase && phase[0] ? phase : "listening", detail);
    pthread_mutex_unlock(&s_voice.mutex);
}

static void backend_partial(void *context, unsigned generation, const char *text) {
    (void)context;
    pthread_mutex_lock(&s_voice.mutex);
    if (current_locked(generation)) {
        char next[sizeof(s_voice.state.partial)];
        copy_text(next, sizeof(next), text);
        if (strcmp(next, s_voice.state.partial) != 0) {
            memcpy(s_voice.state.partial, next, sizeof(next));
            set_state_locked("hearing", next[0] ? "Hearing speech..." : "Listening...");
        }
    }
    pthread_mutex_unlock(&s_voice.mutex);
}

static size_t clean_utterance(const char *src, char *dst, size_t cap, bool *too_long) {
    if (!src || !dst || cap == 0)
        return 0;
    if (too_long)
        *too_long = false;
    while (*src && isspace((unsigned char)*src))
        src++;
    size_t n = 0;
    bool pending_space = false;
    for (; *src; src++) {
        unsigned char c = (unsigned char)*src;
        if (isspace(c) || c < 0x20 || c == 0x7f) {
            pending_space = n > 0;
            continue;
        }
        if (pending_space) {
            if (n + 1 >= cap) {
                if (too_long) *too_long = true;
                break;
            }
            dst[n++] = ' ';
        }
        pending_space = false;
        if (n + 1 >= cap) {
            if (too_long) *too_long = true;
            break;
        }
        dst[n++] = (char)c;
    }
    while (n > 0 && isspace((unsigned char)dst[n - 1]))
        n--;
    dst[n] = '\0';
    return n;
}

static bool is_local_stop_command(const char *text) {
    if (!text)
        return false;
    char normalized[64];
    copy_text(normalized, sizeof(normalized), text);
    size_t n = strlen(normalized);
    while (n > 0 && strchr(".!?,;:", normalized[n - 1]))
        normalized[--n] = '\0';
    return strcasecmp(normalized, "stop voice") == 0 ||
           strcasecmp(normalized, "voice off") == 0 ||
           strcasecmp(normalized, "return to typing") == 0;
}

static void backend_final(void *context, unsigned generation, const char *text) {
    (void)context;
    char cleaned[NATIVE_VOICE_UTTERANCE_CAP];
    bool too_long = false;
    if (clean_utterance(text, cleaned, sizeof(cleaned), &too_long) == 0 && !too_long)
        return;

    bool stop_locally = false;
    pthread_mutex_lock(&s_voice.mutex);
    if (current_locked(generation)) {
        s_voice.state.partial[0] = '\0';
        if (too_long) {
            set_state_locked("error", "Voice utterance too long; direction not queued.");
        } else if (is_local_stop_command(cleaned)) {
            stop_locally = true;
            s_voice.state.active = false;
            s_voice.generation++; /* reject late recognizer callbacks immediately */
            clear_queue_locked();
            set_state_locked("off", "Voice stopped by spoken command.");
        } else {
            if (s_voice.count == NATIVE_VOICE_QUEUE_CAP) {
                set_state_locked("error", "Voice queue full; direction not queued.");
            } else {
                unsigned tail = (s_voice.head + s_voice.count) % NATIVE_VOICE_QUEUE_CAP;
                copy_text(s_voice.utterances[tail], sizeof(s_voice.utterances[tail]), cleaned);
                s_voice.count++;
                set_state_locked("listening", "Utterance ready.");
            }
        }
    }
    pthread_mutex_unlock(&s_voice.mutex);

    /* Backend stop is deliberately asynchronous, so this is safe from a
     * recognition callback running on the backend owner thread. */
    if (stop_locally)
        native_voice_backend_stop(generation);
}

static void backend_inactive(void *context, unsigned generation,
                             const char *phase, const char *detail) {
    (void)context;
    pthread_mutex_lock(&s_voice.mutex);
    if (generation == s_voice.generation && s_voice.state.active) {
        s_voice.state.active = false;
        s_voice.state.partial[0] = '\0';
        clear_queue_locked();
        (void)phase;
        set_state_locked("error", detail && detail[0] ? detail : "Native voice stopped unexpectedly.");
    }
    pthread_mutex_unlock(&s_voice.mutex);
}

static const native_voice_backend_callbacks_t s_callbacks = {
    .state = backend_state,
    .partial = backend_partial,
    .final_text = backend_final,
    .inactive = backend_inactive,
};

bool native_voice_start(void) {
    unsigned generation;
    pthread_mutex_lock(&s_voice.mutex);
    if (s_voice.state.active) {
        pthread_mutex_unlock(&s_voice.mutex);
        return true;
    }
    generation = ++s_voice.generation;
    clear_queue_locked();
    s_voice.state.active = true;
    s_voice.state.partial[0] = '\0';
    set_state_locked("starting", "Starting on-device speech recognition...");
    pthread_mutex_unlock(&s_voice.mutex);

    char error[256] = "Native voice backend is unavailable.";
    if (native_voice_backend_start(&s_callbacks, NULL, generation, error, sizeof(error))) {
        /* stop() can race the non-blocking platform launch after we publish
         * `starting`. Never leave a stale generation owning the microphone. */
        pthread_mutex_lock(&s_voice.mutex);
        bool current = current_locked(generation);
        pthread_mutex_unlock(&s_voice.mutex);
        if (current)
            return true;
        native_voice_backend_stop(generation);
        return false;
    }

    pthread_mutex_lock(&s_voice.mutex);
    if (generation == s_voice.generation) {
        s_voice.state.active = false;
        s_voice.state.partial[0] = '\0';
        clear_queue_locked();
        set_state_locked("error", error);
    }
    pthread_mutex_unlock(&s_voice.mutex);
    return false;
}

void native_voice_stop(void) {
    unsigned generation;
    pthread_mutex_lock(&s_voice.mutex);
    generation = s_voice.generation;
    s_voice.generation++;
    s_voice.state.active = false;
    s_voice.state.partial[0] = '\0';
    clear_queue_locked();
    set_state_locked("off", "Voice is off.");
    pthread_mutex_unlock(&s_voice.mutex);
    native_voice_backend_stop(generation);
}

bool native_voice_active(void) {
    pthread_mutex_lock(&s_voice.mutex);
    bool active = s_voice.state.active;
    pthread_mutex_unlock(&s_voice.mutex);
    return active;
}

void native_voice_snapshot(native_voice_state_t *out) {
    if (!out)
        return;
    pthread_mutex_lock(&s_voice.mutex);
    *out = s_voice.state;
    pthread_mutex_unlock(&s_voice.mutex);
}

bool native_voice_poll(char *out, size_t cap) {
    if (!out || cap == 0)
        return false;
    out[0] = '\0';
    pthread_mutex_lock(&s_voice.mutex);
    if (s_voice.count == 0) {
        pthread_mutex_unlock(&s_voice.mutex);
        return false;
    }
    const char *next = s_voice.utterances[s_voice.head];
    if (strlen(next) + 1 > cap) {
        set_state_locked("error", "Voice transcript exceeds the destination buffer.");
        pthread_mutex_unlock(&s_voice.mutex);
        return false;
    }
    memcpy(out, next, strlen(next) + 1);
    s_voice.utterances[s_voice.head][0] = '\0';
    s_voice.head = (s_voice.head + 1) % NATIVE_VOICE_QUEUE_CAP;
    s_voice.count--;
    s_voice.state.revision++;
    pthread_mutex_unlock(&s_voice.mutex);
    return true;
}

void native_voice_speak(const char *text) {
    if (!text || !text[0])
        return;
    pthread_mutex_lock(&s_voice.mutex);
    bool active = s_voice.state.active;
    unsigned generation = s_voice.generation;
    pthread_mutex_unlock(&s_voice.mutex);
    if (active)
        native_voice_backend_speak(text, generation);
}

void native_voice_interrupt(void) {
    pthread_mutex_lock(&s_voice.mutex);
    bool active = s_voice.state.active;
    unsigned generation = s_voice.generation;
    pthread_mutex_unlock(&s_voice.mutex);
    if (active)
        native_voice_backend_interrupt(generation);
}

#if !defined(__APPLE__)
/* Portable builds retain the API and fail closed. Weak definitions let unit
 * tests provide a deterministic backend without production injection knobs. */
__attribute__((weak))
bool native_voice_backend_start(const native_voice_backend_callbacks_t *callbacks,
                                void *context, unsigned generation,
                                char *error, size_t error_cap) {
    (void)callbacks;
    (void)context;
    (void)generation;
    copy_text(error, error_cap, "Native on-device voice requires macOS.");
    return false;
}

__attribute__((weak))
void native_voice_backend_stop(unsigned generation) {
    (void)generation;
}

__attribute__((weak))
void native_voice_backend_speak(const char *text, unsigned generation) {
    (void)text;
    (void)generation;
}

__attribute__((weak))
void native_voice_backend_interrupt(unsigned generation) {
    (void)generation;
}
#endif
