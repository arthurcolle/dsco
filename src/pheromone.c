#include "pheromone.h"
#include "error.h"
#include "event_loop.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/time.h>

/* §6: Event loop integration — register a periodic timer for pheromone decay */
static ev_loop_t *g_phero_ev_loop = NULL;
static pheromone_field_t *g_phero_timer_field = NULL;
static int g_phero_timer_id = -1;

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

static const char *TYPE_NAMES[] = {"PROGRESS", "ATTRACTION",  "WARNING",
                                   "SUCCESS",  "HELP_NEEDED", "CAPACITY"};

static const char *DECAY_NAMES[] = {"exponential", "linear", "step", "logarithmic", "sigmoid"};

static const char *AGG_NAMES[] = {"sum", "max", "mean", "weighted", "quorum"};

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

static double compute_decay(pheromone_decay_t fn, double initial, double lambda, double ttl,
                            double age) {
    if (age < 0)
        age = 0;
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

int pheromone_deposit(pheromone_field_t *f, pheromone_type_t type, double concentration,
                      const char *region, const char *source, const char *meta) {
    return pheromone_deposit_ex(f, type, concentration, region, source, meta,
                                PHERO_DECAY_EXPONENTIAL, f->default_lambda, 0);
}

int pheromone_deposit_ex(pheromone_field_t *f, pheromone_type_t type, double concentration,
                         const char *region, const char *source, const char *meta,
                         pheromone_decay_t decay_fn, double lambda, double ttl) {
    if (!f || !f->initialized)
        return -1;
    /* ENDOCRINE CONTRACT: a hormone with non-finite or negative concentration is
     * meaningless and would poison every downstream gradient aggregation. The
     * signal type must be a valid enum member. Fail closed (return -1). */
    DSCO_REQUIRE_RET(type >= 0 && type < PHERO_TYPE_COUNT, -1);
    DSCO_REQUIRE_RET(concentration == concentration, -1); /* reject NaN */
    DSCO_REQUIRE_RET(concentration >= 0.0, -1);
    DSCO_REQUIRE_RET(decay_fn >= 0 && decay_fn < PHERO_DECAY_COUNT, -1);
    if (f->count >= PHEROMONE_MAX_SIGNALS) {
        /* Try to reap dead signals first */
        pheromone_tick(f);
        if (f->count >= PHEROMONE_MAX_SIGNALS)
            return -1;
    }

    /* Find empty slot */
    int slot = -1;
    for (int i = 0; i < PHEROMONE_MAX_SIGNALS; i++) {
        if (!f->signals[i].active) {
            slot = i;
            break;
        }
    }
    if (slot < 0)
        return -1;

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

    if (region)
        snprintf(s->region, sizeof(s->region), "%s", region);
    else
        s->region[0] = '\0';
    if (source)
        snprintf(s->source, sizeof(s->source), "%s", source);
    else
        s->source[0] = '\0';
    if (meta)
        snprintf(s->meta, sizeof(s->meta), "%s", meta);
    else
        s->meta[0] = '\0';

    f->count++;
    f->total_deposits++;
    return s->id;
}

/* ── Sense ────────────────────────────────────────────────────────────── */

double pheromone_sense(pheromone_field_t *f, pheromone_type_t type, const char *region,
                       pheromone_aggregation_t agg) {
    pheromone_gradient_t g;
    if (pheromone_gradient(f, type, region, agg, &g))
        return g.concentration;
    return 0.0;
}

bool pheromone_gradient(pheromone_field_t *f, pheromone_type_t type, const char *region,
                        pheromone_aggregation_t agg, pheromone_gradient_t *out) {
    if (!f || !f->initialized || !out)
        return false;
    f->total_reads++;

    double t = now_sec();
    memset(out, 0, sizeof(*out));
    out->type = type;

    double sum = 0, maxv = 0, wsum = 0, wweight = 0;
    int count = 0;
    double strongest_age = 1e18;

    for (int i = 0; i < PHEROMONE_MAX_SIGNALS; i++) {
        pheromone_signal_t *s = &f->signals[i];
        if (!s->active || s->type != type)
            continue;
        if (region && region[0] && strcmp(s->region, region) != 0)
            continue;

        double age = t - s->deposit_time;
        double c = compute_decay(s->decay_fn, s->initial, s->lambda, s->ttl, age);
        if (c < PHEROMONE_CLEANUP_THRESHOLD)
            continue;

        count++;
        sum += c;
        if (c > maxv) {
            maxv = c;
            strongest_age = age;
            snprintf(out->strongest_source, sizeof(out->strongest_source), "%s", s->source);
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
        case PHERO_AGG_SUM:
            out->concentration = sum;
            break;
        case PHERO_AGG_MAX:
            out->concentration = maxv;
            break;
        case PHERO_AGG_MEAN:
            out->concentration = sum / count;
            break;
        case PHERO_AGG_WEIGHTED:
            out->concentration = wweight > 0 ? wsum / wweight : 0;
            break;
        case PHERO_AGG_QUORUM:
            out->concentration = count >= 3 ? sum : 0;
            break;
        default:
            out->concentration = sum;
            break;
    }
    return true;
}

int pheromone_sense_all(pheromone_field_t *f, const char *region, pheromone_aggregation_t agg,
                        pheromone_gradient_t *out, int max) {
    int n = 0;
    for (int t = 0; t < PHERO_TYPE_COUNT && n < max; t++) {
        pheromone_gradient_t g;
        if (pheromone_gradient(f, (pheromone_type_t)t, region, agg, &g) && g.signal_count > 0) {
            out[n++] = g;
        }
    }
    return n;
}

/* ── Maintenance ──────────────────────────────────────────────────────── */

int pheromone_tick(pheromone_field_t *f) {
    if (!f || !f->initialized)
        return 0;
    double t = now_sec();
    int reaped = 0;

    for (int i = 0; i < PHEROMONE_MAX_SIGNALS; i++) {
        pheromone_signal_t *s = &f->signals[i];
        if (!s->active)
            continue;

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
    if (!f || !f->initialized)
        return false;
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
    if (!f || !f->initialized || !region)
        return 0;
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
    if (!f || !buf)
        return 0;
    double t = now_sec();
    int n = snprintf(buf, len, "{\"signals\":[");

    bool first = true;
    for (int i = 0; i < PHEROMONE_MAX_SIGNALS && (size_t)n < len - 256; i++) {
        const pheromone_signal_t *s = &f->signals[i];
        if (!s->active)
            continue;

        double age = t - s->deposit_time;
        double c = compute_decay(s->decay_fn, s->initial, s->lambda, s->ttl, age);

        n += snprintf(buf + n, len - n,
                      "%s{\"id\":%d,\"type\":\"%s\",\"concentration\":%.4f,"
                      "\"region\":\"%s\",\"source\":\"%s\",\"age\":%.1f,"
                      "\"decay\":\"%s\",\"lambda\":%.4f}",
                      first ? "" : ",", s->id, pheromone_type_name(s->type), c, s->region,
                      s->source, age, pheromone_decay_name(s->decay_fn), s->lambda);
        first = false;
    }

    n += snprintf(buf + n, len - n, "]}");
    return n;
}

bool pheromone_from_json(pheromone_field_t *f, const char *json) {
    if (!f || !json || !f->initialized)
        return false;

    /*
     * Parse the array produced by pheromone_to_json:
     *   {"signals":[{"id":N,"type":"T","concentration":C,
     *                "region":"R","source":"S","age":A,
     *                "decay":"D","lambda":L}, ...]}
     *
     * Each surviving signal is re-deposited with its stored concentration
     * and a deposit_time adjusted so its age is preserved relative to now.
     * Uses simple substring extraction — no external JSON library.
     */

    const char *arr = strstr(json, "\"signals\":[");
    if (!arr)
        return false;
    arr = strchr(arr, '[');
    if (!arr)
        return false;
    arr++;

    int loaded = 0;
    double now = now_sec();

    static const char *s_decay_names[] = {"EXPONENTIAL", "LINEAR",  "STEP",
                                          "LOGARITHMIC", "SIGMOID", NULL};

    while (*arr && *arr != ']') {
        /* advance to next { */
        while (*arr && *arr != '{')
            arr++;
        if (*arr != '{')
            break;

        /* find matching } */
        const char *obj_end = arr + 1;
        int depth = 1;
        while (*obj_end && depth > 0) {
            if (*obj_end == '{')
                depth++;
            else if (*obj_end == '}')
                depth--;
            obj_end++;
        }

        /* --- field extraction helpers (operate within [arr, obj_end)) --- */
#define PFJ_STR(key, dst, dsz)                                                                     \
    do {                                                                                           \
        const char *_pk = strstr(arr, "\"" key "\":\"");                                           \
        if (_pk && _pk < obj_end) {                                                                \
            _pk += (int)sizeof("\"" key "\":\"") - 1;                                              \
            const char *_pe = memchr(_pk, '"', (size_t)(obj_end - _pk));                           \
            if (_pe) {                                                                             \
                size_t _n = (size_t)(_pe - _pk);                                                   \
                if (_n >= (dsz))                                                                   \
                    _n = (dsz) - 1;                                                                \
                memcpy((dst), _pk, _n);                                                            \
                ((char *)(dst))[_n] = '\0';                                                        \
            }                                                                                      \
        }                                                                                          \
    } while (0)

#define PFJ_DBL(key, dst)                                                                          \
    do {                                                                                           \
        const char *_pk = strstr(arr, "\"" key "\":");                                             \
        if (_pk && _pk < obj_end) {                                                                \
            _pk += (int)sizeof("\"" key "\":") - 1;                                                \
            while (*_pk == ' ')                                                                    \
                _pk++;                                                                             \
            (dst) = strtod(_pk, NULL);                                                             \
        }                                                                                          \
    } while (0)

        char type_str[32] = "PROGRESS";
        char region_str[PHEROMONE_MAX_REGION_LEN] = "";
        char source_str[PHEROMONE_MAX_SOURCE_LEN] = "";
        char decay_str[32] = "EXPONENTIAL";
        double concentration = 0.0, age_s = 0.0;
        double lambda = PHEROMONE_DEFAULT_LAMBDA;

        PFJ_STR("type", type_str, sizeof(type_str));
        PFJ_STR("region", region_str, sizeof(region_str));
        PFJ_STR("source", source_str, sizeof(source_str));
        PFJ_STR("decay", decay_str, sizeof(decay_str));
        PFJ_DBL("concentration", concentration);
        PFJ_DBL("age", age_s);
        PFJ_DBL("lambda", lambda);

#undef PFJ_STR
#undef PFJ_DBL

        /* resolve type enum */
        pheromone_type_t ptype = PHERO_PROGRESS;
        for (int ti = 0; ti < PHERO_TYPE_COUNT; ti++) {
            if (strcmp(TYPE_NAMES[ti], type_str) == 0) {
                ptype = (pheromone_type_t)ti;
                break;
            }
        }

        /* resolve decay enum */
        pheromone_decay_t pdecay = PHERO_DECAY_EXPONENTIAL;
        for (int di = 0; s_decay_names[di]; di++) {
            if (strcmp(s_decay_names[di], decay_str) == 0) {
                pdecay = (pheromone_decay_t)di;
                break;
            }
        }

        /* only restore signals that are still alive */
        if (concentration >= PHEROMONE_CLEANUP_THRESHOLD && region_str[0]) {
            double synthetic_deposit = now - age_s;

            for (int si = 0; si < PHEROMONE_MAX_SIGNALS; si++) {
                if (f->signals[si].active)
                    continue;
                pheromone_signal_t *s = &f->signals[si];
                memset(s, 0, sizeof(*s));
                s->id = ++f->next_id;
                s->type = ptype;
                s->concentration = concentration;
                s->initial = concentration;
                s->deposit_time = synthetic_deposit;
                s->decay_fn = pdecay;
                s->lambda = (lambda > 0.0) ? lambda : PHEROMONE_DEFAULT_LAMBDA;
                s->active = true;
                strncpy(s->region, region_str, PHEROMONE_MAX_REGION_LEN - 1);
                strncpy(s->source, source_str, PHEROMONE_MAX_SOURCE_LEN - 1);
                f->count++;
                loaded++;
                break;
            }
        }

        arr = obj_end;
    }

    return loaded > 0;
}

int pheromone_status_json(const pheromone_field_t *f, char *buf, size_t len) {
    if (!f || !buf)
        return 0;

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
        n += snprintf(buf + n, len - n, "%s\"%s\":%d", t ? "," : "", pheromone_type_name(t),
                      per_type[t]);
    }
    n += snprintf(buf + n, len - n, "}}");
    return n;
}

/* ── §6: Event Loop Timer Integration ──────────────────────────────── */

static void pheromone_timer_cb(int timer_id, void *ctx) {
    (void)timer_id;
    (void)ctx;
    if (g_phero_timer_field && g_phero_timer_field->initialized) {
        pheromone_tick(g_phero_timer_field);
    }
}

void pheromone_attach_event_loop(pheromone_field_t *f, ev_loop_t *loop, int interval_ms) {
    if (!f || !loop)
        return;
    /* Cancel existing timer if any */
    if (g_phero_timer_id >= 0 && g_phero_ev_loop) {
        ev_timer_cancel(g_phero_ev_loop, g_phero_timer_id);
    }
    g_phero_ev_loop = loop;
    g_phero_timer_field = f;
    g_phero_timer_id =
        ev_timer_repeat(loop, interval_ms > 0 ? interval_ms : 5000, pheromone_timer_cb, NULL);
}

void pheromone_detach_event_loop(void) {
    if (g_phero_timer_id >= 0 && g_phero_ev_loop) {
        ev_timer_cancel(g_phero_ev_loop, g_phero_timer_id);
        g_phero_timer_id = -1;
    }
    g_phero_ev_loop = NULL;
    g_phero_timer_field = NULL;
}
