#ifndef DSCO_SEALED_STORE_H
#define DSCO_SEALED_STORE_H

/* ── Sealed Secret Store ───────────────────────────────────────────────────
 *
 * Central, tamper-aware registry for all secrets (API keys, mesh keypairs,
 * session tokens, HMAC keys).  Backed by libsodium secretbox when available;
 * falls back to in-memory-only storage with mlock().
 *
 * Lifecycle:
 *   sealed_store_init()          — called once from main(), before secrets load
 *   sealed_store_put(key, ...)   — store a secret
 *   sealed_store_get(key, ...)   — retrieve a secret (caller owns copy)
 *   sealed_store_wipe(key)       — zero and remove one entry
 *   sealed_store_wipe_all()      — zero all entries (called by tamper wiper)
 *
 * On tamper detection, tamper.c calls sealed_store_wipe_all() via the
 * registered wiper before any further response action.
 *
 * Thread-safe: all operations take a mutex.
 * ─────────────────────────────────────────────────────────────────────── */

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

/* Maximum length of a secret key name (e.g. "ANTHROPIC_API_KEY") */
#define SEALED_KEY_MAX  64
/* Maximum length of a secret value */
#define SEALED_VAL_MAX  4096
/* Maximum number of entries */
#define SEALED_ENTRIES  64

/* Initialise the store and register the wiper with tamper.c.
 * Loads existing secrets from environment variables into the store. */
void sealed_store_init(void);

/* Supply a 32-byte hardware-derived master key (from se_store on macOS SE).
 * Must be called before sealed_store_init() if SE is available.
 * The key is copied into mlock'd memory; caller should zero their copy. */
void sealed_store_set_master_key(const uint8_t key[32]);

/* Copy the master key into the caller's buffer. Returns false if no master
 * key has been registered yet. Caller MUST zero the buffer after use. */
bool sealed_store_master_key_copy(uint8_t out[32]);

/* Store or overwrite a secret.  val_len == 0 means strlen(val).
 * Returns 0 on success, -1 on overflow. */
int  sealed_store_put(const char *key, const char *val, size_t val_len);

/* Copy the secret for *key into buf[0..buf_len).
 * Returns the number of bytes written (not including NUL), or -1 if not found
 * or buf too small.  Always NUL-terminates on success. */
int  sealed_store_get(const char *key, char *buf, size_t buf_len);

/* Remove and zero one entry.  No-op if key not found. */
void sealed_store_wipe(const char *key);

/* Remove and zero all entries (called automatically on tamper detection). */
void sealed_store_wipe_all(void);

/* Convenience: get from store, fall back to getenv(), intern into store.
 * Returns static pointer valid until next wipe — do NOT free. */
const char *sealed_getenv(const char *key);

/* Return a direct, stable pointer to the interned value for *key, or NULL if
 * not present.  Unlike sealed_getenv(), this does NOT fall back to getenv() or
 * intern anything, and the returned pointer is stable for the lifetime of the
 * entry (until wiped) rather than rotating per-call.  Do NOT free or modify. */
const char *sealed_store_peek(const char *key);

#endif /* DSCO_SEALED_STORE_H */
