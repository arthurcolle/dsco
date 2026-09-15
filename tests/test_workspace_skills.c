#define _DARWIN_C_SOURCE 1
#include "workspace.h"
#include "directive_store.h"
/* Catalog tests do not read or mutate persistent directives. */
const char *dsco_directive_prompt(void) { return NULL; }
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void fixture(const char *root, const char *name, const char *text) {
    char path[2048];
    snprintf(path, sizeof(path), "%s/skills/%s", root, name);
    assert(mkdir(path, 0700) == 0);
    snprintf(path, sizeof(path), "%s/skills/%s/SKILL.md", root, name);
    FILE *f = fopen(path, "w"); assert(f);
    assert(fputs(text, f) >= 0); assert(fclose(f) == 0);
}
int main(int argc, char **argv) {
    char out[32768];
    if (argc == 2 && strcmp(argv[1], "--live") == 0) {
        assert(dsco_workspace_list_skills(out, sizeof(out)) > 0);
        assert(strstr(out, ": ---") == NULL);
        puts(out); return 0;
    }
    char home[] = "/tmp/dsco-skills-test.XXXXXX";
    assert(mkdtemp(home)); assert(setenv("HOME", home, 1) == 0);
    char path[2048], root[2048];
    snprintf(path, sizeof(path), "%s/.dsco", home); assert(mkdir(path, 0700) == 0);
    snprintf(root, sizeof(root), "%s/.dsco/workspace", home); assert(mkdir(root, 0700) == 0);
    snprintf(path, sizeof(path), "%s/skills", root); assert(mkdir(path, 0700) == 0);
    fixture(root, "a-plain", "---\nname: ignored\ndescription: Narrow trigger.\n---\n# Title\nBody ignored.\n");
    fixture(root, "b-quote", "---\r\ndescription: \"Quoted trigger.\"\r\n---\r\n# Heading\r\n");
    fixture(root, "c-single", "---\ndescription: 'Single quoted.'\n---\n");
    fixture(root, "d-folded", "---\ndescription: >-\n  Folded first\n  second line.\nversion: 2\n---\n");
    fixture(root, "e-literal", "---\ndescription: |\n  Literal first\n  second line.\n---\n");
    fixture(root, "f-fallback", "---\nname: hidden metadata\n---\n# Heading\nFallback prose.\n");
    fixture(root, "g-markdown", "# Legacy\n\nLegacy prose.\n");
    fixture(root, "h-empty", "---\nname: metadata only\n---\n# Heading\n");
    fixture(root, "i-malformed", "---\ndescription: incomplete header\n");
    char longtext[1800]; memset(longtext, 'x', sizeof(longtext));
    memcpy(longtext, "---\nnotes: ", 11);
    snprintf(longtext + 1000, sizeof(longtext) - 1000, "\ndescription: After long metadata.\n---\n");
    fixture(root, "j-long", longtext);
    assert(dsco_workspace_list_skills(out, sizeof(out)) == 10);
    assert(strstr(out, "a-plain: Narrow trigger."));
    assert(strstr(out, "b-quote: Quoted trigger."));
    assert(strstr(out, "c-single: Single quoted."));
    assert(strstr(out, "d-folded: Folded first second line."));
    assert(strstr(out, "e-literal: Literal first second line."));
    assert(strstr(out, "f-fallback: Fallback prose."));
    assert(strstr(out, "g-markdown: Legacy prose."));
    assert(strstr(out, "- h-empty\n") && strstr(out, "- i-malformed\n"));
    assert(strstr(out, "j-long: After long metadata."));
    assert(!strstr(out, ": ---") && !strstr(out, "Body ignored") && !strstr(out, "incomplete header"));
    char tiny[16]; memset(tiny, 0x7f, sizeof(tiny));
    (void)dsco_workspace_list_skills(tiny, sizeof(tiny));
    assert(memchr(tiny, 0, sizeof(tiny)));
    puts("ok: 10 skill catalog fixtures, YAML metadata, legacy fallback, bounded output");
    return 0;
}
