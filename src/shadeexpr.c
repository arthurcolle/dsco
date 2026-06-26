/* ── shadeexpr.c: compile a math formula to bytecode, eval it in a hot loop ───
 * Recursive-descent parser over the C-like infix grammar in shadeexpr.h,
 * emitting a flat stack-machine program. Identifiers resolve to fixed slot
 * indices and constants at compile time; function arity is checked at compile
 * time and baked into the CALL op, so eval is a branch-table walk with no
 * lookups, no allocation, and no global state. */

#include "shadeexpr.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stddef.h>

/* ── opcodes ─────────────────────────────────────────────────────────────── */
enum { OP_NUM, OP_VAR, OP_NEG, OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD, OP_POW, OP_CALL };

typedef struct {
    unsigned char op;
    unsigned char fn;
    short var;
    double num;
} instr_t;

struct shadeexpr {
    instr_t *code;
    int n, cap;
    int maxdepth;
    char *src;
};

/* ── variable + constant tables ──────────────────────────────────────────── */
static const struct {
    const char *name;
    int slot;
} VARS[] = {
    {"x", SE_X}, {"y", SE_Y}, {"z", SE_Z}, {"r", SE_R}, {"t", SE_T}, {"p", SE_P},
    {"q", SE_Q}, {"s", SE_S}, {"i", SE_I}, {"u", SE_U}, {"v", SE_V}, {"w", SE_W},
};
static const struct {
    const char *name;
    double val;
} CONSTS[] = {
    {"pi", 3.14159265358979323846},
    {"tau", 6.28318530717958647692},
    {"e", 2.71828182845904523536},
    {"phi", 1.61803398874989484820},
};

/* ── function table (id order matters: used directly in eval switch) ─────── */
enum {
    FN_SIN,
    FN_COS,
    FN_TAN,
    FN_ASIN,
    FN_ACOS,
    FN_ATAN,
    FN_EXP,
    FN_LOG,
    FN_LOG2,
    FN_SQRT,
    FN_CBRT,
    FN_ABS,
    FN_FLOOR,
    FN_CEIL,
    FN_ROUND,
    FN_FRAC,
    FN_SIGN,
    FN_ATAN2,
    FN_MOD,
    FN_MIN,
    FN_MAX,
    FN_POW,
    FN_HYPOT,
    FN_STEP,
    FN_CLAMP,
    FN_MIX,
    FN_SMOOTHSTEP,
    FN_LENGTH,
    FN_DIST,
    FN_SPHERE,
    FN_BOX,
    FN_TORUS,
    FN_NOISE,
    FN_FBM
};
static const struct {
    const char *name;
    int id;
    int arity;
} FUNCS[] = {
    {"sin", FN_SIN, 1},       {"cos", FN_COS, 1},     {"tan", FN_TAN, 1},
    {"asin", FN_ASIN, 1},     {"acos", FN_ACOS, 1},   {"atan", FN_ATAN, 1},
    {"exp", FN_EXP, 1},       {"log", FN_LOG, 1},     {"log2", FN_LOG2, 1},
    {"sqrt", FN_SQRT, 1},     {"cbrt", FN_CBRT, 1},   {"abs", FN_ABS, 1},
    {"floor", FN_FLOOR, 1},   {"ceil", FN_CEIL, 1},   {"round", FN_ROUND, 1},
    {"frac", FN_FRAC, 1},     {"sign", FN_SIGN, 1},   {"atan2", FN_ATAN2, 2},
    {"mod", FN_MOD, 2},       {"min", FN_MIN, 2},     {"max", FN_MAX, 2},
    {"pow", FN_POW, 2},       {"hypot", FN_HYPOT, 2}, {"step", FN_STEP, 2},
    {"clamp", FN_CLAMP, 3},   {"mix", FN_MIX, 3},     {"smoothstep", FN_SMOOTHSTEP, 3},
    {"length", FN_LENGTH, 3}, {"dist", FN_DIST, 6},   {"sphere", FN_SPHERE, 4},
    {"box", FN_BOX, 6},       {"torus", FN_TORUS, 5}, {"noise", FN_NOISE, 3},
    {"fbm", FN_FBM, 3},
};
#define NFUNCS ((int)(sizeof FUNCS / sizeof FUNCS[0]))

/* ── parser state ────────────────────────────────────────────────────────── */
typedef struct {
    const char *s; /* full source */
    const char *p; /* cursor */
    shadeexpr_t *e;
    int depth; /* current simulated stack depth */
    char err[160];
    int failed;
} P;

static void emit(P *pp, instr_t in, int push) {
    if (pp->failed)
        return;
    shadeexpr_t *e = pp->e;
    if (e->n == e->cap) {
        int nc = e->cap ? e->cap * 2 : 32;
        instr_t *nx = realloc(e->code, (size_t)nc * sizeof *nx);
        if (!nx) {
            snprintf(pp->err, sizeof pp->err, "out of memory");
            pp->failed = 1;
            return;
        }
        e->code = nx;
        e->cap = nc;
    }
    e->code[e->n++] = in;
    pp->depth += push; /* net stack effect */
    if (pp->depth > e->maxdepth)
        e->maxdepth = pp->depth;
}

static void fail(P *pp, const char *msg) {
    if (pp->failed)
        return;
    pp->failed = 1;
    snprintf(pp->err, sizeof pp->err, "%s (at offset %d)", msg, (int)(pp->p - pp->s));
}

static void skip_ws(P *pp) {
    while (*pp->p == ' ' || *pp->p == '\t' || *pp->p == '\n' || *pp->p == '\r')
        pp->p++;
}

static double parse_expr(P *pp); /* fwd */

/* atom := number | const | var | func '(' args ')' | '(' expr ')' */
static void parse_atom(P *pp) {
    skip_ws(pp);
    char c = *pp->p;
    if (c == '(') {
        pp->p++;
        parse_expr(pp);
        skip_ws(pp);
        if (*pp->p != ')') {
            fail(pp, "expected ')'");
            return;
        }
        pp->p++;
        return;
    }
    if ((c >= '0' && c <= '9') || c == '.') {
        char *end = NULL;
        double v = strtod(pp->p, &end);
        if (end == pp->p) {
            fail(pp, "bad number");
            return;
        }
        pp->p = end;
        emit(pp, (instr_t){.op = OP_NUM, .num = v}, +1);
        return;
    }
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_') {
        char id[32];
        int k = 0;
        while (((*pp->p >= 'a' && *pp->p <= 'z') || (*pp->p >= 'A' && *pp->p <= 'Z') ||
                (*pp->p >= '0' && *pp->p <= '9') || *pp->p == '_') &&
               k < (int)sizeof id - 1)
            id[k++] = *pp->p++;
        id[k] = '\0';
        skip_ws(pp);
        if (*pp->p == '(') { /* function call */
            int fi = -1;
            for (int j = 0; j < NFUNCS; j++)
                if (!strcmp(id, FUNCS[j].name)) {
                    fi = j;
                    break;
                }
            if (fi < 0) {
                fail(pp, "unknown function");
                return;
            }
            pp->p++; /* consume '(' */
            int argc = 0;
            skip_ws(pp);
            if (*pp->p != ')') {
                for (;;) {
                    parse_expr(pp);
                    argc++;
                    skip_ws(pp);
                    if (*pp->p == ',') {
                        pp->p++;
                        continue;
                    }
                    break;
                }
            }
            skip_ws(pp);
            if (*pp->p != ')') {
                fail(pp, "expected ')' in call");
                return;
            }
            pp->p++;
            if (argc != FUNCS[fi].arity) {
                fail(pp, "wrong number of arguments");
                return;
            }
            /* net stack effect: pops arity, pushes 1 */
            emit(pp, (instr_t){.op = OP_CALL, .fn = (unsigned char)FUNCS[fi].id}, 1 - argc);
            return;
        }
        for (int j = 0; j < (int)(sizeof VARS / sizeof VARS[0]); j++)
            if (!strcmp(id, VARS[j].name)) {
                emit(pp, (instr_t){.op = OP_VAR, .var = (short)VARS[j].slot}, +1);
                return;
            }
        for (int j = 0; j < (int)(sizeof CONSTS / sizeof CONSTS[0]); j++)
            if (!strcmp(id, CONSTS[j].name)) {
                emit(pp, (instr_t){.op = OP_NUM, .num = CONSTS[j].val}, +1);
                return;
            }
        fail(pp, "unknown identifier");
        return;
    }
    fail(pp, "unexpected character");
}

/* unary := ('-'|'+')? atom */
static void parse_unary(P *pp) {
    skip_ws(pp);
    if (*pp->p == '-') {
        pp->p++;
        parse_unary(pp);
        emit(pp, (instr_t){.op = OP_NEG}, 0);
        return;
    }
    if (*pp->p == '+') {
        pp->p++;
        parse_unary(pp);
        return;
    }
    parse_atom(pp);
}

/* pow := unary ('^' unary)*  (right-assoc) */
static void parse_pow(P *pp) {
    parse_unary(pp);
    skip_ws(pp);
    if (*pp->p == '^') {
        pp->p++;
        parse_pow(pp); /* right associative */
        emit(pp, (instr_t){.op = OP_POW}, -1);
    }
}

/* term := pow (('*'|'/'|'%') pow)* */
static void parse_term(P *pp) {
    parse_pow(pp);
    for (;;) {
        skip_ws(pp);
        char c = *pp->p;
        if (c == '*') {
            pp->p++;
            parse_pow(pp);
            emit(pp, (instr_t){.op = OP_MUL}, -1);
        } else if (c == '/') {
            pp->p++;
            parse_pow(pp);
            emit(pp, (instr_t){.op = OP_DIV}, -1);
        } else if (c == '%') {
            pp->p++;
            parse_pow(pp);
            emit(pp, (instr_t){.op = OP_MOD}, -1);
        } else
            break;
    }
}

/* expr := term (('+'|'-') term)* */
static double parse_expr(P *pp) {
    parse_term(pp);
    for (;;) {
        skip_ws(pp);
        char c = *pp->p;
        if (c == '+') {
            pp->p++;
            parse_term(pp);
            emit(pp, (instr_t){.op = OP_ADD}, -1);
        } else if (c == '-') {
            pp->p++;
            parse_term(pp);
            emit(pp, (instr_t){.op = OP_SUB}, -1);
        } else
            break;
    }
    return 0;
}

/* ── compile ─────────────────────────────────────────────────────────────── */
shadeexpr_t *shadeexpr_compile(const char *src, char *errbuf, size_t errcap) {
    if (!src) {
        if (errbuf && errcap)
            snprintf(errbuf, errcap, "null source");
        return NULL;
    }
    shadeexpr_t *e = calloc(1, sizeof *e);
    if (!e) {
        if (errbuf && errcap)
            snprintf(errbuf, errcap, "out of memory");
        return NULL;
    }
    P pp;
    memset(&pp, 0, sizeof pp);
    pp.s = src;
    pp.p = src;
    pp.e = e;
    parse_expr(&pp);
    skip_ws(&pp);
    if (!pp.failed && *pp.p != '\0')
        fail(&pp, "trailing characters");
    if (!pp.failed && e->maxdepth > 64)
        fail(&pp, "expression too deep");
    if (pp.failed) {
        if (errbuf && errcap)
            snprintf(errbuf, errcap, "%s", pp.err);
        free(e->code);
        free(e);
        return NULL;
    }
    e->src = src ? strdup(src) : NULL;
    if (errbuf && errcap)
        errbuf[0] = '\0';
    return e;
}

/* ── deterministic value noise (no globals, no rand) ─────────────────────── */
static double vhash3(int x, int y, int z) {
    unsigned int h = (unsigned)(x * 374761393 + y * 668265263 + z * 1442695040u + 0x9E3779B9u);
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return (double)(h & 0x00FFFFFFu) / (double)0x01000000; /* [0,1) */
}
static double vnoise3(double x, double y, double z) {
    double xi = floor(x), yi = floor(y), zi = floor(z);
    double xf = x - xi, yf = y - yi, zf = z - zi;
    double u = xf * xf * (3.0 - 2.0 * xf);
    double v = yf * yf * (3.0 - 2.0 * yf);
    double w = zf * zf * (3.0 - 2.0 * zf);
    int X = (int)xi, Y = (int)yi, Z = (int)zi;
    double c000 = vhash3(X, Y, Z), c100 = vhash3(X + 1, Y, Z);
    double c010 = vhash3(X, Y + 1, Z), c110 = vhash3(X + 1, Y + 1, Z);
    double c001 = vhash3(X, Y, Z + 1), c101 = vhash3(X + 1, Y, Z + 1);
    double c011 = vhash3(X, Y + 1, Z + 1), c111 = vhash3(X + 1, Y + 1, Z + 1);
    double x00 = c000 + (c100 - c000) * u, x10 = c010 + (c110 - c010) * u;
    double x01 = c001 + (c101 - c001) * u, x11 = c011 + (c111 - c011) * u;
    double y0 = x00 + (x10 - x00) * v, y1 = x01 + (x11 - x01) * v;
    double r = y0 + (y1 - y0) * w;
    return r * 2.0 - 1.0; /* [-1,1] */
}
static double fbm3(double x, double y, double z) {
    double sum = 0.0, amp = 0.5, freq = 1.0;
    for (int o = 0; o < 4; o++) {
        sum += amp * vnoise3(x * freq, y * freq, z * freq);
        freq *= 2.0;
        amp *= 0.5;
    }
    return sum;
}

/* ── eval ────────────────────────────────────────────────────────────────── */
static double clampd(double v, double lo, double hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

double shadeexpr_eval(const shadeexpr_t *e, const double *slots) {
    if (!e)
        return 0.0;
    double st[66];
    int sp = 0;
    const instr_t *c = e->code;
    for (int ip = 0; ip < e->n; ip++) {
        const instr_t *in = &c[ip];
        switch (in->op) {
            case OP_NUM:
                st[sp++] = in->num;
                break;
            case OP_VAR:
                st[sp++] = slots[in->var];
                break;
            case OP_NEG:
                st[sp - 1] = -st[sp - 1];
                break;
            case OP_ADD:
                st[sp - 2] += st[sp - 1];
                sp--;
                break;
            case OP_SUB:
                st[sp - 2] -= st[sp - 1];
                sp--;
                break;
            case OP_MUL:
                st[sp - 2] *= st[sp - 1];
                sp--;
                break;
            case OP_DIV:
                st[sp - 2] /= st[sp - 1];
                sp--;
                break;
            case OP_MOD:
                st[sp - 2] = fmod(st[sp - 2], st[sp - 1]);
                sp--;
                break;
            case OP_POW:
                st[sp - 2] = pow(st[sp - 2], st[sp - 1]);
                sp--;
                break;
            case OP_CALL: {
                switch (in->fn) {
                    case FN_SIN:
                        st[sp - 1] = sin(st[sp - 1]);
                        break;
                    case FN_COS:
                        st[sp - 1] = cos(st[sp - 1]);
                        break;
                    case FN_TAN:
                        st[sp - 1] = tan(st[sp - 1]);
                        break;
                    case FN_ASIN:
                        st[sp - 1] = asin(clampd(st[sp - 1], -1, 1));
                        break;
                    case FN_ACOS:
                        st[sp - 1] = acos(clampd(st[sp - 1], -1, 1));
                        break;
                    case FN_ATAN:
                        st[sp - 1] = atan(st[sp - 1]);
                        break;
                    case FN_EXP:
                        st[sp - 1] = exp(st[sp - 1]);
                        break;
                    case FN_LOG:
                        st[sp - 1] = log(st[sp - 1]);
                        break;
                    case FN_LOG2:
                        st[sp - 1] = log2(st[sp - 1]);
                        break;
                    case FN_SQRT:
                        st[sp - 1] = sqrt(st[sp - 1]);
                        break;
                    case FN_CBRT:
                        st[sp - 1] = cbrt(st[sp - 1]);
                        break;
                    case FN_ABS:
                        st[sp - 1] = fabs(st[sp - 1]);
                        break;
                    case FN_FLOOR:
                        st[sp - 1] = floor(st[sp - 1]);
                        break;
                    case FN_CEIL:
                        st[sp - 1] = ceil(st[sp - 1]);
                        break;
                    case FN_ROUND:
                        st[sp - 1] = round(st[sp - 1]);
                        break;
                    case FN_FRAC:
                        st[sp - 1] = st[sp - 1] - floor(st[sp - 1]);
                        break;
                    case FN_SIGN:
                        st[sp - 1] = (st[sp - 1] > 0) - (st[sp - 1] < 0);
                        break;
                    case FN_ATAN2:
                        st[sp - 2] = atan2(st[sp - 2], st[sp - 1]);
                        sp--;
                        break;
                    case FN_MOD:
                        st[sp - 2] = fmod(st[sp - 2], st[sp - 1]);
                        sp--;
                        break;
                    case FN_MIN:
                        st[sp - 2] = st[sp - 2] < st[sp - 1] ? st[sp - 2] : st[sp - 1];
                        sp--;
                        break;
                    case FN_MAX:
                        st[sp - 2] = st[sp - 2] > st[sp - 1] ? st[sp - 2] : st[sp - 1];
                        sp--;
                        break;
                    case FN_POW:
                        st[sp - 2] = pow(st[sp - 2], st[sp - 1]);
                        sp--;
                        break;
                    case FN_HYPOT:
                        st[sp - 2] = hypot(st[sp - 2], st[sp - 1]);
                        sp--;
                        break;
                    case FN_STEP:
                        st[sp - 2] = st[sp - 1] < st[sp - 2] ? 0.0 : 1.0;
                        sp--;
                        break;
                    case FN_CLAMP:
                        st[sp - 3] = clampd(st[sp - 3], st[sp - 2], st[sp - 1]);
                        sp -= 2;
                        break;
                    case FN_MIX:
                        st[sp - 3] = st[sp - 3] + (st[sp - 2] - st[sp - 3]) * st[sp - 1];
                        sp -= 2;
                        break;
                    case FN_SMOOTHSTEP: {
                        double e0 = st[sp - 3], e1 = st[sp - 2], xx = st[sp - 1];
                        double tt = clampd((xx - e0) / (e1 - e0), 0.0, 1.0);
                        st[sp - 3] = tt * tt * (3.0 - 2.0 * tt);
                        sp -= 2;
                        break;
                    }
                    case FN_LENGTH:
                        st[sp - 3] = sqrt(st[sp - 3] * st[sp - 3] + st[sp - 2] * st[sp - 2] +
                                          st[sp - 1] * st[sp - 1]);
                        sp -= 2;
                        break;
                    case FN_DIST: {
                        double dx = st[sp - 6] - st[sp - 3], dy = st[sp - 5] - st[sp - 2],
                               dz = st[sp - 4] - st[sp - 1];
                        st[sp - 6] = sqrt(dx * dx + dy * dy + dz * dz);
                        sp -= 5;
                        break;
                    }
                    case FN_SPHERE: {
                        double px = st[sp - 4], py = st[sp - 3], pz = st[sp - 2], rr = st[sp - 1];
                        st[sp - 4] = sqrt(px * px + py * py + pz * pz) - rr;
                        sp -= 3;
                        break;
                    }
                    case FN_BOX: {
                        double px = st[sp - 6], py = st[sp - 5], pz = st[sp - 4], bx = st[sp - 3],
                               by = st[sp - 2], bz = st[sp - 1];
                        double qx = fabs(px) - bx, qy = fabs(py) - by, qz = fabs(pz) - bz;
                        double ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0, oz = qz > 0 ? qz : 0;
                        double mx = qx > qy ? (qx > qz ? qx : qz) : (qy > qz ? qy : qz);
                        st[sp - 6] = sqrt(ox * ox + oy * oy + oz * oz) + (mx < 0 ? mx : 0);
                        sp -= 5;
                        break;
                    }
                    case FN_TORUS: {
                        double px = st[sp - 5], py = st[sp - 4], pz = st[sp - 3], RR = st[sp - 2],
                               rr = st[sp - 1];
                        double qx = sqrt(px * px + pz * pz) - RR;
                        st[sp - 5] = sqrt(qx * qx + py * py) - rr;
                        sp -= 4;
                        break;
                    }
                    case FN_NOISE:
                        st[sp - 3] = vnoise3(st[sp - 3], st[sp - 2], st[sp - 1]);
                        sp -= 2;
                        break;
                    case FN_FBM:
                        st[sp - 3] = fbm3(st[sp - 3], st[sp - 2], st[sp - 1]);
                        sp -= 2;
                        break;
                    default:
                        break;
                }
                break;
            }
            default:
                break;
        }
    }
    return sp > 0 ? st[sp - 1] : 0.0;
}

const char *shadeexpr_source(const shadeexpr_t *e) {
    return e ? e->src : NULL;
}

void shadeexpr_free(shadeexpr_t *e) {
    if (!e)
        return;
    free(e->code);
    free(e->src);
    free(e);
}
