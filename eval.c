#include "eval.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include <float.h>

/*
 * Recursive descent expression evaluator
 * Inspired by Simon Tatham's spigot exact real calculator.
 *
 * Grammar:
 *   expr     → assign
 *   assign   → ternary ('=' assign)?
 *   ternary  → logic_or ('?' expr ':' expr)?
 *   logic_or → logic_and ('||' logic_and)*
 *   logic_and→ bitwise_or ('&&' bitwise_or)*
 *   bitwise_or → bitwise_xor ('|' bitwise_xor)*
 *   bitwise_xor→ bitwise_and ('^' bitwise_and)*  [note: ^ is power in math mode]
 *   bitwise_and→ equality ('&' equality)*
 *   equality → compare (('=='|'!=') compare)*
 *   compare  → shift (('<'|'>'|'<='|'>=') shift)*
 *   shift    → sum (('<<'|'>>') sum)*
 *   sum      → product (('+'|'-') product)*
 *   product  → unary (('*'|'/'|'%') unary)*
 *   unary    → ('-'|'~'|'!') unary | power
 *   power    → postfix ('**'|'^' unary)?     [right-associative]
 *   postfix  → primary ('!')?                 [factorial]
 *   primary  → NUMBER | IDENT | IDENT'('args')' | '('expr')'
 */

/* ── Forward declarations ────────────────────────────────────────────── */

static double parse_expr(eval_ctx_t *ctx);
static double parse_assign(eval_ctx_t *ctx);
static double parse_ternary(eval_ctx_t *ctx);
static double parse_or(eval_ctx_t *ctx);
static double parse_and(eval_ctx_t *ctx);
static double parse_bitor(eval_ctx_t *ctx);
static double parse_bitxor(eval_ctx_t *ctx);
static double parse_bitand(eval_ctx_t *ctx);
static double parse_equality(eval_ctx_t *ctx);
static double parse_compare(eval_ctx_t *ctx);
static double parse_shift(eval_ctx_t *ctx);
static double parse_sum(eval_ctx_t *ctx);
static double parse_product(eval_ctx_t *ctx);
static double parse_unary(eval_ctx_t *ctx);
static double parse_power(eval_ctx_t *ctx);
static double parse_postfix(eval_ctx_t *ctx);
static double parse_primary(eval_ctx_t *ctx);

/* ── Helpers ──────────────────────────────────────────────────────────── */

static void skip_ws(eval_ctx_t *ctx) {
    while (*ctx->pos && isspace((unsigned char)*ctx->pos)) ctx->pos++;
}

static bool match_char(eval_ctx_t *ctx, char c) {
    skip_ws(ctx);
    if (*ctx->pos == c) { ctx->pos++; return true; }
    return false;
}

static bool match_str(eval_ctx_t *ctx, const char *s) {
    skip_ws(ctx);
    size_t len = strlen(s);
    if (strncmp(ctx->pos, s, len) == 0) {
        ctx->pos += len;
        return true;
    }
    return false;
}

static bool peek_char(eval_ctx_t *ctx, char c) {
    skip_ws(ctx);
    return *ctx->pos == c;
}

static void set_error(eval_ctx_t *ctx, const char *msg) {
    if (!ctx->has_error) {
        snprintf(ctx->error, sizeof(ctx->error), "%s at position %ld",
                 msg, (long)(ctx->pos - ctx->input));
        ctx->has_error = true;
    }
}

static double factorial_f(double n) {
    if (n < 0) return NAN;
    if (n > 170) return INFINITY;
    double result = 1;
    for (int i = 2; i <= (int)n; i++) result *= i;
    return result;
}

static double gcd_f(double a, double b) {
    long la = (long)fabs(a), lb = (long)fabs(b);
    while (lb) { long t = lb; lb = la % lb; la = t; }
    return (double)la;
}

static double lcm_f(double a, double b) {
    return fabs(a * b) / gcd_f(a, b);
}

static double fib_f(double n) {
    if (n < 0) return NAN;
    int ni = (int)n;
    if (ni <= 1) return ni;
    double a = 0, b = 1;
    for (int i = 2; i <= ni; i++) { double t = a + b; a = b; b = t; }
    return b;
}

/* ── Variable management ─────────────────────────────────────────────── */

void eval_init(eval_ctx_t *ctx) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->base = 10;
    /* Pre-define constants */
    eval_set_var(ctx, "pi",  3.14159265358979323846);
    eval_set_var(ctx, "e",   2.71828182845904523536);
    eval_set_var(ctx, "tau", 6.28318530717958647692);
    eval_set_var(ctx, "phi", 1.61803398874989484820);
    eval_set_var(ctx, "inf", INFINITY);
    eval_set_var(ctx, "nan", NAN);
}

void eval_set_var(eval_ctx_t *ctx, const char *name, double value) {
    for (int i = 0; i < ctx->var_count; i++) {
        if (strcmp(ctx->vars[i].name, name) == 0) {
            ctx->vars[i].value = value;
            return;
        }
    }
    if (ctx->var_count < EVAL_MAX_VARS) {
        strncpy(ctx->vars[ctx->var_count].name, name, 63);
        ctx->vars[ctx->var_count].name[63] = '\0';
        ctx->vars[ctx->var_count].value = value;
        ctx->var_count++;
    }
}

double eval_get_var(eval_ctx_t *ctx, const char *name) {
    for (int i = 0; i < ctx->var_count; i++) {
        if (strcmp(ctx->vars[i].name, name) == 0)
            return ctx->vars[i].value;
    }
    return NAN;
}

/* ── Parse number ────────────────────────────────────────────────────── */

static double parse_number(eval_ctx_t *ctx) {
    skip_ws(ctx);
    const char *start = ctx->pos;

    /* Hex: 0x... */
    if (ctx->pos[0] == '0' && (ctx->pos[1] == 'x' || ctx->pos[1] == 'X')) {
        ctx->pos += 2;
        long val = strtol(ctx->pos, (char **)&ctx->pos, 16);
        return (double)val;
    }
    /* Binary: 0b... */
    if (ctx->pos[0] == '0' && (ctx->pos[1] == 'b' || ctx->pos[1] == 'B')) {
        ctx->pos += 2;
        long val = strtol(ctx->pos, (char **)&ctx->pos, 2);
        return (double)val;
    }
    /* Octal: 0o... */
    if (ctx->pos[0] == '0' && (ctx->pos[1] == 'o' || ctx->pos[1] == 'O')) {
        ctx->pos += 2;
        long val = strtol(ctx->pos, (char **)&ctx->pos, 8);
        return (double)val;
    }

    double val = strtod(start, (char **)&ctx->pos);
    if (ctx->pos == start) {
        set_error(ctx, "expected number");
        return NAN;
    }
    return val;
}

/* ── Parse identifier & function calls ───────────────────────────────── */

static double call_function(eval_ctx_t *ctx, const char *name) {
    /* Parse arguments */
    double args[4] = {0};
    int argc = 0;
    if (!peek_char(ctx, ')')) {
        args[argc++] = parse_expr(ctx);
        while (match_char(ctx, ',') && argc < 4) {
            args[argc++] = parse_expr(ctx);
        }
    }
    if (!match_char(ctx, ')')) {
        set_error(ctx, "expected ')'");
        return NAN;
    }

    /* Built-in functions */
    if (strcmp(name, "sqrt") == 0)     return sqrt(args[0]);
    if (strcmp(name, "cbrt") == 0)     return cbrt(args[0]);
    if (strcmp(name, "abs") == 0)      return fabs(args[0]);
    if (strcmp(name, "ceil") == 0)     return ceil(args[0]);
    if (strcmp(name, "floor") == 0)    return floor(args[0]);
    if (strcmp(name, "round") == 0)    return round(args[0]);
    if (strcmp(name, "trunc") == 0)    return trunc(args[0]);

    if (strcmp(name, "sin") == 0)      return sin(args[0]);
    if (strcmp(name, "cos") == 0)      return cos(args[0]);
    if (strcmp(name, "tan") == 0)      return tan(args[0]);
    if (strcmp(name, "asin") == 0)     return asin(args[0]);
    if (strcmp(name, "acos") == 0)     return acos(args[0]);
    if (strcmp(name, "atan") == 0)     return atan(args[0]);
    if (strcmp(name, "atan2") == 0)    return atan2(args[0], args[1]);
    if (strcmp(name, "sinh") == 0)     return sinh(args[0]);
    if (strcmp(name, "cosh") == 0)     return cosh(args[0]);
    if (strcmp(name, "tanh") == 0)     return tanh(args[0]);

    if (strcmp(name, "log") == 0)      return log10(args[0]);
    if (strcmp(name, "log2") == 0)     return log2(args[0]);
    if (strcmp(name, "log10") == 0)    return log10(args[0]);
    if (strcmp(name, "ln") == 0)       return log(args[0]);
    if (strcmp(name, "exp") == 0)      return exp(args[0]);
    if (strcmp(name, "exp2") == 0)     return exp2(args[0]);
    if (strcmp(name, "pow") == 0)      return pow(args[0], args[1]);

    if (strcmp(name, "min") == 0)      return fmin(args[0], args[1]);
    if (strcmp(name, "max") == 0)      return fmax(args[0], args[1]);
    if (strcmp(name, "clamp") == 0)    return fmin(fmax(args[0], args[1]), args[2]);

    if (strcmp(name, "gcd") == 0)      return gcd_f(args[0], args[1]);
    if (strcmp(name, "lcm") == 0)      return lcm_f(args[0], args[1]);
    if (strcmp(name, "fib") == 0)      return fib_f(args[0]);
    if (strcmp(name, "factorial") == 0) return factorial_f(args[0]);

    if (strcmp(name, "deg") == 0)      return args[0] * 180.0 / M_PI;
    if (strcmp(name, "rad") == 0)      return args[0] * M_PI / 180.0;

    if (strcmp(name, "sign") == 0)     return (args[0] > 0) - (args[0] < 0);
    if (strcmp(name, "fmod") == 0)     return fmod(args[0], args[1]);
    if (strcmp(name, "hypot") == 0)    return hypot(args[0], args[1]);

    /* Format conversions — set output base */
    if (strcmp(name, "hex") == 0)      { ctx->base = 16; return args[0]; }
    if (strcmp(name, "oct") == 0)      { ctx->base = 8;  return args[0]; }
    if (strcmp(name, "bin") == 0)      { ctx->base = 2;  return args[0]; }
    if (strcmp(name, "dec") == 0)      { ctx->base = 10; return args[0]; }

    char msg[128];
    snprintf(msg, sizeof(msg), "unknown function '%s'", name);
    set_error(ctx, msg);
    return NAN;
}

/* ── Recursive descent parser ────────────────────────────────────────── */

static double parse_primary(eval_ctx_t *ctx) {
    if (ctx->has_error) return NAN;
    skip_ws(ctx);

    /* Parenthesized expression */
    if (match_char(ctx, '(')) {
        double val = parse_expr(ctx);
        if (!match_char(ctx, ')')) set_error(ctx, "expected ')'");
        return val;
    }

    /* Number */
    if (isdigit((unsigned char)*ctx->pos) || *ctx->pos == '.') {
        return parse_number(ctx);
    }

    /* Identifier or function call */
    if (isalpha((unsigned char)*ctx->pos) || *ctx->pos == '_') {
        const char *start = ctx->pos;
        while (isalnum((unsigned char)*ctx->pos) || *ctx->pos == '_') ctx->pos++;
        size_t len = (size_t)(ctx->pos - start);
        char name[64];
        if (len >= sizeof(name)) len = sizeof(name) - 1;
        memcpy(name, start, len);
        name[len] = '\0';

        skip_ws(ctx);
        if (*ctx->pos == '(') {
            ctx->pos++;
            return call_function(ctx, name);
        }

        /* Variable lookup */
        double val = eval_get_var(ctx, name);
        if (isnan(val) && strcmp(name, "nan") != 0) {
            char msg[128];
            snprintf(msg, sizeof(msg), "undefined variable '%s'", name);
            set_error(ctx, msg);
        }
        return val;
    }

    set_error(ctx, "unexpected character");
    return NAN;
}

static double parse_postfix(eval_ctx_t *ctx) {
    double val = parse_primary(ctx);
    skip_ws(ctx);
    /* Factorial: 5! */
    while (*ctx->pos == '!' && *(ctx->pos + 1) != '=') {
        ctx->pos++;
        val = factorial_f(val);
    }
    return val;
}

static double parse_power(eval_ctx_t *ctx) {
    double base = parse_postfix(ctx);
    skip_ws(ctx);
    if (match_str(ctx, "**") || (*ctx->pos == '^' && *(ctx->pos + 1) != '^')) {
        if (*ctx->pos == '^') ctx->pos++;
        double exp_val = parse_unary(ctx); /* right-associative */
        return pow(base, exp_val);
    }
    return base;
}

static double parse_unary(eval_ctx_t *ctx) {
    skip_ws(ctx);
    if (match_char(ctx, '-')) return -parse_unary(ctx);
    if (match_char(ctx, '+')) return parse_unary(ctx);
    if (match_char(ctx, '~')) return (double)(~(long)parse_unary(ctx));
    if (*ctx->pos == '!' && *(ctx->pos + 1) != '=') {
        ctx->pos++;
        return parse_unary(ctx) == 0.0 ? 1.0 : 0.0;
    }
    return parse_power(ctx);
}

static double parse_product(eval_ctx_t *ctx) {
    double val = parse_unary(ctx);
    while (!ctx->has_error) {
        skip_ws(ctx);
        if (match_char(ctx, '*') && *ctx->pos != '*') val *= parse_unary(ctx);
        else if (match_char(ctx, '/')) { double d = parse_unary(ctx); val = d != 0 ? val / d : (set_error(ctx, "division by zero"), NAN); }
        else if (match_char(ctx, '%')) { double d = parse_unary(ctx); val = fmod(val, d); }
        else break;
    }
    return val;
}

static double parse_sum(eval_ctx_t *ctx) {
    double val = parse_product(ctx);
    while (!ctx->has_error) {
        skip_ws(ctx);
        if (match_char(ctx, '+')) val += parse_product(ctx);
        else if (match_char(ctx, '-')) val -= parse_product(ctx);
        else break;
    }
    return val;
}

static double parse_shift(eval_ctx_t *ctx) {
    double val = parse_sum(ctx);
    while (!ctx->has_error) {
        skip_ws(ctx);
        if (match_str(ctx, "<<")) val = (double)((long)val << (long)parse_sum(ctx));
        else if (match_str(ctx, ">>")) val = (double)((long)val >> (long)parse_sum(ctx));
        else break;
    }
    return val;
}

static double parse_compare(eval_ctx_t *ctx) {
    double val = parse_shift(ctx);
    while (!ctx->has_error) {
        skip_ws(ctx);
        if (match_str(ctx, "<=")) val = val <= parse_shift(ctx) ? 1.0 : 0.0;
        else if (match_str(ctx, ">=")) val = val >= parse_shift(ctx) ? 1.0 : 0.0;
        else if (match_char(ctx, '<')) val = val < parse_shift(ctx) ? 1.0 : 0.0;
        else if (match_char(ctx, '>')) val = val > parse_shift(ctx) ? 1.0 : 0.0;
        else break;
    }
    return val;
}

static double parse_equality(eval_ctx_t *ctx) {
    double val = parse_compare(ctx);
    while (!ctx->has_error) {
        skip_ws(ctx);
        if (match_str(ctx, "==")) val = val == parse_compare(ctx) ? 1.0 : 0.0;
        else if (match_str(ctx, "!=")) val = val != parse_compare(ctx) ? 1.0 : 0.0;
        else break;
    }
    return val;
}

static double parse_bitand(eval_ctx_t *ctx) {
    double val = parse_equality(ctx);
    while (!ctx->has_error) {
        skip_ws(ctx);
        if (*ctx->pos == '&' && *(ctx->pos+1) != '&') {
            ctx->pos++;
            val = (double)((long)val & (long)parse_equality(ctx));
        } else break;
    }
    return val;
}

static double parse_bitxor(eval_ctx_t *ctx) {
    double val = parse_bitand(ctx);
    /* Note: ^ is used for exponentiation in parse_power, so bitwise xor
       is handled only if we see ^^ */
    while (!ctx->has_error) {
        skip_ws(ctx);
        if (match_str(ctx, "^^")) val = (double)((long)val ^ (long)parse_bitand(ctx));
        else break;
    }
    return val;
}

static double parse_bitor(eval_ctx_t *ctx) {
    double val = parse_bitxor(ctx);
    while (!ctx->has_error) {
        skip_ws(ctx);
        if (*ctx->pos == '|' && *(ctx->pos+1) != '|') {
            ctx->pos++;
            val = (double)((long)val | (long)parse_bitxor(ctx));
        } else break;
    }
    return val;
}

static double parse_and(eval_ctx_t *ctx) {
    double val = parse_bitor(ctx);
    while (!ctx->has_error) {
        skip_ws(ctx);
        if (match_str(ctx, "&&")) {
            double right = parse_bitor(ctx);
            val = (val != 0.0 && right != 0.0) ? 1.0 : 0.0;
        } else break;
    }
    return val;
}

static double parse_or(eval_ctx_t *ctx) {
    double val = parse_and(ctx);
    while (!ctx->has_error) {
        skip_ws(ctx);
        if (match_str(ctx, "||")) {
            double right = parse_and(ctx);
            val = (val != 0.0 || right != 0.0) ? 1.0 : 0.0;
        } else break;
    }
    return val;
}

static double parse_ternary(eval_ctx_t *ctx) {
    double val = parse_or(ctx);
    skip_ws(ctx);
    if (match_char(ctx, '?')) {
        double then_val = parse_expr(ctx);
        if (!match_char(ctx, ':')) set_error(ctx, "expected ':'");
        double else_val = parse_expr(ctx);
        return val != 0.0 ? then_val : else_val;
    }
    return val;
}

static double parse_assign(eval_ctx_t *ctx) {
    /* Save position for possible variable assignment */
    const char *save = ctx->pos;
    skip_ws(ctx);

    if (isalpha((unsigned char)*ctx->pos) || *ctx->pos == '_') {
        const char *name_start = ctx->pos;
        while (isalnum((unsigned char)*ctx->pos) || *ctx->pos == '_') ctx->pos++;
        size_t len = (size_t)(ctx->pos - name_start);
        skip_ws(ctx);
        if (*ctx->pos == '=' && *(ctx->pos+1) != '=') {
            ctx->pos++; /* skip '=' */
            char name[64];
            if (len >= sizeof(name)) len = sizeof(name) - 1;
            memcpy(name, name_start, len);
            name[len] = '\0';
            double val = parse_assign(ctx);
            eval_set_var(ctx, name, val);
            return val;
        }
    }

    /* Not an assignment, backtrack */
    ctx->pos = save;
    return parse_ternary(ctx);
}

static double parse_expr(eval_ctx_t *ctx) {
    return parse_assign(ctx);
}

/* ── Public API ──────────────────────────────────────────────────────── */

double eval_expr(eval_ctx_t *ctx, const char *expr) {
    ctx->input = expr;
    ctx->pos = expr;
    ctx->has_error = false;
    ctx->error[0] = '\0';
    ctx->base = 10;

    double result = parse_expr(ctx);

    skip_ws(ctx);
    if (*ctx->pos != '\0' && *ctx->pos != ';') {
        set_error(ctx, "unexpected trailing characters");
    }

    return result;
}

void eval_format(eval_ctx_t *ctx, const char *expr, char *out, size_t out_len) {
    double val = eval_expr(ctx, expr);
    if (ctx->has_error) {
        snprintf(out, out_len, "error: %s", ctx->error);
        return;
    }

    switch (ctx->base) {
    case 16:
        snprintf(out, out_len, "0x%lx", (long)val);
        break;
    case 8:
        snprintf(out, out_len, "0o%lo", (long)val);
        break;
    case 2: {
        long v = (long)val;
        char bits[65];
        int i = 0;
        if (v == 0) { bits[i++] = '0'; }
        else {
            unsigned long u = (unsigned long)v;
            while (u) { bits[i++] = (u & 1) ? '1' : '0'; u >>= 1; }
        }
        bits[i] = '\0';
        /* Reverse */
        for (int a = 0, b = i - 1; a < b; a++, b--) {
            char t = bits[a]; bits[a] = bits[b]; bits[b] = t;
        }
        snprintf(out, out_len, "0b%s", bits);
        break;
    }
    default:
        if (val == (long)val && fabs(val) < 1e15)
            snprintf(out, out_len, "%ld", (long)val);
        else
            snprintf(out, out_len, "%.15g", val);
        break;
    }
}

void eval_multi(eval_ctx_t *ctx, const char *exprs, char *out, size_t out_len) {
    char *copy = strdup(exprs);
    char *save = NULL;
    char *token = strtok_r(copy, ";", &save);
    size_t pos = 0;

    while (token && pos < out_len - 1) {
        while (*token == ' ') token++;
        char *end = token + strlen(token);
        while (end > token && end[-1] == ' ') *--end = '\0';

        if (*token) {
            char buf[256];
            eval_format(ctx, token, buf, sizeof(buf));
            int n = snprintf(out + pos, out_len - pos, "%s%s",
                             pos > 0 ? "\n" : "", buf);
            if (n > 0) pos += (size_t)n;
        }
        token = strtok_r(NULL, ";", &save);
    }
    if (pos == 0 && out_len > 0) out[0] = '\0';
    free(copy);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Big Integer — for exact integer arithmetic
 * ═══════════════════════════════════════════════════════════════════════════ */

void bigint_from_str(bigint_t *b, const char *s) {
    memset(b, 0, sizeof(*b));
    if (*s == '-') { b->negative = true; s++; }
    int slen = (int)strlen(s);
    b->len = slen;
    for (int i = 0; i < slen; i++) {
        b->digits[i] = s[slen - 1 - i] - '0';
    }
}

void bigint_to_str(const bigint_t *b, char *s, size_t len) {
    if (b->len == 0) { snprintf(s, len, "0"); return; }
    size_t pos = 0;
    if (b->negative && pos < len - 1) s[pos++] = '-';
    for (int i = b->len - 1; i >= 0 && pos < len - 1; i--) {
        s[pos++] = '0' + b->digits[i];
    }
    s[pos] = '\0';
}

void bigint_add(const bigint_t *a, const bigint_t *b, bigint_t *result) {
    memset(result, 0, sizeof(*result));
    int maxlen = a->len > b->len ? a->len : b->len;
    int carry = 0;
    for (int i = 0; i < maxlen || carry; i++) {
        int sum = carry;
        if (i < a->len) sum += a->digits[i];
        if (i < b->len) sum += b->digits[i];
        result->digits[result->len++] = sum % 10;
        carry = sum / 10;
    }
}

void bigint_mul(const bigint_t *a, const bigint_t *b, bigint_t *result) {
    memset(result, 0, sizeof(*result));
    result->negative = a->negative != b->negative;
    for (int i = 0; i < a->len; i++) {
        int carry = 0;
        for (int j = 0; j < b->len || carry; j++) {
            int prod = result->digits[i + j] + carry;
            if (j < b->len) prod += a->digits[i] * b->digits[j];
            result->digits[i + j] = prod % 10;
            carry = prod / 10;
            if (i + j + 1 > result->len) result->len = i + j + 1;
        }
    }
    /* Trim leading zeros */
    while (result->len > 1 && result->digits[result->len - 1] == 0)
        result->len--;
}

void bigint_factorial(int n, bigint_t *result) {
    bigint_t temp;
    memset(result, 0, sizeof(*result));
    result->digits[0] = 1;
    result->len = 1;

    for (int i = 2; i <= n; i++) {
        char num[16];
        snprintf(num, sizeof(num), "%d", i);
        bigint_t factor;
        bigint_from_str(&factor, num);
        bigint_mul(result, &factor, &temp);
        *result = temp;
    }
}

bool bigint_is_prime(const bigint_t *n) {
    /* Simple primality for small numbers */
    if (n->len > 10) return false; /* Too big for simple test */
    long val = 0;
    for (int i = n->len - 1; i >= 0; i--) val = val * 10 + n->digits[i];
    if (val < 2) return false;
    if (val < 4) return true;
    if (val % 2 == 0 || val % 3 == 0) return false;
    for (long i = 5; i * i <= val; i += 6) {
        if (val % i == 0 || val % (i + 2) == 0) return false;
    }
    return true;
}
