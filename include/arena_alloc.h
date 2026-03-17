#ifndef DSCO_ARENA_ALLOC_H
#define DSCO_ARENA_ALLOC_H

#include <stddef.h>
#include <stdbool.h>
#include "json_util.h"  /* arena_t lives here */

/* ── Global arenas ─────────────────────────────────────────────────── */

arena_t *arena_scratch(void);
arena_t *arena_session(void);

/* ── Lifecycle ─────────────────────────────────────────────────────── */

void arena_subsystem_init(void);
void arena_subsystem_shutdown(void);
void arena_scratch_reset(void);

/* ── Convenience: scratch arena ────────────────────────────────────── */

void *scratch_alloc(size_t size);
char *scratch_strdup(const char *s);
char *scratch_sprintf(const char *fmt, ...);

/* ── Convenience: session arena ────────────────────────────────────── */

void *session_alloc(size_t size);
char *session_strdup(const char *s);
char *session_sprintf(const char *fmt, ...);

/* ── Temp scope (stack-like) ───────────────────────────────────────── */

/* Save/restore scratch arena state for scoped temporaries.
   Note: only valid if no oversized allocs happened in the scope
   (oversized allocs are on separate malloc chain and always freed on reset). */
typedef struct {
    size_t saved_used;   /* head chunk used offset at snapshot time */
} arena_temp_t;

arena_temp_t arena_temp_begin(void);
void         arena_temp_end(arena_temp_t mark);

/* ── Stats ─────────────────────────────────────────────────────────── */

typedef struct {
    size_t scratch_bytes_allocated;
    size_t session_bytes_allocated;
    size_t scratch_resets;
    size_t temp_scopes;
} arena_stats_t;

arena_stats_t arena_get_stats(void);

#endif /* DSCO_ARENA_ALLOC_H */
