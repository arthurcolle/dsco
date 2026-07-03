/* bench_libs.c — before/after profiling for modern-C-lib candidates vs DSCO
 * incumbents. Standalone: links vendored libs + a few DSCO sources directly so
 * it can run before any main-tree integration. Reports ns/op + throughput. */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <math.h>

#include "../include/json_util.h" /* DSCO incumbent parser */
#include "yyjson.h"               /* candidate */
#include "picohttpparser.h"       /* candidate */
#include "blake3.h"               /* candidate */
#if defined(__ARM_NEON)
#include <arm_neon.h>
/* NEON f32 cosine — the SIMD ceiling. (SimSIMD's C API churned to "numkong"
 * tiled dispatch; a stable drop-in needs a pinned older release or ~this.) */
static float neon_cosine(const float *a, const float *b, int n) {
    float32x4_t vdot = vdupq_n_f32(0), vna = vdupq_n_f32(0), vnb = vdupq_n_f32(0);
    int i = 0;
    for (; i + 4 <= n; i += 4) {
        float32x4_t xa = vld1q_f32(a + i), xb = vld1q_f32(b + i);
        vdot = vfmaq_f32(vdot, xa, xb);
        vna = vfmaq_f32(vna, xa, xa);
        vnb = vfmaq_f32(vnb, xb, xb);
    }
    float dot = vaddvq_f32(vdot), na = vaddvq_f32(vna), nb = vaddvq_f32(vnb);
    for (; i < n; i++) {
        dot += a[i] * b[i];
        na += a[i] * a[i];
        nb += b[i] * b[i];
    }
    return dot / (sqrtf(na) * sqrtf(nb) + 1e-9f);
}
#endif

static double now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}
#define BENCH(label, iters, ...)                                                                   \
    do {                                                                                           \
        /* warmup */                                                                               \
        for (int _w = 0; _w < (iters) / 10 + 1; _w++) {                                            \
            __VA_ARGS__                                                                            \
        }                                                                                          \
        double _t0 = now_ns();                                                                     \
        for (int _i = 0; _i < (iters); _i++) {                                                     \
            __VA_ARGS__                                                                            \
        }                                                                                          \
        double _t1 = now_ns();                                                                     \
        double _ns = (_t1 - _t0) / (iters);                                                        \
        printf("  %-34s %10.1f ns/op  (%d iters)\n", label, _ns, iters);                           \
        g_last_ns = _ns;                                                                           \
    } while (0)
static double g_last_ns;

/* ── realistic payloads ─────────────────────────────────────────────────── */
static const char *ANTHROPIC =
    "{\"id\":\"msg_01\",\"type\":\"message\",\"role\":\"assistant\",\"model\":\"claude\","
    "\"content\":"
    "[{\"type\":\"text\",\"text\":\"Let me analyze this codebase and propose improvements to the "
    "provider routing layer with careful attention to memory safety.\"},{\"type\":\"tool_use\","
    "\"id\":\"toolu_01\",\"name\":\"bash\",\"input\":{\"command\":\"grep -rn tools_execute "
    "src/"
    "\",\"timeout\":120000,\"description\":\"search\"}}],\"stop_reason\":\"tool_use\",\"usage\":"
    "{\"input_tokens\":2451,\"output_tokens\":184}}";

static const char *CHATREQ =
    "{\"model\":\"claude\",\"stream\":true,\"temperature\":0.7,\"max_tokens\":2048,\"messages\":"
    "[{\"role\":\"system\",\"content\":\"You are dsco.\"},{\"role\":\"user\",\"content\":\"Review "
    "src/net_tool.c and propose 10 augmentations with profiling.\"}],\"stop\":[\"\\n\\n\"]}";

int main(void) {
    printf("=== DSCO modern-lib benchmark (Apple Silicon) ===\n");

    /* ── 1. JSON: parse + field extraction ─────────────────────────────── */
    printf("\n[1] JSON parse+extract (Anthropic response, %zu bytes)\n", strlen(ANTHROPIC));
    volatile double sink = 0;
    /* DSCO incumbent: parse full response into content blocks */
    BENCH("dsco json_parse_response", 200000, {
        parsed_response_t r;
        json_parse_response(ANTHROPIC, &r);
        sink += r.count;
        json_free_response(&r);
    });
    double dsco_json = g_last_ns;
    /* yyjson: parse + walk to same fields */
    BENCH("yyjson read+walk", 200000, {
        yyjson_doc *d = yyjson_read(ANTHROPIC, strlen(ANTHROPIC), 0);
        yyjson_val *content = yyjson_obj_get(yyjson_doc_get_root(d), "content");
        sink += yyjson_arr_size(content);
        yyjson_doc_free(d);
    });
    printf("  -> yyjson speedup: %.2fx\n", dsco_json / g_last_ns);

    /* field-extract pattern used all over DSCO: pull one string key */
    printf("\n[2] JSON single-field extract (chat body, %zu bytes)\n", strlen(CHATREQ));
    BENCH("dsco json_get_str(model)", 500000, {
        char *m = json_get_str(CHATREQ, "model");
        sink += m ? m[0] : 0;
        free(m);
    });
    double dsco_get = g_last_ns;
    BENCH("yyjson obj_get(model)", 500000, {
        yyjson_doc *d = yyjson_read(CHATREQ, strlen(CHATREQ), 0);
        yyjson_val *m = yyjson_obj_get(yyjson_doc_get_root(d), "model");
        sink += yyjson_get_str(m) ? yyjson_get_str(m)[0] : 0;
        yyjson_doc_free(d);
    });
    printf("  -> yyjson %.2fx (note: yyjson parses whole doc; dsco scans to key)\n",
           dsco_get / g_last_ns);
    /* yyjson with reuse (parse once, N lookups) — the realistic chat-route pattern */
    yyjson_doc *cd = yyjson_read(CHATREQ, strlen(CHATREQ), 0);
    yyjson_val *croot = yyjson_doc_get_root(cd);
    BENCH("yyjson obj_get (doc reused)", 2000000, {
        yyjson_val *m = yyjson_obj_get(croot, "model");
        sink += yyjson_get_str(m)[0];
    });
    yyjson_doc_free(cd);

    /* ── 3. Hashing ────────────────────────────────────────────────────── */
    printf("\n[3] Hashing throughput\n");
    size_t HN = 1 << 20; /* 1 MiB */
    uint8_t *buf = malloc(HN);
    for (size_t i = 0; i < HN; i++)
        buf[i] = (uint8_t)(i * 2654435761u >> 13);
    uint8_t out[32];
    BENCH("blake3 (1 MiB)", 2000, {
        blake3_hasher h;
        blake3_hasher_init(&h);
        blake3_hasher_update(&h, buf, HN);
        blake3_hasher_finalize(&h, out, 32);
        sink += out[0];
    });
    double mbps = (HN / (1024.0 * 1024.0)) / (g_last_ns / 1e9);
    printf("  -> blake3: %.0f MiB/s\n", mbps);
    /* small-input (file fingerprint size ~4 KiB) */
    BENCH("blake3 (4 KiB)", 200000, {
        blake3_hasher h;
        blake3_hasher_init(&h);
        blake3_hasher_update(&h, buf, 4096);
        blake3_hasher_finalize(&h, out, 32);
        sink += out[0];
    });
    free(buf);

    /* ── 4. Vector cosine similarity ───────────────────────────────────── */
    printf("\n[4] Vector cosine similarity (dim=1024, the vecstore hot loop)\n");
    int DIM = 1024;
    float *va = malloc(DIM * sizeof(float)), *vb = malloc(DIM * sizeof(float));
    for (int i = 0; i < DIM; i++) {
        va[i] = sinf((float)i * 0.01f);
        vb[i] = cosf((float)i * 0.017f);
    }
    /* DSCO incumbent scalar cosine (replicate cosine_similarity_f semantics) */
    BENCH("dsco scalar cosine", 500000, {
        float dot = 0, na = 0, nb = 0;
        for (int i = 0; i < DIM; i++) {
            dot += va[i] * vb[i];
            na += va[i] * va[i];
            nb += vb[i] * vb[i];
        }
        sink += dot / (sqrtf(na) * sqrtf(nb) + 1e-9f);
    });
    double dsco_cos = g_last_ns;
#if defined(__ARM_NEON)
    BENCH("NEON cosine (SIMD ceiling)", 500000, { sink += neon_cosine(va, vb, DIM); });
    printf("  -> SIMD speedup: %.2fx\n", dsco_cos / g_last_ns);
#endif
    free(va);
    free(vb);

    /* ── 5. HTTP request-line + header parse ───────────────────────────── */
    printf("\n[5] HTTP request parse (the net_server.c sscanf path)\n");
    const char *req = "POST /v1/chat/completions HTTP/1.1\r\nHost: localhost:7547\r\n"
                      "Content-Type: application/json\r\nContent-Length: 512\r\n"
                      "X-DSCO-Auth: deadbeef\r\nConnection: keep-alive\r\n\r\n";
    size_t reqlen = strlen(req);
    BENCH("dsco sscanf request-line", 500000, {
        char method[16], path[256];
        sscanf(req, "%15s %255s", method, path);
        sink += method[0] + path[1];
    });
    double dsco_http = g_last_ns;
    BENCH("picohttpparser full headers", 500000, {
        const char *m, *p;
        size_t ml, pl, nh = 16;
        int mv;
        struct phr_header hs[16];
        phr_parse_request(req, reqlen, &m, &ml, &p, &pl, &mv, hs, &nh, 0);
        sink += m[0] + nh;
    });
    printf("  -> picohttp parses ALL headers vs sscanf's 2 fields (%.2fx raw)\n",
           dsco_http / g_last_ns);

    printf("\n(sink=%.1f)\n", (double)sink);
    return 0;
}
