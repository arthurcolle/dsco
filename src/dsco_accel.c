#define _DARWIN_C_SOURCE
#define _GNU_SOURCE
#include <unistd.h>
#include "dsco_accel.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__APPLE__)
#define DSCO_HAVE_ACCELERATE 1
#define ACCELERATE_NEW_LAPACK 1
#include <Accelerate/Accelerate.h>
#include <sys/sysctl.h>
#endif

#if defined(__aarch64__) || defined(__arm64__)
#include <arm_neon.h>
#elif defined(__x86_64__)
#include <emmintrin.h>
#endif

/* MLX is loaded by dsco_mlx.c at startup. It exposes a function that lets
 * us check whether MLX is live. We do not link MLX here. */
extern bool dsco_mlx_is_available(void);

/* ──────────────────────────────────────────────────────────────────────────
 *  State
 * ────────────────────────────────────────────────────────────────────────── */

static dsco_accel_info_t g_info = {0};

static int detect_cpu_count(void) {
#if defined(__APPLE__)
    int count = 0;
    size_t sz = sizeof(count);
    if (sysctlbyname("hw.perflevel0.logicalcpu", &count, &sz, NULL, 0) == 0 && count > 0)
        return count;
    if (sysctlbyname("hw.logicalcpu", &count, &sz, NULL, 0) == 0 && count > 0)
        return count;
#endif
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 4;
}

int dsco_accel_init(void) {
    memset(&g_info, 0, sizeof(g_info));
    g_info.cpu_count = detect_cpu_count();

#if defined(__aarch64__) || defined(__arm64__) || defined(__x86_64__)
    g_info.cpu_backend = DSCO_ACCEL_BACKEND_SIMD;
#else
    g_info.cpu_backend = DSCO_ACCEL_BACKEND_SCALAR;
#endif

#if DSCO_HAVE_ACCELERATE
    g_info.accelerate_present = true;
    g_info.batch_backend = DSCO_ACCEL_BACKEND_ACCELERATE;
#else
    g_info.batch_backend = g_info.cpu_backend;
#endif

    g_info.mlx_loaded = dsco_mlx_is_available();
    if (g_info.mlx_loaded) {
        g_info.batch_backend = DSCO_ACCEL_BACKEND_MLX;
    }

    const char *cpu_n = (g_info.cpu_backend == DSCO_ACCEL_BACKEND_SIMD) ?
#if defined(__aarch64__) || defined(__arm64__)
                                                                        "neon"
#else
                                                                        "sse2"
#endif
                                                                        : "scalar";
    const char *batch_n = "scalar";
    switch (g_info.batch_backend) {
        case DSCO_ACCEL_BACKEND_MLX:
            batch_n = "mlx";
            break;
        case DSCO_ACCEL_BACKEND_ACCELERATE:
            batch_n = "accelerate";
            break;
        case DSCO_ACCEL_BACKEND_SIMD:
            batch_n = cpu_n;
            break;
        default:
            break;
    }
    snprintf(g_info.banner, sizeof(g_info.banner), "dsco-accel: cpu=%s, batch=%s, %d cores%s",
             cpu_n, batch_n, g_info.cpu_count, g_info.mlx_loaded ? ", mlx" : "");
    return 0;
}

void dsco_accel_shutdown(void) {
    memset(&g_info, 0, sizeof(g_info));
}

const dsco_accel_info_t *dsco_accel_info(void) {
    return &g_info;
}

/* ──────────────────────────────────────────────────────────────────────────
 *  Single-vector ops
 * ────────────────────────────────────────────────────────────────────────── */

float dsco_accel_dot(const float *a, const float *b, int dim) {
    if (!a || !b || dim <= 0)
        return 0.0f;
#if DSCO_HAVE_ACCELERATE
    float r = 0.0f;
    vDSP_dotpr(a, 1, b, 1, &r, (vDSP_Length)dim);
    return r;
#elif defined(__aarch64__) || defined(__arm64__)
    int i = 0;
    float32x4_t acc = vdupq_n_f32(0.0f);
    for (; i + 16 <= dim; i += 16) {
        acc = vmlaq_f32(acc, vld1q_f32(a + i), vld1q_f32(b + i));
        acc = vmlaq_f32(acc, vld1q_f32(a + i + 4), vld1q_f32(b + i + 4));
        acc = vmlaq_f32(acc, vld1q_f32(a + i + 8), vld1q_f32(b + i + 8));
        acc = vmlaq_f32(acc, vld1q_f32(a + i + 12), vld1q_f32(b + i + 12));
    }
    for (; i + 4 <= dim; i += 4)
        acc = vmlaq_f32(acc, vld1q_f32(a + i), vld1q_f32(b + i));
    float r = vaddvq_f32(acc);
    for (; i < dim; i++)
        r += a[i] * b[i];
    return r;
#else
    float r = 0.0f;
    for (int i = 0; i < dim; i++)
        r += a[i] * b[i];
    return r;
#endif
}

float dsco_accel_l2norm(const float *a, int dim) {
    float d = dsco_accel_dot(a, a, dim);
    return sqrtf(d);
}

float dsco_accel_cosine(const float *a, const float *b, int dim) {
    float d = dsco_accel_dot(a, b, dim);
    float na = dsco_accel_l2norm(a, dim);
    float nb = dsco_accel_l2norm(b, dim);
    if (na == 0.0f || nb == 0.0f)
        return 0.0f;
    return d / (na * nb);
}

/* ──────────────────────────────────────────────────────────────────────────
 *  Batched ops — for non-MLX path. MLX path is in dsco_mlx.c.
 * ────────────────────────────────────────────────────────────────────────── */

extern int dsco_mlx_cosine_batch(const float *q, int dim, const float *cands, int n, float *out);

int dsco_accel_cosine_batch(const float *q, int dim, const float *cands, int n, float *out) {
    if (!q || !cands || !out || dim <= 0 || n <= 0)
        return -1;

    if (g_info.mlx_loaded && n >= 64) {
        int rc = dsco_mlx_cosine_batch(q, dim, cands, n, out);
        if (rc == 0)
            return 0;
        /* fall through to CPU if MLX path failed for any reason */
    }

#if DSCO_HAVE_ACCELERATE
    /* y = C * q  (where C is [n, dim] row-major) */
    cblas_sgemv(CblasRowMajor, CblasNoTrans, n, dim, 1.0f, cands, dim, q, 1, 0.0f, out, 1);
    /* normalize: out[i] /= ||cands[i]|| * ||q|| */
    float qn = dsco_accel_l2norm(q, dim);
    if (qn == 0.0f)
        qn = 1.0f;
    for (int i = 0; i < n; i++) {
        float cn = dsco_accel_l2norm(cands + (size_t)i * dim, dim);
        out[i] = (cn == 0.0f) ? 0.0f : out[i] / (qn * cn);
    }
    return 0;
#else
    float qn = dsco_accel_l2norm(q, dim);
    if (qn == 0.0f)
        qn = 1.0f;
    for (int i = 0; i < n; i++) {
        const float *c = cands + (size_t)i * dim;
        float d = dsco_accel_dot(q, c, dim);
        float cn = dsco_accel_l2norm(c, dim);
        out[i] = (cn == 0.0f) ? 0.0f : d / (qn * cn);
    }
    return 0;
#endif
}

int dsco_accel_gemv(const float *A, int m, int n, const float *x, float *y) {
    if (!A || !x || !y || m <= 0 || n <= 0)
        return -1;
#if DSCO_HAVE_ACCELERATE
    cblas_sgemv(CblasRowMajor, CblasNoTrans, m, n, 1.0f, A, n, x, 1, 0.0f, y, 1);
    return 0;
#else
    for (int i = 0; i < m; i++) {
        float r = 0.0f;
        const float *row = A + (size_t)i * n;
        for (int j = 0; j < n; j++)
            r += row[j] * x[j];
        y[i] = r;
    }
    return 0;
#endif
}

/* ──────────────────────────────────────────────────────────────────────────
 *  Softmax + reductions
 * ────────────────────────────────────────────────────────────────────────── */

float dsco_accel_softmax(float *x, int n) {
    if (!x || n <= 0)
        return 0.0f;
    float m = x[0];
    for (int i = 1; i < n; i++)
        if (x[i] > m)
            m = x[i];
    float sum = 0.0f;
    for (int i = 0; i < n; i++) {
        x[i] = expf(x[i] - m);
        sum += x[i];
    }
    float inv = sum > 0.0f ? 1.0f / sum : 0.0f;
    for (int i = 0; i < n; i++)
        x[i] *= inv;
    return sum;
}

void dsco_accel_log_softmax(float *x, int n) {
    if (!x || n <= 0)
        return;
    float m = x[0];
    for (int i = 1; i < n; i++)
        if (x[i] > m)
            m = x[i];
    float sum = 0.0f;
    for (int i = 0; i < n; i++)
        sum += expf(x[i] - m);
    float lz = m + logf(sum);
    for (int i = 0; i < n; i++)
        x[i] = x[i] - lz;
}

int dsco_accel_argmax(const float *x, int n) {
    if (!x || n <= 0)
        return -1;
    int idx = 0;
    float best = x[0];
    for (int i = 1; i < n; i++) {
        if (x[i] > best) {
            best = x[i];
            idx = i;
        }
    }
    return idx;
}

/* Top-k via small heap (k typically <= 32). */
int dsco_accel_topk(const float *x, int n, int k, int *out_idx, float *out_val) {
    if (!x || n <= 0 || k <= 0 || !out_idx || !out_val)
        return -1;
    if (k > n)
        k = n;
    /* min-heap of size k */
    for (int i = 0; i < k; i++) {
        out_idx[i] = i;
        out_val[i] = x[i];
    }
    /* heapify (sift down) */
    for (int start = (k - 2) / 2; start >= 0; start--) {
        int root = start;
        for (;;) {
            int child = 2 * root + 1;
            if (child >= k)
                break;
            if (child + 1 < k && out_val[child + 1] < out_val[child])
                child++;
            if (out_val[root] <= out_val[child])
                break;
            float tv = out_val[root];
            out_val[root] = out_val[child];
            out_val[child] = tv;
            int ti = out_idx[root];
            out_idx[root] = out_idx[child];
            out_idx[child] = ti;
            root = child;
        }
    }
    for (int i = k; i < n; i++) {
        if (x[i] > out_val[0]) {
            out_val[0] = x[i];
            out_idx[0] = i;
            int root = 0;
            for (;;) {
                int child = 2 * root + 1;
                if (child >= k)
                    break;
                if (child + 1 < k && out_val[child + 1] < out_val[child])
                    child++;
                if (out_val[root] <= out_val[child])
                    break;
                float tv = out_val[root];
                out_val[root] = out_val[child];
                out_val[child] = tv;
                int ti = out_idx[root];
                out_idx[root] = out_idx[child];
                out_idx[child] = ti;
                root = child;
            }
        }
    }
    /* sort descending */
    for (int i = 0; i < k - 1; i++) {
        int best = i;
        for (int j = i + 1; j < k; j++)
            if (out_val[j] > out_val[best])
                best = j;
        if (best != i) {
            float tv = out_val[i];
            out_val[i] = out_val[best];
            out_val[best] = tv;
            int ti = out_idx[i];
            out_idx[i] = out_idx[best];
            out_idx[best] = ti;
        }
    }
    return k;
}

/* ──────────────────────────────────────────────────────────────────────────
 *  Quantization
 * ────────────────────────────────────────────────────────────────────────── */

int dsco_accel_dequant_int8(const int8_t *src, const float *scales, int n, int dim, float *out) {
    if (!src || !scales || !out || n <= 0 || dim <= 0)
        return -1;
    for (int i = 0; i < n; i++) {
        float s = scales[i];
        const int8_t *row = src + (size_t)i * dim;
        float *o = out + (size_t)i * dim;
        for (int j = 0; j < dim; j++)
            o[j] = (float)row[j] * s;
    }
    return 0;
}

int dsco_accel_quant_int8(const float *src, int n, int dim, float *scales_out, int8_t *dst) {
    if (!src || !scales_out || !dst || n <= 0 || dim <= 0)
        return -1;
    for (int i = 0; i < n; i++) {
        const float *row = src + (size_t)i * dim;
        float amax = 0.0f;
        for (int j = 0; j < dim; j++) {
            float a = fabsf(row[j]);
            if (a > amax)
                amax = a;
        }
        float s = amax > 0.0f ? amax / 127.0f : 1.0f;
        scales_out[i] = s;
        float inv = 1.0f / s;
        int8_t *d = dst + (size_t)i * dim;
        for (int j = 0; j < dim; j++) {
            float v = row[j] * inv;
            if (v > 127.0f)
                v = 127.0f;
            if (v < -128.0f)
                v = -128.0f;
            d[j] = (int8_t)lrintf(v);
        }
    }
    return 0;
}
