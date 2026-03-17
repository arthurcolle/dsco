#include "pheromone.h"
#include "event_loop.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>

/* §6: Event loop integration — register a periodic timer for pheromone decay */
static ev_loop_t  *g_phero_ev_loop = NULL;
static pheromone_field_t *g_phero_timer_field = NULL;
static int         g_phero_timer_id = -1;

/* ═══════════════════════════════════════════════════════════════════════════
 * Pheromone Coordination System — Implementation
 *
 * Stigmergic coordination: agents deposit typed signals that decay over
 * time. Other agents sense concentration gradients to guide behavior.
 * No central planner — emergence from local signal interactions.
 * ═══════════════════════════════════════════════════════════════════════════ */

static double now_sec(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec / 1e6;
}

/* ── Name Tables ──────────────────────────────────────────────────────── */

static const char *TYPE_NAMES[] = {
    "PROGRESS", "ATTRACTION", "WARNING", "SUCCESS", "HELP_NEEDED", "CAPACITY"
};

static const char *DECAY_NAMES[] = {
    "exponential", "linear", "step", "logarithmic", "sigmoid"
};

static const char *AGG_NAMES[] = {
    "sum", "max", "mean", "weighted", "quorum"
};

const char *pheromone_type_name(pheromone_type_t t) {
    return (t >= 0 && t < PHERO_TYPE_COUNT) ? TYPE_NAMES[t] : "unknown";
}

const char *pheromone_decay_name(pheromone_decay_t d) {
    return (d >= 0 && d < PHERO_DECAY_COUNT) ? DECAY_NAMES[d] : "unknown";
}

const char *pheromone_agg_name(pheromone_aggregation_t a) {
    return (a >= 0 && a < PHERO_AGG_COUNT) ? AGG_NAMES[a] : "unknown";
}

/* ── Decay Functions ──────────────────────────────────────────────────── */

static double compute_decay(pheromone_decay_t fn, double initial,
                            double lambda, double ttl, double age) {
    if (age < 0) age = 0;
    switch (fn) {
    case PHERO_DECAY_EXPONENTIAL:
        return initial * exp(-lambda * age);
    case PHERO_DECAY_LINEAR: {
        double rate = lambda > 0 ? lambda : (initial / (ttl > 0 ? ttl : 300));
        double v = initial - rate * age;
        return v > 0 ? v : 0;
    }
    case PHERO_DECAY_STEP:
        return (ttl > 0 && age >= ttl) ? 0 : initial;
    case PHERO_DECAY_LOGARITHMIC:
        return initial / (1.0 + lambda * log(1.0 + age));
    case PHERO_DECAY_SIGMOID: {
        double mid = ttl > 0 ? ttl / 2.0 : 60.0;
        return initial / (1.0 + exp(lambda * (age - mid)));
    }
    default:
        return initial * exp(-lambda * age);
    }
}

/* ── Lifecycle ────────────────────────────────────────────────────────── */

void pheromone_field_init(pheromone_field_t *f) {
    memset(f, 0, sizeof(*f));
    f->default_aggregation = PHERO_AGG_SUM;
    f->default_lambda = PHEROMONE_DEFAULT_LAMBDA;
    f->initialized = true;
}

void pheromone_field_destroy(pheromone_field_t *f) {
    memset(f, 0, sizeof(*f));
}

/* ── Deposit ──────────────────────────────────────────────────────────── */

int pheromone_deposit(pheromone_field_t *f, pheromone_type_t type,
                      double concentration, const char *region,
                      const char *source, const char *meta) {
    return pheromone_deposit_ex(f, type, concentration, region, source, meta,
                                PHERO_DECAY_EXPONENTIAL, f->default_lambda, 0);
}

int pheromone_deposit_ex(pheromone_field_t *f, pheromone_type_t type,
                         double concentration, const char *region,
                         const char *source, const char *meta,
                         pheromone_decay_t decay_fn, double lambda, double ttl) {
    if (!f || !f->initialized) return -1;
    if (f->count >= PHEROMONE_MAX_SIGNALS) {
        /* Try to reap dead signals first */
        pheromone_tick(f);
        if (f->count >= PHEROMONE_MAX_SIGNALS) return -1;
    }

    /* Find empty slot */
    int slot = -1;
    for (int i = 0; i < PHEROMONE_MAX_SIGNALS; i++) {
        if (!f->signals[i].active) { slot = i; break; }
    }
    if (slot < 0) return -1;

    pheromone_signal_t *s = &f->signals[slot];
    s->id = f->next_id++;
    s->type = type;
    s->concentration = concentration;
    s->initial = concentration;
    s->deposit_time = now_sec();
    s->decay_fn = decay_fn;
    s->lambda = lambda;
    s->ttl = ttl;
    s->active = true;

    if (region) snprintf(s->region, sizeof(s->region), "%s", region);
    else s->region[0] = '\0';
    if (source) snprintf(s->source, sizeof(s->source), "%s", source);
    else s->source[0] = '\0';
    if (meta) snprintf(s->meta, sizeof(s->meta), "%s", meta);
    else s->meta[0] = '\0';

    f->count++;
    f->total_deposits++;
    return s->id;
}

/* ── Sense ────────────────────────────────────────────────────────────── */

double pheromone_sense(pheromone_field_t *f, pheromone_type_t type,
                       const char *region, pheromone_aggregation_t agg) {
    pheromone_gradient_t g;
    if (pheromone_gradient(f, type, region, agg, &g))
        return g.concentration;
    return 0.0;
}

bool pheromone_gradient(pheromone_field_t *f, pheromone_type_t type,
                        const char *region, pheromone_aggregation_t agg,
                        pheromone_gradient_t *out) {
    if (!f || !f->initialized || !out) return false;
    f->total_reads++;

    double t = now_sec();
    memset(out, 0, sizeof(*out));
    out->type = type;

    double sum = 0, maxv = 0, wsum = 0, wweight = 0;
    int count = 0;
    double strongest_age = 1e18;

    for (int i = 0; i < PHEROMONE_MAX_SIGNALS; i++) {
        pheromone_signal_t *s = &f->signals[i];
        if (!s->active || s->type != type) continue;
        if (region && region[0] && strcmp(s->region, region) != 0) continue;

        double age = t - s->deposit_time;
        double c = compute_decay(s->decay_fn, s->initial, s->lambda, s->ttl, age);
        if (c < PHEROMONE_CLEANUP_THRESHOLD) continue;

        count++;
        sum += c;
        if (c > maxv) {
            maxv = c;
            strongest_age = age;
            snprintf(out->strongest_source, sizeof(out->strongest_source),
                     "%s", s->source);
        }
        /* Recency-weighted: weight by 1/(1+age) */
        double w = 1.0 / (1.0 + age);
        wsum += c * w;
        wweight += w;
    }

    out->signal_count = count;
    out->strongest_age = (strongest_age < 1e17) ? strongest_age : 0;

    if (count == 0) {
        out->concentration = 0;
        return true;
    }

    switch (agg) {
    case PHERO_AGG_SUM:      out->concentration = sum; break;
    case PHERO_AGG_MAX:      out->concentration = maxv; break;
    case PHERO_AGG_MEAN:     out->concentration = sum / count; break;
    case PHERO_AGG_WEIGHTED: out->concentration = wweight > 0 ? wsum / wweight : 0; break;
    case PHERO_AGG_QUORUM:   out->concentration = count >= 3 ? sum : 0; break;
    default:                 out->concentration = sum; break;
    }
    return true;
}

int pheromone_sense_all(pheromone_field_t *f, const char *region,
                        pheromone_aggregation_t agg,
                        pheromone_gradient_t *out, int max) {
    int n = 0;
    for (int t = 0; t < PHERO_TYPE_COUNT && n < max; t++) {
        pheromone_gradient_t g;
        if (pheromone_gradient(f, (pheromone_type_t)t, region, agg, &g) &&
            g.signal_count > 0) {
            out[n++] = g;
        }
    }
    return n;
}

/* ── Maintenance ──────────────────────────────────────────────────────── */

int pheromone_tick(pheromone_field_t *f) {
    if (!f || !f->initialized) return 0;
    double t = now_sec();
    int reaped = 0;

    for (int i = 0; i < PHEROMONE_MAX_SIGNALS; i++) {
        pheromone_signal_t *s = &f->signals[i];
        if (!s->active) continue;

        double age = t - s->deposit_time;
        double c = compute_decay(s->decay_fn, s->initial, s->lambda, s->ttl, age);

        if (c < PHEROMONE_CLEANUP_THRESHOLD) {
            s->active = false;
            f->count--;
            f->total_reaped++;
            reaped++;
        } else {
            s->concentration = c;
        }
    }
    return reaped;
}

bool pheromone_reinforce(pheromone_field_t *f, int signal_id, double amount) {
    if (!f || !f->initialized) return false;
    for (int i = 0; i < PHEROMONE_MAX_SIGNALS; i++) {
        if (f->signals[i].active && f->signals[i].id == signal_id) {
            f->signals[i].initial += amount;
            /* Reset deposit time to extend life */
            f->signals[i].deposit_time = now_sec();
            return true;
        }
    }
    return false;
}

int pheromone_evaporate_region(pheromone_field_t *f, const char *region) {
    if (!f || !f->initialized || !region) return 0;
    int evaporated = 0;
    for (int i = 0; i < PHEROMONE_MAX_SIGNALS; i++) {
        pheromone_signal_t *s = &f->signals[i];
        if (s->active && strcmp(s->region, region) == 0) {
            s->active = false;
            f->count--;
            evaporated++;
        }
    }
    return evaporated;
}

/* ── Serialization ────────────────────────────────────────────────────── */

int pheromone_to_json(const pheromone_field_t *f, char *buf, size_t len) {
    if (!f || !buf) return 0;
    double t = now_sec();
    int n = snprintf(buf, len, "{\"signals\":[");

    bool first = true;
    for (int i = 0; i < PHEROMONE_MAX_SIGNALS && (size_t)n < len - 256; i++) {
        const pheromone_signal_t *s = &f->signals[i];
        if (!s->active) continue;

        double age = t - s->deposit_time;
        double c = compute_decay(s->decay_fn, s->initial, s->lambda, s->ttl, age);

        n += snprintf(buf + n, len - n,
            "%s{\"id\":%d,\"type\":\"%s\",\"concentration\":%.4f,"
            "\"region\":\"%s\",\"source\":\"%s\",\"age\":%.1f,"
            "\"decay\":\"%s\",\"lambda\":%.4f}",
            first ? "" : ",", s->id, pheromone_type_name(s->type), c,
            s->region, s->source, age,
            pheromone_decay_name(s->decay_fn), s->lambda);
        first = false;
    }

    n += snprintf(buf + n, len - n, "]}");
    return n;
}

bool pheromone_from_json(pheromone_field_t *f, const char *json) {
    (void)f; (void)json;
    /* Minimal stub — full JSON parsing deferred to tools.c integration */
    return false;
}

int pheromone_status_json(const pheromone_field_t *f, char *buf, size_t len) {
    if (!f || !buf) return 0;

    /* Count per type */
    int per_type[PHERO_TYPE_COUNT] = {0};
    for (int i = 0; i < PHEROMONE_MAX_SIGNALS; i++) {
        if (f->signals[i].active && f->signals[i].type < PHERO_TYPE_COUNT)
            per_type[f->signals[i].type]++;
    }

    int n = snprintf(buf, len,
        "{\"active_signals\":%d,\"total_deposits\":%d,\"total_reads\":%d,"
        "\"total_reaped\":%d,\"per_type\":{",
        f->count, f->total_deposits, f->total_reads, f->total_reaped);

    for (int t = 0; t < PHERO_TYPE_COUNT; t++) {
        n += snprintf(buf + n, len - n, "%s\"%s\":%d",
                      t ? "," : "", pheromone_type_name(t), per_type[t]);
    }
    n += snprintf(buf + n, len - n, "}}");
    return n;
}

/* ── §6: Event Loop Timer Integration ──────────────────────────────── */

static void pheromone_timer_cb(int timer_id, void *ctx) {
    (void)timer_id; (void)ctx;
    if (g_phero_timer_field && g_phero_timer_field->initialized) {
        pheromone_tick(g_phero_timer_field);
    }
}

void pheromone_attach_event_loop(pheromone_field_t *f, ev_loop_t *loop,
                                  int interval_ms) {
    if (!f || !loop) return;
    /* Cancel existing timer if any */
    if (g_phero_timer_id >= 0 && g_phero_ev_loop) {
        ev_timer_cancel(g_phero_ev_loop, g_phero_timer_id);
    }
    g_phero_ev_loop = loop;
    g_phero_timer_field = f;
    g_phero_timer_id = ev_timer_repeat(loop, interval_ms > 0 ? interval_ms : 5000,
                                        pheromone_timer_cb, NULL);
}

void pheromone_detach_event_loop(void) {
    if (g_phero_timer_id >= 0 && g_phero_ev_loop) {
        ev_timer_cancel(g_phero_ev_loop, g_phero_timer_id);
        g_phero_timer_id = -1;
    }
    g_phero_ev_loop = NULL;
    g_phero_timer_field = NULL;
}
