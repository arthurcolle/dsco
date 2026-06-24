#include "avian.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>

static double avian_now_sec(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec / 1e6;
}

static double clamp01(double x) {
    if (x < 0.0) return 0.0;
    if (x > 1.0) return 1.0;
    return x;
}

static void copy_str(char *dst, size_t n, const char *src) {
    if (!dst || n == 0) return;
    snprintf(dst, n, "%s", src ? src : "");
}

void avian_init(avian_engine_t *a) {
    if (!a) return;
    memset(a, 0, sizeof(*a));
    a->next_nest_id = 1;
    a->next_egg_id = 1;
    a->initialized = true;
}

void avian_destroy(avian_engine_t *a) {
    if (!a) return;
    memset(a, 0, sizeof(*a));
}

const char *avian_nest_state_name(avian_nest_state_t s) {
    switch (s) {
    case AVIAN_NEST_BUILDING: return "building";
    case AVIAN_NEST_WARM: return "warm";
    case AVIAN_NEST_READY: return "ready";
    case AVIAN_NEST_ROOSTING: return "roosting";
    case AVIAN_NEST_MOLTING: return "molting";
    case AVIAN_NEST_ABANDONED: return "abandoned";
    default: return "unknown";
    }
}

const char *avian_egg_state_name(avian_egg_state_t s) {
    switch (s) {
    case AVIAN_EGG_LAID: return "laid";
    case AVIAN_EGG_INCUBATING: return "incubating";
    case AVIAN_EGG_READY: return "ready";
    case AVIAN_EGG_FLEDGED: return "fledged";
    case AVIAN_EGG_FAILED: return "failed";
    case AVIAN_EGG_ABANDONED: return "abandoned";
    default: return "unknown";
    }
}

static avian_nest_t *find_nest(avian_engine_t *a, int id) {
    if (!a) return NULL;
    for (int i = 0; i < AVIAN_MAX_NESTS; i++)
        if (a->nests[i].active && a->nests[i].id == id) return &a->nests[i];
    return NULL;
}

static avian_egg_t *find_egg(avian_engine_t *a, int id) {
    if (!a) return NULL;
    for (int i = 0; i < AVIAN_MAX_EGGS; i++)
        if (a->eggs[i].active && a->eggs[i].id == id) return &a->eggs[i];
    return NULL;
}

const avian_nest_t *avian_nest_get(const avian_engine_t *a, int nest_id) {
    return (const avian_nest_t *)find_nest((avian_engine_t *)a, nest_id);
}

const avian_egg_t *avian_egg_get(const avian_engine_t *a, int egg_id) {
    return (const avian_egg_t *)find_egg((avian_engine_t *)a, egg_id);
}

static void refresh_nest_state(avian_nest_t *n) {
    if (!n || !n->active) return;
    if (n->state == AVIAN_NEST_ROOSTING || n->state == AVIAN_NEST_MOLTING ||
        n->state == AVIAN_NEST_ABANDONED)
        return;
    if (n->stability >= 0.70 && n->warmth >= 0.45 && n->material_count >= 2)
        n->state = AVIAN_NEST_READY;
    else if (n->warmth >= 0.35 || n->material_count > 0)
        n->state = AVIAN_NEST_WARM;
    else
        n->state = AVIAN_NEST_BUILDING;
}

int avian_nest_create(avian_engine_t *a, const char *name, const char *purpose,
                      double warmth, double stability) {
    if (!a || !a->initialized) return -1;
    int slot = -1;
    for (int i = 0; i < AVIAN_MAX_NESTS; i++) {
        if (!a->nests[i].active) { slot = i; break; }
    }
    if (slot < 0) return -1;

    avian_nest_t *n = &a->nests[slot];
    memset(n, 0, sizeof(*n));
    n->id = a->next_nest_id++;
    copy_str(n->name, sizeof(n->name), name && *name ? name : "unnamed-nest");
    copy_str(n->purpose, sizeof(n->purpose), purpose && *purpose ? purpose : "bounded workspace");
    n->warmth = clamp01(warmth <= 0.0 ? 0.25 : warmth);
    n->stability = clamp01(stability <= 0.0 ? 0.25 : stability);
    n->created_at = avian_now_sec();
    n->last_tended_at = n->created_at;
    n->state = AVIAN_NEST_BUILDING;
    n->active = true;
    a->nest_count++;
    refresh_nest_state(n);
    return n->id;
}

bool avian_nest_add_material(avian_engine_t *a, int nest_id, const char *material,
                             double quality, bool lining) {
    avian_nest_t *n = find_nest(a, nest_id);
    if (!n || n->state == AVIAN_NEST_ABANDONED) return false;
    if (n->material_count >= AVIAN_MATERIALS_MAX) return false;
    copy_str(n->materials[n->material_count], sizeof(n->materials[n->material_count]),
             material && *material ? material : "context");
    n->material_count++;
    double q = clamp01(quality <= 0.0 ? 0.5 : quality);
    n->stability = clamp01(n->stability + q * (lining ? 0.12 : 0.08));
    n->warmth = clamp01(n->warmth + q * 0.04);
    n->last_tended_at = avian_now_sec();
    a->total_materials_added++;
    if (n->state == AVIAN_NEST_MOLTING) n->state = AVIAN_NEST_WARM;
    refresh_nest_state(n);
    return true;
}

bool avian_nest_roost(avian_engine_t *a, int nest_id, const char *reason, double cooldown) {
    (void)reason;
    avian_nest_t *n = find_nest(a, nest_id);
    if (!n || n->state == AVIAN_NEST_ABANDONED) return false;
    n->state = AVIAN_NEST_ROOSTING;
    n->warmth = clamp01(n->warmth - (cooldown <= 0.0 ? 0.15 : cooldown));
    n->last_tended_at = avian_now_sec();
    a->total_roosts++;
    return true;
}

bool avian_nest_molt(avian_engine_t *a, int nest_id, const char *reason) {
    (void)reason;
    avian_nest_t *n = find_nest(a, nest_id);
    if (!n || n->state == AVIAN_NEST_ABANDONED) return false;
    n->state = AVIAN_NEST_MOLTING;
    n->stability = clamp01(n->stability * 0.85);
    n->last_tended_at = avian_now_sec();
    a->total_molts++;
    return true;
}

int avian_brood_lay(avian_engine_t *a, int nest_id, const char *name, const char *kind,
                    const char *lineage, double risk, int required_cycles) {
    avian_nest_t *n = find_nest(a, nest_id);
    if (!a || !a->initialized || !n || n->state == AVIAN_NEST_ABANDONED) return -1;
    int slot = -1;
    for (int i = 0; i < AVIAN_MAX_EGGS; i++) {
        if (!a->eggs[i].active) { slot = i; break; }
    }
    if (slot < 0) return -1;

    avian_egg_t *e = &a->eggs[slot];
    memset(e, 0, sizeof(*e));
    e->id = a->next_egg_id++;
    e->nest_id = nest_id;
    copy_str(e->name, sizeof(e->name), name && *name ? name : "candidate");
    copy_str(e->kind, sizeof(e->kind), kind && *kind ? kind : "proposal");
    copy_str(e->lineage, sizeof(e->lineage), lineage && *lineage ? lineage : "local");
    e->risk = clamp01(risk < 0.0 ? 0.5 : risk);
    e->readiness = clamp01((n->warmth * 0.25) + (n->stability * 0.25) + ((1.0 - e->risk) * 0.10));
    e->required_cycles = required_cycles > 0 ? required_cycles : 3;
    e->created_at = avian_now_sec();
    e->last_tended_at = e->created_at;
    e->state = AVIAN_EGG_LAID;
    e->active = true;
    a->egg_count++;
    return e->id;
}

bool avian_brood_tend(avian_engine_t *a, int egg_id, double warmth, const char *evidence) {
    avian_egg_t *e = find_egg(a, egg_id);
    if (!e || e->state == AVIAN_EGG_FLEDGED || e->state == AVIAN_EGG_ABANDONED ||
        e->state == AVIAN_EGG_FAILED)
        return false;
    avian_nest_t *n = find_nest(a, e->nest_id);
    double w = clamp01(warmth <= 0.0 ? 0.5 : warmth);
    double evidence_bonus = (evidence && *evidence) ? 0.08 : 0.0;
    double nest_bonus = n ? (n->stability * 0.04 + n->warmth * 0.03) : 0.0;
    double risk_drag = e->risk * 0.03;
    e->readiness = clamp01(e->readiness + (0.15 * w) + evidence_bonus + nest_bonus - risk_drag);
    e->cycles++;
    e->last_tended_at = avian_now_sec();
    e->state = (e->readiness >= 0.80 && e->cycles >= e->required_cycles) ? AVIAN_EGG_READY
                                                                          : AVIAN_EGG_INCUBATING;
    if (n) {
        n->warmth = clamp01(n->warmth + 0.03 * w);
        n->last_tended_at = e->last_tended_at;
        if (n->state == AVIAN_NEST_ROOSTING || n->state == AVIAN_NEST_MOLTING)
            n->state = AVIAN_NEST_WARM;
        refresh_nest_state(n);
    }
    a->total_brood_cycles++;
    return true;
}

bool avian_brood_fledge(avian_engine_t *a, int egg_id, char *reason, size_t reason_len) {
    avian_egg_t *e = find_egg(a, egg_id);
    if (!e) {
        if (reason && reason_len) snprintf(reason, reason_len, "egg not found");
        return false;
    }
    if (e->state != AVIAN_EGG_READY) {
        if (reason && reason_len)
            snprintf(reason, reason_len, "not ready: state=%s readiness=%.2f cycles=%d/%d risk=%.2f",
                     avian_egg_state_name(e->state), e->readiness, e->cycles, e->required_cycles,
                     e->risk);
        return false;
    }
    if (e->risk > 0.65 && e->readiness < 0.92) {
        if (reason && reason_len)
            snprintf(reason, reason_len, "high-risk candidate needs readiness >=0.92 before fledging");
        return false;
    }
    e->state = AVIAN_EGG_FLEDGED;
    e->last_tended_at = avian_now_sec();
    a->total_fledged++;
    if (reason && reason_len)
        snprintf(reason, reason_len, "fledged: %s (%s) readiness=%.2f cycles=%d", e->name, e->kind,
                 e->readiness, e->cycles);
    return true;
}

bool avian_brood_abandon(avian_engine_t *a, int egg_id, const char *reason) {
    (void)reason;
    avian_egg_t *e = find_egg(a, egg_id);
    if (!e) return false;
    e->state = AVIAN_EGG_ABANDONED;
    e->last_tended_at = avian_now_sec();
    return true;
}

static int append_json_str(char *buf, size_t len, int n, const char *s) {
    if (!buf || len == 0 || n < 0 || (size_t)n >= len) return n;
    n += snprintf(buf + n, len - (size_t)n, "\"");
    for (const char *p = s ? s : ""; *p && (size_t)n < len; p++) {
        if (*p == '"' || *p == '\\') n += snprintf(buf + n, len - (size_t)n, "\\%c", *p);
        else if (*p == '\n') n += snprintf(buf + n, len - (size_t)n, "\\n");
        else n += snprintf(buf + n, len - (size_t)n, "%c", *p);
    }
    if ((size_t)n < len) n += snprintf(buf + n, len - (size_t)n, "\"");
    return n;
}

int avian_nest_json(const avian_engine_t *a, int nest_id, char *buf, size_t len) {
    const avian_nest_t *n0 = avian_nest_get(a, nest_id);
    if (!buf || len == 0) return 0;
    if (!n0) return snprintf(buf, len, "{\"ok\":false,\"error\":\"nest not found\"}");
    int n = snprintf(buf, len,
                     "{\"ok\":true,\"id\":%d,\"name\":", n0->id);
    n = append_json_str(buf, len, n, n0->name);
    n += snprintf(buf + n, len - (size_t)n,
                  ",\"purpose\":");
    n = append_json_str(buf, len, n, n0->purpose);
    n += snprintf(buf + n, len - (size_t)n,
                  ",\"state\":\"%s\",\"warmth\":%.3f,\"stability\":%.3f,"
                  "\"materials\":[",
                  avian_nest_state_name(n0->state), n0->warmth, n0->stability);
    for (int i = 0; i < n0->material_count; i++) {
        if (i) n += snprintf(buf + n, len - (size_t)n, ",");
        n = append_json_str(buf, len, n, n0->materials[i]);
    }
    n += snprintf(buf + n, len - (size_t)n, "]}");
    return n;
}

int avian_egg_json(const avian_engine_t *a, int egg_id, char *buf, size_t len) {
    const avian_egg_t *e = avian_egg_get(a, egg_id);
    if (!buf || len == 0) return 0;
    if (!e) return snprintf(buf, len, "{\"ok\":false,\"error\":\"egg not found\"}");
    int n = snprintf(buf, len, "{\"ok\":true,\"id\":%d,\"nest_id\":%d,\"name\":", e->id,
                     e->nest_id);
    n = append_json_str(buf, len, n, e->name);
    n += snprintf(buf + n, len - (size_t)n, ",\"kind\":");
    n = append_json_str(buf, len, n, e->kind);
    n += snprintf(buf + n, len - (size_t)n, ",\"state\":\"%s\",\"readiness\":%.3f,"
                  "\"risk\":%.3f,\"cycles\":%d,\"required_cycles\":%d,\"lineage\":",
                  avian_egg_state_name(e->state), e->readiness, e->risk, e->cycles,
                  e->required_cycles);
    n = append_json_str(buf, len, n, e->lineage);
    n += snprintf(buf + n, len - (size_t)n, "}");
    return n;
}

int avian_status_json(const avian_engine_t *a, char *buf, size_t len) {
    if (!buf || len == 0) return 0;
    if (!a || !a->initialized) return snprintf(buf, len, "{\"initialized\":false}");
    int active_nests = 0, active_eggs = 0, ready_eggs = 0, incubating = 0, roosting = 0;
    for (int i = 0; i < AVIAN_MAX_NESTS; i++) {
        if (!a->nests[i].active) continue;
        active_nests++;
        if (a->nests[i].state == AVIAN_NEST_ROOSTING) roosting++;
    }
    for (int i = 0; i < AVIAN_MAX_EGGS; i++) {
        if (!a->eggs[i].active) continue;
        active_eggs++;
        if (a->eggs[i].state == AVIAN_EGG_READY) ready_eggs++;
        if (a->eggs[i].state == AVIAN_EGG_INCUBATING) incubating++;
    }
    return snprintf(buf, len,
                    "{\"initialized\":true,\"nests\":%d,\"eggs\":%d,\"ready_eggs\":%d,"
                    "\"incubating\":%d,\"roosting_nests\":%d,\"materials_added\":%d,"
                    "\"brood_cycles\":%d,\"fledged\":%d,\"roosts\":%d,\"molts\":%d}",
                    active_nests, active_eggs, ready_eggs, incubating, roosting,
                    a->total_materials_added, a->total_brood_cycles, a->total_fledged,
                    a->total_roosts, a->total_molts);
}
