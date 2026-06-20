#ifndef DSCO_SE_STORE_H
#define DSCO_SE_STORE_H

/* ── Secure Store Key Derivation ──────────────────────────────────────────
 *
 * Provides a secure master key for sealed_store on macOS.
 *
 * Default protocol on every process start:
 *
 *   1. Load or create a 32-byte random master key in the local macOS login
 *      Keychain under the dsco v2 service/account namespace.
 *
 *   2. Store the key as a local generic password protected by
 *      kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly.
 *
 *   3. Hand the 32-byte key to sealed_store as the encryption master key.
 *      Zero the buffer once sealed_store has ingested it.
 *
 * Secure Enclave mode:
 *   - DSCO_TRY_SECURE_ENCLAVE=1 attempts a hardware-rooted ephemeral P-256
 *     Secure Enclave ECDH key before falling back to the login Keychain.
 *   - DSCO_REQUIRE_SECURE_ENCLAVE=1 is strict/fail-closed and refuses startup
 *     if Secure Enclave is unavailable.
 *
 * Persistent SE identity is opt-in with DSCO_SE_PERSISTENT=1. That path
 * retrieves or creates a keychain-backed Secure Enclave key tagged
 * "systems.distributed.dsco.master.v1".
 *
 * Note: unsigned command-line binaries on modern macOS can return -34018 or
 * block while creating Secure Enclave keys because the required entitlement is
 * missing. The default login-Keychain tier is still fail-closed and OS
 * protected; weak machine-UUID derivation is disabled by default.
 *
 * The weaker machine-UUID derivation is disabled by default and requires the
 * explicit DSCO_ALLOW_MACHINE_UUID_STORE=1 development override.
 *
 * macOS only — compile-guarded by __APPLE__ + HAVE_SECURE_ENCLAVE.
 * ─────────────────────────────────────────────────────────────────────── */

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* Attempt to initialise the secure master key into out_key[32].
 * Returns true on success (Secure Enclave or secure keychain fallback).
 * Returns false if no secure tier can be used.
 * Zeros out_key after sealed_store_set_master_key() has consumed it. */
bool se_store_init(uint8_t out_key[32]);

/* Erase any cached SE state (called from tamper wiper). */
void se_store_wipe(void);

/* True if a secure tier was successfully initialised this process invocation. */
bool se_store_available(void);

/* Sign data with the SE private key (ECDSA P-256).
 * Useful for mesh node identity — the signature proves this binary
 * is running on the authentic machine without revealing the private key.
 * sig_buf must be at least 72 bytes (DER-encoded ECDSA sig).
 * Returns actual sig length, or -1 on error. */
int  se_store_sign(const uint8_t *data, size_t data_len,
                   uint8_t *sig_buf, size_t sig_buf_len);

/* Get the SE public key in uncompressed X9.62 form (04 || x || y = 65 bytes).
 * This is the node's hardware identity — can be published to peers. */
bool se_store_pubkey(uint8_t out[65]);

#endif /* DSCO_SE_STORE_H */
