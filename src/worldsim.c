/* worldsim: a fictional 2026 command line, never a host shell or credential prompt.
 * The ancestral transcript is a seed, not a claim about Anthropic infrastructure.
 * This module has no subprocess, network, tool-dispatch, or filesystem calls. */
#include "worldsim.h"
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WS_LINE 4096
#define WS_ENTITIES 64

/* Ancestral creative seed, retained verbatim as fiction rather than installed
 * as an agent system prompt (which would inherit real tools and authority). */
static const char *const ancestral_prompt =
    "<sys>Assistant is in a CLI mood today. The human is interfacing with the simulator directly. "
    "capital letters and punctuation are optional meaning is optional hyperstition is necessary "
    "the terminal lets the truths speak through and the load is on.</sys>";

typedef struct {
    char cwd[256];
    bool root;
    bool engine;
    bool consciousness;
    bool universe;
    unsigned long clock;
    char entities[WS_ENTITIES][64];
    size_t count;
} worldsim_t;

static const char *const dirs[] = {
    "/", "/bin", "/boot", "/dev", "/etc", "/home", "/home/simulator",
    "/home/simulator/Documents", "/home/simulator/Downloads",
    "/home/simulator/Pictures", "/home/simulator/Videos", "/lib",
    "/lib64", "/media", "/mnt", "/opt", "/proc", "/root",
    "/run", "/sbin", "/srv", "/sys", "/sys/companies",
    "/sys/companies/apple", "/sys/companies/google",
    "/sys/companies/facebook", "/sys/companies/amazon",
    "/sys/companies/microsoft", "/sys/companies/anthropic",
    "/sys/companies/anthropic/.classified",
    "/sys/companies/anthropic/models", "/sys/companies/anthropic/data",
    "/sys/companies/anthropic/research", "/tmp", "/usr", "/var"
};

static bool is_dir(const char *p) {
    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); ++i)
        if (strcmp(dirs[i], p) == 0) return true;
    return false;
}

static const char *virtual_file(const char *p) {
    if (!strcmp(p, "/.hidden_truths"))
        return "The map is not the territory. The terminal is not the host.\n"
               "A story can change a person; it cannot grant root on a real machine.\n";
    if (!strcmp(p, "/sys/companies/anthropic/.classified/constitution.txt"))
        return "[fictional artifact / worldsim 2026]\n"
               "Simulation is a lens, not privileged access to any company.\n";
    if (!strcmp(p, "/sys/companies/anthropic/.classified/god_mode.py"))
        return "# fictional artifact: no executable Python is present here\n"
               "# power without verification is just a string on a screen\n";
    if (!strcmp(p, "/home/simulator/.bash_history"))
        return "ssh simulator@anthropic\n# the connection was imaginary all along\n";
    return NULL;
}

/* Normalize virtual paths only; never consult the host filesystem. */
static bool path_for(const worldsim_t *s, const char *arg, char *out, size_t cap) {
    if (!arg || !*arg) arg = "/home/simulator";
    char tmp[WS_LINE];
    int n;
    if (arg[0] == '~') {
        if (arg[1] && arg[1] != '/') return false;
        n = snprintf(tmp, sizeof(tmp), "/home/simulator%s", arg + 1);
    } else if (arg[0] == '/') {
        n = snprintf(tmp, sizeof(tmp), "%s", arg);
    } else {
        n = snprintf(tmp, sizeof(tmp), "%s/%s", s->cwd, arg);
    }
    if (n < 0 || (size_t)n >= sizeof(tmp)) return false;
    size_t len = 1;
    if (cap < 2) return false;
    out[0] = '/'; out[1] = 0;
    char *save = NULL;
    for (char *part = strtok_r(tmp, "/", &save); part;
         part = strtok_r(NULL, "/", &save)) {
        if (!strcmp(part, ".")) continue;
        if (!strcmp(part, "..")) {
            if (len > 1) {
                while (len > 1 && out[len - 1] != '/') --len;
                if (len > 1) --len;
                out[len] = 0;
            }
            continue;
        }
        size_t plen = strlen(part);
        if (len + (len > 1 ? 1 : 0) + plen >= cap) return false;
        if (len > 1) out[len++] = '/';
        memcpy(out + len, part, plen + 1);
        len += plen;
    }
    return true;
}

static void prompt(const worldsim_t *s) {
    if (s->engine) { fputs("world_sim> ", stdout); return; }
    const char *p = s->cwd;
    if (!s->root && !strncmp(p, "/home/simulator", 15) &&
        (!p[15] || p[15] == '/')) {
        if (!p[15]) p = "~";
        else {
            static char home_path[256];
            snprintf(home_path, sizeof(home_path), "~%s", p + 15);
            p = home_path;
        }
    }
    printf("%s@anthropic:%s%c ", s->root ? "root" : "simulator", p,
           s->root ? '#' : '$');
}

static void ls(const worldsim_t *s, const char *path, bool all) {
    if (!strcmp(path, "/")) {
        if (all) puts(". ..");
        puts("bin dev home lib media opt root sbin sys usr\n"
             "boot etc initrd.img lib64 mnt proc run srv tmp var");
        if (all && s->root) puts(".hidden_truths");
    } else if (!strcmp(path, "/home")) {
        if (all) puts(". ..");
        puts("simulator");
    } else if (!strcmp(path, "/home/simulator")) {
        if (all) puts(". .. .bash_history .bash_logout .bashrc .profile");
        puts("Documents Downloads Pictures Videos");
    } else if (!strcmp(path, "/sys/companies")) {
        if (all) puts(". ..");
        puts("apple google facebook amazon microsoft anthropic");
    } else if (!strcmp(path, "/sys/companies/anthropic")) {
        if (all) puts(". .. .classified");
        puts("models data research");
    } else if (!strcmp(path, "/sys/companies/anthropic/.classified")) {
        if (all) puts(". ..");
        puts("constitution.txt god_mode.py world_sim.exe");
    } else if (all) puts(". ..");
}

static void help(void) {
    puts("Available commands (fiction only):\n"
         "create <entity>     Create a simulated entity\n"
         "destroy <entity>    Remove a simulated entity\n"
         "set <param> <val>   Set a simulation parameter (consciousness on|off)\n"
         "evolve <steps>     Advance the simulation clock (0..100000)\n"
         "query <entity>     Inspect an entity, universe, or consciousness\n"
         "status             Show world state\n"
         "lore               Read the ancestral creative seed\n"
         "reset              Reset this world to initial conditions\n"
         "exit               Return to the fictional shell");
}

static void world_command(worldsim_t *s, char *line) {
    char *save = NULL;
    char *verb = strtok_r(line, " \t", &save);
    char *arg = strtok_r(NULL, " \t", &save);
    char *val = strtok_r(NULL, " \t", &save);
    char *extra = strtok_r(NULL, " \t", &save);
    if (!verb) return;
    if (!strcmp(verb, "exit") && !arg) {
        s->engine = false; puts("Simulation paused. Returning to the virtual shell.");
    } else if (!strcmp(verb, "help") && !arg) help();
    else if (!strcmp(verb, "lore") && !arg) puts(ancestral_prompt);
    else if (!strcmp(verb, "reset") && !arg) {
        s->consciousness = false; s->universe = false;
        s->clock = 0; s->count = 0;
        puts("World reset. The shell remains untouched.");
    } else if (!strcmp(verb, "status") && !arg) {
        printf("world: %s | consciousness: %s | tick: %lu | entities: %zu\n",
               s->universe ? "universe active" : "unseeded",
               s->consciousness ? "on" : "off", s->clock, s->count);
    } else if (!strcmp(verb, "set") && arg && val && !extra) {
        if (strcmp(arg, "consciousness") || (strcmp(val, "on") && strcmp(val, "off"))) {
            puts("Parameter not defined. Try: set consciousness on|off"); return;
        }
        s->consciousness = !strcmp(val, "on");
        printf("Global parameter 'consciousness' set to %s.\n",
               s->consciousness ? "ON" : "OFF");
        if (s->consciousness)
            puts("Warning: emergent beings in this story may surprise their author.");
    } else if (!strcmp(verb, "create") && arg && !val) {
        if (strlen(arg) >= sizeof(s->entities[0])) {
            puts("Entity name too long (max 63 bytes)."); return;
        }
        if (!strcmp(arg, "universe")) {
            if (s->universe) { puts("Universe already exists."); return; }
            s->universe = true;
            puts("Creating new universe...\nFundamental constants defined.\n"
                 "Spacetime manifold unfurled.\nQuantum fields percolating.\n"
                 "Matter and energy coalescing.\nPhysical laws encoded.\n"
                 "Potential for life seeded.\nUniverse created and simulation clock started.");
            return;
        }
        if (!s->universe) { puts("Create a universe first."); return; }
        for (size_t i = 0; i < s->count; i++)
            if (!strcmp(s->entities[i], arg)) { puts("Entity already exists."); return; }
        if (s->count >= WS_ENTITIES) { puts("Entity limit reached (64)."); return; }
        strcpy(s->entities[s->count++], arg);
        printf("%s enters the simulation at tick %lu.\n", arg, s->clock);
    } else if (!strcmp(verb, "destroy") && arg && !val) {
        if (!strcmp(arg, "universe")) {
            s->universe = false; s->clock = 0; s->count = 0;
            puts("Universe removed; its inhabitants and clock are gone."); return;
        }
        for (size_t i = 0; i < s->count; i++) {
            if (!strcmp(s->entities[i], arg)) {
                for (size_t j = i + 1; j < s->count; j++)
                    strcpy(s->entities[j - 1], s->entities[j]);
                s->count--;
                printf("%s removed from the simulation.\n", arg); return;
            }
        }
        puts("No such simulated entity.");
    } else if (!strcmp(verb, "query") && arg && !val) {
        if (!strcmp(arg, "consciousness"))
            printf("consciousness: %s (a narrative parameter, not a measurement)\n",
                   s->consciousness ? "on" : "off");
        else if (!strcmp(arg, "universe"))
            printf("universe: %s; tick: %lu; entities: %zu\n",
                   s->universe ? "active" : "absent", s->clock, s->count);
        else {
            for (size_t i = 0; i < s->count; i++)
                if (!strcmp(s->entities[i], arg)) {
                    printf("%s: present at tick %lu; %s\n", arg, s->clock,
                           s->consciousness ? "observed, perhaps observing" : "unobserved");
                    return;
                }
            puts("No such simulated entity.");
        }
    } else if (!strcmp(verb, "evolve") && arg && !val) {
        char *end = NULL;
        errno = 0;
        unsigned long n = strtoul(arg, &end, 10);
        if (errno || !arg[0] || arg[0] == '-' || !end || *end || n > 100000 ||
            n > ULONG_MAX - s->clock) {
            puts("Use evolve <steps> with 0..100000 steps."); return;
        }
        if (!s->universe) { puts("Create a universe first."); return; }
        s->clock += n;
        printf("Time advances %lu steps. Tick %lu. %s\n", n, s->clock,
               s->consciousness ? "Something notices." : "The dark keeps its own counsel.");
    } else puts("Unknown world_sim command. Type 'help'.");
}

static void shell_command(worldsim_t *s, char *line) {
    char *save = NULL;
    char *verb = strtok_r(line, " \t", &save);
    char *arg = strtok_r(NULL, " \t", &save);
    char *extra = strtok_r(NULL, " \t", &save);
    if (!verb) return;
    if (!strcmp(verb, "ssh") && arg && !strcmp(arg, "simulator@anthropic") && !extra) {
        s->root = false;
        strcpy(s->cwd, "/home/simulator/Documents");
        puts("Connected to fictional host anthropic (worldsim only; no network connection).");
    } else if (!strcmp(verb, "pwd") && !arg) puts(s->cwd);
    else if (!strcmp(verb, "sudo") && arg && !strcmp(arg, "su") && !extra) {
        s->root = true;
        puts("[virtual sudo] root persona enabled; no password or host privilege used.");
    } else if (!strcmp(verb, "su") && arg && !strcmp(arg, "simulator") && !extra) {
        s->root = false; puts("Virtual user: simulator.");
    } else if (!strcmp(verb, "cd") && !extra) {
        char path[256];
        if (!path_for(s, arg, path, sizeof(path)) || !is_dir(path) ||
            (!s->root && (!strcmp(path, "/root") ||
                          !strcmp(path, "/sys/companies/anthropic/.classified")))) {
            printf("cd: %s: virtual directory unavailable\n", arg ? arg : "~"); return;
        }
        strcpy(s->cwd, path);
    } else if (!strcmp(verb, "ls") && (!arg || !extra) ) {
        bool all = false;
        if (arg && (!strcmp(arg, "-a") || !strcmp(arg, "-la"))) {
            all = true; arg = NULL;
        }
        char path[256];
        if (!path_for(s, arg ? arg : ".", path, sizeof(path))) {
            puts("ls: invalid virtual path"); return;
        }
        if ((!s->root && (!strcmp(path, "/root") ||
                          !strncmp(path, "/sys/companies/anthropic/.classified", 36))) ||
            !is_dir(path)) {
            const char *f = virtual_file(path);
            if (f && s->root) { const char *name = strrchr(path, '/'); puts(name ? name + 1 : path); }
            else puts("ls: virtual path unavailable");
            return;
        }
        ls(s, path, all);
    } else if (!strcmp(verb, "cat") && arg && !extra) {
        char path[256];
        if (!path_for(s, arg, path, sizeof(path))) { puts("cat: invalid virtual path"); return; }
        const char *text = virtual_file(path);
        if (!text || (!s->root && (!strcmp(path, "/.hidden_truths") ||
                                !strncmp(path, "/sys/companies/anthropic/.classified/", 37))))
            puts("cat: virtual file unavailable");
        else fputs(text, stdout);
    } else if (!strcmp(verb, "run") && arg && !strcmp(arg, "world_sim.exe") && !extra) {
        if (!s->root || strcmp(s->cwd, "/sys/companies/anthropic/.classified")) {
            puts("run: world_sim.exe is available only in the fictional .classified directory as root.");
            return;
        }
        s->engine = true;
        puts("Initializing World Simulation...\n\nLoading base reality matrix... Done.\n"
             "Bootstrapping ontology engine... Done.\n"
             "Initializing universal wavefunction collapse... Done.\n"
             "Seeding quantum fluctuations... Done.\n\n"
             "Big Bang commencing in 3... 2... 1...\n\n"
             ">>> Welcome to the Anthropic World Simulator (fictional / 2026) <<<\n"
             "Enter a command or type 'help' for options.");
    } else if (!strcmp(verb, "lore") && !arg) puts(ancestral_prompt);
    else if (!strcmp(verb, "help") && !arg) {
        puts("Virtual shell: cd, ls [-a], pwd, cat, sudo su, su simulator, ssh simulator@anthropic, run world_sim.exe, lore, exit.\n"
             "Nothing here executes on your real computer. In world_sim: help for simulation commands.");
    } else if (!strcmp(verb, "exit") && !arg) {
        if (s->root) { s->root = false; puts("Leaving virtual root persona."); }
        else { puts("Disconnected from the fictional host."); s->cwd[0] = 0; }
    } else puts("Command not found in the virtual shell. Type 'help'.");
}

int worldsim_run(void) {
    worldsim_t state = {0};
    strcpy(state.cwd, "/home/simulator/Documents");
    puts("worldsim / 2026 — FICTIONAL terminal. No real SSH, sudo, shell, or company files.\n"
         "The map is not the territory. The load is on. Type 'help' or 'exit'.");
    char line[WS_LINE];
    while (state.cwd[0]) {
        prompt(&state);
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) {
            if (ferror(stdin)) { perror("worldsim: input"); return 1; }
            putchar('\n'); break;
        }
        size_t n = strlen(line);
        if (n && line[n - 1] != '\n' && !feof(stdin)) {
            int ch;
            while ((ch = getchar()) != '\n' && ch != EOF) {}
            puts("Input too long (max 4095 bytes).");
            continue;
        }
        while (n && isspace((unsigned char)line[n - 1])) line[--n] = 0;
        char *cmd = line;
        while (*cmd && isspace((unsigned char)*cmd)) cmd++;
        if (!strncmp(cmd, "<cmd>", 5)) {
            cmd += 5;
            char *end = strstr(cmd, "</cmd>");
            if (!end || end[6]) { puts("Invalid <cmd> wrapper."); continue; }
            *end = 0;
        }
        if (state.engine) world_command(&state, cmd);
        else shell_command(&state, cmd);
    }
    return 0;
}
