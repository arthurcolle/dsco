#ifndef DSCO_WEBHOOK_SECURITY_H
#define DSCO_WEBHOOK_SECURITY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Webhook ingress verification.
 *
 * Accepts provider-style HMAC-SHA256 signatures encoded as hex. The verifier
 * accepts either bare hex or a leading algorithm prefix such as "sha256=<hex>"
 * / "hmac-sha256=<hex>". Comparison is constant-time after parsing.
 */
bool webhook_verify_hmac_sha256_hex(const uint8_t *secret, size_t secret_len,
                                    const uint8_t *body, size_t body_len,
                                    const char *signature_header);

/* Compute an outbound/inbound signature in the canonical "sha256=<hex>" form.
 * out_len must be at least WEBHOOK_HMAC_SHA256_HEADER_LEN bytes.
 */
#define WEBHOOK_HMAC_SHA256_HEX_LEN 64u
#define WEBHOOK_HMAC_SHA256_HEADER_LEN 72u /* "sha256=" + 64 hex + NUL */

bool webhook_hmac_sha256_header(const uint8_t *secret, size_t secret_len,
                                const uint8_t *body, size_t body_len,
                                char *out, size_t out_len);

/* Webhook egress SSRF guard. It validates the URL and, for DNS names, checks
 * every A/AAAA answer before allowing the request. The resolved addresses can
 * also be supplied to libcurl so the connect cannot silently re-resolve a
 * rebinding hostname. */
typedef enum {
    WEBHOOK_SSRF_ALLOW = 0,
    WEBHOOK_SSRF_BLOCK_NULL_URL,
    WEBHOOK_SSRF_BLOCK_MALFORMED_URL,
    WEBHOOK_SSRF_BLOCK_UNSUPPORTED_SCHEME,
    WEBHOOK_SSRF_BLOCK_LOCAL_HOST,
    WEBHOOK_SSRF_BLOCK_PRIVATE_IP_LITERAL,
    WEBHOOK_SSRF_BLOCK_PRIVATE_RESOLVED_IP,
    WEBHOOK_SSRF_BLOCK_UNRESOLVED_HOST_STUB
} webhook_ssrf_decision_t;

webhook_ssrf_decision_t webhook_ssrf_guard_url(const char *url,
                                               char *reason,
                                               size_t reason_len);

#define WEBHOOK_RESOLVED_HOST_MAX 256u
#define WEBHOOK_RESOLVED_ADDR_MAX 16u
#define WEBHOOK_RESOLVED_ADDR_TEXT_MAX 64u

typedef struct {
    char host[WEBHOOK_RESOLVED_HOST_MAX];
    unsigned short port;
    char addresses[WEBHOOK_RESOLVED_ADDR_MAX][WEBHOOK_RESOLVED_ADDR_TEXT_MAX];
    size_t address_count;
} webhook_resolved_target_t;

/* Resolve a callback URL and return only public A/AAAA targets. The result is
 * a stable textual snapshot for CURLOPT_RESOLVE; callers must not resolve the
 * hostname again between this check and the network operation. */
bool webhook_resolve_public_url(const char *url, webhook_resolved_target_t *out,
                                char *reason, size_t reason_len);

bool webhook_egress_url_allowed(const char *url, char *reason, size_t reason_len);

#endif /* DSCO_WEBHOOK_SECURITY_H */
