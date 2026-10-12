#ifndef DSCO_NATIVE_VOICE_BACKEND_H
#define DSCO_NATIVE_VOICE_BACKEND_H

#include <stdbool.h>
#include <stddef.h>

/* Internal platform seam. Tests link native_voice.c against a deterministic
 * implementation of these functions; production links native_voice_macos.m. */
typedef struct {
    void (*state)(void *context, unsigned generation,
                  const char *phase, const char *detail);
    void (*partial)(void *context, unsigned generation, const char *text);
    void (*final_text)(void *context, unsigned generation, const char *text);
    void (*inactive)(void *context, unsigned generation,
                     const char *phase, const char *detail);
} native_voice_backend_callbacks_t;

bool native_voice_backend_start(const native_voice_backend_callbacks_t *callbacks,
                                void *context, unsigned generation,
                                char *error, size_t error_cap);
void native_voice_backend_stop(unsigned generation);
void native_voice_backend_speak(const char *text, unsigned generation);
void native_voice_backend_interrupt(unsigned generation);

#endif
