#ifndef DSCO_SIMD_H
#define DSCO_SIMD_H

/* ──────────────────────────────────────────────────────────────────────────
 *  Intrinsics-direct SIMD primitives — no template abstraction.
 *
 *  Rationale: a std::simd-style wrapper hides operations from the optimizer
 *  and forces fixed-width semantics. We write the NEON / SSE2 path inline
 *  and keep a scalar fallback. The compiler sees the actual instructions,
 *  inlines aggressively, and produces the assembly we want.
 *
 *  Targets:
 *    - arm64 (Apple Silicon, Graviton): NEON 128-bit
 *    - x86-64 (any CPU since 2003):     SSE2 128-bit
 *    - everything else:                 scalar
 * ────────────────────────────────────────────────────────────────────────── */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(__aarch64__) || defined(__arm64__) || defined(_M_ARM64)
  #define DSCO_SIMD_NEON 1
  #include <arm_neon.h>
#elif defined(__x86_64__) || defined(_M_X64) || defined(__i386__)
  #define DSCO_SIMD_SSE2 1
  #include <emmintrin.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ── Find LAST occurrence of `needle` in [base, base+len).
 *    Returns offset from base, or -1 if not found.
 *    Used by render to walk newlines from the tail. ───────────────────────── */
static inline ssize_t dsco_simd_rfind_byte(const char *base, size_t len, char needle) {
    if (!base || len == 0) return -1;
    const uint8_t *p = (const uint8_t *)base;
    const uint8_t n = (uint8_t)needle;

#if DSCO_SIMD_NEON
    /* tail (unaligned remainder) first — walk back to a 16-byte boundary */
    size_t tail = len & 15;
    for (size_t i = 0; i < tail; i++) {
        if (p[len - 1 - i] == n) return (ssize_t)(len - 1 - i);
    }
    size_t blocks = len >> 4;   /* 16-byte blocks */
    uint8x16_t needle_v = vdupq_n_u8(n);
    for (size_t b = blocks; b > 0; b--) {
        size_t off = (b - 1) << 4;
        uint8x16_t v = vld1q_u8(p + off);
        uint8x16_t eq = vceqq_u8(v, needle_v);
        /* fast reject */
        if (vmaxvq_u8(eq) == 0) continue;
        /* fold to 64-bit lanes and pick highest set bit */
        uint64_t lo = vgetq_lane_u64(vreinterpretq_u64_u8(eq), 0);
        uint64_t hi = vgetq_lane_u64(vreinterpretq_u64_u8(eq), 1);
        /* find highest byte-position set in (hi<<64 | lo) */
        if (hi) {
            int bit = 63 - __builtin_clzll(hi);
            return (ssize_t)(off + 8 + (bit >> 3));
        }
        int bit = 63 - __builtin_clzll(lo);
        return (ssize_t)(off + (bit >> 3));
    }
    return -1;

#elif DSCO_SIMD_SSE2
    size_t tail = len & 15;
    for (size_t i = 0; i < tail; i++) {
        if (p[len - 1 - i] == n) return (ssize_t)(len - 1 - i);
    }
    size_t blocks = len >> 4;
    __m128i needle_v = _mm_set1_epi8((char)n);
    for (size_t b = blocks; b > 0; b--) {
        size_t off = (b - 1) << 4;
        __m128i v = _mm_loadu_si128((const __m128i *)(p + off));
        __m128i eq = _mm_cmpeq_epi8(v, needle_v);
        unsigned m = (unsigned)_mm_movemask_epi8(eq);
        if (!m) continue;
        int bit = 31 - __builtin_clz(m);
        return (ssize_t)(off + bit);
    }
    return -1;

#else
    for (ssize_t i = (ssize_t)len - 1; i >= 0; i--) {
        if (p[i] == n) return i;
    }
    return -1;
#endif
}

/* ── Find FIRST occurrence — like memchr but inlined. ──────────────────── */
static inline ssize_t dsco_simd_find_byte(const char *base, size_t len, char needle) {
    if (!base || len == 0) return -1;
    const uint8_t *p = (const uint8_t *)base;
    const uint8_t n = (uint8_t)needle;

#if DSCO_SIMD_NEON
    size_t i = 0;
    uint8x16_t needle_v = vdupq_n_u8(n);
    for (; i + 16 <= len; i += 16) {
        uint8x16_t v = vld1q_u8(p + i);
        uint8x16_t eq = vceqq_u8(v, needle_v);
        if (vmaxvq_u8(eq) == 0) continue;
        uint64_t lo = vgetq_lane_u64(vreinterpretq_u64_u8(eq), 0);
        uint64_t hi = vgetq_lane_u64(vreinterpretq_u64_u8(eq), 1);
        if (lo) {
            int bit = __builtin_ctzll(lo);
            return (ssize_t)(i + (bit >> 3));
        }
        int bit = __builtin_ctzll(hi);
        return (ssize_t)(i + 8 + (bit >> 3));
    }
    for (; i < len; i++) if (p[i] == n) return (ssize_t)i;
    return -1;

#elif DSCO_SIMD_SSE2
    size_t i = 0;
    __m128i needle_v = _mm_set1_epi8((char)n);
    for (; i + 16 <= len; i += 16) {
        __m128i v = _mm_loadu_si128((const __m128i *)(p + i));
        __m128i eq = _mm_cmpeq_epi8(v, needle_v);
        unsigned m = (unsigned)_mm_movemask_epi8(eq);
        if (!m) continue;
        int bit = __builtin_ctz(m);
        return (ssize_t)(i + bit);
    }
    for (; i < len; i++) if (p[i] == n) return (ssize_t)i;
    return -1;

#else
    for (size_t i = 0; i < len; i++) if (p[i] == n) return (ssize_t)i;
    return -1;
#endif
}

/* ── Count occurrences of `needle`. Used for line-count, token estimation. */
static inline size_t dsco_simd_count_byte(const char *base, size_t len, char needle) {
    if (!base || len == 0) return 0;
    const uint8_t *p = (const uint8_t *)base;
    const uint8_t n = (uint8_t)needle;
    size_t count = 0;

#if DSCO_SIMD_NEON
    size_t i = 0;
    uint8x16_t needle_v = vdupq_n_u8(n);
    /* accumulate matches in a per-lane counter; flush every 255 iters
     * to avoid u8 overflow. */
    for (; i + 16 <= len; ) {
        uint8x16_t acc = vdupq_n_u8(0);
        size_t blocks = (len - i) / 16;
        if (blocks > 255) blocks = 255;
        for (size_t b = 0; b < blocks; b++) {
            uint8x16_t v = vld1q_u8(p + i + (b << 4));
            uint8x16_t eq = vceqq_u8(v, needle_v);
            /* eq lanes are 0xff or 0x00; subtract to add 1 per match */
            acc = vsubq_u8(acc, eq);
        }
        /* horizontal sum of 16 u8 lanes */
        count += (size_t)vaddlvq_u8(acc);
        i += blocks << 4;
    }
    for (; i < len; i++) if (p[i] == n) count++;
    return count;

#elif DSCO_SIMD_SSE2
    size_t i = 0;
    __m128i needle_v = _mm_set1_epi8((char)n);
    for (; i + 16 <= len; i += 16) {
        __m128i v = _mm_loadu_si128((const __m128i *)(p + i));
        __m128i eq = _mm_cmpeq_epi8(v, needle_v);
        count += (size_t)__builtin_popcount((unsigned)_mm_movemask_epi8(eq));
    }
    for (; i < len; i++) if (p[i] == n) count++;
    return count;

#else
    for (size_t i = 0; i < len; i++) if (p[i] == n) count++;
    return count;
#endif
}

/* ── Walk backwards from end, find the start offset of the Nth newline-bounded
 *    line. Returns the byte offset where the last `n` lines begin.
 *    If fewer than `n` lines exist, returns 0 (start of buffer).
 *
 *    Use case: render snapshot, "show me the last 24 lines". ─────────────── */
static inline size_t dsco_simd_rline_start(const char *base, size_t len, size_t n) {
    if (!base || len == 0 || n == 0) return len;
    /* Step backward; each newline crossed counts. We want the offset right
     * after the (n+1)th newline from the tail (or 0 if not found). */
    size_t pos = len;
    size_t found = 0;
    while (pos > 0) {
        ssize_t nl = dsco_simd_rfind_byte(base, pos, '\n');
        if (nl < 0) return 0;
        /* don't count a trailing newline at end of buffer */
        if ((size_t)nl == len - 1 && found == 0) { pos = (size_t)nl; continue; }
        found++;
        if (found >= n) return (size_t)nl + 1;
        pos = (size_t)nl;
    }
    return 0;
}

#ifdef __cplusplus
}
#endif

#endif /* DSCO_SIMD_H */
