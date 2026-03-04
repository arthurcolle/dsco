#ifndef DSCO_CRYPTO_H
#define DSCO_CRYPTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ── SHA-256 (pure C implementation) ─────────────────────────────────── */

typedef struct {
    uint32_t state[8];
    uint64_t bitcount;
    uint8_t  buffer[64];
} sha256_ctx_t;

void sha256_init(sha256_ctx_t *ctx);
void sha256_update(sha256_ctx_t *ctx, const uint8_t *data, size_t len);
void sha256_final(sha256_ctx_t *ctx, uint8_t hash[32]);
void sha256_hex(const uint8_t *data, size_t len, char hex[65]);

/* ── MD5 (pure C) ────────────────────────────────────────────────────── */

typedef struct {
    uint32_t state[4];
    uint64_t count;
    uint8_t  buffer[64];
} md5_ctx_t;

void md5_init(md5_ctx_t *ctx);
void md5_update(md5_ctx_t *ctx, const uint8_t *data, size_t len);
void md5_final(md5_ctx_t *ctx, uint8_t hash[16]);
void md5_hex(const uint8_t *data, size_t len, char hex[33]);

/* ── HMAC-SHA256 ─────────────────────────────────────────────────────── */

void hmac_sha256(const uint8_t *key, size_t key_len,
                 const uint8_t *data, size_t data_len,
                 uint8_t out[32]);
void hmac_sha256_hex(const uint8_t *key, size_t key_len,
                     const uint8_t *data, size_t data_len,
                     char hex[65]);

/* ── Base64 encode/decode ────────────────────────────────────────────── */

size_t base64_encode(const uint8_t *src, size_t src_len, char *dst, size_t dst_len);
size_t base64_decode(const char *src, size_t src_len, uint8_t *dst, size_t dst_len);
size_t base64url_encode(const uint8_t *src, size_t src_len, char *dst, size_t dst_len);
size_t base64url_decode(const char *src, size_t src_len, uint8_t *dst, size_t dst_len);

/* ── Hex encode/decode ───────────────────────────────────────────────── */

void hex_encode(const uint8_t *src, size_t len, char *dst);
size_t hex_decode(const char *src, size_t src_len, uint8_t *dst, size_t dst_len);

/* ── UUID v4 ─────────────────────────────────────────────────────────── */

void uuid_v4(char uuid[37]);

/* ── Random bytes ────────────────────────────────────────────────────── */

bool crypto_random_bytes(uint8_t *buf, size_t len);
void crypto_random_hex(size_t nbytes, char *hex);

/* ── HKDF-SHA256 (key derivation) ────────────────────────────────────── */

void hkdf_sha256(const uint8_t *ikm, size_t ikm_len,
                 const uint8_t *salt, size_t salt_len,
                 const uint8_t *info, size_t info_len,
                 uint8_t *okm, size_t okm_len);

/* ── JWT decode (no verification, just parse) ────────────────────────── */

bool jwt_decode(const char *token, char *header, size_t h_len,
                char *payload, size_t p_len);

/* ── Constant-time comparison ────────────────────────────────────────── */

bool crypto_ct_equal(const uint8_t *a, const uint8_t *b, size_t len);

#endif
