#ifndef DSCO_C_H
#define DSCO_C_H

/* DSCO's C capability layer.
 *
 * Standard dates do not prove compiler support. This header combines the
 * language epoch with compiler feature probes, then exposes one vocabulary to
 * the rest of the runtime. The fallback path stays valid C11; the fast path
 * uses the C23/C2y and Clang/GCC features available in the active build. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#ifndef __has_attribute
#define __has_attribute(x) 0
#endif
#ifndef __has_builtin
#define __has_builtin(x) 0
#endif
#ifndef __has_c_attribute
#define __has_c_attribute(x) 0
#endif
#ifndef __has_extension
#define __has_extension(x) 0
#endif

#if defined(__STDC_VERSION__)
#define DSCO_C_VERSION __STDC_VERSION__
#else
#define DSCO_C_VERSION 0L
#endif

#define DSCO_C89 1
#define DSCO_C95 (DSCO_C_VERSION >= 199409L)
#define DSCO_C99 (DSCO_C_VERSION >= 199901L)
#define DSCO_C11 (DSCO_C_VERSION >= 201112L)
#define DSCO_C17 (DSCO_C_VERSION >= 201710L)
#define DSCO_C23 (DSCO_C_VERSION >= 202311L)
#define DSCO_C2Y (DSCO_C_VERSION >= 202400L)

#if defined(__clang__)
#define DSCO_COMPILER_CLANG 1
#define DSCO_COMPILER_GCC 0
#define DSCO_COMPILER_MSVC 0
#define DSCO_COMPILER_NAME "clang"
#define DSCO_COMPILER_VERSION ((__clang_major__ * 100) + __clang_minor__)
#elif defined(__GNUC__)
#define DSCO_COMPILER_CLANG 0
#define DSCO_COMPILER_GCC 1
#define DSCO_COMPILER_MSVC 0
#define DSCO_COMPILER_NAME "gcc"
#define DSCO_COMPILER_VERSION ((__GNUC__ * 100) + __GNUC_MINOR__)
#elif defined(_MSC_VER)
#define DSCO_COMPILER_CLANG 0
#define DSCO_COMPILER_GCC 0
#define DSCO_COMPILER_MSVC 1
#define DSCO_COMPILER_NAME "msvc"
#define DSCO_COMPILER_VERSION _MSC_VER
#else
#define DSCO_COMPILER_CLANG 0
#define DSCO_COMPILER_GCC 0
#define DSCO_COMPILER_MSVC 0
#define DSCO_COMPILER_NAME "unknown"
#define DSCO_COMPILER_VERSION 0
#endif

#if DSCO_C2Y
#define DSCO_C_STANDARD_NAME "C2y-draft"
#elif DSCO_C23
#define DSCO_C_STANDARD_NAME "C23"
#elif DSCO_C17
#define DSCO_C_STANDARD_NAME "C17"
#elif DSCO_C11
#define DSCO_C_STANDARD_NAME "C11"
#elif DSCO_C99
#define DSCO_C_STANDARD_NAME "C99"
#elif DSCO_C95
#define DSCO_C_STANDARD_NAME "C95"
#else
#define DSCO_C_STANDARD_NAME "C89-or-earlier"
#endif

/* Language and library facilities, organized by the epoch that introduced
 * them. These flags are integer constant expressions. */
#define DSCO_HAS_PROTOTYPES 1
#define DSCO_HAS_CONST_VOLATILE 1
#define DSCO_HAS_WIDE_CHARS DSCO_C95
#define DSCO_HAS_INLINE DSCO_C99
#define DSCO_HAS_RESTRICT DSCO_C99
#define DSCO_HAS_DESIGNATED_INITIALIZERS DSCO_C99
#define DSCO_HAS_COMPOUND_LITERALS DSCO_C99
#define DSCO_HAS_FLEXIBLE_ARRAYS DSCO_C99
#define DSCO_HAS_VARIADIC_MACROS DSCO_C99
#define DSCO_HAS_BOOL DSCO_C99
#define DSCO_HAS_GENERIC DSCO_C11
#define DSCO_HAS_STATIC_ASSERT DSCO_C11
#define DSCO_HAS_ALIGNMENT DSCO_C11
#if DSCO_C11 && !defined(__STDC_NO_ATOMICS__)
#define DSCO_HAS_ATOMICS 1
#else
#define DSCO_HAS_ATOMICS 0
#endif
#if DSCO_C11 && !defined(__STDC_NO_THREADS__)
#define DSCO_HAS_THREADS 1
#else
#define DSCO_HAS_THREADS 0
#endif
#define DSCO_HAS_THREAD_LOCAL DSCO_C11
#define DSCO_HAS_NORETURN DSCO_C11

#if DSCO_C23 || DSCO_COMPILER_CLANG || DSCO_COMPILER_GCC
#define DSCO_HAS_TYPEOF 1
#define DSCO_HAS_TYPEOF_UNQUAL (DSCO_C23 || (DSCO_COMPILER_CLANG && __clang_major__ >= 16))
#else
#define DSCO_HAS_TYPEOF 0
#define DSCO_HAS_TYPEOF_UNQUAL 0
#endif

#if DSCO_C23 || (DSCO_COMPILER_CLANG && __clang_major__ >= 12) ||                                  \
    (DSCO_COMPILER_GCC && __GNUC__ >= 8)
#define DSCO_HAS_VA_OPT 1
#else
#define DSCO_HAS_VA_OPT 0
#endif

#define DSCO_HAS_ATTRIBUTES                                                                        \
    (DSCO_C23 || DSCO_COMPILER_CLANG || DSCO_COMPILER_GCC || DSCO_COMPILER_MSVC)

#if defined(__BITINT_MAXWIDTH__)
#define DSCO_HAS_BITINT 1
#define DSCO_BITINT_MAXWIDTH __BITINT_MAXWIDTH__
#else
#define DSCO_HAS_BITINT 0
#define DSCO_BITINT_MAXWIDTH 0
#endif

/* Apple Clang 21 exposes the complete C2y spelling set used here. GCC and
 * older Clang builds take the proven extension/fallback route. */
#if DSCO_C2Y && DSCO_COMPILER_CLANG && __clang_major__ >= 19
#define DSCO_HAS_AUTO_INFERENCE 1
#define DSCO_HAS_NULLPTR 1
#define DSCO_HAS_CONSTEXPR 1
#else
#define DSCO_HAS_AUTO_INFERENCE 0
#define DSCO_HAS_NULLPTR 0
#define DSCO_HAS_CONSTEXPR 0
#endif

#if defined(__has_embed)
#define DSCO_HAS_EMBED 1
#else
#define DSCO_HAS_EMBED 0
#endif

#if defined(__has_include)
#if __has_include(<stdckdint.h>)
#include <stdckdint.h>
#define DSCO_HAS_STDCKDINT 1
#else
#define DSCO_HAS_STDCKDINT 0
#endif
#if __has_include(<stdbit.h>)
#include <stdbit.h>
#define DSCO_HAS_STDBIT 1
#else
#define DSCO_HAS_STDBIT 0
#endif
#else
#define DSCO_HAS_STDCKDINT 0
#define DSCO_HAS_STDBIT 0
#endif

/* Attributes and optimizer contracts. */
#if __has_c_attribute(nodiscard)
#define DSCO_NODISCARD [[nodiscard]]
#elif __has_attribute(warn_unused_result)
#define DSCO_NODISCARD __attribute__((warn_unused_result))
#else
#define DSCO_NODISCARD
#endif

#if __has_c_attribute(maybe_unused)
#define DSCO_MAYBE_UNUSED [[maybe_unused]]
#elif __has_attribute(unused)
#define DSCO_MAYBE_UNUSED __attribute__((unused))
#else
#define DSCO_MAYBE_UNUSED
#endif

#if __has_c_attribute(deprecated)
#define DSCO_DEPRECATED(message) [[deprecated(message)]]
#elif __has_attribute(deprecated)
#define DSCO_DEPRECATED(message) __attribute__((deprecated(message)))
#else
#define DSCO_DEPRECATED(message)
#endif

#if __has_c_attribute(fallthrough)
#define DSCO_FALLTHROUGH [[fallthrough]]
#elif __has_attribute(fallthrough)
#define DSCO_FALLTHROUGH __attribute__((fallthrough))
#else
#define DSCO_FALLTHROUGH ((void)0)
#endif

#if __has_attribute(format)
#define DSCO_PRINTF(format_index, first_arg)                                                       \
    __attribute__((format(printf, format_index, first_arg)))
#else
#define DSCO_PRINTF(format_index, first_arg)
#endif

#if __has_attribute(nonnull)
#define DSCO_NONNULL(...) __attribute__((nonnull(__VA_ARGS__)))
#else
#define DSCO_NONNULL(...)
#endif

#if __has_attribute(cleanup)
#define DSCO_HAS_CLEANUP 1
#define DSCO_CLEANUP(function) __attribute__((cleanup(function)))
#else
#define DSCO_HAS_CLEANUP 0
#define DSCO_CLEANUP(function)
#endif

#if __has_attribute(always_inline)
#define DSCO_ALWAYS_INLINE inline __attribute__((always_inline))
#else
#define DSCO_ALWAYS_INLINE inline
#endif
#if __has_attribute(noinline)
#define DSCO_NOINLINE __attribute__((noinline))
#else
#define DSCO_NOINLINE
#endif
#if __has_attribute(hot)
#define DSCO_HOT __attribute__((hot))
#else
#define DSCO_HOT
#endif
#if __has_attribute(cold)
#define DSCO_COLD __attribute__((cold))
#else
#define DSCO_COLD
#endif

#if DSCO_C11
#define DSCO_STATIC_ASSERT(condition, message) _Static_assert(condition, message)
#define DSCO_ALIGNAS(bytes) _Alignas(bytes)
#define DSCO_ALIGNOF(type) _Alignof(type)
#define DSCO_THREAD_LOCAL _Thread_local
#define DSCO_NORETURN _Noreturn
#else
#define DSCO_STATIC_ASSERT_JOIN_(a, b) a##b
#define DSCO_STATIC_ASSERT_JOIN(a, b) DSCO_STATIC_ASSERT_JOIN_(a, b)
#define DSCO_STATIC_ASSERT(condition, message)                                                     \
    typedef char DSCO_STATIC_ASSERT_JOIN(dsco_static_assert_, __LINE__)[(condition) ? 1 : -1]
#define DSCO_ALIGNAS(bytes)
#define DSCO_ALIGNOF(type) sizeof(type)
#define DSCO_THREAD_LOCAL
#define DSCO_NORETURN
#endif

#if DSCO_HAS_TYPEOF
#define DSCO_TYPEOF(expression) typeof(expression)
#else
#define DSCO_TYPEOF(expression) void *
#endif
#if DSCO_HAS_TYPEOF_UNQUAL
#define DSCO_TYPEOF_UNQUAL(expression) typeof_unqual(expression)
#else
#define DSCO_TYPEOF_UNQUAL(expression) DSCO_TYPEOF(expression)
#endif

#if DSCO_HAS_AUTO_INFERENCE
#define DSCO_AUTO(name, expression) auto name = (expression)
#elif DSCO_COMPILER_CLANG || DSCO_COMPILER_GCC
#define DSCO_AUTO(name, expression) __auto_type name = (expression)
#else
#define DSCO_AUTO(name, expression) DSCO_TYPEOF(expression) name = (expression)
#endif

#if DSCO_HAS_NULLPTR
#define DSCO_NULLPTR nullptr
#else
#define DSCO_NULLPTR NULL
#endif

#if DSCO_HAS_CONSTEXPR
#define DSCO_CONSTEXPR constexpr
#else
#define DSCO_CONSTEXPR const
#endif

#if __has_builtin(__builtin_expect) || DSCO_COMPILER_GCC
#define DSCO_LIKELY(condition) __builtin_expect(!!(condition), 1)
#define DSCO_UNLIKELY(condition) __builtin_expect(!!(condition), 0)
#else
#define DSCO_LIKELY(condition) (!!(condition))
#define DSCO_UNLIKELY(condition) (!!(condition))
#endif

#if __has_builtin(__builtin_unreachable) || DSCO_COMPILER_GCC
#define DSCO_UNREACHABLE() __builtin_unreachable()
#else
#define DSCO_UNREACHABLE() abort()
#endif

#if __has_builtin(__builtin_assume)
#define DSCO_ASSUME(condition) __builtin_assume(condition)
#else
#define DSCO_ASSUME(condition)                                                                     \
    do {                                                                                           \
        if (!(condition))                                                                          \
            DSCO_UNREACHABLE();                                                                    \
    } while (0)
#endif

/* Checked size arithmetic powers every generated collection. */
static inline bool dsco_size_add_overflow(size_t a, size_t b, size_t *out) {
#if DSCO_HAS_STDCKDINT
    return ckd_add(out, a, b);
#elif __has_builtin(__builtin_add_overflow) || DSCO_COMPILER_GCC
    return __builtin_add_overflow(a, b, out);
#else
    if (a > SIZE_MAX - b)
        return true;
    *out = a + b;
    return false;
#endif
}

static inline bool dsco_size_mul_overflow(size_t a, size_t b, size_t *out) {
#if DSCO_HAS_STDCKDINT
    return ckd_mul(out, a, b);
#elif __has_builtin(__builtin_mul_overflow) || DSCO_COMPILER_GCC
    return __builtin_mul_overflow(a, b, out);
#else
    if (a && b > SIZE_MAX / a)
        return true;
    *out = a * b;
    return false;
#endif
}

static inline unsigned dsco_popcount64(uint64_t value) {
#if DSCO_HAS_STDBIT
    return stdc_count_ones(value);
#elif __has_builtin(__builtin_popcountll) || DSCO_COMPILER_GCC
    return (unsigned)__builtin_popcountll(value);
#else
    unsigned count = 0;
    while (value) {
        value &= value - 1;
        count++;
    }
    return count;
#endif
}

enum {
    DSCO_C_FEATURE_C99 = UINT64_C(1) << 0,
    DSCO_C_FEATURE_C11 = UINT64_C(1) << 1,
    DSCO_C_FEATURE_C17 = UINT64_C(1) << 2,
    DSCO_C_FEATURE_C23 = UINT64_C(1) << 3,
    DSCO_C_FEATURE_C2Y = UINT64_C(1) << 4,
    DSCO_C_FEATURE_GENERIC = UINT64_C(1) << 5,
    DSCO_C_FEATURE_ATOMICS = UINT64_C(1) << 6,
    DSCO_C_FEATURE_THREADS = UINT64_C(1) << 7,
    DSCO_C_FEATURE_TYPEOF = UINT64_C(1) << 8,
    DSCO_C_FEATURE_VA_OPT = UINT64_C(1) << 9,
    DSCO_C_FEATURE_ATTRIBUTES = UINT64_C(1) << 10,
    DSCO_C_FEATURE_BITINT = UINT64_C(1) << 11,
    DSCO_C_FEATURE_AUTO = UINT64_C(1) << 12,
    DSCO_C_FEATURE_NULLPTR = UINT64_C(1) << 13,
    DSCO_C_FEATURE_CONSTEXPR = UINT64_C(1) << 14,
    DSCO_C_FEATURE_EMBED = UINT64_C(1) << 15,
    DSCO_C_FEATURE_CHECKED_INT = UINT64_C(1) << 16,
    DSCO_C_FEATURE_CLEANUP = UINT64_C(1) << 17,
};

typedef struct {
    long standard_version;
    const char *standard_name;
    const char *compiler_name;
    int compiler_version;
    uint64_t mask;
    unsigned bitint_maxwidth;
} dsco_c_capabilities_t;

const dsco_c_capabilities_t *dsco_c_capabilities(void);
int dsco_c_format_manifest(char *buffer, size_t capacity);

#endif /* DSCO_C_H */
