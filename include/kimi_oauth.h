#ifndef DSCO_KIMI_OAUTH_H
#define DSCO_KIMI_OAUTH_H

#include <stdbool.h>
#include <time.h>

#include <curl/curl.h>

/* Native Kimi Code subscription credential bridge. Reads and atomically
 * refreshes the 0600 OAuth cache produced by `kimi login`; inference and
 * token refresh remain in-process. */
const char *kimi_oauth_access_token(bool allow_refresh);
/* Refresh after a rejected request. If another process already rotated the
 * credential, returns that newer access token without rotating it again. */
const char *kimi_oauth_refresh_after_unauthorized(const char *rejected_token);
bool kimi_oauth_available(void);
const char *kimi_oauth_source_name(void);

/* Append the stable Kimi coding-host identity required by both OAuth and
 * inference requests. The product/version identify the actual embedding
 * binary; the protocol platform value remains Kimi's documented coding lane. */
struct curl_slist *kimi_oauth_append_identity_headers(struct curl_slist *headers,
                                                       const char *product,
                                                       const char *version);

/* Query GET {coding base}/usages (the endpoint the official Kimi CLI uses)
 * and return the epoch time when the currently exhausted quota window next
 * resets, or 0 if unavailable. Network call; only invoke after a quota
 * rejection. */
time_t kimi_code_quota_reset_at(void);

#endif
