#include "swarm_hierarchy.h"
#include "agent_interop.h"
#include "json_util.h"
#include "tools.h"
#include "../vendor/yyjson.h"
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define TREE_NODES 64
#define TREE_RUNS 8
#define TREE_TASK_BYTES 4096
#define TREE_CONTEXT_BYTES 2048
#define TREE_INPUT_BYTES (1024U * 1024U)
#define TREE_REPLY_BYTES (8U * 1024U * 1024U)
#define TREE_WAVE_WORKERS 32

typedef enum { QUEUED, RUNNING, DONE, ERROR, CANCEL_PENDING, CANCELLED, BLOCKED } state_t;
typedef struct {
    char id[49], parent_id[49], executor[64], provider[64], model[128];
    char *task;
    char output[TREE_CONTEXT_BYTES + 1];
    char generation[64], interop_outcome[48], artifact_dir[1024];
    int parent, depth, agent_id, group_id;
    uint64_t run_id;
    state_t state;
    bool context_truncated, output_truncated, output_binary;
} node_t;
typedef struct {
    char id[96], name[96], cwd[1024], generation[64], error[256];
    node_t nodes[TREE_NODES];
    int count, root, limit, depth, width, wave, group_id;
    int members[TREE_WAVE_WORKERS], member_count;
    int max_turns, max_tokens, run_timeout_ms;
    bool active_wave, fault, submitted;
} tree_t;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static tree_t *runs[TREE_RUNS];
static uint64_t sequence;

static const char *state_name(state_t s) {
    static const char *names[] = {"queued", "running", "done", "error", "cancel_pending", "cancelled", "blocked"};
    return names[s];
}
static bool fail(char *out, size_t cap, const char *reason) {
    jbuf_t b; jbuf_init(&b, 256); jbuf_append(&b, "{\"error\":");
    jbuf_append_json_str(&b, reason); jbuf_append(&b, ",\"submitted\":false}");
    if (b.len < cap) memcpy(out, b.data, b.len + 1);
    else if (cap >= 3) snprintf(out, cap, "{}");
    jbuf_free(&b); return false;
}
static void tree_free(tree_t *t) {
    if (!t) return;
    for (int i = 0; i < t->count; i++) free(t->nodes[i].task);
    free(t);
}
/* Reject ambiguity before the legacy task parser or any worker sees input. */
static bool valid_json(yyjson_val *v, int depth) {
    if (depth > 24) return false;
    if (yyjson_is_str(v)) return strlen(yyjson_get_str(v)) == yyjson_get_len(v);
    size_t i, n; yyjson_val *k, *x;
    if (yyjson_is_obj(v)) {
        if (yyjson_obj_size(v) > 128) return false;
        yyjson_obj_foreach(v, i, n, k, x) {
            if (!valid_json(k, depth + 1) || !valid_json(x, depth + 1)) return false;
            size_t j, m; yyjson_val *other, *ignored;
            yyjson_obj_foreach(v, j, m, other, ignored) {
                (void)ignored;
                if (j >= i) break;
                if (!strcmp(yyjson_get_str(k), yyjson_get_str(other))) return false;
            }
        }
    } else if (yyjson_is_arr(v)) {
        if (yyjson_arr_size(v) > 1024) return false;
        yyjson_arr_foreach(v, i, n, x) if (!valid_json(x, depth + 1)) return false;
    }
    return true;
}
static const char *string(yyjson_val *v, const char *key) {
    return yyjson_get_str(yyjson_obj_get(v, key));
}
static bool fields(yyjson_val *v, const char *const *allowed) {
    size_t i, n; yyjson_val *k, *x;
    yyjson_obj_foreach(v, i, n, k, x) {
        (void)x; bool found = false;
        for (int j = 0; allowed[j]; j++) if (!strcmp(yyjson_get_str(k), allowed[j])) found = true;
        if (!found) return false;
    }
    return true;
}
static bool text_field(yyjson_val *v, const char *key, size_t max) {
    yyjson_val *x = yyjson_obj_get(v, key);
    return !x || (yyjson_is_str(x) && yyjson_get_len(x) > 0 && yyjson_get_len(x) <= max);
}
static bool number(yyjson_val *v, const char *key, int min, int max, int fallback, int *out) {
    yyjson_val *x = yyjson_obj_get(v, key);
    if (!x) { *out = fallback; return true; }
    if (!yyjson_is_int(x) || yyjson_get_sint(x) < min || yyjson_get_sint(x) > max) return false;
    *out = (int)yyjson_get_sint(x); return true;
}
static bool identifier(const char *id) {
    if (!id || !*id || strlen(id) > 48) return false;
    for (const unsigned char *p = (const unsigned char *)id; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '_' || *p == '-' || *p == '.')) return false;
    return true;
}
static void copy(char *dst, size_t cap, const char *src) {
    snprintf(dst, cap, "%s", src ? src : "");
}
static bool complete(const tree_t *t) {
    if (t->active_wave) return false;
    for (int i = 0; i < t->count; i++)
        if (t->nodes[i].state == QUEUED || t->nodes[i].state == RUNNING ||
            t->nodes[i].state == CANCEL_PENDING) return false;
    return true;
}
static const char *outcome(const tree_t *t) {
    if (!t->submitted && !t->id[0]) return "planned";
    if (!complete(t)) return t->fault ? "reconciliation_required" : "running";
    bool cancelled = false;
    for (int i = 0; i < t->count; i++) {
        if (t->nodes[i].state == ERROR || t->nodes[i].state == BLOCKED) return "failed";
        if (t->nodes[i].state == CANCELLED) cancelled = true;
    }
    return cancelled ? "cancelled" : "success";
}
static void block_descendants(tree_t *t) {
    bool changed;
    do {
        changed = false;
        for (int i = 0; i < t->count; i++) {
            node_t *n = &t->nodes[i];
            if (n->state != QUEUED || n->parent < 0) continue;
            state_t p = t->nodes[n->parent].state;
            if (p == ERROR || p == BLOCKED || p == CANCELLED || p == CANCEL_PENDING) {
                n->state = BLOCKED; changed = true;
            }
        }
    } while (changed);
}
static void tree_lines(jbuf_t *b, const tree_t *t, int index) {
    const node_t *n = &t->nodes[index];
    for (int i = 1; i < n->depth; i++) jbuf_append(b, "  ");
    jbuf_appendf(b, "%s [%s] %s%s%s\n", n->id, state_name(n->state),
                 n->executor, n->provider[0] ? ":" : "", n->provider);
    for (int i = 0; i < t->count; i++) if (t->nodes[i].parent == index) tree_lines(b, t, i);
}
static bool snapshot(const tree_t *t, const char *selected, bool ok, char *out, size_t cap) {
    jbuf_t b; jbuf_init(&b, 4096);
    jbuf_append(&b, "{\"schema\":\"dsco.swarm_hierarchy.v1\",\"hierarchy_id\":");
    if (t->id[0]) jbuf_append_json_str(&b, t->id); else jbuf_append(&b, "null");
    jbuf_append(&b, ",\"name\":"); jbuf_append_json_str(&b, t->name);
    jbuf_append(&b, ",\"outcome\":"); jbuf_append_json_str(&b, outcome(t));
    jbuf_appendf(&b, ",\"submitted\":%s,\"complete\":%s,\"logical_nodes\":%d,\"logical_depth\":%d,\"max_level_width\":%d,\"max_concurrency\":%d,\"wave\":%d,\"controller_pid\":%d,\"controller_scope\":\"this process; advance pumps scheduling; retain receipts before process exit\"",
                 t->submitted ? "true" : "false", complete(t) ? "true" : "false",
                 t->count, t->depth, t->width, t->limit, t->wave, (int)getpid());
    if (t->error[0]) { jbuf_append(&b, ",\"error\":"); jbuf_append_json_str(&b, t->error); }
    if (t->group_id >= 0) {
        jbuf_appendf(&b, ",\"group_id\":%d,\"group_generation\":", t->group_id);
        jbuf_append_json_str(&b, t->generation);
        jbuf_appendf(&b, ",\"active_wave\":%s", t->active_wave ? "true" : "false");
    }
    size_t prefix = b.len;
    jbuf_t tree; jbuf_init(&tree, 1024); tree_lines(&tree, t, t->root);
    jbuf_append(&b, ",\"tree\":"); jbuf_append_json_str(&b, tree.data); jbuf_free(&tree);
    jbuf_append(&b, ",\"nodes\":["); bool comma = false;
    for (int i = 0; i < t->count; i++) {
        const node_t *n = &t->nodes[i];
        if (selected && strcmp(selected, n->id)) continue;
        if (comma) jbuf_append_char(&b, ','); comma = true;
        jbuf_append(&b, "{\"id\":"); jbuf_append_json_str(&b, n->id);
        jbuf_append(&b, ",\"parent\":");
        if (n->parent < 0) jbuf_append(&b, "null"); else jbuf_append_json_str(&b, n->parent_id);
        jbuf_appendf(&b, ",\"depth\":%d,\"status\":", n->depth); jbuf_append_json_str(&b, state_name(n->state));
        jbuf_append(&b, ",\"executor\":"); jbuf_append_json_str(&b, n->executor);
        jbuf_append(&b, ",\"provider\":"); jbuf_append_json_str(&b, n->provider);
        jbuf_append(&b, ",\"model\":"); jbuf_append_json_str(&b, n->model);
        jbuf_appendf(&b, ",\"parent_context_truncated\":%s,\"output_truncated\":%s",
                     n->context_truncated ? "true" : "false", n->output_truncated ? "true" : "false");
        jbuf_appendf(&b, ",\"output_encoding\":\"%s\"", n->output_binary ? "base64" : "text");
        if (n->agent_id >= 0) {
            jbuf_appendf(&b, ",\"agent_id\":%d,\"run_id\":%llu,\"group_id\":%d,\"group_generation\":",
                         n->agent_id, (unsigned long long)n->run_id, n->group_id);
            jbuf_append_json_str(&b, n->generation);
        }
        if (n->interop_outcome[0]) { jbuf_append(&b, ",\"interop_outcome\":"); jbuf_append_json_str(&b, n->interop_outcome); }
        if (selected) {
            jbuf_append(&b, ",\"output\":"); jbuf_append_json_str(&b, n->output);
            if (n->artifact_dir[0]) { jbuf_append(&b, ",\"artifact_dir\":"); jbuf_append_json_str(&b, n->artifact_dir); }
        }
        jbuf_append_char(&b, '}');
    }
    jbuf_append(&b, "]}");
    if (b.len >= cap) {
        /* Preserve the control handle after effects, never emit cut JSON. */
        b.len = prefix; b.data[prefix] = '\0';
        jbuf_append(&b, ",\"nodes_omitted\":true,\"hint\":\"use status with node_id or a larger result buffer\"}");
    }
    if (b.len < cap) memcpy(out, b.data, b.len + 1);
    else { jbuf_free(&b); return fail(out, cap, "hierarchy result capacity too small"); }
    jbuf_free(&b); return ok;
}
static tree_t *parse_tree(yyjson_val *root, int physical, int depth_limit, const char **error) {
    static const char *const top_fields[] = {"action", "operation", "nodes", "name", "dry_run", "executor", "provider", "model", "cwd", "max_concurrency", "max_worker_turns", "max_tokens", "run_timeout_ms", NULL};
    static const char *const node_fields[] = {"id", "parent", "task", "executor", "provider", "model", NULL};
    yyjson_val *nodes = yyjson_obj_get(root, "nodes"), *dry = yyjson_obj_get(root, "dry_run");
    if (!fields(root, top_fields) || !yyjson_is_arr(nodes) || !yyjson_arr_size(nodes) ||
        yyjson_arr_size(nodes) > TREE_NODES || (dry && !yyjson_is_bool(dry))) {
        *error = "hierarchy start requires 1..64 nodes and supported fields only"; return NULL;
    }
    if (!text_field(root, "name", 95) || !text_field(root, "cwd", 1023) ||
        !text_field(root, "executor", 63) || !text_field(root, "provider", 63) ||
        !text_field(root, "model", 127)) {
        *error = "invalid hierarchy default field"; return NULL;
    }
    tree_t *t = calloc(1, sizeof(*t));
    if (!t) { *error = "hierarchy allocation failed"; return NULL; }
    t->group_id = -1; t->root = -1; t->count = (int)yyjson_arr_size(nodes);
    copy(t->name, sizeof(t->name), string(root, "name") ? string(root, "name") : "hierarchy");
    copy(t->cwd, sizeof(t->cwd), string(root, "cwd"));
    if (t->cwd[0]) {
        char *resolved = realpath(t->cwd, NULL); struct stat st;
        if (!resolved || strlen(resolved) >= sizeof(t->cwd) || stat(resolved, &st) != 0 ||
            !S_ISDIR(st.st_mode) || access(resolved, X_OK) != 0) {
            free(resolved); *error = "hierarchy cwd must resolve to an accessible directory"; goto bad;
        }
        copy(t->cwd, sizeof(t->cwd), resolved); free(resolved);
    }
    int ceiling = physical < TREE_WAVE_WORKERS ? physical : TREE_WAVE_WORKERS;
    if (!number(root, "max_concurrency", 1, ceiling, ceiling, &t->limit) ||
        !number(root, "max_worker_turns", 1, 64, 12, &t->max_turns) ||
        !number(root, "max_tokens", 1, 65536, 4096, &t->max_tokens) ||
        !number(root, "run_timeout_ms", 100, 3600000, 600000, &t->run_timeout_ms)) {
        *error = "invalid hierarchy worker bounds"; goto bad;
    }
    for (int i = 0; i < t->count; i++) {
        yyjson_val *v = yyjson_arr_get(nodes, i), *p = yyjson_obj_get(v, "parent");
        node_t *n = &t->nodes[i]; n->agent_id = n->group_id = n->parent = -1;
        const char *id = string(v, "id"), *task = string(v, "task");
        if (!yyjson_is_obj(v) || !fields(v, node_fields) || !identifier(id) ||
            !task || !*task || strlen(task) > TREE_TASK_BYTES ||
            (p && !yyjson_is_null(p) && (!yyjson_is_str(p) || !identifier(yyjson_get_str(p)))) ||
            !text_field(v, "executor", 63) || !text_field(v, "provider", 63) || !text_field(v, "model", 127)) {
            *error = "invalid hierarchy node, ID, parent, task, or execution fields"; goto bad;
        }
        copy(n->id, sizeof(n->id), id); copy(n->parent_id, sizeof(n->parent_id), yyjson_get_str(p));
        n->task = strdup(task);
        if (!n->task) { *error = "hierarchy allocation failed"; goto bad; }
        for (int j = 0; j < i; j++) if (!strcmp(n->id, t->nodes[j].id)) {
            *error = "duplicate hierarchy node ID"; goto bad;
        }
        /* A node's explicit route replaces the inherited route as a pair. */
        bool local_route = yyjson_obj_get(v, "provider") || yyjson_obj_get(v, "executor");
        yyjson_val *route = local_route ? v : root;
        const char *executor = string(route, "executor"), *provider = string(route, "provider");
        if (provider && executor && strcmp(executor, "dsco")) {
            *error = "provider and external executor cannot both select a node"; goto bad;
        }
        if (executor && strcmp(executor, "dsco") &&
            (strncmp(executor, "interop:", 8) || !agent_interop_find(executor + 8))) {
            *error = "hierarchy executor must be dsco or a registered interop adapter"; goto bad;
        }
        copy(n->executor, sizeof(n->executor), executor ? executor : "dsco");
        copy(n->provider, sizeof(n->provider), provider);
        copy(n->model, sizeof(n->model), string(v, "model") ? string(v, "model") : string(root, "model"));
    }
    for (int i = 0; i < t->count; i++) {
        node_t *n = &t->nodes[i];
        if (!n->parent_id[0]) {
            if (t->root >= 0) { *error = "hierarchy must have exactly one root"; goto bad; }
            t->root = i;
        } else {
            for (int j = 0; j < t->count; j++) if (!strcmp(n->parent_id, t->nodes[j].id)) n->parent = j;
            if (n->parent < 0) { *error = "hierarchy parent does not exist"; goto bad; }
        }
    }
    if (t->root < 0) { *error = "hierarchy must have exactly one root"; goto bad; }
    int widths[TREE_NODES + 1] = {0};
    for (int i = 0; i < t->count; i++) {
        int at = i, depth = 0;
        while (at >= 0 && depth <= t->count) { depth++; at = t->nodes[at].parent; }
        if (at >= 0) { *error = "hierarchy contains a cycle"; goto bad; }
        if (depth > depth_limit) { *error = "hierarchy exceeds configured logical depth limit"; goto bad; }
        t->nodes[i].depth = depth;
        if (depth > t->depth) t->depth = depth;
        if (++widths[depth] > t->width) t->width = widths[depth];
    }
    return t;
bad:
    tree_free(t); return NULL;
}

static yyjson_doc *delegate(const char *name, jbuf_t *request, const char *tier, bool *accepted) {
    char *reply = calloc(1, TREE_REPLY_BYTES);
    if (!reply) { *accepted = false; return NULL; }
    *accepted = tools_execute_for_tier(name, request->data, tier, reply, TREE_REPLY_BYTES);
    yyjson_doc *doc = yyjson_read(reply, strlen(reply), 0); free(reply);
    if (doc && (!yyjson_is_obj(yyjson_doc_get_root(doc)) || !valid_json(yyjson_doc_get_root(doc), 0))) {
        yyjson_doc_free(doc); doc = NULL;
    }
    return doc;
}
static void group_request(jbuf_t *b, const tree_t *t, const char *action, int timeout) {
    jbuf_init(b, 256); jbuf_appendf(b, "{\"action\":\"%s\",\"group_id\":%d,\"group_generation\":", action, t->group_id);
    jbuf_append_json_str(b, t->generation); jbuf_appendf(b, ",\"timeout\":%d}", timeout);
}
static void set_fault(tree_t *t, const char *error) {
    t->fault = true; copy(t->error, sizeof(t->error), error);
    for (int i = 0; i < t->count; i++) if (t->nodes[i].state == QUEUED) t->nodes[i].state = BLOCKED;
}
static bool launch(tree_t *t, const char *tier) {
    block_descendants(t);
    int level = TREE_NODES + 1; t->member_count = 0;
    for (int i = 0; i < t->count; i++) {
        node_t *n = &t->nodes[i];
        if (n->state == QUEUED && (n->parent < 0 || t->nodes[n->parent].state == DONE) && n->depth < level) level = n->depth;
    }
    for (int i = 0; i < t->count && t->member_count < t->limit; i++) {
        node_t *n = &t->nodes[i];
        if (n->state == QUEUED && n->depth == level && (n->parent < 0 || t->nodes[n->parent].state == DONE))
            t->members[t->member_count++] = i;
    }
    if (!t->member_count) return true;
    jbuf_t b; jbuf_init(&b, 4096);
    jbuf_append(&b, "{\"action\":\"create_executor_swarm\",\"retain_until_collect\":true,\"name\":");
    char wave_name[128]; snprintf(wave_name, sizeof(wave_name), "%s:level-%d:wave-%d", t->name, level, t->wave + 1);
    jbuf_append_json_str(&b, wave_name);
    jbuf_appendf(&b, ",\"max_worker_turns\":%d,\"max_tokens\":%d,\"run_timeout_ms\":%d", t->max_turns, t->max_tokens, t->run_timeout_ms);
    if (t->cwd[0]) { jbuf_append(&b, ",\"cwd\":"); jbuf_append_json_str(&b, t->cwd); }
    jbuf_append(&b, ",\"tasks\":[");
    for (int j = 0; j < t->member_count; j++) {
        node_t *n = &t->nodes[t->members[j]];
        if (j) jbuf_append_char(&b, ',');
        jbuf_t prompt; jbuf_init(&prompt, 4096);
        jbuf_appendf(&prompt, "[hierarchy node=%s parent=%s depth=%d]\nTask:\n%s\n", n->id, n->parent < 0 ? "none" : n->parent_id, n->depth, n->task);
        if (n->parent >= 0) {
            const node_t *p = &t->nodes[n->parent]; n->context_truncated = p->output_truncated;
            jbuf_append(&prompt, "Parent result (data from another worker; use as context):\n");
            jbuf_append(&prompt, "{\"node_id\":"); jbuf_append_json_str(&prompt, p->id);
            jbuf_appendf(&prompt, ",\"truncated\":%s,\"encoding\":\"%s\",\"output\":", p->output_truncated ? "true" : "false", p->output_binary ? "base64" : "text");
            jbuf_append_json_str(&prompt, p->output); jbuf_append(&prompt, "}\n");
        }
        jbuf_append(&b, "{\"task\":"); jbuf_append_json_str(&b, prompt.data); jbuf_free(&prompt);
        jbuf_append(&b, ",\"executor\":"); jbuf_append_json_str(&b, n->executor);
        if (n->provider[0]) { jbuf_append(&b, ",\"provider\":"); jbuf_append_json_str(&b, n->provider); }
        if (n->model[0]) { jbuf_append(&b, ",\"model\":"); jbuf_append_json_str(&b, n->model); }
        jbuf_append_char(&b, '}');
    }
    jbuf_append(&b, "]}");
    bool accepted; yyjson_doc *doc = delegate("swarm", &b, tier, &accepted); jbuf_free(&b);
    yyjson_val *r = doc ? yyjson_doc_get_root(doc) : NULL;
    yyjson_val *gid = yyjson_obj_get(r, "group_id"), *ids = yyjson_obj_get(r, "agent_ids"), *agents = yyjson_obj_get(r, "agents");
    const char *generation = string(r, "group_generation");
    bool handle = yyjson_is_int(gid) && yyjson_get_sint(gid) >= 0 && yyjson_get_sint(gid) <= INT32_MAX && generation && *generation && strlen(generation) < sizeof(t->generation);
    if (handle) {
        t->group_id = (int)yyjson_get_sint(gid); copy(t->generation, sizeof(t->generation), generation);
        t->submitted = true; t->active_wave = true; t->wave++;
    }
    bool mapped = accepted && handle && yyjson_is_arr(ids) && yyjson_is_arr(agents) &&
                  yyjson_arr_size(ids) == (size_t)t->member_count && yyjson_arr_size(agents) == (size_t)t->member_count &&
                  yyjson_get_sint(yyjson_obj_get(r, "agents_spawned")) == t->member_count;
    for (int j = 0; mapped && j < t->member_count; j++) {
        yyjson_val *id = yyjson_arr_get(ids, j), *a = yyjson_arr_get(agents, j), *run = yyjson_obj_get(a, "run_id");
        mapped = yyjson_is_int(id) && yyjson_get_sint(id) >= 0 && yyjson_get_sint(id) <= INT32_MAX &&
                 yyjson_is_int(yyjson_obj_get(a, "id")) && yyjson_get_sint(yyjson_obj_get(a, "id")) == yyjson_get_sint(id) &&
                 yyjson_is_uint(run) && yyjson_get_uint(run) > 0;
        for (int k = 0; mapped && k < j; k++)
            if (yyjson_get_sint(id) == yyjson_get_sint(yyjson_arr_get(ids, k)) ||
                yyjson_get_uint(run) == yyjson_get_uint(yyjson_obj_get(yyjson_arr_get(agents, k), "run_id"))) mapped = false;
    }
    if (mapped) for (int j = 0; j < t->member_count; j++) {
        node_t *n = &t->nodes[t->members[j]]; yyjson_val *a = yyjson_arr_get(agents, j);
        n->agent_id = (int)yyjson_get_sint(yyjson_arr_get(ids, j)); n->run_id = yyjson_get_uint(yyjson_obj_get(a, "run_id"));
        n->group_id = t->group_id; copy(n->generation, sizeof(n->generation), t->generation); n->state = RUNNING;
        copy(n->model, sizeof(n->model), string(a, "model"));
    }
    if (doc) yyjson_doc_free(doc);
    if (mapped) return true;
    set_fault(t, handle ? "partial or invalid wave dispatch; cleanup required before retry" : "wave dispatch denied or failed; no collectable handle returned");
    if (handle) {
        group_request(&b, t, "abort", 1);
        doc = delegate("swarm", &b, tier, &accepted); jbuf_free(&b);
        yyjson_val *active = doc ? yyjson_obj_get(yyjson_doc_get_root(doc), "active") : NULL;
        if (accepted && yyjson_is_int(active) && yyjson_get_sint(active) == 0) t->active_wave = false;
        if (doc) yyjson_doc_free(doc);
    }
    return false;
}
static void capture_output(node_t *n, const char *raw) {
    if (!raw) raw = "";
    /* External receipts remain durable; children get the adapter's text, not
     * nested argv, billing metadata, or base64 masquerading as text. */
    yyjson_doc *doc = yyjson_read(raw, strlen(raw), 0);
    yyjson_val *r = doc ? yyjson_doc_get_root(doc) : NULL;
    const char *schema = string(r, "schema"), *text = string(r, "output");
    bool receipt = schema && !strcmp(schema, "dsco.agent.interop/v1") && text;
    const char *base64 = receipt ? string(r, "output_base64") : NULL;
    n->output_binary = base64 != NULL;
    const char *output = base64 ? base64 : receipt ? text : raw;
    size_t len = strlen(output), bytes = len < TREE_CONTEXT_BYTES ? len : TREE_CONTEXT_BYTES;
    /* Never split a UTF-8 continuation sequence at the byte ceiling. */
    if (base64) bytes -= bytes % 4;
    else while (bytes < len && bytes && ((unsigned char)output[bytes] & 0xc0) == 0x80) bytes--;
    memcpy(n->output, output, bytes); n->output[bytes] = '\0';
    n->output_truncated = len > bytes || !strncmp(raw, "[...truncated ", 14) ||
                          (receipt && yyjson_is_true(yyjson_obj_get(r, "truncated")));
    if (doc) yyjson_doc_free(doc);
}
static bool advance(tree_t *t, const char *tier, int timeout) {
    if (!t->active_wave) return t->fault ? false : launch(t, tier);
    jbuf_t b; group_request(&b, t, "collect", timeout);
    bool accepted; yyjson_doc *doc = delegate("swarm", &b, tier, &accepted); jbuf_free(&b);
    yyjson_val *r = doc ? yyjson_doc_get_root(doc) : NULL;
    const char *generation = string(r, "group_generation");
    bool identity = accepted && generation && !strcmp(generation, t->generation) &&
                    yyjson_is_int(yyjson_obj_get(r, "group_id")) && yyjson_get_sint(yyjson_obj_get(r, "group_id")) == t->group_id;
    bool finished = yyjson_is_true(yyjson_obj_get(r, "complete"));
    if (!identity) { copy(t->error, sizeof(t->error), "collect denied, stale group, or invalid receipt; workers retained"); if (doc) yyjson_doc_free(doc); return false; }
    if (!finished) { if (doc) yyjson_doc_free(doc); return true; }
    if (t->fault) { t->active_wave = false; if (doc) yyjson_doc_free(doc); return false; }
    yyjson_val *rows = yyjson_obj_get(r, "results");
    yyjson_val *matched[TREE_WAVE_WORKERS] = {0};
    bool reconciled = yyjson_is_arr(rows) && yyjson_arr_size(rows) == (size_t)t->member_count && !yyjson_is_true(yyjson_obj_get(r, "results_truncated"));
    for (int j = 0; reconciled && j < t->member_count; j++) {
        const node_t *n = &t->nodes[t->members[j]];
        size_t k, count; yyjson_val *row;
        yyjson_arr_foreach(rows, k, count, row) {
            yyjson_val *id = yyjson_obj_get(row, "id"), *run = yyjson_obj_get(row, "run_id");
            if (yyjson_is_int(id) && yyjson_get_sint(id) == n->agent_id && yyjson_is_uint(run) && yyjson_get_uint(run) == n->run_id) {
                if (matched[j]) { reconciled = false; break; }
                matched[j] = row;
            }
        }
        const char *status = string(matched[j], "status");
        if (!matched[j] || !status || (strcmp(status, "done") && strcmp(status, "error") && strcmp(status, "killed")) ||
            !yyjson_is_str(yyjson_obj_get(matched[j], "output"))) reconciled = false;
    }
    if (!reconciled) {
        copy(t->error, sizeof(t->error), "incomplete or mismatched terminal results; no children scheduled; collect again or abort");
        if (doc) yyjson_doc_free(doc); return false;
    }
    for (int j = 0; j < t->member_count; j++) {
        node_t *n = &t->nodes[t->members[j]]; yyjson_val *row = matched[j]; const char *status = string(row, "status");
        n->state = n->state == CANCEL_PENDING || !strcmp(status, "killed") ? CANCELLED : !strcmp(status, "done") ? DONE : ERROR;
        copy(n->interop_outcome, sizeof(n->interop_outcome), string(row, "interop_outcome"));
        if (n->state == DONE && !strncmp(n->executor, "interop:", 8) && strcmp(n->interop_outcome, "success")) n->state = ERROR;
        copy(n->artifact_dir, sizeof(n->artifact_dir), string(r, "artifact_dir"));
        capture_output(n, string(row, "output"));
    }
    t->active_wave = false; t->error[0] = '\0'; yyjson_doc_free(doc);
    return launch(t, tier);
}
static bool descendant(const tree_t *t, int node, int ancestor) {
    for (int at = node; at >= 0; at = t->nodes[at].parent) if (at == ancestor) return true;
    return false;
}
static bool abort_branch(tree_t *t, const char *tier, int target) {
    bool ok = true; t->error[0] = '\0';
    if (t->fault && t->active_wave) {
        jbuf_t b; group_request(&b, t, "abort", 1); bool accepted;
        yyjson_doc *doc = delegate("swarm", &b, tier, &accepted); jbuf_free(&b);
        yyjson_val *active = doc ? yyjson_obj_get(yyjson_doc_get_root(doc), "active") : NULL;
        if (accepted && yyjson_is_int(active) && yyjson_get_sint(active) == 0) t->active_wave = false;
        else ok = false;
        if (doc) yyjson_doc_free(doc);
    }
    for (int i = 0; i < t->count; i++) {
        node_t *n = &t->nodes[i]; if (!descendant(t, i, target)) continue;
        if (n->state == QUEUED) { n->state = CANCELLED; continue; }
        if (n->state != RUNNING) continue;
        jbuf_t b; jbuf_init(&b, 256);
        jbuf_appendf(&b, "{\"action\":\"kill\",\"id\":%d,\"run_id\":%llu,\"group_generation\":", n->agent_id, (unsigned long long)n->run_id);
        jbuf_append_json_str(&b, n->generation); jbuf_append_char(&b, '}');
        bool accepted; yyjson_doc *doc = delegate("agent", &b, tier, &accepted); jbuf_free(&b);
        if (accepted && doc && yyjson_is_true(yyjson_obj_get(yyjson_doc_get_root(doc), "killed"))) n->state = CANCEL_PENDING;
        else ok = false;
        if (doc) yyjson_doc_free(doc);
    }
    if (!ok) copy(t->error, sizeof(t->error), "branch cancellation was not fully acknowledged; advance to reconcile before retry");
    return ok;
}

bool swarm_hierarchy_execute(const char *input, const char *tier, int physical_limit,
                             int depth_limit, char *out, size_t cap) {
    if (!out || !cap) return false;
    if (!input || !tier || !*tier || physical_limit < 1 || depth_limit < 1 || strlen(input) > TREE_INPUT_BYTES)
        return fail(out, cap, "invalid hierarchy request or authority context");
    /* A post-effect receipt must always have room for the unique handle. */
    if (cap < 4096) return fail(out, cap, "hierarchy requires at least 4096 result bytes; no workers submitted");
    yyjson_doc *doc = yyjson_read(input, strlen(input), 0);
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    if (!yyjson_is_obj(root) || !valid_json(root, 0)) {
        if (doc) yyjson_doc_free(doc); return fail(out, cap, "invalid hierarchy JSON, duplicate keys, NUL, or request bounds");
    }
    const char *op = string(root, "operation");
    if (!op && !yyjson_obj_get(root, "operation")) op = "start";
    bool ok = false;
    if (op && !strcmp(op, "start")) {
        const char *error = NULL; tree_t *t = parse_tree(root, physical_limit, depth_limit, &error);
        if (!t) { yyjson_doc_free(doc); return fail(out, cap, error); }
        if (yyjson_is_true(yyjson_obj_get(root, "dry_run"))) { ok = snapshot(t, NULL, true, out, cap); tree_free(t); }
        else {
            pthread_mutex_lock(&lock); int slot = -1;
            for (int i = 0; i < TREE_RUNS; i++) if (!runs[i]) { slot = i; break; }
            if (slot < 0) for (int i = 0; i < TREE_RUNS; i++) if (complete(runs[i])) { slot = i; tree_free(runs[i]); break; }
            if (slot < 0) { tree_free(t); ok = fail(out, cap, "all eight hierarchy controller slots are active"); }
            else {
                struct timespec now = {0}; clock_gettime(CLOCK_REALTIME, &now);
                snprintf(t->id, sizeof(t->id), "h-%d-%lld-%ld-%llu", (int)getpid(), (long long)now.tv_sec, now.tv_nsec, (unsigned long long)++sequence);
                runs[slot] = t; ok = launch(t, tier); ok = snapshot(t, NULL, ok, out, cap);
            }
            pthread_mutex_unlock(&lock);
        }
    } else {
        static const char *const control_fields[] = {"action", "operation", "hierarchy_id", "node_id", "timeout", NULL};
        const char *id = string(root, "hierarchy_id"), *selected = string(root, "node_id"); int timeout;
        if (!op || (strcmp(op, "status") && strcmp(op, "advance") && strcmp(op, "abort")) ||
            !fields(root, control_fields) || !text_field(root, "hierarchy_id", 95) || !id ||
            (yyjson_obj_get(root, "node_id") && !identifier(selected)) ||
            !number(root, "timeout", 1, 20, 1, &timeout) || (!strcmp(op, "advance") && selected)) {
            yyjson_doc_free(doc); return fail(out, cap, "invalid hierarchy operation, identity, node, or timeout 1..20");
        }
        pthread_mutex_lock(&lock); tree_t *t = NULL;
        for (int i = 0; i < TREE_RUNS; i++) if (runs[i] && !strcmp(runs[i]->id, id)) t = runs[i];
        int target = -1;
        if (t && selected) for (int i = 0; i < t->count; i++) if (!strcmp(t->nodes[i].id, selected)) target = i;
        if (!t) ok = fail(out, cap, "unknown or expired hierarchy_id in this controller process");
        else if (selected && target < 0) ok = fail(out, cap, "unknown hierarchy node_id");
        else {
            ok = true;
            if (!strcmp(op, "advance")) ok = advance(t, tier, timeout);
            else if (!strcmp(op, "abort")) ok = abort_branch(t, tier, selected ? target : t->root);
            ok = snapshot(t, !strcmp(op, "status") ? selected : NULL, ok, out, cap);
        }
        pthread_mutex_unlock(&lock);
    }
    yyjson_doc_free(doc); return ok;
}
