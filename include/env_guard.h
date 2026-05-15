#ifndef DSCO_ENV_GUARD_H
#define DSCO_ENV_GUARD_H

/* ── Environment & Hook Guard ─────────────────────────────────────────────
 *
 * Detects and neutralises environmental attack vectors before any secrets
 * are accessed:
 *
 *  1. LD_PRELOAD / DYLD_INSERT_LIBRARIES sanitiser
 *     — strips any injected dynamic libraries from the environment.
 *     — logs which libraries were present so the event is auditable.
 *
 *  2. Unsafe-environment detection
 *     — flags suspicious variables: MALLOC_CHECK_, MALLOC_PERTURB_,
 *       DYLD_FORCE_FLAT_NAMESPACE, DYLD_IMAGE_SUFFIX, LD_AUDIT, etc.
 *
 *  3. dlopen canary
 *     — on macOS, checks dyld image list for unexpected shared libraries
 *       that appeared after the main binary loaded (i.e., were injected).
 *
 * Usage:
 *   env_guard_init();   — call once, early in main() after sealed_store_init
 *
 * Returns a bitmask of what was found; 0 means clean.
 * ─────────────────────────────────────────────────────────────────────── */

#include <stdbool.h>
#include <stdint.h>

#define ENVG_PRELOAD_FOUND    (1u << 0)
#define ENVG_SUSPICIOUS_VAR   (1u << 1)
#define ENVG_INJECTED_DYLIB   (1u << 2)
#define ENVG_PTRACE_DETECTED  (1u << 3)

/* Run all checks.  Strips dangerous env vars.  Returns bitmask (0 = clean).
 * If any flag is set the event is written to the audit log and to stderr.
 * Does NOT terminate — caller decides whether to continue or panic. */
uint32_t env_guard_init(void);

/* Re-check for newly injected libraries (call periodically or after fork). */
bool env_guard_check_dylibs(void);

#endif /* DSCO_ENV_GUARD_H */
