#ifndef DSCO_CRYPTO_H
#define DSCO_CRYPTO_H

/* dsco_crypto.h — 2026-standard crypto chokepoint.
 *
 * All call-sites in the codebase route through this header so primitives can
 * be rotated in one place. Direct calls to libsodium / liboqs / blake3 are
 * forbidden outside the src/crypto_*.c implementation files.
 *
 * Symmetric AEAD:     AEGIS-256 (libsodium ≥ 1.0.19), XChaCha20-Poly1305 fallback.
 * KDF:                HKDF-SHA-512 (per-purpose subkeys from a 32-byte root).
 * Password KDF:       Argon2id (m=256MiB, t=4, p=2).
 * Hash / keyed-MAC:   BLAKE3 when HAVE_BLAKE3, SHA-256 fallback.
 * Hybrid signing:     Ed25519 + ML-DSA-65 (liboqs) when HAVE_LIBOQS.
 * Hybrid KEM:         X25519 + ML-KEM-768 (liboqs) when HAVE_LIBOQS.
 * Constant-time cmp:  sodium_memcmp.
 * Zero / mlock:       sodium_memzero / sodium_mlock wrappers.
 *
 * NOTE: every value-returning function takes pre-allocated buffers; we don't
 * malloc inside the crypto layer so callers control the secret lifetime.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Initialization ───────────────────────────────────────────────────── */

/* Call once at program start. Returns 0 on success.
 * Internally initialises libsodium (and liboqs / blake3 if compiled in). */
int  dsco_crypto_init(void);

/* True if liboqs is linked and the post-quantum primitives are usable. */
bool dsco_crypto_has_pq(void);

/* True if a hardware-accelerated AEGIS-256 path is selected. */
bool dsco_crypto_aead_is_aegis(void);

/* Human-readable identifier for telemetry, e.g.
 *   "AEGIS-256 + HKDF-SHA512 + BLAKE3 + Ed25519+ML-DSA-65 + X25519+ML-KEM-768"
 */
const char *dsco_crypto_suite_id(void);

/* ── Constants ────────────────────────────────────────────────────────── */

/* AEAD: AEGIS-256 if HAVE_AEGIS256 else XChaCha20-Poly1305.
 * Both have 32-byte keys; AEGIS-256 nonce is 32B, XChaCha20 nonce is 24B.
 * We always reserve 32B in the wire format and zero-pad on XChaCha20.        */
#define DSCO_AEAD_KEY_BYTES   32u
#define DSCO_AEAD_NONCE_BYTES 32u
#define DSCO_AEAD_TAG_BYTES   16u

/* HKDF / hash. */
#define DSCO_HASH_BYTES       32u   /* BLAKE3-256 or SHA-256 */
#define DSCO_HKDF_PRK_BYTES   64u   /* HKDF-SHA-512 PRK length */

/* Identity (hybrid Ed25519 + ML-DSA-65 when HAVE_LIBOQS, else Ed25519 only). */
#define DSCO_ED25519_PK_BYTES  32u
#define DSCO_ED25519_SK_BYTES  64u
#define DSCO_ED25519_SIG_BYTES 64u
/* ML-DSA-65: pk=1952, sk=4032, sig=3309. We expose conservative caps. */
#define DSCO_MLDSA_PK_BYTES   1952u
#define DSCO_MLDSA_SK_BYTES   4032u
#define DSCO_MLDSA_SIG_BYTES  3309u

#define DSCO_HYBRID_PK_BYTES  (DSCO_ED25519_PK_BYTES  + DSCO_MLDSA_PK_BYTES)
#define DSCO_HYBRID_SIG_BYTES (DSCO_ED25519_SIG_BYTES + DSCO_MLDSA_SIG_BYTES)

/* KEM (hybrid X25519 + ML-KEM-768 when HAVE_LIBOQS, else X25519 only). */
#define DSCO_X25519_PK_BYTES  32u
#define DSCO_X25519_SK_BYTES  32u
#define DSCO_X25519_SS_BYTES  32u
/* ML-KEM-768: pk=1184, ct=1088, ss=32. */
#define DSCO_MLKEM_PK_BYTES   1184u
#define DSCO_MLKEM_CT_BYTES   1088u
#define DSCO_MLKEM_SS_BYTES   32u

#define DSCO_HYBRID_KEM_PK_BYTES (DSCO_X25519_PK_BYTES + DSCO_MLKEM_PK_BYTES)
#define DSCO_HYBRID_KEM_CT_BYTES (DSCO_X25519_PK_BYTES + DSCO_MLKEM_CT_BYTES)
#define DSCO_HYBRID_KEM_SS_BYTES 64u   /* HKDF-BLAKE3(x25519_ss || mlkem_ss) */

/* ── Random ───────────────────────────────────────────────────────────── */

void dsco_random_bytes(void *buf, size_t len);

/* ── Memory hygiene ───────────────────────────────────────────────────── */

void dsco_memzero(void *p, size_t n);
int  dsco_memcmp_ct(const void *a, const void *b, size_t n);   /* constant-time */
int  dsco_mlock(void *p, size_t n);                            /* best-effort */
int  dsco_munlock(void *p, size_t n);

/* ── KDF ──────────────────────────────────────────────────────────────── */

/* HKDF-SHA-512. Salt may be NULL. Returns 0 on success. */
int dsco_hkdf_sha512(const uint8_t *ikm,  size_t ikm_len,
                     const uint8_t *salt, size_t salt_len,
                     const uint8_t *info, size_t info_len,
                     uint8_t       *okm,  size_t okm_len);

/* Convenience: derive a 32-byte subkey from a 32-byte root with a labelled
 * info string. Equivalent to HKDF-SHA-512(root, salt="dsco.v2", info=label,
 * okm_len=32). Returns 0 on success. */
int dsco_kdf_subkey(const uint8_t  root[32],
                    const char    *label,
                    uint8_t        out[32]);

/* Argon2id-based password derivation. opslimit / memlimit per libsodium. */
int dsco_pwhash_argon2id(const uint8_t *password, size_t pw_len,
                         const uint8_t  salt[16],
                         uint8_t       *out, size_t out_len);

/* ── Hash ─────────────────────────────────────────────────────────────── */

/* One-shot hash (BLAKE3-256 or SHA-256 depending on build). */
void dsco_hash(const uint8_t *msg, size_t msg_len, uint8_t out[DSCO_HASH_BYTES]);

/* Keyed MAC. */
void dsco_keyed_mac(const uint8_t key[32],
                    const uint8_t *msg, size_t msg_len,
                    uint8_t out[DSCO_HASH_BYTES]);

/* ── AEAD ─────────────────────────────────────────────────────────────── */

/* Encrypt-then-MAC AEAD. The output ciphertext buffer must hold
 *   pt_len + DSCO_AEAD_TAG_BYTES.
 * Nonce MUST be unique per (key, message). Use dsco_random_bytes for nonces
 * or a monotonic counter where replay is bounded.
 *
 * Returns the ciphertext length on success, -1 on failure. */
long dsco_aead_seal(const uint8_t  key[DSCO_AEAD_KEY_BYTES],
                    const uint8_t  nonce[DSCO_AEAD_NONCE_BYTES],
                    const uint8_t *aad,        size_t aad_len,
                    const uint8_t *plaintext,  size_t pt_len,
                    uint8_t       *ciphertext, size_t ct_cap);

/* Returns plaintext length on success, -1 on tag verification failure. */
long dsco_aead_open(const uint8_t  key[DSCO_AEAD_KEY_BYTES],
                    const uint8_t  nonce[DSCO_AEAD_NONCE_BYTES],
                    const uint8_t *aad,        size_t aad_len,
                    const uint8_t *ciphertext, size_t ct_len,
                    uint8_t       *plaintext,  size_t pt_cap);

/* ── Hybrid identity (Ed25519 + ML-DSA-65 when HAVE_LIBOQS) ───────────── */

/* Generates a fresh hybrid keypair. Buffers must be sized to
 * DSCO_HYBRID_PK_BYTES / DSCO_ED25519_SK_BYTES+DSCO_MLDSA_SK_BYTES.
 * Returns 0 on success. */
int dsco_identity_keygen(uint8_t  pk_hybrid[DSCO_HYBRID_PK_BYTES],
                         uint8_t  sk_ed25519[DSCO_ED25519_SK_BYTES],
                         uint8_t  sk_mldsa[DSCO_MLDSA_SK_BYTES]);

/* Hybrid sign. Returns total signature length (Ed25519 || ML-DSA) on success,
 * -1 on failure. On builds without HAVE_LIBOQS, only the Ed25519 half is
 * produced and DSCO_ED25519_SIG_BYTES is returned. */
long dsco_identity_sign(const uint8_t  sk_ed25519[DSCO_ED25519_SK_BYTES],
                        const uint8_t  sk_mldsa[DSCO_MLDSA_SK_BYTES],
                        const uint8_t *msg, size_t msg_len,
                        uint8_t        sig_out[DSCO_HYBRID_SIG_BYTES]);

/* Hybrid verify. Both halves MUST verify when HAVE_LIBOQS. Returns 0 on
 * success, -1 on failure. */
int dsco_identity_verify(const uint8_t  pk_hybrid[DSCO_HYBRID_PK_BYTES],
                         const uint8_t *msg,    size_t msg_len,
                         const uint8_t *sig,    size_t sig_len);

/* ── Hybrid KEM (X25519 + ML-KEM-768 when HAVE_LIBOQS) ────────────────── */

int dsco_kem_keygen(uint8_t pk_hybrid[DSCO_HYBRID_KEM_PK_BYTES],
                    uint8_t sk_x25519[DSCO_X25519_SK_BYTES],
                    uint8_t sk_mlkem[DSCO_MLKEM_SK_BYTES]);

/* Encapsulate to a peer's hybrid public key. Outputs the ciphertext to send
 * and the derived 64-byte shared secret. */
int dsco_kem_encap(const uint8_t pk_hybrid[DSCO_HYBRID_KEM_PK_BYTES],
                   uint8_t       ct_hybrid[DSCO_HYBRID_KEM_CT_BYTES],
                   uint8_t       ss_out[DSCO_HYBRID_KEM_SS_BYTES]);

/* Decapsulate. */
int dsco_kem_decap(const uint8_t sk_x25519[DSCO_X25519_SK_BYTES],
                   const uint8_t sk_mlkem[DSCO_MLKEM_SK_BYTES],
                   const uint8_t ct_hybrid[DSCO_HYBRID_KEM_CT_BYTES],
                   uint8_t       ss_out[DSCO_HYBRID_KEM_SS_BYTES]);

#ifdef __cplusplus
}
#endif

#endif /* DSCO_CRYPTO_H */
