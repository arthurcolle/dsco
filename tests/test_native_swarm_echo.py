#!/usr/bin/env python3
"""Compile the actual static presentation callbacks with a small swarm fixture.
No providers, tool dispatch, windows, or changes to worker storage are involved.
"""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / "src/tools.c").read_text()

def function(name):
    start = source.index(name + "(")
    start = source.rfind("\n", 0, start) + 1
    end = source.index("\n}", start) + 2
    return source[start:end]

preamble = r'''
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>
#define SWARM_PROGRESS_GUARD ((void)0)
#define TUI_DIM ""
#define TUI_RESET ""
#define TUI_CYAN ""
typedef struct { int group_id; char stream_buf[4096]; size_t stream_buf_len; } swarm_child_t;
typedef struct { swarm_child_t child; } swarm_t;
static swarm_t g_swarm;
static bool native_active;
static bool pixel_tui_session_active(void) { return native_active; }
static bool tui_swarm_dock_visible(void) { return false; }
static swarm_child_t *swarm_get(swarm_t *s, int id) { return id == 0 ? &s->child : NULL; }
typedef struct { int group_id; swarm_t *swarm; } swarm_live_ctx_t;
'''
main = r'''
int main(void) {
    FILE *capture = tmpfile();
    assert(capture);
    int saved = dup(STDERR_FILENO);
    assert(saved >= 0);
    assert(dup2(fileno(capture), STDERR_FILENO) >= 0);
    swarm_live_ctx_t ctx = {.group_id=0, .swarm=&g_swarm};
    const char *line = "usage: input=8420 output=3131; autonomy active\n";
    native_active = true;
    default_swarm_stream_cb(0, line, strlen(line), NULL);
    swarm_live_stream_cb(0, line, strlen(line), &ctx);
    swarm_live_print_line(0, "tail", 4);
    fflush(stderr);
    assert(ftell(capture) == 0); /* captured stderr is NOT a tty */
    native_active = false;
    default_swarm_stream_cb(0, "default \xe2", 9, NULL);
    default_swarm_stream_cb(0, "\x86\x92\n", 3, NULL);
    swarm_live_stream_cb(0, "live\n", 5, &ctx);
    fflush(stderr);
    assert(ftell(capture) > 0);
    rewind(capture);
    char text[512] = {0};
    assert(fread(text, 1, sizeof(text)-1, capture) > 0);
    assert(strstr(text, "default \xe2\x86\x92"));
    assert(strstr(text, "live"));
    assert(!strstr(text, "usage:"));
    assert(!strstr(text, "tail"));
    assert(dup2(saved, STDERR_FILENO) >= 0);
    close(saved);
    fclose(capture);
    puts("native swarm echo: native silent; terminal output and split UTF-8 preserved");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="dsco-native-echo-") as tmp:
    path = Path(tmp)
    fixture = path / "echo.c"
    fixture.write_text(preamble + "\n" + "\n".join(function(n) for n in (
        "utf8_safe_prefix_len", "default_swarm_stream_cb", "swarm_live_print_line",
        "swarm_live_stream_cb")) + "\n" + main)
    binary = path / "echo"
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-D_DARWIN_C_SOURCE",
                    "-D_POSIX_C_SOURCE=200809L", "-Wall", "-Wextra", "-Werror",
                    str(fixture), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
