#ifndef DSCO_AVIAN_H
#define DSCO_AVIAN_H

#include <stdbool.h>
#include <stddef.h>

/* ═══════════════════════════════════════════════════════════════════════════
 * Avian Mechanisms (Wings)
 *
 * Bird-inspired coordination primitives that complement pheromones:
 *   Nesting  — create a bounded, well-lined workspace/context before work.
 *   Brooding — incubate fragile candidates through repeated tending cycles.
 *   Fledging — promote a mature brooded candidate into independent execution.
 *   Roosting — temporarily quiesce a nest after heat/failure/cost pressure.
 *   Molting  — mark stale nest material for refresh without destroying lineage.
 *
 * These are intentionally small, in-memory primitives. Persistence can be
 * layered later through VFS/session_memory; the invariant here is that immature
 * work should not be promoted just because it exists.
 * ═══════════════════════════════════════════════════════════════════════════ */

#define AVIAN_MAX_NESTS       64
#define AVIAN_MAX_EGGS        128
#define AVIAN_NAME_LEN        96
#define AVIAN_TEXT_LEN        512
#define AVIAN_MATERIALS_MAX   12
#define AVIAN_MATERIAL_LEN    96

typedef enum {
    AVIAN_NEST_BUILDING = 0,
    AVIAN_NEST_WARM,
    AVIAN_NEST_READY,
    AVIAN_NEST_ROOSTING,
    AVIAN_NEST_MOLTING,
    AVIAN_NEST_ABANDONED,
} avian_nest_state_t;

typedef enum {
    AVIAN_EGG_LAID = 0,
    AVIAN_EGG_INCUBATING,
    AVIAN_EGG_READY,
    AVIAN_EGG_FLEDGED,
    AVIAN_EGG_FAILED,
    AVIAN_EGG_ABANDONED,
} avian_egg_state_t;

typedef struct {
    int    id;
    char   name[AVIAN_NAME_LEN];
    char   purpose[AVIAN_TEXT_LEN];
    char   materials[AVIAN_MATERIALS_MAX][AVIAN_MATERIAL_LEN];
    int    material_count;
    double warmth;        /* 0..1: attention/energy applied to the nest */
    double stability;     /* 0..1: invariants, tests, boundaries, evidence */
    double created_at;
    double last_tended_at;
    avian_nest_state_t state;
    bool   active;
} avian_nest_t;

typedef struct {
    int    id;
    int    nest_id;
    char   name[AVIAN_NAME_LEN];
    char   kind[AVIAN_NAME_LEN];
    char   lineage[AVIAN_TEXT_LEN];
    double risk;              /* 0..1: fragility / blast radius */
    double readiness;         /* 0..1: maturity under brood */
    int    cycles;            /* tending cycles completed */
    int    required_cycles;   /* minimum cycles before fledging */
    double created_at;
    double last_tended_at;
    avian_egg_state_t state;
    bool   active;
} avian_egg_t;

typedef struct {
    avian_nest_t nests[AVIAN_MAX_NESTS];
    avian_egg_t  eggs[AVIAN_MAX_EGGS];
    int nest_count;
    int egg_count;
    int next_nest_id;
    int next_egg_id;
    int total_materials_added;
    int total_brood_cycles;
    int total_fledged;
    int total_roosts;
    int total_molts;
    bool initialized;
} avian_engine_t;

void avian_init(avian_engine_t *a);
void avian_destroy(avian_engine_t *a);

int  avian_nest_create(avian_engine_t *a, const char *name, const char *purpose,
                       double warmth, double stability);
bool avian_nest_add_material(avian_engine_t *a, int nest_id, const char *material,
                             double quality, bool lining);
bool avian_nest_roost(avian_engine_t *a, int nest_id, const char *reason, double cooldown);
bool avian_nest_molt(avian_engine_t *a, int nest_id, const char *reason);

int  avian_brood_lay(avian_engine_t *a, int nest_id, const char *name, const char *kind,
                     const char *lineage, double risk, int required_cycles);
bool avian_brood_tend(avian_engine_t *a, int egg_id, double warmth, const char *evidence);
bool avian_brood_fledge(avian_engine_t *a, int egg_id, char *reason, size_t reason_len);
bool avian_brood_abandon(avian_engine_t *a, int egg_id, const char *reason);

const avian_nest_t *avian_nest_get(const avian_engine_t *a, int nest_id);
const avian_egg_t  *avian_egg_get(const avian_engine_t *a, int egg_id);

const char *avian_nest_state_name(avian_nest_state_t s);
const char *avian_egg_state_name(avian_egg_state_t s);

int avian_status_json(const avian_engine_t *a, char *buf, size_t len);
int avian_nest_json(const avian_engine_t *a, int nest_id, char *buf, size_t len);
int avian_egg_json(const avian_engine_t *a, int egg_id, char *buf, size_t len);

#endif /* DSCO_AVIAN_H */
