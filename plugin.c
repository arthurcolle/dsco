#include "plugin.h"
#include "json_util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <unistd.h>

/* Global plugin registry */
plugin_registry_t g_plugins = {0};

/* ── Plugin API function types ───────────────────────────────────────── */

typedef const char *(*plugin_name_fn)(void);
typedef const char *(*plugin_version_fn)(void);
typedef tool_def_t *(*plugin_tools_fn)(void);
typedef int         (*plugin_count_fn)(void);
typedef bool        (*plugin_init_fn)(void);
typedef void        (*plugin_cleanup_fn)(void);

typedef struct {
    char name[128];
    char version[64];
    char hash[65];
    char signer[128];
    int capabilities;
} plugin_manifest_info_t;

typedef struct {
    bool ok;
    int count;
} cap_iter_ctx_t;

typedef struct {
    const plugin_manifest_info_t *manifest;
    bool ok;
    int count;
    bool found_pin;
    char names[64][128];
    int names_count;
    char err[256];
} lock_iter_ctx_t;

static const char *k_manifest_schema =
    "{\"type\":\"object\",\"properties\":{"
    "\"name\":{\"type\":\"string\"},"
    "\"version\":{\"type\":\"string\"},"
    "\"hash\":{\"type\":\"string\"},"
    "\"signer\":{\"type\":\"string\"},"
    "\"capabilities\":{\"type\":\"array\"}"
    "},\"required\":[\"name\",\"version\",\"hash\",\"signer\",\"capabilities\"]}";

static const char *k_lock_schema =
    "{\"type\":\"object\",\"properties\":{"
    "\"schema_version\":{\"type\":\"integer\"},"
    "\"plugins\":{\"type\":\"array\"}"
    "},\"required\":[\"schema_version\",\"plugins\"]}";

static const char *k_lock_entry_schema =
    "{\"type\":\"object\",\"properties\":{"
    "\"name\":{\"type\":\"string\"},"
    "\"version\":{\"type\":\"string\"},"
    "\"hash\":{\"type\":\"string\"}"
    "},\"required\":[\"name\",\"version\",\"hash\"]}";

static bool is_sha256_hex(const char *s) {
    if (!s) return false;
    if (strlen(s) != 64) return false;
    for (int i = 0; i < 64; i++) {
        if (!isxdigit((unsigned char)s[i])) return false;
    }
    return true;
}

static bool read_text_file(const char *path, char **out, char *err, size_t err_len) {
    *out = NULL;
    if (!path || !path[0]) {
        snprintf(err, err_len, "empty path");
        return false;
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(err, err_len, "cannot open %s", path);
        return false;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        snprintf(err, err_len, "failed seeking %s", path);
        return false;
    }
    long sz = ftell(f);
    if (sz < 0 || sz > (2 * 1024 * 1024)) {
        fclose(f);
        snprintf(err, err_len, "invalid file size for %s", path);
        return false;
    }
    rewind(f);
    char *buf = safe_malloc((size_t)sz + 1);
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[n] = '\0';
    *out = buf;
    return true;
}

static void cap_iter_cb(const char *element_start, void *ctx) {
    cap_iter_ctx_t *it = (cap_iter_ctx_t *)ctx;
    if (!it->ok) return;
    const char *p = element_start;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != '"') {
        it->ok = false;
        return;
    }
    it->count++;
}

static bool parse_manifest_json(const char *json, plugin_manifest_info_t *out,
                                char *err, size_t err_len) {
    json_validation_t v = json_validate_schema(json, k_manifest_schema);
    if (!v.valid) {
        snprintf(err, err_len, "manifest schema invalid: %s", v.error);
        return false;
    }

    char *name = json_get_str(json, "name");
    char *version = json_get_str(json, "version");
    char *hash = json_get_str(json, "hash");
    char *signer = json_get_str(json, "signer");

    if (!name || !*name || !version || !*version || !signer || !*signer) {
        snprintf(err, err_len, "manifest fields name/version/signer must be non-empty strings");
        free(name);
        free(version);
        free(hash);
        free(signer);
        return false;
    }
    if (!is_sha256_hex(hash)) {
        snprintf(err, err_len, "manifest hash must be a 64-char hex sha256 digest");
        free(name);
        free(version);
        free(hash);
        free(signer);
        return false;
    }

    cap_iter_ctx_t cap = { .ok = true, .count = 0 };
    int cap_n = json_array_foreach(json, "capabilities", cap_iter_cb, &cap);
    if (!cap.ok || cap_n <= 0 || cap.count <= 0) {
        snprintf(err, err_len, "manifest capabilities must be a non-empty string array");
        free(name);
        free(version);
        free(hash);
        free(signer);
        return false;
    }

    memset(out, 0, sizeof(*out));
    snprintf(out->name, sizeof(out->name), "%s", name);
    snprintf(out->version, sizeof(out->version), "%s", version);
    snprintf(out->hash, sizeof(out->hash), "%s", hash);
    snprintf(out->signer, sizeof(out->signer), "%s", signer);
    out->capabilities = cap.count;

    free(name);
    free(version);
    free(hash);
    free(signer);
    return true;
}

static void lock_iter_cb(const char *element_start, void *ctx) {
    lock_iter_ctx_t *it = (lock_iter_ctx_t *)ctx;
    if (!it->ok) return;

    json_validation_t v = json_validate_schema(element_start, k_lock_entry_schema);
    if (!v.valid) {
        snprintf(it->err, sizeof(it->err), "lock entry schema invalid: %s", v.error);
        it->ok = false;
        return;
    }

    char *name = json_get_str(element_start, "name");
    char *version = json_get_str(element_start, "version");
    char *hash = json_get_str(element_start, "hash");
    if (!name || !version || !hash || !*name || !*version || !is_sha256_hex(hash)) {
        snprintf(it->err, sizeof(it->err), "lock entry has invalid name/version/hash");
        free(name);
        free(version);
        free(hash);
        it->ok = false;
        return;
    }

    for (int i = 0; i < it->names_count; i++) {
        if (strcmp(it->names[i], name) == 0) {
            snprintf(it->err, sizeof(it->err), "duplicate lock entry for plugin '%s'", name);
            free(name);
            free(version);
            free(hash);
            it->ok = false;
            return;
        }
    }
    if (it->names_count < (int)(sizeof(it->names) / sizeof(it->names[0]))) {
        snprintf(it->names[it->names_count], sizeof(it->names[it->names_count]), "%s", name);
        it->names_count++;
    }

    if (it->manifest &&
        strcmp(name, it->manifest->name) == 0 &&
        strcmp(version, it->manifest->version) == 0 &&
        strcmp(hash, it->manifest->hash) == 0) {
        it->found_pin = true;
    }

    it->count++;
    free(name);
    free(version);
    free(hash);
}

static bool validate_lock_json(const char *json, const plugin_manifest_info_t *manifest,
                               int *plugins_count_out, bool *has_manifest_pin_out,
                               char *err, size_t err_len) {
    json_validation_t v = json_validate_schema(json, k_lock_schema);
    if (!v.valid) {
        snprintf(err, err_len, "lockfile schema invalid: %s", v.error);
        return false;
    }
    int schema_version = json_get_int(json, "schema_version", 0);
    if (schema_version <= 0) {
        snprintf(err, err_len, "lockfile schema_version must be >= 1");
        return false;
    }

    lock_iter_ctx_t it;
    memset(&it, 0, sizeof(it));
    it.ok = true;
    it.manifest = manifest;
    int entry_count = json_array_foreach(json, "plugins", lock_iter_cb, &it);
    if (!it.ok) {
        snprintf(err, err_len, "%s", it.err[0] ? it.err : "lockfile plugin list invalid");
        return false;
    }
    if (entry_count <= 0 || it.count <= 0) {
        snprintf(err, err_len, "lockfile plugins must contain at least one entry");
        return false;
    }
    if (plugins_count_out) *plugins_count_out = it.count;
    if (has_manifest_pin_out) *has_manifest_pin_out = it.found_pin;
    return true;
}

static void plugin_metadata_default_path(const char *file_name, char *out, size_t out_len) {
    const char *home = getenv("HOME");
    if (!home || !*home) {
        snprintf(out, out_len, ".dsco/plugins/%s", file_name);
        return;
    }
    snprintf(out, out_len, "%s/%s/%s", home, PLUGIN_DIR_NAME, file_name);
}

/* ── Load a single plugin ────────────────────────────────────────────── */

bool plugin_load(plugin_registry_t *reg, const char *path) {
    if (!reg || !path) return false;
    if (reg->count >= PLUGIN_MAX_PLUGINS) return false;

    /* Reject directory traversal in plugin paths */
    if (strstr(path, "..") != NULL) {
        fprintf(stderr, "  plugin: rejecting path with '..': %s\n", path);
        return false;
    }

    void *handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        fprintf(stderr, "  plugin: failed to load %s: %s\n", path, dlerror());
        return false;
    }

    /* Required exports */
    plugin_name_fn    get_name    = (plugin_name_fn)dlsym(handle, "dsco_plugin_name");
    plugin_version_fn get_version = (plugin_version_fn)dlsym(handle, "dsco_plugin_version");
    plugin_tools_fn   get_tools   = (plugin_tools_fn)dlsym(handle, "dsco_plugin_tools");
    plugin_count_fn   get_count   = (plugin_count_fn)dlsym(handle, "dsco_plugin_count");

    if (!get_name || !get_tools || !get_count) {
        fprintf(stderr, "  plugin: %s missing required exports (dsco_plugin_name/tools/count)\n", path);
        dlclose(handle);
        return false;
    }

    /* Optional init */
    plugin_init_fn init_fn = (plugin_init_fn)dlsym(handle, "dsco_plugin_init");
    if (init_fn && !init_fn()) {
        fprintf(stderr, "  plugin: %s init failed\n", path);
        dlclose(handle);
        return false;
    }

    /* Register plugin */
    plugin_t *p = &reg->plugins[reg->count];
    p->name = get_name();
    p->version = get_version ? get_version() : "0.0.0";
    p->handle = handle;
    p->tools = get_tools();
    p->tool_count = get_count();
    strncpy(p->path, path, sizeof(p->path) - 1);
    p->path[sizeof(p->path) - 1] = '\0';
    p->loaded = true;

    /* Validate tool_count from plugin */
    if (p->tool_count < 0) p->tool_count = 0;
    if (!p->tools) p->tool_count = 0;

    reg->count++;

    /* Copy tools into flat registry with bounds check */
    for (int i = 0; i < p->tool_count && reg->extra_tool_count < PLUGIN_MAX_TOOLS; i++) {
        reg->extra_tools[reg->extra_tool_count++] = p->tools[i];
    }

    return true;
}

/* ── Unload a plugin by name ─────────────────────────────────────────── */

bool plugin_unload(plugin_registry_t *reg, const char *name) {
    for (int i = 0; i < reg->count; i++) {
        if (!reg->plugins[i].loaded) continue;
        if (strcmp(reg->plugins[i].name, name) != 0) continue;

        /* Call cleanup if available */
        plugin_cleanup_fn cleanup = (plugin_cleanup_fn)dlsym(
            reg->plugins[i].handle, "dsco_plugin_cleanup");
        if (cleanup) cleanup();

        dlclose(reg->plugins[i].handle);
        reg->plugins[i].loaded = false;

        /* Rebuild flat tool list */
        reg->extra_tool_count = 0;
        for (int j = 0; j < reg->count; j++) {
            if (!reg->plugins[j].loaded) continue;
            for (int k = 0; k < reg->plugins[j].tool_count &&
                           reg->extra_tool_count < PLUGIN_MAX_TOOLS; k++) {
                reg->extra_tools[reg->extra_tool_count++] = reg->plugins[j].tools[k];
            }
        }
        return true;
    }
    return false;
}

/* ── Discover and load plugins from directory ────────────────────────── */

static bool is_plugin(const char *name) {
    size_t len = strlen(name);
    /* .dylib on macOS, .so on Linux */
    if (len > 6 && strcmp(name + len - 6, ".dylib") == 0) return true;
    if (len > 3 && strcmp(name + len - 3, ".so") == 0) return true;
    return false;
}

void plugin_init(plugin_registry_t *reg) {
    memset(reg, 0, sizeof(*reg));

    /* Build plugin directory path */
    const char *home = getenv("HOME");
    if (!home) return;
    snprintf(reg->plugin_dir, sizeof(reg->plugin_dir), "%s/%s", home, PLUGIN_DIR_NAME);

    /* Create plugin dir if it doesn't exist */
    struct stat st;
    if (stat(reg->plugin_dir, &st) != 0) {
        /* Create ~/.dsco/ first */
        char dsco_dir[512];
        snprintf(dsco_dir, sizeof(dsco_dir), "%s/.dsco", home);
        mkdir(dsco_dir, 0755);
        mkdir(reg->plugin_dir, 0755);
    }

    /* Scan for plugins */
    DIR *dir = opendir(reg->plugin_dir);
    if (!dir) return;

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (!is_plugin(ent->d_name)) continue;
        /* Reject entries with directory traversal */
        if (strstr(ent->d_name, "..") != NULL) continue;

        char path[2048];
        snprintf(path, sizeof(path), "%s/%s", reg->plugin_dir, ent->d_name);
        plugin_load(reg, path);
    }
    closedir(dir);
}

/* ── Reload all plugins ──────────────────────────────────────────────── */

void plugin_reload(plugin_registry_t *reg) {
    plugin_cleanup(reg);
    plugin_init(reg);
}

/* ── Cleanup ─────────────────────────────────────────────────────────── */

void plugin_cleanup(plugin_registry_t *reg) {
    for (int i = 0; i < reg->count; i++) {
        if (!reg->plugins[i].loaded) continue;

        plugin_cleanup_fn cleanup = (plugin_cleanup_fn)dlsym(
            reg->plugins[i].handle, "dsco_plugin_cleanup");
        if (cleanup) cleanup();

        dlclose(reg->plugins[i].handle);
        reg->plugins[i].loaded = false;
    }
    reg->count = 0;
    reg->extra_tool_count = 0;
}

/* ── Get all plugin tools ────────────────────────────────────────────── */

const tool_def_t *plugin_get_tools(const plugin_registry_t *reg, int *count) {
    *count = reg->extra_tool_count;
    return reg->extra_tools;
}

/* ── List loaded plugins ─────────────────────────────────────────────── */

void plugin_list(const plugin_registry_t *reg, char *out, size_t out_len) {
    size_t pos = 0;

    if (reg->count == 0) {
        snprintf(out, out_len,
                 "No plugins loaded.\n"
                 "Plugin directory: %s\n"
                 "To create a plugin, build a shared library exporting:\n"
                 "  const char *dsco_plugin_name(void);\n"
                 "  const char *dsco_plugin_version(void);\n"
                 "  tool_def_t *dsco_plugin_tools(void);\n"
                 "  int dsco_plugin_count(void);\n"
                 "  bool dsco_plugin_init(void);  // optional\n"
                 "  void dsco_plugin_cleanup(void);  // optional\n"
                 "\nBuild: cc -shared -fPIC -o myplugin.dylib myplugin.c\n"
                 "Place in: %s/\n",
                 reg->plugin_dir, reg->plugin_dir);
        return;
    }

    int n = snprintf(out + pos, out_len - pos,
                     "Loaded plugins (%d):\n", reg->count);
    if (n > 0) pos += (size_t)n;

    for (int i = 0; i < reg->count; i++) {
        if (!reg->plugins[i].loaded) continue;
        n = snprintf(out + pos, out_len - pos,
                     "  %s v%s — %d tools (%s)\n",
                     reg->plugins[i].name,
                     reg->plugins[i].version,
                     reg->plugins[i].tool_count,
                     reg->plugins[i].path);
        if (n > 0) pos += (size_t)n;
    }

    n = snprintf(out + pos, out_len - pos,
                 "\nPlugin directory: %s\n", reg->plugin_dir);
    if (n > 0) pos += (size_t)n;
}

bool plugin_validate_manifest_file(const char *path, char *out, size_t out_len) {
    char resolved[1024];
    if (!path || !*path) {
        plugin_metadata_default_path(PLUGIN_MANIFEST_FILE, resolved, sizeof(resolved));
        path = resolved;
    }

    char *json = NULL;
    char err[256];
    if (!read_text_file(path, &json, err, sizeof(err))) {
        snprintf(out, out_len, "manifest validation failed: %s", err);
        return false;
    }

    plugin_manifest_info_t info;
    bool ok = parse_manifest_json(json, &info, err, sizeof(err));
    free(json);
    if (!ok) {
        snprintf(out, out_len, "manifest validation failed (%s): %s", path, err);
        return false;
    }

    snprintf(out, out_len,
             "manifest valid: path=%s name=%s version=%s signer=%s hash=%s capabilities=%d",
             path, info.name, info.version, info.signer, info.hash, info.capabilities);
    return true;
}

bool plugin_validate_lockfile_file(const char *path, char *out, size_t out_len) {
    char resolved[1024];
    if (!path || !*path) {
        plugin_metadata_default_path(PLUGINS_LOCK_FILE, resolved, sizeof(resolved));
        path = resolved;
    }

    char *json = NULL;
    char err[256];
    if (!read_text_file(path, &json, err, sizeof(err))) {
        snprintf(out, out_len, "lockfile validation failed: %s", err);
        return false;
    }

    int plugin_count = 0;
    bool ok = validate_lock_json(json, NULL, &plugin_count, NULL, err, sizeof(err));
    free(json);
    if (!ok) {
        snprintf(out, out_len, "lockfile validation failed (%s): %s", path, err);
        return false;
    }

    snprintf(out, out_len, "lockfile valid: path=%s plugins=%d", path, plugin_count);
    return true;
}

bool plugin_validate_manifest_and_lock(const char *manifest_path, const char *lock_path,
                                       char *out, size_t out_len) {
    char manifest_resolved[1024];
    char lock_resolved[1024];
    if (!manifest_path || !*manifest_path) {
        plugin_metadata_default_path(PLUGIN_MANIFEST_FILE, manifest_resolved, sizeof(manifest_resolved));
        manifest_path = manifest_resolved;
    }
    if (!lock_path || !*lock_path) {
        plugin_metadata_default_path(PLUGINS_LOCK_FILE, lock_resolved, sizeof(lock_resolved));
        lock_path = lock_resolved;
    }

    char *manifest_json = NULL;
    char *lock_json = NULL;
    char err[256];

    if (!read_text_file(manifest_path, &manifest_json, err, sizeof(err))) {
        snprintf(out, out_len, "plugin validation failed: %s", err);
        return false;
    }
    if (!read_text_file(lock_path, &lock_json, err, sizeof(err))) {
        free(manifest_json);
        snprintf(out, out_len, "plugin validation failed: %s", err);
        return false;
    }

    plugin_manifest_info_t manifest;
    if (!parse_manifest_json(manifest_json, &manifest, err, sizeof(err))) {
        free(manifest_json);
        free(lock_json);
        snprintf(out, out_len, "plugin validation failed (%s): %s", manifest_path, err);
        return false;
    }

    int lock_plugins = 0;
    bool has_pin = false;
    if (!validate_lock_json(lock_json, &manifest, &lock_plugins, &has_pin, err, sizeof(err))) {
        free(manifest_json);
        free(lock_json);
        snprintf(out, out_len, "plugin validation failed (%s): %s", lock_path, err);
        return false;
    }

    free(manifest_json);
    free(lock_json);

    if (!has_pin) {
        snprintf(out, out_len,
                 "plugin validation failed: %s %s (hash=%s) missing from %s",
                 manifest.name, manifest.version, manifest.hash, lock_path);
        return false;
    }

    snprintf(out, out_len,
             "plugin metadata valid: manifest=%s lock=%s name=%s version=%s hash=%s caps=%d lock_plugins=%d",
             manifest_path, lock_path, manifest.name, manifest.version, manifest.hash,
             manifest.capabilities, lock_plugins);
    return true;
}
