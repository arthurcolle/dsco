#ifndef DSCO_TAMPER_H
#define DSCO_TAMPER_H

/* ── Tamper / Anti-analysis Guard ─────────────────────────────────────────
 *
 * Layered defensive protection:
 *
 *  1. Anti-debug      — denies ptrace attachment on macOS; detects debugger
 *                       presence via sysctl (macOS) or /proc/self/status (Linux).
 *
 *  2. Code integrity  — hashes the executable __TEXT/__text segment at startup
 *                       and re-verifies on each tamper_check() call.  Detects
 *                       breakpoint injection and in-memory code patches.
 *
 *  3. File watcher    — kqueue EVFILT_VNODE (macOS / BSD) or inotify (Linux)
 *                       watching the binary file itself.  Fires if any external
 *                       process opens, writes, or renames the file — e.g. Ghidra
 *                       loading it for analysis.
 *
 *  4. Response        — registered wipers are called first (zero API keys,
 *                       session state, etc.).  If DSCO_TAMPER_DESTRUCT=1 env var
 *                       is set, the on-disk binary is overwritten with random
 *                       bytes before exiting.
 *
 * Usage:
 *   tamper_init();                          // call once from main(), before any
 *                                           // secrets are loaded
 *   tamper_register_wiper(fn, ctx);         // register per-module secret wipers
 *   // in main loop:
 *   if (!tamper_check()) handle_tamper();   // optional periodic check
 * ─────────────────────────────────────────────────────────────────────── */

#include <stdbool.h>
#include <stddef.h>

typedef void (*tamper_wiper_fn)(void *ctx);

/* Initialise the guard.  Must be called before any secrets are loaded.
 * Installs ptrace-deny, computes the initial code hash, and starts the
 * background file-watcher thread. */
void tamper_init(void);

/* Register a callback that will be invoked when tampering is detected.
 * Use to zero out API keys, session tokens, cryptographic keys, etc. */
void tamper_register_wiper(tamper_wiper_fn fn, void *ctx);

/* Perform an on-demand integrity check.  Returns true if everything looks
 * clean; returns false (and triggers the response) if tampering is detected. */
bool tamper_check(void);

/* Immediately trigger the tamper response regardless of detection state.
 * Useful for manual "panic wipe" scenarios. */
void tamper_trigger(const char *reason);

#endif /* DSCO_TAMPER_H */
