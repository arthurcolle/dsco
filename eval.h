#ifndef DSCO_EVAL_H
#define DSCO_EVAL_H

#include <stdbool.h>
#include <stddef.h>

/*
 * Mathematical expression evaluator — recursive descent parser.
 * Inspired by Simon Tatham's spigot exact real calculator.
 *
 * Supports:
 *   - Arithmetic: + - * / % ^
 *   - Comparison: == != < > <= >=
 *   - Bitwise: & | ~ << >>
 *   - Functions: sqrt, cbrt, abs, ceil, floor, round
 *                sin, cos, tan, asin, acos, atan, atan2
 *                log, log2, log10, ln, exp
 *                min, max, gcd, lcm, fib, factorial (!)
 *                hex, oct, bin (format conversions)
 *   - Constants: pi, e, tau, phi, inf, nan
 *   - Variables: x=42; x*2
 *   - Hex/oct/bin literals: 0xFF, 0o77, 0b1010
 *   - Grouping: ( )
 *   - Multiple expressions separated by ;
 */

#define EVAL_MAX_VARS 64

typedef struct {
    char   name[64];
    double value;
} eval_var_t;

typedef struct {
    eval_var_t  vars[EVAL_MAX_VARS];
    int         var_count;
    const char *input;
    const char *pos;
    char        error[256];
    bool        has_error;
    int         base;       /* output base: 10, 16, 8, 2 */
} eval_ctx_t;

/* Initialize evaluator context */
void eval_init(eval_ctx_t *ctx);

/* Evaluate an expression string. Returns the result.
 * On error, ctx->has_error is set and ctx->error contains the message. */
double eval_expr(eval_ctx_t *ctx, const char *expr);

/* Evaluate and format result to string */
void eval_format(eval_ctx_t *ctx, const char *expr, char *out, size_t out_len);

/* Evaluate multiple semicolon-separated expressions */
void eval_multi(eval_ctx_t *ctx, const char *exprs, char *out, size_t out_len);

/* Set a variable */
void eval_set_var(eval_ctx_t *ctx, const char *name, double value);

/* Get a variable (NAN if not found) */
double eval_get_var(eval_ctx_t *ctx, const char *name);

/* ── Big integer support for exact integer arithmetic ────────────────── */

#define BIGINT_MAX_DIGITS 1024

typedef struct {
    int  digits[BIGINT_MAX_DIGITS]; /* stored little-endian, base 10 */
    int  len;
    bool negative;
} bigint_t;

void    bigint_from_str(bigint_t *b, const char *s);
void    bigint_to_str(const bigint_t *b, char *s, size_t len);
void    bigint_add(const bigint_t *a, const bigint_t *b, bigint_t *result);
void    bigint_mul(const bigint_t *a, const bigint_t *b, bigint_t *result);
void    bigint_factorial(int n, bigint_t *result);
bool    bigint_is_prime(const bigint_t *n);

#endif
