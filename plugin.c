#include "plugin.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

/* ── Load a single plugin ────────────────────────────────────────────── */

bool plugin_load(plugin_registry_t *reg, const char *path) {
    if (reg->count >= PLUGIN_MAX_PLUGINS) return false;

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
    p->loaded = true;
    reg->count++;

    /* Copy tools into flat registry */
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
