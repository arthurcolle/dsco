#ifndef DSCO_PLUGIN_H
#define DSCO_PLUGIN_H

#include <stdbool.h>
#include <stddef.h>
#include "tools.h"

/*
 * Dynamic plugin system — load external .dylib/.so tool plugins at runtime.
 *
 * Plugin directory: ~/.dsco/plugins/
 * Each plugin exports:
 *   - dsco_plugin_name()     → const char* (plugin name)
 *   - dsco_plugin_version()  → const char* (version string)
 *   - dsco_plugin_tools()    → tool_def_t* (array of tool definitions)
 *   - dsco_plugin_count()    → int (number of tools)
 *   - dsco_plugin_init()     → bool (optional init function)
 *   - dsco_plugin_cleanup()  → void (optional cleanup function)
 *
 * Build a plugin:
 *   cc -shared -fPIC -o myplugin.dylib myplugin.c
 *   cp myplugin.dylib ~/.dsco/plugins/
 */

#define PLUGIN_MAX_PLUGINS   32
#define PLUGIN_MAX_TOOLS     256
#define PLUGIN_DIR_NAME      ".dsco/plugins"

typedef struct {
    const char *name;
    const char *version;
    void       *handle;     /* dlopen handle */
    tool_def_t *tools;
    int         tool_count;
    char        path[1024];
    bool        loaded;
} plugin_t;

typedef struct {
    plugin_t  plugins[PLUGIN_MAX_PLUGINS];
    int       count;
    tool_def_t extra_tools[PLUGIN_MAX_TOOLS]; /* flattened tools from all plugins */
    int        extra_tool_count;
    char       plugin_dir[1024];
} plugin_registry_t;

/* Initialize plugin system, discover and load plugins */
void plugin_init(plugin_registry_t *reg);

/* Reload all plugins (hot reload) */
void plugin_reload(plugin_registry_t *reg);

/* Unload all plugins */
void plugin_cleanup(plugin_registry_t *reg);

/* Get all plugin-provided tools */
const tool_def_t *plugin_get_tools(const plugin_registry_t *reg, int *count);

/* List loaded plugins */
void plugin_list(const plugin_registry_t *reg, char *out, size_t out_len);

/* Load a single plugin by path */
bool plugin_load(plugin_registry_t *reg, const char *path);

/* Unload a single plugin by name */
bool plugin_unload(plugin_registry_t *reg, const char *name);

/* Global plugin registry */
extern plugin_registry_t g_plugins;

#endif
