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
#include <sys/types.h> /* ssize_t */

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

/* ── Count bytes from `p` up to (not including) the first '"', '\\', or NUL.
 *    `p` must be NUL-terminated; no length is needed. Page-safe: reads only
 *    16-byte-ALIGNED blocks, masking off lanes before `p`, so it never touches a
 *    page the caller's own bytes don't already reach (the SIMD-strlen trick).
 *
 *    This is the decode fast path for parse_string: JSON string *values* pulled
 *    from responses are mostly escape-free plain text, so the run bulk-copies
 *    instead of appending one char at a time. */
static inline size_t dsco_simd_json_unescaped_run(const char *p) {
    const uint8_t *s = (const uint8_t *)p;
#if DSCO_SIMD_NEON
    const uint8x16_t q = vdupq_n_u8('"');
    const uint8x16_t bs = vdupq_n_u8('\\');
    const uint8x16_t z = vdupq_n_u8(0);
    uintptr_t addr = (uintptr_t)s;
    const uint8_t *base = (const uint8_t *)(addr & ~(uintptr_t)15);
    size_t off = (size_t)(addr - (uintptr_t)base);
    uint8x16_t v = vld1q_u8(base);
    uint8x16_t m = vorrq_u8(vorrq_u8(vceqq_u8(v, q), vceqq_u8(v, bs)), vceqq_u8(v, z));
    uint8x8_t nb = vshrn_n_u16(vreinterpretq_u16_u8(m), 4);
    uint64_t bits = vget_lane_u64(vreinterpret_u64_u8(nb), 0);
    bits &= (~0ULL << (off * 4)); /* ignore lanes before p */
    if (bits)
        return ((size_t)__builtin_ctzll(bits) >> 2) - off;
    for (const uint8_t *cur = base + 16;; cur += 16) {
        v = vld1q_u8(cur);
        m = vorrq_u8(vorrq_u8(vceqq_u8(v, q), vceqq_u8(v, bs)), vceqq_u8(v, z));
        if (vmaxvq_u8(m) == 0)
            continue;
        nb = vshrn_n_u16(vreinterpretq_u16_u8(m), 4);
        bits = vget_lane_u64(vreinterpret_u64_u8(nb), 0);
        return (size_t)(cur - s) + ((size_t)__builtin_ctzll(bits) >> 2);
    }
#elif DSCO_SIMD_SSE2
    const __m128i q = _mm_set1_epi8('"');
    const __m128i bs = _mm_set1_epi8('\\');
    const __m128i z = _mm_setzero_si128();
    uintptr_t addr = (uintptr_t)s;
    const uint8_t *base = (const uint8_t *)(addr & ~(uintptr_t)15);
    size_t off = (size_t)(addr - (uintptr_t)base);
    __m128i v = _mm_load_si128((const __m128i *)base);
    __m128i m = _mm_or_si128(_mm_or_si128(_mm_cmpeq_epi8(v, q), _mm_cmpeq_epi8(v, bs)),
                             _mm_cmpeq_epi8(v, z));
    unsigned bits = (unsigned)_mm_movemask_epi8(m) & (~0u << off);
    if (bits)
        return (size_t)__builtin_ctz(bits) - off;
    for (const uint8_t *cur = base + 16;; cur += 16) {
        v = _mm_load_si128((const __m128i *)cur);
        m = _mm_or_si128(_mm_or_si128(_mm_cmpeq_epi8(v, q), _mm_cmpeq_epi8(v, bs)),
                         _mm_cmpeq_epi8(v, z));
        unsigned b2 = (unsigned)_mm_movemask_epi8(m);
        if (!b2)
            continue;
        return (size_t)(cur - s) + (size_t)__builtin_ctz(b2);
    }
#else
    size_t i = 0;
    while (p[i] && p[i] != '"' && p[i] != '\\')
        i++;
    return i;
#endif
}

/* ── Count leading bytes in [base, base+len) that can be copied verbatim into a
 *    JSON string literal: printable ASCII (0x20..0x7E) except '"' and '\'.
 *    Stops at the first byte needing escaping (any control byte < 0x20, '"',
 *    '\\') or UTF-8 validation (>= 0x80). Returns that run length (0..len).
 *
 *    This is the fast path for jbuf_append_json_str: request bodies are built by
 *    escaping every message every turn, and almost all of that text is plain
 *    ASCII, so a SIMD run-scan turns a per-byte append loop into bulk memcpy. */
static inline size_t dsco_simd_json_safe_run(const char *base, size_t len) {
    const uint8_t *p = (const uint8_t *)base;
    size_t i = 0;

#if DSCO_SIMD_NEON
    const uint8x16_t lo = vdupq_n_u8(0x20);
    const uint8x16_t hi = vdupq_n_u8(0x7e);
    const uint8x16_t q = vdupq_n_u8('"');
    const uint8x16_t bs = vdupq_n_u8('\\');
    for (; i + 16 <= len; i += 16) {
        uint8x16_t v = vld1q_u8(p + i);
        uint8x16_t bad = vcltq_u8(v, lo);     /* < 0x20 (controls)      */
        bad = vorrq_u8(bad, vcgtq_u8(v, hi)); /* > 0x7e (>=0x80 + DEL)  */
        bad = vorrq_u8(bad, vceqq_u8(v, q));  /* '"'                    */
        bad = vorrq_u8(bad, vceqq_u8(v, bs)); /* '\\'                   */
        if (vmaxvq_u8(bad) == 0)
            continue;
        /* First bad lane: narrow each 0xFF lane to a nibble, ctz/4 = index. */
        uint8x8_t nb = vshrn_n_u16(vreinterpretq_u16_u8(bad), 4);
        uint64_t m = vget_lane_u64(vreinterpret_u64_u8(nb), 0);
        return i + ((size_t)__builtin_ctzll(m) >> 2);
    }
#elif DSCO_SIMD_SSE2
    /* Signed cmplt(v, 0x20) flags both controls (<0x20) and high bytes
     * (0x80..0xFF are negative in int8), covering the UTF-8 case for free. */
    const __m128i lo = _mm_set1_epi8(0x20);
    const __m128i q = _mm_set1_epi8('"');
    const __m128i bs = _mm_set1_epi8('\\');
    for (; i + 16 <= len; i += 16) {
        __m128i v = _mm_loadu_si128((const __m128i *)(p + i));
        __m128i bad = _mm_cmplt_epi8(v, lo);
        bad = _mm_or_si128(bad, _mm_cmpeq_epi8(v, q));
        bad = _mm_or_si128(bad, _mm_cmpeq_epi8(v, bs));
        unsigned m = (unsigned)_mm_movemask_epi8(bad);
        if (!m)
            continue;
        return i + (size_t)__builtin_ctz(m);
    }
#endif
    for (; i < len; i++) {
        uint8_t c = p[i];
        if (c < 0x20 || c > 0x7e || c == '"' || c == '\\')
            return i;
    }
    return i;
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
