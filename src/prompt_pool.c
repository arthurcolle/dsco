#include "prompt_pool.h"
#include "tools.h"
#include "json_util.h"
#include "env_config.h"
#include "http_pool.h"

#include <curl/curl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

/* ── Prompt Pool — autonomous, ever-growing prompt suggestion cache ──────── */

#define POOL_MAX 65536
#define POOL_SEED_FLOOR 1000
#define POOL_PROMPT_MAX 512
#define POOL_HASH_BUCKETS 131072 /* power of two; open-addressed FNV set */

static char **s_prompts = NULL;
static int s_count = 0;
static unsigned long long *s_hashes = NULL; /* 0 = empty slot */
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;

static pthread_t s_thread;
static _Atomic int s_running = 0;
static _Atomic int s_started = 0;
static _Atomic int s_initialized = 0;

static _Atomic unsigned long s_news_added = 0;
static _Atomic unsigned long s_refreshes = 0;
static _Atomic time_t s_last_refresh = 0;

/* ── Hash set (FNV-1a, open addressing) ───────────────────────────────── */

static unsigned long long fnv1a(const char *s) {
    unsigned long long h = 1469598103934665603ULL;
    for (; *s; s++) {
        h ^= (unsigned char)*s;
        h *= 1099511628211ULL;
    }
    return h ? h : 1; /* reserve 0 for empty */
}

/* Returns true if inserted (novel), false if already present or table full.
 * Caller holds s_lock. */
static bool hashset_insert(unsigned long long h) {
    size_t mask = POOL_HASH_BUCKETS - 1;
    size_t i = (size_t)h & mask;
    for (size_t probe = 0; probe < POOL_HASH_BUCKETS; probe++, i = (i + 1) & mask) {
        if (s_hashes[i] == 0) {
            s_hashes[i] = h;
            return true;
        }
        if (s_hashes[i] == h)
            return false;
    }
    return false;
}

/* ── Persistence ──────────────────────────────────────────────────────── */

static void pool_path(char *out, size_t out_sz) {
    const char *home = getenv("HOME");
    snprintf(out, out_sz, "%s/.dsco/prompt_pool.jsonl", home ? home : ".");
}

static void jsonl_escape(const char *in, char *out, size_t out_sz) {
    size_t o = 0;
    for (; *in && o + 2 < out_sz; in++) {
        unsigned char c = (unsigned char)*in;
        if (c == '"' || c == '\\') {
            out[o++] = '\\';
            out[o++] = (char)c;
        } else if (c == '\n') {
            out[o++] = '\\';
            out[o++] = 'n';
        } else if (c == '\t') {
            out[o++] = ' ';
        } else if (c >= 0x20) {
            out[o++] = (char)c;
        }
    }
    out[o] = '\0';
}

/* Append one record to the JSONL file. Must be called without s_lock held. */
static void persist_append(const char *prompt, const char *src) {
    char path[1024];
    pool_path(path, sizeof(path));
    FILE *f = fopen(path, "a");
    if (!f)
        return;
    char esc[POOL_PROMPT_MAX * 2];
    jsonl_escape(prompt, esc, sizeof(esc));
    fprintf(f, "{\"t\":%ld,\"src\":\"%s\",\"q\":\"%s\"}\n", (long)time(NULL), src ? src : "?", esc);
    fclose(f);
}

/* Insert into memory + hash set (no disk write). Caller holds s_lock.
 * Returns true if novel. */
static bool pool_insert_mem(const char *prompt) {
    if (!prompt || !*prompt || s_count >= POOL_MAX)
        return false;
    size_t n = strlen(prompt);
    if (n < 8 || n >= POOL_PROMPT_MAX)
        return false;
    if (!hashset_insert(fnv1a(prompt)))
        return false;
    s_prompts[s_count] = strdup(prompt);
    if (!s_prompts[s_count])
        return false;
    s_count++;
    return true;
}

static bool pool_insert_persist(const char *prompt, const char *src) {
    pthread_mutex_lock(&s_lock);
    bool added = pool_insert_mem(prompt);
    pthread_mutex_unlock(&s_lock);
    if (added)
        persist_append(prompt, src);
    return added;
}

bool prompt_pool_add(const char *prompt, const char *src) {
    if (!atomic_load_explicit(&s_initialized, memory_order_acquire) || !prompt)
        return false;
    /* Trim leading/trailing whitespace into a local copy. */
    char buf[POOL_PROMPT_MAX];
    while (*prompt == ' ' || *prompt == '\n' || *prompt == '\t')
        prompt++;
    snprintf(buf, sizeof(buf), "%s", prompt);
    size_t n = strlen(buf);
    while (n > 0 && (buf[n - 1] == ' ' || buf[n - 1] == '\n' || buf[n - 1] == '\t'))
        buf[--n] = '\0';

    return pool_insert_persist(buf, src);
}

/* ── Seed corpus ──────────────────────────────────────────────────────────
 * subjects × templates ≥ 1000 combinatorial prompts, generated once when the
 * pool is below the floor. Subjects skew toward what dsco is good at:
 * systems, markets, code, infra — interesting to test against. */

static const char *k_subjects[] = {
    "the Raft consensus algorithm", "io_uring vs kqueue", "CRDTs for offline-first apps",
    "the xz backdoor", "eBPF-based observability", "QUIC vs TCP head-of-line blocking",
    "arena allocators in C", "lock-free ring buffers", "vector clocks vs hybrid logical clocks",
    "the Bitcoin halving cycle", "ETH staking economics", "prediction market arbitrage",
    "yield curve inversion", "options gamma exposure", "stablecoin depegs",
    "the SR-71's engine inlet design", "Apollo guidance computer", "RISC-V vector extensions",
    "Apple Silicon unified memory", "CUDA warp divergence", "speculative execution attacks",
    "DNS over HTTPS", "BGP route hijacking", "certificate transparency logs",
    "SQLite's WAL mode", "LSM trees vs B-trees", "Postgres MVCC bloat",
    "column stores for analytics", "mmap vs read syscalls", "NUMA-aware scheduling",
    "the LLVM optimization pipeline", "profile-guided optimization", "link-time optimization",
    "WebAssembly component model", "seccomp sandboxing", "capability-based security",
    "distributed tracing overhead", "tail latency amplification", "backpressure in stream processing",
    "exactly-once delivery semantics", "event sourcing tradeoffs", "saga pattern failure modes",
    "transformer KV-cache optimization", "mixture-of-experts routing", "speculative decoding",
    "RLHF reward hacking", "context window scaling laws", "tool-use fine-tuning",
    "chip export controls", "TSMC's process roadmap", "datacenter power constraints",
    "sovereign compute policy",
};

static const char *k_templates[] = {
    "Explain %s to a systems engineer in 5 bullet points",
    "Write a C program that demonstrates %s",
    "What are the three biggest misconceptions about %s?",
    "Benchmark plan: how would you measure the real-world impact of %s?",
    "Compare %s with its closest alternative — table format",
    "Draft a doctrine document about %s for an agentic CLI",
    "What changed in the last year regarding %s? Search and summarize",
    "Design an interview question that tests deep understanding of %s",
    "Steelman the case against %s",
    "Write a runbook for debugging issues related to %s",
    "How does %s affect tail latency in production systems?",
    "Sketch a minimal implementation of %s in under 200 lines",
    "What would a prediction market price about %s right now?",
    "Find the canonical paper on %s and summarize its key result",
    "How would you teach %s using only analogies from cooking?",
    "Threat-model %s: what are the failure modes and attack surfaces?",
    "Trace the money: who profits from %s and how?",
    "Write property-based tests that would catch bugs in %s",
    "What does %s look like at 10x scale? At 100x?",
    "Post-mortem template: an outage caused by %s",
    "Estimate the market size for tooling built around %s",
    "Refactor plan: migrating a legacy system toward %s",
    "What signals would tell you %s is about to become obsolete?",
    "Explain the second-order effects of %s on developer workflows",
    "Build a one-file demo of %s and verify it compiles",
    "Argue both sides: is %s overhyped or underrated?",
};

#define N_SUBJECTS ((int)(sizeof(k_subjects) / sizeof(k_subjects[0])))
#define N_TEMPLATES ((int)(sizeof(k_templates) / sizeof(k_templates[0])))

/* Generate the seed corpus. Locks only around memory inserts; disk appends run
 * outside s_lock. Returns number added. */
static int seed_generate(void) {
    int added = 0;
    char buf[POOL_PROMPT_MAX];
    /* Stride the traversal so early entries mix subjects and templates
     * rather than exhausting one subject at a time. */
    for (int t = 0; t < N_TEMPLATES; t++) {
        for (int s = 0; s < N_SUBJECTS; s++) {
            int si = (s + t * 7) % N_SUBJECTS;
            snprintf(buf, sizeof(buf), k_templates[t], k_subjects[si]);
            if (pool_insert_persist(buf, "seed"))
                added++;
        }
    }
    return added;
}

/* ── Current events → prompts (Hacker News Algolia, keyless) ─────────── */

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} fetch_buf_t;

static size_t fetch_write_cb(char *ptr, size_t size, size_t nmemb, void *ud) {
    fetch_buf_t *b = (fetch_buf_t *)ud;
    size_t n = size * nmemb;
    if (b->len + n + 1 > b->cap) {
        size_t ncap = b->cap ? b->cap * 2 : 65536;
        while (ncap < b->len + n + 1)
            ncap *= 2;
        char *nd = realloc(b->data, ncap);
        if (!nd)
            return 0;
        b->data = nd;
        b->cap = ncap;
    }
    memcpy(b->data + b->len, ptr, n);
    b->len += n;
    b->data[b->len] = '\0';
    return n;
}

static const char *k_news_templates[] = {
    "What's the story with \"%s\"? Search for context and give me the technical take",
    "\"%s\" — is this significant or noise? Assess with evidence",
    "Regarding \"%s\": what are the second-order effects for developers?",
    "Summarize the discussion around \"%s\" and steelman both sides",
    "\"%s\" — how does this affect the market? Any tradeable angle?",
    "Deep-dive \"%s\": timeline, key players, and what happens next",
    "If \"%s\" is true, what should an infrastructure company do about it?",
    "Fact-check the headline \"%s\" against primary sources",
};
#define N_NEWS_TEMPLATES ((int)(sizeof(k_news_templates) / sizeof(k_news_templates[0])))

/* Extract successive "title":"..." values from Algolia JSON. Returns number
 * of novel prompts added. */
static int news_ingest_titles(const char *json) {
    int added = 0;
    int ti = (int)(time(NULL) % N_NEWS_TEMPLATES); /* rotate template daily-ish */
    const char *p = json;
    while ((p = strstr(p, "\"title\":\"")) != NULL) {
        p += 9;
        char title[256];
        size_t o = 0;
        while (*p && *p != '"' && o + 1 < sizeof(title)) {
            if (*p == '\\' && p[1]) {
                p++;
                if (*p == 'n' || *p == 't')
                    title[o++] = ' ';
                else if (*p == 'u' && p[1] && p[2] && p[3] && p[4])
                    p += 4; /* skip \uXXXX */
                else
                    title[o++] = *p;
                p++;
                continue;
            }
            title[o++] = *p++;
        }
        title[o] = '\0';
        if (o < 12)
            continue; /* too short to be an interesting headline */

        char prompt[POOL_PROMPT_MAX];
        snprintf(prompt, sizeof(prompt), k_news_templates[ti % N_NEWS_TEMPLATES], title);
        ti++;

        if (pool_insert_persist(prompt, "news"))
            added++;
    }
    return added;
}

int prompt_pool_refresh_now(void) {
    if (!atomic_load_explicit(&s_initialized, memory_order_acquire))
        return -1;
    CURL *c = curl_easy_init();
    if (!c)
        return -1;
    dsco_http_pool_apply(c);

    fetch_buf_t fb = {0};
    int total = 0;
    const char *urls[] = {
        "https://hn.algolia.com/api/v1/search?tags=front_page&hitsPerPage=30",
        "https://hn.algolia.com/api/v1/search_by_date?tags=story&hitsPerPage=30",
    };
    bool any_ok = false;
    for (size_t u = 0; u < sizeof(urls) / sizeof(urls[0]); u++) {
        fb.len = 0;
        if (fb.data)
            fb.data[0] = '\0';
        curl_easy_setopt(c, CURLOPT_URL, urls[u]);
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, fetch_write_cb);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, &fb);
        curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
        curl_easy_setopt(c, CURLOPT_TIMEOUT, 20L);
        curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(c, CURLOPT_USERAGENT, "dsco-prompt-pool/1.0");
        if (curl_easy_perform(c) == CURLE_OK && fb.len > 0) {
            any_ok = true;
            total += news_ingest_titles(fb.data);
        }
    }
    free(fb.data);
    curl_easy_cleanup(c);

    if (!any_ok)
        return -1;
    atomic_fetch_add_explicit(&s_refreshes, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&s_news_added, (unsigned long)total, memory_order_relaxed);
    atomic_store_explicit(&s_last_refresh, time(NULL), memory_order_relaxed);
    return total;
}

/* ── Background refresher ─────────────────────────────────────────────── */

static void *refresher_thread(void *arg) {
    (void)arg;
    int period_s = dsco_env_int("DSCO_PROMPT_POOL_REFRESH_S", 3600, 300, 86400);
    /* First fetch shortly after boot (grace so startup isn't contended). */
    int warmup = 15;
    while (atomic_load_explicit(&s_running, memory_order_acquire) && warmup-- > 0)
        sleep(1);
    while (atomic_load_explicit(&s_running, memory_order_acquire)) {
        prompt_pool_refresh_now();
        int slept = 0;
        while (atomic_load_explicit(&s_running, memory_order_acquire) && slept < period_s) {
            sleep(1);
            slept++;
        }
    }
    return NULL;
}

/* ── Suggestion rotation ──────────────────────────────────────────────── */

unsigned prompt_pool_bucket(void) {
    int rotate_s = dsco_env_int("DSCO_PROMPT_POOL_ROTATE_S", 8, 2, 600);
    return (unsigned)(time(NULL) / rotate_s);
}

bool prompt_pool_suggestion(char *out, size_t out_sz) {
    if (!atomic_load_explicit(&s_initialized, memory_order_acquire) || !out || out_sz == 0)
        return false;
    unsigned bucket = prompt_pool_bucket();
    /* Golden-ratio scramble so consecutive buckets jump around the pool
     * instead of walking it in insertion order. */
    unsigned long long mix = (unsigned long long)bucket * 11400714819323198485ULL;
    pthread_mutex_lock(&s_lock);
    if (s_count == 0) {
        pthread_mutex_unlock(&s_lock);
        return false;
    }
    const char *p = s_prompts[(int)(mix % (unsigned long long)s_count)];
    snprintf(out, out_sz, "%s", p);
    pthread_mutex_unlock(&s_lock);
    return true;
}

int prompt_pool_count(void) {
    pthread_mutex_lock(&s_lock);
    int n = s_count;
    pthread_mutex_unlock(&s_lock);
    return n;
}

/* ── Init / shutdown ──────────────────────────────────────────────────── */

static void load_jsonl(void) {
    char path[1024];
    pool_path(path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    char line[POOL_PROMPT_MAX * 2 + 128];
    while (fgets(line, sizeof(line), f)) {
        char *q = json_get_str(line, "q");
        if (q) {
            pthread_mutex_lock(&s_lock);
            pool_insert_mem(q); /* no persist — already on disk */
            pthread_mutex_unlock(&s_lock);
            free(q);
        }
    }
    fclose(f);
}

void prompt_pool_init(void) {
    if (!dsco_env_int("DSCO_PROMPT_POOL", 1, 0, 1))
        return;
    pthread_mutex_lock(&s_lock);
    if (atomic_load_explicit(&s_initialized, memory_order_acquire) || s_prompts || s_hashes) {
        pthread_mutex_unlock(&s_lock);
        return;
    }

    s_prompts = calloc(POOL_MAX, sizeof(char *));
    s_hashes = calloc(POOL_HASH_BUCKETS, sizeof(unsigned long long));
    if (!s_prompts || !s_hashes) {
        free(s_prompts);
        free(s_hashes);
        s_prompts = NULL;
        s_hashes = NULL;
        pthread_mutex_unlock(&s_lock);
        return;
    }
    pthread_mutex_unlock(&s_lock);

    load_jsonl();
    if (s_count < POOL_SEED_FLOOR)
        seed_generate();
    atomic_store_explicit(&s_initialized, 1, memory_order_release);

    pthread_mutex_lock(&s_lock);
    if (!atomic_load_explicit(&s_started, memory_order_acquire)) {
        atomic_store_explicit(&s_running, 1, memory_order_release);
        if (pthread_create(&s_thread, NULL, refresher_thread, NULL) == 0)
            atomic_store_explicit(&s_started, 1, memory_order_release);
        else
            atomic_store_explicit(&s_running, 0, memory_order_release);
    }
    pthread_mutex_unlock(&s_lock);
}

void prompt_pool_shutdown(void) {
    pthread_t thread;
    bool join = false;
    pthread_mutex_lock(&s_lock);
    if (atomic_load_explicit(&s_started, memory_order_acquire)) {
        atomic_store_explicit(&s_running, 0, memory_order_release);
        thread = s_thread;
        join = true;
    }
    pthread_mutex_unlock(&s_lock);
    if (join) {
        pthread_join(thread, NULL);
        atomic_store_explicit(&s_started, 0, memory_order_release);
    }

    pthread_mutex_lock(&s_lock);
    if (s_prompts) {
        for (int i = 0; i < s_count; i++)
            free(s_prompts[i]);
    }
    free(s_prompts);
    free(s_hashes);
    s_prompts = NULL;
    s_hashes = NULL;
    s_count = 0;
    atomic_store_explicit(&s_initialized, 0, memory_order_release);
    pthread_mutex_unlock(&s_lock);
}

/* ── Agent-facing tool ────────────────────────────────────────────────── */

static const char *s_pp_schema =
    "{\"type\":\"object\",\"properties\":{"
    "\"action\":{\"type\":\"string\",\"enum\":[\"suggest\",\"sample\",\"add\",\"stats\","
    "\"refresh\"],\"description\":\"suggest: current ghost prompt; sample: N random prompts; "
    "add: insert a prompt; stats: pool metrics; refresh: fetch current events now\"},"
    "\"n\":{\"type\":\"integer\",\"description\":\"sample size (default 5, max 50)\"},"
    "\"prompt\":{\"type\":\"string\",\"description\":\"prompt text (add)\"}"
    "},\"required\":[\"action\"]}";

static char *pp_tool_cb(const char *name, const char *input_json, void *ctx) {
    (void)name;
    (void)ctx;
    char *action = json_get_str(input_json, "action");
    const char *act = action ? action : "stats";
    char *result = NULL;

    if (strcmp(act, "suggest") == 0) {
        char sug[POOL_PROMPT_MAX];
        if (prompt_pool_suggestion(sug, sizeof(sug))) {
            char esc[POOL_PROMPT_MAX * 2];
            jsonl_escape(sug, esc, sizeof(esc));
            char buf[POOL_PROMPT_MAX * 2 + 64];
            snprintf(buf, sizeof(buf), "{\"suggestion\":\"%s\",\"bucket\":%u}", esc,
                     prompt_pool_bucket());
            result = strdup(buf);
        } else {
            result = strdup("{\"error\":\"pool empty or disabled\"}");
        }
    } else if (strcmp(act, "sample") == 0) {
        int n = json_get_int(input_json, "n", 5);
        if (n < 1)
            n = 1;
        if (n > 50)
            n = 50;
        size_t cap = (size_t)n * (POOL_PROMPT_MAX * 2) + 64;
        char *buf = malloc(cap);
        size_t off = 0;
        off += (size_t)snprintf(buf + off, cap - off, "{\"prompts\":[");
        unsigned long long seed =
            (unsigned long long)time(NULL) ^ ((unsigned long long)getpid() << 32);
        pthread_mutex_lock(&s_lock);
        int avail = s_count;
        for (int i = 0; i < n && avail > 0 && off + POOL_PROMPT_MAX * 2 < cap; i++) {
            seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
            const char *p = s_prompts[(int)((seed >> 16) % (unsigned long long)avail)];
            char esc[POOL_PROMPT_MAX * 2];
            jsonl_escape(p, esc, sizeof(esc));
            off += (size_t)snprintf(buf + off, cap - off, "%s\"%s\"", i ? "," : "", esc);
        }
        pthread_mutex_unlock(&s_lock);
        off += (size_t)snprintf(buf + off, cap - off, "],\"pool_size\":%d}", prompt_pool_count());
        result = buf;
    } else if (strcmp(act, "add") == 0) {
        char *prompt = json_get_str(input_json, "prompt");
        if (!prompt || !*prompt) {
            result = strdup("{\"error\":\"add requires 'prompt'\"}");
        } else {
            bool novel = prompt_pool_add(prompt, "user");
            char buf[128];
            snprintf(buf, sizeof(buf), "{\"ok\":true,\"novel\":%s,\"pool_size\":%d}",
                     novel ? "true" : "false", prompt_pool_count());
            result = strdup(buf);
        }
        free(prompt);
    } else if (strcmp(act, "stats") == 0) {
        char buf[512];
        bool running = atomic_load_explicit(&s_started, memory_order_acquire) &&
                       atomic_load_explicit(&s_running, memory_order_acquire);
        unsigned long refreshes = atomic_load_explicit(&s_refreshes, memory_order_relaxed);
        unsigned long news_added = atomic_load_explicit(&s_news_added, memory_order_relaxed);
        time_t last_refresh = atomic_load_explicit(&s_last_refresh, memory_order_relaxed);
        snprintf(buf, sizeof(buf),
                 "{\"pool_size\":%d,\"refresher_running\":%s,\"refreshes\":%lu,"
                 "\"news_prompts_added\":%lu,\"last_refresh\":%ld,\"rotate_s\":%d,"
                 "\"current_bucket\":%u}",
                 prompt_pool_count(), running ? "true" : "false", refreshes,
                 news_added, (long)last_refresh,
                 dsco_env_int("DSCO_PROMPT_POOL_ROTATE_S", 8, 2, 600), prompt_pool_bucket());
        result = strdup(buf);
    } else if (strcmp(act, "refresh") == 0) {
        int added = prompt_pool_refresh_now();
        char buf[160];
        if (added < 0)
            snprintf(buf, sizeof(buf), "{\"error\":\"fetch failed (network?)\",\"pool_size\":%d}",
                     prompt_pool_count());
        else
            snprintf(buf, sizeof(buf), "{\"ok\":true,\"added\":%d,\"pool_size\":%d}", added,
                     prompt_pool_count());
        result = strdup(buf);
    } else {
        result = strdup("{\"error\":\"unknown action (suggest|sample|add|stats|refresh)\"}");
    }

    free(action);
    return result;
}

void prompt_pool_register_tool(void) {
    tools_register_external(
        "prompt_pool",
        "Ever-growing cached corpus of 1000+ test prompts (seeded combinatorially, grown daily "
        "from current-events headlines) that rotate as ghost suggestions in the input box. "
        "Actions: suggest (current ghost prompt), sample (N random prompts for testing), add "
        "(insert a custom prompt), stats (pool size, refresh metrics), refresh (fetch current "
        "events and synthesize new prompts now).",
        s_pp_schema, pp_tool_cb, NULL);
}
