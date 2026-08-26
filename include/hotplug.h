#ifndef DSCO_HOTPLUG_H
#define DSCO_HOTPLUG_H

/* Hot-reload for agent tools: dynamic recompilation without restarting dsco.
 *
 * Convention: ~/.dsco/plugins/src/<name>.c  is a plugin source exporting the
 * usual plugin API (dsco_plugin_name/init/tools/tool_count, see plugin.h).
 * hotplug_scan() compares source mtimes against ~/.dsco/plugins/.hotplug_state
 * and, for any new/changed source:
 *
 *   1. clang -O2 -shared -fPIC src/<name>.c -o <name>.hotplug-N.dylib
 *      (N = mtime, so the new build has a *different path* — dlopen caching
 *       never confuses old and new code, even while the old dylib is mapped)
 *   2. plugin_load() the fresh dylib (hot-swap; old one is dlclosed only by a
 *      full plugin_reload, which also rebuilds the tool map)
 *   3. failures leave the previous build live and log to stderr
 *
 * Typical wiring: call hotplug_scan() once per REPL iteration (cheap: a
 * readdir + stat per source) and, when it returns true, invoke the existing
 * plugin_reload tool path so tool_map picks up the new entries.
 *
 * Pure C11 + POSIX, no globals besides one static last-scan timestamp array
 * is deliberately avoided: all state lives on disk (.hotplug_state) so the
 * mechanism is correct across restarts for free. */

#include <stdbool.h>

/* One scan/compile pass. Returns true if at least one plugin was (re)built
 * and loaded — caller should rebuild the tool map. */
bool hotplug_scan(void);

/* Force rebuild+load of every source (used by a `hotplug` tool / command).
 * Returns number of plugins successfully rebuilt. */
int hotplug_rebuild_all(void);

/* Directory used for sources (created if missing). */
const char *hotplug_src_dir(void);

#endif /* DSCO_HOTPLUG_H */
