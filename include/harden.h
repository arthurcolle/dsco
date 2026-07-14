/* harden.h — runtime anti-reverse-engineering hardening.
 *
 * All entry points are always callable; the protective bodies are compiled in
 * only when built with -DDSCO_HARDENED (see `make harden`). Development builds
 * (`make`) keep full debuggability — nothing here fires.
 *
 * Layers (each independently bypassable; together they price out casual RE):
 *   1. Environment scrub  — strip DYLD_* and LD_PRELOAD/malloc-logging injectors.
 *   2. Anti-debug         — PT_DENY_ATTACH / PTRACE_TRACEME + tracer polling.
 *   3. Anti-instrument    — detect Frida/substrate/injected dylibs.
 *   4. Signature check    — verify the process is signed & unmodified (macOS).
 * On detection the process dies quietly (no attacker-useful diagnostics).
 */
#ifndef DSCO_HARDEN_H
#define DSCO_HARDEN_H

#include <stdbool.h>

/* Call once, as the very first statement in main(), before any parsing. */
void dsco_harden_init(void);

/* Cheap re-check to sprinkle on hot/sensitive paths (agent loop, key use).
 * Returns true if the environment still looks clean; on tamper it does not
 * return (process is terminated). Safe to call frequently. */
bool dsco_harden_checkpoint(void);

/* True when this build has the hardening bodies compiled in. */
bool dsco_harden_enabled(void);

#endif /* DSCO_HARDEN_H */
