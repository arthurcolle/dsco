#ifndef DSCO_NATIVE_VOICE_H
#define DSCO_NATIVE_VOICE_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    bool active;
    unsigned revision;
    char phase[48];
    char detail[256];
    char partial[1024];
} native_voice_state_t;

/* Process-local, non-blocking native voice input and speech output. */
bool native_voice_start(void);
void native_voice_stop(void);
bool native_voice_active(void);
void native_voice_snapshot(native_voice_state_t *out);
bool native_voice_poll(char *out, size_t cap);
void native_voice_speak(const char *text);
void native_voice_interrupt(void);

#endif
