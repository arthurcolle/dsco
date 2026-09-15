#include "watchdog.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <spawn.h>
#include <stdint.h>

extern char **environ;

/* ── helpers ────────────────────────────────────────────────────────────── */

static int mkdirs(const char *path) {
    char tmp[4096];
    int n = snprintf(tmp, sizeof(tmp), "%s", path ? path : "");
    if (n < 0 || (size_t)n >= sizeof(tmp))
        return -1;
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0700);
            *p = '/';
        }
    }
    return mkdir(tmp, 0700) == 0 || errno == EEXIST ? 0 : -1;
}

static const char *self_path(void) {
    static char buf[4096];
    if (buf[0])
        return buf;
#ifdef __APPLE__
    uint32_t sz = sizeof(buf);
    extern int _NSGetExecutablePath(char *, uint32_t *);
    _NSGetExecutablePath(buf, &sz);
#else
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0)
        buf[n] = '\0';
#endif
    return buf;
}

static const char *home(void) {
    const char *h = getenv("HOME");
    return h ? h : "/tmp";
}

/* Service-manager inputs ultimately originate in CLI/configuration data.
 * Keep them out of a shell entirely; quoting a shell command is not a safe
 * substitute because labels and paths are also written into service files. */
static int run_silent_argv(const char *const argv[]) {
    if (!argv || !argv[0])
        return -1;

    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions) != 0)
        return -1;
    int action_rc = posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null",
                                                      O_WRONLY, 0);
    if (action_rc == 0)
        action_rc = posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null",
                                                      O_WRONLY, 0);
    pid_t pid = -1;
    int spawn_rc = action_rc == 0
                       ? posix_spawnp(&pid, argv[0], &actions, NULL,
                                      (char *const *)argv, environ)
                       : action_rc;
    posix_spawn_file_actions_destroy(&actions);
    if (spawn_rc != 0)
        return -1;

    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR)
            return -1;
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}

/* Capture a bounded amount of manager output without a shell. The pipe is
 * drained even after the caller's buffer fills so a verbose manager cannot
 * deadlock before waitpid(). Return its exit status, or -1 on spawn/I/O error. */
static int run_capture_argv(const char *const argv[], char *buf, size_t buf_len) {
    if (!argv || !argv[0] || !buf || buf_len == 0)
        return -1;
    buf[0] = '\0';

    int pipefd[2];
    if (pipe(pipefd) != 0)
        return -1;
    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions) != 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }
    int action_rc = posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDOUT_FILENO);
    if (action_rc == 0)
        action_rc = posix_spawn_file_actions_addclose(&actions, pipefd[0]);
    if (action_rc == 0)
        action_rc = posix_spawn_file_actions_addclose(&actions, pipefd[1]);
    if (action_rc == 0)
        action_rc = posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null",
                                                      O_WRONLY, 0);
    pid_t pid = -1;
    int spawn_rc = action_rc == 0
                       ? posix_spawnp(&pid, argv[0], &actions, NULL,
                                      (char *const *)argv, environ)
                       : action_rc;
    posix_spawn_file_actions_destroy(&actions);
    close(pipefd[1]);
    if (spawn_rc != 0) {
        close(pipefd[0]);
        return -1;
    }

    size_t used = 0;
    char scratch[512];
    for (;;) {
        char *dst = used + 1 < buf_len ? buf + used : scratch;
        size_t cap = used + 1 < buf_len ? buf_len - used - 1 : sizeof(scratch);
        ssize_t n = read(pipefd[0], dst, cap);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        if (dst == buf + used)
            used += (size_t)n;
    }
    close(pipefd[0]);
    buf[used] = '\0';

    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR)
            return -1;
    }
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    return 128;
}

static int valid_label(const char *label) {
    if (!label || !label[0] || strlen(label) >= 256 || strcmp(label, ".") == 0 ||
        strcmp(label, "..") == 0)
        return 0;
    for (const unsigned char *p = (const unsigned char *)label; *p; p++) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '.' || *p == '_' || *p == '-' || *p == '@'))
            return 0;
    }
    return 1;
}

/* ── macOS launchd ──────────────────────────────────────────────────────── */

#ifdef __APPLE__

static int plist_path(const char *label, char *out, size_t len) {
    int n = snprintf(out, len, "%s/Library/LaunchAgents/%s.plist", home(), label);
    return n >= 0 && (size_t)n < len ? 0 : -1;
}

static int plist_string(FILE *f, const char *value) {
    if (!f || !value)
        return -1;
    for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
        if (*p < 0x20 && *p != '\t' && *p != '\n' && *p != '\r')
            return -1;
        const char *escaped = NULL;
        if (*p == '&') escaped = "&amp;";
        else if (*p == '<') escaped = "&lt;";
        else if (*p == '>') escaped = "&gt;";
        if (escaped) {
            if (fputs(escaped, f) == EOF)
                return -1;
        } else if (fputc(*p, f) == EOF) {
            return -1;
        }
    }
    return 0;
}

static int write_plist(const char *label, const char *args[], int argc) {
    char path[4096];
    if (!valid_label(label) || argc < 0 || argc > 128 || plist_path(label, path, sizeof(path)) != 0)
        return -1;

    /* ensure LaunchAgents dir exists */
    char dir[4096];
    int n = snprintf(dir, sizeof(dir), "%s/Library/LaunchAgents", home());
    if (n < 0 || (size_t)n >= sizeof(dir) || mkdirs(dir) != 0)
        return -1;

    char tmp_path[4096];
    n = snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.XXXXXX", path);
    if (n < 0 || (size_t)n >= sizeof(tmp_path))
        return -1;
    int fd = mkstemp(tmp_path);
    if (fd < 0)
        return -1;
    fchmod(fd, 0600);
    FILE *f = fdopen(fd, "w");
    if (!f) {
        close(fd);
        unlink(tmp_path);
        return -1;
    }

    int ok = fprintf(f,
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\"\n"
            "  \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
            "<plist version=\"1.0\">\n<dict>\n"
            "  <key>Label</key><string>");
    if (ok >= 0) {
        if (plist_string(f, label) != 0)
            ok = -1;
        else
            ok = fprintf(f, "</string>\n  <key>ProgramArguments</key>\n  <array>\n"
                            "    <string>");
    }
    if (ok >= 0) {
        if (!self_path()[0] || plist_string(f, self_path()) != 0)
            ok = -1;
        else
            ok = fprintf(f, "</string>\n");
    }

    for (int i = 0; ok >= 0 && i < argc; i++) {
        if (!args || !args[i] || fprintf(f, "    <string>") < 0 ||
            plist_string(f, args[i]) != 0 || fprintf(f, "</string>\n") < 0)
            ok = -1;
    }

    if (ok >= 0 && fprintf(f,
            "  </array>\n"
            "  <key>KeepAlive</key><true/>\n"
            "  <key>RunAtLoad</key><true/>\n"
            "  <key>StandardOutPath</key><string>") < 0)
        ok = -1;
    if (ok >= 0 && plist_string(f, home()) == 0 &&
        fprintf(f, "/.dsco/daemon.log</string>\n"
                "  <key>StandardErrorPath</key><string>") >= 0 &&
        plist_string(f, home()) == 0 &&
        fprintf(f, "/.dsco/daemon.err</string>\n"
                "  <key>ThrottleInterval</key><integer>10</integer>\n"
                "</dict>\n</plist>\n") >= 0) {
        /* complete */
    } else if (ok >= 0) {
        ok = -1;
    }

    if (ok >= 0 && (fflush(f) != 0 || ferror(f)))
        ok = -1;
    if (fclose(f) != 0)
        ok = -1;
    if (ok >= 0 && rename(tmp_path, path) == 0)
        return 0;
    unlink(tmp_path);
    return -1;
}

int watchdog_install(const char *label, const char **args, int argc) {
    if (!label)
        label = WATCHDOG_DEFAULT_LABEL;
    if (write_plist(label, args, argc) != 0)
        return -1;

    char path[4096];
    if (plist_path(label, path, sizeof(path)) != 0)
        return -1;

    /* unload first in case it's already loaded (ignore error) */
    const char *unload[] = {"launchctl", "unload", "-w", path, NULL};
    run_silent_argv(unload);

    const char *load[] = {"launchctl", "load", "-w", path, NULL};
    return run_silent_argv(load);
}

int watchdog_uninstall(const char *label) {
    if (!label)
        label = WATCHDOG_DEFAULT_LABEL;
    char path[4096];
    if (!valid_label(label) || plist_path(label, path, sizeof(path)) != 0)
        return -1;

    const char *unload[] = {"launchctl", "unload", "-w", path, NULL};
    run_silent_argv(unload);
    return remove(path);
}

int watchdog_status(const char *label, char *buf, size_t buf_len) {
    if (!label)
        label = WATCHDOG_DEFAULT_LABEL;
    if (!buf || !buf_len || !valid_label(label))
        return -1;

    const char *list[] = {"launchctl", "list", label, NULL};
    char output[4096];
    int rc = run_capture_argv(list, output, sizeof(output));
    if (rc < 0) {
        snprintf(buf, buf_len, "unknown");
        return -1;
    }
    snprintf(buf, buf_len, rc == 0 ? (strstr(output, "\"PID\"") ? "running" : "loaded")
                                   : "stopped");
    return 0;
}

#else /* Linux / systemd */

static int unit_path(const char *label, char *out, size_t len) {
    int n = snprintf(out, len, "%s/.config/systemd/user/%s.service", home(), label);
    return n >= 0 && (size_t)n < len ? 0 : -1;
}

/* Quote one systemd ExecStart token and neutralize systemd's own expansion
 * syntax. This is deliberately stricter than shell quoting: '%' and '$' are
 * meaningful to systemd even when no shell is involved. */
static int systemd_token(FILE *f, const char *value) {
    if (!f || !value || fputc('"', f) == EOF)
        return -1;
    for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
        const char *escaped = NULL;
        if (*p == '\\') escaped = "\\\\";
        else if (*p == '"') escaped = "\\\"";
        else if (*p == '$') escaped = "\\$";
        else if (*p == '%') escaped = "%%";
        else if (*p == '\n') escaped = "\\n";
        else if (*p == '\r') escaped = "\\r";
        else if (*p == '\t') escaped = "\\t";
        if (*p < 0x20 && !escaped) {
            if (fprintf(f, "\\x%02x", *p) < 0)
                return -1;
        } else if (escaped) {
            if (fputs(escaped, f) == EOF)
                return -1;
        } else if (fputc(*p, f) == EOF) {
            return -1;
        }
    }
    return fputc('"', f) == EOF ? -1 : 0;
}

static int write_unit(const char *label, const char *args[], int argc) {
    char path[4096];
    if (!valid_label(label) || argc < 0 || argc > 128 || unit_path(label, path, sizeof(path)) != 0)
        return -1;

    char dir[4096];
    int n = snprintf(dir, sizeof(dir), "%s/.config/systemd/user", home());
    if (n < 0 || (size_t)n >= sizeof(dir) || mkdirs(dir) != 0)
        return -1;

    char tmp_path[4096];
    n = snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.XXXXXX", path);
    if (n < 0 || (size_t)n >= sizeof(tmp_path))
        return -1;
    int fd = mkstemp(tmp_path);
    if (fd < 0)
        return -1;
    fchmod(fd, 0600);
    FILE *f = fdopen(fd, "w");
    if (!f) {
        close(fd);
        unlink(tmp_path);
        return -1;
    }

    int ok = fprintf(f, "[Unit]\nDescription=dsco distributed agent daemon\n"
                        "After=network.target\n\n[Service]\nType=simple\nExecStart=") >= 0;
    if (!(ok && self_path()[0] && systemd_token(f, self_path()) == 0))
        ok = 0;
    for (int i = 0; ok && i < argc; i++) {
        if (!args || !args[i] || fputc(' ', f) == EOF || systemd_token(f, args[i]) != 0)
            ok = 0;
    }
    if (ok && fputc('\n', f) == EOF)
        ok = 0;
    if (ok && fprintf(f, "\nRestart=always\nRestartSec=10\n") < 0)
        ok = 0;
    char outlog[4096], errlog[4096];
    n = snprintf(outlog, sizeof(outlog), "append:%s/.dsco/daemon.log", home());
    if (n < 0 || (size_t)n >= sizeof(outlog)) ok = 0;
    n = snprintf(errlog, sizeof(errlog), "append:%s/.dsco/daemon.err", home());
    if (n < 0 || (size_t)n >= sizeof(errlog)) ok = 0;
    if (ok && fprintf(f, "StandardOutput=") >= 0 && systemd_token(f, outlog) == 0 &&
        fprintf(f, "\nStandardError=") >= 0 && systemd_token(f, errlog) == 0 &&
        fprintf(f, "\n\n[Install]\nWantedBy=default.target\n") >= 0) {
        /* complete */
    } else {
        ok = 0;
    }
    if (ok && (fflush(f) != 0 || ferror(f)))
        ok = 0;
    if (fclose(f) != 0)
        ok = 0;
    if (ok && rename(tmp_path, path) == 0)
        return 0;
    unlink(tmp_path);
    return -1;
}

int watchdog_install(const char *label, const char **args, int argc) {
    if (!label)
        label = WATCHDOG_DEFAULT_LABEL;
    if (write_unit(label, args, argc) != 0)
        return -1;

    const char *reload[] = {"systemctl", "--user", "daemon-reload", NULL};
    if (run_silent_argv(reload) != 0)
        return -1;
    const char *enable[] = {"systemctl", "--user", "enable", "--now", label, NULL};
    return run_silent_argv(enable);
}

int watchdog_uninstall(const char *label) {
    if (!label)
        label = WATCHDOG_DEFAULT_LABEL;
    if (!valid_label(label))
        return -1;
    const char *disable[] = {"systemctl", "--user", "disable", "--now", label, NULL};
    run_silent_argv(disable);
    char path[4096];
    if (unit_path(label, path, sizeof(path)) != 0)
        return -1;
    return remove(path);
}

int watchdog_status(const char *label, char *buf, size_t buf_len) {
    if (!label)
        label = WATCHDOG_DEFAULT_LABEL;
    if (!buf || !buf_len || !valid_label(label))
        return -1;
    const char *active[] = {"systemctl", "--user", "is-active", label, NULL};
    char output[256];
    int rc = run_capture_argv(active, output, sizeof(output));
    if (rc < 0) {
        snprintf(buf, buf_len, "unknown");
        return -1;
    }
    snprintf(buf, buf_len, rc == 0 && strncmp(output, "active", 6) == 0 ? "running" : "stopped");
    return 0;
}

#endif /* __APPLE__ */

/* ── heartbeat ping (cross-platform) ───────────────────────────────────── */

void watchdog_ping(void) {
    char path[4096];
    snprintf(path, sizeof(path), "%s/.dsco/watchdog.ping", home());

    /* ensure .dsco dir */
    char dir[4096];
    snprintf(dir, sizeof(dir), "%s/.dsco", home());
    mkdirs(dir);

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
        return;

    char ts[64];
    time_t now = time(NULL);
    int n = snprintf(ts, sizeof(ts), "%ld\n", (long)now);
    (void)write(fd, ts, (size_t)n);
    close(fd);
}
