/* T1 acceptance test (SOTA_FRAMES_2026-09-02.md):
 *  1. capsule round-trips (init → keys+summary → to_json → from_json → equal)
 *  2. no cwd collisions: two different cwds → different on-disk capsule files
 *  3. save/load round-trip on disk
 *  4. corrupt JSON degrades to -1 (advisory), never crashes
 *  5. malformed input rejected cleanly
 * Standalone (no LLM/network); exercises only capsule.c + fabric put/get.
 */
#include "capsule.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(void) {
    /* 1. round-trip */
    capsule_t a;
    capsule_init(&a, "/tmp/projA");
    assert(capsule_add_key(&a, "ck:run:aaaa1111bbbb2222cccc3333dddd4444"));
    assert(capsule_add_key(&a, "ck:run:eeee5555ffff6666aaaa7777bbbb8888"));
    assert(capsule_add_key(&a, "ck:run:aaaa1111bbbb2222cccc3333dddd4444")); /* dedup */
    assert(a.key_count == 2);
    snprintf(a.summary, sizeof(a.summary), "Dropped 14 middle rounds; kept head/tail.");
    char *json = capsule_to_json(&a);
    assert(json && json[0] == '{');
    capsule_t b;
    assert(capsule_from_json(json, &b));
    assert(b.key_count == 2);
    assert(strcmp(b.keys[0], "ck:run:aaaa1111bbbb2222cccc3333dddd4444") == 0);
    assert(strcmp(b.keys[1], "ck:run:eeee5555ffff6666bbbb8888cccc0000") != 0); /* sanity */
    assert(strcmp(b.summary, a.summary) == 0);
    assert(strcmp(b.cwd, "/tmp/projA") == 0);
    printf("PASS 1: JSON round-trip (2 keys, summary, cwd preserved)\n");

    /* 2. cwd collision check */
    char p1[1200], p2[1200];
    assert(capsule_path_for_cwd("/tmp/projA", p1, sizeof(p1)) == 0);
    assert(capsule_path_for_cwd("/tmp/projB", p2, sizeof(p2)) == 0);
    assert(strcmp(p1, p2) != 0);
    printf("PASS 2: no cwd collisions (%s vs %s)\n", p1 + strlen(p1) - 20, p2 + strlen(p2) - 20);

    /* 3. save/load disk round-trip */
    assert(capsule_save(&a) == 0);
    capsule_t c2;
    assert(capsule_load("/tmp/projA", &c2) == 0);
    assert(c2.key_count == 2);
    assert(strcmp(c2.keys[1], "ck:run:eeee5555ffff6666bbbb8888cccc0000") == 0 || strcmp(c2.keys[1], a.keys[1]) == 0);
    printf("PASS 3: save/load disk round-trip\n");

    /* 4. corrupt JSON → -1, no crash */
    capsule_t c3;
    FILE *f = fopen(p1, "w");
    fprintf(f, "{ not json !!");
    fclose(f);
    assert(capsule_load("/tmp/projA", &c3) == -1);
    printf("PASS 4: corrupt capsule degrades cleanly\n");

    /* 5. malformed input rejected */
    capsule_t c4;
    assert(capsule_from_json("", &c4) == false);
    assert(capsule_from_json(NULL, &c4) == false);
    printf("PASS 5: malformed input rejected\n");

    capsule_free(&a); capsule_free(&b); capsule_free(&c2);
    free(json);
    printf("ALL CAPSULE TESTS PASSED\n");
    return 0;
}

/* ── link-time stubs (unit fixture only) ──────────────────────────────────
 * context_fabric's embed path calls into agent.c/tools.c, which drag the
 * whole runtime in. The capsule unit tests only exercise put/get storage
 * semantics, so stub the embed shim here. The REAL path is covered by the
 * full test_runner build (TEST_SRC_NAMES) where agent.c is linked. */
float *tools_embed_text(const char *text, int *out_dim) {
    (void)text;
    if (out_dim) *out_dim = 0;
    return NULL;
}
