#include "provider_pool.h"
#include "provider_profiles.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Circuit breaker: after this many consecutive failures a slot is tripped and
 * routing should avoid it until tripped_until passes. */
#define POOL_TRIP_THRESHOLD   3
#define POOL_TRIP_BASE_SECS   15
#define POOL_TRIP_MAX_SECS    300

static provider_pool_t g_pool;

provider_pool_t *provider_pool(void) {
    return &g_pool;
}

static const char *pool_canon(const char *name) {
    if (!name || !name[0])
        return name;
    const char *c = provider_profile_canonical_name(name);
    return (c && c[0]) ? c : name;
}

provider_slot_t *provider_pool_slot(const char *name) {
    if (!name || !name[0])
        return NULL;
    const char *canon = pool_canon(name);
    for (int i = 0; i < g_pool.count; i++) {
        if (strcmp(g_pool.slots[i].name, canon) == 0)
            return &g_pool.slots[i];
    }
    return NULL;
}

/* Register a slot (without building transport) if not already present. Returns
 * the slot, or NULL if the pool is full. */
static provider_slot_t *pool_register(const char *name, bool is_subscription) {
    provider_slot_t *existing = provider_pool_slot(name);
    if (existing) {
        if (is_subscription)
            existing->is_subscription = true;
        return existing;
    }
    if (g_pool.count >= PROVIDER_POOL_MAX)
        return NULL;
    provider_slot_t *s = &g_pool.slots[g_pool.count++];
    memset(s, 0, sizeof(*s));
    snprintf(s->name, sizeof(s->name), "%s", pool_canon(name));
    s->is_subscription = is_subscription;
    s->state = POOL_SLOT_EMPTY;
    const provider_profile_t *prof = provider_profile_find(s->name);
    if (prof && prof->default_model)
        snprintf(s->default_model, sizeof(s->default_model), "%s", prof->default_model);
    return s;
}

/* Build + warm the transport for a slot. Safe to call repeatedly. */
static void pool_warm(provider_slot_t *s) {
    if (!s)
        return;
    const char *sk = g_pool.session_key[0] ? g_pool.session_key : NULL;
    bool have_key = provider_has_usable_key(s->name, sk);
    if (!s->provider) {
        s->provider = provider_create(s->name);
        if (s->provider)
            provider_prepare(s->provider); /* warm DNS/TCP/TLS; no-op if unsupported */
    }
    if (!s->provider) {
        s->state = POOL_SLOT_NOKEY; /* uncreatable — treat as unusable */
        return;
    }
    if (s->state == POOL_SLOT_TRIPPED)
        return; /* leave tripped state to report()/healthy() to clear */
    s->state = have_key ? POOL_SLOT_UP : POOL_SLOT_NOKEY;
}

void provider_pool_init(const char *session_key) {
    if (session_key && session_key[0])
        snprintf(g_pool.session_key, sizeof(g_pool.session_key), "%s", session_key);

    /* The three flat-rate core subscriptions are always registered so they show
     * up in /providers even before first use; they are warmed when a credential
     * is available. openai-codex covers the ChatGPT subscription path. */
    struct {
        const char *name;
        bool        is_sub;
    } core[] = {
        {"sakana", true},     {"anthropic", true}, {"openai", true},
        {"openai-codex", true},
        /* Common metered fallbacks — registered lazily-warm only if keyed. */
        {"openrouter", false}, {"xai", false},      {"moonshot", false},
        {"google", false},
    };

    for (size_t i = 0; i < sizeof(core) / sizeof(core[0]); i++) {
        const char *sk = g_pool.session_key[0] ? g_pool.session_key : NULL;
        bool keyed = provider_has_usable_key(core[i].name, sk);
        /* Always register the core subscriptions; register metered providers
         * only when they actually have a key (keeps the table meaningful). */
        if (!core[i].is_sub && !keyed)
            continue;
        provider_slot_t *s = pool_register(core[i].name, core[i].is_sub);
        if (s && keyed)
            pool_warm(s); /* warm the ones we can authenticate now */
        else if (s)
            s->state = POOL_SLOT_NOKEY;
    }

    g_pool.initialized = true;
}

provider_t *provider_pool_acquire(const char *name) {
    if (!name || !name[0])
        return NULL;
    provider_slot_t *s = provider_pool_slot(name);
    if (!s)
        s = pool_register(name, false);
    if (!s)
        return NULL; /* pool full (unreachable in practice): caller keeps current
                      * provider rather than receiving a non-pool instance */
    /* Auto-reset an expired circuit breaker. */
    if (s->state == POOL_SLOT_TRIPPED && s->tripped_until && time(NULL) >= s->tripped_until) {
        s->state = POOL_SLOT_UP;
        s->tripped_until = 0;
        s->consec_failures = 0;
    }
    if (!s->provider)
        pool_warm(s);
    s->last_used = time(NULL);
    return s->provider;
}

void provider_pool_report(const char *name, bool ok, double latency_ms) {
    provider_slot_t *s = provider_pool_slot(name);
    if (!s)
        return;
    s->total_requests++;
    if (latency_ms > 0)
        s->last_latency_ms = latency_ms;
    if (ok) {
        s->consec_failures = 0;
        s->tripped_until = 0;
        if (s->provider)
            s->state = POOL_SLOT_UP;
    } else {
        s->total_failures++;
        s->consec_failures++;
        if (s->consec_failures >= POOL_TRIP_THRESHOLD) {
            long backoff = (long)POOL_TRIP_BASE_SECS * s->consec_failures;
            if (backoff > POOL_TRIP_MAX_SECS)
                backoff = POOL_TRIP_MAX_SECS;
            s->state = POOL_SLOT_TRIPPED;
            s->tripped_until = time(NULL) + backoff;
        }
    }
}

bool provider_pool_healthy(const char *name) {
    provider_slot_t *s = provider_pool_slot(name);
    if (!s || !s->provider)
        return false;
    if (s->state == POOL_SLOT_TRIPPED) {
        if (s->tripped_until && time(NULL) >= s->tripped_until)
            return true; /* breaker window elapsed; treat as recoverable */
        return false;
    }
    return s->state == POOL_SLOT_UP;
}

static const char *pool_state_str(pool_slot_state_t st) {
    switch (st) {
    case POOL_SLOT_UP:
        return "up";
    case POOL_SLOT_NOKEY:
        return "no-key";
    case POOL_SLOT_TRIPPED:
        return "tripped";
    case POOL_SLOT_EMPTY:
    default:
        return "idle";
    }
}

void provider_pool_render(char *out, size_t out_len) {
    if (!out || out_len == 0)
        return;
    size_t pos = 0;
    int n = snprintf(out + pos, out_len - pos,
                     "  %-14s %-4s %-8s %8s %7s  %s\n", "provider", "sub", "state",
                     "lat(ms)", "fails", "model");
    if (n > 0)
        pos += (size_t)n;
    for (int i = 0; i < g_pool.count && pos < out_len; i++) {
        provider_slot_t *s = &g_pool.slots[i];
        char fails[32];
        snprintf(fails, sizeof(fails), "%ld/%ld", s->total_failures, s->total_requests);
        n = snprintf(out + pos, out_len - pos, "  %-14s %-4s %-8s %8.0f %7s  %s\n",
                     s->name, s->is_subscription ? "yes" : "-", pool_state_str(s->state),
                     s->last_latency_ms, fails,
                     s->default_model[0] ? s->default_model : "-");
        if (n <= 0)
            break;
        pos += (size_t)n;
    }
    if (pos < out_len)
        out[pos] = '\0';
    else if (out_len > 0)
        out[out_len - 1] = '\0';
}

void provider_pool_shutdown(void) {
    for (int i = 0; i < g_pool.count; i++) {
        if (g_pool.slots[i].provider) {
            provider_free(g_pool.slots[i].provider);
            g_pool.slots[i].provider = NULL;
        }
    }
    g_pool.count = 0;
    g_pool.initialized = false;
    g_pool.session_key[0] = '\0';
}
