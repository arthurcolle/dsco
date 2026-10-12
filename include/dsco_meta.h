#ifndef DSCO_META_H
#define DSCO_META_H

#include "dsco_c.h"

#include <string.h>

/* Preprocessor calculus: expansion, token construction, arity, overloads,
 * iteration, and X-macro enum generation. */
#define DSCO_EXPAND(...) __VA_ARGS__
#define DSCO_STRINGIFY_RAW(value) #value
#define DSCO_STRINGIFY(value) DSCO_STRINGIFY_RAW(value)
#define DSCO_CAT_RAW(a, b) a##b
#define DSCO_CAT(a, b) DSCO_CAT_RAW(a, b)
#define DSCO_CAT3(a, b, c) DSCO_CAT(DSCO_CAT(a, b), c)
#if defined(__COUNTER__)
#define DSCO_UNIQUE(prefix) DSCO_CAT(prefix, __COUNTER__)
#else
#define DSCO_UNIQUE(prefix) DSCO_CAT(prefix, __LINE__)
#endif

#define DSCO_PP_ARG_N(_1, _2, _3, _4, _5, _6, _7, _8, _9, _10, _11, _12, _13, _14, _15, _16,       \
                      count, ...)                                                                  \
    count
#define DSCO_PP_RSEQ() 16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0
#define DSCO_PP_NARG_I(...) DSCO_PP_ARG_N(__VA_ARGS__)
#if DSCO_HAS_VA_OPT
#define DSCO_NARG(...) DSCO_PP_NARG_I(__VA_OPT__(__VA_ARGS__, ) DSCO_PP_RSEQ())
#else
#define DSCO_NARG(...) DSCO_PP_NARG_I(__VA_ARGS__, DSCO_PP_RSEQ())
#endif
#define DSCO_OVERLOAD(prefix, ...) DSCO_CAT(prefix, DSCO_NARG(__VA_ARGS__))(__VA_ARGS__)

#define DSCO_FOR_EACH_0(macro)
#define DSCO_FOR_EACH_1(macro, a) macro(a)
#define DSCO_FOR_EACH_2(macro, a, ...) macro(a) DSCO_FOR_EACH_1(macro, __VA_ARGS__)
#define DSCO_FOR_EACH_3(macro, a, ...) macro(a) DSCO_FOR_EACH_2(macro, __VA_ARGS__)
#define DSCO_FOR_EACH_4(macro, a, ...) macro(a) DSCO_FOR_EACH_3(macro, __VA_ARGS__)
#define DSCO_FOR_EACH_5(macro, a, ...) macro(a) DSCO_FOR_EACH_4(macro, __VA_ARGS__)
#define DSCO_FOR_EACH_6(macro, a, ...) macro(a) DSCO_FOR_EACH_5(macro, __VA_ARGS__)
#define DSCO_FOR_EACH_7(macro, a, ...) macro(a) DSCO_FOR_EACH_6(macro, __VA_ARGS__)
#define DSCO_FOR_EACH_8(macro, a, ...) macro(a) DSCO_FOR_EACH_7(macro, __VA_ARGS__)
#define DSCO_FOR_EACH_9(macro, a, ...) macro(a) DSCO_FOR_EACH_8(macro, __VA_ARGS__)
#define DSCO_FOR_EACH_10(macro, a, ...) macro(a) DSCO_FOR_EACH_9(macro, __VA_ARGS__)
#define DSCO_FOR_EACH_11(macro, a, ...) macro(a) DSCO_FOR_EACH_10(macro, __VA_ARGS__)
#define DSCO_FOR_EACH_12(macro, a, ...) macro(a) DSCO_FOR_EACH_11(macro, __VA_ARGS__)
#define DSCO_FOR_EACH_13(macro, a, ...) macro(a) DSCO_FOR_EACH_12(macro, __VA_ARGS__)
#define DSCO_FOR_EACH_14(macro, a, ...) macro(a) DSCO_FOR_EACH_13(macro, __VA_ARGS__)
#define DSCO_FOR_EACH_15(macro, a, ...) macro(a) DSCO_FOR_EACH_14(macro, __VA_ARGS__)
#define DSCO_FOR_EACH_16(macro, a, ...) macro(a) DSCO_FOR_EACH_15(macro, __VA_ARGS__)
#define DSCO_FOR_EACH(macro, ...)                                                                  \
    DSCO_CAT(DSCO_FOR_EACH_, DSCO_NARG(__VA_ARGS__))(macro, __VA_ARGS__)

#define DSCO_ENUM_MEMBER(symbol, label) symbol,
#define DSCO_ENUM_CASE(symbol, label)                                                              \
    case symbol:                                                                                   \
        return label;

/* Type-safe expression helpers. GNU statement expressions supply single
 * evaluation; the portable branch retains the same call surface. */
#define DSCO_ARRAY_COUNT(array) (sizeof(array) / sizeof((array)[0]))
#define DSCO_ZERO(type) ((type){0})
#define DSCO_CONTAINER_OF(pointer, type, member)                                                   \
    ((type *)((char *)(pointer) - offsetof(type, member)))

#if DSCO_COMPILER_CLANG || DSCO_COMPILER_GCC
#define DSCO_MIN(a, b)                                                                             \
    ({                                                                                             \
        DSCO_AUTO(_a, (a));                                                                        \
        DSCO_AUTO(_b, (b));                                                                        \
        _a < _b ? _a : _b;                                                                         \
    })
#define DSCO_MAX(a, b)                                                                             \
    ({                                                                                             \
        DSCO_AUTO(_a, (a));                                                                        \
        DSCO_AUTO(_b, (b));                                                                        \
        _a > _b ? _a : _b;                                                                         \
    })
#define DSCO_CLAMP(value, low, high)                                                               \
    ({                                                                                             \
        DSCO_AUTO(_v, (value));                                                                    \
        DSCO_AUTO(_lo, (low));                                                                     \
        DSCO_AUTO(_hi, (high));                                                                    \
        _v < _lo ? _lo : (_v > _hi ? _hi : _v);                                                    \
    })
#define DSCO_SWAP(a, b)                                                                            \
    do {                                                                                           \
        DSCO_AUTO(_tmp, (a));                                                                      \
        (a) = (b);                                                                                 \
        (b) = _tmp;                                                                                \
    } while (0)
#else
#define DSCO_MIN(a, b) ((a) < (b) ? (a) : (b))
#define DSCO_MAX(a, b) ((a) > (b) ? (a) : (b))
#define DSCO_CLAMP(value, low, high) DSCO_MIN(DSCO_MAX((value), (low)), (high))
#define DSCO_SWAP(a, b)                                                                            \
    do {                                                                                           \
        unsigned char _tmp[sizeof(a)];                                                             \
        memcpy(_tmp, &(a), sizeof(a));                                                             \
        memcpy(&(a), &(b), sizeof(a));                                                             \
        memcpy(&(b), _tmp, sizeof(a));                                                             \
    } while (0)
#endif

#if DSCO_HAS_GENERIC
#define DSCO_TYPE_NAME(value)                                                                      \
    _Generic((value),                                                                              \
        bool: "bool",                                                                              \
        char: "char",                                                                              \
        signed char: "signed char",                                                                \
        unsigned char: "unsigned char",                                                            \
        short: "short",                                                                            \
        unsigned short: "unsigned short",                                                          \
        int: "int",                                                                                \
        unsigned int: "unsigned int",                                                              \
        long: "long",                                                                              \
        unsigned long: "unsigned long",                                                            \
        long long: "long long",                                                                    \
        unsigned long long: "unsigned long long",                                                  \
        float: "float",                                                                            \
        double: "double",                                                                          \
        long double: "long double",                                                                \
        char *: "char *",                                                                          \
        const char *: "const char *",                                                              \
        void *: "void *",                                                                          \
        const void *: "const void *",                                                              \
        default: "other")
#else
#define DSCO_TYPE_NAME(value) "unknown"
#endif

/* Algebraic data types generated as ordinary C structs. */
#define DSCO_OPTION_DEFINE(name, type)                                                             \
    typedef struct {                                                                               \
        bool has_value;                                                                            \
        type value;                                                                                \
    } name;                                                                                        \
    DSCO_MAYBE_UNUSED static inline name DSCO_CAT(name, _some)(type value) {                       \
        name result = {.has_value = true, .value = value};                                         \
        return result;                                                                             \
    }                                                                                              \
    DSCO_MAYBE_UNUSED static inline name DSCO_CAT(name, _none)(void) {                             \
        name result = {0};                                                                         \
        return result;                                                                             \
    }

#define DSCO_RESULT_DEFINE(name, value_type, error_type)                                           \
    typedef struct {                                                                               \
        bool ok;                                                                                   \
        union {                                                                                    \
            value_type value;                                                                      \
            error_type error;                                                                      \
        };                                                                                         \
    } name;                                                                                        \
    DSCO_MAYBE_UNUSED static inline name DSCO_CAT(name, _ok)(value_type value) {                   \
        name result = {.ok = true, .value = value};                                                \
        return result;                                                                             \
    }                                                                                              \
    DSCO_MAYBE_UNUSED static inline name DSCO_CAT(name, _error)(error_type error) {                \
        name result = {.ok = false, .error = error};                                               \
        return result;                                                                             \
    }

#define DSCO_SPAN_DEFINE(name, type)                                                               \
    typedef struct {                                                                               \
        type *data;                                                                                \
        size_t len;                                                                                \
    } name;                                                                                        \
    DSCO_MAYBE_UNUSED static inline name DSCO_CAT(name, _make)(type * data, size_t len) {          \
        name result = {.data = data, .len = len};                                                  \
        return result;                                                                             \
    }

#define DSCO_VEC_DEFINE(name, type)                                                                \
    typedef struct {                                                                               \
        type *data;                                                                                \
        size_t len;                                                                                \
        size_t cap;                                                                                \
    } name;                                                                                        \
    DSCO_MAYBE_UNUSED static inline void DSCO_CAT(name, _free)(name * vector) {                    \
        if (!vector)                                                                               \
            return;                                                                                \
        free(vector->data);                                                                        \
        *vector = (name){0};                                                                       \
    }                                                                                              \
    DSCO_MAYBE_UNUSED static inline bool DSCO_CAT(name, _reserve)(name * vector,                   \
                                                                  size_t required) {               \
        if (!vector)                                                                               \
            return false;                                                                          \
        if (required <= vector->cap)                                                               \
            return true;                                                                           \
        size_t next = vector->cap ? vector->cap : required;                                        \
        while (next < required) {                                                                  \
            size_t grown;                                                                          \
            if (dsco_size_add_overflow(next, next / 2 + 1, &grown))                                \
                return false;                                                                      \
            next = grown;                                                                          \
        }                                                                                          \
        size_t bytes;                                                                              \
        if (dsco_size_mul_overflow(next, sizeof(type), &bytes))                                    \
            return false;                                                                          \
        void *grown = realloc(vector->data, bytes);                                                \
        if (!grown)                                                                                \
            return false;                                                                          \
        vector->data = grown;                                                                      \
        vector->cap = next;                                                                        \
        return true;                                                                               \
    }                                                                                              \
    DSCO_MAYBE_UNUSED static inline bool DSCO_CAT(name, _push)(name * vector, type value) {        \
        size_t required;                                                                           \
        if (!vector || dsco_size_add_overflow(vector->len, 1, &required) ||                        \
            !DSCO_CAT(name, _reserve)(vector, required))                                           \
            return false;                                                                          \
        vector->data[vector->len++] = value;                                                       \
        return true;                                                                               \
    }                                                                                              \
    DSCO_MAYBE_UNUSED static inline bool DSCO_CAT(name, _pop)(name * vector, type * out) {         \
        if (!vector || vector->len == 0)                                                           \
            return false;                                                                          \
        type value = vector->data[--vector->len];                                                  \
        if (out)                                                                                   \
            *out = value;                                                                          \
        return true;                                                                               \
    }                                                                                              \
    DSCO_MAYBE_UNUSED static inline type *DSCO_CAT(name, _at)(name * vector, size_t index) {       \
        return vector && index < vector->len ? &vector->data[index] : NULL;                        \
    }

/* Deterministic scope exit. The compiler cleanup path executes on every return
 * from the scope. Unsupported compilers reject DSCO_DEFER at compile time. */
typedef void (*dsco_defer_fn)(void *context);
typedef struct {
    dsco_defer_fn function;
    void *context;
} dsco_defer_t;

static inline void dsco_defer_run(dsco_defer_t *defer) {
    if (defer && defer->function)
        defer->function(defer->context);
}

#if DSCO_HAS_CLEANUP
#define DSCO_SCOPE_EXIT(name, function_value, context_value)                                       \
    DSCO_MAYBE_UNUSED DSCO_CLEANUP(dsco_defer_run)                                                 \
    dsco_defer_t name = {                                                                          \
        .function = (dsco_defer_fn)(function_value),                                               \
        .context = (void *)(context_value),                                                        \
    }
#define DSCO_DEFER(function_value, context_value)                                                  \
    DSCO_SCOPE_EXIT(DSCO_UNIQUE(_dsco_defer_), function_value, context_value)
#else
#define DSCO_SCOPE_EXIT(name, function_value, context_value)                                       \
    DSCO_STATIC_ASSERT(0, "DSCO_SCOPE_EXIT requires compiler cleanup support")
#define DSCO_DEFER(function_value, context_value)                                                  \
    DSCO_STATIC_ASSERT(0, "DSCO_DEFER requires compiler cleanup support")
#endif

#endif /* DSCO_META_H */
