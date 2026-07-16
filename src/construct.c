#include "construct.h"
#include "tools.h"
#include "json_util.h"
#include "env_config.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ── Contextual Construct — process-lifecycle control plane ────────────────
 *
 * Implementation notes:
 *  - Policy table is a small fixed array guarded by a mutex. Policies are
 *    keyed by tool name; insert-or-update semantics via construct_protect.
 *  - The supervisor thread wakes every DSCO_CONSTRUCT_TICK_MS (default 1000ms)
 *    and runs construct_tick(): snapshot in-flight watchdogs, and for each
 *    protected tool whose remaining deadline has dropped below its low-water
 *    mark, renew via watchdog_renew_by_name — unless the estimated total
 *    lifetime would exceed the policy's max_lifetime_s cap.
 *  - Lifetime estimation: the snapshot exposes timeout_s and renew_count but
 *    not started_at, so the cap check uses the conservative estimate
 *    timeout_s + renew_count * renew_quantum_s. watchdog_renew additionally
 *    honors the per-watchdog max_lifetime_s field as a hard backstop.
 *  - Priority raises the effective low-water mark (renew earlier), so under
 *    scheduling jitter CRITICAL work is renewed with the most margin:
 *      effective_low_water = low_water_s * (2 + priority) / 2
 *  - Env gates: DSCO_CONSTRUCT=0 disables the supervisor thread entirely;
 *    DSCO_CONSTRUCT_TICK_MS tunes cadence (50..60000).
 */

#define CONSTRUCT_MAX_POLICIES 64
#define CONSTRUCT_SNAPSHOT_MAX 128

static construct_policy_t s_policies[CONSTRUCT_MAX_POLICIES];
static int s_policy_count = 0;
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;

static pthread_t s_thread;
static _Atomic int s_running = 0;
static _Atomic int s_started = 0;

static _Atomic unsigned long s_ticks = 0;
static _Atomic unsigned long s_total_renewals = 0;

/* ── Priority helpers ─────────────────────────────────────────────────── */

static const char *prio_name(construct_priority_t p) {
    switch (p) {
    case CONSTRUCT_PRIO_IDLE: return "idle";
    case CONSTRUCT_PRIO_LOW: return "low";
    case CONSTRUCT_PRIO_NORMAL: return "normal";
    case CONSTRUCT_PRIO_HIGH: return "high";
    case CONSTRUCT_PRIO_CRITICAL: return "critical";
    default: return "unknown";
    }
}

static construct_priority_t prio_parse(const char *s) {
    if (!s || !*s) return CONSTRUCT_PRIO_NORMAL;
    if (strcmp(s, "idle") == 0) return CONSTRUCT_PRIO_IDLE;
    if (strcmp(s, "low") == 0) return CONSTRUCT_PRIO_LOW;
    if (strcmp(s, "normal") == 0) return CONSTRUCT_PRIO_NORMAL;
    if (strcmp(s, "high") == 0) return CONSTRUCT_PRIO_HIGH;
    if (strcmp(s, "critical") == 0) return CONSTRUCT_PRIO_CRITICAL;
    return CONSTRUCT_PRIO_NORMAL;
}

/* ── Policy table (callers hold s_lock) ───────────────────────────────── */

static int policy_find_locked(const char *tool_name) {
    for (int i = 0; i < s_policy_count; i++) {
        if (strcmp(s_policies[i].tool_name, tool_name) == 0) return i;
    }
    return -1;
}

void construct_protect(const char *tool_name, construct_priority_t prio, int renew_quantum_s,
                       int low_water_s, int max_lifetime_s) {
    if (!tool_name || !*tool_name) return;
    if (renew_quantum_s <= 0) renew_quantum_s = 60;
    if (low_water_s <= 0) low_water_s = 10;
    if (max_lifetime_s < 0) max_lifetime_s = 0;
    if (prio < 0 || prio >= CONSTRUCT_PRIO_COUNT) prio = CONSTRUCT_PRIO_NORMAL;

    pthread_mutex_lock(&s_lock);
    int idx = policy_find_locked(tool_name);
    if (idx < 0) {
        if (s_policy_count >= CONSTRUCT_MAX_POLICIES) {
            pthread_mutex_unlock(&s_lock);
            return;
        }
        idx = s_policy_count++;
        memset(&s_policies[idx], 0, sizeof(s_policies[idx]));
        snprintf(s_policies[idx].tool_name, sizeof(s_policies[idx].tool_name), "%s", tool_name);
    }
    s_policies[idx].priority = prio;
    s_policies[idx].renew_quantum_s = renew_quantum_s;
    s_policies[idx].low_water_s = low_water_s;
    s_policies[idx].max_lifetime_s = max_lifetime_s;
    s_policies[idx].enabled = true;
    pthread_mutex_unlock(&s_lock);
}

void construct_unprotect(const char *tool_name) {
    if (!tool_name || !*tool_name) return;
    pthread_mutex_lock(&s_lock);
    int idx = policy_find_locked(tool_name);
    if (idx >= 0) {
        s_policies[idx] = s_policies[s_policy_count - 1];
        s_policy_count--;
    }
    pthread_mutex_unlock(&s_lock);
}

/* ── Supervisor tick ──────────────────────────────────────────────────── */

int construct_tick(void) {
    watchdog_info_t snap[CONSTRUCT_SNAPSHOT_MAX];
    int n = watchdog_active_snapshot(snap, CONSTRUCT_SNAPSHOT_MAX);
    if (n <= 0) {
        atomic_fetch_add_explicit(&s_ticks, 1, memory_order_relaxed);
        return 0;
    }

    /* Copy the policy table so renewals run without holding the lock. */
    construct_policy_t policies[CONSTRUCT_MAX_POLICIES];
    int npol;
    pthread_mutex_lock(&s_lock);
    npol = s_policy_count;
    memcpy(policies, s_policies, (size_t)npol * sizeof(construct_policy_t));
    pthread_mutex_unlock(&s_lock);

    /* Renew each qualifying tool name at most once per tick:
     * watchdog_renew_by_name touches every in-flight call with that name. */
    char renewed_names[CONSTRUCT_SNAPSHOT_MAX][64];
    int renewed_name_count = 0;
    int renewals = 0;

    for (int i = 0; i < n; i++) {
        if (snap[i].timed_out) continue;

        const construct_policy_t *pol = NULL;
        for (int j = 0; j < npol; j++) {
            if (policies[j].enabled && strcmp(policies[j].tool_name, snap[i].tool_name) == 0) {
                pol = &policies[j];
                break;
            }
        }
        if (!pol) continue; /* unprotected work is allowed to expire */

        /* Lifetime cap: conservative estimate of total granted runtime. */
        if (pol->max_lifetime_s > 0) {
            long est = (long)snap[i].timeout_s +
                       (long)snap[i].renew_count * (long)pol->renew_quantum_s;
            if (est >= (long)pol->max_lifetime_s) continue;
        }

        /* Priority-scaled low-water mark: higher priority renews earlier. */
        double low_water = (double)pol->low_water_s * (2.0 + (double)pol->priority) / 2.0;
        if (snap[i].remaining_s > low_water) continue;

        /* Dedup by name within this tick. */
        bool seen = false;
        for (int k = 0; k < renewed_name_count; k++) {
            if (strcmp(renewed_names[k], snap[i].tool_name) == 0) {
                seen = true;
                break;
            }
        }
        if (seen) continue;
        if (renewed_name_count < CONSTRUCT_SNAPSHOT_MAX) {
            snprintf(renewed_names[renewed_name_count], sizeof(renewed_names[0]), "%s",
                     snap[i].tool_name);
            renewed_name_count++;
        }

        renewals += watchdog_renew_by_name(snap[i].tool_name, pol->renew_quantum_s);
    }

    atomic_fetch_add_explicit(&s_ticks, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&s_total_renewals, (unsigned long)renewals, memory_order_relaxed);
    return renewals;
}

/* ── Supervisor thread ────────────────────────────────────────────────── */

static void *construct_thread(void *arg) {
    (void)arg;
    int tick_ms = dsco_env_int("DSCO_CONSTRUCT_TICK_MS", 1000, 50, 60000);
    while (atomic_load_explicit(&s_running, memory_order_acquire)) {
        construct_tick();
        /* Sleep in small slices so construct_stop() returns promptly. */
        int slept = 0;
        while (atomic_load_explicit(&s_running, memory_order_acquire) && slept < tick_ms) {
            int slice = tick_ms - slept;
            if (slice > 100) slice = 100;
            usleep((useconds_t)slice * 1000);
            slept += slice;
        }
    }
    return NULL;
}

void construct_start(void) {
    if (!dsco_env_int("DSCO_CONSTRUCT", 1, 0, 1)) return; /* env gate */
    pthread_mutex_lock(&s_lock);
    if (atomic_load_explicit(&s_started, memory_order_acquire)) {
        pthread_mutex_unlock(&s_lock);
        return;
    }
    atomic_store_explicit(&s_running, 1, memory_order_release);
    if (pthread_create(&s_thread, NULL, construct_thread, NULL) == 0) {
        atomic_store_explicit(&s_started, 1, memory_order_release);
    } else {
        atomic_store_explicit(&s_running, 0, memory_order_release);
    }
    pthread_mutex_unlock(&s_lock);
}

void construct_stop(void) {
    pthread_t thread;
    bool join = false;
    pthread_mutex_lock(&s_lock);
    if (atomic_load_explicit(&s_started, memory_order_acquire)) {
        atomic_store_explicit(&s_running, 0, memory_order_release);
        thread = s_thread;
        join = true;
    }
    pthread_mutex_unlock(&s_lock);
    if (!join) return;
    pthread_join(thread, NULL);
    atomic_store_explicit(&s_started, 0, memory_order_release);
}

/* ── Agent-facing `construct` tool ────────────────────────────────────── */

static const char *s_construct_schema =
    "{\"type\":\"object\",\"properties\":{"
    "\"action\":{\"type\":\"string\",\"enum\":[\"status\",\"protect\",\"unprotect\",\"renew\","
    "\"tick\"],\"description\":\"Supervisor operation\"},"
    "\"tool\":{\"type\":\"string\",\"description\":\"Target tool name (protect/unprotect/renew)\"},"
    "\"priority\":{\"type\":\"string\",\"enum\":[\"idle\",\"low\",\"normal\",\"high\",\"critical\"],"
    "\"description\":\"Renewal priority (protect)\"},"
    "\"renew_quantum_s\":{\"type\":\"integer\",\"description\":\"Seconds granted per renewal "
    "(default 60)\"},"
    "\"low_water_s\":{\"type\":\"integer\",\"description\":\"Renew once remaining deadline drops "
    "below this (default 10)\"},"
    "\"max_lifetime_s\":{\"type\":\"integer\",\"description\":\"Absolute runtime cap; 0 = "
    "unlimited\"},"
    "\"extra_s\":{\"type\":\"integer\",\"description\":\"Seconds to extend (renew; default 60)\"}"
    "},\"required\":[\"action\"]}";

static char *construct_status_json(void) {
    char buf[8192];
    size_t off = 0;
    bool running = atomic_load_explicit(&s_started, memory_order_acquire) &&
                   atomic_load_explicit(&s_running, memory_order_acquire);
    unsigned long ticks = atomic_load_explicit(&s_ticks, memory_order_relaxed);
    unsigned long renewals = atomic_load_explicit(&s_total_renewals, memory_order_relaxed);

    off += (size_t)snprintf(buf + off, sizeof(buf) - off,
                            "{\"running\":%s,\"ticks\":%lu,\"total_renewals\":%lu,\"policies\":[",
                            running ? "true" : "false", ticks, renewals);

    pthread_mutex_lock(&s_lock);
    for (int i = 0; i < s_policy_count && off < sizeof(buf) - 256; i++) {
        off += (size_t)snprintf(
            buf + off, sizeof(buf) - off,
            "%s{\"tool\":\"%s\",\"priority\":\"%s\",\"renew_quantum_s\":%d,"
            "\"low_water_s\":%d,\"max_lifetime_s\":%d,\"enabled\":%s}",
            i ? "," : "", s_policies[i].tool_name, prio_name(s_policies[i].priority),
            s_policies[i].renew_quantum_s, s_policies[i].low_water_s,
            s_policies[i].max_lifetime_s, s_policies[i].enabled ? "true" : "false");
    }
    pthread_mutex_unlock(&s_lock);

    off += (size_t)snprintf(buf + off, sizeof(buf) - off, "],\"in_flight\":[");

    watchdog_info_t snap[CONSTRUCT_SNAPSHOT_MAX];
    int n = watchdog_active_snapshot(snap, CONSTRUCT_SNAPSHOT_MAX);
    for (int i = 0; i < n && off < sizeof(buf) - 256; i++) {
        off += (size_t)snprintf(buf + off, sizeof(buf) - off,
                                "%s{\"call_id\":%lu,\"tool\":\"%s\",\"remaining_s\":%.1f,"
                                "\"timeout_s\":%d,\"renew_count\":%d,\"timed_out\":%s}",
                                i ? "," : "", snap[i].call_id, snap[i].tool_name,
                                snap[i].remaining_s, snap[i].timeout_s, snap[i].renew_count,
                                snap[i].timed_out ? "true" : "false");
    }
    off += (size_t)snprintf(buf + off, sizeof(buf) - off, "]}");
    (void)off;
    return strdup(buf);
}

static char *construct_tool_cb(const char *name, const char *input_json, void *ctx) {
    (void)name;
    (void)ctx;
    char *action = json_get_str(input_json, "action");
    const char *act = action ? action : "status";
    char *result = NULL;

    if (strcmp(act, "status") == 0) {
        result = construct_status_json();
    } else if (strcmp(act, "protect") == 0) {
        char *tool = json_get_str(input_json, "tool");
        if (!tool || !*tool) {
            result = strdup("{\"error\":\"protect requires 'tool'\"}");
        } else {
            char *prio_s = json_get_str(input_json, "priority");
            construct_priority_t prio = prio_parse(prio_s);
            int quantum = json_get_int(input_json, "renew_quantum_s", 60);
            int low_water = json_get_int(input_json, "low_water_s", 10);
            int max_life = json_get_int(input_json, "max_lifetime_s", 0);
            construct_protect(tool, prio, quantum, low_water, max_life);
            char buf[512];
            snprintf(buf, sizeof(buf),
                     "{\"ok\":true,\"protected\":\"%s\",\"priority\":\"%s\","
                     "\"renew_quantum_s\":%d,\"low_water_s\":%d,\"max_lifetime_s\":%d}",
                     tool, prio_name(prio), quantum, low_water, max_life);
            result = strdup(buf);
            free(prio_s);
        }
        free(tool);
    } else if (strcmp(act, "unprotect") == 0) {
        char *tool = json_get_str(input_json, "tool");
        if (!tool || !*tool) {
            result = strdup("{\"error\":\"unprotect requires 'tool'\"}");
        } else {
            construct_unprotect(tool);
            char buf[256];
            snprintf(buf, sizeof(buf), "{\"ok\":true,\"unprotected\":\"%s\"}", tool);
            result = strdup(buf);
        }
        free(tool);
    } else if (strcmp(act, "renew") == 0) {
        char *tool = json_get_str(input_json, "tool");
        if (!tool || !*tool) {
            result = strdup("{\"error\":\"renew requires 'tool'\"}");
        } else {
            int extra = json_get_int(input_json, "extra_s", 60);
            if (extra <= 0) extra = 60;
            int renewed = watchdog_renew_by_name(tool, extra);
            atomic_fetch_add_explicit(&s_total_renewals, (unsigned long)renewed,
                                      memory_order_relaxed);
            char buf[256];
            snprintf(buf, sizeof(buf), "{\"ok\":true,\"tool\":\"%s\",\"renewed\":%d,\"extra_s\":%d}",
                     tool, renewed, extra);
            result = strdup(buf);
        }
        free(tool);
    } else if (strcmp(act, "tick") == 0) {
        int renewed = construct_tick();
        char buf[128];
        snprintf(buf, sizeof(buf), "{\"ok\":true,\"renewals\":%d}", renewed);
        result = strdup(buf);
    } else {
        char buf[256];
        snprintf(buf, sizeof(buf),
                 "{\"error\":\"unknown action '%s' (expected status|protect|unprotect|renew|tick)\"}",
                 act);
        result = strdup(buf);
    }

    free(action);
    return result;
}

void construct_register_tool(void) {
    tools_register_external(
        "construct",
        "Process-lifecycle control plane: manage renewal policies for long-running tool calls. "
        "Actions: status (policies + in-flight watchdogs), protect (add/update a renewal policy "
        "for a tool: priority, renew_quantum_s, low_water_s, max_lifetime_s), unprotect (remove "
        "policy — work expires normally), renew (immediately extend all in-flight calls of a "
        "tool by extra_s), tick (run one supervisor step manually). The supervisor thread renews "
        "protected work near its deadline up to the lifetime cap; higher priority renews earlier.",
        s_construct_schema, construct_tool_cb, NULL);
}
