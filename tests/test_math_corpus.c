#include "math_fastpath.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *trim(char *s) {
    while (*s && isspace((unsigned char)*s))
        s++;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1]))
        *--end = '\0';
    return s;
}

static bool parse_double_full(const char *s, double *out) {
    if (!s || !*s)
        return false;
    errno = 0;
    char *end = NULL;
    double v = strtod(s, &end);
    if (end == s)
        return false;
    while (end && *end && isspace((unsigned char)*end))
        end++;
    if (!end || *end != '\0')
        return false;
    if (out)
        *out = v;
    return errno != ERANGE;
}

static bool values_equal(const char *want, const char *got, bool *float_cmp) {
    if (float_cmp)
        *float_cmp = false;
    if (strcmp(want, got) == 0)
        return true;

    double a = 0.0;
    double b = 0.0;
    if (!parse_double_full(want, &a) || !parse_double_full(got, &b))
        return false;
    if (float_cmp)
        *float_cmp = true;
    if (isnan(a) && isnan(b))
        return true;
    if (isinf(a) || isinf(b))
        return isinf(a) && isinf(b) && signbit(a) == signbit(b);

    double scale = fmax(1.0, fmax(fabs(a), fabs(b)));
    return fabs(a - b) <= 1e-9 * scale;
}

static void fail_case(const char *route, const char *expr, const char *want, const char *got) {
    fprintf(stderr, "FAIL [%s] %s  (want='%s' got='%s')\n", route, expr, want, got);
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "tests/math_corpus.tsv";
    FILE *f = fopen(path, "r");
    if (!f) {
        perror(path);
        return 2;
    }

    char line[4096];
    int passed = 0;
    int failed = 0;
    int route_failures = 0;
    int value_failures = 0;

    while (fgets(line, sizeof(line), f)) {
        char *row = trim(line);
        if (!*row || *row == '#')
            continue;

        char *save = NULL;
        char *route = strtok_r(row, "\t", &save);
        char *expr = strtok_r(NULL, "\t", &save);
        char *want = strtok_r(NULL, "\n", &save);
        if (!route || !expr) {
            failed++;
            route_failures++;
            fail_case("parse", row, "route<TAB>expr<TAB>want", "malformed row");
            continue;
        }
        route = trim(route);
        expr = trim(expr);
        want = want ? trim(want) : "";

        char got[256];
        bool handled = mfp_eval(expr, got, sizeof(got));

        if (strcmp(route, "llm") == 0) {
            if (handled) {
                failed++;
                route_failures++;
                fail_case(route, expr, "expected LLM-route but fast-path HANDLED it", got);
            } else {
                passed++;
            }
            continue;
        }

        if (strcmp(route, "mathf") != 0) {
            failed++;
            route_failures++;
            fail_case(route, expr, "mathf or llm", "unknown route");
            continue;
        }

        if (!handled) {
            failed++;
            route_failures++;
            fail_case(route, expr, "expected math-route but fast-path DECLINED", got);
            continue;
        }

        bool float_cmp = false;
        if (!values_equal(want, got, &float_cmp)) {
            failed++;
            value_failures++;
            fail_case(route, expr, float_cmp ? "float value mismatch" : "value mismatch", got);
            fprintf(stderr, "       expected value: %s\n", want);
            continue;
        }

        passed++;
    }

    fclose(f);
    printf("=== math corpus: %d passed, %d failed (route=%d, value=%d) ===\n",
           passed, failed, route_failures, value_failures);
    return failed == 0 ? 0 : 1;
}
