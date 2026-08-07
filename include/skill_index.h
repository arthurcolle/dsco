#ifndef DSCO_SKILL_INDEX_H
#define DSCO_SKILL_INDEX_H

/* skill_index — semantic top-k retrieval over the workspace skill corpus.
 *
 * Sizing (named, per the no-magic-buffers rule):
 *   DSCO_SKILLS_MAX          corpus capacity — up to 32k skills.
 *   DSCO_SKILL_EMBED_DIM_MAX Matryoshka ceiling — vectors may carry up to 4196
 *                            dims. MRL lets us keep the full width on disk and
 *                            compare on a prefix.
 *   DSCO_SKILL_EMBED_DIM     active compare width. We operate at 1024 today; the
 *                            index truncates any wider vector to this prefix,
 *                            which is exactly a Matryoshka truncation.
 *
 * Only the 1024 path is exercised now. Raising the active dim later is a one-line
 * change plus re-embedding; the ceiling already reserves the room.
 */

#include <stddef.h>
#include <stdbool.h>

#define DSCO_SKILLS_MAX 32000
#define DSCO_SKILL_EMBED_DIM_MAX 4196
#define DSCO_SKILL_EMBED_DIM 1024
#define DSCO_SKILL_NAME_MAX 128

typedef struct skill_index skill_index_t;

typedef struct {
    char name[DSCO_SKILL_NAME_MAX];
    float score; /* cosine similarity on the active-dim prefix, [-1, 1] */
} skill_hit_t;

/* Create/destroy. Capacity is capped at DSCO_SKILLS_MAX. */
skill_index_t *skill_index_new(void);
void skill_index_free(skill_index_t *ix);

/* Active compare dimension (currently DSCO_SKILL_EMBED_DIM). */
int skill_index_dim(void);

/* Add one skill embedding. `vec` holds `dim` floats; if dim > the active width
 * the vector is truncated to the prefix (Matryoshka), if smaller it is rejected.
 * The vector is L2-normalized on insert so queries are plain dot products.
 * Returns 0 on success, -1 on error (bad args, full, dim < active). */
int skill_index_add(skill_index_t *ix, const char *name, const float *vec, int dim);

/* Number of skills currently indexed. */
size_t skill_index_count(const skill_index_t *ix);

/* Top-k by cosine similarity to `query` (dim floats, truncated/normalized like
 * add). Fills up to k hits sorted by descending score. Returns the number
 * written, or -1 on error. */
int skill_index_query(const skill_index_t *ix, const float *query, int dim,
                      skill_hit_t *out, int k);

#endif /* DSCO_SKILL_INDEX_H */
