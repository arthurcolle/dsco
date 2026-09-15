#ifndef DSCO_PTY_SESSION_H
#define DSCO_PTY_SESSION_H

#include <stdbool.h>
#include <stddef.h>

/* Sessions persist across tool calls in one DSCO process. They are not resumed
 * after exec/restart and are not inherited by forked workers. Normal shutdown
 * terminates the process groups owned by this manager. A PTY is an execution
 * surface, not a sandbox; programs can deliberately detach from their group. */
#define PTY_SESSION_DESCRIPTION \
    "Manage persistent, isolated terminal processes owned by this DSCO process. " \
    "Spawn an executable with direct argv (no implicit shell), then read, write, " \
    "resize, wait, inspect or close by opaque session_id. Output has byte offsets, " \
    "UTF-8 text and lossless base64; a bounded ring reports discarded history. " \
    "read without offset advances a shared cursor; explicit offsets are replayable. " \
    "wait timeout is observational; ttl_seconds and close terminate the owned " \
    "process group. Sessions do not survive DSCO restart."

#define PTY_SESSION_SCHEMA \
    "{\"type\":\"object\",\"properties\":{" \
    "\"action\":{\"type\":\"string\",\"enum\":[\"spawn\",\"list\",\"status\",\"read\",\"write\",\"resize\",\"wait\",\"close\"]}," \
    "\"session_id\":{\"type\":\"string\"}," \
    "\"surface_id\":{\"type\":\"string\",\"description\":\"Returned pty: surface ID, accepted instead of session_id.\"}," \
    "\"command\":{\"type\":\"string\",\"description\":\"Executable path or PATH name; a shell must be requested explicitly.\"}," \
    "\"args\":{\"type\":\"array\",\"maxItems\":64,\"items\":{\"type\":\"string\"}}," \
    "\"cwd\":{\"type\":\"string\"}," \
    "\"term\":{\"type\":\"string\",\"description\":\"TERM for a new session; default xterm-256color.\"}," \
    "\"input\":{\"type\":\"string\",\"description\":\"At most 64 KiB UTF-8; JSON escapes may carry control keys (no NUL).\"}," \
    "\"cols\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":1000}," \
    "\"rows\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":1000}," \
    "\"offset\":{\"type\":\"integer\",\"minimum\":0}," \
    "\"max_bytes\":{\"type\":\"integer\",\"minimum\":4,\"maximum\":65536}," \
    "\"timeout_ms\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":30000,\"description\":\"Read/write/wait deadline; wait defaults to 1000 ms.\"}," \
    "\"ttl_seconds\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":86400,\"description\":\"Spawn lifetime, default 1800 seconds.\"}" \
    "},\"required\":[\"action\"]}"

bool tool_pty_session(const char *input_json, char *result, size_t result_len);
void pty_sessions_shutdown(void);

#endif
