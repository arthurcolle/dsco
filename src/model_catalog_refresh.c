#define _GNU_SOURCE 1
#define _DARWIN_C_SOURCE 1
#include "model_catalog_refresh.h"
#include "model_pricing.h"
#include "deepseek_pricing.h"
#include "openrouter_cache.h"
#include "codex_cache.h"
#include "http_pool.h"
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
extern char **environ;

static void load_cached(void) {
    openrouter_cache_load_cached();
    model_pricing_load_cached();
    deepseek_pricing_load_cached();
    codex_cache_load_cached();
}

static void *reap_refresh(void *arg) {
    pid_t pid = *(pid_t *)arg;
    free(arg);
    int status;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    load_cached();
    return NULL;
}

/* The worker owns a new process group; its watchdog also terminates a hung
 * Codex catalog command. No inference requests are made. */
static void expire_worker(int sig) {
    (void)sig;
    kill(0, SIGKILL);
    _exit(124);
}

int model_catalog_refresh_worker(void) {
    if (setsid() < 0) return 1;
    signal(SIGALRM, expire_worker);
    alarm(45);
    const char *home = getenv("HOME");
    if (!home || !*home || getenv("DSCO_PRICING_OFFLINE")) return 0;
    char path[2048];
    snprintf(path, sizeof(path), "%s/.dsco", home);
    mkdir(path, 0700);
    snprintf(path, sizeof(path), "%s/.dsco/model_catalog_refresh.lock", home);
    int fd = open(path, O_CREAT | O_RDWR, 0600);
    if (fd < 0) return 1;
    /* Simultaneous launches share the active refresh; no age-based TTL. */
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        /* Remain bounded while waiting so the parent reloads completed data. */
        if (flock(fd, LOCK_SH) != 0) { close(fd); return 1; }
        close(fd);
        return 0;
    }
    dsco_http_global_init();
    openrouter_cache_load_sync();
    model_pricing_refresh_sync();
    deepseek_pricing_refresh_sync();
    codex_cache_load_sync();
    close(fd);
    alarm(0);
    return 0;
}

void model_catalog_refresh_start(const char *executable) {
    load_cached();
    if (getenv("DSCO_PRICING_OFFLINE")) return;
    char resolved[4096];
#ifdef __APPLE__
    uint32_t size = sizeof(resolved);
    if (_NSGetExecutablePath(resolved, &size) == 0) executable = resolved;
#else
    ssize_t size = readlink("/proc/self/exe", resolved, sizeof(resolved) - 1);
    if (size > 0) { resolved[size] = 0; executable = resolved; }
#endif
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attr;
    posix_spawn_file_actions_init(&actions);
    posix_spawnattr_init(&attr);
#ifdef POSIX_SPAWN_CLOEXEC_DEFAULT
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_CLOEXEC_DEFAULT);
#else
    posix_spawn_file_actions_addclosefrom_np(&actions, 3);
#endif
    for (int fd = 0; fd < 3; fd++)
        posix_spawn_file_actions_addopen(&actions, fd, "/dev/null", O_RDWR, 0);
    char *args[] = {(char *)executable, "--internal-model-refresh", NULL};
    pid_t pid;
    int rc = posix_spawnp(&pid, executable, &actions, &attr, args, environ);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attr);
    if (rc) return;
    pid_t *saved = malloc(sizeof(*saved));
    if (!saved) return;
    *saved = pid;
    pthread_t thread;
    if (pthread_create(&thread, NULL, reap_refresh, saved) == 0)
        pthread_detach(thread);
    else free(saved);
}
