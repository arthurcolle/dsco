#include "crypto.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>

/* ═══════════════════════════════════════════════════════════════════════════
 * SHA-256 — Pure C implementation (FIPS 180-4)
 * ═══════════════════════════════════════════════════════════════════════════ */

static const uint32_t SHA256_K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
    0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
    0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

#define ROR32(x, n)  (((x) >> (n)) | ((x) << (32 - (n))))
#define CH(x,y,z)    (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x,y,z)   (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define EP0(x)        (ROR32(x,2) ^ ROR32(x,13) ^ ROR32(x,22))
#define EP1(x)        (ROR32(x,6) ^ ROR32(x,11) ^ ROR32(x,25))
#define SIG0(x)       (ROR32(x,7) ^ ROR32(x,18) ^ ((x) >> 3))
#define SIG1(x)       (ROR32(x,17) ^ ROR32(x,19) ^ ((x) >> 10))

static void sha256_transform(sha256_ctx_t *ctx, const uint8_t data[64]) {
    uint32_t w[64], a, b, c, d, e, f, g, h, t1, t2;

    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)data[i*4] << 24) | ((uint32_t)data[i*4+1] << 16) |
               ((uint32_t)data[i*4+2] << 8) | data[i*4+3];
    for (int i = 16; i < 64; i++)
        w[i] = SIG1(w[i-2]) + w[i-7] + SIG0(w[i-15]) + w[i-16];

    a = ctx->state[0]; b = ctx->state[1]; c = ctx->state[2]; d = ctx->state[3];
    e = ctx->state[4]; f = ctx->state[5]; g = ctx->state[6]; h = ctx->state[7];

    for (int i = 0; i < 64; i++) {
        t1 = h + EP1(e) + CH(e,f,g) + SHA256_K[i] + w[i];
        t2 = EP0(a) + MAJ(a,b,c);
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
    ctx->state[4] += e; ctx->state[5] += f; ctx->state[6] += g; ctx->state[7] += h;
}

void sha256_init(sha256_ctx_t *ctx) {
    ctx->state[0] = 0x6a09e667; ctx->state[1] = 0xbb67ae85;
    ctx->state[2] = 0x3c6ef372; ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f; ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab; ctx->state[7] = 0x5be0cd19;
    ctx->bitcount = 0;
}

void sha256_update(sha256_ctx_t *ctx, const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        ctx->buffer[ctx->bitcount / 8 % 64] = data[i];
        ctx->bitcount += 8;
        if (ctx->bitcount / 8 % 64 == 0)
            sha256_transform(ctx, ctx->buffer);
    }
}

void sha256_final(sha256_ctx_t *ctx, uint8_t hash[32]) {
    uint64_t bits = ctx->bitcount;
    size_t idx = (size_t)(bits / 8 % 64);

    ctx->buffer[idx++] = 0x80;
    if (idx > 56) {
        while (idx < 64) ctx->buffer[idx++] = 0;
        sha256_transform(ctx, ctx->buffer);
        idx = 0;
    }
    while (idx < 56) ctx->buffer[idx++] = 0;

    for (int i = 7; i >= 0; i--)
        ctx->buffer[56 + (7 - i)] = (uint8_t)(bits >> (i * 8));

    sha256_transform(ctx, ctx->buffer);

    for (int i = 0; i < 8; i++) {
        hash[i*4]   = (uint8_t)(ctx->state[i] >> 24);
        hash[i*4+1] = (uint8_t)(ctx->state[i] >> 16);
        hash[i*4+2] = (uint8_t)(ctx->state[i] >> 8);
        hash[i*4+3] = (uint8_t)(ctx->state[i]);
    }
}

void sha256_hex(const uint8_t *data, size_t len, char hex[65]) {
    if (!data || !hex) { if (hex) hex[0] = '\0'; return; }
    sha256_ctx_t ctx;
    uint8_t hash[32];
    sha256_init(&ctx);
    sha256_update(&ctx, data, len);
    sha256_final(&ctx, hash);
    hex_encode(hash, 32, hex);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * MD5 — Pure C implementation (RFC 1321)
 * ═══════════════════════════════════════════════════════════════════════════ */

#define MD5_F(x,y,z) (((x)&(y)) | (~(x)&(z)))
#define MD5_G(x,y,z) (((x)&(z)) | ((y)&~(z)))
#define MD5_H(x,y,z) ((x)^(y)^(z))
#define MD5_I(x,y,z) ((y)^((x)|~(z)))
#define ROL32(x,n)   (((x)<<(n)) | ((x)>>(32-(n))))

static const uint32_t MD5_T[64] = {
    0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
    0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
    0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
    0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
    0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
    0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
    0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
    0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391
};
static const int MD5_S[64] = {
    7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22,
    5,9,14,20,5,9,14,20,5,9,14,20,5,9,14,20,
    4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23,
    6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21
};

static void md5_transform(md5_ctx_t *ctx, const uint8_t block[64]) {
    uint32_t M[16];
    for (int i = 0; i < 16; i++)
        M[i] = (uint32_t)block[i*4] | ((uint32_t)block[i*4+1]<<8) |
               ((uint32_t)block[i*4+2]<<16) | ((uint32_t)block[i*4+3]<<24);

    uint32_t a = ctx->state[0], b = ctx->state[1];
    uint32_t c = ctx->state[2], d = ctx->state[3];

    for (int i = 0; i < 64; i++) {
        uint32_t f; int g;
        if (i < 16)      { f = MD5_F(b,c,d); g = i; }
        else if (i < 32) { f = MD5_G(b,c,d); g = (5*i+1) % 16; }
        else if (i < 48) { f = MD5_H(b,c,d); g = (3*i+5) % 16; }
        else              { f = MD5_I(b,c,d); g = (7*i) % 16; }
        f += a + MD5_T[i] + M[g];
        a = d; d = c; c = b; b = b + ROL32(f, MD5_S[i]);
    }

    ctx->state[0] += a; ctx->state[1] += b;
    ctx->state[2] += c; ctx->state[3] += d;
}

void md5_init(md5_ctx_t *ctx) {
    ctx->state[0] = 0x67452301; ctx->state[1] = 0xefcdab89;
    ctx->state[2] = 0x98badcfe; ctx->state[3] = 0x10325476;
    ctx->count = 0;
}

void md5_update(md5_ctx_t *ctx, const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        ctx->buffer[ctx->count % 64] = data[i];
        ctx->count++;
        if (ctx->count % 64 == 0)
            md5_transform(ctx, ctx->buffer);
    }
}

void md5_final(md5_ctx_t *ctx, uint8_t hash[16]) {
    uint64_t bits = ctx->count * 8;
    size_t idx = ctx->count % 64;

    ctx->buffer[idx++] = 0x80;
    if (idx > 56) {
        while (idx < 64) ctx->buffer[idx++] = 0;
        md5_transform(ctx, ctx->buffer);
        idx = 0;
    }
    while (idx < 56) ctx->buffer[idx++] = 0;
    for (int i = 0; i < 8; i++)
        ctx->buffer[56 + i] = (uint8_t)(bits >> (i * 8));
    md5_transform(ctx, ctx->buffer);

    for (int i = 0; i < 4; i++) {
        hash[i*4]   = (uint8_t)(ctx->state[i]);
        hash[i*4+1] = (uint8_t)(ctx->state[i] >> 8);
        hash[i*4+2] = (uint8_t)(ctx->state[i] >> 16);
        hash[i*4+3] = (uint8_t)(ctx->state[i] >> 24);
    }
}

void md5_hex(const uint8_t *data, size_t len, char hex[33]) {
    if (!data || !hex) { if (hex) hex[0] = '\0'; return; }
    md5_ctx_t ctx;
    uint8_t hash[16];
    md5_init(&ctx);
    md5_update(&ctx, data, len);
    md5_final(&ctx, hash);
    hex_encode(hash, 16, hex);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * HMAC-SHA256
 * ═══════════════════════════════════════════════════════════════════════════ */

void hmac_sha256(const uint8_t *key, size_t key_len,
                 const uint8_t *data, size_t data_len,
                 uint8_t out[32]) {
    if (!key || !data || !out) { if (out) memset(out, 0, 32); return; }
    if (key_len > 1048576) { memset(out, 0, 32); return; } /* reject keys > 1MB */
    uint8_t k_pad[64];
    uint8_t k_hash[32];

    /* If key > 64 bytes, hash it first */
    if (key_len > 64) {
        sha256_ctx_t kctx;
        sha256_init(&kctx);
        sha256_update(&kctx, key, key_len);
        sha256_final(&kctx, k_hash);
        key = k_hash;
        key_len = 32;
    }

    /* Inner pad */
    memset(k_pad, 0x36, 64);
    for (size_t i = 0; i < key_len; i++) k_pad[i] ^= key[i];

    sha256_ctx_t ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, k_pad, 64);
    sha256_update(&ctx, data, data_len);
    uint8_t inner[32];
    sha256_final(&ctx, inner);

    /* Outer pad */
    memset(k_pad, 0x5c, 64);
    for (size_t i = 0; i < key_len; i++) k_pad[i] ^= key[i];

    sha256_init(&ctx);
    sha256_update(&ctx, k_pad, 64);
    sha256_update(&ctx, inner, 32);
    sha256_final(&ctx, out);
}

void hmac_sha256_hex(const uint8_t *key, size_t key_len,
                     const uint8_t *data, size_t data_len,
                     char hex[65]) {
    if (!hex) return;
    if (!key || !data) { hex[0] = '\0'; return; }
    uint8_t mac[32];
    hmac_sha256(key, key_len, data, data_len, mac);
    hex_encode(mac, 32, hex);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Base64 encode/decode
 * ═══════════════════════════════════════════════════════════════════════════ */

static const char B64_TABLE[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static const char B64URL_TABLE[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

static size_t b64_encode_internal(const uint8_t *src, size_t src_len,
                                   char *dst, size_t dst_len,
                                   const char *table, bool pad) {
    size_t out = 0;
    for (size_t i = 0; i < src_len; i += 3) {
        uint32_t n = (uint32_t)src[i] << 16;
        if (i + 1 < src_len) n |= (uint32_t)src[i+1] << 8;
        if (i + 2 < src_len) n |= src[i+2];

        if (out < dst_len) dst[out++] = table[(n >> 18) & 0x3F];
        if (out < dst_len) dst[out++] = table[(n >> 12) & 0x3F];
        if (i + 1 < src_len) { if (out < dst_len) dst[out++] = table[(n >> 6) & 0x3F]; }
        else if (pad) { if (out < dst_len) dst[out++] = '='; }
        if (i + 2 < src_len) { if (out < dst_len) dst[out++] = table[n & 0x3F]; }
        else if (pad) { if (out < dst_len) dst[out++] = '='; }
    }
    if (out < dst_len) dst[out] = '\0';
    return out;
}

static int b64_char_val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}

static size_t b64_decode_internal(const char *src, size_t src_len,
                                   uint8_t *dst, size_t dst_len) {
    size_t out = 0;
    uint32_t buf = 0;
    int bits = 0;
    for (size_t i = 0; i < src_len; i++) {
        if (src[i] == '=' || src[i] == '\n' || src[i] == '\r') continue;
        int v = b64_char_val(src[i]);
        if (v < 0) continue;
        buf = (buf << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (out < dst_len) dst[out++] = (uint8_t)(buf >> bits);
            buf &= (1u << bits) - 1;
        }
    }
    if (out < dst_len) dst[out] = 0;
    return out;
}

size_t base64_encode(const uint8_t *src, size_t src_len, char *dst, size_t dst_len) {
    return b64_encode_internal(src, src_len, dst, dst_len, B64_TABLE, true);
}

size_t base64_decode(const char *src, size_t src_len, uint8_t *dst, size_t dst_len) {
    return b64_decode_internal(src, src_len, dst, dst_len);
}

size_t base64url_encode(const uint8_t *src, size_t src_len, char *dst, size_t dst_len) {
    return b64_encode_internal(src, src_len, dst, dst_len, B64URL_TABLE, false);
}

size_t base64url_decode(const char *src, size_t src_len, uint8_t *dst, size_t dst_len) {
    return b64_decode_internal(src, src_len, dst, dst_len);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Hex encode/decode
 * ═══════════════════════════════════════════════════════════════════════════ */

void hex_encode(const uint8_t *src, size_t len, char *dst) {
    static const char hex_chars[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        dst[i*2]   = hex_chars[src[i] >> 4];
        dst[i*2+1] = hex_chars[src[i] & 0xF];
    }
    dst[len*2] = '\0';
}

size_t hex_decode(const char *src, size_t src_len, uint8_t *dst, size_t dst_len) {
    size_t out = 0;
    for (size_t i = 0; i + 1 < src_len && out < dst_len; i += 2) {
        int hi = 0, lo = 0;
        char c = src[i];
        if (c >= '0' && c <= '9') hi = c - '0';
        else if (c >= 'a' && c <= 'f') hi = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') hi = c - 'A' + 10;
        c = src[i+1];
        if (c >= '0' && c <= '9') lo = c - '0';
        else if (c >= 'a' && c <= 'f') lo = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') lo = c - 'A' + 10;
        dst[out++] = (uint8_t)((hi << 4) | lo);
    }
    return out;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * UUID v4
 * ═══════════════════════════════════════════════════════════════════════════ */

void uuid_v4(char uuid[37]) {
    uint8_t bytes[16];
    crypto_random_bytes(bytes, 16);

    /* Set version 4 */
    bytes[6] = (bytes[6] & 0x0F) | 0x40;
    /* Set variant (RFC 4122) */
    bytes[8] = (bytes[8] & 0x3F) | 0x80;

    snprintf(uuid, 37, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             bytes[0],bytes[1],bytes[2],bytes[3],
             bytes[4],bytes[5],bytes[6],bytes[7],
             bytes[8],bytes[9],bytes[10],bytes[11],
             bytes[12],bytes[13],bytes[14],bytes[15]);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Random bytes (from /dev/urandom)
 * ═══════════════════════════════════════════════════════════════════════════ */

bool crypto_random_bytes(uint8_t *buf, size_t len) {
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) return false;
    size_t total = 0;
    while (total < len) {
        ssize_t n = read(fd, buf + total, len - total);
        if (n <= 0) { close(fd); return false; }
        total += (size_t)n;
    }
    close(fd);
    return true;
}

void crypto_random_hex(size_t nbytes, char *hex) {
    uint8_t *buf = malloc(nbytes);
    if (!buf) { hex[0] = '\0'; return; }
    crypto_random_bytes(buf, nbytes);
    hex_encode(buf, nbytes, hex);
    free(buf);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * HKDF-SHA256 (RFC 5869)
 * ═══════════════════════════════════════════════════════════════════════════ */

void hkdf_sha256(const uint8_t *ikm, size_t ikm_len,
                 const uint8_t *salt, size_t salt_len,
                 const uint8_t *info, size_t info_len,
                 uint8_t *okm, size_t okm_len) {
    uint8_t prk[32];
    uint8_t default_salt[32];

    /* Extract */
    if (!salt || salt_len == 0) {
        memset(default_salt, 0, 32);
        salt = default_salt;
        salt_len = 32;
    }
    hmac_sha256(salt, salt_len, ikm, ikm_len, prk);

    /* Expand */
    uint8_t t[32];
    size_t t_len = 0;
    uint8_t counter = 1;
    size_t pos = 0;

    while (pos < okm_len) {
        /* T(n) = HMAC(PRK, T(n-1) || info || counter) */
        size_t msg_len = t_len + info_len + 1;
        uint8_t *msg = malloc(msg_len);
        if (!msg) return;

        if (t_len > 0) memcpy(msg, t, t_len);
        if (info_len > 0) memcpy(msg + t_len, info, info_len);
        msg[t_len + info_len] = counter;

        hmac_sha256(prk, 32, msg, msg_len, t);
        free(msg);
        t_len = 32;

        size_t copy = okm_len - pos;
        if (copy > 32) copy = 32;
        memcpy(okm + pos, t, copy);
        pos += copy;
        counter++;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * JWT decode (header + payload, no verification)
 * ═══════════════════════════════════════════════════════════════════════════ */

bool jwt_decode(const char *token, char *header, size_t h_len,
                char *payload, size_t p_len) {
    /* JWT = header.payload.signature */
    const char *dot1 = strchr(token, '.');
    if (!dot1) return false;
    const char *dot2 = strchr(dot1 + 1, '.');
    if (!dot2) return false;

    /* Decode header */
    size_t h_b64_len = (size_t)(dot1 - token);
    uint8_t *h_raw = malloc(h_b64_len + 4);
    size_t h_decoded = base64url_decode(token, h_b64_len, h_raw, h_b64_len + 4);
    if (h_decoded < h_len) {
        memcpy(header, h_raw, h_decoded);
        header[h_decoded] = '\0';
    } else {
        memcpy(header, h_raw, h_len - 1);
        header[h_len - 1] = '\0';
    }
    free(h_raw);

    /* Decode payload */
    size_t p_b64_len = (size_t)(dot2 - dot1 - 1);
    uint8_t *p_raw = malloc(p_b64_len + 4);
    size_t p_decoded = base64url_decode(dot1 + 1, p_b64_len, p_raw, p_b64_len + 4);
    if (p_decoded < p_len) {
        memcpy(payload, p_raw, p_decoded);
        payload[p_decoded] = '\0';
    } else {
        memcpy(payload, p_raw, p_len - 1);
        payload[p_len - 1] = '\0';
    }
    free(p_raw);

    return true;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Constant-time comparison
 * ═══════════════════════════════════════════════════════════════════════════ */

bool crypto_ct_equal(const uint8_t *a, const uint8_t *b, size_t len) {
    volatile uint8_t diff = 0;
    for (size_t i = 0; i < len; i++)
        diff |= a[i] ^ b[i];
    return diff == 0;
}
