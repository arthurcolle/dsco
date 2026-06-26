#include "codex_app_directory.h"
#include "json_util.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

static void cad_strlcpy(char *dst, size_t dst_len, const char *src) {
    if (!dst || dst_len == 0)
        return;
    snprintf(dst, dst_len, "%s", src ? src : "");
}

static void cad_append_token(char *dst, size_t dst_len, const char *tok) {
    if (!dst || dst_len == 0 || !tok || !tok[0])
        return;
    if (strstr(dst, tok))
        return;
    size_t n = strlen(dst);
    if (n > 0 && n + 1 < dst_len) {
        dst[n++] = ',';
        dst[n] = '\0';
    }
    snprintf(dst + n, dst_len - n, "%s", tok);
}

static bool cad_ci_contains(const char *hay, const char *needle) {
    if (!hay || !needle || !needle[0])
        return false;
    size_t nl = strlen(needle);
    for (const char *p = hay; *p; p++) {
        size_t i = 0;
        while (i < nl && p[i] && tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i]))
            i++;
        if (i == nl)
            return true;
    }
    return false;
}

static bool cad_raw_bool(const char *obj, const char *key, bool def) {
    return json_get_bool(obj, key, def);
}

static void cad_copy_str_key(const char *obj, const char *key, char *dst, size_t dst_len) {
    char *s = json_get_str(obj, key);
    if (s && s[0])
        cad_strlcpy(dst, dst_len, s);
    free(s);
}

static void cad_copy_first_str(const char *obj, const char *const *keys, char *dst, size_t dst_len) {
    for (int i = 0; keys[i]; i++) {
        char *s = json_get_str(obj, keys[i]);
        if (s && s[0]) {
            cad_strlcpy(dst, dst_len, s);
            free(s);
            return;
        }
        free(s);
    }
}

static void cad_copy_array_strings(const char *obj, const char *key, char *dst, size_t dst_len) {
    char *raw = json_get_raw(obj, key);
    if (!raw)
        return;
    const char *p = raw;
    while ((p = strchr(p, '"')) != NULL) {
        p++;
        const char *q = p;
        bool esc = false;
        while (*q) {
            if (esc) {
                esc = false;
            } else if (*q == '\\') {
                esc = true;
            } else if (*q == '"') {
                break;
            }
            q++;
        }
        if (*q != '"')
            break;
        char tok[128];
        size_t n = (size_t)(q - p);
        if (n >= sizeof(tok))
            n = sizeof(tok) - 1;
        memcpy(tok, p, n);
        tok[n] = '\0';
        cad_append_token(dst, dst_len, tok);
        p = q + 1;
    }
    free(raw);
}

static void cad_extract_labels(codex_app_directory_entry_t *e, const char *obj) {
    e->interactive = cad_raw_bool(obj, "interactive", e->interactive);
    e->consequential = cad_raw_bool(obj, "consequential", e->consequential);
    e->retrievable = cad_raw_bool(obj, "retrievable", e->retrievable);
    e->sync = cad_raw_bool(obj, "sync", e->sync);

    char *labels_raw = json_get_raw(obj, "labels");
    if (labels_raw) {
        if (cad_ci_contains(labels_raw, "interactive"))
            e->interactive = true;
        if (cad_ci_contains(labels_raw, "consequential"))
            e->consequential = true;
        if (cad_ci_contains(labels_raw, "retrievable"))
            e->retrievable = true;
        if (cad_ci_contains(labels_raw, "sync"))
            e->sync = true;
        free(labels_raw);
    }

    cad_copy_array_strings(obj, "labels", e->labels, sizeof(e->labels));
    if (e->interactive)
        cad_append_token(e->labels, sizeof(e->labels), "interactive");
    if (e->consequential)
        cad_append_token(e->labels, sizeof(e->labels), "consequential");
    if (e->retrievable)
        cad_append_token(e->labels, sizeof(e->labels), "retrievable");
    if (e->sync)
        cad_append_token(e->labels, sizeof(e->labels), "sync");
}

unsigned codex_app_directory_actions_for_labels(bool retrievable, bool sync,
                                                bool consequential, bool interactive) {
    unsigned actions = DSCO_INTEGRATION_ACTION_NONE;
    if (retrievable || sync)
        actions |= DSCO_INTEGRATION_ACTION_READ | DSCO_INTEGRATION_ACTION_UNTRUSTED_CONTENT;
    if (consequential)
        actions |= DSCO_INTEGRATION_ACTION_WRITE | DSCO_INTEGRATION_ACTION_REQUIRES_CONFIRMATION;
    if (interactive)
        actions |= DSCO_INTEGRATION_ACTION_INTERACTIVE;
    return actions;
}

static void cad_finalize_entry(codex_app_directory_entry_t *e) {
    if (!e->id[0])
        cad_strlcpy(e->id, sizeof(e->id), e->connector_id[0] ? e->connector_id : e->display_name);
    if (!e->connector_id[0])
        cad_strlcpy(e->connector_id, sizeof(e->connector_id), e->id);
    if (!e->display_name[0])
        cad_strlcpy(e->display_name, sizeof(e->display_name), e->id);
    if (!e->distribution_channel[0])
        cad_strlcpy(e->distribution_channel, sizeof(e->distribution_channel), "codex_app_directory");
    if (!e->catalog_status[0])
        cad_strlcpy(e->catalog_status, sizeof(e->catalog_status), e->stale ? "stale" : "cached");
    e->action_flags = codex_app_directory_actions_for_labels(e->retrievable, e->sync,
                                                             e->consequential, e->interactive);
}

static bool cad_add(codex_app_directory_t *dir, const codex_app_directory_entry_t *e) {
    if (!dir || !e)
        return false;
    if (dir->count == dir->cap) {
        size_t ncap = dir->cap ? dir->cap * 2 : 128;
        codex_app_directory_entry_t *ne = realloc(dir->entries, ncap * sizeof(*ne));
        if (!ne)
            return false;
        dir->entries = ne;
        dir->cap = ncap;
    }
    dir->entries[dir->count++] = *e;
    dir->total_seen++;
    if (e->is_enabled)
        dir->enabled_count++;
    if (e->is_accessible)
        dir->accessible_count++;
    if (e->interactive)
        dir->interactive_count++;
    if (e->consequential)
        dir->consequential_count++;
    if (e->retrievable)
        dir->retrievable_count++;
    if (e->sync)
        dir->sync_count++;
    if (e->stale)
        dir->stale_count++;
    return true;
}

void codex_app_directory_init(codex_app_directory_t *dir) {
    if (dir)
        memset(dir, 0, sizeof(*dir));
}

void codex_app_directory_free(codex_app_directory_t *dir) {
    if (!dir)
        return;
    free(dir->entries);
    memset(dir, 0, sizeof(*dir));
}

const char *codex_app_directory_default_path(char *buf, size_t buf_len) {
    if (!buf || buf_len == 0)
        return NULL;
    const char *env = getenv("DSCO_CODEX_APP_DIRECTORY");
    if (env && env[0]) {
        cad_strlcpy(buf, buf_len, env);
        return buf;
    }
    const char *home = getenv("HOME");
    if (!home || !home[0])
        home = ".";
    snprintf(buf, buf_len, "%s/.dsco/codex_app_directory.json", home);
    return buf;
}

static void cad_parse_entry_cb(const char *element_start, void *ctx) {
    codex_app_directory_t *dir = (codex_app_directory_t *)ctx;
    if (!dir || !element_start || *element_start != '{')
        return;

    codex_app_directory_entry_t e;
    memset(&e, 0, sizeof(e));

    static const char *id_keys[] = {"id", "app_id", "name", NULL};
    static const char *connector_keys[] = {"connector_id", "connectorId", "slug", "id", NULL};
    static const char *display_keys[] = {"display_name", "displayName", "title", "name", NULL};
    static const char *channel_keys[] = {"distribution_channel", "distributionChannel", "distribution", "source", NULL};
    static const char *scope_keys[] = {"scope", "auth_scope", "authScope", NULL};
    static const char *status_keys[] = {"catalog_status", "catalogStatus", "status", NULL};

    cad_copy_first_str(element_start, id_keys, e.id, sizeof(e.id));
    cad_copy_first_str(element_start, connector_keys, e.connector_id, sizeof(e.connector_id));
    cad_copy_first_str(element_start, display_keys, e.display_name, sizeof(e.display_name));
    cad_copy_first_str(element_start, channel_keys, e.distribution_channel, sizeof(e.distribution_channel));
    cad_copy_first_str(element_start, scope_keys, e.scope, sizeof(e.scope));
    cad_copy_first_str(element_start, status_keys, e.catalog_status, sizeof(e.catalog_status));
    cad_copy_array_strings(element_start, "categories", e.categories, sizeof(e.categories));
    if (!e.categories[0])
        cad_copy_str_key(element_start, "category", e.categories, sizeof(e.categories));

    e.is_enabled = cad_raw_bool(element_start, "isEnabled", cad_raw_bool(element_start, "is_enabled", false));
    e.is_accessible = cad_raw_bool(element_start, "isAccessible", cad_raw_bool(element_start, "is_accessible", false));
    e.stale = cad_raw_bool(element_start, "stale", false) || cad_ci_contains(e.catalog_status, "stale");
    cad_extract_labels(&e, element_start);
    cad_finalize_entry(&e);

    if (e.id[0] || e.connector_id[0] || e.display_name[0])
        (void)cad_add(dir, &e);
}

bool codex_app_directory_load_file(codex_app_directory_t *dir, const char *path, char *err, size_t err_len) {
    if (!dir) {
        if (err && err_len)
            snprintf(err, err_len, "directory output is NULL");
        return false;
    }
    codex_app_directory_free(dir);
    codex_app_directory_init(dir);

    char default_path[1024];
    if (!path || !path[0])
        path = codex_app_directory_default_path(default_path, sizeof(default_path));
    cad_strlcpy(dir->source_path, sizeof(dir->source_path), path ? path : "");

    FILE *f = fopen(path, "rb");
    if (!f) {
        if (err && err_len)
            snprintf(err, err_len, "could not open %s", path ? path : "(null)");
        return false;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        if (err && err_len)
            snprintf(err, err_len, "could not seek %s", path);
        return false;
    }
    long len = ftell(f);
    if (len < 0) {
        fclose(f);
        if (err && err_len)
            snprintf(err, err_len, "could not size %s", path);
        return false;
    }
    rewind(f);
    char *buf = malloc((size_t)len + 1);
    if (!buf) {
        fclose(f);
        if (err && err_len)
            snprintf(err, err_len, "out of memory reading %s", path);
        return false;
    }
    size_t got = fread(buf, 1, (size_t)len, f);
    fclose(f);
    buf[got] = '\0';

    int n = json_array_foreach(buf, "apps", cad_parse_entry_cb, dir);
    if (n == 0)
        n = json_array_foreach(buf, "connectors", cad_parse_entry_cb, dir);
    if (n == 0)
        n = json_array_foreach(buf, "items", cad_parse_entry_cb, dir);
    if (n == 0 && buf[0] == '[') {
        char *wrapped = malloc(got + 16);
        if (wrapped) {
            snprintf(wrapped, got + 16, "{\"items\":%s}", buf);
            n = json_array_foreach(wrapped, "items", cad_parse_entry_cb, dir);
            free(wrapped);
        }
    }
    free(buf);

    if (dir->count == 0) {
        if (err && err_len)
            snprintf(err, err_len, "no connector entries found in %s", path);
        return false;
    }
    if (err && err_len)
        err[0] = '\0';
    return true;
}

const codex_app_directory_entry_t *codex_app_directory_find(const codex_app_directory_t *dir,
                                                            const char *id_or_name) {
    if (!dir || !id_or_name || !id_or_name[0])
        return NULL;
    for (size_t i = 0; i < dir->count; i++) {
        const codex_app_directory_entry_t *e = &dir->entries[i];
        if (strcasecmp(e->id, id_or_name) == 0 || strcasecmp(e->connector_id, id_or_name) == 0 ||
            strcasecmp(e->display_name, id_or_name) == 0)
            return e;
    }
    return NULL;
}
