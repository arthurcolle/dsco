#ifndef DSCO_VECSTORE_H
#define DSCO_VECSTORE_H

#include "vfs.h"
#include <stdbool.h>
#include <stddef.h>

/* ═══════════════════════════════════════════════════════════════════════════
 * VFS-backed Vector Store
 *
 * General-purpose embedding storage and similarity search.
 * Stores float vectors in VFS KV buckets with brute-force cosine query.
 * Suitable for <100K vectors; add IVF clustering for larger scale.
 * ═══════════════════════════════════════════════════════════════════════════ */

#define VECSTORE_MAX_DIM 1024

typedef struct vecstore vecstore_t;

typedef struct {
    char  *id;
    float  score;
    char  *metadata;
} vecstore_result_t;

/* ── Lifecycle ────────────────────────────────────────────────────────── */

vecstore_t *vecstore_open(vfs_db_t *vfs, const char *collection);
void        vecstore_close(vecstore_t *vs);

/* ── CRUD ─────────────────────────────────────────────────────────────── */

/* Insert or update a vector. metadata is optional (may be NULL). */
bool   vecstore_insert(vecstore_t *vs, const char *id,
                       const float *vec, int dim, const char *metadata);

/* Delete a vector by ID. */
bool   vecstore_delete(vecstore_t *vs, const char *id);

/* Retrieve a single vector by ID. Caller frees return value. */
float *vecstore_get(vecstore_t *vs, const char *id, int *out_dim);

/* ── Query ────────────────────────────────────────────────────────────── */

/* Find top-k most similar vectors. Returns count found.
   Caller must call vecstore_result_free() on results. */
int    vecstore_query(vecstore_t *vs, const float *query_vec, int dim,
                      vecstore_result_t *out, int max_results);

void   vecstore_result_free(vecstore_result_t *results, int count);

/* ── Stats ────────────────────────────────────────────────────────────── */

int    vecstore_count(vecstore_t *vs);

/* ── Shared Utility ───────────────────────────────────────────────────── */

/* Cosine similarity for float vectors. */
float  cosine_similarity_f(const float *a, const float *b, int dim);

#endif /* DSCO_VECSTORE_H */
