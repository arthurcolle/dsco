/* trading.c — Cross-platform prediction market trading infrastructure.
 *
 * Implements authenticated trading on Kalshi (RSA-PSS) and Polymarket CLOB
 * (HMAC-SHA256 + EIP-712 order signing), plus cross-platform arbitrage
 * execution, risk management, and unified portfolio views.
 *
 * All tools follow the standard dsco tool signature:
 *   bool tool_xxx(const char *input_json, char *result, size_t rlen);
 */

#include "trading.h"
#include "json_util.h"
#include "crypto.h"
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <time.h>
#include <sys/time.h>
#include <ctype.h>
#include <math.h>
#include <curl/curl.h>

/* ══════════════════════════════════════════════════════════════════════════
 *  SECTION 1: HTTP HELPERS
 * ══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} http_buf_t;

static size_t http_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    size_t total = size * nmemb;
    http_buf_t *b = (http_buf_t *)userdata;
    if (b->len + total >= b->cap) {
        b->cap = (b->len + total) * 2 + 1;
        b->data = realloc(b->data, b->cap);
        if (!b->data) return 0;
    }
    memcpy(b->data + b->len, ptr, total);
    b->len += total;
    b->data[b->len] = '\0';
    return total;
}

static long trading_http_request(const char *method, const char *url,
                                 const char *body,
                                 const char *headers[], int header_count,
                                 http_buf_t *out_buf) {
    CURL *curl = curl_easy_init();
    if (!curl) return -1;

    out_buf->data = malloc(8192);
    out_buf->len = 0;
    out_buf->cap = 8192;
    out_buf->data[0] = '\0';

    struct curl_slist *hdrs = NULL;
    hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
    hdrs = curl_slist_append(hdrs, "Accept: application/json");
    char ua[64];
    snprintf(ua, sizeof(ua), "User-Agent: dsco/%s", DSCO_VERSION);
    hdrs = curl_slist_append(hdrs, ua);
    for (int i = 0; i < header_count; i++) {
        hdrs = curl_slist_append(hdrs, headers[i]);
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);

    if (strcmp(method, "POST") == 0) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body ? body : "");
    } else if (strcmp(method, "PUT") == 0) {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
        if (body) curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    } else if (strcmp(method, "DELETE") == 0) {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
        if (body) curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    } else if (strcmp(method, "PATCH") == 0) {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PATCH");
        if (body) curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    }
    /* GET is the default, no special setup needed */

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, http_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, out_buf);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    if (res == CURLE_OK)
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        snprintf(out_buf->data, out_buf->cap, "curl error: %s", curl_easy_strerror(res));
        return -1;
    }
    return http_code;
}

/* Check if API key / env var is set */
static bool trading_require_key(const char *env_var, const char *service,
                                char *result, size_t rlen, const char **out_key) {
    const char *key = getenv(env_var);
    if (!key || !key[0]) {
        snprintf(result, rlen,
                 "%s not set. Run: export %s=your_value_here",
                 service, env_var);
        return false;
    }
    *out_key = key;
    return true;
}

/* Truncate response to fit in result buffer with room for wrapper text */
static void trading_truncate(const char *src, char *dst, size_t dst_len, size_t reserved) {
    size_t available = dst_len - reserved;
    size_t src_len = strlen(src);
    size_t pos = strlen(dst);
    if (src_len <= available) {
        memcpy(dst + pos, src, src_len + 1);
    } else {
        memcpy(dst + pos, src, available - 20);
        dst[pos + available - 20] = '\0';
        strcat(dst, "\n[truncated]");
    }
}

/* ══════════════════════════════════════════════════════════════════════════
 *  SECTION 2: KECCAK-256 (Original Keccak, NOT SHA-3)
 *
 *  Keccak-f[1600] permutation with 24 rounds, sponge construction.
 *  Padding byte 0x01 (original Keccak), NOT 0x06 (SHA-3/FIPS-202).
 *  Rate = 1088 bits (136 bytes), capacity = 512 bits, output = 256 bits.
 * ══════════════════════════════════════════════════════════════════════════ */

static const uint64_t KECCAK_RC[24] = {
    0x0000000000000001ULL, 0x0000000000008082ULL,
    0x800000000000808AULL, 0x8000000080008000ULL,
    0x000000000000808BULL, 0x0000000080000001ULL,
    0x8000000080008081ULL, 0x8000000000008009ULL,
    0x000000000000008AULL, 0x0000000000000088ULL,
    0x0000000080008009ULL, 0x000000008000000AULL,
    0x000000008000808BULL, 0x800000000000008BULL,
    0x8000000000008089ULL, 0x8000000000008003ULL,
    0x8000000000008002ULL, 0x8000000000000080ULL,
    0x000000000000800AULL, 0x800000008000000AULL,
    0x8000000080008081ULL, 0x8000000000008080ULL,
    0x0000000080000001ULL, 0x8000000080008008ULL,
};

/* Rotation offsets for rho step, indexed by (x,y) linearized as x + 5*y */
static const int KECCAK_ROT[25] = {
     0,  1, 62, 28, 27,
    36, 44,  6, 55, 20,
     3, 10, 43, 25, 39,
    41, 45, 15, 21,  8,
    18,  2, 61, 56, 14,
};

static inline uint64_t rotl64(uint64_t x, int n) {
    if (n == 0) return x;
    return (x << n) | (x >> (64 - n));
}

static void keccak_f1600(uint64_t A[25]) {
    for (int round = 0; round < 24; round++) {
        /* Theta */
        uint64_t C[5], D[5];
        for (int x = 0; x < 5; x++)
            C[x] = A[x] ^ A[x + 5] ^ A[x + 10] ^ A[x + 15] ^ A[x + 20];
        for (int x = 0; x < 5; x++) {
            D[x] = C[(x + 4) % 5] ^ rotl64(C[(x + 1) % 5], 1);
            for (int y = 0; y < 5; y++)
                A[x + 5 * y] ^= D[x];
        }

        /* Rho and Pi */
        uint64_t B[25];
        for (int x = 0; x < 5; x++) {
            for (int y = 0; y < 5; y++) {
                int idx = x + 5 * y;
                int new_x = y;
                int new_y = (2 * x + 3 * y) % 5;
                B[new_x + 5 * new_y] = rotl64(A[idx], KECCAK_ROT[idx]);
            }
        }

        /* Chi */
        for (int x = 0; x < 5; x++) {
            for (int y = 0; y < 5; y++) {
                int idx = x + 5 * y;
                A[idx] = B[idx] ^ (~B[((x + 1) % 5) + 5 * y] & B[((x + 2) % 5) + 5 * y]);
            }
        }

        /* Iota */
        A[0] ^= KECCAK_RC[round];
    }
}

/* Keccak-256: rate=136 bytes, capacity=64 bytes, output=32 bytes.
 * Uses ORIGINAL Keccak padding (0x01), not SHA-3 padding (0x06). */
static void keccak256(const uint8_t *data, size_t len, uint8_t out[32]) {
    const size_t rate = 136; /* 1088 bits / 8 */
    uint64_t A[25];
    memset(A, 0, sizeof(A));

    /* Absorb complete blocks */
    size_t offset = 0;
    while (offset + rate <= len) {
        for (size_t i = 0; i < rate / 8; i++) {
            uint64_t lane = 0;
            memcpy(&lane, data + offset + i * 8, 8);
            A[i] ^= lane; /* little-endian */
        }
        keccak_f1600(A);
        offset += rate;
    }

    /* Absorb final partial block with padding */
    uint8_t block[136];
    memset(block, 0, rate);
    size_t remaining = len - offset;
    if (remaining > 0)
        memcpy(block, data + offset, remaining);

    /* Original Keccak padding: pad with 0x01 at start, 0x80 at end */
    block[remaining] = 0x01;
    block[rate - 1] |= 0x80;

    for (size_t i = 0; i < rate / 8; i++) {
        uint64_t lane = 0;
        memcpy(&lane, block + i * 8, 8);
        A[i] ^= lane;
    }
    keccak_f1600(A);

    /* Squeeze 32 bytes */
    memcpy(out, A, 32);
}

/* Keccak-256 with hex output */
__attribute__((unused))
static void keccak256_hex(const uint8_t *data, size_t len, char hex[65]) {
    uint8_t hash[32];
    keccak256(data, len, hash);
    hex_encode(hash, 32, hex);
}

/* ══════════════════════════════════════════════════════════════════════════
 *  SECTION 3: RSA-PSS SIGNING (via OpenSSL pipe)
 * ══════════════════════════════════════════════════════════════════════════ */

static bool rsa_pss_sign(const char *key_path, const uint8_t *msg, size_t msg_len,
                         char *b64_sig, size_t sig_len) {
    /* Write message to temp file */
    char tmpfile[] = "/tmp/dsco_rsa_msg_XXXXXX";
    int fd = mkstemp(tmpfile);
    if (fd < 0) return false;

    ssize_t written = write(fd, msg, msg_len);
    close(fd);
    if ((size_t)written != msg_len) {
        unlink(tmpfile);
        return false;
    }

    /* Sign with OpenSSL: RSA-PSS, SHA-256, salt length 32 */
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "openssl dgst -sha256 -sigopt rsa_padding_mode:pss "
             "-sigopt rsa_pss_saltlen:32 -sign '%s' '%s' | openssl base64 -A",
             key_path, tmpfile);

    FILE *fp = popen(cmd, "r");
    if (!fp) {
        unlink(tmpfile);
        return false;
    }

    size_t total = 0;
    while (total < sig_len - 1) {
        size_t n = fread(b64_sig + total, 1, sig_len - 1 - total, fp);
        if (n == 0) break;
        total += n;
    }
    b64_sig[total] = '\0';
    int status = pclose(fp);
    unlink(tmpfile);

    /* Strip trailing whitespace */
    while (total > 0 && (b64_sig[total - 1] == '\n' || b64_sig[total - 1] == '\r' ||
                         b64_sig[total - 1] == ' ')) {
        b64_sig[--total] = '\0';
    }

    return (status == 0 && total > 0);
}

/* ══════════════════════════════════════════════════════════════════════════
 *  SECTION 4: secp256k1 ECDSA SIGNING (via OpenSSL pipe)
 * ══════════════════════════════════════════════════════════════════════════ */

static bool secp256k1_sign_hash(const uint8_t privkey[32], const uint8_t hash[32],
                                uint8_t out_r[32], uint8_t out_s[32], uint8_t *out_v) {
    /* Build DER encoding for secp256k1 private key:
     * SEQUENCE {
     *   INTEGER 1
     *   OCTET STRING (32 bytes) <privkey>
     *   [0] OID 1.3.132.0.10 (secp256k1)
     * }
     * Encoded: 30 2e 02 01 01 04 20 <32 bytes key> a0 07 06 05 2b 81 04 00 0a
     */
    uint8_t der[48];
    const uint8_t der_prefix[] = { 0x30, 0x2e, 0x02, 0x01, 0x01, 0x04, 0x20 };
    const uint8_t der_suffix[] = { 0xa0, 0x07, 0x06, 0x05, 0x2b, 0x81, 0x04, 0x00, 0x0a };
    memcpy(der, der_prefix, sizeof(der_prefix));
    memcpy(der + sizeof(der_prefix), privkey, 32);
    memcpy(der + sizeof(der_prefix) + 32, der_suffix, sizeof(der_suffix));
    size_t der_len = sizeof(der_prefix) + 32 + sizeof(der_suffix);

    /* Write DER to temp file */
    char der_file[] = "/tmp/dsco_ec_der_XXXXXX";
    int fd_der = mkstemp(der_file);
    if (fd_der < 0) return false;
    write(fd_der, der, der_len);
    close(fd_der);

    /* Convert DER to PEM */
    char pem_file[] = "/tmp/dsco_ec_pem_XXXXXX";
    int fd_pem = mkstemp(pem_file);
    close(fd_pem);

    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "openssl ec -inform DER -in '%s' -outform PEM -out '%s' 2>/dev/null",
             der_file, pem_file);
    int rc = system(cmd);
    unlink(der_file);
    if (rc != 0) {
        unlink(pem_file);
        return false;
    }

    /* Write hash to temp file */
    char hash_file[] = "/tmp/dsco_ec_hash_XXXXXX";
    int fd_hash = mkstemp(hash_file);
    if (fd_hash < 0) {
        unlink(pem_file);
        return false;
    }
    write(fd_hash, hash, 32);
    close(fd_hash);

    /* Sign with pkeyutl */
    snprintf(cmd, sizeof(cmd),
             "openssl pkeyutl -sign -inkey '%s' -in '%s'",
             pem_file, hash_file);
    FILE *fp = popen(cmd, "r");
    if (!fp) {
        unlink(pem_file);
        unlink(hash_file);
        return false;
    }

    uint8_t sig_der[128];
    size_t sig_len = fread(sig_der, 1, sizeof(sig_der), fp);
    int status = pclose(fp);
    unlink(pem_file);
    unlink(hash_file);

    if (status != 0 || sig_len < 6) return false;

    /* Parse DER-encoded ECDSA signature:
     * SEQUENCE { INTEGER r, INTEGER s }
     * 30 <total_len> 02 <r_len> <r_bytes> 02 <s_len> <s_bytes>
     */
    size_t pos = 0;
    if (sig_der[pos++] != 0x30) return false;  /* SEQUENCE */
    /* Skip total length (could be 1 or 2 bytes) */
    if (sig_der[pos] & 0x80) {
        pos += 1 + (sig_der[pos] & 0x7F);
    } else {
        pos++;
    }

    /* Parse r */
    if (sig_der[pos++] != 0x02) return false;  /* INTEGER */
    size_t r_len = sig_der[pos++];
    const uint8_t *r_data = sig_der + pos;
    pos += r_len;

    /* Parse s */
    if (pos >= sig_len || sig_der[pos++] != 0x02) return false;
    size_t s_len = sig_der[pos++];
    const uint8_t *s_data = sig_der + pos;

    /* Copy r into 32 bytes (right-aligned, skip leading zeros) */
    memset(out_r, 0, 32);
    if (r_len <= 32) {
        memcpy(out_r + (32 - r_len), r_data, r_len);
    } else {
        /* r has leading zero byte for positive encoding */
        memcpy(out_r, r_data + (r_len - 32), 32);
    }

    /* Copy s into 32 bytes */
    memset(out_s, 0, 32);
    if (s_len <= 32) {
        memcpy(out_s + (32 - s_len), s_data, s_len);
    } else {
        memcpy(out_s, s_data + (s_len - 32), 32);
    }

    /* EIP-2: enforce low-s.
     * secp256k1 order N = FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFEBAAEDCE6AF48A03BBFD25E8CD0364141
     * half_N            = 7FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF5D576E7357A4501DDFE92F46681B20A0
     * If s > half_N, set s = N - s. */
    static const uint8_t HALF_N[32] = {
        0x7F,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
        0x5D,0x57,0x6E,0x73,0x57,0xA4,0x50,0x1D,0xDF,0xE9,0x2F,0x46,0x68,0x1B,0x20,0xA0
    };
    static const uint8_t ORDER_N[32] = {
        0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFE,
        0xBA,0xAE,0xDC,0xE6,0xAF,0x48,0xA0,0x3B,0xBF,0xD2,0x5E,0x8C,0xD0,0x36,0x41,0x41
    };
    /* Compare s > half_N (big-endian) */
    bool s_high = false;
    for (int i = 0; i < 32; i++) {
        if (out_s[i] > HALF_N[i]) { s_high = true; break; }
        if (out_s[i] < HALF_N[i]) break;
    }
    if (s_high) {
        /* s = N - s (big-endian subtraction) */
        int borrow = 0;
        for (int i = 31; i >= 0; i--) {
            int diff = (int)ORDER_N[i] - (int)out_s[i] - borrow;
            if (diff < 0) { diff += 256; borrow = 1; } else { borrow = 0; }
            out_s[i] = (uint8_t)diff;
        }
    }

    /* Default v = 27 (caller may override for recovery ID brute-force) */
    *out_v = 27;

    return true;
}

/* ══════════════════════════════════════════════════════════════════════════
 *  SECTION 5: KALSHI AUTH
 * ══════════════════════════════════════════════════════════════════════════ */

#define KALSHI_TRADE_BASE "https://api.elections.kalshi.com/trade-api/v2"

static bool kalshi_require_auth(char *result, size_t rlen,
                                const char **api_key, const char **key_path) {
    if (!trading_require_key("KALSHI_API_KEY", "Kalshi API key",
                             result, rlen, api_key))
        return false;
    if (!trading_require_key("KALSHI_RSA_PRIVATE_KEY_PATH", "Kalshi RSA private key path",
                             result, rlen, key_path))
        return false;
    return true;
}

static bool kalshi_sign_request(const char *key_path, const char *timestamp_ms,
                                const char *method, const char *path_no_query,
                                char *b64_sig, size_t sig_len) {
    /* message = timestamp_ms + method + "/trade-api/v2" + path_no_query */
    char message[2048];
    snprintf(message, sizeof(message), "%s%s/trade-api/v2%s",
             timestamp_ms, method, path_no_query);

    return rsa_pss_sign(key_path,
                        (const uint8_t *)message, strlen(message),
                        b64_sig, sig_len);
}

static long kalshi_authed_get(const char *path, http_buf_t *out) {
    const char *api_key = getenv("KALSHI_API_KEY");
    const char *key_path = getenv("KALSHI_RSA_PRIVATE_KEY_PATH");
    if (!api_key || !key_path) return -1;

    /* Timestamp in milliseconds */
    struct timeval tv;
    gettimeofday(&tv, NULL);
    char ts[32];
    snprintf(ts, sizeof(ts), "%lld", (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000);

    /* Strip query params for signing */
    char path_no_query[512];
    strncpy(path_no_query, path, sizeof(path_no_query) - 1);
    path_no_query[sizeof(path_no_query) - 1] = '\0';
    char *qmark = strchr(path_no_query, '?');
    if (qmark) *qmark = '\0';

    /* Sign */
    char sig[1024];
    if (!kalshi_sign_request(key_path, ts, "GET", path_no_query, sig, sizeof(sig)))
        return -1;

    /* Build URL */
    char url[1024];
    snprintf(url, sizeof(url), "%s%s", KALSHI_TRADE_BASE, path);

    /* Headers */
    char h_key[256], h_sig[1200], h_ts[64];
    snprintf(h_key, sizeof(h_key), "KALSHI-ACCESS-KEY: %s", api_key);
    snprintf(h_sig, sizeof(h_sig), "KALSHI-ACCESS-SIGNATURE: %s", sig);
    snprintf(h_ts, sizeof(h_ts), "KALSHI-ACCESS-TIMESTAMP: %s", ts);
    const char *hdrs[] = { h_key, h_sig, h_ts };

    return trading_http_request("GET", url, NULL, hdrs, 3, out);
}

static long kalshi_authed_post(const char *path, const char *body, http_buf_t *out) {
    const char *api_key = getenv("KALSHI_API_KEY");
    const char *key_path = getenv("KALSHI_RSA_PRIVATE_KEY_PATH");
    if (!api_key || !key_path) return -1;

    struct timeval tv;
    gettimeofday(&tv, NULL);
    char ts[32];
    snprintf(ts, sizeof(ts), "%lld", (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000);

    char path_no_query[512];
    strncpy(path_no_query, path, sizeof(path_no_query) - 1);
    path_no_query[sizeof(path_no_query) - 1] = '\0';
    char *qmark = strchr(path_no_query, '?');
    if (qmark) *qmark = '\0';

    char sig[1024];
    if (!kalshi_sign_request(key_path, ts, "POST", path_no_query, sig, sizeof(sig)))
        return -1;

    char url[1024];
    snprintf(url, sizeof(url), "%s%s", KALSHI_TRADE_BASE, path);

    char h_key[256], h_sig[1200], h_ts[64];
    snprintf(h_key, sizeof(h_key), "KALSHI-ACCESS-KEY: %s", api_key);
    snprintf(h_sig, sizeof(h_sig), "KALSHI-ACCESS-SIGNATURE: %s", sig);
    snprintf(h_ts, sizeof(h_ts), "KALSHI-ACCESS-TIMESTAMP: %s", ts);
    const char *hdrs[] = { h_key, h_sig, h_ts };

    return trading_http_request("POST", url, body, hdrs, 3, out);
}

static long kalshi_authed_delete(const char *path, http_buf_t *out) {
    const char *api_key = getenv("KALSHI_API_KEY");
    const char *key_path = getenv("KALSHI_RSA_PRIVATE_KEY_PATH");
    if (!api_key || !key_path) return -1;

    struct timeval tv;
    gettimeofday(&tv, NULL);
    char ts[32];
    snprintf(ts, sizeof(ts), "%lld", (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000);

    char path_no_query[512];
    strncpy(path_no_query, path, sizeof(path_no_query) - 1);
    path_no_query[sizeof(path_no_query) - 1] = '\0';
    char *qmark = strchr(path_no_query, '?');
    if (qmark) *qmark = '\0';

    char sig[1024];
    if (!kalshi_sign_request(key_path, ts, "DELETE", path_no_query, sig, sizeof(sig)))
        return -1;

    char url[1024];
    snprintf(url, sizeof(url), "%s%s", KALSHI_TRADE_BASE, path);

    char h_key[256], h_sig[1200], h_ts[64];
    snprintf(h_key, sizeof(h_key), "KALSHI-ACCESS-KEY: %s", api_key);
    snprintf(h_sig, sizeof(h_sig), "KALSHI-ACCESS-SIGNATURE: %s", sig);
    snprintf(h_ts, sizeof(h_ts), "KALSHI-ACCESS-TIMESTAMP: %s", ts);
    const char *hdrs[] = { h_key, h_sig, h_ts };

    return trading_http_request("DELETE", url, NULL, hdrs, 3, out);
}

/* ══════════════════════════════════════════════════════════════════════════
 *  SECTION 6: POLYMARKET AUTH
 * ══════════════════════════════════════════════════════════════════════════ */

#define POLY_CLOB_BASE "https://clob.polymarket.com"
#define POLY_MSG_TO_SIGN "This message attests that I control the given wallet"

static bool poly_require_auth(char *result, size_t rlen,
                              const char **addr, const char **api_key,
                              const char **secret, const char **pass) {
    if (!trading_require_key("POLYMARKET_ADDRESS", "Polymarket wallet address",
                             result, rlen, addr))
        return false;
    if (!trading_require_key("POLYMARKET_API_KEY", "Polymarket API key",
                             result, rlen, api_key))
        return false;
    if (!trading_require_key("POLYMARKET_API_SECRET", "Polymarket API secret",
                             result, rlen, secret))
        return false;
    if (!trading_require_key("POLYMARKET_PASSPHRASE", "Polymarket passphrase",
                             result, rlen, pass))
        return false;
    return true;
}

static bool poly_hmac_sign(const char *api_secret, const char *timestamp,
                           const char *method, const char *path, const char *body,
                           char *b64_sig, size_t sig_len) {
    /* message = timestamp + method + path + (body or "") */
    size_t ts_len = strlen(timestamp);
    size_t m_len = strlen(method);
    size_t p_len = strlen(path);
    size_t b_len = body ? strlen(body) : 0;
    size_t msg_len = ts_len + m_len + p_len + b_len;

    char *message = malloc(msg_len + 1);
    if (!message) return false;
    memcpy(message, timestamp, ts_len);
    memcpy(message + ts_len, method, m_len);
    memcpy(message + ts_len + m_len, path, p_len);
    if (b_len > 0) memcpy(message + ts_len + m_len + p_len, body, b_len);
    message[msg_len] = '\0';

    /* base64_decode(api_secret) -> raw_key */
    uint8_t raw_key[256];
    size_t key_len = base64_decode(api_secret, strlen(api_secret), raw_key, sizeof(raw_key));
    if (key_len == 0) {
        free(message);
        return false;
    }

    /* hmac_sha256(raw_key, raw_key_len, message, message_len, mac) */
    uint8_t mac[32];
    hmac_sha256(raw_key, key_len,
                (const uint8_t *)message, msg_len, mac);
    free(message);

    /* URL-safe base64 encode (matches Python's base64.urlsafe_b64encode) */
    base64url_encode(mac, 32, b64_sig, sig_len);
    return true;
}

static long poly_authed_request(const char *method, const char *path,
                                const char *body, http_buf_t *out) {
    const char *addr, *api_key, *secret, *pass;
    /* We assume env vars are present — caller should have checked */
    addr = getenv("POLYMARKET_ADDRESS");
    api_key = getenv("POLYMARKET_API_KEY");
    secret = getenv("POLYMARKET_API_SECRET");
    pass = getenv("POLYMARKET_PASSPHRASE");
    if (!addr || !api_key || !secret || !pass) return -1;

    /* Timestamp: seconds as string */
    struct timeval tv;
    gettimeofday(&tv, NULL);
    char ts[32];
    snprintf(ts, sizeof(ts), "%lld", (long long)tv.tv_sec);

    /* HMAC signs base path only (no query params) — Polymarket convention */
    char base_path[512];
    strncpy(base_path, path, sizeof(base_path) - 1);
    base_path[sizeof(base_path) - 1] = '\0';
    char *qmark = strchr(base_path, '?');
    if (qmark) *qmark = '\0';

    char sig[256];
    if (!poly_hmac_sign(secret, ts, method, base_path, body, sig, sizeof(sig)))
        return -1;

    /* Build full URL (with query params) */
    char url[1024];
    snprintf(url, sizeof(url), "%s%s", POLY_CLOB_BASE, path);

    /* Headers */
    char h_addr[256], h_sig[512], h_ts[64], h_key[256], h_pass[256];
    snprintf(h_addr, sizeof(h_addr), "POLY_ADDRESS: %s", addr);
    snprintf(h_sig, sizeof(h_sig), "POLY_SIGNATURE: %s", sig);
    snprintf(h_ts, sizeof(h_ts), "POLY_TIMESTAMP: %s", ts);
    snprintf(h_key, sizeof(h_key), "POLY_API_KEY: %s", api_key);
    snprintf(h_pass, sizeof(h_pass), "POLY_PASSPHRASE: %s", pass);
    const char *hdrs[] = { h_addr, h_sig, h_ts, h_key, h_pass };

    return trading_http_request(method, url, body, hdrs, 5, out);
}

/* ══════════════════════════════════════════════════════════════════════════
 *  SECTION 7: EIP-712 ORDER SIGNING
 * ══════════════════════════════════════════════════════════════════════════ */

/* EIP-712 domain separator (cached) */
static bool g_domain_cached = false;
static uint8_t g_domain_sep[32];         /* ClobAuth domain (for L1/L2 headers) */
static uint8_t g_order_domain_sep[32];   /* CTF Exchange domain (for order signing) */
static uint8_t g_neg_risk_domain_sep[32]; /* Neg-risk exchange domain */
static bool g_order_domain_cached = false;

/* Polymarket CTF Exchange contract addresses (Polygon mainnet) */
#define POLY_CTF_EXCHANGE     "0x4bFb41d5B3570DeFd03C39a9A4D8dE6Bd8B8982E"
#define POLY_NEG_RISK_EXCHANGE "0xC5d563A36AE78145C45a50134d48A1215220f80a"

/* Encode a decimal string as a uint256 (32 bytes, big-endian) */
static void abi_encode_uint256_decimal(const char *decimal, uint8_t out[32]) {
    memset(out, 0, 32);
    /* Simple decimal to big-endian 256-bit conversion.
     * Process digit by digit: result = result * 10 + digit */
    for (const char *p = decimal; *p; p++) {
        if (*p < '0' || *p > '9') continue;
        /* Multiply out[0..31] by 10 and add digit */
        uint16_t carry = (uint16_t)(*p - '0');
        for (int i = 31; i >= 0; i--) {
            uint16_t val = (uint16_t)out[i] * 10 + carry;
            out[i] = (uint8_t)(val & 0xFF);
            carry = val >> 8;
        }
    }
}

/* Encode an Ethereum address (0x-prefixed hex) as 32 bytes (left-padded) */
static void abi_encode_address(const char *hex_addr, uint8_t out[32]) {
    memset(out, 0, 32);
    const char *hex = hex_addr;
    if (hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X')) hex += 2;
    size_t hex_len = strlen(hex);
    uint8_t addr[20];
    hex_decode(hex, hex_len, addr, 20);
    /* Address goes in the last 20 bytes */
    memcpy(out + 12, addr, 20);
}

/* Encode a uint8 as 32 bytes (left-padded) */
static void abi_encode_uint8(uint8_t val, uint8_t out[32]) {
    memset(out, 0, 32);
    out[31] = val;
}

static void eip712_compute_domain(void) {
    if (g_domain_cached) return;

    /* typeHash = keccak256("EIP712Domain(string name,string version,uint256 chainId)") */
    const char *domain_type = "EIP712Domain(string name,string version,uint256 chainId)";
    uint8_t type_hash[32];
    keccak256((const uint8_t *)domain_type, strlen(domain_type), type_hash);

    /* nameHash = keccak256("ClobAuthDomain") */
    const char *name = "ClobAuthDomain";
    uint8_t name_hash[32];
    keccak256((const uint8_t *)name, strlen(name), name_hash);

    /* versionHash = keccak256("1") */
    const char *version = "1";
    uint8_t version_hash[32];
    keccak256((const uint8_t *)version, strlen(version), version_hash);

    /* chainId = 137 (Polygon), encoded as uint256 */
    uint8_t chain_id[32];
    abi_encode_uint256_decimal("137", chain_id);

    /* domainSep = keccak256(typeHash || nameHash || versionHash || chainId) */
    uint8_t buf[128]; /* 4 * 32 = 128 */
    memcpy(buf, type_hash, 32);
    memcpy(buf + 32, name_hash, 32);
    memcpy(buf + 64, version_hash, 32);
    memcpy(buf + 96, chain_id, 32);
    keccak256(buf, 128, g_domain_sep);

    g_domain_cached = true;
}

/* Order domain: "Polymarket CTF Exchange" / version "1" / chainId 137 / verifyingContract */
static void eip712_compute_order_domain(void) {
    if (g_order_domain_cached) return;

    /* EIP712Domain WITH verifyingContract */
    const char *dt = "EIP712Domain(string name,string version,uint256 chainId,address verifyingContract)";
    uint8_t th[32]; keccak256((const uint8_t *)dt, strlen(dt), th);

    const char *nm = "Polymarket CTF Exchange";
    uint8_t nh[32]; keccak256((const uint8_t *)nm, strlen(nm), nh);

    uint8_t vh[32]; keccak256((const uint8_t *)"1", 1, vh);

    uint8_t ci[32]; abi_encode_uint256_decimal("137", ci);

    /* Standard exchange */
    uint8_t addr1[32]; abi_encode_address(POLY_CTF_EXCHANGE, addr1);
    uint8_t buf[160];
    memcpy(buf, th, 32); memcpy(buf+32, nh, 32); memcpy(buf+64, vh, 32);
    memcpy(buf+96, ci, 32); memcpy(buf+128, addr1, 32);
    keccak256(buf, 160, g_order_domain_sep);

    /* Neg-risk exchange */
    uint8_t addr2[32]; abi_encode_address(POLY_NEG_RISK_EXCHANGE, addr2);
    memcpy(buf+128, addr2, 32);
    keccak256(buf, 160, g_neg_risk_domain_sep);

    g_order_domain_cached = true;
}

static void eip712_order_struct_hash(const poly_order_t *order, uint8_t out[32]) {
    /* Order type string for EIP-712 struct hashing */
    const char *order_type =
        "Order(uint256 salt,address maker,address signer,address taker,"
        "uint256 tokenId,uint256 makerAmount,uint256 takerAmount,"
        "uint256 expiration,uint256 nonce,uint256 feeRateBps,"
        "uint8 side,uint8 signatureType)";
    uint8_t type_hash[32];
    keccak256((const uint8_t *)order_type, strlen(order_type), type_hash);

    /* ABI-encode each field as 32 bytes */
    /* 13 fields: typeHash + 12 order fields = 13 * 32 = 416 bytes */
    uint8_t encoded[416];
    size_t off = 0;

    /* typeHash */
    memcpy(encoded + off, type_hash, 32); off += 32;

    /* salt (uint256) */
    abi_encode_uint256_decimal(order->salt, encoded + off); off += 32;

    /* maker (address) */
    abi_encode_address(order->maker, encoded + off); off += 32;

    /* signer (address) */
    abi_encode_address(order->signer, encoded + off); off += 32;

    /* taker (address) */
    abi_encode_address(order->taker, encoded + off); off += 32;

    /* tokenId (uint256) */
    abi_encode_uint256_decimal(order->token_id, encoded + off); off += 32;

    /* makerAmount (uint256) */
    abi_encode_uint256_decimal(order->maker_amount, encoded + off); off += 32;

    /* takerAmount (uint256) */
    abi_encode_uint256_decimal(order->taker_amount, encoded + off); off += 32;

    /* expiration (uint256) */
    abi_encode_uint256_decimal(order->expiration, encoded + off); off += 32;

    /* nonce (uint256) */
    abi_encode_uint256_decimal(order->nonce, encoded + off); off += 32;

    /* feeRateBps (uint256 — yes, encoded as 256 even though it's conceptually small) */
    char fee_str[20];
    snprintf(fee_str, sizeof(fee_str), "%d", order->fee_rate_bps);
    abi_encode_uint256_decimal(fee_str, encoded + off); off += 32;

    /* side (uint8) */
    abi_encode_uint8((uint8_t)order->side, encoded + off); off += 32;

    /* signatureType (uint8) */
    abi_encode_uint8((uint8_t)order->signature_type, encoded + off); off += 32;

    /* structHash = keccak256(encoded) */
    keccak256(encoded, off, out);
}

static bool poly_sign_order(const uint8_t privkey[32], const poly_order_t *order,
                            bool neg_risk, char *hex_sig, size_t sig_len) {
    /* Compute ORDER domain separator (includes verifyingContract) */
    eip712_compute_order_domain();

    /* Compute struct hash */
    uint8_t struct_hash[32];
    eip712_order_struct_hash(order, struct_hash);

    /* Use the correct domain: standard or neg-risk exchange */
    const uint8_t *domain = neg_risk ? g_neg_risk_domain_sep : g_order_domain_sep;

    /* digest = keccak256(0x19 0x01 || domainSep || structHash) */
    uint8_t digest_input[66]; /* 2 + 32 + 32 */
    digest_input[0] = 0x19;
    digest_input[1] = 0x01;
    memcpy(digest_input + 2, domain, 32);
    memcpy(digest_input + 34, struct_hash, 32);

    uint8_t digest[32];
    keccak256(digest_input, 66, digest);

    /* Sign digest using Python eth_account (gives correct recovery ID).
     * OpenSSL pkeyutl doesn't return the ECDSA recovery ID (v),
     * which is required for Ethereum signature verification. */
    {
        char pk_hex[65];
        hex_encode(privkey, 32, pk_hex);
        char digest_hex[65];
        hex_encode(digest, 32, digest_hex);

        char cmd[512];
        snprintf(cmd, sizeof(cmd),
                 "python3 -c \""
                 "from eth_account import Account;"
                 "s=Account._sign_hash('0x%s','0x%s');"
                 "print(s.signature.hex())\" 2>/dev/null",
                 digest_hex, pk_hex);
        FILE *fp = popen(cmd, "r");
        if (!fp) return false;

        char sig_out[200] = {0};
        if (!fgets(sig_out, sizeof(sig_out), fp)) {
            pclose(fp);
            return false;
        }
        pclose(fp);

        /* Trim whitespace */
        size_t slen = strlen(sig_out);
        while (slen > 0 && (sig_out[slen-1] == '\n' || sig_out[slen-1] == '\r'))
            sig_out[--slen] = '\0';

        if (slen < 130) return false;

        /* Format: "0x" + 130 hex chars */
        if (sig_len < 133) return false;
        snprintf(hex_sig, sig_len, "0x%s", sig_out);
    }

    return true;
}

/* ══════════════════════════════════════════════════════════════════════════
 *  SECTION 8: RISK MANAGEMENT
 * ══════════════════════════════════════════════════════════════════════════ */

static risk_limits_t g_risk = {0};
static bool g_risk_inited = false;

static void risk_init(void) {
    if (g_risk_inited) return;
    g_risk = risk_limits_default();
    const char *e;
    if ((e = getenv("DSCO_TRADING_DRY_RUN")))
        g_risk.dry_run = !(strcmp(e, "0") == 0 || strcasecmp(e, "false") == 0 || strcasecmp(e, "off") == 0 || strcasecmp(e, "no") == 0);
    if ((e = getenv("DSCO_TRADING_MAX_ORDER")))
        g_risk.max_order_usd = atof(e);
    if ((e = getenv("DSCO_TRADING_MAX_EXPOSURE")))
        g_risk.max_total_exposure_usd = atof(e);
    if ((e = getenv("DSCO_TRADING_MIN_ARB_SPREAD")))
        g_risk.min_arb_spread = atof(e);
    if ((e = getenv("DSCO_TRADING_MAX_OPEN_ORDERS"))) {
        int v = atoi(e);
        if (v > 0) g_risk.max_open_orders = v;
    }
    if ((e = getenv("DSCO_TRADING_MAX_POSITION")))
        g_risk.max_position_usd = atof(e);
    g_risk_inited = true;
}

static bool risk_preflight(double order_usd, char *reason, size_t reason_len) {
    risk_init();
    if (order_usd > g_risk.max_order_usd) {
        snprintf(reason, reason_len,
                 "order $%.2f exceeds max $%.2f", order_usd, g_risk.max_order_usd);
        return false;
    }
    if (order_usd > g_risk.max_total_exposure_usd) {
        snprintf(reason, reason_len,
                 "order $%.2f exceeds max total exposure $%.2f",
                 order_usd, g_risk.max_total_exposure_usd);
        return false;
    }
    if (order_usd <= 0.0) {
        snprintf(reason, reason_len, "order size must be positive");
        return false;
    }
    return true;
}

/* ══════════════════════════════════════════════════════════════════════════
 *  SECTION 9: KALSHI TRADING TOOLS
 * ══════════════════════════════════════════════════════════════════════════ */

/* ── tool_kalshi_balance ─────────────────────────────────────────────── */

bool tool_kalshi_balance(const char *input, char *result, size_t rlen) {
    (void)input;
    const char *api_key, *key_path;
    if (!kalshi_require_auth(result, rlen, &api_key, &key_path))
        return false;

    http_buf_t resp = {0};
    long code = kalshi_authed_get("/portfolio/balance", &resp);
    if (code < 0 || !resp.data) {
        snprintf(result, rlen, "Kalshi API request failed (code=%ld)", code);
        free(resp.data);
        return false;
    }

    if (code != 200) {
        snprintf(result, rlen, "Kalshi API error %ld: ", code);
        trading_truncate(resp.data, result, rlen, 64);
        free(resp.data);
        return false;
    }

    /* Parse balance (cents) and portfolio_value */
    double balance = json_get_double(resp.data, "balance", 0.0);
    double portfolio_value = json_get_double(resp.data, "portfolio_value", 0.0);

    /* Balance is in cents, convert to USD */
    double balance_usd = balance / 100.0;
    double portfolio_usd = portfolio_value / 100.0;

    snprintf(result, rlen,
             "{\"platform\":\"kalshi\",\"balance_usd\":%.2f,\"portfolio_value_usd\":%.2f}",
             balance_usd, portfolio_usd);

    free(resp.data);
    return true;
}

/* ── tool_kalshi_positions ───────────────────────────────────────────── */

bool tool_kalshi_positions(const char *input, char *result, size_t rlen) {
    (void)input;
    const char *api_key, *key_path;
    if (!kalshi_require_auth(result, rlen, &api_key, &key_path))
        return false;

    http_buf_t resp = {0};
    long code = kalshi_authed_get("/portfolio/positions?count_filter=position&limit=100", &resp);
    if (code < 0 || !resp.data) {
        snprintf(result, rlen, "Kalshi positions request failed (code=%ld)", code);
        free(resp.data);
        return false;
    }

    if (code != 200) {
        snprintf(result, rlen, "Kalshi API error %ld: ", code);
        trading_truncate(resp.data, result, rlen, 64);
        free(resp.data);
        return false;
    }

    /* Pass through JSON */
    result[0] = '\0';
    trading_truncate(resp.data, result, rlen, 16);
    free(resp.data);
    return true;
}

/* ── tool_kalshi_portfolio ───────────────────────────────────────────── */

bool tool_kalshi_portfolio(const char *input, char *result, size_t rlen) {
    (void)input;
    const char *api_key, *key_path;
    if (!kalshi_require_auth(result, rlen, &api_key, &key_path))
        return false;

    /* Fetch balance */
    http_buf_t bal_resp = {0};
    long bal_code = kalshi_authed_get("/portfolio/balance", &bal_resp);

    /* Fetch positions */
    http_buf_t pos_resp = {0};
    long pos_code = kalshi_authed_get("/portfolio/positions?count_filter=position&limit=100", &pos_resp);

    if (bal_code < 0 || pos_code < 0 || !bal_resp.data || !pos_resp.data) {
        snprintf(result, rlen, "Kalshi portfolio request failed");
        free(bal_resp.data);
        free(pos_resp.data);
        return false;
    }

    if (bal_code != 200) {
        snprintf(result, rlen, "Kalshi balance error %ld: ", bal_code);
        trading_truncate(bal_resp.data, result, rlen, 64);
        free(bal_resp.data);
        free(pos_resp.data);
        return false;
    }

    double balance = json_get_double(bal_resp.data, "balance", 0.0);
    double portfolio_value = json_get_double(bal_resp.data, "portfolio_value", 0.0);
    double balance_usd = balance / 100.0;
    double portfolio_usd = portfolio_value / 100.0;

    jbuf_t jb;
    jbuf_init(&jb, 4096);
    jbuf_append(&jb, "{\"platform\":\"kalshi\"");
    jbuf_appendf(&jb, ",\"balance_usd\":%.2f", balance_usd);
    jbuf_appendf(&jb, ",\"portfolio_value_usd\":%.2f", portfolio_usd);
    jbuf_append(&jb, ",\"positions\":");

    /* Extract positions array if present */
    char *positions_raw = json_get_raw(bal_resp.data, "market_positions");
    if (!positions_raw) positions_raw = json_get_raw(pos_resp.data, "market_positions");
    if (positions_raw) {
        jbuf_append(&jb, positions_raw);
        free(positions_raw);
    } else {
        /* Fall back: embed full positions response */
        jbuf_append(&jb, pos_resp.data);
    }
    jbuf_append(&jb, "}");

    snprintf(result, rlen, "%s", jb.data);
    jbuf_free(&jb);
    free(bal_resp.data);
    free(pos_resp.data);
    return true;
}

/* ── tool_kalshi_fills ───────────────────────────────────────────────── */

bool tool_kalshi_fills(const char *input, char *result, size_t rlen) {
    const char *api_key, *key_path;
    if (!kalshi_require_auth(result, rlen, &api_key, &key_path))
        return false;

    char *ticker = json_get_str(input, "ticker");
    int limit = json_get_int(input, "limit", 100);
    if (limit < 1) limit = 1;
    if (limit > 1000) limit = 1000;

    char path[512];
    if (ticker && ticker[0]) {
        snprintf(path, sizeof(path), "/portfolio/fills?ticker=%s&limit=%d", ticker, limit);
    } else {
        snprintf(path, sizeof(path), "/portfolio/fills?limit=%d", limit);
    }
    free(ticker);

    http_buf_t resp = {0};
    long code = kalshi_authed_get(path, &resp);
    if (code < 0 || !resp.data) {
        snprintf(result, rlen, "Kalshi fills request failed (code=%ld)", code);
        free(resp.data);
        return false;
    }

    if (code != 200) {
        snprintf(result, rlen, "Kalshi API error %ld: ", code);
        trading_truncate(resp.data, result, rlen, 64);
        free(resp.data);
        return false;
    }

    result[0] = '\0';
    trading_truncate(resp.data, result, rlen, 16);
    free(resp.data);
    return true;
}

/* ── tool_kalshi_create_order ────────────────────────────────────────── */

bool tool_kalshi_create_order(const char *input, char *result, size_t rlen) {
    const char *api_key, *key_path;
    if (!kalshi_require_auth(result, rlen, &api_key, &key_path))
        return false;

    risk_init();

    /* Parse required params */
    char *ticker = json_get_str(input, "ticker");
    char *action = json_get_str(input, "action");
    char *side = json_get_str(input, "side");
    int count = json_get_int(input, "count", 0);

    if (!ticker || !ticker[0]) {
        snprintf(result, rlen, "missing required parameter: ticker");
        free(ticker); free(action); free(side);
        return false;
    }
    if (!action || (strcmp(action, "buy") != 0 && strcmp(action, "sell") != 0)) {
        snprintf(result, rlen, "invalid action: must be 'buy' or 'sell'");
        free(ticker); free(action); free(side);
        return false;
    }
    if (!side || (strcmp(side, "yes") != 0 && strcmp(side, "no") != 0)) {
        snprintf(result, rlen, "invalid side: must be 'yes' or 'no'");
        free(ticker); free(action); free(side);
        return false;
    }
    if (count <= 0) {
        snprintf(result, rlen, "count must be a positive integer");
        free(ticker); free(action); free(side);
        return false;
    }

    /* Optional params */
    char *type_str = json_get_str(input, "type");
    int yes_price = json_get_int(input, "yes_price", 0);
    bool is_limit = (type_str && strcmp(type_str, "limit") == 0);

    if (is_limit && (yes_price < 1 || yes_price > 99)) {
        snprintf(result, rlen, "limit order requires yes_price between 1 and 99 (cents)");
        free(ticker); free(action); free(side); free(type_str);
        return false;
    }

    /* Risk check: estimate order value in USD (price in cents * count / 100) */
    double price_est = is_limit ? (double)yes_price : 50.0; /* assume 50c for market */
    double order_usd = price_est * (double)count / 100.0;
    char risk_reason[256] = {0};
    if (!risk_preflight(order_usd, risk_reason, sizeof(risk_reason))) {
        snprintf(result, rlen, "{\"error\":\"risk_check_failed\",\"reason\":\"%s\"}", risk_reason);
        free(ticker); free(action); free(side); free(type_str);
        return false;
    }

    /* Generate client_order_id */
    char client_oid[37];
    uuid_v4(client_oid);

    /* Dry run check */
    if (g_risk.dry_run) {
        snprintf(result, rlen,
                 "{\"dry_run\":true,\"platform\":\"kalshi\","
                 "\"ticker\":\"%s\",\"action\":\"%s\",\"side\":\"%s\","
                 "\"count\":%d,\"type\":\"%s\""
                 "%s%s"
                 ",\"client_order_id\":\"%s\""
                 ",\"estimated_usd\":%.2f}",
                 ticker, action, side, count,
                 is_limit ? "limit" : "market",
                 is_limit ? ",\"yes_price\":" : "",
                 is_limit ? "" : "",
                 client_oid, order_usd);
        /* Patch in the yes_price if limit */
        if (is_limit) {
            char tmp[32];
            snprintf(tmp, sizeof(tmp), "%d", yes_price);
            /* Re-format cleanly */
            snprintf(result, rlen,
                     "{\"dry_run\":true,\"platform\":\"kalshi\","
                     "\"ticker\":\"%s\",\"action\":\"%s\",\"side\":\"%s\","
                     "\"count\":%d,\"type\":\"limit\",\"yes_price\":%d,"
                     "\"client_order_id\":\"%s\",\"estimated_usd\":%.2f}",
                     ticker, action, side, count, yes_price, client_oid, order_usd);
        }
        free(ticker); free(action); free(side); free(type_str);
        return true;
    }

    /* Build order body */
    jbuf_t body;
    jbuf_init(&body, 512);
    jbuf_append(&body, "{");
    jbuf_appendf(&body, "\"ticker\":\"%s\"", ticker);
    jbuf_appendf(&body, ",\"client_order_id\":\"%s\"", client_oid);
    jbuf_appendf(&body, ",\"action\":\"%s\"", action);
    jbuf_appendf(&body, ",\"side\":\"%s\"", side);
    jbuf_appendf(&body, ",\"count\":%d", count);
    jbuf_appendf(&body, ",\"type\":\"%s\"", is_limit ? "limit" : "market");
    if (is_limit) {
        jbuf_appendf(&body, ",\"yes_price\":%d", yes_price);
    }
    jbuf_append(&body, "}");

    http_buf_t resp = {0};
    long code = kalshi_authed_post("/portfolio/orders", body.data, &resp);
    jbuf_free(&body);

    if (code < 0 || !resp.data) {
        snprintf(result, rlen, "Kalshi create order request failed (code=%ld)", code);
        free(resp.data);
        free(ticker); free(action); free(side); free(type_str);
        return false;
    }

    if (code != 200 && code != 201) {
        snprintf(result, rlen, "Kalshi order error %ld: ", code);
        trading_truncate(resp.data, result, rlen, 64);
        free(resp.data);
        free(ticker); free(action); free(side); free(type_str);
        return false;
    }

    result[0] = '\0';
    trading_truncate(resp.data, result, rlen, 16);
    free(resp.data);
    free(ticker); free(action); free(side); free(type_str);
    return true;
}

/* ── tool_kalshi_batch_create_orders ─────────────────────────────────── */

typedef struct {
    jbuf_t *orders_buf;
    int count;
    bool error;
    char error_msg[256];
    double max_order_usd;
} batch_ctx_t;

static void batch_order_cb(const char *element_start, void *ctx) {
    batch_ctx_t *bc = (batch_ctx_t *)ctx;
    if (bc->error || bc->count >= 20) return;

    /* Validate each order element */
    char *ticker = json_get_str(element_start, "ticker");
    char *action = json_get_str(element_start, "action");
    char *side = json_get_str(element_start, "side");
    int count = json_get_int(element_start, "count", 0);

    if (!ticker || !ticker[0] || !action || !side || count <= 0) {
        bc->error = true;
        snprintf(bc->error_msg, sizeof(bc->error_msg),
                 "invalid order at index %d: missing required fields", bc->count);
        free(ticker); free(action); free(side);
        return;
    }

    char *type_str = json_get_str(element_start, "type");
    int yes_price = json_get_int(element_start, "yes_price", 0);
    bool is_limit = (type_str && strcmp(type_str, "limit") == 0);

    char client_oid[37];
    uuid_v4(client_oid);

    if (bc->count > 0) jbuf_append(bc->orders_buf, ",");
    jbuf_append(bc->orders_buf, "{");
    jbuf_appendf(bc->orders_buf, "\"ticker\":\"%s\"", ticker);
    jbuf_appendf(bc->orders_buf, ",\"client_order_id\":\"%s\"", client_oid);
    jbuf_appendf(bc->orders_buf, ",\"action\":\"%s\"", action);
    jbuf_appendf(bc->orders_buf, ",\"side\":\"%s\"", side);
    jbuf_appendf(bc->orders_buf, ",\"count\":%d", count);
    jbuf_appendf(bc->orders_buf, ",\"type\":\"%s\"", is_limit ? "limit" : "market");
    if (is_limit && yes_price >= 1 && yes_price <= 99) {
        jbuf_appendf(bc->orders_buf, ",\"yes_price\":%d", yes_price);
    }
    jbuf_append(bc->orders_buf, "}");

    bc->count++;

    free(ticker); free(action); free(side); free(type_str);
}

bool tool_kalshi_batch_create_orders(const char *input, char *result, size_t rlen) {
    const char *api_key, *key_path;
    if (!kalshi_require_auth(result, rlen, &api_key, &key_path))
        return false;

    risk_init();

    jbuf_t orders_buf;
    jbuf_init(&orders_buf, 2048);
    jbuf_append(&orders_buf, "[");

    batch_ctx_t bc = {
        .orders_buf = &orders_buf,
        .count = 0,
        .error = false,
        .max_order_usd = g_risk.max_order_usd,
    };

    int n = json_array_foreach(input, "orders", batch_order_cb, &bc);

    if (n < 0 || bc.count == 0) {
        snprintf(result, rlen, "missing or empty 'orders' array (max 20 orders)");
        jbuf_free(&orders_buf);
        return false;
    }
    if (bc.error) {
        snprintf(result, rlen, "%s", bc.error_msg);
        jbuf_free(&orders_buf);
        return false;
    }
    if (bc.count > 20) {
        snprintf(result, rlen, "batch limit exceeded: max 20 orders, got %d", bc.count);
        jbuf_free(&orders_buf);
        return false;
    }

    jbuf_append(&orders_buf, "]");

    /* Dry run check */
    if (g_risk.dry_run) {
        snprintf(result, rlen,
                 "{\"dry_run\":true,\"platform\":\"kalshi\","
                 "\"batch_count\":%d,\"orders\":%s}",
                 bc.count, orders_buf.data);
        jbuf_free(&orders_buf);
        return true;
    }

    /* Build body: {"orders": [...]} */
    jbuf_t body;
    jbuf_init(&body, 2048);
    jbuf_append(&body, "{\"orders\":");
    jbuf_append(&body, orders_buf.data);
    jbuf_append(&body, "}");
    jbuf_free(&orders_buf);

    http_buf_t resp = {0};
    long code = kalshi_authed_post("/portfolio/orders/batched", body.data, &resp);
    jbuf_free(&body);

    if (code < 0 || !resp.data) {
        snprintf(result, rlen, "Kalshi batch order request failed (code=%ld)", code);
        free(resp.data);
        return false;
    }

    if (code != 200 && code != 201) {
        snprintf(result, rlen, "Kalshi batch error %ld: ", code);
        trading_truncate(resp.data, result, rlen, 64);
        free(resp.data);
        return false;
    }

    result[0] = '\0';
    trading_truncate(resp.data, result, rlen, 16);
    free(resp.data);
    return true;
}

/* ── tool_kalshi_cancel_order ────────────────────────────────────────── */

bool tool_kalshi_cancel_order(const char *input, char *result, size_t rlen) {
    const char *api_key, *key_path;
    if (!kalshi_require_auth(result, rlen, &api_key, &key_path))
        return false;

    char *order_id = json_get_str(input, "order_id");
    if (!order_id || !order_id[0]) {
        snprintf(result, rlen, "missing required parameter: order_id");
        free(order_id);
        return false;
    }

    char path[256];
    snprintf(path, sizeof(path), "/portfolio/orders/%s", order_id);
    free(order_id);

    http_buf_t resp = {0};
    long code = kalshi_authed_delete(path, &resp);
    if (code < 0 || !resp.data) {
        snprintf(result, rlen, "Kalshi cancel request failed (code=%ld)", code);
        free(resp.data);
        return false;
    }

    if (code != 200 && code != 204) {
        snprintf(result, rlen, "Kalshi cancel error %ld: ", code);
        trading_truncate(resp.data, result, rlen, 64);
        free(resp.data);
        return false;
    }

    if (resp.len > 0 && resp.data[0] == '{') {
        result[0] = '\0';
        trading_truncate(resp.data, result, rlen, 16);
    } else {
        snprintf(result, rlen, "{\"status\":\"cancelled\"}");
    }
    free(resp.data);
    return true;
}

/* ── tool_kalshi_cancel_all ──────────────────────────────────────────── */

bool tool_kalshi_cancel_all(const char *input, char *result, size_t rlen) {
    (void)input;
    const char *api_key, *key_path;
    if (!kalshi_require_auth(result, rlen, &api_key, &key_path))
        return false;

    /* First get open orders */
    http_buf_t orders_resp = {0};
    long code = kalshi_authed_get("/portfolio/orders?status=resting", &orders_resp);
    if (code < 0 || !orders_resp.data) {
        snprintf(result, rlen, "Kalshi get orders failed (code=%ld)", code);
        free(orders_resp.data);
        return false;
    }

    if (code != 200) {
        snprintf(result, rlen, "Kalshi orders error %ld: ", code);
        trading_truncate(orders_resp.data, result, rlen, 64);
        free(orders_resp.data);
        return false;
    }

    /* Collect order IDs and cancel each one */
    typedef struct {
        int cancelled;
        int failed;
    } cancel_ctx_t;

    (void)sizeof(cancel_ctx_t); /* type used below */

    /* Simple approach: iterate through orders array and cancel each */
    /* We use a callback to extract order_id and cancel */
    struct cancel_iter_ctx {
        int cancelled;
        int failed;
        char last_error[256];
    } iter_ctx = { .cancelled = 0, .failed = 0 };

    /* Parse each order and cancel */
    /* We'll just extract order IDs via a simple scan */
    const char *search = orders_resp.data;
    while ((search = strstr(search, "\"order_id\"")) != NULL) {
        search += 10; /* skip past "order_id" */
        /* Skip whitespace and colon */
        while (*search && (*search == ' ' || *search == ':' || *search == '"')) search++;
        if (!*search) break;

        char oid[128];
        int i = 0;
        while (*search && *search != '"' && i < 126) {
            oid[i++] = *search++;
        }
        oid[i] = '\0';

        if (i > 0) {
            char del_path[256];
            snprintf(del_path, sizeof(del_path), "/portfolio/orders/%s", oid);
            http_buf_t del_resp = {0};
            long del_code = kalshi_authed_delete(del_path, &del_resp);
            if (del_code == 200 || del_code == 204) {
                iter_ctx.cancelled++;
            } else {
                iter_ctx.failed++;
                if (del_resp.data) {
                    snprintf(iter_ctx.last_error, sizeof(iter_ctx.last_error),
                             "%s", del_resp.data);
                }
            }
            free(del_resp.data);
        }
    }

    free(orders_resp.data);

    snprintf(result, rlen,
             "{\"platform\":\"kalshi\",\"cancelled\":%d,\"failed\":%d%s%s}",
             iter_ctx.cancelled, iter_ctx.failed,
             iter_ctx.failed > 0 ? ",\"last_error\":\"" : "",
             iter_ctx.failed > 0 ? iter_ctx.last_error : "");
    /* Close the quotes properly if there was an error */
    if (iter_ctx.failed > 0) {
        size_t len = strlen(result);
        if (len > 0 && result[len-1] != '}') {
            if (len + 2 < rlen) {
                result[len] = '"';
                result[len+1] = '}';
                result[len+2] = '\0';
            }
        }
    }

    return true;
}

/* ── tool_kalshi_amend_order ─────────────────────────────────────────── */

bool tool_kalshi_amend_order(const char *input, char *result, size_t rlen) {
    const char *api_key, *key_path;
    if (!kalshi_require_auth(result, rlen, &api_key, &key_path))
        return false;

    char *order_id = json_get_str(input, "order_id");
    if (!order_id || !order_id[0]) {
        snprintf(result, rlen, "missing required parameter: order_id");
        free(order_id);
        return false;
    }

    int count = json_get_int(input, "count", -1);
    int price = json_get_int(input, "price", -1);

    if (count < 0 && price < 0) {
        snprintf(result, rlen, "must specify at least one of: count, price");
        free(order_id);
        return false;
    }

    /* Build amend body */
    jbuf_t body;
    jbuf_init(&body, 256);
    jbuf_append(&body, "{");
    bool first = true;
    if (count >= 0) {
        jbuf_appendf(&body, "\"count\":%d", count);
        first = false;
    }
    if (price >= 0) {
        if (!first) jbuf_append(&body, ",");
        jbuf_appendf(&body, "\"price\":%d", price);
    }
    jbuf_append(&body, "}");

    char path[256];
    snprintf(path, sizeof(path), "/portfolio/orders/%s/amend", order_id);
    free(order_id);

    http_buf_t resp = {0};
    long code = kalshi_authed_post(path, body.data, &resp);
    jbuf_free(&body);

    if (code < 0 || !resp.data) {
        snprintf(result, rlen, "Kalshi amend request failed (code=%ld)", code);
        free(resp.data);
        return false;
    }

    if (code != 200 && code != 201) {
        snprintf(result, rlen, "Kalshi amend error %ld: ", code);
        trading_truncate(resp.data, result, rlen, 64);
        free(resp.data);
        return false;
    }

    result[0] = '\0';
    trading_truncate(resp.data, result, rlen, 16);
    free(resp.data);
    return true;
}

/* ── tool_kalshi_open_orders ─────────────────────────────────────────── */

bool tool_kalshi_open_orders(const char *input, char *result, size_t rlen) {
    (void)input;
    const char *api_key, *key_path;
    if (!kalshi_require_auth(result, rlen, &api_key, &key_path))
        return false;

    http_buf_t resp = {0};
    long code = kalshi_authed_get("/portfolio/orders?status=resting", &resp);
    if (code < 0 || !resp.data) {
        snprintf(result, rlen, "Kalshi open orders request failed (code=%ld)", code);
        free(resp.data);
        return false;
    }

    if (code != 200) {
        snprintf(result, rlen, "Kalshi API error %ld: ", code);
        trading_truncate(resp.data, result, rlen, 64);
        free(resp.data);
        return false;
    }

    result[0] = '\0';
    trading_truncate(resp.data, result, rlen, 16);
    free(resp.data);
    return true;
}

/* ══════════════════════════════════════════════════════════════════════════
 *  SECTION 10: POLYMARKET TRADING TOOLS
 * ══════════════════════════════════════════════════════════════════════════ */

/* ── tool_polymarket_balance ─────────────────────────────────────────── */

bool tool_polymarket_balance(const char *input, char *result, size_t rlen) {
    (void)input;
    const char *addr, *api_key, *secret, *pass;
    if (!poly_require_auth(result, rlen, &addr, &api_key, &secret, &pass))
        return false;

    http_buf_t resp = {0};
    long code = poly_authed_request("GET", "/balance-allowance?asset_type=COLLATERAL&signature_type=1",
                                    NULL, &resp);
    if (code < 0 || !resp.data) {
        snprintf(result, rlen, "Polymarket balance request failed (code=%ld)", code);
        free(resp.data);
        return false;
    }

    if (code != 200) {
        snprintf(result, rlen, "Polymarket API error %ld: ", code);
        trading_truncate(resp.data, result, rlen, 64);
        free(resp.data);
        return false;
    }

    /* Parse balance — it's a string in wei (USDC has 6 decimals) */
    char *balance_str = json_get_str(resp.data, "balance");
    double balance_usdc = 0.0;
    if (balance_str) {
        double wei = atof(balance_str);
        balance_usdc = wei / 1e6;
        free(balance_str);
    }

    char *allowance_str = json_get_str(resp.data, "allowance");
    double allowance_usdc = 0.0;
    if (allowance_str) {
        double wei = atof(allowance_str);
        allowance_usdc = wei / 1e6;
        free(allowance_str);
    }

    snprintf(result, rlen,
             "{\"platform\":\"polymarket\",\"balance_usdc\":%.6f,\"allowance_usdc\":%.6f}",
             balance_usdc, allowance_usdc);

    free(resp.data);
    return true;
}

/* ── tool_polymarket_positions ───────────────────────────────────────── */

bool tool_polymarket_positions(const char *input, char *result, size_t rlen) {
    (void)input;
    /* Prefer proxy address (where positions live) over signer address */
    const char *addr = getenv("POLYMARKET_PROXY_ADDRESS");
    if (!addr || !addr[0])
        addr = getenv("POLYMARKET_ADDRESS");
    if (!addr || !addr[0]) {
        snprintf(result, rlen,
                 "Polymarket wallet address not set. Run: export POLYMARKET_PROXY_ADDRESS=0x...");
        return false;
    }

    /* Public data API — no auth needed */
    char url[512];
    snprintf(url, sizeof(url),
             "https://data-api.polymarket.com/positions?user=%s", addr);

    http_buf_t resp = {0};
    long code = trading_http_request("GET", url, NULL, NULL, 0, &resp);
    if (code < 0 || !resp.data) {
        snprintf(result, rlen, "Polymarket positions request failed (code=%ld)", code);
        free(resp.data);
        return false;
    }

    if (code != 200) {
        snprintf(result, rlen, "Polymarket data API error %ld: ", code);
        trading_truncate(resp.data, result, rlen, 64);
        free(resp.data);
        return false;
    }

    result[0] = '\0';
    trading_truncate(resp.data, result, rlen, 16);
    free(resp.data);
    return true;
}

/* ── tool_polymarket_open_orders ─────────────────────────────────────── */

bool tool_polymarket_open_orders(const char *input, char *result, size_t rlen) {
    (void)input;
    const char *addr, *api_key, *secret, *pass;
    if (!poly_require_auth(result, rlen, &addr, &api_key, &secret, &pass))
        return false;

    http_buf_t resp = {0};
    long code = poly_authed_request("GET", "/orders", NULL, &resp);
    if (code < 0 || !resp.data) {
        snprintf(result, rlen, "Polymarket open orders request failed (code=%ld)", code);
        free(resp.data);
        return false;
    }

    if (code != 200) {
        snprintf(result, rlen, "Polymarket API error %ld: ", code);
        trading_truncate(resp.data, result, rlen, 64);
        free(resp.data);
        return false;
    }

    result[0] = '\0';
    trading_truncate(resp.data, result, rlen, 16);
    free(resp.data);
    return true;
}

/* ── tool_polymarket_api_keys ────────────────────────────────────────── */

bool tool_polymarket_api_keys(const char *input, char *result, size_t rlen) {
    (void)input;
    const char *addr, *api_key, *secret, *pass;
    if (!poly_require_auth(result, rlen, &addr, &api_key, &secret, &pass))
        return false;

    /* Use L2 HMAC auth to list API keys (most users already have keys) */
    http_buf_t resp = {0};
    long code = poly_authed_request("GET", "/auth/api-keys", NULL, &resp);
    if (code < 0 || !resp.data) {
        snprintf(result, rlen, "Polymarket api-keys request failed (code=%ld)", code);
        free(resp.data);
        return false;
    }

    if (code != 200) {
        snprintf(result, rlen, "Polymarket API error %ld: ", code);
        trading_truncate(resp.data, result, rlen, 64);
        free(resp.data);
        return false;
    }

    result[0] = '\0';
    trading_truncate(resp.data, result, rlen, 16);
    free(resp.data);
    return true;
}

/* ── EIP-712 ClobAuth signing (for derive-api-key) ─────────────────── */

/*
 * ClobAuth EIP-712 struct:
 *   ClobAuth(address address,string timestamp,uint256 nonce,string message)
 *
 * Domain: ClobAuthDomain / version "1" / chainId 137 (Polygon).
 * For derive-api-key: nonce=0, message=POLY_MSG_TO_SIGN.
 */
static bool poly_sign_clob_auth(const uint8_t privkey[32], const char *address,
                                 const char *timestamp, int nonce,
                                 const char *message, uint8_t forced_v,
                                 char *hex_sig, size_t sig_len) {
    /* Same domain as order signing: chainId=137 */
    eip712_compute_domain();

    /* typeHash = keccak256("ClobAuth(address address,string timestamp,uint256 nonce,string message)") */
    const char *auth_type = "ClobAuth(address address,string timestamp,uint256 nonce,string message)";
    uint8_t type_hash[32];
    keccak256((const uint8_t *)auth_type, strlen(auth_type), type_hash);

    /* Encode address as 32 bytes (left-padded) */
    uint8_t addr_encoded[32];
    abi_encode_address(address, addr_encoded);

    /* timestamp is a string type in EIP-712 → hash it */
    uint8_t ts_hash[32];
    keccak256((const uint8_t *)timestamp, strlen(timestamp), ts_hash);

    /* nonce as uint256 */
    uint8_t nonce_encoded[32];
    char nonce_str[20];
    snprintf(nonce_str, sizeof(nonce_str), "%d", nonce);
    abi_encode_uint256_decimal(nonce_str, nonce_encoded);

    /* message is a string type → hash it (empty string = keccak256("")) */
    uint8_t msg_hash[32];
    keccak256((const uint8_t *)message, strlen(message), msg_hash);

    /* structHash = keccak256(typeHash || address || hashedTimestamp || nonce || hashedMessage) */
    uint8_t encoded[160]; /* 5 * 32 = 160 */
    memcpy(encoded,       type_hash,     32);
    memcpy(encoded + 32,  addr_encoded,  32);
    memcpy(encoded + 64,  ts_hash,       32);
    memcpy(encoded + 96,  nonce_encoded, 32);
    memcpy(encoded + 128, msg_hash,      32);

    uint8_t struct_hash[32];
    keccak256(encoded, 160, struct_hash);

    /* digest = keccak256(0x19 0x01 || domainSep || structHash) */
    uint8_t digest_input[66];
    digest_input[0] = 0x19;
    digest_input[1] = 0x01;
    memcpy(digest_input + 2, g_domain_sep, 32);
    memcpy(digest_input + 34, struct_hash, 32);

    uint8_t digest[32];
    keccak256(digest_input, 66, digest);

    /* Sign with secp256k1 */
    uint8_t r[32], s[32], v;
    if (!secp256k1_sign_hash(privkey, digest, r, s, &v))
        return false;

    /* Use forced_v if caller specified (for v recovery brute-force) */
    if (forced_v != 0) v = forced_v;

    /* Pack: 0x + r(32) + s(32) + v(1) = "0x" + 130 hex chars */
    if (sig_len < 133) return false;
    hex_sig[0] = '0';
    hex_sig[1] = 'x';
    hex_encode(r, 32, hex_sig + 2);
    hex_encode(s, 32, hex_sig + 66);
    snprintf(hex_sig + 130, sig_len - 130, "%02x", v);

    return true;
}

/* Update a key=value line in .env file (or append if not found) */
static void env_file_set(const char *env_path, const char *key, const char *value) {
    FILE *f = fopen(env_path, "r");
    char *lines[256];
    int nlines = 0;
    bool found = false;
    size_t key_len = strlen(key);

    if (f) {
        char buf[2048];
        while (nlines < 256 && fgets(buf, sizeof(buf), f)) {
            /* Match uncommented KEY= lines */
            if (strncmp(buf, key, key_len) == 0 && buf[key_len] == '=') {
                char *newline = malloc(key_len + strlen(value) + 3);
                sprintf(newline, "%s=%s\n", key, value);
                lines[nlines++] = newline;
                found = true;
            } else {
                lines[nlines++] = strdup(buf);
            }
        }
        fclose(f);
    }

    if (!found) {
        char *newline = malloc(key_len + strlen(value) + 3);
        sprintf(newline, "%s=%s\n", key, value);
        lines[nlines++] = newline;
    }

    f = fopen(env_path, "w");
    if (f) {
        for (int i = 0; i < nlines; i++) {
            fputs(lines[i], f);
            free(lines[i]);
        }
        fclose(f);
    } else {
        for (int i = 0; i < nlines; i++) free(lines[i]);
    }
}

/* ── tool_polymarket_derive_api_key ──────────────────────────────────── */

bool tool_polymarket_derive_api_key(const char *input, char *result, size_t rlen) {
    (void)input;

    /* Only need address + private key — NOT the CLOB API credentials */
    const char *addr;
    if (!trading_require_key("POLYMARKET_ADDRESS", "Polymarket wallet address",
                              result, rlen, &addr))
        return false;

    const char *privkey_hex = getenv("POLYMARKET_PRIVATE_KEY");
    if (!privkey_hex || strlen(privkey_hex) < 64) {
        snprintf(result, rlen,
                 "POLYMARKET_PRIVATE_KEY not set or invalid. "
                 "Run: export POLYMARKET_PRIVATE_KEY=<hex private key>");
        return false;
    }

    /* Decode private key */
    uint8_t privkey[32];
    const char *pk_start = privkey_hex;
    if (pk_start[0] == '0' && (pk_start[1] == 'x' || pk_start[1] == 'X'))
        pk_start += 2;
    hex_decode(pk_start, 64, privkey, 32);

    /* Build EIP-712 ClobAuth signature */
    struct timeval tv;
    gettimeofday(&tv, NULL);
    char timestamp[32];
    snprintf(timestamp, sizeof(timestamp), "%lld", (long long)tv.tv_sec);

    /* Try v=27 first, then v=28 (openssl doesn't give recovery ID) */
    char url[256];
    snprintf(url, sizeof(url), "%s/auth/derive-api-key", POLY_CLOB_BASE);

    http_buf_t resp = {0};
    long code = -1;

    for (uint8_t try_v = 27; try_v <= 28; try_v++) {
        char hex_sig[136];
        if (!poly_sign_clob_auth(privkey, addr, timestamp, 0,
                                  POLY_MSG_TO_SIGN, try_v,
                                  hex_sig, sizeof(hex_sig))) {
            snprintf(result, rlen, "EIP-712 ClobAuth signing failed");
            return false;
        }

        char h_addr[256], h_sig[256], h_ts[64], h_nonce[32];
        snprintf(h_addr, sizeof(h_addr), "POLY_ADDRESS: %s", addr);
        snprintf(h_sig, sizeof(h_sig), "POLY_SIGNATURE: %s", hex_sig);
        snprintf(h_ts, sizeof(h_ts), "POLY_TIMESTAMP: %s", timestamp);
        snprintf(h_nonce, sizeof(h_nonce), "POLY_NONCE: 0");
        const char *hdrs[] = { h_addr, h_sig, h_ts, h_nonce };

        if (resp.data) { free(resp.data); resp.data = NULL; resp.len = 0; }
        code = trading_http_request("GET", url, NULL, hdrs, 4, &resp);

        if (code == 200) break;  /* success */
        if (code != 401) break;  /* non-auth error, don't retry */
        /* 401 with v=27 → try v=28 */
    }

    if (code < 0 || !resp.data) {
        snprintf(result, rlen, "Polymarket derive-api-key request failed (code=%ld)", code);
        free(resp.data);
        return false;
    }

    if (code != 200) {
        snprintf(result, rlen, "Polymarket derive-api-key error (HTTP %ld): ", code);
        trading_truncate(resp.data, result, rlen, 64);
        free(resp.data);
        return false;
    }

    /* Parse response: {"apiKey":"...","secret":"...","passphrase":"..."} */
    char *new_api_key = json_get_str(resp.data, "apiKey");
    char *new_secret = json_get_str(resp.data, "secret");
    char *new_passphrase = json_get_str(resp.data, "passphrase");

    if (!new_api_key || !new_api_key[0] ||
        !new_secret || !new_secret[0] ||
        !new_passphrase || !new_passphrase[0]) {
        snprintf(result, rlen, "Unexpected response (missing fields): ");
        trading_truncate(resp.data, result, rlen, 64);
        free(resp.data);
        free(new_api_key); free(new_secret); free(new_passphrase);
        return false;
    }

    /* Set in current process environment */
    setenv("POLYMARKET_API_KEY", new_api_key, 1);
    setenv("POLYMARKET_API_SECRET", new_secret, 1);
    setenv("POLYMARKET_PASSPHRASE", new_passphrase, 1);

    /* Persist to .env file if it exists in cwd */
    const char *env_path = ".env";
    if (access(env_path, F_OK) == 0) {
        env_file_set(env_path, "POLYMARKET_API_KEY", new_api_key);
        env_file_set(env_path, "POLYMARKET_API_SECRET", new_secret);
        env_file_set(env_path, "POLYMARKET_PASSPHRASE", new_passphrase);
    }

    /* Report success */
    snprintf(result, rlen,
             "{\"status\":\"derived\","
             "\"apiKey\":\"%s\","
             "\"secret\":\"%.8s...\","
             "\"passphrase\":\"%.8s...\","
             "\"persisted_env\":true,"
             "\"persisted_dotenv\":%s}",
             new_api_key,
             new_secret,
             new_passphrase,
             access(env_path, F_OK) == 0 ? "true" : "false");

    free(resp.data);
    free(new_api_key);
    free(new_secret);
    free(new_passphrase);
    return true;
}

/* ── tool_polymarket_create_order ────────────────────────────────────── */

bool tool_polymarket_create_order(const char *input, char *result, size_t rlen) {
    const char *addr, *api_key, *secret, *pass;
    if (!poly_require_auth(result, rlen, &addr, &api_key, &secret, &pass))
        return false;

    risk_init();

    /* Parse required params */
    char *token_id = json_get_str(input, "token_id");
    char *side_str = json_get_str(input, "side");
    double price = json_get_double(input, "price", 0.0);
    double size = json_get_double(input, "size", 0.0);

    if (!token_id || !token_id[0]) {
        snprintf(result, rlen, "missing required parameter: token_id");
        free(token_id); free(side_str);
        return false;
    }
    if (!side_str || (strcmp(side_str, "buy") != 0 && strcmp(side_str, "sell") != 0 &&
                      strcmp(side_str, "BUY") != 0 && strcmp(side_str, "SELL") != 0)) {
        snprintf(result, rlen, "invalid side: must be 'buy' or 'sell'");
        free(token_id); free(side_str);
        return false;
    }
    if (price < 0.01 || price > 0.99) {
        snprintf(result, rlen, "price must be between 0.01 and 0.99");
        free(token_id); free(side_str);
        return false;
    }
    if (size <= 0.0) {
        snprintf(result, rlen, "size (USDC amount) must be positive");
        free(token_id); free(side_str);
        return false;
    }

    /* Optional params */
    char *order_type = json_get_str(input, "order_type");
    if (!order_type || !order_type[0]) {
        free(order_type);
        order_type = strdup("GTC");
    }

    /* Normalize side */
    bool is_buy = (strcmp(side_str, "buy") == 0 || strcmp(side_str, "BUY") == 0);
    int side_val = is_buy ? 0 : 1;

    /* Risk check */
    char risk_reason[256] = {0};
    if (!risk_preflight(size, risk_reason, sizeof(risk_reason))) {
        snprintf(result, rlen, "{\"error\":\"risk_check_failed\",\"reason\":\"%s\"}", risk_reason);
        free(token_id); free(side_str); free(order_type);
        return false;
    }

    /* Convert price/size to makerAmount/takerAmount (in wei, 6 decimals for USDC) */
    char maker_amount[32], taker_amount[32];
    if (is_buy) {
        /* BUY: makerAmount = size * 1e6, takerAmount = (size / price) * 1e6 */
        long long maker = (long long)(size * 1e6);
        long long taker = (long long)((size / price) * 1e6);
        snprintf(maker_amount, sizeof(maker_amount), "%lld", maker);
        snprintf(taker_amount, sizeof(taker_amount), "%lld", taker);
    } else {
        /* SELL: makerAmount = (size / (1-price)) * 1e6, takerAmount = size * 1e6 */
        long long maker = (long long)((size / (1.0 - price)) * 1e6);
        long long taker = (long long)(size * 1e6);
        snprintf(maker_amount, sizeof(maker_amount), "%lld", maker);
        snprintf(taker_amount, sizeof(taker_amount), "%lld", taker);
    }

    /* Generate salt — small random integer (Polymarket SDK uses ~9 digits) */
    uint8_t salt_bytes[4];
    crypto_random_bytes(salt_bytes, 4);
    uint32_t salt_num = 0;
    for (int i = 0; i < 4; i++)
        salt_num = (salt_num << 8) | salt_bytes[i];
    salt_num = salt_num % 999999999 + 1; /* keep it under 10 digits */
    char salt_str[32];
    snprintf(salt_str, sizeof(salt_str), "%u", salt_num);

    /* Nonce — Polymarket CLOB expects "0" for standard orders */
    char nonce_str[24];
    snprintf(nonce_str, sizeof(nonce_str), "0");

    /* Expiration — 0 for no expiration (GTC) */
    char expiration_str[20];
    snprintf(expiration_str, sizeof(expiration_str), "0");

    /* Taker address — 0x0 for open orders */
    const char *taker_addr = "0x0000000000000000000000000000000000000000";

    /* Get proxy address — Polymarket uses Safe proxy wallets (sig_type=1) */
    const char *funder = getenv("POLYMARKET_PROXY_ADDRESS");
    if (!funder) funder = getenv("POLYMARKET_FUNDER");
    const char *signer_addr = addr;

    /* Build poly_order_t */
    poly_order_t order;
    memset(&order, 0, sizeof(order));
    strncpy(order.salt, salt_str, sizeof(order.salt) - 1);
    strncpy(order.maker, funder ? funder : addr, sizeof(order.maker) - 1);
    strncpy(order.signer, signer_addr, sizeof(order.signer) - 1);
    strncpy(order.taker, taker_addr, sizeof(order.taker) - 1);
    strncpy(order.token_id, token_id, sizeof(order.token_id) - 1);
    strncpy(order.maker_amount, maker_amount, sizeof(order.maker_amount) - 1);
    strncpy(order.taker_amount, taker_amount, sizeof(order.taker_amount) - 1);
    strncpy(order.expiration, expiration_str, sizeof(order.expiration) - 1);
    strncpy(order.nonce, nonce_str, sizeof(order.nonce) - 1);
    /* Fetch fee rate from CLOB API for this token */
    {
        char fee_url[256];
        snprintf(fee_url, sizeof(fee_url), "%s/fee-rate?tokenID=%s",
                 POLY_CLOB_BASE, token_id);
        http_buf_t fee_resp = {0};
        long fee_code = trading_http_request("GET", fee_url, NULL, NULL, 0, &fee_resp);
        if (fee_code == 200 && fee_resp.data) {
            /* Response: {"fee_rate_bps": "150"} or similar */
            char *fee_str = json_get_str(fee_resp.data, "fee_rate_bps");
            if (fee_str) {
                order.fee_rate_bps = atoi(fee_str);
                free(fee_str);
            } else {
                /* Try numeric */
                order.fee_rate_bps = json_get_int(fee_resp.data, "fee_rate_bps", 0);
            }
        }
        free(fee_resp.data);
    }

    /* Fetch tick size for validation */
    {
        char tick_url[256];
        snprintf(tick_url, sizeof(tick_url), "%s/tick-size?tokenID=%s",
                 POLY_CLOB_BASE, token_id);
        http_buf_t tick_resp = {0};
        long tick_code = trading_http_request("GET", tick_url, NULL, NULL, 0, &tick_resp);
        if (tick_code == 200 && tick_resp.data) {
            char *tick_str = json_get_str(tick_resp.data, "minimum_tick_size");
            if (tick_str) {
                double tick = atof(tick_str);
                /* Validate price aligns with tick size */
                if (tick > 0.0) {
                    double remainder = fmod(price, tick);
                    if (remainder > tick * 0.01 && remainder < tick * 0.99) {
                        /* Price doesn't align — round to nearest tick */
                        price = round(price / tick) * tick;
                    }
                }
                free(tick_str);
            }
        }
        free(tick_resp.data);
    }

    /* Check neg_risk status (determines which exchange contract to use) */
    bool neg_risk = false;
    {
        char nr_url[256];
        snprintf(nr_url, sizeof(nr_url), "%s/neg-risk?tokenID=%s",
                 POLY_CLOB_BASE, token_id);
        http_buf_t nr_resp = {0};
        long nr_code = trading_http_request("GET", nr_url, NULL, NULL, 0, &nr_resp);
        if (nr_code == 200 && nr_resp.data) {
            neg_risk = json_get_bool(nr_resp.data, "neg_risk", false);
        }
        free(nr_resp.data);
    }

    order.side = side_val;
    order.signature_type = funder ? 1 : 0; /* POLY_PROXY if funder, else EOA */

    /* Dry run check */
    if (g_risk.dry_run) {
        snprintf(result, rlen,
                 "{\"dry_run\":true,\"platform\":\"polymarket\","
                 "\"token_id\":\"%s\",\"side\":\"%s\","
                 "\"price\":%.4f,\"size\":%.2f,"
                 "\"maker_amount\":\"%s\",\"taker_amount\":\"%s\","
                 "\"order_type\":\"%s\",\"salt\":\"%s\","
                 "\"fee_rate_bps\":%d,\"neg_risk\":%s}",
                 token_id, is_buy ? "BUY" : "SELL",
                 price, size, maker_amount, taker_amount,
                 order_type, salt_str,
                 order.fee_rate_bps, neg_risk ? "true" : "false");
        free(token_id); free(side_str); free(order_type);
        return true;
    }

    /* Get private key for signing */
    const char *privkey_hex = getenv("POLYMARKET_PRIVATE_KEY");
    if (!privkey_hex || strlen(privkey_hex) < 64) {
        snprintf(result, rlen,
                 "POLYMARKET_PRIVATE_KEY not set or invalid. "
                 "Run: export POLYMARKET_PRIVATE_KEY=<64-char hex>");
        free(token_id); free(side_str); free(order_type);
        return false;
    }

    /* Decode private key */
    uint8_t privkey[32];
    const char *pk_start = privkey_hex;
    if (pk_start[0] == '0' && (pk_start[1] == 'x' || pk_start[1] == 'X'))
        pk_start += 2;
    hex_decode(pk_start, 64, privkey, 32);

    /* Sign order with EIP-712 */
    char hex_sig[136]; /* 0x + 130 hex chars + null */
    if (!poly_sign_order(privkey, &order, neg_risk, hex_sig, sizeof(hex_sig))) {
        snprintf(result, rlen, "EIP-712 order signing failed");
        free(token_id); free(side_str); free(order_type);
        return false;
    }

    /* Build POST body for /order — must match Python SDK format exactly */
    jbuf_t body;
    jbuf_init(&body, 1024);
    jbuf_append(&body, "{");
    jbuf_appendf(&body, "\"order\":{");
    /* salt as integer, not string */
    jbuf_appendf(&body, "\"salt\":%s", order.salt);
    jbuf_appendf(&body, ",\"maker\":\"%s\"", order.maker);
    jbuf_appendf(&body, ",\"signer\":\"%s\"", order.signer);
    jbuf_appendf(&body, ",\"taker\":\"%s\"", order.taker);
    jbuf_appendf(&body, ",\"tokenId\":\"%s\"", order.token_id);
    jbuf_appendf(&body, ",\"makerAmount\":\"%s\"", order.maker_amount);
    jbuf_appendf(&body, ",\"takerAmount\":\"%s\"", order.taker_amount);
    jbuf_appendf(&body, ",\"expiration\":\"%s\"", order.expiration);
    jbuf_appendf(&body, ",\"nonce\":\"%s\"", order.nonce);
    /* feeRateBps as string */
    jbuf_appendf(&body, ",\"feeRateBps\":\"%d\"", order.fee_rate_bps);
    /* side as string "BUY" or "SELL" */
    jbuf_appendf(&body, ",\"side\":\"%s\"", is_buy ? "BUY" : "SELL");
    jbuf_appendf(&body, ",\"signatureType\":%d", order.signature_type);
    jbuf_appendf(&body, ",\"signature\":\"%s\"", hex_sig);
    jbuf_append(&body, "}");
    /* owner = API key UUID (not the maker address) */
    jbuf_appendf(&body, ",\"owner\":\"%s\"", api_key);
    jbuf_appendf(&body, ",\"orderType\":\"%s\"", order_type);
    jbuf_appendf(&body, ",\"postOnly\":false");
    jbuf_append(&body, "}");

    http_buf_t resp = {0};
    long code = poly_authed_request("POST", "/order", body.data, &resp);
    jbuf_free(&body);

    if (code < 0 || !resp.data) {
        snprintf(result, rlen, "Polymarket create order request failed (code=%ld)", code);
        free(resp.data);
        free(token_id); free(side_str); free(order_type);
        return false;
    }

    if (code != 200 && code != 201) {
        snprintf(result, rlen, "Polymarket order error %ld: ", code);
        trading_truncate(resp.data, result, rlen, 64);
        free(resp.data);
        free(token_id); free(side_str); free(order_type);
        return false;
    }

    result[0] = '\0';
    trading_truncate(resp.data, result, rlen, 16);
    free(resp.data);
    free(token_id); free(side_str); free(order_type);
    return true;
}

/* ── tool_polymarket_cancel_order ────────────────────────────────────── */

bool tool_polymarket_cancel_order(const char *input, char *result, size_t rlen) {
    const char *addr, *api_key, *secret, *pass;
    if (!poly_require_auth(result, rlen, &addr, &api_key, &secret, &pass))
        return false;

    char *order_id = json_get_str(input, "order_id");
    if (!order_id || !order_id[0]) {
        snprintf(result, rlen, "missing required parameter: order_id");
        free(order_id);
        return false;
    }

    char path[256];
    snprintf(path, sizeof(path), "/order/%s", order_id);
    free(order_id);

    http_buf_t resp = {0};
    long code = poly_authed_request("DELETE", path, NULL, &resp);
    if (code < 0 || !resp.data) {
        snprintf(result, rlen, "Polymarket cancel request failed (code=%ld)", code);
        free(resp.data);
        return false;
    }

    if (code != 200 && code != 204) {
        snprintf(result, rlen, "Polymarket cancel error %ld: ", code);
        trading_truncate(resp.data, result, rlen, 64);
        free(resp.data);
        return false;
    }

    if (resp.len > 0 && resp.data[0] == '{') {
        result[0] = '\0';
        trading_truncate(resp.data, result, rlen, 16);
    } else {
        snprintf(result, rlen, "{\"status\":\"cancelled\"}");
    }
    free(resp.data);
    return true;
}

/* ── tool_polymarket_cancel_all ──────────────────────────────────────── */

bool tool_polymarket_cancel_all(const char *input, char *result, size_t rlen) {
    (void)input;
    const char *addr, *api_key, *secret, *pass;
    if (!poly_require_auth(result, rlen, &addr, &api_key, &secret, &pass))
        return false;

    http_buf_t resp = {0};
    long code = poly_authed_request("DELETE", "/cancel-all", NULL, &resp);
    if (code < 0 || !resp.data) {
        snprintf(result, rlen, "Polymarket cancel-all request failed (code=%ld)", code);
        free(resp.data);
        return false;
    }

    if (code != 200 && code != 204) {
        snprintf(result, rlen, "Polymarket cancel-all error %ld: ", code);
        trading_truncate(resp.data, result, rlen, 64);
        free(resp.data);
        return false;
    }

    if (resp.len > 0 && resp.data[0] == '{') {
        result[0] = '\0';
        trading_truncate(resp.data, result, rlen, 16);
    } else {
        snprintf(result, rlen, "{\"platform\":\"polymarket\",\"status\":\"all_orders_cancelled\"}");
    }
    free(resp.data);
    return true;
}

/* ══════════════════════════════════════════════════════════════════════════
 *  SECTION 10b: POLYMARKET RELAYER (gasless transactions)
 *
 *  Two auth modes:
 *    1) Relayer API Key: RELAYER_API_KEY + RELAYER_API_KEY_ADDRESS headers
 *    2) Builder API Key: HMAC-signed POLY_BUILDER_* headers
 *
 *  Relayer base: https://relayer-v2.polymarket.com
 * ══════════════════════════════════════════════════════════════════════════ */

#define POLY_RELAYER_BASE "https://relayer-v2.polymarket.com"

/* Polygon contract addresses */
#define POLY_USDC   "0x2791Bca1f2de4661ED88A30C99A7a9449Aa84174"
#define POLY_CTF    "0x4D97DCd97eC945f40cF65F87097ACe5EA0476045"
#define POLY_NEG_CTF "0xC5d563A36AE78145C45a50134d48A1215220f80a"

static bool poly_relayer_auth_headers(char *result, size_t rlen,
                                       const char **out_addr,
                                       char hdrs_buf[][256], const char **hdrs, int *nhdr) {
    /* Try Relayer API Key first (simpler) */
    const char *relayer_key = getenv("POLYMARKET_RELAYER_API_KEY");
    const char *addr = getenv("POLYMARKET_ADDRESS");

    if (relayer_key && relayer_key[0] && addr && addr[0]) {
        snprintf(hdrs_buf[0], 256, "RELAYER_API_KEY: %s", relayer_key);
        snprintf(hdrs_buf[1], 256, "RELAYER_API_KEY_ADDRESS: %s", addr);
        hdrs[0] = hdrs_buf[0];
        hdrs[1] = hdrs_buf[1];
        *nhdr = 2;
        *out_addr = addr;
        return true;
    }

    /* Fallback: Builder API Key (HMAC) */
    const char *builder_key = getenv("POLY_BUILDER_API_KEY");
    const char *builder_secret = getenv("POLY_BUILDER_SECRET");
    const char *builder_pass = getenv("POLY_BUILDER_PASSPHRASE");

    if (builder_key && builder_key[0] &&
        builder_secret && builder_secret[0] &&
        builder_pass && builder_pass[0] && addr && addr[0]) {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        char ts[32];
        snprintf(ts, sizeof(ts), "%lld", (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000);

        /* HMAC-SHA256(secret, timestamp + method + path + body) */
        char hmac_sig[128];
        if (!poly_hmac_sign(builder_secret, ts, "POST", "/relayer", "",
                            hmac_sig, sizeof(hmac_sig))) {
            snprintf(result, rlen, "Builder API HMAC signing failed");
            return false;
        }
        snprintf(hdrs_buf[0], 256, "POLY_BUILDER_API_KEY: %s", builder_key);
        snprintf(hdrs_buf[1], 256, "POLY_BUILDER_TIMESTAMP: %s", ts);
        snprintf(hdrs_buf[2], 256, "POLY_BUILDER_PASSPHRASE: %s", builder_pass);
        snprintf(hdrs_buf[3], 256, "POLY_BUILDER_SIGNATURE: %s", hmac_sig);
        hdrs[0] = hdrs_buf[0];
        hdrs[1] = hdrs_buf[1];
        hdrs[2] = hdrs_buf[2];
        hdrs[3] = hdrs_buf[3];
        *nhdr = 4;
        *out_addr = addr;
        return true;
    }

    snprintf(result, rlen,
             "Relayer auth not configured. Set either:\n"
             "  POLYMARKET_RELAYER_API_KEY + POLYMARKET_ADDRESS\n"
             "  or POLY_BUILDER_API_KEY + POLY_BUILDER_SECRET + POLY_BUILDER_PASSPHRASE + POLYMARKET_ADDRESS");
    return false;
}

/* ── tool_polymarket_relayer_deploy ──────────────────────────────────── */

bool tool_polymarket_relayer_deploy(const char *input, char *result, size_t rlen) {
    (void)input;
    const char *addr;
    char hdrs_buf[4][256];
    const char *hdrs[4];
    int nhdr;
    if (!poly_relayer_auth_headers(result, rlen, &addr, hdrs_buf, hdrs, &nhdr))
        return false;

    char url[256];
    snprintf(url, sizeof(url), "%s/deploy", POLY_RELAYER_BASE);

    http_buf_t resp = {0};
    long code = trading_http_request("POST", url, "{}", hdrs, nhdr, &resp);
    if (code < 0 || !resp.data) {
        snprintf(result, rlen, "Relayer deploy request failed (code=%ld)", code);
        free(resp.data);
        return false;
    }
    if (code != 200 && code != 201) {
        snprintf(result, rlen, "Relayer deploy error (HTTP %ld): ", code);
        trading_truncate(resp.data, result, rlen, 64);
        free(resp.data);
        return false;
    }
    result[0] = '\0';
    trading_truncate(resp.data, result, rlen, 16);
    free(resp.data);
    return true;
}

/* ── tool_polymarket_relayer_approve ─────────────────────────────────── */

bool tool_polymarket_relayer_approve(const char *input, char *result, size_t rlen) {
    const char *addr;
    char hdrs_buf[4][256];
    const char *hdrs[4];
    int nhdr;
    if (!poly_relayer_auth_headers(result, rlen, &addr, hdrs_buf, hdrs, &nhdr))
        return false;

    /* Parse optional token (default: USDC) and spender (default: CTF exchange) */
    char *token = json_get_str(input, "token");
    char *spender = json_get_str(input, "spender");
    const char *tok = (token && token[0]) ? token : POLY_USDC;
    const char *spd = (spender && spender[0]) ? spender : POLY_CTF;

    /* ERC20 approve(address, uint256) → selector 0x095ea7b3
     * arg0: address (20 bytes, left-padded to 32)
     * arg1: uint256 max = 0xff...ff */
    char calldata[256];
    const char *a = spd;
    if (a[0] == '0' && (a[1] == 'x' || a[1] == 'X')) a += 2;
    snprintf(calldata, sizeof(calldata),
             "0x095ea7b3"
             "000000000000000000000000%.40s"
             "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
             a);

    char body[1024];
    snprintf(body, sizeof(body),
             "{\"transactions\":[{\"to\":\"%s\",\"data\":\"%s\",\"value\":\"0\"}],"
             "\"description\":\"Approve %s for spending\"}",
             tok, calldata, spd);

    char url[256];
    snprintf(url, sizeof(url), "%s/execute", POLY_RELAYER_BASE);

    http_buf_t resp = {0};
    long code = trading_http_request("POST", url, body, hdrs, nhdr, &resp);
    if (code < 0 || !resp.data) {
        snprintf(result, rlen, "Relayer approve failed (code=%ld)", code);
        free(resp.data); free(token); free(spender);
        return false;
    }
    if (code != 200 && code != 201) {
        snprintf(result, rlen, "Relayer approve error (HTTP %ld): ", code);
        trading_truncate(resp.data, result, rlen, 64);
        free(resp.data); free(token); free(spender);
        return false;
    }
    result[0] = '\0';
    trading_truncate(resp.data, result, rlen, 16);
    free(resp.data); free(token); free(spender);
    return true;
}

/* ── tool_polymarket_relayer_execute ─────────────────────────────────── */

bool tool_polymarket_relayer_execute(const char *input, char *result, size_t rlen) {
    const char *addr;
    char hdrs_buf[4][256];
    const char *hdrs[4];
    int nhdr;
    if (!poly_relayer_auth_headers(result, rlen, &addr, hdrs_buf, hdrs, &nhdr))
        return false;

    /* Expect: { "to": "0x...", "data": "0x...", "value": "0", "description": "..." } */
    char *to = json_get_str(input, "to");
    char *data = json_get_str(input, "data");
    char *value = json_get_str(input, "value");
    char *desc = json_get_str(input, "description");

    if (!to || !to[0] || !data || !data[0]) {
        snprintf(result, rlen, "Required: to (address), data (hex calldata)");
        free(to); free(data); free(value); free(desc);
        return false;
    }

    const char *val = (value && value[0]) ? value : "0";
    const char *dsc = (desc && desc[0]) ? desc : "dsco relayer tx";

    char *body = malloc(strlen(data) + 1024);
    sprintf(body,
            "{\"transactions\":[{\"to\":\"%s\",\"data\":\"%s\",\"value\":\"%s\"}],"
            "\"description\":\"%s\"}",
            to, data, val, dsc);

    char url[256];
    snprintf(url, sizeof(url), "%s/execute", POLY_RELAYER_BASE);

    http_buf_t resp = {0};
    long code = trading_http_request("POST", url, body, hdrs, nhdr, &resp);
    free(body);
    if (code < 0 || !resp.data) {
        snprintf(result, rlen, "Relayer execute failed (code=%ld)", code);
        free(resp.data);
        free(to); free(data); free(value); free(desc);
        return false;
    }
    if (code != 200 && code != 201) {
        snprintf(result, rlen, "Relayer execute error (HTTP %ld): ", code);
        trading_truncate(resp.data, result, rlen, 64);
        free(resp.data);
        free(to); free(data); free(value); free(desc);
        return false;
    }
    result[0] = '\0';
    trading_truncate(resp.data, result, rlen, 16);
    free(resp.data);
    free(to); free(data); free(value); free(desc);
    return true;
}

/* ── tool_polymarket_relayer_status ──────────────────────────────────── */

bool tool_polymarket_relayer_status(const char *input, char *result, size_t rlen) {
    const char *addr;
    char hdrs_buf[4][256];
    const char *hdrs[4];
    int nhdr;
    if (!poly_relayer_auth_headers(result, rlen, &addr, hdrs_buf, hdrs, &nhdr))
        return false;

    char *tx_id = json_get_str(input, "tx_id");
    if (!tx_id || !tx_id[0]) {
        snprintf(result, rlen, "Required: tx_id (relayer transaction ID)");
        free(tx_id);
        return false;
    }

    char url[512];
    snprintf(url, sizeof(url), "%s/status/%s", POLY_RELAYER_BASE, tx_id);

    http_buf_t resp = {0};
    long code = trading_http_request("GET", url, NULL, hdrs, nhdr, &resp);
    free(tx_id);
    if (code < 0 || !resp.data) {
        snprintf(result, rlen, "Relayer status request failed (code=%ld)", code);
        free(resp.data);
        return false;
    }
    if (code != 200) {
        snprintf(result, rlen, "Relayer status error (HTTP %ld): ", code);
        trading_truncate(resp.data, result, rlen, 64);
        free(resp.data);
        return false;
    }
    result[0] = '\0';
    trading_truncate(resp.data, result, rlen, 16);
    free(resp.data);
    return true;
}

/* ══════════════════════════════════════════════════════════════════════════
 *  SECTION 11: CROSS-PLATFORM ARBITRAGE & PORTFOLIO
 * ══════════════════════════════════════════════════════════════════════════ */

/* ── tool_arb_execute ────────────────────────────────────────────────── */

bool tool_arb_execute(const char *input, char *result, size_t rlen) {
    risk_init();

    /* Parse Kalshi leg params */
    char *k_ticker = json_get_str(input, "kalshi_ticker");
    char *k_action = json_get_str(input, "kalshi_action");
    char *k_side = json_get_str(input, "kalshi_side");
    int k_count = json_get_int(input, "kalshi_count", 0);
    int k_price = json_get_int(input, "kalshi_price", 0);

    /* Parse Polymarket leg params */
    char *p_token = json_get_str(input, "poly_token_id");
    char *p_side = json_get_str(input, "poly_side");
    double p_price = json_get_double(input, "poly_price", 0.0);
    double p_size = json_get_double(input, "poly_size", 0.0);

    if (!k_ticker || !k_action || !k_side || k_count <= 0) {
        snprintf(result, rlen, "missing Kalshi leg params: kalshi_ticker, kalshi_action, kalshi_side, kalshi_count");
        free(k_ticker); free(k_action); free(k_side);
        free(p_token); free(p_side);
        return false;
    }
    if (!p_token || !p_side || p_price <= 0.0 || p_size <= 0.0) {
        snprintf(result, rlen, "missing Polymarket leg params: poly_token_id, poly_side, poly_price, poly_size");
        free(k_ticker); free(k_action); free(k_side);
        free(p_token); free(p_side);
        return false;
    }

    /* Risk check both legs */
    double k_usd = (double)(k_price > 0 ? k_price : 50) * (double)k_count / 100.0;
    double p_usd = p_size;
    double total_usd = k_usd + p_usd;

    char risk_reason[256] = {0};
    if (!risk_preflight(k_usd, risk_reason, sizeof(risk_reason))) {
        snprintf(result, rlen,
                 "{\"error\":\"risk_check_failed\",\"leg\":\"kalshi\",\"reason\":\"%s\"}",
                 risk_reason);
        free(k_ticker); free(k_action); free(k_side);
        free(p_token); free(p_side);
        return false;
    }
    if (!risk_preflight(p_usd, risk_reason, sizeof(risk_reason))) {
        snprintf(result, rlen,
                 "{\"error\":\"risk_check_failed\",\"leg\":\"polymarket\",\"reason\":\"%s\"}",
                 risk_reason);
        free(k_ticker); free(k_action); free(k_side);
        free(p_token); free(p_side);
        return false;
    }

    /* Dry run */
    if (g_risk.dry_run) {
        snprintf(result, rlen,
                 "{\"dry_run\":true,\"arb_execution\":{"
                 "\"kalshi\":{\"ticker\":\"%s\",\"action\":\"%s\",\"side\":\"%s\","
                 "\"count\":%d,\"price\":%d,\"est_usd\":%.2f},"
                 "\"polymarket\":{\"token_id\":\"%s\",\"side\":\"%s\","
                 "\"price\":%.4f,\"size\":%.2f},"
                 "\"total_exposure_usd\":%.2f}}",
                 k_ticker, k_action, k_side, k_count, k_price, k_usd,
                 p_token, p_side, p_price, p_size, total_usd);
        free(k_ticker); free(k_action); free(k_side);
        free(p_token); free(p_side);
        return true;
    }

    /* Execute Kalshi leg first */
    jbuf_t k_input;
    jbuf_init(&k_input, 512);
    jbuf_appendf(&k_input,
                 "{\"ticker\":\"%s\",\"action\":\"%s\",\"side\":\"%s\","
                 "\"count\":%d,\"type\":\"%s\"",
                 k_ticker, k_action, k_side, k_count,
                 k_price > 0 ? "limit" : "market");
    if (k_price > 0) jbuf_appendf(&k_input, ",\"yes_price\":%d", k_price);
    jbuf_append(&k_input, "}");

    char k_result[MAX_TOOL_RESULT];
    bool k_ok = tool_kalshi_create_order(k_input.data, k_result, sizeof(k_result));
    jbuf_free(&k_input);

    if (!k_ok) {
        snprintf(result, rlen,
                 "{\"error\":\"kalshi_leg_failed\",\"details\":\"%s\"}", k_result);
        free(k_ticker); free(k_action); free(k_side);
        free(p_token); free(p_side);
        return false;
    }

    /* Execute Polymarket leg */
    jbuf_t p_input;
    jbuf_init(&p_input, 512);
    jbuf_appendf(&p_input,
                 "{\"token_id\":\"%s\",\"side\":\"%s\","
                 "\"price\":%.4f,\"size\":%.2f,\"order_type\":\"GTC\"}",
                 p_token, p_side, p_price, p_size);

    char p_result[MAX_TOOL_RESULT];
    bool p_ok = tool_polymarket_create_order(p_input.data, p_result, sizeof(p_result));
    jbuf_free(&p_input);

    /* Rollback: if Polymarket leg failed, attempt to cancel the Kalshi order
       to avoid leaving an unhedged position. Extract order_id from k_result. */
    bool rollback_attempted = false;
    bool rollback_ok = false;
    char rollback_result[1024] = "";
    if (k_ok && !p_ok) {
        /* Try to extract order_id from Kalshi response for cancellation */
        char *oid = json_get_str(k_result, "order_id");
        if (!oid) oid = json_get_str(k_result, "order");  /* fallback key */
        if (oid && oid[0]) {
            rollback_attempted = true;
            char cancel_input[256];
            snprintf(cancel_input, sizeof(cancel_input),
                     "{\"order_id\":\"%s\"}", oid);
            rollback_ok = tool_kalshi_cancel_order(cancel_input,
                            rollback_result, sizeof(rollback_result));
        }
        free(oid);
    }

    /* Combine results */
    jbuf_t combined;
    jbuf_init(&combined, 4096);
    jbuf_append(&combined, "{\"arb_execution\":{");
    jbuf_appendf(&combined, "\"kalshi\":{\"success\":%s,\"result\":%s},",
                 k_ok ? "true" : "false", k_result);
    jbuf_appendf(&combined, "\"polymarket\":{\"success\":%s,\"result\":%s}",
                 p_ok ? "true" : "false", p_result);
    if (rollback_attempted) {
        jbuf_appendf(&combined,
                     ",\"rollback\":{\"attempted\":true,\"success\":%s,"
                     "\"reason\":\"polymarket_leg_failed\",\"details\":\"%s\"}",
                     rollback_ok ? "true" : "false",
                     rollback_ok ? "kalshi_order_cancelled" : "cancel_failed");
    }
    jbuf_append(&combined, "}}");

    snprintf(result, rlen, "%s", combined.data);
    jbuf_free(&combined);

    free(k_ticker); free(k_action); free(k_side);
    free(p_token); free(p_side);
    return k_ok && p_ok;
}

/* ── tool_arb_monitor ────────────────────────────────────────────────── */

/* Helper: scan Polymarket response for within-market arb (YES+NO < $1) */
static int scan_poly_within_market_arbs(const char *json, jbuf_t *out,
                                         double min_spread, int max_results) {
    if (!json || json[0] != '[') return 0;
    int found = 0;
    const char *p = json;

    while (*p && found < max_results) {
        const char *start = strchr(p, '{');
        if (!start) break;
        int depth = 1;
        const char *end = start + 1;
        while (*end && depth > 0) {
            if (*end == '{') depth++;
            else if (*end == '}') depth--;
            end++;
        }
        size_t olen = (size_t)(end - start);
        char *obj = malloc(olen + 1);
        memcpy(obj, start, olen);
        obj[olen] = '\0';

        char *question = json_get_str(obj, "question");
        char *prices = json_get_str(obj, "outcomePrices");
        char *cid = json_get_str(obj, "conditionId");
        char *vol_s = json_get_str(obj, "volume24hr");

        if (question && prices && prices[0] == '[') {
            /* Parse outcomePrices: ["0.65","0.35"] */
            double yes_p = 0, no_p = 0;
            const char *fp = prices + 2;
            yes_p = atof(fp);
            const char *comma = strchr(fp, ',');
            if (comma) {
                const char *sp = comma + 2;
                no_p = atof(sp);
            }

            double total = yes_p + no_p;
            double spread = 1.0 - total;

            /* Within-market arb: if YES + NO < 1.0, buy both → guaranteed profit */
            if (spread > min_spread && spread < 0.50) {
                if (found > 0) jbuf_append(out, ",");
                jbuf_append(out, "{\"type\":\"within_market\",\"platform\":\"polymarket\"");
                jbuf_append(out, ",\"question\":");
                jbuf_append_json_str(out, question);
                jbuf_appendf(out, ",\"yes_price\":%.4f,\"no_price\":%.4f", yes_p, no_p);
                jbuf_appendf(out, ",\"total\":%.4f,\"spread\":%.4f", total, spread);
                jbuf_appendf(out, ",\"guaranteed_profit_per_contract\":%.4f", spread);
                if (cid) {
                    jbuf_append(out, ",\"condition_id\":");
                    jbuf_append_json_str(out, cid);
                }
                if (vol_s) jbuf_appendf(out, ",\"volume_24h\":%.0f", atof(vol_s));
                jbuf_append(out, ",\"action\":\"BUY both YES and NO → one pays $1.00\"}");
                found++;
            }
        }

        free(question); free(prices); free(cid); free(vol_s); free(obj);
        p = end;
    }
    return found;
}

/* Helper: scan Kalshi response for within-market arb */
static int scan_kalshi_within_market_arbs(const char *json, jbuf_t *out,
                                           double min_spread, int max_results) {
    if (!json) return 0;
    int found = 0;
    const char *p = json;

    /* Look for "yes_bid" and "no_bid" or "yes_ask" and "no_ask" pairs */
    while (*p && found < max_results) {
        const char *title_key = strstr(p, "\"title\"");
        if (!title_key) break;

        /* Extract title */
        const char *qs = strchr(title_key + 7, '"');
        if (!qs) break;
        qs++;
        const char *qe = strchr(qs, '"');
        if (!qe) break;
        char title[256];
        size_t tlen = (size_t)(qe - qs);
        if (tlen >= sizeof(title)) tlen = sizeof(title) - 1;
        memcpy(title, qs, tlen);
        title[tlen] = '\0';

        /* Look for yes_bid and no_bid nearby (both are in cents) */
        const char *yb = strstr(qe, "\"yes_bid\"");
        const char *nb = strstr(qe, "\"no_bid\"");
        const char *tk = strstr(qe, "\"ticker\"");

        char ticker[64] = "";
        if (tk && tk - qe < 800) {
            const char *ts = strchr(tk + 8, '"');
            if (ts) {
                ts++;
                const char *te = strchr(ts, '"');
                if (te) {
                    size_t tkl = (size_t)(te - ts);
                    if (tkl >= sizeof(ticker)) tkl = sizeof(ticker) - 1;
                    memcpy(ticker, ts, tkl);
                    ticker[tkl] = '\0';
                }
            }
        }

        if (yb && nb && yb - qe < 800 && nb - qe < 800) {
            double yes_bid = atof(yb + 10) / 100.0;
            double no_bid = atof(nb + 9) / 100.0;
            double total = yes_bid + no_bid;
            double spread = 1.0 - total;

            if (spread > min_spread && spread < 0.50 && yes_bid > 0 && no_bid > 0) {
                if (found > 0) jbuf_append(out, ",");
                jbuf_append(out, "{\"type\":\"within_market\",\"platform\":\"kalshi\"");
                jbuf_append(out, ",\"title\":");
                jbuf_append_json_str(out, title);
                jbuf_appendf(out, ",\"yes_bid\":%.2f,\"no_bid\":%.2f", yes_bid, no_bid);
                jbuf_appendf(out, ",\"total\":%.4f,\"spread\":%.4f", total, spread);
                jbuf_appendf(out, ",\"guaranteed_profit_per_contract\":%.4f", spread);
                if (ticker[0]) {
                    jbuf_append(out, ",\"ticker\":");
                    jbuf_append_json_str(out, ticker);
                }
                jbuf_append(out, ",\"action\":\"BUY both YES and NO → one pays $1.00\"}");
                found++;
            }
        }

        p = qe + 1;
    }
    return found;
}

bool tool_arb_monitor(const char *input, char *result, size_t rlen) {
    risk_init();

    char *topic = json_get_str(input, "topic");
    int limit = json_get_int(input, "limit", 20);
    if (limit > 50) limit = 50;
    double custom_spread = json_get_double(input, "min_spread", -1.0);
    double min_spread = (custom_spread > 0) ? custom_spread : g_risk.min_arb_spread;

    /* ── Fetch Polymarket markets ──────────────────────────────────────── */
    http_buf_t p_resp = {0};
    char p_url[512];
    if (topic && topic[0]) {
        CURL *c = curl_easy_init();
        char *enc = c ? curl_easy_escape(c, topic, 0) : NULL;
        snprintf(p_url, sizeof(p_url),
                 "https://gamma-api.polymarket.com/markets?_q=%s&limit=%d&active=true"
                 "&order=volume24hr&ascending=false",
                 enc ? enc : topic, limit);
        if (enc) curl_free(enc);
        if (c) curl_easy_cleanup(c);
    } else {
        snprintf(p_url, sizeof(p_url),
                 "https://gamma-api.polymarket.com/markets?limit=%d&active=true"
                 "&order=volume24hr&ascending=false", limit);
    }
    long p_code = trading_http_request("GET", p_url, NULL, NULL, 0, &p_resp);

    /* ── Fetch Kalshi events (public, no auth needed) ──────────────────── */
    http_buf_t k_resp = {0};
    char k_url[512];
    snprintf(k_url, sizeof(k_url),
             "https://api.elections.kalshi.com/trade-api/v2/events"
             "?limit=%d&status=open&with_nested_markets=true", limit);
    long k_code = trading_http_request("GET", k_url, NULL, NULL, 0, &k_resp);

    free(topic);

    /* ── Scan for within-market arb opportunities ──────────────────────── */
    jbuf_t jb;
    jbuf_init(&jb, rlen > 16384 ? 16384 : rlen);
    jbuf_append(&jb, "{\"arb_monitor\":{");
    jbuf_appendf(&jb, "\"min_spread\":%.4f", min_spread);
    jbuf_appendf(&jb, ",\"markets_scanned\":{\"polymarket\":%s,\"kalshi\":%s}",
                 p_code == 200 ? "true" : "false",
                 k_code == 200 ? "true" : "false");

    /* Within-market arbs: YES + NO < $1.00 on same platform */
    jbuf_append(&jb, ",\"within_market_arbs\":[");
    int arb_count = 0;
    if (p_code == 200 && p_resp.data)
        arb_count += scan_poly_within_market_arbs(p_resp.data, &jb, min_spread, 10);
    if (k_code == 200 && k_resp.data)
        arb_count += scan_kalshi_within_market_arbs(k_resp.data, &jb, min_spread, 10);
    jbuf_append(&jb, "]");
    jbuf_appendf(&jb, ",\"within_market_count\":%d", arb_count);

    /* Cross-platform data for LLM-driven matching */
    jbuf_append(&jb, ",\"cross_platform_data\":{");

    /* Polymarket summaries */
    jbuf_append(&jb, "\"polymarket_markets\":");
    if (p_code == 200 && p_resp.data) {
        /* Truncate to fit */
        size_t cap = rlen / 3;
        if (p_resp.len > cap) p_resp.data[cap] = '\0';
        jbuf_append(&jb, p_resp.data);
    } else {
        jbuf_appendf(&jb, "{\"error\":%ld}", p_code);
    }

    /* Kalshi summaries */
    jbuf_append(&jb, ",\"kalshi_events\":");
    if (k_code == 200 && k_resp.data) {
        size_t cap = rlen / 3;
        if (k_resp.len > cap) k_resp.data[cap] = '\0';
        jbuf_append(&jb, k_resp.data);
    } else {
        jbuf_appendf(&jb, "{\"error\":%ld}", k_code);
    }
    jbuf_append(&jb, "}");

    /* Arb detection rules */
    jbuf_append(&jb, ",\"detection_rules\":{"
        "\"within_market\":\"YES+NO < $1.00 → buy both, profit = $1.00 - total\","
        "\"cross_platform\":\"Same event on different platforms with price divergence → "
        "buy cheaper side, sell more expensive on other platform\","
        "\"threshold\":\"Only arbs with spread > min_spread are reported\","
        "\"fees\":{\"polymarket\":\"0% (maker), variable taker\","
        "\"kalshi\":\"varies by contract, check series fee\"}"
    "}");

    jbuf_append(&jb, "}}");

    result[0] = '\0';
    trading_truncate(jb.data ? jb.data : "{}", result, rlen, 16);
    jbuf_free(&jb);
    free(k_resp.data);
    free(p_resp.data);
    return true;
}

/* ── tool_portfolio_cross ────────────────────────────────────────────── */

bool tool_portfolio_cross(const char *input, char *result, size_t rlen) {
    (void)input;

    jbuf_t jb;
    jbuf_init(&jb, 8192);
    jbuf_append(&jb, "{\"cross_platform_portfolio\":{");

    /* Kalshi section */
    const char *k_api_key = getenv("KALSHI_API_KEY");
    const char *k_key_path = getenv("KALSHI_RSA_PRIVATE_KEY_PATH");
    if (k_api_key && k_api_key[0] && k_key_path && k_key_path[0]) {

        /* Get balance */
        http_buf_t bal_resp = {0};
        long bal_code = kalshi_authed_get("/portfolio/balance", &bal_resp);

        jbuf_append(&jb, "\"kalshi\":{");
        if (bal_code == 200 && bal_resp.data) {
            double balance = json_get_double(bal_resp.data, "balance", 0.0);
            double portfolio_value = json_get_double(bal_resp.data, "portfolio_value", 0.0);
            jbuf_appendf(&jb, "\"balance_usd\":%.2f", balance / 100.0);
            jbuf_appendf(&jb, ",\"portfolio_value_usd\":%.2f", portfolio_value / 100.0);
        } else {
            jbuf_appendf(&jb, "\"error\":\"balance fetch failed (code=%ld)\"", bal_code);
        }
        free(bal_resp.data);

        /* Get positions */
        http_buf_t pos_resp = {0};
        long pos_code = kalshi_authed_get("/portfolio/positions?count_filter=position&limit=50", &pos_resp);
        if (pos_code == 200 && pos_resp.data) {
            jbuf_append(&jb, ",\"positions\":");
            jbuf_append(&jb, pos_resp.data);
        }
        free(pos_resp.data);

        jbuf_append(&jb, "}");
    } else {
        jbuf_append(&jb, "\"kalshi\":{\"status\":\"not_configured\","
                    "\"note\":\"Set KALSHI_API_KEY and KALSHI_RSA_PRIVATE_KEY_PATH\"}");
    }

    /* Polymarket section */
    jbuf_append(&jb, ",");
    /* Prefer proxy address for positions (where funds/positions live) */
    const char *p_addr = getenv("POLYMARKET_PROXY_ADDRESS");
    if (!p_addr || !p_addr[0])
        p_addr = getenv("POLYMARKET_ADDRESS");
    const char *p_key = getenv("POLYMARKET_API_KEY");
    const char *p_secret = getenv("POLYMARKET_API_SECRET");
    const char *p_pass = getenv("POLYMARKET_PASSPHRASE");
    bool have_poly = (p_addr && p_addr[0] && p_key && p_key[0] &&
                      p_secret && p_secret[0] && p_pass && p_pass[0]);

    if (have_poly) {
        /* Get balance */
        http_buf_t bal_resp = {0};
        long bal_code = poly_authed_request("GET", "/balance-allowance?asset_type=COLLATERAL&signature_type=1",
                                            NULL, &bal_resp);

        jbuf_append(&jb, "\"polymarket\":{");
        if (bal_code == 200 && bal_resp.data) {
            char *bal_str = json_get_str(bal_resp.data, "balance");
            if (bal_str) {
                double usdc = atof(bal_str) / 1e6;
                jbuf_appendf(&jb, "\"balance_usdc\":%.6f", usdc);
                free(bal_str);
            }
        } else {
            jbuf_appendf(&jb, "\"error\":\"balance fetch failed (code=%ld)\"", bal_code);
        }
        free(bal_resp.data);

        /* Get positions (public API) */
        char p_url[512];
        snprintf(p_url, sizeof(p_url),
                 "https://data-api.polymarket.com/positions?user=%s", p_addr);
        http_buf_t pos_resp = {0};
        long pos_code = trading_http_request("GET", p_url, NULL, NULL, 0, &pos_resp);
        if (pos_code == 200 && pos_resp.data) {
            jbuf_append(&jb, ",\"positions\":");
            jbuf_append(&jb, pos_resp.data);
        }
        free(pos_resp.data);

        jbuf_append(&jb, "}");
    } else {
        jbuf_append(&jb, "\"polymarket\":{\"status\":\"not_configured\","
                    "\"note\":\"Set POLYMARKET_ADDRESS, POLYMARKET_API_KEY, "
                    "POLYMARKET_API_SECRET, POLYMARKET_PASSPHRASE\"}");
    }

    jbuf_append(&jb, "}}");

    snprintf(result, rlen, "%s", jb.data);
    jbuf_free(&jb);
    return true;
}

/* ── tool_risk_check ─────────────────────────────────────────────────── */

bool tool_risk_check(const char *input, char *result, size_t rlen) {
    risk_init();

    char *platform = json_get_str(input, "platform");
    double amount_usd = json_get_double(input, "amount_usd", 0.0);

    if (amount_usd <= 0.0) {
        snprintf(result, rlen, "amount_usd must be positive");
        free(platform);
        return false;
    }

    char risk_reason[256] = {0};
    bool passes = risk_preflight(amount_usd, risk_reason, sizeof(risk_reason));

    jbuf_t jb;
    jbuf_init(&jb, 512);
    jbuf_append(&jb, "{");
    jbuf_appendf(&jb, "\"platform\":\"%s\"", platform ? platform : "any");
    jbuf_appendf(&jb, ",\"amount_usd\":%.2f", amount_usd);
    jbuf_appendf(&jb, ",\"passes\":%s", passes ? "true" : "false");
    if (!passes) {
        jbuf_append(&jb, ",\"reason\":\"");
        jbuf_append_json_str(&jb, risk_reason);
        jbuf_append(&jb, "\"");
    }
    jbuf_append(&jb, ",\"limits\":{");
    jbuf_appendf(&jb, "\"max_order_usd\":%.2f", g_risk.max_order_usd);
    jbuf_appendf(&jb, ",\"max_position_usd\":%.2f", g_risk.max_position_usd);
    jbuf_appendf(&jb, ",\"max_total_exposure_usd\":%.2f", g_risk.max_total_exposure_usd);
    jbuf_appendf(&jb, ",\"min_arb_spread\":%.4f", g_risk.min_arb_spread);
    jbuf_appendf(&jb, ",\"max_open_orders\":%d", g_risk.max_open_orders);
    jbuf_appendf(&jb, ",\"dry_run\":%s", g_risk.dry_run ? "true" : "false");
    jbuf_append(&jb, "}}");

    snprintf(result, rlen, "%s", jb.data);
    jbuf_free(&jb);
    free(platform);
    return true;
}

/* ── tool_risk_configure ─────────────────────────────────────────────── */

bool tool_risk_configure(const char *input, char *result, size_t rlen) {
    risk_init();

    /* Update fields if provided */
    double v;

    v = json_get_double(input, "max_order_usd", -1.0);
    if (v > 0.0) g_risk.max_order_usd = v;

    v = json_get_double(input, "max_position_usd", -1.0);
    if (v > 0.0) g_risk.max_position_usd = v;

    v = json_get_double(input, "max_total_exposure_usd", -1.0);
    if (v > 0.0) g_risk.max_total_exposure_usd = v;

    v = json_get_double(input, "min_arb_spread", -1.0);
    if (v >= 0.0) g_risk.min_arb_spread = v;

    int max_orders = json_get_int(input, "max_open_orders", -1);
    if (max_orders > 0) g_risk.max_open_orders = max_orders;

    /* dry_run: check if explicitly set */
    /* json_get_bool returns the default if key not found, so we need to
     * detect whether the key was actually present */
    char *dry_run_str = json_get_str(input, "dry_run");
    if (dry_run_str) {
        g_risk.dry_run = (strcmp(dry_run_str, "true") == 0 ||
                         strcmp(dry_run_str, "1") == 0);
        free(dry_run_str);
    }

    /* Return current config */
    jbuf_t jb;
    jbuf_init(&jb, 512);
    jbuf_append(&jb, "{\"risk_config\":{");
    jbuf_appendf(&jb, "\"max_order_usd\":%.2f", g_risk.max_order_usd);
    jbuf_appendf(&jb, ",\"max_position_usd\":%.2f", g_risk.max_position_usd);
    jbuf_appendf(&jb, ",\"max_total_exposure_usd\":%.2f", g_risk.max_total_exposure_usd);
    jbuf_appendf(&jb, ",\"min_arb_spread\":%.4f", g_risk.min_arb_spread);
    jbuf_appendf(&jb, ",\"max_open_orders\":%d", g_risk.max_open_orders);
    jbuf_appendf(&jb, ",\"dry_run\":%s", g_risk.dry_run ? "true" : "false");
    jbuf_append(&jb, "}}");

    snprintf(result, rlen, "%s", jb.data);
    jbuf_free(&jb);
    return true;
}
