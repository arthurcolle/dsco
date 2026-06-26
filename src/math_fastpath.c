#include "math_fastpath.h"

#include "eval.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static const char *const k_eval_funcs[] = {
    "sqrt", "cbrt", "ceil", "floor", "round", "trunc", "sign",
    "asin", "acos", "atan", "atan2", "sinh", "cosh", "tanh",
    "log", "ln", "log2", "log10", "exp", "exp2", "abs",
    "sin", "cos", "tan", "pow", "min", "max", "gcd", "lcm", "fib",
    "factorial", "hypot", "fmod", "clamp", "deg", "rad", "hex",
    "oct", "bin", "dec",
};

static const char *const k_eval_consts[] = {
    "e", "pi", "tau", "phi", "inf", "nan",
};

static bool token_eq(const char *name, size_t len, const char *want) {
    return strlen(want) == len && strncmp(name, want, len) == 0;
}

bool mfp_is_func(const char *name, size_t len) {
    if (!name || len == 0)
        return false;
    for (size_t i = 0; i < sizeof(k_eval_funcs) / sizeof(k_eval_funcs[0]); i++) {
        if (token_eq(name, len, k_eval_funcs[i]))
            return true;
    }
    return false;
}

bool mfp_is_const(const char *name, size_t len) {
    if (!name || len == 0)
        return false;
    for (size_t i = 0; i < sizeof(k_eval_consts) / sizeof(k_eval_consts[0]); i++) {
        if (token_eq(name, len, k_eval_consts[i]))
            return true;
    }
    return false;
}

bool mfp_ident_known(const char *name, size_t len) {
    return mfp_is_func(name, len) || mfp_is_const(name, len);
}

static bool consume_digits_for_base(const char **pp, int base) {
    const char *p = *pp;
    const char *start = p;
    while (*p) {
        unsigned char c = (unsigned char)*p;
        int digit = -1;
        if (isdigit(c)) {
            digit = c - '0';
        } else if (isalpha(c)) {
            digit = 10 + (tolower(c) - 'a');
        } else {
            break;
        }
        if (digit < 0 || digit >= base)
            break;
        p++;
    }
    *pp = p;
    return p > start;
}

static bool consume_number(const char **pp) {
    const char *p = *pp;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        p += 2;
        bool ok = consume_digits_for_base(&p, 16);
        *pp = p;
        return ok;
    }
    if (p[0] == '0' && (p[1] == 'b' || p[1] == 'B')) {
        p += 2;
        bool ok = consume_digits_for_base(&p, 2);
        *pp = p;
        return ok;
    }
    if (p[0] == '0' && (p[1] == 'o' || p[1] == 'O')) {
        p += 2;
        bool ok = consume_digits_for_base(&p, 8);
        *pp = p;
        return ok;
    }

    bool saw_digit = false;
    while (isdigit((unsigned char)*p)) {
        saw_digit = true;
        p++;
    }
    if (*p == '.') {
        p++;
        while (isdigit((unsigned char)*p)) {
            saw_digit = true;
            p++;
        }
    }
    if (!saw_digit)
        return false;
    if (*p == 'e' || *p == 'E') {
        const char *exp = p + 1;
        if (*exp == '+' || *exp == '-')
            exp++;
        const char *exp_digits = exp;
        while (isdigit((unsigned char)*exp))
            exp++;
        if (exp > exp_digits)
            p = exp;
    }
    *pp = p;
    return true;
}

bool mfp_is_pure_math(const char *expr, char *reason) {
    if (reason)
        reason[0] = '\0';
    if (!expr || !expr[0]) {
        if (reason)
            snprintf(reason, 128, "empty expression");
        return false;
    }

    bool has_digit = false;
    bool has_op = false;
    bool has_func = false;
    bool has_const = false;

    for (const char *p = expr; *p;) {
        unsigned char c = (unsigned char)*p;
        if (isspace(c)) {
            p++;
            continue;
        }
        if (isdigit(c) || (c == '.' && isdigit((unsigned char)p[1]))) {
            const char *next = p;
            if (!consume_number(&next)) {
                if (reason)
                    snprintf(reason, 128, "invalid number");
                return false;
            }
            has_digit = true;
            p = next;
            continue;
        }
        if (strchr("+-*/%^(),!<>=&|?:~", c)) {
            has_op = true;
            p++;
            continue;
        }
        if (isalpha(c) || c == '_') {
            const char *start = p;
            p++;
            while (isalnum((unsigned char)*p) || *p == '_')
                p++;
            size_t len = (size_t)(p - start);
            if (mfp_is_func(start, len)) {
                has_func = true;
                continue;
            }
            if (mfp_is_const(start, len)) {
                has_const = true;
                continue;
            }
            if (reason)
                snprintf(reason, 128, "unknown identifier '%.*s'", (int)len, start);
            return false;
        }
        if (reason)
            snprintf(reason, 128, "unsafe character '%c'", *p);
        return false;
    }

    if (!has_digit && !has_const) {
        if (reason)
            snprintf(reason, 128, "no numeric value");
        return false;
    }
    if (!has_op && !has_func) {
        if (reason)
            snprintf(reason, 128, "no mathematical operation");
        return false;
    }
    return has_digit || has_const;
}

bool mfp_rewrite_func_shorthand(const char *expr, char *out, size_t out_len) {
    if (!out || out_len == 0)
        return false;
    out[0] = '\0';
    if (!expr)
        return false;

    while (isspace((unsigned char)*expr))
        expr++;

    const char *p = expr;
    while (isalnum((unsigned char)*p) || *p == '_')
        p++;
    size_t fn_len = (size_t)(p - expr);
    if (!mfp_is_func(expr, fn_len))
        return false;
    while (isspace((unsigned char)*p))
        p++;
    if (*p == '(' || *p == '\0')
        return false;

    snprintf(out, out_len, "%.*s(%s)", (int)fn_len, expr, p);
    return true;
}

bool mfp_eval(const char *expr, char *out, size_t out_len) {
    if (!out || out_len == 0)
        return false;
    out[0] = '\0';

    char reason[128];
    if (!mfp_is_pure_math(expr, reason)) {
        snprintf(out, out_len, "%s", reason[0] ? reason : "not a pure math expression");
        return false;
    }

    char rewritten[1024];
    const char *input = expr;
    if (mfp_rewrite_func_shorthand(expr, rewritten, sizeof(rewritten)))
        input = rewritten;

    eval_ctx_t ctx;
    eval_init(&ctx);
    eval_format(&ctx, input, out, out_len);
    return !ctx.has_error;
}
