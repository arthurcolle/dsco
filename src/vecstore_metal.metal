/* Metal Shading Language — GPU cosine similarity kernel
 * Compiled at build time with xcrun metal → vecstore_metal.metallib
 *
 * vecstore_cosine: each thread computes similarity between the query vector
 * and one document vector.  Handles arbitrary dimension via a loop; the GPU
 * parallelizes across documents.
 */
#include <metal_stdlib>
using namespace metal;

kernel void vecstore_cosine(
    device const float *query    [[buffer(0)]],   /* [dim] */
    device const float *corpus   [[buffer(1)]],   /* [n_docs × dim] */
    device       float *scores   [[buffer(2)]],   /* [n_docs] output */
    constant     uint  &dim      [[buffer(3)]],
    constant     uint  &n_docs   [[buffer(4)]],
    uint gid [[thread_position_in_grid]])
{
    if (gid >= n_docs) return;

    float dot = 0.0f, q_sq = 0.0f, c_sq = 0.0f;
    uint base = gid * dim;
    for (uint i = 0; i < dim; i++) {
        float q = query[i];
        float c = corpus[base + i];
        dot  += q * c;
        q_sq += q * q;
        c_sq += c * c;
    }
    scores[gid] = dot / (sqrt(q_sq) * sqrt(c_sq) + 1e-8f);
}

/* Top-k selection: single thread, runs after cosine pass */
kernel void vecstore_topk(
    device const float *scores   [[buffer(0)]],
    device       uint  *indices  [[buffer(1)]],   /* [k] output */
    device       float *top_vals [[buffer(2)]],   /* [k] output */
    constant     uint  &n_docs   [[buffer(3)]],
    constant     uint  &k        [[buffer(4)]],
    uint gid [[thread_position_in_grid]])
{
    if (gid != 0) return;   /* single-threaded selection */
    for (uint j = 0; j < k && j < n_docs; j++) {
        float best = -2.0f;
        uint  best_i = 0;
        for (uint i = 0; i < n_docs; i++) {
            bool already = false;
            for (uint p = 0; p < j; p++) if (indices[p] == i) { already = true; break; }
            if (!already && scores[i] > best) { best = scores[i]; best_i = i; }
        }
        indices[j]  = best_i;
        top_vals[j] = best;
    }
}
