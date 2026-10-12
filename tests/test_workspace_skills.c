#define _DARWIN_C_SOURCE 1
#include "workspace.h"
#include "directive_store.h"
#include "crypto.h"
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
    snprintf(path, sizeof(path), "%s/skills/README.md", root);
    FILE *readme = fopen(path, "w"); assert(readme);
    assert(fputs("# Skill packages\n", readme) >= 0); assert(fclose(readme) == 0);
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
    /* Small catalogs are useful in the prompt; a truncated alphabetical
     * prefix of a large catalog only burns input tokens each request. */
    assert(strstr(dsco_workspace_prompt(), "a-plain: Narrow trigger."));
    for (int i = 0; i < 260; i++) {
        char name[32];
        snprintf(name, sizeof(name), "extra-%03d", i);
        fixture(root, name, "# Skill\nSynthetic test description.\n");
    }
    dsco_workspace_prompt_invalidate();
    const char *prompt = dsco_workspace_prompt();
    assert(strstr(prompt, "Discover relevant skills by task"));
    assert(!strstr(prompt, "a-plain: Narrow trigger."));
    assert(!strstr(prompt, "Synthetic test description."));
    const char *skill = dsco_workspace_skill_prompt("a-plain");
    char digest[65]; sha256_hex((const uint8_t *)skill, strlen(skill), digest);
    char *selected = dsco_workspace_skill_context("a-plain", digest, "openai", "openai");
    assert(selected && strstr(selected, "Active Skill: a-plain") && strstr(selected, "Narrow trigger.") && strstr(selected, digest)); free(selected);
    selected = dsco_workspace_skill_context("a-plain", "stale", "openai", "openai");
    assert(selected && strstr(selected, "Instructions withheld") && !strstr(selected, "Narrow trigger.")); free(selected);
    selected = dsco_workspace_skill_context("a-plain", digest, "openai", "anthropic");
    assert(selected && strstr(selected, "instructions withheld") && !strstr(selected, "Narrow trigger.") && !strstr(selected, digest)); free(selected);
    selected = dsco_workspace_skill_context("a-plain", digest, "", "openai");
    assert(selected && strstr(selected, "instructions withheld")); free(selected);
    selected = dsco_workspace_skill_context("missing", digest, "openai", "openai");
    assert(selected && strstr(selected, "could not be read")); free(selected);
    assert(dsco_workspace_skill_context("", digest, "openai", "openai") == NULL);
    puts("ok: skill catalog metadata and large-catalog prompt disclosure");
    return 0;
}
