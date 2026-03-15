#include "killswitch.h"
#include <stdio.h>
#include <string.h>
#include <sys/time.h>

/* ═══════════════════════════════════════════════════════════════════════════
 * Kill Switch Hierarchy — Implementation
 *
 * Five granularities of emergency shutdown from surgical (single agent)
 * to total (system halt). The last line of defense.
 * ═══════════════════════════════════════════════════════════════════════════ */

static double now_sec(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec / 1e6;
}

/* ── Name Tables ──────────────────────────────────────────────────────── */

static const char *LEVEL_NAMES[] = {
    "agent", "workflow", "service", "pheromone", "system"
};

static const char *STATE_NAMES[] = {
    "armed", "triggered", "resolved", "expired"
};

static const char *TRIGGER_NAMES[] = {
    "manual", "budget", "timeout", "safety", "cascade", "anomaly", "heartbeat"
};

const char *killswitch_level_name(kill_level_t l) {
    return (l >= 0 && l < KILL_LEVEL_COUNT) ? LEVEL_NAMES[l] : "unknown";
}

const char *killswitch_state_name(kill_state_t s) {
    return (s >= 0 && s <= KILL_STATE_EXPIRED) ? STATE_NAMES[s] : "unknown";
}

const char *killswitch_trigger_name(kill_trigger_t t) {
    return (t >= 0 && t <= KILL_TRIGGER_HEARTBEAT) ? TRIGGER_NAMES[t] : "unknown";
}

/* ── Tier Authorization ───────────────────────────────────────────────── */

static int required_tier_for_level(kill_level_t level, kill_trigger_t trigger) {
    if (level == KILL_SYSTEM) {
        /* System-level normally requires Tier 0 (founder) */
        /* Exception: safety trigger allows Tier 1 */
        return (trigger == KILL_TRIGGER_SAFETY) ? 1 : 0;
    }
    /* All other levels: Tier 1+ */
    return 1;
}

/* ── Lifecycle ────────────────────────────────────────────────────────── */

void killswitch_init(killswitch_registry_t *r) {
    memset(r, 0, sizeof(*r));
    r->initialized = true;
}

/* ── Trigger ──────────────────────────────────────────────────────────── */

int killswitch_trigger(killswitch_registry_t *r, kill_level_t level,
                       const char *target, const char *reason,
                       kill_trigger_t trigger, int principal_tier,
                       double timeout, bool cascade) {
    if (!r || !r->initialized) return -1;
    if (r->active_count >= KILLSWITCH_MAX_ACTIVE) return -1;

    /* Authorization check */
    int req_tier = required_tier_for_level(level, trigger);
    if (principal_tier > req_tier) return -1; /* higher number = lower authority */

    killswitch_entry_t *e = &r->active[r->active_count];
    memset(e, 0, sizeof(*e));
    e->id = r->next_id++;
    e->level = level;
    e->state = KILL_STATE_TRIGGERED;
    e->trigger = trigger;
    e->required_tier = req_tier;
    e->activated_by_tier = principal_tier;
    e->triggered_at = now_sec();
    e->timeout = timeout;
    e->cascade = cascade;

    if (target) snprintf(e->target, sizeof(e->target), "%s", target);
    if (reason) snprintf(e->reason, sizeof(e->reason), "%s", reason);

    r->active_count++;
    r->total_triggers++;
    r->per_level_count[level]++;

    if (level == KILL_SYSTEM) {
        r->system_halted = true;
    }

    return e->id;
}

/* ── Resolve ──────────────────────────────────────────────────────────── */

bool killswitch_resolve(killswitch_registry_t *r, int kill_id,
                        int principal_tier) {
    if (!r || !r->initialized) return false;

    for (int i = 0; i < r->active_count; i++) {
        killswitch_entry_t *e = &r->active[i];
        if (e->id == kill_id && e->state == KILL_STATE_TRIGGERED) {
            /* Must have equal or higher authority than the trigger level */
            if (principal_tier > e->required_tier) return false;

            e->state = KILL_STATE_RESOLVED;
            e->resolved_at = now_sec();

            /* Archive to history */
            if (r->history_count < KILLSWITCH_MAX_HISTORY) {
                r->history[r->history_count++] = *e;
            }

            /* Remove from active list */
            for (int j = i; j < r->active_count - 1; j++)
                r->active[j] = r->active[j + 1];
            r->active_count--;

            /* Check if system halt can be lifted */
            if (e->level == KILL_SYSTEM) {
                bool still_halted = false;
                for (int j = 0; j < r->active_count; j++) {
                    if (r->active[j].level == KILL_SYSTEM &&
                        r->active[j].state == KILL_STATE_TRIGGERED) {
                        still_halted = true;
                        break;
                    }
                }
                r->system_halted = still_halted;
            }
            return true;
        }
    }
    return false;
}

/* ── Query ────────────────────────────────────────────────────────────── */

bool killswitch_is_killed(const killswitch_registry_t *r, const char *target) {
    if (!r || !r->initialized || !target) return false;
    if (r->system_halted) return true;

    for (int i = 0; i < r->active_count; i++) {
        const killswitch_entry_t *e = &r->active[i];
        if (e->state == KILL_STATE_TRIGGERED &&
            strcmp(e->target, target) == 0) {
            return true;
        }
    }
    return false;
}

bool killswitch_system_halted(const killswitch_registry_t *r) {
    return r && r->system_halted;
}

bool killswitch_level_active(const killswitch_registry_t *r,
                             kill_level_t level, const char *target) {
    if (!r || !r->initialized) return false;
    for (int i = 0; i < r->active_count; i++) {
        const killswitch_entry_t *e = &r->active[i];
        if (e->state == KILL_STATE_TRIGGERED && e->level == level) {
            if (!target || strcmp(e->target, target) == 0)
                return true;
        }
    }
    return false;
}

int killswitch_get_active(const killswitch_registry_t *r, const char *target,
                          killswitch_entry_t *out, int max) {
    if (!r || !out || max <= 0) return 0;
    int count = 0;
    for (int i = 0; i < r->active_count && count < max; i++) {
        const killswitch_entry_t *e = &r->active[i];
        if (e->state == KILL_STATE_TRIGGERED) {
            if (!target || strcmp(e->target, target) == 0) {
                out[count++] = *e;
            }
        }
    }
    return count;
}

int killswitch_list_active(const killswitch_registry_t *r,
                           killswitch_entry_t *out, int max) {
    return killswitch_get_active(r, NULL, out, max);
}

/* ── Maintenance ──────────────────────────────────────────────────────── */

int killswitch_tick(killswitch_registry_t *r) {
    if (!r || !r->initialized) return 0;
    double t = now_sec();
    int changes = 0;

    for (int i = r->active_count - 1; i >= 0; i--) {
        killswitch_entry_t *e = &r->active[i];
        if (e->state != KILL_STATE_TRIGGERED) continue;

        /* Check timeout */
        if (e->timeout > 0 && (t - e->triggered_at) >= e->timeout) {
            e->state = KILL_STATE_EXPIRED;
            e->resolved_at = t;

            /* Archive */
            if (r->history_count < KILLSWITCH_MAX_HISTORY)
                r->history[r->history_count++] = *e;

            /* Remove */
            for (int j = i; j < r->active_count - 1; j++)
                r->active[j] = r->active[j + 1];
            r->active_count--;
            changes++;
        }
    }

    /* Refresh system_halted flag */
    if (changes) {
        r->system_halted = false;
        for (int i = 0; i < r->active_count; i++) {
            if (r->active[i].level == KILL_SYSTEM &&
                r->active[i].state == KILL_STATE_TRIGGERED) {
                r->system_halted = true;
                break;
            }
        }
    }

    return changes;
}

/* ── Serialization ────────────────────────────────────────────────────── */

int killswitch_to_json(const killswitch_registry_t *r, char *buf, size_t len) {
    if (!r || !buf) return 0;
    double t = now_sec();
    int n = snprintf(buf, len, "{\"active\":[");

    for (int i = 0; i < r->active_count && (size_t)n < len - 256; i++) {
        const killswitch_entry_t *e = &r->active[i];
        n += snprintf(buf + n, len - n,
            "%s{\"id\":%d,\"level\":\"%s\",\"state\":\"%s\","
            "\"trigger\":\"%s\",\"target\":\"%s\","
            "\"reason\":\"%s\",\"age\":%.1f,\"timeout\":%.1f}",
            i ? "," : "", e->id,
            killswitch_level_name(e->level),
            killswitch_state_name(e->state),
            killswitch_trigger_name(e->trigger),
            e->target, e->reason,
            t - e->triggered_at, e->timeout);
    }

    n += snprintf(buf + n, len - n, "]}");
    return n;
}

int killswitch_status_json(const killswitch_registry_t *r, char *buf, size_t len) {
    if (!r || !buf) return 0;
    int n = snprintf(buf, len,
        "{\"system_halted\":%s,\"active_count\":%d,"
        "\"total_triggers\":%d,\"total_cascades\":%d,"
        "\"per_level\":{",
        r->system_halted ? "true" : "false",
        r->active_count, r->total_triggers, r->total_cascades);

    for (int l = 0; l < KILL_LEVEL_COUNT; l++) {
        n += snprintf(buf + n, len - n, "%s\"%s\":%d",
                      l ? "," : "", killswitch_level_name(l),
                      r->per_level_count[l]);
    }
    n += snprintf(buf + n, len - n, "}}");
    return n;
}
