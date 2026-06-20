#ifndef DSCO_ACCEL_H
#define DSCO_ACCEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ──────────────────────────────────────────────────────────────────────────
 *  Unified accelerator interface.
 *
 *  Backends, picked at init:
 *    1. MLX (Apple Silicon GPU + Neural Engine)  — runtime-loaded via dlopen
 *    2. Accelerate.framework (Apple — vDSP + cBLAS)
 *    3. NEON / SSE2 SIMD via include/simd.h
 *    4. Scalar
 *
 *  Routing policy: hot small kernels (dot product, single cosine) stay on
 *  CPU SIMD because dispatch overhead dominates. Batched workloads
 *  (>= 64 vectors of dim 384+) go to MLX if present, else Accelerate.
 * ────────────────────────────────────────────────────────────────────────── */

typedef enum {
    DSCO_ACCEL_BACKEND_SCALAR = 0,
    DSCO_ACCEL_BACKEND_SIMD,           /* include/simd.h primitives */
    DSCO_ACCEL_BACKEND_ACCELERATE,     /* Apple vDSP / cBLAS */
    DSCO_ACCEL_BACKEND_MLX,            /* Apple MLX (Metal + ANE) */
} dsco_accel_backend_t;

typedef struct {
    dsco_accel_backend_t cpu_backend;   /* picked for small ops */
    dsco_accel_backend_t batch_backend; /* picked for batched ops */
    bool                 mlx_loaded;
    bool                 accelerate_present;
    int                  cpu_count;
    char                 banner[128];   /* human-readable summary, e.g. "mlx + accel + neon, 12 cores" */
} dsco_accel_info_t;

/* ── Lifecycle ─────────────────────────────────────────────────────────── */
int  dsco_accel_init(void);
void dsco_accel_shutdown(void);
const dsco_accel_info_t *dsco_accel_info(void);

/* ── Single-vector ops ─────────────────────────────────────────────────── */
float dsco_accel_dot(const float *a, const float *b, int dim);
float dsco_accel_l2norm(const float *a, int dim);
float dsco_accel_cosine(const float *a, const float *b, int dim);

/* ── Batched ops ───────────────────────────────────────────────────────── */
/* Cosine similarity of a single query against N candidates.
 *   q     : [dim]
 *   cands : [n * dim], row-major
 *   out   : [n] — fills with cosine scores
 * Returns 0 on success. */
int dsco_accel_cosine_batch(const float *q, int dim,
                             const float *cands, int n,
                             float *out);

/* y = A * x where A is [m, n] row-major. */
int dsco_accel_gemv(const float *A, int m, int n,
                     const float *x, float *y);

/* In-place softmax over [n]. Returns sum-of-exps for entropy/diagnostics. */
float dsco_accel_softmax(float *x, int n);

/* In-place reverse softmax (subtract max for numerical stability). */
void  dsco_accel_log_softmax(float *x, int n);

/* Top-k indices + values, descending. out_idx/out_val are [k]. */
int dsco_accel_topk(const float *x, int n, int k,
                     int *out_idx, float *out_val);

int dsco_accel_argmax(const float *x, int n);

/* ── Quantized ops (int8 cosine for compressed embedding stores) ────────── */
/* Dequantize int8 to float using a per-row scale.
 *   src [n*dim] int8, scales [n], out [n*dim] float */
int dsco_accel_dequant_int8(const int8_t *src, const float *scales,
                             int n, int dim, float *out);

/* Quick float→int8 quantization with per-row scale.
 *   src [n*dim], scales_out [n], dst [n*dim] */
int dsco_accel_quant_int8(const float *src, int n, int dim,
                           float *scales_out, int8_t *dst);

#endif /* DSCO_ACCEL_H */
