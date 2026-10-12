#include "dsco_meta.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

DSCO_OPTION_DEFINE(dsco_test_option_int, int)
DSCO_RESULT_DEFINE(dsco_test_result_int, int, const char *)
DSCO_SPAN_DEFINE(dsco_test_int_span, int)
DSCO_VEC_DEFINE(dsco_test_int_vec, int)

#define TEST_COLOR_ITEMS(X)                                                                        \
    X(TEST_RED, "red")                                                                             \
    X(TEST_GREEN, "green")                                                                         \
    X(TEST_BLUE, "blue")

typedef enum { TEST_COLOR_ITEMS(DSCO_ENUM_MEMBER) } test_color_t;

static const char *test_color_name(test_color_t color) {
    switch (color) {
        TEST_COLOR_ITEMS(DSCO_ENUM_CASE)
        default:
            return "unknown";
    }
}

#define TEST_PLUS_ONE(value) +1
#define TEST_ARITY_1(a) 1
#define TEST_ARITY_2(a, b) 2
#define TEST_ARITY_3(a, b, c) 3

DSCO_STATIC_ASSERT(DSCO_NARG(a) == 1, "one variadic argument");
DSCO_STATIC_ASSERT(DSCO_NARG(a, b, c) == 3, "three variadic arguments");
#if DSCO_HAS_VA_OPT
DSCO_STATIC_ASSERT(DSCO_NARG() == 0, "empty variadic argument list");
#endif
DSCO_STATIC_ASSERT(DSCO_ARRAY_COUNT(((int[]){EXIT_SUCCESS, EXIT_FAILURE, EOF})) == 3,
                   "compound literal count");

enum {
    TEST_FIRST_VALUE = sizeof(char),
    TEST_SECOND_VALUE = sizeof(uint16_t),
    TEST_THIRD_VALUE = sizeof(uint32_t),
    TEST_WORD_BITS = sizeof(uint64_t) * CHAR_BIT,
};

static int failures;

#define CHECK(condition, message)                                                                  \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "FAIL: %s (line %d)\n", (message), __LINE__);                          \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

static void increment(void *context) {
    int *value = context;
    (*value)++;
}

static void exercise_scope_exit(int *counter) {
    DSCO_DEFER(increment, counter);
    return;
}

static void test_preprocessor(void) {
    CHECK(!strcmp(DSCO_STRINGIFY(dsco), "dsco"), "stringification expands");
    CHECK(!strcmp(test_color_name(TEST_GREEN), "green"), "X-macro enum mapping expands");
    int DSCO_CAT(value_, token) = EXIT_SUCCESS;
    CHECK(value_token == EXIT_SUCCESS, "token concatenation expands");
    CHECK(0 DSCO_FOR_EACH(TEST_PLUS_ONE, a, b, c, d) == 4, "variadic for-each expands");
    CHECK(DSCO_OVERLOAD(TEST_ARITY_, x, y, z) == 3, "arity overload dispatches");
}

static void test_typed_algebra(void) {
    dsco_test_option_int some = dsco_test_option_int_some(TEST_SECOND_VALUE);
    dsco_test_option_int none = dsco_test_option_int_none();
    CHECK(some.has_value && some.value == TEST_SECOND_VALUE && !none.has_value,
          "option encodes some and none");

    dsco_test_result_int ok = dsco_test_result_int_ok(EXIT_SUCCESS);
    dsco_test_result_int error = dsco_test_result_int_error("failed");
    CHECK(ok.ok && ok.value == EXIT_SUCCESS, "result encodes success");
    CHECK(!error.ok && !strcmp(error.error, "failed"), "result encodes failure");

    int raw[] = {TEST_FIRST_VALUE, TEST_SECOND_VALUE, TEST_THIRD_VALUE};
    dsco_test_int_span span = dsco_test_int_span_make(raw, DSCO_ARRAY_COUNT(raw));
    CHECK(span.len == DSCO_ARRAY_COUNT(raw) && span.data[span.len - 1] == TEST_THIRD_VALUE,
          "span carries pointer and length");

    dsco_test_int_vec vector = {0};
    CHECK(dsco_test_int_vec_push(&vector, 0), "vector accepts its first element");
    CHECK(vector.cap == vector.len, "first allocation matches requested capacity");
    for (int i = 1; i < TEST_WORD_BITS; i++)
        CHECK(dsco_test_int_vec_push(&vector, i), "vector grows");
    CHECK(vector.len == TEST_WORD_BITS &&
              *dsco_test_int_vec_at(&vector, vector.len - 1) == TEST_WORD_BITS - 1,
          "vector retains typed elements");
    int popped = -1;
    CHECK(dsco_test_int_vec_pop(&vector, &popped) && popped == TEST_WORD_BITS - 1,
          "vector pops last element");
    dsco_test_int_vec_free(&vector);
}

static void test_expressions_and_lifetime(void) {
    int left = TEST_SECOND_VALUE;
    int right = TEST_THIRD_VALUE;
    CHECK(DSCO_MIN(left++, right++) == TEST_SECOND_VALUE, "minimum evaluates each argument once");
    CHECK(left == TEST_SECOND_VALUE + 1 && right == TEST_THIRD_VALUE + 1,
          "single-evaluation state is exact");
    CHECK(DSCO_MAX(TEST_FIRST_VALUE, TEST_THIRD_VALUE) == TEST_THIRD_VALUE &&
              DSCO_CLAMP(TEST_WORD_BITS, 0, TEST_THIRD_VALUE) == TEST_THIRD_VALUE,
          "max and clamp preserve type");
    DSCO_SWAP(left, right);
    CHECK(left == TEST_THIRD_VALUE + 1 && right == TEST_SECOND_VALUE + 1, "swap is typed");
    CHECK(!strcmp(DSCO_TYPE_NAME(left), "int"), "generic selection identifies int");

    int scope_counter = 0;
    exercise_scope_exit(&scope_counter);
    CHECK(scope_counter == 1, "scope exit executes on return");
}

static void test_checked_arithmetic_and_bits(void) {
    size_t out = 0;
    CHECK(!dsco_size_add_overflow(sizeof(uint64_t), sizeof(uint32_t), &out) &&
              out == sizeof(uint64_t) + sizeof(uint32_t),
          "checked add succeeds");
    CHECK(dsco_size_add_overflow(SIZE_MAX, 1, &out), "checked add catches overflow");
    CHECK(!dsco_size_mul_overflow(sizeof(uint64_t), CHAR_BIT, &out) && out == TEST_WORD_BITS,
          "checked multiply succeeds");
    CHECK(dsco_size_mul_overflow(SIZE_MAX, 2, &out), "checked multiply catches overflow");
    CHECK(dsco_popcount64(UINT64_MAX) == TEST_WORD_BITS, "portable popcount is exact");
}

static void test_modern_surface(void) {
    DSCO_AUTO(inferred, DSCO_COMPILER_VERSION);
    CHECK(inferred == DSCO_COMPILER_VERSION, "auto inference facade works");
    DSCO_CONSTEXPR int fixed = TEST_WORD_BITS;
    CHECK(fixed == TEST_WORD_BITS, "constexpr facade works");
    void *pointer = DSCO_NULLPTR;
    CHECK(pointer == NULL, "nullptr facade works");
#if DSCO_HAS_BITINT
    unsigned _BitInt(sizeof(uint16_t) * CHAR_BIT + 1) precise = (uint64_t)UINT16_MAX + 1;
    CHECK(precise == (uint64_t)UINT16_MAX + 1, "bit-precise integer works");
#endif
}

static void test_manifest(void) {
    const dsco_c_capabilities_t *features = dsco_c_capabilities();
    CHECK(features && features->standard_version == DSCO_C_VERSION,
          "runtime manifest matches compilation standard");
    CHECK(features->mask & DSCO_C_FEATURE_TYPEOF, "manifest exposes typeof");
    char json[BUFSIZ];
    int needed = dsco_c_format_manifest(json, sizeof(json));
    CHECK(needed > 0 && (size_t)needed < sizeof(json), "feature manifest fits output");
    CHECK(strstr(json, "\"compiler\":") && strstr(json, "\"checked_int\":"),
          "feature manifest is machine readable");
}

int main(void) {
    test_preprocessor();
    test_typed_algebra();
    test_expressions_and_lifetime();
    test_checked_arithmetic_and_bits();
    test_modern_surface();
    test_manifest();
    if (failures) {
        fprintf(stderr, "%d C metaprogramming test(s) failed\n", failures);
        return 1;
    }
    printf("dsco C substrate: %s/%s mask=0x%llx passed\n", DSCO_COMPILER_NAME, DSCO_C_STANDARD_NAME,
           (unsigned long long)dsco_c_capabilities()->mask);
    return 0;
}
