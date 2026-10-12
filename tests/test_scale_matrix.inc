/* test_scale_matrix.inc — 600 deterministic, independently counted cases.
 *
 * Included by test.c after TEST/PASS/FAIL are defined so this matrix uses the
 * same accounting and reporting path as every other core regression.
 */

enum {
    SCALE_INTENT_DIRECT_CASES = 5 * 10 * 2,
    SCALE_INTENT_ACTION_CASES = 5 * 10,
    SCALE_INTENT_CONTEXT_CASES = 5 * 10,
    SCALE_ENV_CASES = 40 * 5,
    SCALE_SEQUENCE_CASES = 200,
    SCALE_MATRIX_CASES = SCALE_INTENT_DIRECT_CASES + SCALE_INTENT_ACTION_CASES +
                         SCALE_INTENT_CONTEXT_CASES + SCALE_ENV_CASES +
                         SCALE_SEQUENCE_CASES,
};

_Static_assert(SCALE_MATRIX_CASES == 600, "scale regression matrix must stay at 600 cases");

static void scale_matrix_result(const char *label, bool ok, const char *failure) {
    TEST(label);
    if (ok)
        PASS();
    else
        FAIL(failure);
}

static void scale_ascii_upper(char *dst, size_t capacity, const char *src) {
    if (!dst || capacity == 0)
        return;
    size_t i = 0;
    for (; src && src[i] && i + 1 < capacity; i++)
        dst[i] = (char)toupper((unsigned char)src[i]);
    dst[i] = '\0';
}

static void test_scale_intent_matrix_200(void) {
    static const char *const direct_forms[] = {
        "What is", "Explain", "Define", "Calculate", "Teach me",
    };
    static const char *const neutral_subjects[] = {
        "mutex ownership",
        "semaphore fairness",
        "bounded queue backpressure",
        "binary tree traversal",
        "vector clock causality",
        "cache coherence",
        "quorum intersection",
        "idempotency",
        "tail recursion",
        "consistent hashing",
    };
    static const char *const action_subjects[] = {
        "fix the parser",
        "edit the workspace file",
        "run the benchmark",
        "test the build",
        "compile the project",
        "inspect the logs",
        "search the repository",
        "deploy the service",
        "call tool weather",
        "read the file",
    };
    static const char *const context_subjects[] = {
        "current node status",
        "latest release notes",
        "live account balance",
        "weather in Boston",
        "news from the market",
        "my account quota",
        "the last command failure",
        "the attached screenshot",
        "the stock price",
        "tomorrow's forecast",
    };

    _Static_assert(sizeof(direct_forms) / sizeof(*direct_forms) == 5,
                   "intent form matrix changed");
    _Static_assert(sizeof(neutral_subjects) / sizeof(*neutral_subjects) == 10,
                   "neutral subject matrix changed");
    _Static_assert(sizeof(action_subjects) / sizeof(*action_subjects) == 10,
                   "action subject matrix changed");
    _Static_assert(sizeof(context_subjects) / sizeof(*context_subjects) == 10,
                   "context subject matrix changed");

    char prompt[256];
    char variant[256];
    char label[96];
    char failure[320];

    /* 100 self-contained prompts: every form/subject pair in normal and
     * uppercase form. This catches marker position, punctuation, and ASCII
     * case-folding regressions. */
    for (size_t f = 0; f < 5; f++) {
        for (size_t s = 0; s < 10; s++) {
            snprintf(prompt, sizeof(prompt), "%s %s?", direct_forms[f], neutral_subjects[s]);
            for (int uppercase = 0; uppercase < 2; uppercase++) {
                if (uppercase)
                    scale_ascii_upper(variant, sizeof(variant), prompt);
                else
                    snprintf(variant, sizeof(variant), "%s", prompt);
                snprintf(label, sizeof(label), "scale intent direct %zu/%zu/%d", f, s, uppercase);
                snprintf(failure, sizeof(failure), "expected direct classification: %s", variant);
                scale_matrix_result(label, request_intent_prefers_direct(variant), failure);
            }
        }
    }

    /* 50 mixed prompts prove that an action marker dominates a direct-answer
     * marker. No direct fast path may strip tools from these requests. */
    for (size_t f = 0; f < 5; f++) {
        for (size_t s = 0; s < 10; s++) {
            snprintf(prompt, sizeof(prompt), "%s how to %s", direct_forms[f], action_subjects[s]);
            snprintf(label, sizeof(label), "scale intent action %zu/%zu", f, s);
            snprintf(failure, sizeof(failure), "action prompt incorrectly classified direct: %s",
                     prompt);
            scale_matrix_result(label, !request_intent_prefers_direct(prompt), failure);
        }
    }

    /* 50 changing/external-context prompts prove the same dominance for live
     * or conversation-relative evidence requirements. */
    for (size_t f = 0; f < 5; f++) {
        for (size_t s = 0; s < 10; s++) {
            snprintf(prompt, sizeof(prompt), "%s the %s?", direct_forms[f], context_subjects[s]);
            snprintf(label, sizeof(label), "scale intent context %zu/%zu", f, s);
            snprintf(failure, sizeof(failure), "context prompt incorrectly classified direct: %s",
                     prompt);
            scale_matrix_result(label, !request_intent_prefers_direct(prompt), failure);
        }
    }
}

static int scale_clamp_int(int value, int min_value, int max_value) {
    if (value < min_value)
        return min_value;
    if (value > max_value)
        return max_value;
    return value;
}

static long scale_clamp_long(long value, long min_value, long max_value) {
    if (value < min_value)
        return min_value;
    if (value > max_value)
        return max_value;
    return value;
}

static size_t scale_clamp_size(size_t value, size_t min_value, size_t max_value) {
    if (value < min_value)
        return min_value;
    if (value > max_value)
        return max_value;
    return value;
}

static double scale_clamp_double(double value, double min_value, double max_value) {
    if (value < min_value)
        return min_value;
    if (value > max_value)
        return max_value;
    return value;
}

static void test_scale_env_matrix_200(void) {
    static const char *const truthy[] = {
        "1", "true", "TRUE", "TrUe", "yes", "YES", "on", "ON", "y", "Y", "t", "T",
    };
    static const char *const falsy[] = {
        "0", "false", "FALSE", "FaLsE", "no", "NO", "off", "OFF", "n", "N", "f", "F",
    };
    static const char *const invalid[] = {
        "", "2", "truth", " false ", "yes!", "00", "null", "enable",
    };
    const char *name = "DSCO_TEST_SCALE_MATRIX_VALUE";
    const char *old_value = getenv(name);
    char *saved_value = old_value ? strdup(old_value) : NULL;
    bool had_value = old_value != NULL;
    char raw[96];
    char label[96];
    char failure[192];

    _Static_assert(sizeof(truthy) / sizeof(*truthy) == 12, "truthy matrix changed");
    _Static_assert(sizeof(falsy) / sizeof(*falsy) == 12, "falsy matrix changed");
    _Static_assert(sizeof(invalid) / sizeof(*invalid) == 8, "invalid bool matrix changed");

    /* 40 boolean cases: all documented tokens plus invalid values under both
     * defaults. */
    for (size_t i = 0; i < 12; i++) {
        bool set_ok = setenv(name, truthy[i], 1) == 0;
        snprintf(label, sizeof(label), "scale env bool truthy %zu", i);
        snprintf(failure, sizeof(failure), "truthy token rejected: %s", truthy[i]);
        scale_matrix_result(label, set_ok && dsco_env_bool(name, false), failure);
    }
    for (size_t i = 0; i < 12; i++) {
        bool set_ok = setenv(name, falsy[i], 1) == 0;
        snprintf(label, sizeof(label), "scale env bool falsy %zu", i);
        snprintf(failure, sizeof(failure), "falsy token rejected: %s", falsy[i]);
        scale_matrix_result(label, set_ok && !dsco_env_bool(name, true), failure);
    }
    for (size_t i = 0; i < 8; i++) {
        for (int def = 0; def < 2; def++) {
            bool set_ok = setenv(name, invalid[i], 1) == 0;
            snprintf(label, sizeof(label), "scale env bool invalid %zu/%d", i, def);
            snprintf(failure, sizeof(failure), "invalid bool token ignored default: %s", invalid[i]);
            scale_matrix_result(label, set_ok && dsco_env_bool(name, def != 0) == (def != 0),
                                failure);
        }
    }

    /* 40 cases per numeric type sweep through both clamp boundaries and the
     * interior using the real process environment parsing path. */
    for (int i = 0; i < 40; i++) {
        int value = i - 20;
        int expected = scale_clamp_int(value, -7, 13);
        snprintf(raw, sizeof(raw), "%d", value);
        bool set_ok = setenv(name, raw, 1) == 0;
        snprintf(label, sizeof(label), "scale env int %d", i);
        snprintf(failure, sizeof(failure), "int parse/clamp mismatch for %s", raw);
        scale_matrix_result(label, set_ok && dsco_env_int(name, 99, -7, 13) == expected, failure);
    }
    for (int i = 0; i < 40; i++) {
        long value = (long)(i - 20) * 1000003L;
        long expected = scale_clamp_long(value, -7000000L, 9000000L);
        snprintf(raw, sizeof(raw), "%ld", value);
        bool set_ok = setenv(name, raw, 1) == 0;
        snprintf(label, sizeof(label), "scale env long %d", i);
        snprintf(failure, sizeof(failure), "long parse/clamp mismatch for %s", raw);
        scale_matrix_result(label,
                            set_ok && dsco_env_long(name, 99L, -7000000L, 9000000L) == expected,
                            failure);
    }
    for (int i = 0; i < 40; i++) {
        size_t value = (size_t)i * 3U;
        size_t expected = scale_clamp_size(value, 17U, 83U);
        snprintf(raw, sizeof(raw), "%zu", value);
        bool set_ok = setenv(name, raw, 1) == 0;
        snprintf(label, sizeof(label), "scale env size %d", i);
        snprintf(failure, sizeof(failure), "size parse/clamp mismatch for %s", raw);
        scale_matrix_result(label, set_ok && dsco_env_size(name, 99U, 17U, 83U) == expected,
                            failure);
    }
    for (int i = 0; i < 40; i++) {
        double value = (double)(i - 20) / 3.0;
        double expected = scale_clamp_double(value, -2.5, 3.5);
        snprintf(raw, sizeof(raw), "%.17g", value);
        bool set_ok = setenv(name, raw, 1) == 0;
        double actual = dsco_env_double(name, 99.0, -2.5, 3.5);
        snprintf(label, sizeof(label), "scale env double %d", i);
        snprintf(failure, sizeof(failure), "double parse/clamp mismatch for %s", raw);
        scale_matrix_result(label, set_ok && fabs(actual - expected) < 1e-12, failure);
    }

    if (had_value && saved_value)
        setenv(name, saved_value, 1);
    else
        unsetenv(name);
    free(saved_value);
}

static void test_scale_sequence_matrix_200(void) {
    char label[96];
    char failure[192];

    for (int case_index = 0; case_index < SCALE_SEQUENCE_CASES; case_index++) {
        dsco_sampling_params_t sampling = {
            .temperature = (double)case_index / 100.0,
            .top_p = 1.0 - (double)(case_index % 50) / 100.0,
            .top_k = case_index + 1,
            .max_tokens = case_index + 32,
        };
        int block_anchor = case_index;
        dsco_kvblock_ref_t blocks = {.opaque = &block_anchor};
        dsco_sequence_state_t seq;
        dsco_sequence_state_init(&seq, &sampling, blocks);

        size_t token_count = (size_t)((case_index * 37) % 97) + 1U;
        bool ok = seq.blocks.opaque == &block_anchor &&
                  seq.sampling.temperature == sampling.temperature &&
                  seq.sampling.top_p == sampling.top_p &&
                  seq.sampling.top_k == sampling.top_k &&
                  seq.sampling.max_tokens == sampling.max_tokens;

        for (size_t j = 0; j < token_count; j++) {
            int token = case_index * 1000 + (int)j;
            if (case_index & 1)
                token = -token;
            if (!dsco_sequence_state_append(&seq, token)) {
                ok = false;
                break;
            }
        }
        ok = ok && seq.count == token_count && seq.capacity >= seq.count;

        for (size_t j = 0; ok && j < token_count; j++) {
            int expected = case_index * 1000 + (int)j;
            if (case_index & 1)
                expected = -expected;
            int actual = 0;
            ok = dsco_sequence_state_next(&seq, &actual) && actual == expected;
        }
        int ignored = 0;
        ok = ok && !dsco_sequence_state_next(&seq, &ignored) && seq.position == token_count;

        dsco_sequence_state_rewind(&seq);
        ok = ok && seq.position == 0 && dsco_sequence_state_next(&seq, NULL) && seq.position == 1;
        dsco_sequence_state_free(&seq);
        ok = ok && seq.tokens == NULL && seq.count == 0 && seq.capacity == 0 &&
             seq.position == 0 && seq.blocks.opaque == NULL;

        snprintf(label, sizeof(label), "scale sequence lifecycle %d", case_index);
        snprintf(failure, sizeof(failure), "sequence lifecycle mismatch in case %d (%zu tokens)",
                 case_index, token_count);
        scale_matrix_result(label, ok, failure);
    }
}

static void test_scale_regression_matrix_600(void) {
    test_scale_intent_matrix_200();
    test_scale_env_matrix_200();
    test_scale_sequence_matrix_200();
}
