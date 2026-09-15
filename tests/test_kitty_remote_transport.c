/* Headless transport contract: never spawns Kitty or opens a terminal. */
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "process_capture.h"
static int test_access(const char *path, int mode) { (void)path; (void)mode; return 0; }
#define access test_access
#include "../src/kitty_tools.c"
#undef access
static int spawned;
static char seen_address[256];
bool process_capture_json(const char *path, char *const argv[], int timeout_ms,
                          const char *label, char *out, size_t cap) {
    (void)path;
    assert(timeout_ms >= 1000 && timeout_ms <= 300000);
    assert(!strcmp(label, "get-text"));
    assert(!strcmp(argv[1], "@") && !strcmp(argv[2], "--to"));
    snprintf(seen_address, sizeof(seen_address), "%s", argv[3]);
    assert(!strcmp(argv[4], "get-text"));
    assert(!strcmp(argv[5], "--match") && !strcmp(argv[6], "id:1") && !argv[7]);
    spawned++;
    snprintf(out, cap, "{\"ok\":true}");
    return true;
}
int main(void) {
    char out[1024];
    const char *request = "{\"command\":\"get-text\",\"args\":[\"--match\",\"id:1\"]}";
    unsetenv("KITTY_LISTEN_ON");
    assert(!tool_kitty_remote(request, out, sizeof(out)));
    assert(strstr(out, "kitty_socket_required") && spawned == 0);
    setenv("KITTY_LISTEN_ON", "", 1);
    assert(!tool_kitty_remote(request, out, sizeof(out)) && spawned == 0);
    setenv("KITTY_LISTEN_ON", "unix:/tmp/owned-fixture", 1);
    assert(tool_kitty_remote(request, out, sizeof(out)) && spawned == 1);
    assert(!strcmp(seen_address, "unix:/tmp/owned-fixture"));
    assert(tool_kitty_remote("{\"command\":\"get-text\",\"to\":\"tcp:localhost:1234\",\"args\":[\"--match\",\"id:1\"]}", out, sizeof(out)));
    assert(spawned == 2 && !strcmp(seen_address, "tcp:localhost:1234"));
    assert(tool_kitty_remote("{\"command\":\"get-text\",\"to\":\"\",\"args\":[\"--match\",\"id:1\"]}", out, sizeof(out)));
    assert(spawned == 3 && !strcmp(seen_address, "unix:/tmp/owned-fixture"));
    const char *bad[] = {"-", "/dev/tty", "unix:", "tcp:", "tcp6:", "unix:/tmp/test\n"};
    for (size_t i=0; i<sizeof(bad)/sizeof(*bad); i++) {
        setenv("KITTY_LISTEN_ON", bad[i], 1);
        assert(!tool_kitty_remote(request, out, sizeof(out)));
        assert(strstr(out, "invalid_kitty_socket") && spawned == 3);
    }
    unsetenv("KITTY_LISTEN_ON");
    assert(!tool_kitty_remote("{\"command\":\"get-text\",\"to\":\"/dev/tty\"}", out, sizeof(out)));
    assert(strstr(out, "invalid_kitty_socket") && spawned == 3);
    puts("PASS: Kitty socket required, env address explicit, override, invalid addresses rejected without spawn (headless)");
    return 0;
}
