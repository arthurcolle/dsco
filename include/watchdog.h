#ifndef DSCO_WATCHDOG_H
#define DSCO_WATCHDOG_H

/* ── Process Watchdog / Persistence ───────────────────────────────────────
 *
 * Installs dsco as a background service so it survives reboots and crash-
 * restarts without any user action.
 *
 * macOS: writes a launchd plist to ~/Library/LaunchAgents and calls
 *        launchctl load.  The service restarts on exit (KeepAlive=true).
 *
 * Linux: writes a systemd user unit to ~/.config/systemd/user/ and calls
 *        systemctl --user enable --now.
 *
 * The watchdog can run dsco in a headless "daemon" mode where it accepts
 * commands over the mesh network or the net_server HTTP interface rather
 * than through stdin/tty.
 *
 * Usage:
 *   watchdog_install(label, args, argc)  — install and load the service
 *   watchdog_uninstall(label)            — stop and remove the service
 *   watchdog_status(label, buf, buf_len) — is the service loaded/running?
 *   watchdog_ping()                      — heartbeat; restarts if overdue
 * ─────────────────────────────────────────────────────────────────────── */

#include <stdbool.h>
#include <stddef.h>

#define WATCHDOG_DEFAULT_LABEL  "systems.distributed.dsco"
#define WATCHDOG_PING_INTERVAL  30   /* seconds between heartbeat pings */

/* Install service with the calling binary's path.
 * args: additional argv to pass to the daemon (may be NULL).
 * Returns 0 on success. */
int watchdog_install(const char *label, const char **args, int argc);

/* Stop and remove the service. */
int watchdog_uninstall(const char *label);

/* Fill buf with one of: "loaded", "running", "stopped", "unknown".
 * Returns 0 if status was determined, -1 on error. */
int watchdog_status(const char *label, char *buf, size_t buf_len);

/* Write a heartbeat timestamp to a well-known file so external monitors
 * can detect stalls.  Call periodically from the main loop. */
void watchdog_ping(void);

#endif /* DSCO_WATCHDOG_H */
