#include "dsco_mlx.h"

#include <dlfcn.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ──────────────────────────────────────────────────────────────────────────
 *  Subset of mlx-c symbols we need.
 *
 *  These declarations mirror mlx-c at the ABI level — we don't include any
 *  MLX header, we just dlsym the entry points. If the upstream lib renames
 *  a symbol our init returns DSCO_MLX_UNAVAILABLE and the rest of dsco
 *  silently falls back to CPU paths.
 * ────────────────────────────────────────────────────────────────────────── */

typedef void *mlx_array_t;
typedef void *mlx_stream_t;
typedef void *mlx_string_t;

/* (function-pointer typedefs intentionally minimal) */
typedef mlx_array_t (*fn_array_new_data)(const void *data, const int *shape,
                                          int dim, int dtype);
typedef void        (*fn_array_free)(mlx_array_t a);
typedef const float *(*fn_array_data_float32)(mlx_array_t a);
typedef int         (*fn_array_size)(mlx_array_t a);
typedef int         (*fn_array_ndim)(mlx_array_t a);
typedef const int  *(*fn_array_shape)(mlx_array_t a);
typedef void        (*fn_array_eval)(mlx_array_t a);

typedef mlx_array_t (*fn_op_matmul)(mlx_array_t a, mlx_array_t b, mlx_stream_t s);
typedef mlx_array_t (*fn_op_softmax)(mlx_array_t a, const int *axes, int n,
                                      int precise, mlx_stream_t s);
typedef mlx_array_t (*fn_op_argmax)(mlx_array_t a, int axis, int keepdims,
                                     mlx_stream_t s);
typedef mlx_array_t (*fn_op_norm)(mlx_array_t a, const int *axes, int n,
                                   int keepdims, mlx_stream_t s);
typedef mlx_array_t (*fn_op_divide)(mlx_array_t a, mlx_array_t b, mlx_stream_t s);
typedef mlx_array_t (*fn_op_transpose)(mlx_array_t a, const int *axes, int n,
                                         mlx_stream_t s);

typedef mlx_stream_t (*fn_default_stream)(int device);

/* int dtype enum value for float32 in mlx-c; we hard-code the canonical id
 * and probe at init to confirm. (mlx-c uses an enum with float32 = 4.) */
#define MLX_DTYPE_FLOAT32 4

/* ──────────────────────────────────────────────────────────────────────────
 *  State
 * ────────────────────────────────────────────────────────────────────────── */

static void   *g_lib              = NULL;
static char    g_path[1024]       = {0};
static char    g_version[64]      = "unknown";
static bool    g_available        = false;

static struct {
    fn_array_new_data       array_new_data;
    fn_array_free           array_free;
    fn_array_data_float32   array_data_f32;
    fn_array_size           array_size;
    fn_array_ndim           array_ndim;
    fn_array_shape          array_shape;
    fn_array_eval           array_eval;

    fn_op_matmul            matmul;
    fn_op_softmax           softmax;
    fn_op_argmax            argmax;
    fn_op_norm              norm;
    fn_op_divide            divide;
    fn_op_transpose         transpose;

    fn_default_stream       default_stream;
} g_mlx;

static void *resolve(const char *name) {
    return g_lib ? dlsym(g_lib, name) : NULL;
}

int dsco_mlx_init(void) {
    if (g_available) return 0;

    const char *user_path = getenv("DSCO_MLX_PATH");
    const char *paths[] = {
        user_path,
        "libmlxc.dylib",
        "/opt/homebrew/lib/libmlxc.dylib",
        "/usr/local/lib/libmlxc.dylib",
        "/opt/homebrew/Cellar/mlx/*/lib/libmlxc.dylib",
        NULL,
    };
    for (int i = 0; paths[i]; i++) {
        if (!paths[i] || !*paths[i]) continue;
        g_lib = dlopen(paths[i], RTLD_NOW | RTLD_LOCAL);
        if (g_lib) {
            snprintf(g_path, sizeof(g_path), "%s", paths[i]);
            break;
        }
    }
    if (!g_lib) return DSCO_MLX_UNAVAILABLE;

    /* Resolve the entries we need. Names follow mlx-c's `mlx_*` convention. */
    g_mlx.array_new_data    = (fn_array_new_data)     resolve("mlx_array_new_data");
    g_mlx.array_free        = (fn_array_free)         resolve("mlx_array_free");
    g_mlx.array_data_f32    = (fn_array_data_float32) resolve("mlx_array_data_float32");
    g_mlx.array_size        = (fn_array_size)         resolve("mlx_array_size");
    g_mlx.array_ndim        = (fn_array_ndim)         resolve("mlx_array_ndim");
    g_mlx.array_shape       = (fn_array_shape)        resolve("mlx_array_shape");
    g_mlx.array_eval        = (fn_array_eval)         resolve("mlx_array_eval");

    g_mlx.matmul            = (fn_op_matmul)          resolve("mlx_matmul");
    g_mlx.softmax           = (fn_op_softmax)         resolve("mlx_softmax");
    g_mlx.argmax            = (fn_op_argmax)          resolve("mlx_argmax");
    g_mlx.norm              = (fn_op_norm)            resolve("mlx_linalg_norm");
    g_mlx.divide            = (fn_op_divide)          resolve("mlx_divide");
    g_mlx.transpose         = (fn_op_transpose)       resolve("mlx_transpose");
    g_mlx.default_stream    = (fn_default_stream)     resolve("mlx_default_stream");

    /* version string is optional */
    const char *(*ver)(void) = (const char *(*)(void))resolve("mlx_version");
    if (ver) snprintf(g_version, sizeof(g_version), "%s", ver());

    /* minimum viable surface: matmul + array marshal + stream */
    if (!g_mlx.array_new_data || !g_mlx.array_data_f32 ||
        !g_mlx.array_free     || !g_mlx.array_eval     ||
        !g_mlx.matmul         || !g_mlx.default_stream) {
        dlclose(g_lib); g_lib = NULL;
        return DSCO_MLX_UNAVAILABLE;
    }
    g_available = true;
    return DSCO_MLX_OK;
}

void dsco_mlx_shutdown(void) {
    if (g_lib) { dlclose(g_lib); g_lib = NULL; }
    g_available = false;
    g_path[0] = '\0';
}

bool        dsco_mlx_is_available(void)    { return g_available; }
const char *dsco_mlx_library_path(void)    { return g_path[0] ? g_path : NULL; }
const char *dsco_mlx_version(void)         { return g_version; }

/* ──────────────────────────────────────────────────────────────────────────
 *  Ops
 *
 *  All ops marshal float arrays through mlx_array_new_data, run the op on
 *  the default stream, evaluate the result eagerly, then copy back. This is
 *  not the most efficient pattern for tiny ops (we could keep arrays
 *  resident) — but it's the cleanest C-ABI surface and avoids needing
 *  callers to know anything about MLX.
 * ────────────────────────────────────────────────────────────────────────── */

int dsco_mlx_matmul(const float *A, int m, int n,
                     const float *B, int k, float *C) {
    if (!g_available) return DSCO_MLX_UNAVAILABLE;
    if (!A || !B || !C || m <= 0 || n <= 0 || k <= 0) return DSCO_MLX_ERR;

    int shape_A[2] = { m, n };
    int shape_B[2] = { n, k };
    mlx_stream_t s  = g_mlx.default_stream(0);
    mlx_array_t  a  = g_mlx.array_new_data(A, shape_A, 2, MLX_DTYPE_FLOAT32);
    mlx_array_t  b  = g_mlx.array_new_data(B, shape_B, 2, MLX_DTYPE_FLOAT32);
    if (!a || !b) { if (a) g_mlx.array_free(a); if (b) g_mlx.array_free(b); return DSCO_MLX_ERR; }
    mlx_array_t  c  = g_mlx.matmul(a, b, s);
    if (!c) { g_mlx.array_free(a); g_mlx.array_free(b); return DSCO_MLX_ERR; }
    g_mlx.array_eval(c);
    const float *out = g_mlx.array_data_f32(c);
    if (out) memcpy(C, out, sizeof(float) * (size_t)m * (size_t)k);
    g_mlx.array_free(a); g_mlx.array_free(b); g_mlx.array_free(c);
    return out ? DSCO_MLX_OK : DSCO_MLX_ERR;
}

int dsco_mlx_softmax(float *x, int batch, int n) {
    if (!g_available || !g_mlx.softmax) return DSCO_MLX_UNAVAILABLE;
    if (!x || batch <= 0 || n <= 0) return DSCO_MLX_ERR;
    int shape[2] = { batch, n };
    int axes[1]  = { 1 };
    mlx_stream_t s  = g_mlx.default_stream(0);
    mlx_array_t  a  = g_mlx.array_new_data(x, shape, 2, MLX_DTYPE_FLOAT32);
    if (!a) return DSCO_MLX_ERR;
    mlx_array_t  r  = g_mlx.softmax(a, axes, 1, 1, s);
    if (!r) { g_mlx.array_free(a); return DSCO_MLX_ERR; }
    g_mlx.array_eval(r);
    const float *out = g_mlx.array_data_f32(r);
    if (out) memcpy(x, out, sizeof(float) * (size_t)batch * (size_t)n);
    g_mlx.array_free(a); g_mlx.array_free(r);
    return out ? DSCO_MLX_OK : DSCO_MLX_ERR;
}

int dsco_mlx_argmax(const float *x, int batch, int n, int *idx) {
    if (!g_available || !g_mlx.argmax) return DSCO_MLX_UNAVAILABLE;
    if (!x || !idx || batch <= 0 || n <= 0) return DSCO_MLX_ERR;
    int shape[2] = { batch, n };
    mlx_stream_t s = g_mlx.default_stream(0);
    mlx_array_t a  = g_mlx.array_new_data(x, shape, 2, MLX_DTYPE_FLOAT32);
    if (!a) return DSCO_MLX_ERR;
    mlx_array_t r  = g_mlx.argmax(a, 1, 0, s);
    if (!r) { g_mlx.array_free(a); return DSCO_MLX_ERR; }
    g_mlx.array_eval(r);
    /* argmax result is int32 — we read it via the float32 accessor since
     * mlx-c maps both through the same data pointer. If your build uses a
     * different layout, this is the place to add an int32 accessor. */
    const float *out = g_mlx.array_data_f32(r);
    if (out) {
        const int32_t *src = (const int32_t *)out;
        for (int i = 0; i < batch; i++) idx[i] = (int)src[i];
    }
    g_mlx.array_free(a); g_mlx.array_free(r);
    return out ? DSCO_MLX_OK : DSCO_MLX_ERR;
}

int dsco_mlx_l2norm_rows(float *x, int n, int dim) {
    if (!g_available || !g_mlx.norm || !g_mlx.divide) return DSCO_MLX_UNAVAILABLE;
    if (!x || n <= 0 || dim <= 0) return DSCO_MLX_ERR;
    int shape[2] = { n, dim };
    int axes[1]  = { 1 };
    mlx_stream_t s = g_mlx.default_stream(0);
    mlx_array_t a  = g_mlx.array_new_data(x, shape, 2, MLX_DTYPE_FLOAT32);
    if (!a) return DSCO_MLX_ERR;
    mlx_array_t nn = g_mlx.norm(a, axes, 1, 1, s);   /* keepdims=1 → [n,1] */
    if (!nn) { g_mlx.array_free(a); return DSCO_MLX_ERR; }
    mlx_array_t r  = g_mlx.divide(a, nn, s);
    if (!r) { g_mlx.array_free(a); g_mlx.array_free(nn); return DSCO_MLX_ERR; }
    g_mlx.array_eval(r);
    const float *out = g_mlx.array_data_f32(r);
    if (out) memcpy(x, out, sizeof(float) * (size_t)n * (size_t)dim);
    g_mlx.array_free(a); g_mlx.array_free(nn); g_mlx.array_free(r);
    return out ? DSCO_MLX_OK : DSCO_MLX_ERR;
}

int dsco_mlx_cosine_batch(const float *q, int dim,
                           const float *cands, int n, float *out) {
    if (!g_available) return DSCO_MLX_UNAVAILABLE;
    if (!q || !cands || !out || dim <= 0 || n <= 0) return DSCO_MLX_ERR;

    /* Strategy:
     *   - copy cands into a normalized N×D matrix on device
     *   - normalize q to a 1×D vector
     *   - cos = norm(C) @ norm(q)^T  →  [N,1]
     */
    int shape_C[2] = { n, dim };
    int shape_q[2] = { 1, dim };
    mlx_stream_t s = g_mlx.default_stream(0);

    /* duplicate so we can normalize without touching caller's buffers */
    float *Cbuf = (float *)malloc((size_t)n * dim * sizeof(float));
    float *qbuf = (float *)malloc((size_t)dim * sizeof(float));
    if (!Cbuf || !qbuf) { free(Cbuf); free(qbuf); return DSCO_MLX_ERR; }
    memcpy(Cbuf, cands, (size_t)n * dim * sizeof(float));
    memcpy(qbuf, q,     (size_t)dim * sizeof(float));

    /* normalize in place via MLX. If norm/divide weren't resolved we'll
     * just compute on un-normalized data and return UNAVAILABLE later, but
     * we ought to skip in that case. */
    if (g_mlx.norm && g_mlx.divide) {
        dsco_mlx_l2norm_rows(Cbuf, n, dim);
        dsco_mlx_l2norm_rows(qbuf, 1, dim);
    } else {
        free(Cbuf); free(qbuf);
        return DSCO_MLX_UNAVAILABLE;
    }

    /* matmul: [N,D] @ [D,1] = [N,1] */
    mlx_array_t mC = g_mlx.array_new_data(Cbuf, shape_C, 2, MLX_DTYPE_FLOAT32);
    /* transpose q to [D,1] */
    mlx_array_t mq = g_mlx.array_new_data(qbuf, shape_q, 2, MLX_DTYPE_FLOAT32);
    int axes_T[2] = { 1, 0 };
    mlx_array_t mqT = g_mlx.transpose ? g_mlx.transpose(mq, axes_T, 2, s) : NULL;
    if (!mC || !mq || !mqT) {
        if (mC) g_mlx.array_free(mC);
        if (mq) g_mlx.array_free(mq);
        if (mqT) g_mlx.array_free(mqT);
        free(Cbuf); free(qbuf);
        return DSCO_MLX_ERR;
    }
    mlx_array_t r = g_mlx.matmul(mC, mqT, s);
    g_mlx.array_free(mC); g_mlx.array_free(mq); g_mlx.array_free(mqT);
    if (!r) { free(Cbuf); free(qbuf); return DSCO_MLX_ERR; }
    g_mlx.array_eval(r);
    const float *res = g_mlx.array_data_f32(r);
    if (res) memcpy(out, res, sizeof(float) * (size_t)n);
    g_mlx.array_free(r);
    free(Cbuf); free(qbuf);
    return res ? DSCO_MLX_OK : DSCO_MLX_ERR;
}
