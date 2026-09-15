#!/usr/bin/env python3
"""Compile the real agent IPC boundary with a deterministic mailbox; no inference."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'src/agent.c').read_text()
start = source.index('            /* IPC receive acknowledges messages')
end = source.index('            /* §9: End-of-turn memory maintenance */', start)
block = source[start:end]
unit = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "json_util.h"
#define TUI_DIM ""
typedef struct { char *from_agent, *topic, *body; } ipc_message_t;
static int pending, acknowledged, applied, telemetry;
static char received[4096];
static void ipc_heartbeat(void) {}
static int ipc_poll(void) { return pending ? 1 : 0; }
static int ipc_recv(ipc_message_t *out, int max) {
    assert(max >= 2);
    int n = pending; pending = 0; acknowledged += n;
    for (int i = 0; i < n; i++) {
        out[i].from_agent = "peer"; out[i].topic = "steering";
        out[i].body = strdup(i ? "preserve original goal" : "prioritize validation");
    }
    return n;
}
static void conv_add_user_text(int *conv, const char *text) {
    (void)conv; applied++; snprintf(received, sizeof(received), "%s", text);
}
static void baseline_log(const char *category, const char *event,
                         const char *text, const char *meta) {
    (void)text; (void)meta;
    assert(!strcmp(category, "agent") && !strcmp(event, "ipc_messages_applied")); telemetry++;
}
static void boundary(bool *finished, bool *followup) {
    bool done = *finished, needs_followup_turn = *followup;
    int conv = 0;
''' + block + r'''
    *finished = done; *followup = needs_followup_turn;
}
int main(void) {
    /* Covers active work and an already-applied operator follow-up (done=false),
     * plus an otherwise finished response. Both must retain both peer messages. */
    for (int initial_done = 0; initial_done <= 1; initial_done++) {
        bool done = initial_done, followup = false;
        pending = 2; acknowledged = applied = telemetry = 0;
        boundary(&done, &followup);
        assert(acknowledged == 2 && applied == 1 && telemetry == 1);
        assert(!done && followup);
        assert(strstr(received, "prioritize validation") && strstr(received, "preserve original goal"));
        boundary(&done, &followup);
        assert(applied == 1 && telemetry == 1); /* no repeated injection */
    }
    bool done = true, followup = false;
    boundary(&done, &followup);
    assert(done && !followup); /* idle mailbox must not force another call */
    puts("PASS: IPC steering applies once at active and completed model boundaries");
}
'''
with tempfile.TemporaryDirectory(prefix='dsco-ipc-boundary-') as directory:
    path = Path(directory)
    (path / 'boundary.c').write_text(unit)
    subprocess.run(shlex.split(os.environ.get('CC', 'cc')) + [
        '-std=c11', '-D_DARWIN_C_SOURCE', '-D_POSIX_C_SOURCE=200809L',
        '-I', str(ROOT / 'include'), str(path / 'boundary.c'),
        str(ROOT / 'src/json_util.c'), '-lm', '-o', str(path / 'boundary')], check=True)
    subprocess.run([str(path / 'boundary')], check=True)
