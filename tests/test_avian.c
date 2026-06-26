/*
 * test_avian.c — standalone tests for Wings avian mechanisms.
 *
 * Build & run:
 *   make test_avian && ./test_avian
 */

#include "avian.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static int tests_run = 0;

#define CHECK(cond, msg)                                                                            \
    do {                                                                                            \
        tests_run++;                                                                                \
        if (!(cond)) {                                                                              \
            fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__);                                \
            assert(cond);                                                                           \
        }                                                                                           \
    } while (0)

static int contains(const char *haystack, const char *needle) {
    return haystack && needle && strstr(haystack, needle) != NULL;
}


static void force_egg_ready(avian_engine_t *a, int egg_id, double readiness, int cycles) {
    for (int i = 0; i < AVIAN_MAX_EGGS; i++) {
        if (a->eggs[i].active && a->eggs[i].id == egg_id) {
            a->eggs[i].state = AVIAN_EGG_READY;
            a->eggs[i].readiness = readiness;
            a->eggs[i].cycles = cycles;
            return;
        }
    }
    CHECK(0, "force_egg_ready found target egg");
}

static int make_ready_nest(avian_engine_t *a, const char *name) {
    int nest_id = avian_nest_create(a, name, "test nest", 0.70, 0.72);
    CHECK(nest_id > 0, "ready nest created");
    CHECK(avian_nest_add_material(a, nest_id, "test plan", 0.90, true), "add test plan");
    CHECK(avian_nest_add_material(a, nest_id, "rollback plan", 0.90, true), "add rollback plan");
    const avian_nest_t *n = avian_nest_get(a, nest_id);
    CHECK(n != NULL, "ready nest retrievable");
    CHECK(n->state == AVIAN_NEST_READY, "nest reaches ready after lining materials");
    return nest_id;
}

static void test_init_and_empty_status(void) {
    avian_engine_t a;
    char json[1024];

    avian_init(&a);
    CHECK(a.initialized, "engine initialized");
    CHECK(a.next_nest_id == 1, "nest IDs start at 1");
    CHECK(a.next_egg_id == 1, "egg IDs start at 1");

    avian_status_json(&a, json, sizeof(json));
    CHECK(contains(json, "\"initialized\":true"), "status reports initialized");
    CHECK(contains(json, "\"nests\":0"), "status reports zero nests");
    CHECK(contains(json, "\"eggs\":0"), "status reports zero eggs");

    avian_destroy(&a);
    CHECK(!a.initialized, "destroy clears initialized flag");
}

static void test_nesting_materials_state_and_json_escaping(void) {
    avian_engine_t a;
    char json[2048];

    avian_init(&a);
    int nest_id = avian_nest_create(&a, "quote\"nest", "line\nbackslash\\purpose", 0.0, 0.0);
    CHECK(nest_id == 1, "first nest id is 1");

    const avian_nest_t *n = avian_nest_get(&a, nest_id);
    CHECK(n != NULL, "nest retrievable");
    CHECK(n->state == AVIAN_NEST_BUILDING, "default low warmth/stability starts building");
    CHECK(fabs(n->warmth - 0.25) < 1e-9, "default warmth applied");
    CHECK(fabs(n->stability - 0.25) < 1e-9, "default stability applied");

    CHECK(avian_nest_add_material(&a, nest_id, "context", 0.50, false), "add first material");
    n = avian_nest_get(&a, nest_id);
    CHECK(n->state == AVIAN_NEST_WARM, "material warms building nest");
    CHECK(n->material_count == 1, "material count increments");

    for (int i = 0; i < AVIAN_MATERIALS_MAX - 1; i++)
        CHECK(avian_nest_add_material(&a, nest_id, "lining", 1.0, true), "fill material slot");
    CHECK(!avian_nest_add_material(&a, nest_id, "overflow", 1.0, true),
          "material capacity enforced");

    avian_nest_json(&a, nest_id, json, sizeof(json));
    CHECK(contains(json, "quote\\\"nest"), "nest JSON escapes quotes");
    CHECK(contains(json, "line\\nbackslash\\\\purpose"), "nest JSON escapes newline/backslash");
    CHECK(contains(json, "\"materials\""), "nest JSON includes materials");
}

static void test_brooding_requires_cycles_before_fledging(void) {
    avian_engine_t a;
    char reason[256];

    avian_init(&a);
    int nest_id = make_ready_nest(&a, "cycle-nest");
    int egg_id = avian_brood_lay(&a, nest_id, "patch", "code-change", "unit-test", 0.20, 3);
    CHECK(egg_id > 0, "egg laid in ready nest");

    const avian_egg_t *e = avian_egg_get(&a, egg_id);
    CHECK(e != NULL, "egg retrievable");
    CHECK(e->state == AVIAN_EGG_LAID, "new egg starts laid");
    CHECK(e->required_cycles == 3, "required cycles stored");

    CHECK(avian_brood_tend(&a, egg_id, 1.0, "build clean"), "first brood tend succeeds");
    CHECK(avian_brood_tend(&a, egg_id, 1.0, "tests pass"), "second brood tend succeeds");
    CHECK(!avian_brood_fledge(&a, egg_id, reason, sizeof(reason)),
          "cannot fledge before required cycle count");
    CHECK(contains(reason, "not ready"), "failed fledge explains not ready");

    CHECK(avian_brood_tend(&a, egg_id, 1.0, "review complete"), "third brood tend succeeds");
    e = avian_egg_get(&a, egg_id);
    CHECK(e->state == AVIAN_EGG_READY, "egg ready after readiness and cycle threshold");
    CHECK(avian_brood_fledge(&a, egg_id, reason, sizeof(reason)), "ready egg fledges");
    CHECK(contains(reason, "fledged: patch"), "fledge reason names candidate");
    CHECK(avian_egg_get(&a, egg_id)->state == AVIAN_EGG_FLEDGED, "state becomes fledged");
}

static void test_high_risk_fledging_requires_extra_readiness(void) {
    avian_engine_t a;
    char reason[256];

    avian_init(&a);
    int nest_id = avian_nest_create(&a, "risk-nest", "high risk test", 0.70, 0.70);
    CHECK(nest_id > 0, "risk nest created");
    CHECK(avian_nest_add_material(&a, nest_id, "risk register", 0.20, true), "risk material 1");
    CHECK(avian_nest_add_material(&a, nest_id, "rollback plan", 0.20, true), "risk material 2");

    int egg_id = avian_brood_lay(&a, nest_id, "risky patch", "self-modification", "unit-test", 0.90, 2);
    CHECK(egg_id > 0, "high risk egg laid");
    /* Force the boundary condition directly: nominally ready by normal threshold,
       but still below the stricter high-risk fledging threshold. */
    force_egg_ready(&a, egg_id, 0.85, 2);

    const avian_egg_t *e = avian_egg_get(&a, egg_id);
    CHECK(e->state == AVIAN_EGG_READY, "high-risk egg can be nominally ready");
    CHECK(e->readiness >= 0.80 && e->readiness < 0.92,
          "test setup produces ready but not high-risk-ready candidate");
    CHECK(!avian_brood_fledge(&a, egg_id, reason, sizeof(reason)),
          "high-risk candidate cannot fledge below 0.92 readiness");
    CHECK(contains(reason, "high-risk"), "high-risk fledge denial is explicit");

    CHECK(avian_brood_tend(&a, egg_id, 1.0, "held-out replay passed"),
          "additional evidence can mature high-risk egg");
    CHECK(avian_brood_fledge(&a, egg_id, reason, sizeof(reason)),
          "high-risk candidate fledges after extra readiness");
}

static void test_roosting_and_molting_transitions(void) {
    avian_engine_t a;

    avian_init(&a);
    int nest_id = make_ready_nest(&a, "maintenance-nest");

    CHECK(avian_nest_roost(&a, nest_id, "heat", 0.20), "roost succeeds");
    const avian_nest_t *n = avian_nest_get(&a, nest_id);
    CHECK(n->state == AVIAN_NEST_ROOSTING, "roost sets roosting state");
    CHECK(a.total_roosts == 1, "roost counter increments");

    CHECK(avian_nest_molt(&a, nest_id, "stale context"), "molt succeeds from roost");
    n = avian_nest_get(&a, nest_id);
    CHECK(n->state == AVIAN_NEST_MOLTING, "molt sets molting state");
    CHECK(a.total_molts == 1, "molt counter increments");

    CHECK(avian_nest_add_material(&a, nest_id, "fresh invariant", 1.0, true),
          "adding material after molt re-lines nest");
    n = avian_nest_get(&a, nest_id);
    CHECK(n->state == AVIAN_NEST_WARM || n->state == AVIAN_NEST_READY,
          "fresh material exits molting state");
}

static void test_capacity_and_invalid_operations(void) {
    avian_engine_t a;
    char reason[128];

    avian_init(&a);
    CHECK(avian_brood_lay(&a, 999, "orphan", "proposal", "none", 0.1, 1) == -1,
          "cannot lay egg in missing nest");
    CHECK(!avian_brood_tend(&a, 999, 1.0, "none"), "cannot tend missing egg");
    CHECK(!avian_brood_fledge(&a, 999, reason, sizeof(reason)), "cannot fledge missing egg");
    CHECK(contains(reason, "not found"), "missing egg fledge reports not found");

    for (int i = 0; i < AVIAN_MAX_NESTS; i++) {
        int id = avian_nest_create(&a, "capacity", "capacity test", 0.5, 0.5);
        CHECK(id > 0, "nest created inside capacity");
    }
    CHECK(avian_nest_create(&a, "overflow", "capacity test", 0.5, 0.5) == -1,
          "nest capacity enforced");
}

static void test_egg_capacity_and_terminal_states(void) {
    avian_engine_t a;

    avian_init(&a);
    int nest_id = make_ready_nest(&a, "egg-capacity-nest");

    for (int i = 0; i < AVIAN_MAX_EGGS; i++) {
        int id = avian_brood_lay(&a, nest_id, "egg", "proposal", "capacity", 0.1, 1);
        CHECK(id > 0, "egg created inside capacity");
    }
    CHECK(avian_brood_lay(&a, nest_id, "overflow", "proposal", "capacity", 0.1, 1) == -1,
          "egg capacity enforced");

    CHECK(avian_brood_abandon(&a, 1, "not viable"), "abandon egg succeeds");
    CHECK(avian_egg_get(&a, 1)->state == AVIAN_EGG_ABANDONED, "egg state abandoned");
    CHECK(!avian_brood_tend(&a, 1, 1.0, "should not revive"),
          "abandoned egg cannot be tended back to life");
}

static void test_status_counters(void) {
    avian_engine_t a;
    char json[2048];
    char reason[256];

    avian_init(&a);
    int nest_id = make_ready_nest(&a, "status-nest");
    int egg_id = avian_brood_lay(&a, nest_id, "status patch", "code-change", "unit-test", 0.2, 1);
    CHECK(egg_id > 0, "status egg laid");
    CHECK(avian_brood_tend(&a, egg_id, 1.0, "evidence"), "status egg tended");
    force_egg_ready(&a, egg_id, 0.90, 1);
    CHECK(avian_brood_fledge(&a, egg_id, reason, sizeof(reason)), "status egg fledged");
    CHECK(avian_nest_roost(&a, nest_id, "done", 0.1), "status nest roosted");
    CHECK(avian_nest_molt(&a, nest_id, "refresh"), "status nest molted");

    avian_status_json(&a, json, sizeof(json));
    CHECK(contains(json, "\"nests\":1"), "status includes one nest");
    CHECK(contains(json, "\"eggs\":1"), "status includes one egg");
    CHECK(contains(json, "\"brood_cycles\":1"), "status includes brood cycle count");
    CHECK(contains(json, "\"fledged\":1"), "status includes fledged count");
    CHECK(contains(json, "\"roosts\":1"), "status includes roost count");
    CHECK(contains(json, "\"molts\":1"), "status includes molt count");
}

int main(void) {
    test_init_and_empty_status();
    test_nesting_materials_state_and_json_escaping();
    test_brooding_requires_cycles_before_fledging();
    test_high_risk_fledging_requires_extra_readiness();
    test_roosting_and_molting_transitions();
    test_capacity_and_invalid_operations();
    test_egg_capacity_and_terminal_states();
    test_status_counters();

    fprintf(stderr, "test_avian: %d assertions passed\n", tests_run);
    return 0;
}
