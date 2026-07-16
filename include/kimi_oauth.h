#ifndef DSCO_KIMI_OAUTH_H
#define DSCO_KIMI_OAUTH_H

#include <stdbool.h>

/* Native Kimi Code subscription credential bridge. Reads and atomically
 * refreshes the 0600 OAuth cache produced by `kimi login`; inference and
 * token refresh remain in-process. */
const char *kimi_oauth_access_token(bool allow_refresh);
/* Refresh after a rejected request. If another process already rotated the
 * credential, returns that newer access token without rotating it again. */
const char *kimi_oauth_refresh_after_unauthorized(const char *rejected_token);
bool kimi_oauth_available(void);
const char *kimi_oauth_source_name(void);

#endif
