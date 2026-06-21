/* pets.h — companion "pet" sprites for background agents/tasks.
 *
 * Ports the deterministic-companion idea from claude-code-pure's buddy/ module
 * into C: every background agent (swarm child, project worker, scheduled task)
 * gets a little animated creature whose species/eyes/hat/rarity are derived
 * deterministically from a seed string (its id/task), and whose expression and
 * frame reflect live status. A roster maps running agents → pets and drives
 * mini-notifications when tasks finish.
 *
 * Design goals:
 *  - Deterministic: same seed → same pet, across processes and restarts. Bones
 *    are never persisted; they regenerate from the seed (hash → mulberry32).
 *  - Cheap: a face glyph is a few bytes; full 5×12 sprites only for focus views.
 *    Scales to thousands of agents because the dense view renders faces, not art.
 *  - Self-contained: depends only on tui.h for colors/glyphs.
 */
#ifndef DSCO_PETS_H
#define DSCO_PETS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <pthread.h>

/* ── Taxonomy ──────────────────────────────────────────────────────────── */

typedef enum {
    PET_DUCK = 0, PET_GOOSE, PET_BLOB, PET_CAT, PET_DRAGON, PET_OCTOPUS,
    PET_OWL, PET_PENGUIN, PET_TURTLE, PET_SNAIL, PET_GHOST, PET_AXOLOTL,
    PET_CAPYBARA, PET_CACTUS, PET_ROBOT, PET_RABBIT, PET_MUSHROOM, PET_CHONK,
    PET_SPECIES_COUNT
} pet_species_t;

typedef enum {
    PET_RARITY_COMMON = 0, PET_RARITY_UNCOMMON, PET_RARITY_RARE,
    PET_RARITY_EPIC, PET_RARITY_LEGENDARY, PET_RARITY_COUNT
} pet_rarity_t;

typedef enum {
    PET_HAT_NONE = 0, PET_HAT_CROWN, PET_HAT_TOPHAT, PET_HAT_PROPELLER,
    PET_HAT_HALO, PET_HAT_WIZARD, PET_HAT_BEANIE, PET_HAT_TINYDUCK,
    PET_HAT_COUNT
} pet_hat_t;

/* Eyes are UTF-8 glyphs: · ✦ × ◉ @ ° */
#define PET_EYE_COUNT 6

/* Stats: DEBUGGING PATIENCE CHAOS WISDOM SNARK */
#define PET_STAT_COUNT 5

/* Deterministic part — derived from hash(seed). */
typedef struct {
    pet_species_t species;
    pet_rarity_t  rarity;
    int           eye;                  /* index into eye table */
    pet_hat_t     hat;
    bool          shiny;                /* ~1% — rainbow tint */
    int           stats[PET_STAT_COUNT];/* 1..100 */
} pet_bones_t;

/* ── Live status binding (one pet ↔ one background agent/task) ─────────── */

typedef enum {
    PET_ST_PENDING = 0,   /* queued, not started   ·  sleepy face */
    PET_ST_WORKING,       /* actively running      ◉  animated    */
    PET_ST_DONE,          /* finished ok           ✓  happy       */
    PET_ST_ERROR,         /* failed                ✗  dizzy        */
    PET_ST_IDLE           /* alive but quiet       -  resting      */
} pet_status_t;

typedef struct {
    int          id;              /* agent/child id (roster key) */
    int          project_id;      /* -1 if none */
    char         label[64];       /* task/agent description */
    char         name[40];        /* optional given name; else species */
    pet_bones_t  bones;
    pet_status_t status;
    double       cost_usd;
    double       start_time;      /* epoch seconds */
    double       end_time;        /* set on completion */
    double       activity_ring[16];/* recent output-bytes sparkline */
    int          activity_head;
    bool         notified;        /* completion mini-note already fired */
} pet_t;

typedef struct {
    pet_t          *pets;
    int             count;
    int             cap;
    int             frame;        /* global animation frame (advanced by clock) */
    pthread_mutex_t mu;
} pet_roster_t;

/* ── Bones derivation ──────────────────────────────────────────────────── */

/* Deterministically roll a pet from a seed string (agent id, task, user id…). */
void pet_roll(const char *seed, pet_bones_t *out);

/* ── Single-pet rendering ──────────────────────────────────────────────── */

/* Render the 5-line (or 4 after dropping a blank hat slot) sprite into `lines`
 * as UTF-8. Returns the number of lines written (≤6). `frame` selects an idle
 * fidget frame (wraps). Eye/hat substitution applied. */
int  pet_render_sprite(const pet_bones_t *b, int frame, char lines[6][96]);

/* Number of fidget frames for a species (currently 3 for all). */
int  pet_sprite_frame_count(pet_species_t s);

/* Compact one-line face, e.g. "(◉>" — for dense rosters. */
void pet_render_face(const pet_bones_t *b, char *buf, size_t n);

const char *pet_species_name(pet_species_t s);
const char *pet_rarity_name(pet_rarity_t r);
const char *pet_rarity_stars(pet_rarity_t r);     /* "★★★" */
const char *pet_rarity_color(pet_rarity_t r);     /* TUI color escape */
const char *pet_stat_name(int i);                 /* 0..PET_STAT_COUNT-1 */

/* Status → glyph / color / face-eye override (working pets "blink"). */
const char *pet_status_glyph(pet_status_t st);
const char *pet_status_color(pet_status_t st);
const char *pet_status_word(pet_status_t st);

/* Pretty "card": sprite on the left, name/rarity/stats/status on the right.
 * Honors the active glyph tier and truecolor. */
void pet_card_print(FILE *out, const pet_t *p, int frame);

/* One-off gallery of several species (for `pets gallery`). */
void pet_gallery_print(FILE *out, int frame);

/* ── Roster (background-agent pets) ────────────────────────────────────── */

void pet_roster_init(pet_roster_t *r);
void pet_roster_free(pet_roster_t *r);

/* Insert or update a pet by id. Rolls bones from `seed` (or label if NULL) on
 * first insert; status/cost/label refresh on update. Returns the pet index. */
int  pet_roster_upsert(pet_roster_t *r, int id, int project_id,
                       const char *label, const char *seed, pet_status_t status);

/* Update just the live status/cost of an existing pet. No-op if absent. */
void pet_roster_set_status(pet_roster_t *r, int id, pet_status_t status, double cost_usd);

/* Record a burst of output activity (bytes) for the sparkline. */
void pet_roster_activity(pet_roster_t *r, int id, double bytes);

void pet_roster_remove(pet_roster_t *r, int id);

/* Counts by status. Any out-param may be NULL. */
void pet_roster_counts(pet_roster_t *r, int *pending, int *working,
                       int *done, int *error, int *total);

/* Render the dense roster (one face row per agent) at the given width.
 * Sorted: working first, then pending, then finished. Caps at `max_rows`
 * (0 = all); logs an overflow count line if truncated. */
void pet_roster_render(FILE *out, pet_roster_t *r, int width, int max_rows);

/* Advance the global animation frame (call from the anim clock callback). */
void pet_roster_tick(pet_roster_t *r);

/* Process: returns the id of the next pet that transitioned to DONE/ERROR and
 * hasn't been notified yet, marking it notified. Returns -1 when none pending.
 * Lets the main loop drain completion notifications without races. */
int  pet_roster_next_unnotified(pet_roster_t *r, pet_status_t *out_status,
                                char *out_label, size_t label_n,
                                pet_bones_t *out_bones);

/* Process-wide roster shared by swarm/agent/project layers. */
pet_roster_t *pet_roster_global(void);

#endif /* DSCO_PETS_H */
