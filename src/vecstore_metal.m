/* Objective-C — compiled with -fobjc-arc
 * Metal GPU cosine similarity for vecstore.
 * The .metallib is compiled once and embedded as a file next to the binary.
 */
#ifdef __APPLE__

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "vecstore_metal.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>

/* ── module state ────────────────────────────────────────────────────────── */

static id<MTLDevice>              s_device   = nil;
static id<MTLCommandQueue>        s_queue    = nil;
static id<MTLComputePipelineState> s_cosine_pso = nil;
static id<MTLComputePipelineState> s_topk_pso   = nil;
static bool s_initialized = false;
static bool s_available   = false;

static bool metal_init(void) {
    if (s_initialized) return s_available;
    s_initialized = true;

    s_device = MTLCreateSystemDefaultDevice();
    if (!s_device) return false;

    s_queue = [s_device newCommandQueue];
    if (!s_queue) return false;

    /* Look for the compiled metallib next to the binary */
    NSString *exe = [NSBundle mainBundle].executablePath;
    if (!exe) exe = @"/proc/self/exe"; /* shouldn't happen on macOS */
    NSString *dir = [exe stringByDeletingLastPathComponent];
    NSString *lib_path = [dir stringByAppendingPathComponent:@"vecstore_metal.metallib"];

    NSError *err = nil;
    id<MTLLibrary> lib = nil;

    if ([[NSFileManager defaultManager] fileExistsAtPath:lib_path]) {
        lib = [s_device newLibraryWithURL:[NSURL fileURLWithPath:lib_path] error:&err];
    }

    if (!lib) {
        /* Compile inline from source (slower first-run; cached by Metal driver) */
        NSString *src = @
            "#include <metal_stdlib>\n"
            "using namespace metal;\n"
            "kernel void vecstore_cosine(\n"
            "    device const float *query [[buffer(0)]],\n"
            "    device const float *corpus [[buffer(1)]],\n"
            "    device       float *scores [[buffer(2)]],\n"
            "    constant     uint  &dim    [[buffer(3)]],\n"
            "    constant     uint  &n_docs [[buffer(4)]],\n"
            "    uint gid [[thread_position_in_grid]]) {\n"
            "    if (gid >= n_docs) return;\n"
            "    float dot=0,q_sq=0,c_sq=0;\n"
            "    uint base=gid*dim;\n"
            "    for(uint i=0;i<dim;i++){\n"
            "        float q=query[i], c=corpus[base+i];\n"
            "        dot+=q*c; q_sq+=q*q; c_sq+=c*c;\n"
            "    }\n"
            "    scores[gid]=dot/(sqrt(q_sq)*sqrt(c_sq)+1e-8f);\n"
            "}\n";
        MTLCompileOptions *opts = [[MTLCompileOptions alloc] init];
        lib = [s_device newLibraryWithSource:src options:opts error:&err];
    }

    if (!lib) return false;

    id<MTLFunction> fn = [lib newFunctionWithName:@"vecstore_cosine"];
    if (!fn) return false;

    err = nil;
    s_cosine_pso = [s_device newComputePipelineStateWithFunction:fn error:&err];
    if (!s_cosine_pso) return false;

    s_available = true;
    return true;
}

/* ── public API ──────────────────────────────────────────────────────────── */

bool vm_metal_available(void) {
    return metal_init();
}

bool vm_query_cosine(const float *query, unsigned int dim,
                     const float *corpus, unsigned int n_docs,
                     float *out_scores) {
    if (n_docs < METAL_MIN_DOCS) return false;
    if (!metal_init()) return false;

    /* Upload buffers — shared mode avoids copy on Apple Silicon unified memory */
    size_t qsz  = dim    * sizeof(float);
    size_t csz  = (size_t)n_docs * dim * sizeof(float);
    size_t ssz  = n_docs * sizeof(float);

    id<MTLBuffer> qbuf = [s_device newBufferWithBytes:query length:qsz
                                              options:MTLResourceStorageModeShared];
    id<MTLBuffer> cbuf = [s_device newBufferWithBytes:corpus length:csz
                                              options:MTLResourceStorageModeShared];
    id<MTLBuffer> sbuf = [s_device newBufferWithLength:ssz
                                               options:MTLResourceStorageModeShared];
    if (!qbuf || !cbuf || !sbuf) return false;

    uint32_t udim = dim, und = n_docs;
    id<MTLBuffer> dbuf = [s_device newBufferWithBytes:&udim length:4
                                              options:MTLResourceStorageModeShared];
    id<MTLBuffer> nbuf = [s_device newBufferWithBytes:&und  length:4
                                              options:MTLResourceStorageModeShared];

    id<MTLCommandBuffer>      cmd    = [s_queue commandBuffer];
    id<MTLComputeCommandEncoder> enc = [cmd computeCommandEncoder];
    [enc setComputePipelineState:s_cosine_pso];
    [enc setBuffer:qbuf offset:0 atIndex:0];
    [enc setBuffer:cbuf offset:0 atIndex:1];
    [enc setBuffer:sbuf offset:0 atIndex:2];
    [enc setBuffer:dbuf offset:0 atIndex:3];
    [enc setBuffer:nbuf offset:0 atIndex:4];

    NSUInteger tpg = s_cosine_pso.maxTotalThreadsPerThreadgroup;
    if (tpg > 256) tpg = 256;
    MTLSize tg   = MTLSizeMake(tpg, 1, 1);
    MTLSize grid = MTLSizeMake(n_docs, 1, 1);
    [enc dispatchThreads:grid threadsPerThreadgroup:tg];
    [enc endEncoding];

    [cmd commit];
    [cmd waitUntilCompleted];

    memcpy(out_scores, [sbuf contents], ssz);
    return true;
}

/* CPU top-k (partial selection sort, O(n*k)) */
void vm_topk(const float *scores, unsigned int n,
             unsigned int k, unsigned int *out_indices, float *out_vals) {
    for (unsigned int j = 0; j < k && j < n; j++) {
        float best = -2.0f;
        unsigned int best_i = 0;
        for (unsigned int i = 0; i < n; i++) {
            bool already = false;
            for (unsigned int p = 0; p < j; p++)
                if (out_indices[p] == i) { already = true; break; }
            if (!already && scores[i] > best) { best = scores[i]; best_i = i; }
        }
        out_indices[j] = best_i;
        out_vals[j]    = best;
    }
}

#else  /* !__APPLE__ */

#include "vecstore_metal.h"
#include <math.h>
#include <string.h>

bool vm_metal_available(void) { return false; }

bool vm_query_cosine(const float *q, unsigned int dim,
                     const float *corpus, unsigned int n_docs,
                     float *out_scores) {
    /* CPU fallback */
    for (unsigned int d = 0; d < n_docs; d++) {
        float dot = 0, q2 = 0, c2 = 0;
        const float *cv = corpus + d * dim;
        for (unsigned int i = 0; i < dim; i++) {
            dot += q[i] * cv[i];
            q2  += q[i] * q[i];
            c2  += cv[i] * cv[i];
        }
        out_scores[d] = dot / (sqrtf(q2) * sqrtf(c2) + 1e-8f);
    }
    return true;
}

void vm_topk(const float *scores, unsigned int n,
             unsigned int k, unsigned int *out_indices, float *out_vals) {
    for (unsigned int j = 0; j < k && j < n; j++) {
        float best = -2.0f; unsigned int best_i = 0;
        for (unsigned int i = 0; i < n; i++) {
            bool skip = false;
            for (unsigned int p = 0; p < j; p++)
                if (out_indices[p] == i) { skip = true; break; }
            if (!skip && scores[i] > best) { best = scores[i]; best_i = i; }
        }
        out_indices[j] = best_i; out_vals[j] = best;
    }
}

#endif /* __APPLE__ */
