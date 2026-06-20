#ifndef DSCO_VECSTORE_METAL_H
#define DSCO_VECSTORE_METAL_H

/* GPU-accelerated cosine similarity via Metal.
 * Falls back to CPU if Metal is unavailable or corpus < METAL_MIN_DOCS.
 *
 * vm_query_cosine(query, dim, corpus, n_docs, out_scores):
 *   Computes cosine(query, corpus[i]) for all i on the GPU.
 *   out_scores must be caller-allocated float[n_docs].
 *   Returns true on GPU success, false on CPU fallback.
 */

#include <stdbool.h>
#include <stddef.h>

#define METAL_MIN_DOCS 512   /* below this, CPU is faster due to dispatch overhead */

bool vm_metal_available(void);

bool vm_query_cosine(const float *query, unsigned int dim,
                     const float *corpus, unsigned int n_docs,
                     float *out_scores);

/* Top-k from pre-computed scores (CPU sort — fast for k≤100) */
void vm_topk(const float *scores, unsigned int n,
             unsigned int k, unsigned int *out_indices, float *out_vals);

#endif /* DSCO_VECSTORE_METAL_H */
