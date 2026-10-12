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

typedef enum {
    REALTIME_STATUS, REALTIME_READY, REALTIME_USER_TEXT,
    REALTIME_ASSISTANT_DELTA, REALTIME_ASSISTANT_END,
    REALTIME_TOOL_CALL, REALTIME_TOOL_RESULT, REALTIME_RESPONSE_DONE,
    REALTIME_ERROR, REALTIME_MIC_STATE
} realtime_event_type_t;

/* Events and polling run on the session thread, never an audio/tool worker.
 * Strings are borrowed for the duration of the callback. */
typedef struct {
    realtime_event_type_t type;
    const char *text, *name, *id, *arguments;
} realtime_event_t;
typedef void (*realtime_event_fn)(const realtime_event_t *event, void *ctx);
typedef bool (*realtime_stop_fn)(void *ctx, bool tools_busy);

typedef struct {
    const char *model;        /* NULL → DSCO_REALTIME_MODEL env or built-in default */
    const char *voice;        /* NULL → "marin" */
    const char *instructions; /* NULL → built-in DSCO voice prompt */
    const char *vad;          /* "semantic_vad" | "server_vad" (default) */
    bool        text_only;    /* no audio: type turns on stdin, text responses */
    bool        no_tools;     /* don't expose the tool registry to the session */
    bool        stdin_pcm;    /* raw signed mono PCM16@24kHz from stdin; JSONL text on stdout */
    bool        half_duplex;  /* mute mic while assistant audio plays (speaker-safe default);
                               * full-duplex/barge-in is for headphones. */
    bool        embedded;     /* caller owns terminal, tool initialization and presentation */
    const char *trust_tier;   /* embedded tool calls retain the current session's tier */
    const char *initial_context; /* bounded text history, sent as a user context item */
    realtime_event_fn on_event;
    realtime_stop_fn should_stop;
    void       *callback_ctx;
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
