#include "agent_profile.h"
#include "json_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>

static agent_profile_t g_profiles[MAX_AGENT_PROFILES];
static int g_profile_count = 0;
static char g_active_name[AP_NAME_LEN] = "";
static bool g_loaded = false;

/* ── Path helpers ────────────────────────────────────────────────────── */

static void profiles_path(char *out, size_t len) {
    const char *home = getenv("HOME");
    if (home && home[0])
        snprintf(out, len, "%s/.dsco/agent_profiles.json", home);
    else
        snprintf(out, len, ".dsco/agent_profiles.json");
}

static void ensure_dir(const char *file_path) {
    char tmp[4096];
    snprintf(tmp, sizeof(tmp), "%s", file_path);
    char *slash = strrchr(tmp, '/');
    if (!slash)
        return;
    *slash = '\0';
    if (tmp[0] == '\0')
        return;
    /* mkdir -p the parent */
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
}

/* ── Init ────────────────────────────────────────────────────────────── */

void agent_profiles_init(void) {
    if (g_loaded)
        return;
    agent_profiles_load();
    g_loaded = true;
}

/* ── CRUD ────────────────────────────────────────────────────────────── */

int agent_profiles_count(void) {
    agent_profiles_init();
    return g_profile_count;
}

agent_profile_t *agent_profile_get(int idx) {
    agent_profiles_init();
    if (idx < 0 || idx >= g_profile_count)
        return NULL;
    return &g_profiles[idx];
}

agent_profile_t *agent_profile_find(const char *name) {
    agent_profiles_init();
    if (!name)
        return NULL;
    for (int i = 0; i < g_profile_count; i++)
        if (strcmp(g_profiles[i].name, name) == 0)
            return &g_profiles[i];
    return NULL;
}

bool agent_profile_save(const agent_profile_t *p) {
    if (!p || !p->name[0])
        return false;
    agent_profiles_init();

    /* Update existing or append */
    for (int i = 0; i < g_profile_count; i++) {
        if (strcmp(g_profiles[i].name, p->name) == 0) {
            g_profiles[i] = *p;
            return agent_profiles_persist();
        }
    }
    if (g_profile_count >= MAX_AGENT_PROFILES)
        return false;
    g_profiles[g_profile_count++] = *p;
    return agent_profiles_persist();
}

bool agent_profile_delete(const char *name) {
    if (!name)
        return false;
    agent_profiles_init();
    for (int i = 0; i < g_profile_count; i++) {
        if (strcmp(g_profiles[i].name, name) == 0) {
            /* Shift remaining profiles down */
            for (int j = i; j < g_profile_count - 1; j++)
                g_profiles[j] = g_profiles[j + 1];
            g_profile_count--;
            /* Clear active if deleted */
            if (strcmp(g_active_name, name) == 0)
                g_active_name[0] = '\0';
            return agent_profiles_persist();
        }
    }
    return false;
}

/* ── Active profile ──────────────────────────────────────────────────── */

void agent_profile_set_active(const char *name) {
    if (!name || !name[0]) {
        g_active_name[0] = '\0';
    } else {
        snprintf(g_active_name, sizeof(g_active_name), "%s", name);
    }
}

const char *agent_profile_active_name(void) {
    return g_active_name[0] ? g_active_name : NULL;
}

const agent_profile_t *agent_profile_active(void) {
    if (!g_active_name[0])
        return NULL;
    return agent_profile_find(g_active_name);
}

bool agent_profile_tool_allowed(const char *tool_name, const char *group_hint) {
    const agent_profile_t *p = agent_profile_active();
    if (!p)
        return true; /* no active profile = allow all */

    /* If neither tool list nor group list is set, allow all */
    if (p->tool_count == 0 && p->group_count == 0)
        return true;

    /* Check explicit tool whitelist first */
    if (p->tool_count > 0) {
        for (int i = 0; i < p->tool_count; i++)
            if (strcmp(p->tools[i], tool_name) == 0)
                return true;
        /* If only tool whitelist (no groups), reject if not in list */
        if (p->group_count == 0)
            return false;
    }

    /* Check group whitelist */
    if (p->group_count > 0 && group_hint && group_hint[0]) {
        for (int i = 0; i < p->group_count; i++)
            if (strcmp(p->groups[i], group_hint) == 0)
                return true;
        /* If only groups (no explicit tools), reject if not in group */
        if (p->tool_count == 0)
            return false;
    }

    /* tool_count > 0 but not found, group_count > 0 but no group_hint: reject */
    if (p->group_count > 0 && (!group_hint || !group_hint[0]))
        return false;

    return false;
}

/* ── JSON serialization ──────────────────────────────────────────────── */

int agent_profile_to_json(const agent_profile_t *p, char *buf, size_t len) {
    jbuf_t b;
    jbuf_init(&b, 512);

    jbuf_append(&b, "{");
    jbuf_append(&b, "\"name\":");
    jbuf_append_json_str(&b, p->name);
    jbuf_append(&b, ",\"description\":");
    jbuf_append_json_str(&b, p->description);
    jbuf_append(&b, ",\"model_hint\":");
    jbuf_append_json_str(&b, p->model_hint);
    jbuf_append(&b, ",\"prompt_prefix\":");
    jbuf_append_json_str(&b, p->prompt_prefix);
    jbuf_appendf(&b, ",\"budget_usd\":%.4f", p->budget_usd);

    jbuf_append(&b, ",\"tools\":[");
    for (int i = 0; i < p->tool_count; i++) {
        if (i > 0)
            jbuf_append(&b, ",");
        jbuf_append_json_str(&b, p->tools[i]);
    }
    jbuf_append(&b, "]");

    jbuf_append(&b, ",\"groups\":[");
    for (int i = 0; i < p->group_count; i++) {
        if (i > 0)
            jbuf_append(&b, ",");
        jbuf_append_json_str(&b, p->groups[i]);
    }
    jbuf_append(&b, "]}");

    int n = 0;
    if (b.data && b.len > 0) {
        n = (int)(b.len < len ? b.len : len - 1);
        memcpy(buf, b.data, (size_t)n);
        buf[n] = '\0';
    }
    jbuf_free(&b);
    return n;
}

int agent_profiles_all_json(char *buf, size_t len) {
    agent_profiles_init();
    jbuf_t b;
    jbuf_init(&b, 4096);

    jbuf_append(&b, "{\"profiles\":[");
    for (int i = 0; i < g_profile_count; i++) {
        if (i > 0)
            jbuf_append(&b, ",");
        char tmp[2048];
        agent_profile_to_json(&g_profiles[i], tmp, sizeof(tmp));
        jbuf_append(&b, tmp);
    }
    jbuf_append(&b, "],\"active\":");
    jbuf_append_json_str(&b, g_active_name[0] ? g_active_name : "");
    jbuf_append(&b, "}");

    int n = 0;
    if (b.data && b.len > 0) {
        n = (int)(b.len < len ? b.len : len - 1);
        memcpy(buf, b.data, (size_t)n);
        buf[n] = '\0';
    }
    jbuf_free(&b);
    return n;
}

/* ── JSON parsing ────────────────────────────────────────────────────── */

typedef struct {
    agent_profile_t *p;
    int *tool_i;
    int *group_i;
} parse_ctx_t;

static void parse_tool_cb(const char *el, void *ctx) {
    parse_ctx_t *c = (parse_ctx_t *)ctx;
    if (*c->tool_i >= AP_MAX_TOOLS)
        return;
    /* Strip quotes */
    const char *s = el;
    while (*s == ' ' || *s == '"')
        s++;
    char *dst = c->p->tools[(*c->tool_i)++];
    int j = 0;
    while (*s && *s != '"' && j < 63)
        dst[j++] = *s++;
    dst[j] = '\0';
}

static void parse_group_cb(const char *el, void *ctx) {
    parse_ctx_t *c = (parse_ctx_t *)ctx;
    if (*c->group_i >= AP_MAX_GROUPS)
        return;
    const char *s = el;
    while (*s == ' ' || *s == '"')
        s++;
    char *dst = c->p->groups[(*c->group_i)++];
    int j = 0;
    while (*s && *s != '"' && j < 31)
        dst[j++] = *s++;
    dst[j] = '\0';
}

bool agent_profile_from_json(agent_profile_t *p, const char *json) {
    if (!p || !json)
        return false;
    memset(p, 0, sizeof(*p));

    char *name = json_get_str(json, "name");
    if (!name || !name[0]) {
        free(name);
        return false;
    }
    snprintf(p->name, sizeof(p->name), "%s", name);
    free(name);

    char *desc = json_get_str(json, "description");
    if (desc) {
        snprintf(p->description, sizeof(p->description), "%s", desc);
        free(desc);
    }

    char *model = json_get_str(json, "model_hint");
    if (model) {
        snprintf(p->model_hint, sizeof(p->model_hint), "%s", model);
        free(model);
    }

    char *prompt = json_get_str(json, "prompt_prefix");
    if (prompt) {
        snprintf(p->prompt_prefix, sizeof(p->prompt_prefix), "%s", prompt);
        free(prompt);
    }

    p->budget_usd = json_get_double(json, "budget_usd", 0.0);

    int tool_i = 0, group_i = 0;
    parse_ctx_t ctx = {p, &tool_i, &group_i};
    json_array_foreach(json, "tools", parse_tool_cb, &ctx);
    json_array_foreach(json, "groups", parse_group_cb, &ctx);
    p->tool_count = tool_i;
    p->group_count = group_i;

    return true;
}

/* ── Persistence ─────────────────────────────────────────────────────── */

typedef struct {
    int idx;
} load_ctx_t;

static void load_profile_cb(const char *el, void *ctx) {
    load_ctx_t *c = (load_ctx_t *)ctx;
    if (c->idx >= MAX_AGENT_PROFILES)
        return;
    agent_profile_t p;
    if (agent_profile_from_json(&p, el))
        g_profiles[c->idx++] = p;
}

bool agent_profiles_load(void) {
    char path[4096];
    profiles_path(path, sizeof(path));

    FILE *f = fopen(path, "r");
    if (!f)
        return true; /* no file = empty registry, not an error */

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    if (sz <= 0 || sz > 4 * 1024 * 1024) {
        fclose(f);
        return false;
    }

    char *buf = malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return false;
    }
    size_t nr = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[nr] = '\0';

    g_profile_count = 0;
    load_ctx_t ctx = {0};
    json_array_foreach(buf, "profiles", load_profile_cb, &ctx);
    g_profile_count = ctx.idx;

    /* Load active profile name */
    char *active = json_get_str(buf, "active");
    if (active) {
        snprintf(g_active_name, sizeof(g_active_name), "%s", active);
        free(active);
    }

    free(buf);
    return true;
}

bool agent_profiles_persist(void) {
    char path[4096];
    profiles_path(path, sizeof(path));
    ensure_dir(path);

    char buf[64 * 1024];
    agent_profiles_all_json(buf, sizeof(buf));

    FILE *f = fopen(path, "w");
    if (!f)
        return false;
    fputs(buf, f);
    fclose(f);
    return true;
}
