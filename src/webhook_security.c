#include "webhook_security.h"

#include "crypto.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>

static void webhook_set_reason(char *reason, size_t reason_len, const char *msg) {
    if (!reason || reason_len == 0)
        return;
    if (!msg)
        msg = "";
    snprintf(reason, reason_len, "%s", msg);
}

static int webhook_hex_nibble(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static bool webhook_parse_hmac_hex(const char *signature_header, uint8_t out[32]) {
    if (!signature_header || !out)
        return false;

    const char *p = signature_header;
    while (isspace((unsigned char)*p))
        p++;

    const char *eq = strchr(p, '=');
    if (eq) {
        size_t alg_len = (size_t)(eq - p);
        bool supported = false;
        if (alg_len == 6) {
            supported = (tolower((unsigned char)p[0]) == 's' &&
                         tolower((unsigned char)p[1]) == 'h' &&
                         tolower((unsigned char)p[2]) == 'a' && p[3] == '2' && p[4] == '5' &&
                         p[5] == '6');
        } else if (alg_len == 11) {
            supported = (tolower((unsigned char)p[0]) == 'h' &&
                         tolower((unsigned char)p[1]) == 'm' &&
                         tolower((unsigned char)p[2]) == 'a' &&
                         tolower((unsigned char)p[3]) == 'c' && p[4] == '-' &&
                         tolower((unsigned char)p[5]) == 's' &&
                         tolower((unsigned char)p[6]) == 'h' &&
                         tolower((unsigned char)p[7]) == 'a' && p[8] == '2' && p[9] == '5' &&
                         p[10] == '6');
        }
        if (!supported)
            return false;
        p = eq + 1;
    }

    while (isspace((unsigned char)*p))
        p++;

    for (size_t i = 0; i < 32; i++) {
        int hi = webhook_hex_nibble(p[i * 2]);
        int lo = webhook_hex_nibble(p[i * 2 + 1]);
        if (hi < 0 || lo < 0)
            return false;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    p += WEBHOOK_HMAC_SHA256_HEX_LEN;
    while (isspace((unsigned char)*p))
        p++;
    return *p == '\0';
}

bool webhook_verify_hmac_sha256_hex(const uint8_t *secret, size_t secret_len,
                                    const uint8_t *body, size_t body_len,
                                    const char *signature_header) {
    static const uint8_t empty_body[1] = {0};
    if (!secret || secret_len == 0 || (!body && body_len != 0) || !signature_header)
        return false;
    if (!body)
        body = empty_body;
    uint8_t expected[32];
    if (!webhook_parse_hmac_hex(signature_header, expected))
        return false;
    return hmac_sha256_verify(secret, secret_len, body, body_len, expected);
}

bool webhook_hmac_sha256_header(const uint8_t *secret, size_t secret_len,
                                const uint8_t *body, size_t body_len,
                                char *out, size_t out_len) {
    static const uint8_t empty_body[1] = {0};
    if (!secret || secret_len == 0 || (!body && body_len != 0) || !out || out_len < WEBHOOK_HMAC_SHA256_HEADER_LEN)
        return false;
    if (!body)
        body = empty_body;
    memcpy(out, "sha256=", 7);
    hmac_sha256_hex(secret, secret_len, body, body_len, out + 7);
    return out[7] != '\0';
}

static bool webhook_ascii_ieq_n(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i]))
            return false;
    }
    return true;
}

static bool webhook_host_is_local_name(const char *host) {
    if (!host || !*host)
        return false;
    if (strcmp(host, "localhost") == 0)
        return true;
    size_t len = strlen(host);
    return len > 10 && strcmp(host + len - 10, ".localhost") == 0;
}

static bool webhook_ipv4_is_private_or_reserved(const uint8_t b[4]) {
    if (b[0] == 0) return true;                         /* this network */
    if (b[0] == 10) return true;                        /* RFC1918 */
    if (b[0] == 127) return true;                       /* loopback */
    if (b[0] == 169 && b[1] == 254) return true;        /* link-local */
    if (b[0] == 172 && b[1] >= 16 && b[1] <= 31) return true;
    if (b[0] == 192 && b[1] == 168) return true;
    if (b[0] == 100 && b[1] >= 64 && b[1] <= 127) return true; /* CGNAT */
    if (b[0] == 192 && b[1] == 0 && b[2] == 0) return true;
    if (b[0] == 192 && b[1] == 0 && b[2] == 2) return true;    /* TEST-NET */
    if (b[0] == 198 && (b[1] == 18 || b[1] == 19)) return true;
    if (b[0] == 198 && b[1] == 51 && b[2] == 100) return true;
    if (b[0] == 203 && b[1] == 0 && b[2] == 113) return true;
    if (b[0] >= 224) return true;                      /* multicast/reserved */
    return false;
}

static bool webhook_ipv6_is_private_or_reserved(const uint8_t b[16]) {
    static const uint8_t loopback[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    if (memcmp(b, loopback, 16) == 0) return true;
    bool unspecified = true;
    for (size_t i = 0; i < 16; i++) unspecified = unspecified && b[i] == 0;
    if (unspecified) return true;
    if ((b[0] & 0xfe) == 0xfc) return true;            /* unique local fc00::/7 */
    if (b[0] == 0xfe && (b[1] & 0xc0) == 0x80) return true; /* link-local fe80::/10 */
    if (b[0] == 0xff) return true;                     /* multicast */
    if (b[0] == 0x20 && b[1] == 0x01 && b[2] == 0x0d && b[3] == 0xb8) return true;

    bool v4_mapped = true;
    for (size_t i = 0; i < 10; i++) v4_mapped = v4_mapped && b[i] == 0;
    v4_mapped = v4_mapped && b[10] == 0xff && b[11] == 0xff;
    if (v4_mapped)
        return webhook_ipv4_is_private_or_reserved(&b[12]);
    return false;
}

webhook_ssrf_decision_t webhook_ssrf_guard_url(const char *url, char *reason, size_t reason_len) {
    if (!url || !*url) {
        webhook_set_reason(reason, reason_len, "empty url");
        return WEBHOOK_SSRF_BLOCK_NULL_URL;
    }

    const char *scheme_end = strstr(url, "://");
    if (!scheme_end || scheme_end == url) {
        webhook_set_reason(reason, reason_len, "malformed url: missing scheme separator");
        return WEBHOOK_SSRF_BLOCK_MALFORMED_URL;
    }
    size_t scheme_len = (size_t)(scheme_end - url);
    if (!((scheme_len == 5 && webhook_ascii_ieq_n(url, "https", 5)) ||
          (scheme_len == 4 && webhook_ascii_ieq_n(url, "http", 4)))) {
        webhook_set_reason(reason, reason_len, "unsupported url scheme");
        return WEBHOOK_SSRF_BLOCK_UNSUPPORTED_SCHEME;
    }

    const char *authority = scheme_end + 3;
    if (*authority == '\0' || *authority == '/' || *authority == '?' || *authority == '#') {
        webhook_set_reason(reason, reason_len, "malformed url: missing authority");
        return WEBHOOK_SSRF_BLOCK_MALFORMED_URL;
    }

    const char *authority_end = authority;
    while (*authority_end && *authority_end != '/' && *authority_end != '?' && *authority_end != '#')
        authority_end++;

    const char *host_start = authority;
    const char *at = NULL;
    for (const char *p = authority; p < authority_end; p++) {
        if (*p == '@')
            at = p;
    }
    if (at)
        host_start = at + 1;

    char host[256];
    size_t host_len = 0;
    if (host_start < authority_end && *host_start == '[') {
        const char *close = memchr(host_start, ']', (size_t)(authority_end - host_start));
        if (!close) {
            webhook_set_reason(reason, reason_len, "malformed url: bad ipv6 literal");
            return WEBHOOK_SSRF_BLOCK_MALFORMED_URL;
        }
        host_start++;
        host_len = (size_t)(close - host_start);
    } else {
        const char *host_end = host_start;
        while (host_end < authority_end && *host_end != ':')
            host_end++;
        host_len = (size_t)(host_end - host_start);
    }

    if (host_len == 0 || host_len >= sizeof(host)) {
        webhook_set_reason(reason, reason_len, "malformed url: invalid host");
        return WEBHOOK_SSRF_BLOCK_MALFORMED_URL;
    }
    for (size_t i = 0; i < host_len; i++)
        host[i] = (char)tolower((unsigned char)host_start[i]);
    host[host_len] = '\0';

    /* Normalize a fully-qualified local name form before policy checks. */
    while (host_len > 0 && host[host_len - 1] == '.')
        host[--host_len] = '\0';

    if (webhook_host_is_local_name(host)) {
        webhook_set_reason(reason, reason_len, "blocked local host name");
        return WEBHOOK_SSRF_BLOCK_LOCAL_HOST;
    }

    uint8_t ip4[4];
    if (inet_pton(AF_INET, host, ip4) == 1) {
        if (webhook_ipv4_is_private_or_reserved(ip4)) {
            webhook_set_reason(reason, reason_len, "blocked private/reserved ipv4 literal");
            return WEBHOOK_SSRF_BLOCK_PRIVATE_IP_LITERAL;
        }
        webhook_set_reason(reason, reason_len, "allowed public ipv4 literal");
        return WEBHOOK_SSRF_ALLOW;
    }

    uint8_t ip6[16];
    if (inet_pton(AF_INET6, host, ip6) == 1) {
        if (webhook_ipv6_is_private_or_reserved(ip6)) {
            webhook_set_reason(reason, reason_len, "blocked private/reserved ipv6 literal");
            return WEBHOOK_SSRF_BLOCK_PRIVATE_IP_LITERAL;
        }
        webhook_set_reason(reason, reason_len, "allowed public ipv6 literal");
        return WEBHOOK_SSRF_ALLOW;
    }

    /* Stub policy for DNS names: syntactically allowed for now. Resolver-aware
     * enforcement must resolve and validate every A/AAAA before connect. */
    webhook_set_reason(reason, reason_len, "allowed dns host; resolver enforcement pending");
    return WEBHOOK_SSRF_ALLOW;
}

bool webhook_egress_url_allowed(const char *url, char *reason, size_t reason_len) {
    return webhook_ssrf_guard_url(url, reason, reason_len) == WEBHOOK_SSRF_ALLOW;
}
