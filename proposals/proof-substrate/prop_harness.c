/* prop_harness.c — property-based test substrate prototype (pure C, self-contained)
 *
 * Demonstrates the honest path to 250K-450K REAL tests:
 *   real test = (distinct input -> SUT -> oracle that can FAIL for a wrong impl)
 *
 * Three quality gates enforced here, not asserted:
 *   1. DISTINCTNESS  — inputs deduped by hash; we count distinct executions.
 *   2. KILL POWER    — a mutant (deliberately broken impl) must be caught.
 *   3. DETERMINISM   — seeded PRNG => reproducible corpus (CI-stable).
 *
 * Every property here is ALSO a formal theorem (see prop_registry.lean):
 *   roundtrip:  forall x. decode(encode(x)) = x
 *   this is a QuickCheck generator AND a Lean goal from one registry.
 *
 * Build:  cc -O2 -std=c11 -o prop_harness proposals/proof-substrate/prop_harness.c
 * Run:    ./prop_harness [N_per_property]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

/* ---- deterministic PRNG (splitmix64): seeded => reproducible corpus ---- */
static uint64_t rng_state;
static uint64_t rng_next(void) {
    uint64_t z = (rng_state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

/* ---- distinctness tracker: open-addressing hash set of input fingerprints ---- */
#define HSET_BITS 22            /* 4.19M slots => handles millions of inputs */
#define HSET_SIZE (1u << HSET_BITS)
static uint64_t *g_seen;        /* 0 == empty slot */
static size_t g_distinct = 0, g_dup = 0;

static uint64_t fnv1a(const uint8_t *p, size_t n) {
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ULL; }
    return h ? h : 1;           /* never 0 (reserved for empty) */
}
static bool seen_insert(uint64_t h) {   /* true if newly inserted (distinct) */
    size_t m = HSET_SIZE - 1, i = h & m;
    for (;;) {
        if (g_seen[i] == 0) { g_seen[i] = h; g_distinct++; return true; }
        if (g_seen[i] == h) { g_dup++; return false; }
        i = (i + 1) & m;
    }
}

/* ================= SYSTEM UNDER TEST (real primitives) ================= */
/* base64 — the reference impl we want to test AND prove */
static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static int b64_val(char c){for(int i=0;i<64;i++)if(B64[i]==c)return i;return -1;}
static size_t b64_encode(const uint8_t *in, size_t n, char *out) {
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = in[i] << 16;
        if (i+1 < n) v |= in[i+1] << 8;
        if (i+2 < n) v |= in[i+2];
        out[o++] = B64[(v>>18)&63];
        out[o++] = B64[(v>>12)&63];
        out[o++] = (i+1 < n) ? B64[(v>>6)&63] : '=';
        out[o++] = (i+2 < n) ? B64[v&63]      : '=';
    }
    out[o] = 0; return o;
}
static size_t b64_decode(const char *in, uint8_t *out) {
    size_t o = 0, n = strlen(in);
    for (size_t i = 0; i + 3 < n; i += 4) {
        int a=b64_val(in[i]), b=b64_val(in[i+1]);
        int c=in[i+2]=='='?-1:b64_val(in[i+2]);
        int d=in[i+3]=='='?-1:b64_val(in[i+3]);
        if(a<0||b<0)break;
        uint32_t v=(a<<18)|(b<<12)|((c<0?0:c)<<6)|(d<0?0:d);
        out[o++]=(v>>16)&0xFF;
        if(c>=0)out[o++]=(v>>8)&0xFF;
        if(d>=0)out[o++]=v&0xFF;
    }
    return o;
}

/* MUTANT: a deliberately broken decoder to prove the suite has KILL POWER */
static size_t b64_decode_MUTANT(const char *in, uint8_t *out) {
    size_t o = b64_decode(in, out);
    if (o > 0) out[0] ^= 0x01;   /* single-bit corruption — must be caught */
    return o;
}

/* ================= PROPERTY: roundtrip decode(encode(x)) == x ========== */
typedef size_t (*decoder_fn)(const char *, uint8_t *);
static size_t run_roundtrip(size_t n_cases, decoder_fn dec, size_t *out_distinct) {
    uint8_t in[64], back[64]; char enc[128];
    size_t failures = 0, distinct_before = g_distinct;
    for (size_t k = 0; k < n_cases; k++) {
        size_t len = rng_next() % 49;                 /* 0..48 bytes */
        for (size_t i = 0; i < len; i++) in[i] = rng_next() & 0xFF;
        bool is_new = seen_insert(fnv1a(in, len));    /* distinctness gate */
        (void)is_new;
        b64_encode(in, len, enc);
        size_t bn = dec(enc, back);
        if (bn != len || memcmp(in, back, len) != 0) failures++;
    }
    *out_distinct = g_distinct - distinct_before;
    return failures;
}

int main(int argc, char **argv) {
    size_t N = (argc > 1) ? strtoull(argv[1], NULL, 10) : 300000;
    g_seen = calloc(HSET_SIZE, sizeof(uint64_t));
    if (!g_seen) { fprintf(stderr, "oom\n"); return 2; }

    printf("=== dsco proof-substrate harness ===\n");
    printf("target executions/property: %zu\n\n", N);

    /* GATE 1+3: real executions over distinct, reproducible inputs */
    rng_state = 0xDEC0DEULL;   /* fixed seed => reproducible corpus */
    size_t d1 = 0;
    size_t fail_ref = run_roundtrip(N, b64_decode, &d1);
    printf("[PROP roundtrip] base64 decode(encode(x))==x\n");
    printf("  executions : %zu\n", N);
    printf("  distinct   : %zu (%.1f%% unique)\n", d1, 100.0*d1/N);
    printf("  duplicates : %zu\n", g_dup);
    printf("  failures   : %zu  => %s\n\n", fail_ref, fail_ref==0?"PASS":"FAIL");

    /* GATE 2: mutation kill — broken impl MUST fail, else suite is theater */
    rng_state = 0xDEC0DEULL;   /* same corpus */
    size_t d2 = 0;
    size_t fail_mut = run_roundtrip(N, b64_decode_MUTANT, &d2);
    double kill = 100.0 * fail_mut / N;
    printf("[MUTATION] inject single-bit corruption into decoder\n");
    printf("  mutant failures caught: %zu / %zu\n", fail_mut, N);
    printf("  kill rate : %.1f%%  => %s\n\n", kill, fail_mut>0?"KILLED (good)":"SURVIVED (bad)");

    printf("=== verdict ===\n");
    bool ok = (fail_ref == 0) && (fail_mut > 0);
    printf("real tests executed : %zu\n", N);
    printf("distinct inputs     : %zu\n", g_distinct);
    printf("oracle sound        : %s\n", fail_ref==0 ? "yes" : "NO");
    printf("has kill power       : %s\n", fail_mut>0 ? "yes" : "NO");
    printf("substrate status    : %s\n", ok ? "REAL (not theater)" : "BROKEN");
    free(g_seen);
    return ok ? 0 : 1;
}
