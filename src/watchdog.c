#include "watchdog.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <sys/stat.h>
#include <stdint.h>

/* ── helpers ────────────────────────────────────────────────────────────── */

static int mkdirs(const char *path) {
    char tmp[4096];
    snprintf(tmp, sizeof(tmp), "%s", path);
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
    if (buf[0]) return buf;
#ifdef __APPLE__
    uint32_t sz = sizeof(buf);
    extern int _NSGetExecutablePath(char *, uint32_t *);
    _NSGetExecutablePath(buf, &sz);
#else
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) buf[n] = '\0';
#endif
    return buf;
}

static const char *home(void) {
    const char *h = getenv("HOME");
    return h ? h : "/tmp";
}

static int run_silent(const char *cmd) {
    char full[4096];
    snprintf(full, sizeof(full), "%s >/dev/null 2>&1", cmd);
    return system(full);
}

/* ── macOS launchd ──────────────────────────────────────────────────────── */

#ifdef __APPLE__

static void plist_path(const char *label, char *out, size_t len) {
    snprintf(out, len, "%s/Library/LaunchAgents/%s.plist", home(), label);
}

static int write_plist(const char *label, const char *args[], int argc) {
    char path[4096];
    plist_path(label, path, sizeof(path));

    /* ensure LaunchAgents dir exists */
    char dir[4096];
    snprintf(dir, sizeof(dir), "%s/Library/LaunchAgents", home());
    mkdirs(dir);

    FILE *f = fopen(path, "w");
    if (!f) return -1;

    fprintf(f,
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\"\n"
        "  \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
        "<plist version=\"1.0\">\n<dict>\n"
        "  <key>Label</key><string>%s</string>\n"
        "  <key>ProgramArguments</key>\n  <array>\n"
        "    <string>%s</string>\n",
        label, self_path());

    for (int i = 0; i < argc && args && args[i]; i++)
        fprintf(f, "    <string>%s</string>\n", args[i]);

    fprintf(f,
        "  </array>\n"
        "  <key>KeepAlive</key><true/>\n"
        "  <key>RunAtLoad</key><true/>\n"
        "  <key>StandardOutPath</key>"
        "<string>%s/.dsco/daemon.log</string>\n"
        "  <key>StandardErrorPath</key>"
        "<string>%s/.dsco/daemon.err</string>\n"
        "  <key>ThrottleInterval</key><integer>10</integer>\n"
        "</dict>\n</plist>\n",
        home(), home());

    fclose(f);
    return 0;
}

int watchdog_install(const char *label, const char **args, int argc) {
    if (!label) label = WATCHDOG_DEFAULT_LABEL;
    if (write_plist(label, args, argc) != 0) return -1;

    char path[4096];
    plist_path(label, path, sizeof(path));

    char cmd[8192];
    /* unload first in case it's already loaded (ignore error) */
    snprintf(cmd, sizeof(cmd), "launchctl unload '%s'", path);
    run_silent(cmd);

    snprintf(cmd, sizeof(cmd), "launchctl load -w '%s'", path);
    return run_silent(cmd);
}

int watchdog_uninstall(const char *label) {
    if (!label) label = WATCHDOG_DEFAULT_LABEL;
    char path[4096];
    plist_path(label, path, sizeof(path));

    char cmd[8192];
    snprintf(cmd, sizeof(cmd), "launchctl unload -w '%s'", path);
    run_silent(cmd);
    return remove(path);
}

int watchdog_status(const char *label, char *buf, size_t buf_len) {
    if (!label) label = WATCHDOG_DEFAULT_LABEL;
    if (!buf || !buf_len) return -1;

    char cmd[512];
    snprintf(cmd, sizeof(cmd),
        "launchctl list '%s' 2>/dev/null | grep -q '\"PID\"' && echo running "
        "|| launchctl list '%s' 2>/dev/null | grep -q Label && echo loaded "
        "|| echo stopped",
        label, label);

    FILE *p = popen(cmd, "r");
    if (!p) { snprintf(buf, buf_len, "unknown"); return -1; }
    if (!fgets(buf, (int)buf_len, p)) snprintf(buf, buf_len, "unknown");
    else {
        /* strip trailing newline */
        size_t n = strlen(buf);
        if (n && buf[n-1] == '\n') buf[n-1] = '\0';
    }
    pclose(p);
    return 0;
}

#else /* Linux / systemd */

static void unit_path(const char *label, char *out, size_t len) {
    snprintf(out, len, "%s/.config/systemd/user/%s.service", home(), label);
}

static int write_unit(const char *label, const char *args[], int argc) {
    char path[4096];
    unit_path(label, path, sizeof(path));

    char dir[4096];
    snprintf(dir, sizeof(dir), "%s/.config/systemd/user", home());
    mkdirs(dir);

    FILE *f = fopen(path, "w");
    if (!f) return -1;

    char execline[8192];
    int off = snprintf(execline, sizeof(execline), "ExecStart=%s", self_path());
    for (int i = 0; i < argc && args && args[i]; i++)
        off += snprintf(execline + off, sizeof(execline) - (size_t)off,
                        " %s", args[i]);

    fprintf(f,
        "[Unit]\nDescription=dsco distributed agent daemon\nAfter=network.target\n\n"
        "[Service]\nType=simple\n%s\nRestart=always\nRestartSec=10\n"
        "StandardOutput=append:%s/.dsco/daemon.log\n"
        "StandardError=append:%s/.dsco/daemon.err\n\n"
        "[Install]\nWantedBy=default.target\n",
        execline, home(), home());

    fclose(f);
    return 0;
}

int watchdog_install(const char *label, const char **args, int argc) {
    if (!label) label = WATCHDOG_DEFAULT_LABEL;
    if (write_unit(label, args, argc) != 0) return -1;

    char cmd[512];
    snprintf(cmd, sizeof(cmd),
        "systemctl --user daemon-reload && "
        "systemctl --user enable --now '%s'", label);
    return run_silent(cmd);
}

int watchdog_uninstall(const char *label) {
    if (!label) label = WATCHDOG_DEFAULT_LABEL;
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
        "systemctl --user disable --now '%s'", label);
    run_silent(cmd);
    char path[4096];
    unit_path(label, path, sizeof(path));
    return remove(path);
}

int watchdog_status(const char *label, char *buf, size_t buf_len) {
    if (!label) label = WATCHDOG_DEFAULT_LABEL;
    if (!buf || !buf_len) return -1;
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
        "systemctl --user is-active '%s' 2>/dev/null", label);
    FILE *p = popen(cmd, "r");
    if (!p) { snprintf(buf, buf_len, "unknown"); return -1; }
    if (!fgets(buf, (int)buf_len, p)) snprintf(buf, buf_len, "unknown");
    else {
        size_t n = strlen(buf);
        if (n && buf[n-1] == '\n') buf[n-1] = '\0';
    }
    pclose(p);
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
    if (fd < 0) return;

    char ts[64];
    time_t now = time(NULL);
    int n = snprintf(ts, sizeof(ts), "%ld\n", (long)now);
    (void)write(fd, ts, (size_t)n);
    close(fd);
}
