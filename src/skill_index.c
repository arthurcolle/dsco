/* skill_index.c — cosine top-k over the skill corpus. See include/skill_index.h.
 *
 * Vectors are L2-normalized on insert, so a query is a plain dot product and the
 * NEON path is a tight fmla reduction. Storage grows on demand up to
 * DSCO_SKILLS_MAX rather than reserving the full 32k*1024*4 = 131 MB up front.
 */

#include "skill_index.h"
#include "json_util.h" /* safe_malloc / safe_reallocarray */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#if defined(__aarch64__) || defined(__arm64__)
#include <arm_neon.h>
#define SKILL_INDEX_NEON 1
#endif

struct skill_index {
    int dim;      /* active compare width (DSCO_SKILL_EMBED_DIM) */
    size_t count; /* rows in use */
    size_t cap;   /* rows allocated */
    float *vecs;  /* count*dim, L2-normalized, row-major */
    char (*names)[DSCO_SKILL_NAME_MAX];
};

int skill_index_dim(void) {
    return DSCO_SKILL_EMBED_DIM;
}

static float dot_f(const float *a, const float *b, int n) {
#if SKILL_INDEX_NEON
    float32x4_t acc = vdupq_n_f32(0.0f);
    int i = 0;
    for (; i + 4 <= n; i += 4)
        acc = vfmaq_f32(acc, vld1q_f32(a + i), vld1q_f32(b + i));
    float s = vaddvq_f32(acc);
    for (; i < n; i++)
        s += a[i] * b[i];
    return s;
#else
    float s = 0.0f;
    for (int i = 0; i < n; i++)
        s += a[i] * b[i];
    return s;
#endif
}

skill_index_t *skill_index_new(void) {
    skill_index_t *ix = calloc(1, sizeof(*ix));
    if (!ix)
        return NULL;
    ix->dim = DSCO_SKILL_EMBED_DIM;
    return ix;
}

void skill_index_free(skill_index_t *ix) {
    if (!ix)
        return;
    free(ix->vecs);
    free(ix->names);
    free(ix);
}

size_t skill_index_count(const skill_index_t *ix) {
    return ix ? ix->count : 0;
}

/* Normalize the active-dim prefix of `src` into `dst`. Returns false if the
 * vector is (near) zero, which can't be normalized. */
static bool normalize_prefix(const float *src, float *dst, int dim) {
    float nrm = sqrtf(dot_f(src, src, dim));
    if (nrm < 1e-8f)
        return false;
    float inv = 1.0f / nrm;
    for (int i = 0; i < dim; i++)
        dst[i] = src[i] * inv;
    return true;
}

static bool ensure_capacity(skill_index_t *ix) {
    if (ix->count < ix->cap)
        return true;
    if (ix->count >= (size_t)DSCO_SKILLS_MAX)
        return false;
    size_t ncap = ix->cap ? ix->cap * 2 : 64;
    if (ncap > (size_t)DSCO_SKILLS_MAX)
        ncap = (size_t)DSCO_SKILLS_MAX;
    float *nv = safe_reallocarray(ix->vecs, ncap * (size_t)ix->dim, sizeof(float));
    ix->vecs = nv;
    char(*nn)[DSCO_SKILL_NAME_MAX] = safe_reallocarray(ix->names, ncap, sizeof(*ix->names));
    ix->names = nn;
    ix->cap = ncap;
    return true;
}

int skill_index_add(skill_index_t *ix, const char *name, const float *vec, int dim) {
    if (!ix || !name || !name[0] || !vec)
        return -1;
    /* Matryoshka: a wider vector is truncated to the active prefix; a narrower
     * one can't be compared and is rejected. */
    if (dim < ix->dim || dim > DSCO_SKILL_EMBED_DIM_MAX)
        return -1;
    if (!ensure_capacity(ix))
        return -1;
    float *row = ix->vecs + ix->count * (size_t)ix->dim;
    if (!normalize_prefix(vec, row, ix->dim))
        return -1;
    snprintf(ix->names[ix->count], DSCO_SKILL_NAME_MAX, "%s", name);
    ix->count++;
    return 0;
}

int skill_index_query(const skill_index_t *ix, const float *query, int dim,
                      skill_hit_t *out, int k) {
    if (!ix || !query || !out || k <= 0)
        return -1;
    if (dim < ix->dim || dim > DSCO_SKILL_EMBED_DIM_MAX)
        return -1;

    float *qn = safe_malloc((size_t)ix->dim * sizeof(float));
    if (!normalize_prefix(query, qn, ix->dim)) {
        free(qn);
        return -1;
    }

    /* Partial top-k by insertion: k is small, corpus is large, so O(n*k) beats a
     * full sort. `filled` grows to min(k, count); the array stays descending. */
    int filled = 0;
    for (size_t i = 0; i < ix->count; i++) {
        float score = dot_f(qn, ix->vecs + i * (size_t)ix->dim, ix->dim);
        if (filled < k) {
            int j = filled - 1;
            while (j >= 0 && out[j].score < score) {
                out[j + 1] = out[j];
                j--;
            }
            snprintf(out[j + 1].name, DSCO_SKILL_NAME_MAX, "%s", ix->names[i]);
            out[j + 1].score = score;
            filled++;
        } else if (score > out[k - 1].score) {
            int j = k - 2;
            while (j >= 0 && out[j].score < score) {
                out[j + 1] = out[j];
                j--;
            }
            snprintf(out[j + 1].name, DSCO_SKILL_NAME_MAX, "%s", ix->names[i]);
            out[j + 1].score = score;
        }
    }
    free(qn);
    return filled;
}
