#include "hotplug.h"
#include "plugin.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/* state file: lines of "<mtime> <source_filename>" for sources already built */
#define HOTPLUG_STATE ".hotplug_state"

static char g_src_dir[1024];
static char g_state_path[1200];

static void hotplug_paths_init(void) {
    if (g_src_dir[0]) return;
    const char *home = getenv("HOME");
    if (!home) home = "/tmp";
    char dir[1024];
    snprintf(dir, sizeof dir, "%s/.dsco", home);              mkdir(dir, 0755);
    snprintf(dir, sizeof dir, "%s/.dsco/plugins", home);      mkdir(dir, 0755);
    snprintf(g_src_dir, sizeof g_src_dir, "%s/.dsco/plugins/src", home);
    mkdir(g_src_dir, 0755);
    snprintf(g_state_path, sizeof g_state_path, "%s/.dsco/plugins/%s", home, HOTPLUG_STATE);
}

const char *hotplug_src_dir(void) {
    hotplug_paths_init();
    return g_src_dir;
}

/* ── state file ──────────────────────────────────────────────────────── */

/* Look up the mtime we last successfully built for `name`; 0 if unknown. */
static long state_get(const char *name) {
    FILE *f = fopen(g_state_path, "r");
    if (!f) return 0;
    char line[1024], fname[900];
    long mt, found = 0;
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "%ld %899s", &mt, fname) == 2 && strcmp(fname, name) == 0) {
            found = mt;
            break;
        }
    }
    fclose(f);
    return found;
}

/* Rewrite the state file replacing/adding one entry. Small N, full rewrite. */
static void state_set(const char *name, long mtime) {
    char tmp[1300];
    snprintf(tmp, sizeof tmp, "%s.tmp", g_state_path);
    FILE *in = fopen(g_state_path, "r");
    FILE *out = fopen(tmp, "w");
    if (!out) { if (in) fclose(in); return; }
    char line[1024], fname[900];
    long mt;
    bool wrote = false;
    if (in) {
        while (fgets(line, sizeof line, in)) {
            if (sscanf(line, "%ld %899s", &mt, fname) == 2 && strcmp(fname, name) == 0) {
                fprintf(out, "%ld %s\n", mtime, name);
                wrote = true;
            } else {
                fputs(line, out);
            }
        }
        fclose(in);
    }
    if (!wrote) fprintf(out, "%ld %s\n", mtime, name);
    fclose(out);
    rename(tmp, g_state_path);
}

/* ── compile + load one source ───────────────────────────────────────── */

/* Build src/<file> into plugins/<base>.hotplug-<mtime>.dylib, then load it.
 * Returns true on success. */
static bool hotplug_build_and_load(const char *file, long mtime) {
    char base[256];
    snprintf(base, sizeof base, "%s", file);
    char *dot = strrchr(base, '.');
    if (!dot || strcmp(dot, ".c")) return false;
    *dot = '\0';

    char src[1400], out[1400];
    snprintf(src, sizeof src, "%s/%s", g_src_dir, file);
    snprintf(out, sizeof out, "%s/.dsco/plugins/%s.hotplug-%ld.dylib",
             getenv("HOME") ? getenv("HOME") : "/tmp", base, mtime);

    fprintf(stderr, "  hotplug: compiling %s ...\n", file);

    pid_t pid = fork();
    if (pid == 0) {
        /* child: clang -O2 -shared -fPIC -undefined dynamic_lookup */
        int devnull = open("/dev/null", 1);
        if (devnull >= 0) { dup2(devnull, 2); close(devnull); }
        execlp("clang", "clang",
               "-O2", "-shared", "-fPIC",
               "-undefined", "dynamic_lookup",
               src, "-o", out, (char *)0);
        _exit(127);
    }
    if (pid < 0) return false;
    int st;
    while (waitpid(pid, &st, 0) < 0) {}
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
        fprintf(stderr, "  hotplug: compile FAILED (%s) — keeping previous build\n", file);
        return false;
    }

    /* unload any previously loaded build of the same plugin, then load new */
    plugin_unload(&g_plugins, base);
    if (!plugin_load(&g_plugins, out)) {
        fprintf(stderr, "  hotplug: load failed (%s)\n", out);
        return false;
    }
    fprintf(stderr, "  hotplug: %s live (%s)\n", base, strrchr(out, '/') + 1);
    return true;
}

/* ── public API ──────────────────────────────────────────────────────── */

bool hotplug_scan(void) {
    hotplug_paths_init();
    DIR *d = opendir(g_src_dir);
    if (!d) return false;

    struct { char name[256]; long mtime; } changed[32];
    int n_changed = 0;

    struct dirent *de;
    while ((de = readdir(d)) && n_changed < 32) {
        size_t l = strlen(de->d_name);
        if (l < 3 || strcmp(de->d_name + l - 2, ".c")) continue;
        char full[1400];
        snprintf(full, sizeof full, "%s/%s", g_src_dir, de->d_name);
        struct stat st;
        if (stat(full, &st)) continue;
        if ((long)st.st_mtime != state_get(de->d_name)) {
            snprintf(changed[n_changed].name, 256, "%s", de->d_name);
            changed[n_changed].mtime = (long)st.st_mtime;
            n_changed++;
        }
    }
    closedir(d);

    bool any = false;
    for (int i = 0; i < n_changed; i++) {
        if (hotplug_build_and_load(changed[i].name, changed[i].mtime)) {
            state_set(changed[i].name, changed[i].mtime);
            any = true;
        }
    }
    return any;
}

int hotplug_rebuild_all(void) {
    hotplug_paths_init();
    DIR *d = opendir(g_src_dir);
    if (!d) return 0;
    int ok = 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        size_t l = strlen(de->d_name);
        if (l < 3 || strcmp(de->d_name + l - 2, ".c")) continue;
        char full[1400];
        snprintf(full, sizeof full, "%s/%s", g_src_dir, de->d_name);
        struct stat st;
        if (stat(full, &st)) continue;
        if (hotplug_build_and_load(de->d_name, (long)st.st_mtime)) {
            state_set(de->d_name, (long)st.st_mtime);
            ok++;
        }
    }
    closedir(d);
    return ok;
}
