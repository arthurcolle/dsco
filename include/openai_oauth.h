#ifndef DSCO_OPENAI_OAUTH_H
#define DSCO_OPENAI_OAUTH_H

/* ── Native ChatGPT-subscription OAuth (Codex-style) ──────────────────────
 *
 * Implements the full OpenAI / ChatGPT OAuth PKCE flow used by the Codex CLI
 * so dsco can talk to the ChatGPT backend Responses API
 * (https://chatgpt.com/backend-api/codex/responses) using a logged-in
 * ChatGPT Plus/Pro/Team subscription instead of a metered API key.
 *
 * Login flow (openai_oauth_login):
 *   1. Generate a PKCE verifier/challenge + state.
 *   2. Spin up a loopback HTTP listener on 127.0.0.1:1455.
 *   3. Open the system browser at auth.openai.com/oauth/authorize.
 *   4. Capture ?code=...&state=... on /auth/callback.
 *   5. Exchange the code at auth.openai.com/oauth/token (PKCE, no secret).
 *   6. Decode the id_token to extract the chatgpt account id.
 *   7. Persist tokens (encrypted dsco cache + ~/.codex/auth.json compatible).
 *
 * Token resolution priority (openai_oauth_load):
 *   env DSCO_CHATGPT_OAUTH_TOKEN  →  ~/.dsco/chatgpt-oauth.json  →
 *   ~/.codex/auth.json (Codex CLI session).
 *
 * macOS + Linux. Browser open uses `open` / `xdg-open`.
 * ─────────────────────────────────────────────────────────────────────── */

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    OPENAI_OAUTH_SOURCE_MISSING = 0,
    OPENAI_OAUTH_SOURCE_ENV,
    OPENAI_OAUTH_SOURCE_DSCO_CACHE,
    OPENAI_OAUTH_SOURCE_CODEX,
} openai_oauth_source_t;

typedef struct {
    openai_oauth_source_t source;
    char access_token[8192];
    char refresh_token[8192];
    char id_token[8192];
    char account_id[128];
    long long expires_at_ms; /* 0 == unknown */
} openai_oauth_bundle_t;

/* Load the best available bundle. Returns true if an access token was found. */
bool openai_oauth_load(openai_oauth_bundle_t *out);

/* Refresh the bundle in place using its refresh_token. Persists on success. */
bool openai_oauth_refresh(openai_oauth_bundle_t *bundle);

/* Resolve a usable access token, refreshing if near expiry when allow_refresh.
 * Returns a pointer to a static buffer (valid until next call) or NULL. */
const char *openai_oauth_access_token(bool allow_refresh);

/* Copy the chatgpt account id into out. Returns true if available. */
bool openai_oauth_account_id(char *out, size_t out_len);

/* True if a ChatGPT subscription token is resolvable (env/cache/codex). */
bool openai_oauth_available(void);

/* Human-readable source: "env", "dsco-cache", "codex", or "missing". */
const char *openai_oauth_source_name(void);

/* Run the interactive browser PKCE login. Returns 0 on success. */
int openai_oauth_login(void);

/* Remove the dsco-managed token cache. Returns 0 on success. */
int openai_oauth_logout(void);

#endif /* DSCO_OPENAI_OAUTH_H */
