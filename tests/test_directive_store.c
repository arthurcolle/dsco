#define _DARWIN_C_SOURCE 1
#include "directive_store.h"
#include "workspace.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int main(void) {
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "/tmp/dsco-directive-test.%ld", (long)getpid());
    assert(mkdir(tmp, 0700) == 0);
    char home[1024]; snprintf(home, sizeof(home), "%s/home", tmp); assert(mkdir(home, 0700) == 0);
    assert(setenv("HOME", home, 1) == 0);

    char workspace[1200]; snprintf(workspace, sizeof(workspace), "%s/.dsco/workspace", home);
    char dsco[1100]; snprintf(dsco, sizeof(dsco), "%s/.dsco", home); assert(mkdir(dsco, 0700) == 0);
    assert(mkdir(workspace, 0700) == 0);

    char out[32768];
    assert(dsco_directive_set("Operate autonomously until verified completion.", "test v1", out, sizeof(out)));
    char v1[65] = {0}; char *p = strstr(out, "\"version\":\""); assert(p); memcpy(v1, p + 11, 64);
    dsco_directive_prompt_invalidate();
    assert(strstr(dsco_directive_prompt(), "autonomously"));

    assert(dsco_directive_set("Second persistent behavior.", "test v2", out, sizeof(out)));
    assert(dsco_directive_history(out, sizeof(out)));
    assert(strstr(out, "test v1") && strstr(out, "test v2"));

    assert(dsco_directive_rollback(v1, "test rollback", out, sizeof(out)));
    dsco_directive_prompt_invalidate();
    assert(strstr(dsco_directive_prompt(), "autonomously"));

    assert(dsco_directive_clear("test clear", out, sizeof(out)));
    dsco_directive_prompt_invalidate();
    assert(dsco_directive_prompt() == NULL);
    puts("ok: persistent directive set/history/rollback/clear");
    return 0;
}
