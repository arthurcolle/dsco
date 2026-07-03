#include "spec_exec.h"
#include "tools.h"
#include "config.h"
#include "json_util.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define SPEC_MAX 32

typedef struct {
    char *tool_id;    /* key (owned) */
    char *tool_name;  /* owned */
    char *tool_input; /* owned */
    char *tier;       /* owned snapshot of the trust tier */
    char *result;     /* MAX_TOOL_RESULT heap buffer (owned) */
    bool ok;
    bool consumed; /* taken by the agent → skip in reset's join set */
    bool joined;   /* thread already joined */
    pthread_t thread;
    bool in_use; /* slot occupied */
} spec_entry_t;

static spec_entry_t g_spec[SPEC_MAX];
static pthread_mutex_t g_spec_mu = PTHREAD_MUTEX_INITIALIZER;
static char g_spec_tier[64] = "";

bool spec_exec_enabled(void) {
    return getenv("DSCO_NO_SPECULATE") == NULL;
}

/* ── Staleness-aware file-tool result cache ─────────────────────────────────
 * Memoizes read-only file-tool results keyed by (tool, full input), tagged with
 * the source file's fingerprint (mtime + size). A lookup is a hit ONLY if the
 * file's current fingerprint still matches — so an unchanged file is served
 * instantly and forever, while an edit invalidates immediately (no stale reads,
 * unlike the time-based tool_cache). Feeds both speculation and normal
 * execution. On by default; DSCO_NO_SPEC_CACHE disables. */
#define SPEC_CACHE_SIZE 64

typedef struct {
    char *tool;     /* owned */
    char *input;    /* owned — full input (offset/limit vary the result) */
    long mtime_sec; /* file fingerprint at store time (mtime + size) */
    long size;
    char *result; /* owned */
    bool ok;
    bool in_use;
    unsigned long last_used; /* LRU tick */
} spec_cache_entry_t;

static spec_cache_entry_t g_scache[SPEC_CACHE_SIZE];
static pthread_mutex_t g_scache_mu = PTHREAD_MUTEX_INITIALIZER;
static unsigned long g_scache_tick = 0;

static bool spec_cache_enabled(void) {
    return getenv("DSCO_NO_SPEC_CACHE") == NULL;
}

/* The file a tool result depends on, for staleness tracking. NULL (caller
 * frees) if the input names no file — such tools are simply not cached here. */
static char *spec_cache_path_of(const char *input) {
    if (!input)
        return NULL;
    char *p = json_get_str(input, "path");
    if (!p)
        p = json_get_str(input, "file_path");
    if (!p)
        p = json_get_str(input, "file");
    return p;
}

/* mtime (seconds) + size — any real content change alters one or both. Only
 * regular files: a directory's mtime doesn't track file-content edits, so
 * caching a listing off it could go stale (read_file/view_file are the target). */
static bool file_fingerprint(const char *path, long *sec, long *size) {
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
        return false;
    *sec = (long)st.st_mtime;
    *size = (long)st.st_size;
    return true;
}

bool spec_cache_lookup(const char *tool, const char *input, char *out, size_t outlen, bool *ok) {
    if (!spec_cache_enabled() || !tool || !input || !out || outlen == 0)
        return false;
    char *path = spec_cache_path_of(input);
    if (!path)
        return false; /* not a file tool */
    long sec = 0, size = 0;
    bool have_fp = file_fingerprint(path, &sec, &size);
    free(path);
    bool hit = false;
    pthread_mutex_lock(&g_scache_mu);
    for (int i = 0; i < SPEC_CACHE_SIZE; i++) {
        spec_cache_entry_t *e = &g_scache[i];
        if (!e->in_use || strcmp(e->tool, tool) != 0 || strcmp(e->input, input) != 0)
            continue;
        if (have_fp && e->mtime_sec == sec && e->size == size) {
            snprintf(out, outlen, "%s", e->result ? e->result : "");
            if (ok)
                *ok = e->ok;
            e->last_used = ++g_scache_tick;
            hit = true;
        } else {
            /* Stale (file changed or vanished) — drop the entry. */
            free(e->tool);
            free(e->input);
            free(e->result);
            memset(e, 0, sizeof(*e));
        }
        break;
    }
    pthread_mutex_unlock(&g_scache_mu);
    return hit;
}

void spec_cache_store(const char *tool, const char *input, const char *result, bool ok) {
    if (!spec_cache_enabled() || !tool || !input)
        return;
    char *path = spec_cache_path_of(input);
    if (!path)
        return; /* only cache file-dependent tools */
    long sec = 0, size = 0;
    bool have_fp = file_fingerprint(path, &sec, &size);
    free(path);
    if (!have_fp)
        return; /* can't fingerprint → don't cache (avoids serving unverifiable data) */

    pthread_mutex_lock(&g_scache_mu);
    int slot = -1, lru = 0;
    unsigned long oldest = ~0UL;
    for (int i = 0; i < SPEC_CACHE_SIZE; i++) {
        spec_cache_entry_t *e = &g_scache[i];
        if (e->in_use && strcmp(e->tool, tool) == 0 && strcmp(e->input, input) == 0) {
            slot = i; /* refresh existing */
            break;
        }
        if (!e->in_use && slot < 0)
            slot = i;
        if (e->in_use && e->last_used < oldest) {
            oldest = e->last_used;
            lru = i;
        }
    }
    if (slot < 0)
        slot = lru; /* evict LRU */
    spec_cache_entry_t *e = &g_scache[slot];
    free(e->tool);
    free(e->input);
    free(e->result);
    memset(e, 0, sizeof(*e));
    e->tool = strdup(tool);
    e->input = strdup(input);
    e->result = strdup(result ? result : "");
    if (!e->tool || !e->input || !e->result) {
        free(e->tool);
        free(e->input);
        free(e->result);
        memset(e, 0, sizeof(*e));
        pthread_mutex_unlock(&g_scache_mu);
        return;
    }
    e->mtime_sec = sec;
    e->size = size;
    e->ok = ok;
    e->in_use = true;
    e->last_used = ++g_scache_tick;
    pthread_mutex_unlock(&g_scache_mu);
}

void spec_exec_set_tier(const char *tier) {
    pthread_mutex_lock(&g_spec_mu);
    snprintf(g_spec_tier, sizeof(g_spec_tier), "%s", tier ? tier : "");
    pthread_mutex_unlock(&g_spec_mu);
}

/* Read-only + concurrency-safe + not interactive → no side effects when run
 * early. Caller also checks tier permission. */
static bool tool_speculatable(const char *name) {
    int total = 0;
    const tool_def_t *all = tools_get_all(&total);
    for (int i = 0; i < total; i++) {
        if (all[i].name && strcmp(all[i].name, name) == 0)
            return all[i].is_read_only && all[i].is_concurrent && !all[i].is_interactive;
    }
    return false;
}

static void *spec_worker(void *arg) {
    spec_entry_t *e = (spec_entry_t *)arg;
    e->result[0] = '\0';
    /* Instant if the file is unchanged since a prior read; else execute and
     * memoize under the current fingerprint. */
    if (spec_cache_lookup(e->tool_name, e->tool_input, e->result, MAX_TOOL_RESULT, &e->ok))
        return NULL;
    e->ok =
        tools_execute_for_tier(e->tool_name, e->tool_input, e->tier, e->result, MAX_TOOL_RESULT);
    spec_cache_store(e->tool_name, e->tool_input, e->result, e->ok);
    return NULL;
}

void spec_exec_hook(const char *tool_name, const char *tool_id, const char *tool_input) {
    if (!spec_exec_enabled() || !tool_name || !tool_id || !tool_id[0])
        return;
    if (!tool_speculatable(tool_name))
        return;

    pthread_mutex_lock(&g_spec_mu);
    /* Must be permitted at the current tier — never speculate something that
     * would be blocked at real dispatch. */
    char why[128];
    if (!tools_is_allowed_for_tier(tool_name, g_spec_tier[0] ? g_spec_tier : NULL, why,
                                   sizeof(why))) {
        pthread_mutex_unlock(&g_spec_mu);
        return;
    }
    /* Dedup + find a free slot. */
    int slot = -1;
    for (int i = 0; i < SPEC_MAX; i++) {
        if (g_spec[i].in_use && g_spec[i].tool_id && strcmp(g_spec[i].tool_id, tool_id) == 0) {
            pthread_mutex_unlock(&g_spec_mu);
            return; /* already speculating this id */
        }
        if (!g_spec[i].in_use && slot < 0)
            slot = i;
    }
    if (slot < 0) {
        pthread_mutex_unlock(&g_spec_mu);
        return; /* registry full — tool runs normally later */
    }
    spec_entry_t *e = &g_spec[slot];
    memset(e, 0, sizeof(*e));
    e->result = malloc(MAX_TOOL_RESULT);
    if (!e->result) {
        pthread_mutex_unlock(&g_spec_mu);
        return;
    }
    e->tool_id = strdup(tool_id);
    e->tool_name = strdup(tool_name);
    e->tool_input = strdup(tool_input ? tool_input : "{}");
    e->tier = strdup(g_spec_tier);
    e->in_use = true;
    if (!e->tool_id || !e->tool_name || !e->tool_input || !e->tier) {
        free(e->result);
        free(e->tool_id);
        free(e->tool_name);
        free(e->tool_input);
        free(e->tier);
        memset(e, 0, sizeof(*e));
        pthread_mutex_unlock(&g_spec_mu);
        return;
    }
    if (pthread_create(&e->thread, NULL, spec_worker, e) != 0) {
        /* Couldn't spawn — release the slot; the tool executes normally later. */
        free(e->result);
        free(e->tool_id);
        free(e->tool_name);
        free(e->tool_input);
        free(e->tier);
        memset(e, 0, sizeof(*e));
    }
    pthread_mutex_unlock(&g_spec_mu);
}

static void spec_entry_free(spec_entry_t *e) {
    free(e->result);
    free(e->tool_id);
    free(e->tool_name);
    free(e->tool_input);
    free(e->tier);
    memset(e, 0, sizeof(*e));
}

bool spec_exec_take(const char *tool_id, char *out, size_t outlen, bool *ok) {
    if (!tool_id || !out || outlen == 0)
        return false;
    pthread_mutex_lock(&g_spec_mu);
    spec_entry_t *e = NULL;
    for (int i = 0; i < SPEC_MAX; i++) {
        if (g_spec[i].in_use && !g_spec[i].consumed && g_spec[i].tool_id &&
            strcmp(g_spec[i].tool_id, tool_id) == 0) {
            e = &g_spec[i];
            break;
        }
    }
    if (!e) {
        pthread_mutex_unlock(&g_spec_mu);
        return false;
    }
    e->consumed = true;
    pthread_t th = e->thread;
    bool need_join = !e->joined;
    e->joined = true;
    pthread_mutex_unlock(&g_spec_mu);

    /* Join outside the lock (the worker may still be running). After join the
     * result buffer is safe to read without the lock — the slot is marked
     * consumed so reset() won't touch it. */
    if (need_join)
        pthread_join(th, NULL);
    snprintf(out, outlen, "%s", e->result ? e->result : "");
    if (ok)
        *ok = e->ok;

    pthread_mutex_lock(&g_spec_mu);
    spec_entry_free(e);
    pthread_mutex_unlock(&g_spec_mu);
    return true;
}

void spec_exec_reset(void) {
    /* Claim every un-consumed speculation under the lock (consumed=true keeps
     * take() off them), join their workers OUTSIDE the lock so no buffer is
     * freed while a worker still writes it, then free. */
    pthread_t to_join[SPEC_MAX];
    int idxs[SPEC_MAX];
    int n = 0;
    pthread_mutex_lock(&g_spec_mu);
    for (int i = 0; i < SPEC_MAX; i++) {
        spec_entry_t *e = &g_spec[i];
        if (!e->in_use || e->consumed)
            continue;
        e->consumed = true;
        if (!e->joined) {
            e->joined = true;
            to_join[n] = e->thread;
            idxs[n] = i;
            n++;
        }
    }
    pthread_mutex_unlock(&g_spec_mu);
    for (int i = 0; i < n; i++)
        pthread_join(to_join[i], NULL);
    pthread_mutex_lock(&g_spec_mu);
    for (int i = 0; i < n; i++)
        spec_entry_free(&g_spec[idxs[i]]);
    pthread_mutex_unlock(&g_spec_mu);
}

int spec_exec_active(void) {
    int n = 0;
    pthread_mutex_lock(&g_spec_mu);
    for (int i = 0; i < SPEC_MAX; i++)
        if (g_spec[i].in_use && !g_spec[i].consumed)
            n++;
    pthread_mutex_unlock(&g_spec_mu);
    return n;
}
