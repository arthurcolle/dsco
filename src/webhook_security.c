#include "webhook_security.h"

#include "crypto.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>

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

    /* Validate the complete encoded length before indexing pairs below. A
     * short attacker-controlled header must be rejected without reading past
     * its terminating NUL. Trailing whitespace is handled after decoding. */
    size_t encoded_len = 0;
    while (webhook_hex_nibble(p[encoded_len]) >= 0)
        encoded_len++;
    if (encoded_len != WEBHOOK_HMAC_SHA256_HEX_LEN)
        return false;

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

typedef struct {
    char host[WEBHOOK_RESOLVED_HOST_MAX];
    char port_text[6];
    unsigned short port;
    bool https;
} webhook_url_parts_t;

static bool webhook_parse_port(const char *start, const char *end, char out[6],
                               unsigned short *port) {
    if (!start || !end || start >= end || !out || !port || (size_t)(end - start) >= 6)
        return false;
    unsigned long value = 0;
    for (const char *p = start; p < end; p++) {
        if (*p < '0' || *p > '9')
            return false;
        value = value * 10U + (unsigned long)(*p - '0');
        if (value > 65535U)
            return false;
    }
    if (value == 0)
        return false;
    size_t len = (size_t)(end - start);
    memcpy(out, start, len);
    out[len] = '\0';
    *port = (unsigned short)value;
    return true;
}

static webhook_ssrf_decision_t webhook_parse_url(const char *url, webhook_url_parts_t *parts,
                                                 char *reason, size_t reason_len) {
    if (!url || !*url) {
        webhook_set_reason(reason, reason_len, "empty url");
        return WEBHOOK_SSRF_BLOCK_NULL_URL;
    }
    if (!parts) {
        webhook_set_reason(reason, reason_len, "internal url parser error");
        return WEBHOOK_SSRF_BLOCK_MALFORMED_URL;
    }
    memset(parts, 0, sizeof(*parts));

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
    parts->https = scheme_len == 5;
    snprintf(parts->port_text, sizeof(parts->port_text), "%s", parts->https ? "443" : "80");
    parts->port = (unsigned short)(parts->https ? 443 : 80);

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

    char host[WEBHOOK_RESOLVED_HOST_MAX];
    size_t host_len = 0;
    const char *port_start = NULL;
    if (host_start < authority_end && *host_start == '[') {
        const char *close = memchr(host_start, ']', (size_t)(authority_end - host_start));
        if (!close) {
            webhook_set_reason(reason, reason_len, "malformed url: bad ipv6 literal");
            return WEBHOOK_SSRF_BLOCK_MALFORMED_URL;
        }
        host_start++;
        host_len = (size_t)(close - host_start);
        if (close + 1 < authority_end) {
            if (close[1] != ':') {
                webhook_set_reason(reason, reason_len, "malformed url: invalid port separator");
                return WEBHOOK_SSRF_BLOCK_MALFORMED_URL;
            }
            port_start = close + 2;
        }
    } else {
        const char *host_end = host_start;
        while (host_end < authority_end && *host_end != ':')
            host_end++;
        host_len = (size_t)(host_end - host_start);
        if (host_end < authority_end) {
            port_start = host_end + 1;
            if (memchr(port_start, ':', (size_t)(authority_end - port_start))) {
                webhook_set_reason(reason, reason_len, "malformed url: ipv6 literal must be bracketed");
                return WEBHOOK_SSRF_BLOCK_MALFORMED_URL;
            }
        }
    }

    if (host_len == 0 || host_len >= sizeof(host)) {
        webhook_set_reason(reason, reason_len, "malformed url: invalid host");
        return WEBHOOK_SSRF_BLOCK_MALFORMED_URL;
    }
    for (size_t i = 0; i < host_len; i++) {
        unsigned char c = (unsigned char)host_start[i];
        if (c <= 0x20 || c == '/' || c == '?' || c == '#' || c == '@') {
            webhook_set_reason(reason, reason_len, "malformed url: invalid host character");
            return WEBHOOK_SSRF_BLOCK_MALFORMED_URL;
        }
        host[i] = (char)tolower(c);
    }
    host[host_len] = '\0';

    /* Normalize a fully-qualified local name form before policy checks. */
    while (host_len > 0 && host[host_len - 1] == '.')
        host[--host_len] = '\0';
    if (host_len == 0) {
        webhook_set_reason(reason, reason_len, "malformed url: empty host");
        return WEBHOOK_SSRF_BLOCK_MALFORMED_URL;
    }

    if (port_start && !webhook_parse_port(port_start, authority_end, parts->port_text,
                                          &parts->port)) {
        webhook_set_reason(reason, reason_len, "malformed url: invalid port");
        return WEBHOOK_SSRF_BLOCK_MALFORMED_URL;
    }
    snprintf(parts->host, sizeof(parts->host), "%s", host);

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

    return WEBHOOK_SSRF_ALLOW;
}

static webhook_ssrf_decision_t webhook_resolve_target(const char *url,
                                                       webhook_resolved_target_t *out,
                                                       char *reason, size_t reason_len) {
    if (out)
        memset(out, 0, sizeof(*out));
    webhook_url_parts_t parts;
    webhook_ssrf_decision_t parsed = webhook_parse_url(url, &parts, reason, reason_len);
    if (parsed != WEBHOOK_SSRF_ALLOW)
        return parsed;

    uint8_t ip4[4];
    if (inet_pton(AF_INET, parts.host, ip4) == 1) {
        if (webhook_ipv4_is_private_or_reserved(ip4)) {
            webhook_set_reason(reason, reason_len, "blocked private/reserved ipv4 literal");
            return WEBHOOK_SSRF_BLOCK_PRIVATE_IP_LITERAL;
        }
        if (out) {
            snprintf(out->host, sizeof(out->host), "%s", parts.host);
            out->port = parts.port;
            inet_ntop(AF_INET, ip4, out->addresses[0], sizeof(out->addresses[0]));
            out->address_count = 1;
        }
        webhook_set_reason(reason, reason_len, "allowed public ipv4 literal");
        return WEBHOOK_SSRF_ALLOW;
    }

    uint8_t ip6[16];
    if (inet_pton(AF_INET6, parts.host, ip6) == 1) {
        if (webhook_ipv6_is_private_or_reserved(ip6)) {
            webhook_set_reason(reason, reason_len, "blocked private/reserved ipv6 literal");
            return WEBHOOK_SSRF_BLOCK_PRIVATE_IP_LITERAL;
        }
        if (out) {
            snprintf(out->host, sizeof(out->host), "%s", parts.host);
            out->port = parts.port;
            inet_ntop(AF_INET6, ip6, out->addresses[0], sizeof(out->addresses[0]));
            out->address_count = 1;
        }
        webhook_set_reason(reason, reason_len, "allowed public ipv6 literal");
        return WEBHOOK_SSRF_ALLOW;
    }

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *answers = NULL;
    int gai = getaddrinfo(parts.host, parts.port_text, &hints, &answers);
    if (gai != 0 || !answers) {
        if (answers)
            freeaddrinfo(answers);
        webhook_set_reason(reason, reason_len, "blocked unresolved dns host");
        return WEBHOOK_SSRF_BLOCK_UNRESOLVED_HOST_STUB;
    }

    webhook_resolved_target_t candidate;
    memset(&candidate, 0, sizeof(candidate));
    snprintf(candidate.host, sizeof(candidate.host), "%s", parts.host);
    candidate.port = parts.port;
    for (struct addrinfo *ai = answers; ai; ai = ai->ai_next) {
        char numeric[WEBHOOK_RESOLVED_ADDR_TEXT_MAX];
        bool private = false;
        int family = ai->ai_family;
        if (family == AF_INET && ai->ai_addrlen >= sizeof(struct sockaddr_in)) {
            const struct sockaddr_in *sa = (const struct sockaddr_in *)ai->ai_addr;
            private = webhook_ipv4_is_private_or_reserved((const uint8_t *)&sa->sin_addr);
            if (!inet_ntop(AF_INET, &sa->sin_addr, numeric, sizeof(numeric)))
                continue;
        } else if (family == AF_INET6 && ai->ai_addrlen >= sizeof(struct sockaddr_in6)) {
            const struct sockaddr_in6 *sa6 = (const struct sockaddr_in6 *)ai->ai_addr;
            private = webhook_ipv6_is_private_or_reserved((const uint8_t *)&sa6->sin6_addr);
            if (!inet_ntop(AF_INET6, &sa6->sin6_addr, numeric, sizeof(numeric)))
                continue;
        } else {
            continue;
        }
        if (private) {
            freeaddrinfo(answers);
            webhook_set_reason(reason, reason_len, "blocked private/reserved dns answer");
            return WEBHOOK_SSRF_BLOCK_PRIVATE_RESOLVED_IP;
        }
        bool duplicate = false;
        for (size_t i = 0; i < candidate.address_count; i++)
            duplicate = duplicate || strcmp(candidate.addresses[i], numeric) == 0;
        if (!duplicate && candidate.address_count < WEBHOOK_RESOLVED_ADDR_MAX)
            snprintf(candidate.addresses[candidate.address_count++],
                     sizeof(candidate.addresses[0]), "%s", numeric);
    }
    freeaddrinfo(answers);
    if (candidate.address_count == 0) {
        webhook_set_reason(reason, reason_len, "blocked dns host with no public address");
        return WEBHOOK_SSRF_BLOCK_UNRESOLVED_HOST_STUB;
    }
    if (out)
        *out = candidate;
    webhook_set_reason(reason, reason_len, "allowed public dns host");
    return WEBHOOK_SSRF_ALLOW;
}

bool webhook_resolve_public_url(const char *url, webhook_resolved_target_t *out,
                                char *reason, size_t reason_len) {
    return webhook_resolve_target(url, out, reason, reason_len) == WEBHOOK_SSRF_ALLOW;
}

webhook_ssrf_decision_t webhook_ssrf_guard_url(const char *url, char *reason, size_t reason_len) {
    return webhook_resolve_target(url, NULL, reason, reason_len);
}

bool webhook_egress_url_allowed(const char *url, char *reason, size_t reason_len) {
    return webhook_ssrf_guard_url(url, reason, reason_len) == WEBHOOK_SSRF_ALLOW;
}
