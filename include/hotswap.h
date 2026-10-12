#ifndef DSCO_HOTSWAP_H
#define DSCO_HOTSWAP_H

/* hotswap.h — atomic, reversible runtime code-swapping for dsco binaries.
 *
 * Builds on plugin.c's per-module load/unload + manifest trust (hash + signer
 * + capabilities). plugin_reload() swaps the *entire* registry — too coarse
 * for hotswapping one module while others keep running. This module adds the
 * orchestration the runtime needs to hotswap a single module safely:
 *
 *   1. SNAPSHOT — capture the currently-loaded module (name, version, path,
 *      file hash) so the swap is reversible.
 *   2. VERIFY   — the candidate must pass the same manifest trust (sha256 of
 *      the file must match the manifest hash; signer must be allowed) before
 *      anything is unloaded. Never unload the good module on an unverified
 *      candidate.
 *   3. SWAP     — atomically: load the candidate, and only on success unload
 *      the old one. If the candidate fails to load, the old module stays.
 *   4. ROLLBACK — a one-call revert to the snapshot if the swapped module
 *      misbehaves. The snapshot is the last-known-good.
 *
 * Governance: every swap writes a bounded receipt (schema dsco.hotswap.v1)
 * with before/after hashes, the verification outcome, and whether a rollback
 * is available. Hotswap loads executable code — it requires the control
 * capability (CAP_CONTROL) and, for the verify step, CAP_SECRETS when a signed
 * manifest is involved. Untrusted/unsigned candidates are rejected outright.
 *
 * This module does NOT itself dlopen — it drives plugin.c's load/unload and
 * computes/verifies file hashes. Pure orchestration over the existing,
 * already-governed plugin machinery. */

#include <stdbool.h>
#include <stddef.h>

#include "plugin.h"

#define HOTSWAP_RECEIPT_MAX 1024
#define HOTSWAP_HASH_HEX 65

/* A snapshot of a loaded module, sufficient to restore it. */
typedef struct {
    char name[128];
    char version[64];
    char path[512];
    char file_sha256[HOTSWAP_HASH_HEX];   /* hash of the module file at snapshot */
    bool valid;
} hotswap_snapshot_t;

/* Compute the sha256 (hex) of a file. Returns false on I/O error. */
bool hotswap_file_sha256(const char *path, char out_hex[HOTSWAP_HASH_HEX]);

/* Snapshot the currently-loaded module `name` in the registry, so a later
 * swap can be rolled back. Returns false if the module isn't loaded. */
bool hotswap_snapshot(plugin_registry_t *reg, const char *name,
                      hotswap_snapshot_t *out);

/* Verify a candidate module file against a manifest hash (and signer when
 * required). Returns true only if the file's sha256 matches expected_hash_hex
 * (and, when require_signed, a non-empty signer is present). Pure check; does
 * not load anything. */
bool hotswap_verify_candidate(const char *candidate_path,
                              const char *expected_hash_hex,
                              bool require_signed,
                              char *err, size_t err_len);

/* Atomically swap the loaded module `name` for a new module file.
 *
 *   reg:            the plugin registry.
 *   name:           the module currently loaded (to replace).
 *   candidate_path: path to the new module file (.dylib/.so).
 *   expected_hash:  sha256 (hex) the candidate MUST match (from the signed
 *                   manifest), or NULL to skip the hash check (trusted-local
 *                   only — not for untrusted candidates).
 *   require_signed: when true, reject candidates without an allowed signer.
 *   snapshot_out:   receives the pre-swap snapshot for rollback (required).
 *   receipt:        bounded JSON receipt (required, >= 256 bytes).
 *   receipt_len:    size of receipt buffer.
 *
 * Atomicity: the candidate is verified, then loaded; only on a successful load
 * is the old module unloaded. If anything fails before the old module unloads,
 * the old module stays loaded and the swap returns false. */
bool hotswap_module(plugin_registry_t *reg, const char *name,
                    const char *candidate_path, const char *expected_hash,
                    bool require_signed,
                    hotswap_snapshot_t *snapshot_out,
                    char *receipt, size_t receipt_len);

/* Roll back to a snapshot: re-load the snapshot's module file and unload the
 * (possibly misbehaving) current module of the same name. Returns true on
 * success. Writes a bounded receipt. */
bool hotswap_rollback(plugin_registry_t *reg, const hotswap_snapshot_t *snapshot,
                      char *receipt, size_t receipt_len);

#endif /* DSCO_HOTSWAP_H */
