#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#if defined(__GNUC__) || defined(__clang__)
#define INST_NOINLINE __attribute__((noinline))
#if defined(__clang__)
#define INST_NOHOOK __attribute__((no_instrument_function, no_sanitize("coverage")))
#else
#define INST_NOHOOK __attribute__((no_instrument_function))
#endif
#else
#define INST_NOINLINE
#define INST_NOHOOK
#endif

#if defined(DSCO_OBJECT_INSTRUMENTATION)

#define INST_MAX_STACK_DEFAULT 256
#define INST_STACK_BUCKETS 65536u
#define INST_FUNC_BUCKETS 65536u

typedef struct {
    uintptr_t func;
    uint64_t enter_ns;
    uint64_t child_ns;
} inst_frame_t;

typedef struct inst_func_agg {
    uintptr_t pc;
    uint64_t calls;
    uint64_t total_ns;
    uint64_t self_ns;
    struct inst_func_agg *next;
} inst_func_agg_t;

typedef struct inst_stack_agg {
    uint64_t hash;
    uint16_t depth;
    uint64_t hits;
    uintptr_t *pcs;
    struct inst_stack_agg *next;
} inst_stack_agg_t;

static bool s_configured = false;
static bool s_enabled = true;
static bool s_stream = false;
static unsigned s_stack_sample_rate = 1;
static unsigned s_max_stack = INST_MAX_STACK_DEFAULT;
static char s_out_dir[1024];
static char s_exe_path[1024];
static uintptr_t s_load_addr = 0;

static pthread_mutex_t s_config_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t s_edge_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t s_func_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t s_stack_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t s_stream_mu = PTHREAD_MUTEX_INITIALIZER;

static uint64_t *s_edge_hits = NULL;
static uintptr_t *s_edge_pcs = NULL;
static uint32_t s_edge_cap = 0;
static uint32_t s_next_guard = 1;

static inst_func_agg_t *s_func_buckets[INST_FUNC_BUCKETS];
static inst_stack_agg_t *s_stack_buckets[INST_STACK_BUCKETS];
static FILE *s_stream_fp = NULL;

static _Thread_local inst_frame_t tls_frames[INST_MAX_STACK_DEFAULT];
static _Thread_local unsigned tls_depth = 0;
static _Thread_local unsigned tls_overflow = 0;
static _Thread_local uint64_t tls_edge_count = 0;
static _Thread_local int tls_in_hook = 0;

static INST_NOHOOK uint64_t inst_now_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static INST_NOHOOK uint64_t inst_thread_id(void) {
    return (uint64_t)(uintptr_t)pthread_self();
}

static INST_NOHOOK bool env_truthy(const char *v) {
    return v && v[0] && strcmp(v, "0") != 0 && strcasecmp(v, "false") != 0 &&
           strcasecmp(v, "no") != 0 && strcasecmp(v, "off") != 0;
}

static INST_NOHOOK bool mkdir_p(const char *path) {
    if (!path || !*path)
        return false;
    char tmp[1024];
    size_t n = strlen(path);
    if (n >= sizeof(tmp))
        return false;
    memcpy(tmp, path, n + 1);
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = '\0';
        if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
            return false;
        *p = '/';
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
        return false;
    return true;
}

static INST_NOHOOK void discover_exe_path(void) {
#if defined(__APPLE__)
    uint32_t n = (uint32_t)sizeof(s_exe_path);
    if (_NSGetExecutablePath(s_exe_path, &n) != 0)
        snprintf(s_exe_path, sizeof(s_exe_path), "unknown");
    const struct mach_header *hdr = _dyld_get_image_header(0);
    s_load_addr = (uintptr_t)hdr;
#elif defined(__linux__)
    ssize_t n = readlink("/proc/self/exe", s_exe_path, sizeof(s_exe_path) - 1);
    if (n > 0)
        s_exe_path[n] = '\0';
    else
        snprintf(s_exe_path, sizeof(s_exe_path), "unknown");
    s_load_addr = 0;
#else
    snprintf(s_exe_path, sizeof(s_exe_path), "unknown");
    s_load_addr = 0;
#endif
}

static INST_NOHOOK void inst_configure(void) {
    if (s_configured)
        return;
    pthread_mutex_lock(&s_config_mu);
    if (s_configured) {
        pthread_mutex_unlock(&s_config_mu);
        return;
    }

    const char *enabled = getenv("DSCO_INSTRUMENT");
    if (enabled && !env_truthy(enabled))
        s_enabled = false;

    const char *rate = getenv("DSCO_INSTRUMENT_STACK_SAMPLE_RATE");
    if (rate && *rate) {
        unsigned n = (unsigned)strtoul(rate, NULL, 10);
        if (n > 0)
            s_stack_sample_rate = n;
    }

    const char *max_stack = getenv("DSCO_INSTRUMENT_MAX_STACK");
    if (max_stack && *max_stack) {
        unsigned n = (unsigned)strtoul(max_stack, NULL, 10);
        if (n > 0 && n < INST_MAX_STACK_DEFAULT)
            s_max_stack = n;
    }

    s_stream = env_truthy(getenv("DSCO_INSTRUMENT_STREAM"));
    discover_exe_path();

    const char *dir = getenv("DSCO_INSTRUMENT_DIR");
    if (dir && *dir)
        snprintf(s_out_dir, sizeof(s_out_dir), "%s", dir);
    else
        snprintf(s_out_dir, sizeof(s_out_dir), "build/profiles/run-%d", (int)getpid());
    if (s_enabled)
        mkdir_p(s_out_dir);

    if (s_enabled && s_stream) {
        char path[1200];
        snprintf(path, sizeof(path), "%s/raw-%d.jsonl", s_out_dir, (int)getpid());
        s_stream_fp = fopen(path, "a");
    }

    s_configured = true;
    pthread_mutex_unlock(&s_config_mu);
}

static INST_NOHOOK bool ensure_edge_capacity(uint32_t want) {
    if (want <= s_edge_cap)
        return true;
    uint32_t next = s_edge_cap ? s_edge_cap : 4096;
    while (next < want)
        next *= 2;
    uint64_t *hits = (uint64_t *)realloc(s_edge_hits, (size_t)next * sizeof(uint64_t));
    if (!hits)
        return false;
    s_edge_hits = hits;
    uintptr_t *pcs = (uintptr_t *)realloc(s_edge_pcs, (size_t)next * sizeof(uintptr_t));
    if (!pcs)
        return false;
    memset(s_edge_hits + s_edge_cap, 0, (size_t)(next - s_edge_cap) * sizeof(uint64_t));
    memset(pcs + s_edge_cap, 0, (size_t)(next - s_edge_cap) * sizeof(uintptr_t));
    s_edge_pcs = pcs;
    s_edge_cap = next;
    return true;
}

static INST_NOHOOK uint64_t mix64(uint64_t x) {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdull;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ull;
    x ^= x >> 33;
    return x;
}

static INST_NOHOOK void stream_event(const char *kind, uintptr_t pc, uintptr_t aux) {
    if (!s_stream_fp)
        return;
    pthread_mutex_lock(&s_stream_mu);
    fprintf(s_stream_fp,
            "{\"pid\":%d,\"tid\":%" PRIu64 ",\"event\":\"%s\","
            "\"pc\":\"0x%" PRIxPTR "\",\"aux\":\"0x%" PRIxPTR "\"}\n",
            (int)getpid(), inst_thread_id(), kind, pc, aux);
    pthread_mutex_unlock(&s_stream_mu);
}

static INST_NOHOOK void record_function_time(uintptr_t pc, uint64_t total_ns, uint64_t self_ns) {
    uint64_t h = mix64((uint64_t)pc);
    size_t bucket = (size_t)(h % INST_FUNC_BUCKETS);
    pthread_mutex_lock(&s_func_mu);
    inst_func_agg_t *cur = s_func_buckets[bucket];
    while (cur) {
        if (cur->pc == pc) {
            cur->calls++;
            cur->total_ns += total_ns;
            cur->self_ns += self_ns;
            pthread_mutex_unlock(&s_func_mu);
            return;
        }
        cur = cur->next;
    }
    cur = (inst_func_agg_t *)calloc(1, sizeof(*cur));
    if (cur) {
        cur->pc = pc;
        cur->calls = 1;
        cur->total_ns = total_ns;
        cur->self_ns = self_ns;
        cur->next = s_func_buckets[bucket];
        s_func_buckets[bucket] = cur;
    }
    pthread_mutex_unlock(&s_func_mu);
}

static INST_NOHOOK void record_stack_sample(uintptr_t edge_pc) {
    uintptr_t local[INST_MAX_STACK_DEFAULT + 1];
    unsigned depth = tls_depth;
    if (depth > s_max_stack)
        depth = s_max_stack;
    for (unsigned i = 0; i < depth; i++)
        local[i] = tls_frames[i].func;
    if (depth == 0 || local[depth - 1] != edge_pc) {
        if (depth < s_max_stack)
            local[depth++] = edge_pc;
    }
    if (depth == 0)
        return;

    uint64_t h = 1469598103934665603ull;
    for (unsigned i = 0; i < depth; i++) {
        h ^= (uint64_t)local[i];
        h *= 1099511628211ull;
    }
    size_t bucket = (size_t)(h % INST_STACK_BUCKETS);

    pthread_mutex_lock(&s_stack_mu);
    inst_stack_agg_t *cur = s_stack_buckets[bucket];
    while (cur) {
        if (cur->hash == h && cur->depth == depth &&
            memcmp(cur->pcs, local, (size_t)depth * sizeof(uintptr_t)) == 0) {
            cur->hits++;
            pthread_mutex_unlock(&s_stack_mu);
            return;
        }
        cur = cur->next;
    }
    cur = (inst_stack_agg_t *)calloc(1, sizeof(*cur));
    if (cur) {
        cur->pcs = (uintptr_t *)malloc((size_t)depth * sizeof(uintptr_t));
        if (cur->pcs) {
            memcpy(cur->pcs, local, (size_t)depth * sizeof(uintptr_t));
            cur->hash = h;
            cur->depth = (uint16_t)depth;
            cur->hits = 1;
            cur->next = s_stack_buckets[bucket];
            s_stack_buckets[bucket] = cur;
        } else {
            free(cur);
        }
    }
    pthread_mutex_unlock(&s_stack_mu);
}

static INST_NOHOOK void record_edge(uint32_t guard_id, uintptr_t pc) {
    if (guard_id == 0)
        return;
    inst_configure();
    if (!s_enabled)
        return;
    if (guard_id < s_edge_cap) {
        __sync_fetch_and_add(&s_edge_hits[guard_id], 1);
        if (s_edge_pcs[guard_id] == 0)
            s_edge_pcs[guard_id] = pc;
    }
    tls_edge_count++;
    if (s_stack_sample_rate == 1 || (tls_edge_count % s_stack_sample_rate) == 0)
        record_stack_sample(pc);
    stream_event("edge", pc, (uintptr_t)guard_id);
}

static INST_NOHOOK void write_outputs(void) {
    inst_configure();
    if (!s_enabled || !s_out_dir[0])
        return;

    int pid = (int)getpid();
    char path[1200];

    snprintf(path, sizeof(path), "%s/manifest-%d.json", s_out_dir, pid);
    FILE *fp = fopen(path, "w");
    if (fp) {
        fprintf(fp,
                "{"
                "\"type\":\"dsco_object_profile\","
                "\"pid\":%d,"
                "\"binary\":\"%s\","
                "\"load_address\":\"0x%" PRIxPTR "\","
                "\"edge_slots\":%u,"
                "\"stack_sample_rate\":%u,"
                "\"max_stack\":%u,"
                "\"function_hooks\":true,"
                "\"coverage_hooks\":true"
                "}\n",
                pid, s_exe_path, s_load_addr, s_next_guard, s_stack_sample_rate, s_max_stack);
        fclose(fp);
    }

    snprintf(path, sizeof(path), "%s/edges-%d.tsv", s_out_dir, pid);
    fp = fopen(path, "w");
    if (fp) {
        fputs("guard_id\tpc\thits\n", fp);
        for (uint32_t i = 1; i < s_next_guard && i < s_edge_cap; i++) {
            uint64_t hits = s_edge_hits[i];
            if (hits == 0)
                continue;
            fprintf(fp, "%u\t0x%" PRIxPTR "\t%" PRIu64 "\n", i, s_edge_pcs[i], hits);
        }
        fclose(fp);
    }

    snprintf(path, sizeof(path), "%s/functions-%d.tsv", s_out_dir, pid);
    fp = fopen(path, "w");
    if (fp) {
        fputs("pc\tcalls\ttotal_ns\tself_ns\n", fp);
        for (size_t b = 0; b < INST_FUNC_BUCKETS; b++) {
            for (inst_func_agg_t *cur = s_func_buckets[b]; cur; cur = cur->next) {
                fprintf(fp, "0x%" PRIxPTR "\t%" PRIu64 "\t%" PRIu64 "\t%" PRIu64 "\n", cur->pc,
                        cur->calls, cur->total_ns, cur->self_ns);
            }
        }
        fclose(fp);
    }

    snprintf(path, sizeof(path), "%s/stacks-%d.folded", s_out_dir, pid);
    fp = fopen(path, "w");
    if (fp) {
        for (size_t b = 0; b < INST_STACK_BUCKETS; b++) {
            for (inst_stack_agg_t *cur = s_stack_buckets[b]; cur; cur = cur->next) {
                for (unsigned i = 0; i < cur->depth; i++) {
                    if (i)
                        fputc(';', fp);
                    fprintf(fp, "0x%" PRIxPTR, cur->pcs[i]);
                }
                fprintf(fp, " %" PRIu64 "\n", cur->hits);
            }
        }
        fclose(fp);
    }

    if (s_stream_fp) {
        fclose(s_stream_fp);
        s_stream_fp = NULL;
    }
}

INST_NOHOOK void __sanitizer_cov_trace_pc_guard_init(uint32_t *start, uint32_t *stop) {
    if (start == stop || *start)
        return;
    inst_configure();
    uint32_t count = (uint32_t)(stop - start);
    pthread_mutex_lock(&s_edge_mu);
    uint32_t first = s_next_guard;
    if (ensure_edge_capacity(first + count + 1)) {
        for (uint32_t i = 0; i < count; i++)
            start[i] = first + i;
        s_next_guard += count;
    }
    pthread_mutex_unlock(&s_edge_mu);
}

INST_NOHOOK void __sanitizer_cov_trace_pc_guard(uint32_t *guard) {
    if (!guard || !*guard || tls_in_hook)
        return;
    tls_in_hook = 1;
    uintptr_t pc = (uintptr_t)__builtin_return_address(0);
    record_edge(*guard, pc);
    tls_in_hook = 0;
}

INST_NOHOOK void __sanitizer_cov_trace_pc_indir(uintptr_t callee) {
    if (tls_in_hook)
        return;
    tls_in_hook = 1;
    stream_event("indirect_call", (uintptr_t)__builtin_return_address(0), callee);
    tls_in_hook = 0;
}

INST_NOHOOK void __sanitizer_cov_trace_cmp1(uint8_t a, uint8_t b) {
    (void)a;
    (void)b;
}

INST_NOHOOK void __sanitizer_cov_trace_cmp2(uint16_t a, uint16_t b) {
    (void)a;
    (void)b;
}

INST_NOHOOK void __sanitizer_cov_trace_cmp4(uint32_t a, uint32_t b) {
    (void)a;
    (void)b;
}

INST_NOHOOK void __sanitizer_cov_trace_cmp8(uint64_t a, uint64_t b) {
    (void)a;
    (void)b;
}

INST_NOHOOK void __sanitizer_cov_trace_const_cmp1(uint8_t a, uint8_t b) {
    (void)a;
    (void)b;
}

INST_NOHOOK void __sanitizer_cov_trace_const_cmp2(uint16_t a, uint16_t b) {
    (void)a;
    (void)b;
}

INST_NOHOOK void __sanitizer_cov_trace_const_cmp4(uint32_t a, uint32_t b) {
    (void)a;
    (void)b;
}

INST_NOHOOK void __sanitizer_cov_trace_const_cmp8(uint64_t a, uint64_t b) {
    (void)a;
    (void)b;
}

INST_NOHOOK void __sanitizer_cov_trace_switch(uint64_t val, uint64_t *cases) {
    (void)val;
    (void)cases;
}

INST_NOHOOK void __sanitizer_cov_trace_div4(uint32_t val) {
    (void)val;
}

INST_NOHOOK void __sanitizer_cov_trace_div8(uint64_t val) {
    (void)val;
}

INST_NOHOOK void __sanitizer_cov_trace_gep(uintptr_t idx) {
    (void)idx;
}

INST_NOHOOK void __cyg_profile_func_enter(void *func, void *caller) {
    if (tls_in_hook)
        return;
    tls_in_hook = 1;
    inst_configure();
    if (s_enabled) {
        if (tls_depth < s_max_stack && tls_depth < INST_MAX_STACK_DEFAULT) {
            tls_frames[tls_depth].func = (uintptr_t)func;
            tls_frames[tls_depth].enter_ns = inst_now_ns();
            tls_frames[tls_depth].child_ns = 0;
            tls_depth++;
        } else {
            tls_overflow++;
        }
        stream_event("func_enter", (uintptr_t)func, (uintptr_t)caller);
    }
    tls_in_hook = 0;
}

INST_NOHOOK void __cyg_profile_func_exit(void *func, void *caller) {
    (void)caller;
    if (tls_in_hook)
        return;
    tls_in_hook = 1;
    inst_configure();
    if (s_enabled) {
        if (tls_overflow > 0) {
            tls_overflow--;
        } else if (tls_depth > 0) {
            unsigned idx = tls_depth - 1;
            if (tls_frames[idx].func != (uintptr_t)func) {
                while (idx > 0 && tls_frames[idx].func != (uintptr_t)func)
                    idx--;
            }
            if (tls_frames[idx].func == (uintptr_t)func) {
                uint64_t now = inst_now_ns();
                uint64_t total =
                    now > tls_frames[idx].enter_ns ? now - tls_frames[idx].enter_ns : 0;
                uint64_t child = tls_frames[idx].child_ns;
                uint64_t self = total > child ? total - child : 0;
                if (idx > 0)
                    tls_frames[idx - 1].child_ns += total;
                tls_depth = idx;
                record_function_time((uintptr_t)func, total, self);
            }
        }
        stream_event("func_exit", (uintptr_t)func, 0);
    }
    tls_in_hook = 0;
}

__attribute__((constructor)) static INST_NOHOOK void dsco_instrumenter_ctor(void) {
    inst_configure();
    if (s_enabled)
        atexit(write_outputs);
}

#else

int dsco_instrumenter_disabled = 0;

#endif
