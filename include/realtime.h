/* realtime.h — OpenAI Realtime API (GA) voice sessions over WebSocket.
 *
 * Speech-to-speech with realtime tool calling: streams mic PCM16@24kHz up,
 * plays model audio down, and bridges response.function_call_arguments.done
 * events into the local tool registry (tools_execute), returning results on
 * the same socket. WebSocket transport is hand-rolled RFC 6455 over mbedTLS
 * (HAVE_MBEDTLS); audio I/O uses AudioToolbox AudioQueues (Darwin). */
#ifndef DSCO_REALTIME_H
#define DSCO_REALTIME_H

#include <stdbool.h>

#define DSCO_REALTIME_TOOL_NAME_MAX 128

typedef struct {
    const char *model;        /* NULL → DSCO_REALTIME_MODEL env or built-in default */
    const char *voice;        /* NULL → "marin" */
    const char *instructions; /* NULL → built-in DSCO voice prompt */
    const char *vad;          /* "semantic_vad" (default) | "server_vad" */
    bool        text_only;    /* no audio: type turns on stdin, text responses */
    bool        no_tools;     /* don't expose the tool registry to the session */
    bool        half_duplex;  /* mute mic while assistant audio plays (speaker-safe default);
                               * full-duplex/barge-in is for headphones. */
} realtime_opts_t;

/* Blocking session loop; returns a process exit code. */
int realtime_voice_run(const realtime_opts_t *opts);

/* Built-in reasoning effort used by GPT Realtime models unless overridden. */
const char *realtime_default_reasoning_effort(void);

/* Default number of DSCO tools exposed to realtime sessions. This follows the
 * same register-file cap used by the baseline text agent. */
int realtime_voice_default_max_tools(void);

/* Test/diagnostic helper: return the ordered built-in tool names that a voice
 * session would expose for `context`, before loaded external tools are appended. */
int realtime_voice_select_tool_names_for_context(
    const char *context,
    char names[][DSCO_REALTIME_TOOL_NAME_MAX],
    int max_tools);

/* `dsco voice …` entry point. */
int realtime_voice_cli(int argc, char **argv);

#endif
