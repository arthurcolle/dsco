/* The typed, addressable unit of work. See include/work_unit.h. */

#include "work_unit.h"
#include "crypto.h" /* uuid_v4 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *work_unit_kind_name(work_unit_kind_t k) {
    switch (k) {
        case WORK_UNIT_PROMPT:    return "prompt";
        case WORK_UNIT_TOOL:      return "tool";
        case WORK_UNIT_EDIT:      return "edit";
        case WORK_UNIT_COMPOSITE: return "composite";
        default:                  return "unknown";
    }
}

const char *work_join_name(work_join_t j) {
    switch (j) {
        case WORK_JOIN_ALL:    return "all";
        case WORK_JOIN_ANY:    return "any";
        case WORK_JOIN_QUORUM: return "quorum";
        case WORK_JOIN_REDUCE: return "reduce";
        default:               return "all";
    }
}

void work_unit_init(work_unit_t *w, work_unit_kind_t kind) {
    if (!w)
        return;
    memset(w, 0, sizeof(*w));
    w->kind = kind;
    w->temperature = -1.0;
    w->top_p = -1.0;
    w->top_k = -1;
    w->thinking_budget = -1;
    w->max_turns = -1;
    char id[37];
    uuid_v4(id);
    snprintf(w->id, sizeof(w->id), "%s", id);
}

void work_unit_free(work_unit_t *w) {
    if (!w)
        return;
    free(w->title);
    free(w->payload);
    free(w->tool);
    free(w->provider);
    free(w->model);
    free(w->executor);
    free(w->effort);
    free(w->system_prompt);
    free(w->tool_choice);
    free(w->cwd);
    free(w->depends_on);
    free(w->acceptance_cmd);
    memset(w, 0, sizeof(*w));
}

static bool wu_set(char **dst, const char *s) {
    char *copy = NULL;
    if (s) {
        size_t n = strlen(s);
        copy = malloc(n + 1);
        if (!copy)
            return false;
        memcpy(copy, s, n + 1);
    }
    free(*dst);
    *dst = copy;
    return true;
}

bool work_unit_set_title(work_unit_t *w, const char *s) {
    return w && wu_set(&w->title, s);
}

bool work_unit_set_payload(work_unit_t *w, const char *s) {
    return w && wu_set(&w->payload, s);
}

bool work_unit_set_route(work_unit_t *w, const char *provider, const char *model,
                         const char *executor, const char *effort) {
    if (!w)
        return false;
    return wu_set(&w->provider, provider) && wu_set(&w->model, model) &&
           wu_set(&w->executor, executor) && wu_set(&w->effort, effort);
}

bool work_unit_set_instance(work_unit_t *w, const char *system_prompt,
                            const char *tool_choice, const char *cwd) {
    if (!w)
        return false;
    return wu_set(&w->system_prompt, system_prompt) && wu_set(&w->tool_choice, tool_choice) &&
           wu_set(&w->cwd, cwd);
}

bool work_unit_add_dependency(work_unit_t *w, const char *id) {
    if (!w || !id || !id[0])
        return false;
    char (*grown)[WORK_ID_LEN] =
        realloc(w->depends_on, (w->depends_count + 1) * sizeof(*w->depends_on));
    if (!grown)
        return false;
    w->depends_on = grown;
    snprintf(w->depends_on[w->depends_count], WORK_ID_LEN, "%s", id);
    w->depends_count++;
    return true;
}

/* ── Content hash ─────────────────────────────────────────────────────────
 * FNV-1a 64 over the semantic fields. id/title excluded by design: two units
 * describing identical work hash identically, which is what lets the replay
 * ledger detect "same id, different work" drift. */

static uint64_t fnv1a(uint64_t h, const void *data, size_t n) {
    const unsigned char *p = data;
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

static uint64_t fnv1a_str(uint64_t h, const char *s) {
    /* Field separator keeps ("ab","c") distinct from ("a","bc"). */
    h = fnv1a(h, "\x1f", 1);
    if (s)
        h = fnv1a(h, s, strlen(s));
    return h;
}

uint64_t work_unit_hash(const work_unit_t *w) {
    if (!w)
        return 0;
    uint64_t h = 0xcbf29ce484222325ULL;
    int kind = (int)w->kind;
    h = fnv1a(h, &kind, sizeof(kind));
    h = fnv1a_str(h, w->payload);
    h = fnv1a_str(h, w->tool);
    h = fnv1a_str(h, w->provider);
    h = fnv1a_str(h, w->model);
    h = fnv1a_str(h, w->executor);
    h = fnv1a_str(h, w->effort);
    h = fnv1a_str(h, w->system_prompt);
    h = fnv1a_str(h, w->tool_choice);
    h = fnv1a_str(h, w->cwd);
    h = fnv1a(h, &w->temperature, sizeof(w->temperature));
    h = fnv1a(h, &w->top_p, sizeof(w->top_p));
    h = fnv1a(h, &w->top_k, sizeof(w->top_k));
    h = fnv1a(h, &w->thinking_budget, sizeof(w->thinking_budget));
    h = fnv1a(h, &w->max_turns, sizeof(w->max_turns));
    h = fnv1a_str(h, w->acceptance_cmd);
    int join = (int)w->join;
    h = fnv1a(h, &join, sizeof(join));
    for (size_t i = 0; i < w->depends_count; i++)
        h = fnv1a_str(h, w->depends_on[i]);
    return h;
}

/* ── JSON round-trip ────────────────────────────────────────────────────── */

#include "json_util.h"

static void wu_emit_opt_str(jbuf_t *b, const char *key, const char *val) {
    if (!val || !val[0])
        return;
    jbuf_appendf(b, ",\"%s\":", key);
    jbuf_append_json_str(b, val);
}

char *work_unit_to_json(const work_unit_t *w) {
    if (!w)
        return NULL;
    jbuf_t b;
    jbuf_init(&b, 512);
    jbuf_append(&b, "{\"id\":");
    jbuf_append_json_str(&b, w->id);
    jbuf_appendf(&b, ",\"kind\":\"%s\"", work_unit_kind_name(w->kind));
    wu_emit_opt_str(&b, "title", w->title);
    /* "task" rather than "payload" on the wire: this is the key the existing
     * swarm tasks arrays already use, so round-trip stays compatible. */
    wu_emit_opt_str(&b, "task", w->payload);
    wu_emit_opt_str(&b, "tool", w->tool);
    wu_emit_opt_str(&b, "provider", w->provider);
    wu_emit_opt_str(&b, "model", w->model);
    wu_emit_opt_str(&b, "executor", w->executor);
    wu_emit_opt_str(&b, "effort", w->effort);
    wu_emit_opt_str(&b, "system_prompt", w->system_prompt);
    wu_emit_opt_str(&b, "tool_choice", w->tool_choice);
    wu_emit_opt_str(&b, "cwd", w->cwd);
    if (w->temperature >= 0)
        jbuf_appendf(&b, ",\"temperature\":%.3f", w->temperature);
    if (w->top_p >= 0)
        jbuf_appendf(&b, ",\"top_p\":%.3f", w->top_p);
    if (w->top_k > 0)
        jbuf_appendf(&b, ",\"top_k\":%d", w->top_k);
    if (w->thinking_budget > 0)
        jbuf_appendf(&b, ",\"thinking_budget\":%d", w->thinking_budget);
    if (w->max_turns > 0)
        jbuf_appendf(&b, ",\"max_turns\":%d", w->max_turns);
    if (w->depends_count) {
        jbuf_append(&b, ",\"depends_on\":[");
        for (size_t i = 0; i < w->depends_count; i++) {
            if (i)
                jbuf_append(&b, ",");
            jbuf_append_json_str(&b, w->depends_on[i]);
        }
        jbuf_append(&b, "]");
    }
    wu_emit_opt_str(&b, "acceptance_cmd", w->acceptance_cmd);
    if (w->kind == WORK_UNIT_COMPOSITE) {
        jbuf_appendf(&b, ",\"join\":\"%s\"", work_join_name(w->join));
        if (w->join == WORK_JOIN_QUORUM)
            jbuf_appendf(&b, ",\"quorum\":%d", w->quorum);
    }
    if (w->max_cost_usd > 0)
        jbuf_appendf(&b, ",\"max_cost_usd\":%.4f", w->max_cost_usd);
    if (w->max_duration_ms > 0)
        jbuf_appendf(&b, ",\"max_duration_ms\":%llu",
                     (unsigned long long)w->max_duration_ms);
    jbuf_append(&b, "}");
    return b.data; /* caller frees; jbuf memory is heap-owned */
}

typedef struct {
    work_unit_t *w;
    int index;
} wu_dep_ctx_t;

static void wu_dep_cb(const char *element_start, void *vctx) {
    wu_dep_ctx_t *ctx = vctx;
    if (!ctx || !element_start)
        return;
    while (*element_start == ' ' || *element_start == '\n' || *element_start == '\t')
        element_start++;
    if (*element_start != '"')
        return;
    const char *p = element_start + 1;
    char id[WORK_ID_LEN];
    size_t n = 0;
    while (*p && *p != '"' && n < sizeof(id) - 1)
        id[n++] = *p++;
    id[n] = '\0';
    if (n)
        work_unit_add_dependency(ctx->w, id);
}

bool work_unit_from_json(work_unit_t *w, const char *json) {
    if (!w || !json)
        return false;
    work_unit_init(w, WORK_UNIT_PROMPT);

    /* Preserve a caller-supplied id; otherwise keep the freshly minted one so
     * legacy {"task": "..."} objects gain identity on ingest. */
    char *id = json_get_str(json, "id");
    if (id && id[0]) {
        snprintf(w->id, sizeof(w->id), "%s", id);
    }
    free(id);

    char *kind = json_get_str(json, "kind");
    if (kind) {
        if (!strcmp(kind, "tool"))
            w->kind = WORK_UNIT_TOOL;
        else if (!strcmp(kind, "edit"))
            w->kind = WORK_UNIT_EDIT;
        else if (!strcmp(kind, "composite"))
            w->kind = WORK_UNIT_COMPOSITE;
        free(kind);
    }

    /* task | prompt | payload accepted; "task" is the legacy swarm key. */
    char *payload = json_get_str(json, "task");
    if (!payload)
        payload = json_get_str(json, "prompt");
    if (!payload)
        payload = json_get_str(json, "payload");
    free(w->payload);
    w->payload = payload;

    w->title = json_get_str(json, "title");
    w->tool = json_get_str(json, "tool");
    w->provider = json_get_str(json, "provider");
    w->model = json_get_str(json, "model");
    w->executor = json_get_str(json, "executor");
    w->effort = json_get_str(json, "effort");
    w->system_prompt = json_get_str(json, "system_prompt");
    w->tool_choice = json_get_str(json, "tool_choice");
    w->cwd = json_get_str(json, "cwd");
    w->temperature = json_get_double(json, "temperature", -1);
    w->top_p = json_get_double(json, "top_p", -1);
    w->top_k = json_get_int(json, "top_k", -1);
    w->thinking_budget = json_get_int(json, "thinking_budget", -1);
    w->max_turns = json_get_int(json, "max_turns", -1);
    w->acceptance_cmd = json_get_str(json, "acceptance_cmd");
    w->max_cost_usd = json_get_double(json, "max_cost_usd", 0);
    w->max_duration_ms = (uint64_t)json_get_double(json, "max_duration_ms", 0);

    char *join = json_get_str(json, "join");
    if (join) {
        if (!strcmp(join, "any"))
            w->join = WORK_JOIN_ANY;
        else if (!strcmp(join, "quorum"))
            w->join = WORK_JOIN_QUORUM;
        else if (!strcmp(join, "reduce"))
            w->join = WORK_JOIN_REDUCE;
        free(join);
    }
    w->quorum = json_get_int(json, "quorum", 0);

    wu_dep_ctx_t ctx = {.w = w, .index = 0};
    json_array_foreach(json, "depends_on", wu_dep_cb, &ctx);

    /* A unit with no payload and no tool is not work. */
    return (w->payload && w->payload[0]) || (w->tool && w->tool[0]);
}
