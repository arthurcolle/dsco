#include "vecstore.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

/* ═══════════════════════════════════════════════════════════════════════════
 * VFS-backed Vector Store — Implementation
 *
 * Uses two VFS KV buckets per collection:
 *   "vec_<name>"     → raw float[dim] blobs
 *   "vecmeta_<name>" → JSON metadata strings
 *
 * Brute-force cosine similarity for queries. Fast enough for <100K vectors.
 * ═══════════════════════════════════════════════════════════════════════════ */

struct vecstore {
    vfs_db_t *vfs;
    char vec_bucket[80];  /* "vec_<collection>" */
    char meta_bucket[80]; /* "vecmeta_<collection>" */
    int default_dim;
};

/* ── Cosine Similarity ────────────────────────────────────────────────── */

float cosine_similarity_f(const float *a, const float *b, int dim) {
    float dot = 0.0f, na = 0.0f, nb = 0.0f;
    for (int i = 0; i < dim; i++) {
        dot += a[i] * b[i];
        na += a[i] * a[i];
        nb += b[i] * b[i];
    }
    float denom = sqrtf(na) * sqrtf(nb);
    return denom > 1e-8f ? dot / denom : 0.0f;
}

/* ── Lifecycle ────────────────────────────────────────────────────────── */

vecstore_t *vecstore_open(vfs_db_t *vfs, const char *collection) {
    if (!vfs || !collection || !collection[0])
        return NULL;

    vecstore_t *vs = calloc(1, sizeof(*vs));
    if (!vs)
        return NULL;

    vs->vfs = vfs;
    snprintf(vs->vec_bucket, sizeof(vs->vec_bucket), "vec_%s", collection);
    snprintf(vs->meta_bucket, sizeof(vs->meta_bucket), "vecmeta_%s", collection);
    vs->default_dim = VECSTORE_MAX_DIM;

    return vs;
}

void vecstore_close(vecstore_t *vs) {
    if (vs)
        free(vs);
}

/* ── Insert ───────────────────────────────────────────────────────────── */

bool vecstore_insert(vecstore_t *vs, const char *id, const float *vec, int dim,
                     const char *metadata) {
    if (!vs || !id || !vec || dim <= 0 || dim > VECSTORE_MAX_DIM)
        return false;

    /* Store vector as raw float blob */
    size_t blob_len = (size_t)dim * sizeof(float);
    if (!vfs_kv_put(vs->vfs, vs->vec_bucket, id, vec, blob_len))
        return false;

    /* Store metadata (optional) */
    if (metadata && metadata[0]) {
        vfs_kv_put_str(vs->vfs, vs->meta_bucket, id, metadata);
    }

    return true;
}

/* ── Delete ───────────────────────────────────────────────────────────── */

bool vecstore_delete(vecstore_t *vs, const char *id) {
    if (!vs || !id)
        return false;
    vfs_kv_delete(vs->vfs, vs->meta_bucket, id);
    return vfs_kv_delete(vs->vfs, vs->vec_bucket, id);
}

/* ── Get ──────────────────────────────────────────────────────────────── */

float *vecstore_get(vecstore_t *vs, const char *id, int *out_dim) {
    if (!vs || !id)
        return NULL;

    size_t blob_len = 0;
    void *blob = vfs_kv_get(vs->vfs, vs->vec_bucket, id, &blob_len);
    if (!blob || blob_len < sizeof(float))
        return NULL;

    int dim = (int)(blob_len / sizeof(float));
    if (out_dim)
        *out_dim = dim;

    /* blob is already a malloc'd float array from vfs_kv_get */
    return (float *)blob;
}

/* ── Query (brute-force cosine similarity) ────────────────────────────── */

/* Comparison for qsort — descending score */
static int result_cmp_desc(const void *a, const void *b) {
    float sa = ((const vecstore_result_t *)a)->score;
    float sb = ((const vecstore_result_t *)b)->score;
    if (sb > sa)
        return 1;
    if (sb < sa)
        return -1;
    return 0;
}

int vecstore_query(vecstore_t *vs, const float *query_vec, int dim, vecstore_result_t *out,
                   int max_results) {
    if (!vs || !query_vec || dim <= 0 || !out || max_results <= 0)
        return 0;

    /* Enumerate all keys in the vector bucket */
    int key_count = 0;
    char **keys = vfs_kv_keys(vs->vfs, vs->vec_bucket, &key_count);
    if (!keys || key_count == 0)
        return 0;

    /* Allocate temporary candidates (may exceed max_results) */
    int cap = key_count < 4096 ? key_count : 4096;
    vecstore_result_t *candidates = calloc((size_t)cap, sizeof(vecstore_result_t));
    if (!candidates) {
        for (int i = 0; i < key_count; i++)
            free(keys[i]);
        free(keys);
        return 0;
    }

    int n_cand = 0;
    for (int i = 0; i < key_count && n_cand < cap; i++) {
        size_t blob_len = 0;
        float *vec = (float *)vfs_kv_get(vs->vfs, vs->vec_bucket, keys[i], &blob_len);
        if (!vec) {
            free(keys[i]);
            keys[i] = NULL;
            continue;
        }

        int vec_dim = (int)(blob_len / sizeof(float));
        int cmp_dim = vec_dim < dim ? vec_dim : dim;

        float score = cosine_similarity_f(query_vec, vec, cmp_dim);
        free(vec);

        if (score > 0.01f) {
            candidates[n_cand].id = keys[i]; /* take ownership */
            candidates[n_cand].score = score;
            candidates[n_cand].metadata = NULL;
            keys[i] = NULL; /* mark as consumed */
            n_cand++;
        } else {
            free(keys[i]);
            keys[i] = NULL;
        }
    }

    /* Free any remaining unconsumed keys (from early cap cutoff) */
    for (int i = 0; i < key_count; i++) {
        free(keys[i]); /* safe: consumed entries are NULL */
    }
    free(keys);

    /* Sort by score descending */
    qsort(candidates, (size_t)n_cand, sizeof(vecstore_result_t), result_cmp_desc);

    /* Copy top results to output */
    int count = n_cand < max_results ? n_cand : max_results;
    for (int i = 0; i < count; i++) {
        out[i].id = candidates[i].id; /* transfer ownership */
        out[i].score = candidates[i].score;
        /* Load metadata lazily */
        out[i].metadata = vfs_kv_get_str(vs->vfs, vs->meta_bucket, out[i].id);
    }

    /* Free remaining candidates beyond top-k */
    for (int i = count; i < n_cand; i++) {
        free(candidates[i].id);
    }
    free(candidates);

    return count;
}

void vecstore_result_free(vecstore_result_t *results, int count) {
    if (!results)
        return;
    for (int i = 0; i < count; i++) {
        free(results[i].id);
        free(results[i].metadata);
    }
}

/* ── Count ────────────────────────────────────────────────────────────── */

int vecstore_count(vecstore_t *vs) {
    if (!vs)
        return 0;
    int count = 0;
    char **keys = vfs_kv_keys(vs->vfs, vs->vec_bucket, &count);
    if (keys) {
        for (int i = 0; i < count; i++)
            free(keys[i]);
        free(keys);
    }
    return count;
}
