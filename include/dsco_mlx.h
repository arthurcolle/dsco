#ifndef DSCO_MLX_H
#define DSCO_MLX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ──────────────────────────────────────────────────────────────────────────
 *  Native MLX bindings — runtime-loaded.
 *
 *  MLX is Apple's array framework for unified-memory M-series machines.
 *  We dlopen libmlxc.dylib (the official mlx-c bindings) at startup so the
 *  binary remains usable on systems without MLX. If MLX is present every
 *  batched op routes through it; otherwise calls return DSCO_MLX_UNAVAILABLE.
 *
 *  install on macOS:    brew install mlx        (or build mlx-c from source)
 *  env override:        DSCO_MLX_PATH=/path/to/libmlxc.dylib
 *
 *  We deliberately re-declare a *very small* subset of MLX's C API as
 *  function-pointer typedefs so this header has zero compile-time dependency
 *  on the MLX source tree.
 * ────────────────────────────────────────────────────────────────────────── */

#define DSCO_MLX_OK            0
#define DSCO_MLX_UNAVAILABLE  -1
#define DSCO_MLX_ERR          -2

/* ── Lifecycle ─────────────────────────────────────────────────────────── */
int  dsco_mlx_init(void);            /* dlopen + dlsym; returns 0 on success */
void dsco_mlx_shutdown(void);
bool dsco_mlx_is_available(void);
const char *dsco_mlx_library_path(void);  /* path of loaded dylib, or NULL */
const char *dsco_mlx_version(void);       /* MLX version string, or "unknown" */

/* ── Convenience routes — these are what the rest of dsco actually calls.
 *    They fall back to CPU implementations when MLX is absent. ─────────── */

/* Cosine similarity of q[dim] against cands[n*dim]. Returns DSCO_MLX_OK
 * iff MLX path was used. */
int dsco_mlx_cosine_batch(const float *q, int dim,
                           const float *cands, int n, float *out);

/* Matrix multiply: C[m,k] = A[m,n] * B[n,k]. Row-major. */
int dsco_mlx_matmul(const float *A, int m, int n,
                     const float *B, int k, float *C);

/* In-place softmax over last dim of x[batch, n]. */
int dsco_mlx_softmax(float *x, int batch, int n);

/* Argmax along last dim: x[batch, n] → idx[batch]. */
int dsco_mlx_argmax(const float *x, int batch, int n, int *idx);

/* L2-normalize each row of x[n, dim] in place. */
int dsco_mlx_l2norm_rows(float *x, int n, int dim);

#endif /* DSCO_MLX_H */
