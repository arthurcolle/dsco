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

/* Webhook egress SSRF guard.
 *
 * This is a syntactic/IP-literal guard intended to run before any network I/O.
 * It blocks unsupported schemes, local hosts, and private/reserved IP literals.
 * A future resolver-aware slice should add DNS resolution + connect-time IP
 * enforcement to close DNS rebinding and CNAME-to-private gaps.
 */
typedef enum {
    WEBHOOK_SSRF_ALLOW = 0,
    WEBHOOK_SSRF_BLOCK_NULL_URL,
    WEBHOOK_SSRF_BLOCK_MALFORMED_URL,
    WEBHOOK_SSRF_BLOCK_UNSUPPORTED_SCHEME,
    WEBHOOK_SSRF_BLOCK_LOCAL_HOST,
    WEBHOOK_SSRF_BLOCK_PRIVATE_IP_LITERAL,
    WEBHOOK_SSRF_BLOCK_UNRESOLVED_HOST_STUB
} webhook_ssrf_decision_t;

webhook_ssrf_decision_t webhook_ssrf_guard_url(const char *url,
                                               char *reason,
                                               size_t reason_len);

bool webhook_egress_url_allowed(const char *url, char *reason, size_t reason_len);

#endif /* DSCO_WEBHOOK_SECURITY_H */
