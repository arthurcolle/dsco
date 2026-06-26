/* provider.c — Multi-provider abstraction for LLM API calls.
 *
 * Currently supports:
 *   - "anthropic" — Anthropic Messages API (default)
 *   - "openai"    — OpenAI Chat Completions API (and compatible)
 *
 * The Anthropic provider delegates to the existing llm.c functions.
 * The OpenAI provider implements request building and SSE parsing
 * for the Chat Completions API format.
 */

#include "provider.h"
#include "http_pool.h"
#include "config.h"
#include "crypto.h"
#include "tools.h"
#include "sealed_store.h"
#include "provider_profiles.h"
#include "openai_oauth.h"
#include "codex_cache.h"
#include "dcr.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <ctype.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <pwd.h>
#include <unistd.h>
#include <time.h>
#include <curl/curl.h>

#ifdef HAVE_LIBSODIUM
#include <sodium.h>
#endif

void llm_debug_save_request(const char *request_json, int http_status);

/* Forward declarations */
static char *openai_build_request(provider_t *p, conversation_t *conv, session_state_t *session,
                                  int max_tokens, const char *credential);
static struct curl_slist *openai_build_headers(provider_t *p, const char *api_key);
static char *xai_build_request(provider_t *p, conversation_t *conv, session_state_t *session,
                               int max_tokens, const char *credential);
static char *codex_exec_build_request(provider_t *p, conversation_t *conv, session_state_t *session,
                                      int max_tokens, const char *credential);
static stream_result_t codex_exec_stream(provider_t *p, const char *api_key,
                                         const char *request_json, stream_text_cb text_cb,
                                         stream_tool_start_cb tool_cb,
                                         stream_thinking_cb thinking_cb, void *cb_ctx);
static bool provider_is_sakana(const provider_t *p);
static bool provider_is_local_endpoint(const char *name);

static char *unsupported_build_request(provider_t *p, conversation_t *conv,
                                       session_state_t *session, int max_tokens,
                                       const char *credential) {
    (void)conv;
    (void)session;
    (void)max_tokens;
    (void)credential;
    const char *name = p && p->name ? p->name : "unknown";
    const char *mode = p && p->data ? (const char *)p->data : "unknown";
    jbuf_t b;
    jbuf_init(&b, 256);
    jbuf_append(&b, "{\"error\":\"provider transport not implemented\",");
    jbuf_append(&b, "\"provider\":");
    jbuf_append_json_str(&b, name);
    jbuf_append(&b, ",\"transport\":");
    jbuf_append_json_str(&b, mode);
    jbuf_append(&b, "}");
    return b.data;
}

static struct curl_slist *unsupported_build_headers(provider_t *p, const char *api_key) {
    (void)p;
    (void)api_key;
    return NULL;
}

static stream_result_t unsupported_stream(provider_t *p, const char *api_key,
                                          const char *request_json, stream_text_cb text_cb,
                                          stream_tool_start_cb tool_cb,
                                          stream_thinking_cb thinking_cb, void *cb_ctx) {
    (void)api_key;
    (void)request_json;
    (void)tool_cb;
    (void)thinking_cb;
    stream_result_t result = {0};
    result.ok = false;
    result.http_status = 501;
    result.parsed.stop_reason = safe_strdup("unsupported_provider");
    result.parsed.blocks = safe_malloc(sizeof(content_block_t));
    memset(result.parsed.blocks, 0, sizeof(content_block_t));
    result.parsed.count = 1;
    result.parsed.blocks[0].type = safe_strdup("text");

    const char *name = p && p->name ? p->name : "unknown";
    const char *mode = p && p->data ? (const char *)p->data : "unknown";
    char msg[512];
    snprintf(msg, sizeof(msg),
             "Provider '%s' is known, but DSCO has no '%s' transport adapter yet.", name, mode);
    result.parsed.blocks[0].text = safe_strdup(msg);
    if (text_cb)
        text_cb(msg, cb_ctx);
    return result;
}

/* ── Anthropic Provider ────────────────────────────────────────────────── */

static char *anthropic_build_request(provider_t *p, conversation_t *conv, session_state_t *session,
                                     int max_tokens, const char *credential) {
    (void)p;
    return llm_build_request_ex_for_credential(conv, session, max_tokens, credential);
}

static struct curl_slist *anthropic_build_headers(provider_t *p, const char *api_key) {
    (void)p;
    struct curl_slist *hdrs = NULL;
    hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
    hdrs = curl_slist_append(hdrs, "Accept: text/event-stream");
    char auth[512];
    if (llm_anthropic_uses_claude_code_auth(api_key))
        snprintf(auth, sizeof(auth), "Authorization: Bearer %s", api_key);
    else
        snprintf(auth, sizeof(auth), "x-api-key: %s", api_key);
    hdrs = curl_slist_append(hdrs, auth);
    char ver[128];
    snprintf(ver, sizeof(ver), "anthropic-version: %s", ANTHROPIC_VERSION);
    hdrs = curl_slist_append(hdrs, ver);
    char beta[256];
    if (llm_anthropic_uses_claude_code_auth(api_key))
        snprintf(beta, sizeof(beta), "anthropic-beta: oauth-2025-04-20,%s", ANTHROPIC_BETAS);
    else
        snprintf(beta, sizeof(beta), "anthropic-beta: %s", ANTHROPIC_BETAS);
    hdrs = curl_slist_append(hdrs, beta);
    hdrs = curl_slist_append(hdrs, "Expect:");
    return hdrs;
}

static stream_result_t anthropic_stream(provider_t *p, const char *api_key,
                                        const char *request_json, stream_text_cb text_cb,
                                        stream_tool_start_cb tool_cb,
                                        stream_thinking_cb thinking_cb, void *cb_ctx) {
    (void)p;
    return llm_stream(api_key, request_json, text_cb, tool_cb, thinking_cb, cb_ctx);
}

/* ── OpenRouter Provider ────────────────────────────────────────────────── */

static struct curl_slist *openrouter_build_headers(provider_t *p, const char *api_key) {
    struct curl_slist *hdrs = openai_build_headers(p, api_key);
    const char *referer = getenv("DSCO_OR_REFERER");
    if (!referer)
        referer = "https://github.com/dsco-cli";
    char hdr[512];
    snprintf(hdr, sizeof(hdr), "HTTP-Referer: %s", referer);
    hdrs = curl_slist_append(hdrs, hdr);
    const char *title = getenv("DSCO_OR_TITLE");
    if (!title)
        title = "dsco";
    snprintf(hdr, sizeof(hdr), "X-Title: %s", title);
    hdrs = curl_slist_append(hdrs, hdr);
    return hdrs;
}

/* Helper: append a comma-separated env var as a JSON string array */
static void or_append_csv_array(jbuf_t *b, const char *csv) {
    jbuf_append(b, "[");
    const char *cur = csv;
    bool first = true;
    while (*cur) {
        const char *end = strchr(cur, ',');
        if (!end)
            end = cur + strlen(cur);
        size_t n = (size_t)(end - cur);
        char name[128];
        if (n >= sizeof(name))
            n = sizeof(name) - 1;
        memcpy(name, cur, n);
        name[n] = '\0';
        char *s = name;
        while (*s == ' ')
            s++;
        char *e2 = s + strlen(s) - 1;
        while (e2 > s && *e2 == ' ')
            *e2-- = '\0';
        if (s[0]) {
            if (!first)
                jbuf_append(b, ",");
            jbuf_append_json_str(b, s);
            first = false;
        }
        cur = *end ? end + 1 : end;
    }
    jbuf_append(b, "]");
}

static bool or_env_bool(const char *val) {
    return val && (val[0] == '1' || strcasecmp(val, "true") == 0);
}

static bool provider_env_truthy(const char *val) {
    return val && (val[0] == '1' || strcasecmp(val, "true") == 0 || strcasecmp(val, "yes") == 0);
}

static bool provider_env_matches(const char *val, const char *a, const char *b) {
    return val && ((a && strcasecmp(val, a) == 0) || (b && strcasecmp(val, b) == 0));
}

static const char *provider_sakana_subscription_key(void);
static const char *provider_sakana_payg_key(void);

static double provider_now_sec(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + (double)tv.tv_usec / 1000000.0;
}

bool provider_debug_auth_enabled(void) {
    return provider_env_truthy(getenv("DSCO_DEBUG_AUTH"));
}

const char *provider_auth_mode(const char *provider_name, const char *resolved_key) {
    if (!resolved_key || !resolved_key[0])
        return "missing";
    if (provider_name && strcmp(provider_name, "anthropic") == 0) {
        return llm_anthropic_uses_claude_code_auth(resolved_key) ? "claude-code-oauth"
                                                                 : "anthropic-api-key";
    }
    if (provider_name && strcmp(provider_name, "openrouter") == 0)
        return "openrouter-api-key";
    if (provider_name && strcmp(provider_name, "openai") == 0)
        return "openai-api-key";
    if (provider_name && strcmp(provider_name, "openai-codex") == 0)
        return "chatgpt-subscription";
    if (provider_name && strcmp(provider_profile_canonical_name(provider_name), "sakana") == 0) {
        const char *payg = provider_sakana_payg_key();
        if (resolved_key && payg && strcmp(resolved_key, payg) == 0)
            return "sakana-payg-api-key";
        const char *sub = provider_sakana_subscription_key();
        if (resolved_key && sub && strcmp(resolved_key, sub) == 0)
            return "sakana-subscription-api-key";
        return provider_sakana_current_key_is_subscription() ? "sakana-subscription-api-key"
                                                            : "sakana-payg-api-key";
    }
    return "api-key";
}

void provider_debug_log_request(const char *provider_name, const char *model,
                                const char *resolved_key) {
    if (!provider_debug_auth_enabled())
        return;
    fprintf(stderr, "  [auth] provider=%s model=%s auth=%s\n",
            provider_name && provider_name[0] ? provider_name : "(none)",
            model && model[0] ? model : "(none)", provider_auth_mode(provider_name, resolved_key));
}

static void provider_expand_path(char *out, size_t out_len, const char *path) {
    if (!out || out_len == 0)
        return;
    out[0] = '\0';
    if (!path || !path[0])
        return;

    if (path[0] == '~' && path[1] == '/') {
        const char *home = getenv("HOME");
        if (!home || !home[0])
            return;
        snprintf(out, out_len, "%s/%s", home, path + 2);
        return;
    }

    snprintf(out, out_len, "%s", path);
}

static char *provider_read_text_file(const char *path) {
    if (!path || !path[0])
        return NULL;
    FILE *fp = fopen(path, "rb");
    if (!fp)
        return NULL;
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return NULL;
    }
    long size = ftell(fp);
    if (size < 0) {
        fclose(fp);
        return NULL;
    }
    rewind(fp);

    char *data = safe_malloc((size_t)size + 1);
    size_t got = fread(data, 1, (size_t)size, fp);
    fclose(fp);
    data[got] = '\0';
    return data;
}

static bool provider_find_executable(const char *name, char *out, size_t out_len) {
    if (out && out_len)
        out[0] = '\0';
    if (!name || !name[0])
        return false;

    if (strchr(name, '/')) {
        if (access(name, X_OK) == 0) {
            if (out && out_len)
                snprintf(out, out_len, "%s", name);
            return true;
        }
        return false;
    }

    const char *path = getenv("PATH");
    if (!path || !path[0])
        path = "/usr/local/bin:/usr/bin:/bin:/opt/homebrew/bin";

    char *copy = safe_strdup(path);
    bool found = false;
    for (char *dir = copy; dir && *dir;) {
        char *colon = strchr(dir, ':');
        if (colon)
            *colon = '\0';
        const char *base = dir[0] ? dir : ".";
        char candidate[1024];
        snprintf(candidate, sizeof(candidate), "%s/%s", base, name);
        if (access(candidate, X_OK) == 0) {
            if (out && out_len)
                snprintf(out, out_len, "%s", candidate);
            found = true;
            break;
        }
        if (!colon)
            break;
        dir = colon + 1;
    }
    free(copy);
    return found;
}

static bool provider_codex_chatgpt_auth_available(void) {
    if (provider_env_truthy(getenv("DSCO_DISABLE_CODEX_OAUTH_DISCOVERY")))
        return false;

    const char *home = getenv("HOME");
    if (!home || !home[0])
        return false;

    char auth_path[1024];
    snprintf(auth_path, sizeof(auth_path), "%s/.codex/auth.json", home);
    char *json = provider_read_text_file(auth_path);
    if (!json)
        return false;

    char *mode = json_get_str(json, "auth_mode");
    bool ok = mode && strcmp(mode, "chatgpt") == 0;
    free(mode);
    free(json);
    return ok;
}

static bool provider_codex_exec_ready(void) {
    if (!provider_codex_chatgpt_auth_available())
        return false;
    return provider_find_executable("codex", NULL, 0);
}

/* Native ChatGPT-subscription path: dsco resolves the OAuth token itself and
 * talks to the backend Responses API directly (no codex binary). This is the
 * preferred path; the codex subprocess is a legacy fallback. */
static bool provider_chatgpt_native_ready(void) {
    if (provider_env_truthy(getenv("DSCO_DISABLE_CHATGPT_NATIVE")))
        return false;
    return openai_oauth_available();
}

/* True if any ChatGPT-subscription path (native or legacy subprocess) works. */
static bool provider_chatgpt_subscription_ready(void) {
    return provider_chatgpt_native_ready() || provider_codex_exec_ready();
}

static const char *provider_codex_subscription_credential(void) {
    if (provider_chatgpt_native_ready()) {
        const char *tok = openai_oauth_access_token(true);
        if (tok && tok[0])
            return tok;
    }
    return provider_codex_exec_ready() ? "chatgpt-subscription" : NULL;
}

static void provider_build_claude_code_service_name(char *out, size_t out_len) {
    const char *override = getenv("DSCO_CLAUDE_CODE_KEYCHAIN_SERVICE");
    if (override && override[0]) {
        snprintf(out, out_len, "%s", override);
        return;
    }

    const char *oauth_suffix = "";
    if (getenv("CLAUDE_CODE_CUSTOM_OAUTH_URL")) {
        oauth_suffix = "-custom-oauth";
    } else if (getenv("USER_TYPE") && strcmp(getenv("USER_TYPE"), "ant") == 0 &&
               provider_env_truthy(getenv("USE_LOCAL_OAUTH"))) {
        oauth_suffix = "-local-oauth";
    } else if (getenv("USER_TYPE") && strcmp(getenv("USER_TYPE"), "ant") == 0 &&
               provider_env_truthy(getenv("USE_STAGING_OAUTH"))) {
        oauth_suffix = "-staging-oauth";
    }

    char dir_suffix[16] = "";
    const char *config_dir = getenv("CLAUDE_CONFIG_DIR");
    if (config_dir && config_dir[0]) {
        char dir_hash[65];
        sha256_hex((const uint8_t *)config_dir, strlen(config_dir), dir_hash);
        snprintf(dir_suffix, sizeof(dir_suffix), "-%.8s", dir_hash);
    }

    snprintf(out, out_len, "Claude Code%s-credentials%s", oauth_suffix, dir_suffix);
}

#define CLAUDE_CODE_OAUTH_TOKEN_URL "https://platform.claude.com/v1/oauth/token"
#define CLAUDE_CODE_OAUTH_CLIENT_ID "9d1c250a-e61b-44d9-88ed-5944d1962f5e"
#define CLAUDE_CODE_OAUTH_SCOPES                                                                   \
    "user:profile user:inference user:sessions:claude_code user:mcp_servers user:file_upload"
#define CLAUDE_CODE_OAUTH_EXPIRY_BUFFER_MS (5LL * 60LL * 1000LL)

typedef enum {
    CLAUDE_CODE_OAUTH_SOURCE_MISSING = 0,
    CLAUDE_CODE_OAUTH_SOURCE_ENV,
    CLAUDE_CODE_OAUTH_SOURCE_KEYCHAIN,
    CLAUDE_CODE_OAUTH_SOURCE_FILE,
} claude_code_oauth_source_t;

typedef struct {
    claude_code_oauth_source_t source;
    char access_token[4096];
    char refresh_token[4096];
    long long expires_at_ms;
    char credentials_path[1024];
    char keychain_service[128];
    char keychain_account[128];
    char *storage_json;
    char *oauth_json;
} claude_code_oauth_bundle_t;

static void provider_claude_code_oauth_bundle_init(claude_code_oauth_bundle_t *bundle) {
    memset(bundle, 0, sizeof(*bundle));
}

static void provider_claude_code_oauth_bundle_free(claude_code_oauth_bundle_t *bundle) {
    if (!bundle)
        return;
    free(bundle->storage_json);
    free(bundle->oauth_json);
    bundle->storage_json = NULL;
    bundle->oauth_json = NULL;
}

static const char *provider_claude_code_oauth_source_name(claude_code_oauth_source_t source) {
    switch (source) {
        case CLAUDE_CODE_OAUTH_SOURCE_ENV:
            return "env";
        case CLAUDE_CODE_OAUTH_SOURCE_KEYCHAIN:
            return "macos-keychain";
        case CLAUDE_CODE_OAUTH_SOURCE_FILE:
            return "credentials-file";
        default:
            return "missing";
    }
}

static long long provider_now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000LL + (long long)tv.tv_usec / 1000LL;
}

static bool provider_claude_code_oauth_expired(long long expires_at_ms) {
    if (expires_at_ms <= 0)
        return false;
    return provider_now_ms() + CLAUDE_CODE_OAUTH_EXPIRY_BUFFER_MS >= expires_at_ms;
}

static void provider_get_username(char *out, size_t out_len) {
    if (!out || out_len == 0)
        return;
    out[0] = '\0';

    const char *user = getenv("USER");
    if (user && user[0]) {
        snprintf(out, out_len, "%s", user);
        return;
    }

    struct passwd *pw = getpwuid(getuid());
    if (pw && pw->pw_name && pw->pw_name[0]) {
        snprintf(out, out_len, "%s", pw->pw_name);
        return;
    }

    snprintf(out, out_len, "claude-code-user");
}

static void provider_build_claude_code_credentials_path(char *out, size_t out_len) {
    const char *override_path = getenv("DSCO_CLAUDE_CODE_CREDENTIALS_FILE");
    if (override_path && override_path[0]) {
        provider_expand_path(out, out_len, override_path);
        return;
    }

    const char *config_dir = getenv("CLAUDE_CONFIG_DIR");
    if (config_dir && config_dir[0]) {
        snprintf(out, out_len, "%s/.credentials.json", config_dir);
        return;
    }

    provider_expand_path(out, out_len, "~/.claude/.credentials.json");
}

/* ── dsco-owned encrypted cache for the Claude Code OAuth bundle ───────────
 *
 * Touching Claude Code's keychain entry via the `security` CLI pops a system
 * password prompt on every dsco invocation unless the user has clicked "Always
 * Allow". To make dsco a practical tool we cache the bundle once, encrypted
 * with the sealed_store master key (which dsco already loads at startup), and
 * read from there on subsequent runs. Cache is invalidated when expired so
 * Claude Code remains the source of truth for refreshes.
 *
 * Disabled by setting DSCO_DISABLE_CLAUDE_CODE_LOCAL_CACHE=1.
 * Override path with DSCO_CLAUDE_CODE_LOCAL_CACHE.
 * ────────────────────────────────────────────────────────────────────────── */

#define DSCO_CC_CACHE_MAGIC "DSCC1"
#define DSCO_CC_CACHE_MAGIC_LEN 5

static void provider_build_dsco_cc_cache_path(char *out, size_t out_len) {
    const char *override = getenv("DSCO_CLAUDE_CODE_LOCAL_CACHE");
    if (override && override[0]) {
        provider_expand_path(out, out_len, override);
        return;
    }
    const char *home = getenv("HOME");
    if (!home || !home[0])
        home = "/tmp";
    snprintf(out, out_len, "%s/.dsco/cc-oauth.bin", home);
}

static bool provider_save_claude_code_bundle_local_cache(const char *storage_json) {
#ifdef HAVE_LIBSODIUM
    if (!storage_json || !storage_json[0])
        return false;
    if (provider_env_truthy(getenv("DSCO_DISABLE_CLAUDE_CODE_LOCAL_CACHE")))
        return false;

    uint8_t key[32];
    if (!sealed_store_master_key_copy(key))
        return false;

    size_t json_len = strlen(storage_json);
    size_t cipher_len = crypto_secretbox_MACBYTES + json_len;
    uint8_t *cipher = malloc(cipher_len);
    if (!cipher) {
        sodium_memzero(key, sizeof(key));
        return false;
    }

    uint8_t nonce[crypto_secretbox_NONCEBYTES];
    randombytes_buf(nonce, sizeof(nonce));

    if (crypto_secretbox_easy(cipher, (const uint8_t *)storage_json, json_len, nonce, key) != 0) {
        sodium_memzero(key, sizeof(key));
        free(cipher);
        return false;
    }
    sodium_memzero(key, sizeof(key));

    char path[1024];
    provider_build_dsco_cc_cache_path(path, sizeof(path));

    char dir[1024];
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        (void)mkdir(dir, 0700);
    }

    char tmp_path[1100];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.XXXXXX", path);
    int fd = mkstemp(tmp_path);
    if (fd < 0) {
        free(cipher);
        return false;
    }
    (void)fchmod(fd, 0600);

    bool ok =
        (write(fd, DSCO_CC_CACHE_MAGIC, DSCO_CC_CACHE_MAGIC_LEN) == DSCO_CC_CACHE_MAGIC_LEN) &&
        (write(fd, nonce, sizeof(nonce)) == (ssize_t)sizeof(nonce)) &&
        (write(fd, cipher, cipher_len) == (ssize_t)cipher_len);
    close(fd);
    free(cipher);

    if (!ok || rename(tmp_path, path) != 0) {
        unlink(tmp_path);
        return false;
    }
    return true;
#else
    (void)storage_json;
    return false;
#endif
}

static char *provider_load_claude_code_bundle_local_cache(void) {
#ifdef HAVE_LIBSODIUM
    if (provider_env_truthy(getenv("DSCO_DISABLE_CLAUDE_CODE_LOCAL_CACHE")))
        return NULL;

    char path[1024];
    provider_build_dsco_cc_cache_path(path, sizeof(path));

    FILE *fp = fopen(path, "rb");
    if (!fp)
        return NULL;

    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return NULL;
    }
    long size = ftell(fp);
    rewind(fp);

    long min_size = (long)DSCO_CC_CACHE_MAGIC_LEN + (long)crypto_secretbox_NONCEBYTES +
                    (long)crypto_secretbox_MACBYTES;
    if (size <= min_size || size > (long)(1024 * 1024)) {
        fclose(fp);
        return NULL;
    }

    uint8_t magic[DSCO_CC_CACHE_MAGIC_LEN];
    uint8_t nonce[crypto_secretbox_NONCEBYTES];
    if (fread(magic, 1, sizeof(magic), fp) != sizeof(magic) ||
        memcmp(magic, DSCO_CC_CACHE_MAGIC, DSCO_CC_CACHE_MAGIC_LEN) != 0 ||
        fread(nonce, 1, sizeof(nonce), fp) != sizeof(nonce)) {
        fclose(fp);
        return NULL;
    }

    size_t cipher_len = (size_t)size - sizeof(magic) - sizeof(nonce);
    uint8_t *cipher = malloc(cipher_len);
    if (!cipher) {
        fclose(fp);
        return NULL;
    }
    if (fread(cipher, 1, cipher_len, fp) != cipher_len) {
        free(cipher);
        fclose(fp);
        return NULL;
    }
    fclose(fp);

    uint8_t key[32];
    if (!sealed_store_master_key_copy(key)) {
        free(cipher);
        return NULL;
    }

    size_t plain_len = cipher_len - crypto_secretbox_MACBYTES;
    char *plain = malloc(plain_len + 1);
    if (!plain) {
        sodium_memzero(key, sizeof(key));
        free(cipher);
        return NULL;
    }

    int rc = crypto_secretbox_open_easy((uint8_t *)plain, cipher, cipher_len, nonce, key);
    sodium_memzero(key, sizeof(key));
    free(cipher);

    if (rc != 0) {
        free(plain);
        unlink(path); /* corrupt or master key rotated — drop it */
        return NULL;
    }
    plain[plain_len] = '\0';
    return plain;
#else
    return NULL;
#endif
}

static void provider_invalidate_claude_code_local_cache(void) {
    char path[1024];
    provider_build_dsco_cc_cache_path(path, sizeof(path));
    (void)unlink(path);
}

static char *provider_shell_quote(const char *s) {
    jbuf_t b;
    jbuf_init(&b, (s ? strlen(s) : 0) + 8);
    jbuf_append_char(&b, '\'');
    if (s) {
        for (const char *p = s; *p; p++) {
            if (*p == '\'')
                jbuf_append(&b, "'\"'\"'");
            else
                jbuf_append_char(&b, *p);
        }
    }
    jbuf_append_char(&b, '\'');
    return b.data;
}

static bool provider_extract_claude_code_oauth_bundle(const char *json,
                                                      claude_code_oauth_bundle_t *bundle) {
    if (!json || !bundle)
        return false;

    char *oauth = json_get_raw(json, "claudeAiOauth");
    if (!oauth)
        return false;

    char *access = json_get_str(oauth, "accessToken");
    if (!access || !access[0]) {
        free(access);
        free(oauth);
        return false;
    }

    char *refresh = json_get_str(oauth, "refreshToken");
    char *expires_raw = json_get_raw(oauth, "expiresAt");

    snprintf(bundle->access_token, sizeof(bundle->access_token), "%s", access);
    if (refresh && refresh[0])
        snprintf(bundle->refresh_token, sizeof(bundle->refresh_token), "%s", refresh);
    bundle->expires_at_ms = expires_raw ? atoll(expires_raw) : 0;
    bundle->oauth_json = oauth;
    bundle->storage_json = safe_strdup(json);

    free(access);
    free(refresh);
    free(expires_raw);
    return true;
}

static bool provider_command_read_all(const char *cmd, char *out, size_t out_len) {
    if (!cmd || !out || out_len == 0)
        return false;
    out[0] = '\0';

    FILE *fp = popen(cmd, "r");
    if (!fp)
        return false;
    size_t got = fread(out, 1, out_len - 1, fp);
    out[got] = '\0';
    int rc = pclose(fp);
    return rc == 0 && got > 0;
}

static bool provider_load_claude_code_bundle_from_keychain(claude_code_oauth_bundle_t *bundle) {
#ifdef __APPLE__
    char service[128];
    char account[128];
    provider_build_claude_code_service_name(service, sizeof(service));
    provider_get_username(account, sizeof(account));

    char *q_service = provider_shell_quote(service);
    char *q_account = provider_shell_quote(account);
    bool ok = false;
    char json[8192];

    if (q_service && q_account) {
        char cmd[512];
        snprintf(cmd, sizeof(cmd), "security find-generic-password -a %s -s %s -w 2>/dev/null",
                 q_account, q_service);
        if (provider_command_read_all(cmd, json, sizeof(json)) &&
            provider_extract_claude_code_oauth_bundle(json, bundle)) {
            bundle->source = CLAUDE_CODE_OAUTH_SOURCE_KEYCHAIN;
            snprintf(bundle->keychain_service, sizeof(bundle->keychain_service), "%s", service);
            snprintf(bundle->keychain_account, sizeof(bundle->keychain_account), "%s", account);
            ok = true;
        }
    }

    if (!ok && q_service) {
        char cmd[384];
        snprintf(cmd, sizeof(cmd), "security find-generic-password -s %s -w 2>/dev/null",
                 q_service);
        if (provider_command_read_all(cmd, json, sizeof(json)) &&
            provider_extract_claude_code_oauth_bundle(json, bundle)) {
            bundle->source = CLAUDE_CODE_OAUTH_SOURCE_KEYCHAIN;
            snprintf(bundle->keychain_service, sizeof(bundle->keychain_service), "%s", service);
            snprintf(bundle->keychain_account, sizeof(bundle->keychain_account), "%s", account);
            ok = true;
        }
    }

    free(q_service);
    free(q_account);
    return ok;
#else
    (void)bundle;
    return false;
#endif
}

static bool provider_load_claude_code_bundle_from_file(claude_code_oauth_bundle_t *bundle) {
    char creds_path[1024];
    provider_build_claude_code_credentials_path(creds_path, sizeof(creds_path));

    char *json = provider_read_text_file(creds_path);
    if (!json)
        return false;

    bool ok = provider_extract_claude_code_oauth_bundle(json, bundle);
    free(json);
    if (!ok)
        return false;

    bundle->source = CLAUDE_CODE_OAUTH_SOURCE_FILE;
    snprintf(bundle->credentials_path, sizeof(bundle->credentials_path), "%s", creds_path);
    return true;
}

static bool provider_load_claude_code_oauth_bundle(claude_code_oauth_bundle_t *bundle) {
    provider_claude_code_oauth_bundle_init(bundle);

    const char *env = getenv("DSCO_CLAUDE_CODE_OAUTH_TOKEN");
    if (env && env[0]) {
        bundle->source = CLAUDE_CODE_OAUTH_SOURCE_ENV;
        snprintf(bundle->access_token, sizeof(bundle->access_token), "%s", env);
        return true;
    }

    env = getenv("CLAUDE_CODE_OAUTH_TOKEN");
    if (env && env[0]) {
        bundle->source = CLAUDE_CODE_OAUTH_SOURCE_ENV;
        snprintf(bundle->access_token, sizeof(bundle->access_token), "%s", env);
        return true;
    }

    if (provider_env_truthy(getenv("DSCO_DISABLE_CLAUDE_CODE_OAUTH_DISCOVERY")))
        return false;

    /* Try dsco-owned local cache first — avoids prompting for the Claude Code
     * keychain entry on every run. Cache is invalidated when expired so
     * Claude Code's keychain remains the authority for refreshes. */
    {
        char *cached_json = provider_load_claude_code_bundle_local_cache();
        if (cached_json) {
            if (provider_extract_claude_code_oauth_bundle(cached_json, bundle) &&
                bundle->access_token[0] &&
                !provider_claude_code_oauth_expired(bundle->expires_at_ms)) {
                /* mark as keychain-sourced so a future refresh persists back
                 * to Claude Code's authoritative store */
                bundle->source = CLAUDE_CODE_OAUTH_SOURCE_KEYCHAIN;
                provider_build_claude_code_service_name(bundle->keychain_service,
                                                        sizeof(bundle->keychain_service));
                provider_get_username(bundle->keychain_account, sizeof(bundle->keychain_account));
                free(cached_json);
                return true;
            }
            /* extracted but stale, or extract failed — drop and re-read source */
            provider_claude_code_oauth_bundle_free(bundle);
            provider_claude_code_oauth_bundle_init(bundle);
            provider_invalidate_claude_code_local_cache();
            free(cached_json);
        }
    }

    if (provider_load_claude_code_bundle_from_keychain(bundle)) {
        if (bundle->storage_json)
            (void)provider_save_claude_code_bundle_local_cache(bundle->storage_json);
        return true;
    }
    if (provider_load_claude_code_bundle_from_file(bundle)) {
        if (bundle->storage_json)
            (void)provider_save_claude_code_bundle_local_cache(bundle->storage_json);
        return true;
    }
    provider_claude_code_oauth_bundle_free(bundle);
    return false;
}

static size_t provider_http_write_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
    jbuf_t *buf = (jbuf_t *)userdata;
    size_t n = size * nmemb;
    jbuf_append_len(buf, ptr, n);
    return n;
}

static char *provider_default_scope_json_array(void) {
    jbuf_t b;
    jbuf_init(&b, 256);
    const char *scopes[] = {"user:profile",     "user:inference",   "user:sessions:claude_code",
                            "user:mcp_servers", "user:file_upload", NULL};
    jbuf_append_char(&b, '[');
    for (int i = 0; scopes[i]; i++) {
        if (i)
            jbuf_append_char(&b, ',');
        jbuf_append_json_str(&b, scopes[i]);
    }
    jbuf_append_char(&b, ']');
    return b.data;
}

static char *provider_build_refreshed_oauth_json(const claude_code_oauth_bundle_t *bundle,
                                                 const char *scope_string) {
    char *scopes_raw = bundle->oauth_json ? json_get_raw(bundle->oauth_json, "scopes") : NULL;
    char *subscription_type =
        bundle->oauth_json ? json_get_str(bundle->oauth_json, "subscriptionType") : NULL;
    char *rate_limit_tier =
        bundle->oauth_json ? json_get_str(bundle->oauth_json, "rateLimitTier") : NULL;
    char *default_scopes = NULL;

    jbuf_t out;
    jbuf_init(&out, 512);
    jbuf_append_char(&out, '{');
    jbuf_append_json_str(&out, "accessToken");
    jbuf_append_char(&out, ':');
    jbuf_append_json_str(&out, bundle->access_token);
    jbuf_append_char(&out, ',');
    jbuf_append_json_str(&out, "refreshToken");
    jbuf_append_char(&out, ':');
    jbuf_append_json_str(&out, bundle->refresh_token);
    jbuf_append_char(&out, ',');
    jbuf_append_json_str(&out, "expiresAt");
    jbuf_appendf(&out, ":%lld", bundle->expires_at_ms);
    jbuf_append_char(&out, ',');
    jbuf_append_json_str(&out, "scopes");
    jbuf_append_char(&out, ':');
    if (scope_string && scope_string[0]) {
        jbuf_append_char(&out, '[');
        const char *cur = scope_string;
        bool first = true;
        while (*cur) {
            while (*cur == ' ')
                cur++;
            if (!*cur)
                break;
            const char *end = strchr(cur, ' ');
            if (!end)
                end = cur + strlen(cur);
            if (!first)
                jbuf_append_char(&out, ',');
            char scope[128];
            size_t n = (size_t)(end - cur);
            if (n >= sizeof(scope))
                n = sizeof(scope) - 1;
            memcpy(scope, cur, n);
            scope[n] = '\0';
            jbuf_append_json_str(&out, scope);
            first = false;
            cur = end;
        }
        jbuf_append_char(&out, ']');
    } else if (scopes_raw && scopes_raw[0]) {
        jbuf_append(&out, scopes_raw);
    } else {
        default_scopes = provider_default_scope_json_array();
        jbuf_append(&out, default_scopes);
    }
    if (subscription_type && subscription_type[0]) {
        jbuf_append_char(&out, ',');
        jbuf_append_json_str(&out, "subscriptionType");
        jbuf_append_char(&out, ':');
        jbuf_append_json_str(&out, subscription_type);
    }
    if (rate_limit_tier && rate_limit_tier[0]) {
        jbuf_append_char(&out, ',');
        jbuf_append_json_str(&out, "rateLimitTier");
        jbuf_append_char(&out, ':');
        jbuf_append_json_str(&out, rate_limit_tier);
    }
    jbuf_append_char(&out, '}');

    free(scopes_raw);
    free(subscription_type);
    free(rate_limit_tier);
    free(default_scopes);
    return out.data;
}

static bool provider_find_json_value_span(const char *json, const char *key, const char **out_start,
                                          const char **out_end) {
    if (!json || !key || !out_start || !out_end)
        return false;

    char needle[128];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = strstr(json, needle);
    if (!p)
        return false;

    p += strlen(needle);
    while (*p && isspace((unsigned char)*p))
        p++;
    if (*p != ':')
        return false;
    p++;
    while (*p && isspace((unsigned char)*p))
        p++;
    if (*p != '{')
        return false;

    const char *start = p;
    int depth = 0;
    bool in_string = false;
    bool escaped = false;
    for (; *p; p++) {
        char ch = *p;
        if (in_string) {
            if (escaped)
                escaped = false;
            else if (ch == '\\')
                escaped = true;
            else if (ch == '"')
                in_string = false;
            continue;
        }
        if (ch == '"') {
            in_string = true;
            continue;
        }
        if (ch == '{')
            depth++;
        else if (ch == '}') {
            depth--;
            if (depth == 0) {
                *out_start = start;
                *out_end = p + 1;
                return true;
            }
        }
    }

    return false;
}

static char *provider_replace_claude_code_oauth_json(const claude_code_oauth_bundle_t *bundle,
                                                     const char *new_oauth_json) {
    if (!new_oauth_json)
        return NULL;
    if (!bundle->storage_json || !bundle->storage_json[0] || !strchr(bundle->storage_json, '{')) {
        jbuf_t out;
        jbuf_init(&out, 256);
        jbuf_append(&out, "{\"claudeAiOauth\":");
        jbuf_append(&out, new_oauth_json);
        jbuf_append_char(&out, '}');
        return out.data;
    }

    const char *value_start = NULL;
    const char *value_end = NULL;
    if (!provider_find_json_value_span(bundle->storage_json, "claudeAiOauth", &value_start,
                                       &value_end)) {
        jbuf_t out;
        jbuf_init(&out, strlen(bundle->storage_json) + strlen(new_oauth_json) + 32);
        size_t len = strlen(bundle->storage_json);
        if (len > 0 && bundle->storage_json[len - 1] == '}') {
            jbuf_append_len(&out, bundle->storage_json, len - 1);
            if (len > 2)
                jbuf_append_char(&out, ',');
            jbuf_append_json_str(&out, "claudeAiOauth");
            jbuf_append_char(&out, ':');
            jbuf_append(&out, new_oauth_json);
            jbuf_append_char(&out, '}');
            return out.data;
        }
        jbuf_free(&out);
        return safe_strdup(bundle->storage_json);
    }

    size_t prefix_len = (size_t)(value_start - bundle->storage_json);
    size_t suffix_len = strlen(value_end);
    size_t new_json_len = strlen(new_oauth_json);
    size_t out_sz = prefix_len + new_json_len + suffix_len + 1;
    char *out = safe_malloc(out_sz);
    memcpy(out, bundle->storage_json, prefix_len);
    snprintf(out + prefix_len, out_sz - prefix_len, "%s", new_oauth_json);
    snprintf(out + prefix_len + new_json_len, out_sz - prefix_len - new_json_len, "%s", value_end);
    return out;
}

static bool provider_write_claude_code_bundle_file(const char *path, const char *json) {
    if (!path || !path[0] || !json)
        return false;
    FILE *fp = fopen(path, "wb");
    if (!fp)
        return false;
    size_t len = strlen(json);
    bool ok = fwrite(json, 1, len, fp) == len;
    fclose(fp);
    return ok;
}

static bool provider_write_claude_code_bundle_keychain(const claude_code_oauth_bundle_t *bundle,
                                                       const char *json) {
#ifdef __APPLE__
    if (!bundle || !json || !bundle->keychain_service[0])
        return false;
    char *q_service = provider_shell_quote(bundle->keychain_service);
    char *q_account = provider_shell_quote(bundle->keychain_account[0] ? bundle->keychain_account
                                                                       : "claude-code-user");
    char *q_json = provider_shell_quote(json);
    bool ok = false;
    if (q_service && q_account && q_json) {
        jbuf_t cmd;
        jbuf_init(&cmd, strlen(json) + 256);
        jbuf_append(&cmd, "security add-generic-password -U -a ");
        jbuf_append(&cmd, q_account);
        jbuf_append(&cmd, " -s ");
        jbuf_append(&cmd, q_service);
        jbuf_append(&cmd, " -w ");
        jbuf_append(&cmd, q_json);
        jbuf_append(&cmd, " >/dev/null 2>&1");
        ok = system(cmd.data) == 0;
        jbuf_free(&cmd);
    }
    free(q_service);
    free(q_account);
    free(q_json);
    return ok;
#else
    (void)bundle;
    (void)json;
    return false;
#endif
}

static bool provider_persist_claude_code_bundle(const claude_code_oauth_bundle_t *bundle,
                                                const char *scope_string) {
    if (!bundle || bundle->source == CLAUDE_CODE_OAUTH_SOURCE_ENV)
        return true;

    char *oauth_json = provider_build_refreshed_oauth_json(bundle, scope_string);
    char *storage_json = provider_replace_claude_code_oauth_json(bundle, oauth_json);
    free(oauth_json);
    if (!storage_json)
        return false;

    bool ok = false;
    if (bundle->source == CLAUDE_CODE_OAUTH_SOURCE_KEYCHAIN) {
        ok = provider_write_claude_code_bundle_keychain(bundle, storage_json);
    } else if (bundle->source == CLAUDE_CODE_OAUTH_SOURCE_FILE) {
        ok = provider_write_claude_code_bundle_file(bundle->credentials_path, storage_json);
    }

    /* Always refresh the local cache with the new bundle so subsequent dsco
     * invocations pick up the refreshed access_token without touching the
     * authoritative store again. */
    (void)provider_save_claude_code_bundle_local_cache(storage_json);

    free(storage_json);
    return ok;
}

static bool provider_refresh_claude_code_oauth_bundle(claude_code_oauth_bundle_t *bundle) {
    if (!bundle || !bundle->refresh_token[0])
        return false;

    const char *token_url = getenv("DSCO_CLAUDE_CODE_OAUTH_TOKEN_URL");
    if (!token_url || !token_url[0])
        token_url = CLAUDE_CODE_OAUTH_TOKEN_URL;
    const char *client_id = getenv("DSCO_CLAUDE_CODE_OAUTH_CLIENT_ID");
    if (!client_id || !client_id[0])
        client_id = CLAUDE_CODE_OAUTH_CLIENT_ID;
    const char *scopes = getenv("DSCO_CLAUDE_CODE_OAUTH_SCOPES");
    if (!scopes || !scopes[0])
        scopes = CLAUDE_CODE_OAUTH_SCOPES;

    curl_global_init(CURL_GLOBAL_DEFAULT);
    CURL *curl = curl_easy_init();
    dsco_http_pool_apply(curl);
    if (!curl)
        return false;

    jbuf_t req;
    jbuf_init(&req, 512);
    jbuf_append(&req, "{\"grant_type\":\"refresh_token\",\"refresh_token\":");
    jbuf_append_json_str(&req, bundle->refresh_token);
    jbuf_append(&req, ",\"client_id\":");
    jbuf_append_json_str(&req, client_id);
    jbuf_append(&req, ",\"scope\":");
    jbuf_append_json_str(&req, scopes);
    jbuf_append_char(&req, '}');

    jbuf_t resp;
    jbuf_init(&resp, 1024);
    struct curl_slist *hdrs = NULL;
    hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_URL, token_url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req.data);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, provider_http_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2TLS);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);
    jbuf_free(&req);

    if (res != CURLE_OK || http_code != 200) {
        jbuf_free(&resp);
        return false;
    }

    char *access_token = json_get_str(resp.data, "access_token");
    char *refresh_token = json_get_str(resp.data, "refresh_token");
    char *scope_string = json_get_str(resp.data, "scope");
    int expires_in = json_get_int(resp.data, "expires_in", -1);

    bool ok = access_token && access_token[0] && expires_in > 0;
    if (ok) {
        snprintf(bundle->access_token, sizeof(bundle->access_token), "%s", access_token);
        if (refresh_token && refresh_token[0])
            snprintf(bundle->refresh_token, sizeof(bundle->refresh_token), "%s", refresh_token);
        bundle->expires_at_ms = provider_now_ms() + (long long)expires_in * 1000LL;
        (void)provider_persist_claude_code_bundle(bundle, scope_string);
    }

    free(access_token);
    free(refresh_token);
    free(scope_string);
    jbuf_free(&resp);
    return ok;
}

static const char *provider_resolve_claude_code_oauth_token(bool allow_refresh) {
    static char token[4096];
    token[0] = '\0';

    claude_code_oauth_bundle_t bundle;
    if (!provider_load_claude_code_oauth_bundle(&bundle))
        return NULL;

    if (allow_refresh && bundle.source != CLAUDE_CODE_OAUTH_SOURCE_ENV && bundle.refresh_token[0] &&
        provider_claude_code_oauth_expired(bundle.expires_at_ms)) {
        (void)provider_refresh_claude_code_oauth_bundle(&bundle);
    }

    if (!bundle.access_token[0]) {
        provider_claude_code_oauth_bundle_free(&bundle);
        return NULL;
    }

    snprintf(token, sizeof(token), "%s", bundle.access_token);
    provider_claude_code_oauth_bundle_free(&bundle);
    return token;
}

/* Builds an OpenAI-compat request then injects OpenRouter-specific fields.
 *
 * Env vars (all optional):
 *   DSCO_OR_TRANSFORMS        — e.g. "middle-out"
 *   DSCO_OR_ROUTE             — e.g. "fallback"
 *   DSCO_OR_PROVIDER_ORDER    — comma-sep provider slugs
 *   DSCO_OR_PROVIDER_ONLY     — comma-sep allowlist
 *   DSCO_OR_PROVIDER_IGNORE   — comma-sep blocklist
 *   DSCO_OR_REQUIRE_PARAMS    — "1"/"true"
 *   DSCO_OR_ALLOW_FALLBACKS   — "0"/"false" to disable (default: true)
 *   DSCO_OR_DATA_COLLECTION   — "deny" to disable
 *   DSCO_OR_ZDR               — "1"/"true" for zero data retention
 *   DSCO_OR_QUANTIZATIONS     — comma-sep: int4,int8,fp6,fp8,fp16,bf16,fp32
 *   DSCO_OR_SORT              — "price", "throughput", or "latency"
 *   DSCO_OR_MAX_PRICE_INPUT   — max $/token for input (e.g. "0.00001")
 *   DSCO_OR_MAX_PRICE_OUTPUT  — max $/token for output
 *   DSCO_OR_FALLBACK_MODELS   — comma-sep model IDs for automatic failover
 *   DSCO_OR_REASONING_EFFORT  — "low", "medium", "high" for reasoning models
 *   DSCO_OR_DEBUG              — "1"/"true" to echo upstream request body
 */
static char *openrouter_build_request(provider_t *p, conversation_t *conv, session_state_t *session,
                                      int max_tokens, const char *credential) {
    char *base = openai_build_request(p, conv, session, max_tokens, credential);
    if (!base)
        return NULL;

    /* Gather all env config */
    const char *transforms = getenv("DSCO_OR_TRANSFORMS");
    const char *route = getenv("DSCO_OR_ROUTE");
    const char *prov_order = getenv("DSCO_OR_PROVIDER_ORDER");
    const char *prov_only = getenv("DSCO_OR_PROVIDER_ONLY");
    const char *prov_ignore = getenv("DSCO_OR_PROVIDER_IGNORE");
    const char *req_params = getenv("DSCO_OR_REQUIRE_PARAMS");
    const char *allow_fb = getenv("DSCO_OR_ALLOW_FALLBACKS");
    const char *data_collect = getenv("DSCO_OR_DATA_COLLECTION");
    const char *zdr = getenv("DSCO_OR_ZDR");
    const char *quantizations = getenv("DSCO_OR_QUANTIZATIONS");
    const char *sort_by = getenv("DSCO_OR_SORT");
    const char *max_price_in = getenv("DSCO_OR_MAX_PRICE_INPUT");
    const char *max_price_out = getenv("DSCO_OR_MAX_PRICE_OUTPUT");
    const char *fallback_models = getenv("DSCO_OR_FALLBACK_MODELS");
    const char *reasoning = getenv("DSCO_OR_REASONING_EFFORT");
    const char *debug_mode = getenv("DSCO_OR_DEBUG");
    /* thinking disabled by omission; explicit type=disabled rejected by some models */

    bool has_provider = prov_order || prov_only || prov_ignore || req_params ||
                        (allow_fb && !or_env_bool(allow_fb)) || data_collect || zdr ||
                        quantizations || sort_by || max_price_in || max_price_out;
    bool has_extras =
        transforms || route || has_provider || fallback_models || reasoning || debug_mode;

    if (!has_extras)
        return base;

    /* Strip trailing '}' to append fields */
    size_t len = strlen(base);
    if (len == 0 || base[len - 1] != '}')
        return base;
    base[len - 1] = '\0';

    jbuf_t b;
    jbuf_init(&b, len + 1024);
    jbuf_append(&b, base);
    free(base);

    /* transforms: ["middle-out"] */
    if (transforms) {
        jbuf_append(&b, ",\"transforms\":");
        or_append_csv_array(&b, transforms);
    }

    /* route: "fallback" */
    if (route) {
        jbuf_append(&b, ",\"route\":");
        jbuf_append_json_str(&b, route);
    }

    /* models: ["model/a", "model/b"] — automatic failover */
    if (fallback_models) {
        jbuf_append(&b, ",\"models\":");
        or_append_csv_array(&b, fallback_models);
    }

    /* reasoning: {"effort": "high"} */
    if (reasoning) {
        jbuf_append(&b, ",\"reasoning\":{\"effort\":");
        jbuf_append_json_str(&b, reasoning);
        jbuf_append(&b, "}");
    }

    /* Never emit type=disabled — some models (e.g. kimi-k2.7-code) only
       accept type=enabled or omission. If the user wants thinking, they set
       DSCO_OR_REASONING_EFFORT or pick a *-thinking / *-think model.
       Otherwise we simply omit the field (thinking disabled by default). */
    /* debug: {"echo_upstream_body": true} */
    if (debug_mode && or_env_bool(debug_mode)) {
        jbuf_append(&b, ",\"debug\":{\"echo_upstream_body\":true}");
    }

    /* provider: { ... } */
    if (has_provider) {
        jbuf_append(&b, ",\"provider\":{");
        bool wrote = false;

        if (prov_order) {
            jbuf_append(&b, "\"order\":");
            or_append_csv_array(&b, prov_order);
            wrote = true;
        }
        if (prov_only) {
            if (wrote)
                jbuf_append(&b, ",");
            jbuf_append(&b, "\"only\":");
            or_append_csv_array(&b, prov_only);
            wrote = true;
        }
        if (prov_ignore) {
            if (wrote)
                jbuf_append(&b, ",");
            jbuf_append(&b, "\"ignore\":");
            or_append_csv_array(&b, prov_ignore);
            wrote = true;
        }
        if (req_params && or_env_bool(req_params)) {
            if (wrote)
                jbuf_append(&b, ",");
            jbuf_append(&b, "\"require_parameters\":true");
            wrote = true;
        }
        if (allow_fb && !or_env_bool(allow_fb)) {
            if (wrote)
                jbuf_append(&b, ",");
            jbuf_append(&b, "\"allow_fallbacks\":false");
            wrote = true;
        }
        if (data_collect) {
            if (wrote)
                jbuf_append(&b, ",");
            jbuf_append(&b, "\"data_collection\":");
            jbuf_append_json_str(&b, data_collect);
            wrote = true;
        }
        if (zdr && or_env_bool(zdr)) {
            if (wrote)
                jbuf_append(&b, ",");
            jbuf_append(&b, "\"zdr\":true");
            wrote = true;
        }
        if (quantizations) {
            if (wrote)
                jbuf_append(&b, ",");
            jbuf_append(&b, "\"quantizations\":");
            or_append_csv_array(&b, quantizations);
            wrote = true;
        }
        if (sort_by) {
            if (wrote)
                jbuf_append(&b, ",");
            jbuf_append(&b, "\"sort\":");
            jbuf_append_json_str(&b, sort_by);
            wrote = true;
        }
        if (max_price_in || max_price_out) {
            if (wrote)
                jbuf_append(&b, ",");
            jbuf_append(&b, "\"max_price\":{");
            bool mp_wrote = false;
            if (max_price_in) {
                jbuf_appendf(&b, "\"input\":%s", max_price_in);
                mp_wrote = true;
            }
            if (max_price_out) {
                if (mp_wrote)
                    jbuf_append(&b, ",");
                jbuf_appendf(&b, "\"output\":%s", max_price_out);
            }
            jbuf_append(&b, "}");
            wrote = true;
        }
        (void)wrote;
        jbuf_append(&b, "}");
    }

    jbuf_append(&b, "}");
    return b.data;
}

/* ── Moonshot (native Kimi API) ────────────────────────────────────────── */

/* Native Moonshot routing. Speaks the OpenAI Chat Completions dialect, but
 * adds Anthropic-style {"thinking": {"type": "enabled"|"disabled"}} so we
 * can toggle Kimi K2.5 reasoning. Per the Kimi K2.7 Code docs, thinking must
 * always be enabled (the model throws an error if disabled), and sampling
 * parameters are fixed: temperature=1.0, top_p=0.95, n=1, penalties=0.0.
 * These are now injected in openai_build_request for all moonshot-compatible
 * models, so moonshot_build_request just needs to strip the type=disabled
 * thinking field that openai_build_request might emit. */
static char *moonshot_build_request(provider_t *p, conversation_t *conv, session_state_t *session,
                                    int max_tokens, const char *credential) {
    char *base = openai_build_request(p, conv, session, max_tokens, credential);
    if (!base)
        return NULL;

    /* openai_build_request() already emits Moonshot-safe payloads: Kimi-compatible
     * models get thinking enabled plus the fixed sampling parameters required by
     * K2.7 Code. Do not mutate the JSON here. A previous version chopped the
     * final closing brace while trying to strip a disabled-thinking field, which
     * made Moonshot return HTTP 400 "unexpected EOF" for every native Kimi call. */
    return base;
}

static bool model_is_moonshot_compatible(const char *model) {
    if (!model || !model[0])
        return false;
    return strstr(model, "kimi") != NULL || strstr(model, "moonshot") != NULL;
}

/* ── xAI (native api.x.ai) ─────────────────────────────────────────────
 *
 * xAI speaks the OpenAI Chat Completions dialect and adds two extras:
 *   - reasoning_effort: "low"|"high" for grok-3-mini and grok-4 family
 *   - search_parameters: { mode, sources[], from_date, to_date, ... }
 *     for Live Search (live web/news/x search)
 *
 * Env vars (all optional):
 *   DSCO_XAI_REASONING_EFFORT  — "low" | "high"
 *   DSCO_XAI_SEARCH_MODE       — "off" (default) | "auto" | "on"
 *   DSCO_XAI_SEARCH_SOURCES    — comma-sep: web,news,x,rss
 *   DSCO_XAI_SEARCH_FROM_DATE  — ISO date YYYY-MM-DD
 *   DSCO_XAI_SEARCH_TO_DATE    — ISO date YYYY-MM-DD
 *   DSCO_XAI_SEARCH_MAX_RESULTS — int
 *   DSCO_XAI_RETURN_CITATIONS  — "1"/"true" to request citations array
 */
static bool xai_supports_reasoning(const char *model) {
    if (!model || !model[0])
        return false;
    if (strstr(model, "grok-3-mini"))
        return true;
    if (strstr(model, "grok-4"))
        return true;
    return false;
}

static char *xai_build_request(provider_t *p, conversation_t *conv, session_state_t *session,
                               int max_tokens, const char *credential) {
    char *base = openai_build_request(p, conv, session, max_tokens, credential);
    if (!base)
        return NULL;

    const char *reasoning = getenv("DSCO_XAI_REASONING_EFFORT");
    const char *search_mode = getenv("DSCO_XAI_SEARCH_MODE");
    const char *src = getenv("DSCO_XAI_SEARCH_SOURCES");
    const char *from_date = getenv("DSCO_XAI_SEARCH_FROM_DATE");
    const char *to_date = getenv("DSCO_XAI_SEARCH_TO_DATE");
    const char *max_results = getenv("DSCO_XAI_SEARCH_MAX_RESULTS");
    const char *return_cite = getenv("DSCO_XAI_RETURN_CITATIONS");

    bool want_reasoning =
        reasoning && reasoning[0] && xai_supports_reasoning(session ? session->model : NULL);
    bool want_search = search_mode && search_mode[0] && strcasecmp(search_mode, "off") != 0;

    if (!want_reasoning && !want_search)
        return base;

    size_t len = strlen(base);
    if (len == 0 || base[len - 1] != '}')
        return base;
    base[len - 1] = '\0';

    jbuf_t b;
    jbuf_init(&b, len + 512);
    jbuf_append(&b, base);
    free(base);

    if (want_reasoning) {
        jbuf_append(&b, ",\"reasoning_effort\":");
        jbuf_append_json_str(&b, reasoning);
    }

    if (want_search) {
        jbuf_append(&b, ",\"search_parameters\":{\"mode\":");
        jbuf_append_json_str(&b, search_mode);
        if (src && src[0]) {
            jbuf_append(&b, ",\"sources\":[");
            const char *cur = src;
            bool first = true;
            while (*cur) {
                const char *end = strchr(cur, ',');
                if (!end)
                    end = cur + strlen(cur);
                size_t n = (size_t)(end - cur);
                char name[64];
                if (n >= sizeof(name))
                    n = sizeof(name) - 1;
                memcpy(name, cur, n);
                name[n] = '\0';
                char *s = name;
                while (*s == ' ')
                    s++;
                char *e2 = s + strlen(s) - 1;
                while (e2 > s && *e2 == ' ')
                    *e2-- = '\0';
                if (s[0]) {
                    if (!first)
                        jbuf_append(&b, ",");
                    jbuf_append(&b, "{\"type\":");
                    jbuf_append_json_str(&b, s);
                    jbuf_append(&b, "}");
                    first = false;
                }
                cur = *end ? end + 1 : end;
            }
            jbuf_append(&b, "]");
        }
        if (from_date && from_date[0]) {
            jbuf_append(&b, ",\"from_date\":");
            jbuf_append_json_str(&b, from_date);
        }
        if (to_date && to_date[0]) {
            jbuf_append(&b, ",\"to_date\":");
            jbuf_append_json_str(&b, to_date);
        }
        if (max_results && max_results[0]) {
            jbuf_appendf(&b, ",\"max_search_results\":%s", max_results);
        }
        if (return_cite && or_env_bool(return_cite)) {
            jbuf_append(&b, ",\"return_citations\":true");
        }
        jbuf_append(&b, "}");
    }

    jbuf_append(&b, "}");
    return b.data;
}

/* ── OpenAI-compatible Provider ────────────────────────────────────────── */

typedef struct {
    char api_url[512];
    CURL *curl;
    bool prepared;
} openai_data_t;

/* Check if a message has any tool_use content blocks */
static bool msg_has_tool_use(message_t *m) {
    for (int j = 0; j < m->content_count; j++) {
        if (m->content[j].type && strcmp(m->content[j].type, "tool_use") == 0)
            return true;
    }
    return false;
}

/* Check if a message has any tool_result content blocks */
static bool msg_has_tool_result(message_t *m) {
    for (int j = 0; j < m->content_count; j++) {
        if (m->content[j].type && strcmp(m->content[j].type, "tool_result") == 0)
            return true;
    }
    return false;
}

static const char *openai_last_user_context(conversation_t *conv) {
    if (!conv)
        return NULL;
    for (int i = conv->count - 1; i >= 0; i--) {
        if (conv->msgs[i].role != ROLE_USER)
            continue;
        for (int j = 0; j < conv->msgs[i].content_count; j++) {
            if (conv->msgs[i].content[j].text) {
                return conv->msgs[i].content[j].text;
            }
        }
    }
    return NULL;
}

static void openai_append_function_tool(jbuf_t *b, const char *name, const char *description,
                                        const char *schema_json, bool cache_mark) {
    jbuf_append(b, "{\"type\":\"function\",\"function\":{\"name\":");
    jbuf_append_json_str(b, name ? name : "");
    jbuf_append(b, ",\"description\":");
    jbuf_append_json_str(b, description ? description : "");
    jbuf_append(b, ",\"parameters\":");
    jbuf_append(b, schema_json ? schema_json : "{}");
    /* cache_control goes at the tool wrapper level, outside function{} */
    if (cache_mark)
        jbuf_append(b, "},\"cache_control\":{\"type\":\"ephemeral\"}}");
    else
        jbuf_append(b, "}}");
}

static bool openai_tools_disabled(void) {
    const char *disable_tools = getenv("DSCO_OR_DISABLE_TOOLS");
    if (disable_tools && disable_tools[0] && strcmp(disable_tools, "0") != 0 &&
        strcasecmp(disable_tools, "false") != 0) {
        return true;
    }
    return false;
}

static bool openai_append_tools_json(jbuf_t *b, conversation_t *conv, session_state_t *session) {
    if (openai_tools_disabled())
        return false;

    int max_tools_send = 128;
    const char *mt_env = getenv("DSCO_OR_MAX_TOOLS");
    if (mt_env && mt_env[0]) {
        max_tools_send = atoi(mt_env);
        if (max_tools_send < 0)
            max_tools_send = 0;
    } else if (g_cheap_mode) {
        max_tools_send = TOOL_REG_ALWAYS;
    } else {
        const char *model = session ? session->model : "";
        if (model && strstr(model, "/"))
            max_tools_send = 48; /* OpenRouter: tighter cap */
    }

    int filtered_count = 0;
    const tool_def_t **filtered = NULL;
    if (max_tools_send > 0) {
        filtered =
            tools_get_filtered(openai_last_user_context(conv), max_tools_send, &filtered_count);
    }

    if (filtered_count <= 0 && g_external_tool_count <= 0) {
        free((void *)filtered);
        return false;
    }

    /* Gate cache markers: only Anthropic claude-* models via OpenRouter
     * understand cache_control in the OpenAI wire format. */
    bool want_cache = provider_model_supports_cache_control(session ? session->model : NULL);

    /* Pre-count total tools to identify the last one for cache marking. */
    int loaded_ext_pre = 0;
    for (int i = 0; i < g_external_tool_count; i++)
        if (g_external_tools[i].loaded)
            loaded_ext_pre++;
    int ext_budget_pre = loaded_ext_pre > 0 ? loaded_ext_pre : 16;
    if (ext_budget_pre > 32)
        ext_budget_pre = 32;
    int ext_total_pre =
        ext_budget_pre < g_external_tool_count ? ext_budget_pre : g_external_tool_count;
    int total_tools = filtered_count + ext_total_pre;

    jbuf_append(b, ",\"tools\":[");
    bool wrote_any = false;
    int emitted = 0;
    for (int i = 0; i < filtered_count; i++) {
        if (wrote_any)
            jbuf_append(b, ",");
        bool is_last = want_cache && (emitted == total_tools - 1);
        openai_append_function_tool(b, filtered[i]->name, filtered[i]->description,
                                    filtered[i]->input_schema_json, is_last);
        wrote_any = true;
        emitted++;
    }
    free((void *)filtered);

    int loaded_ext_count = 0;
    for (int i = 0; i < g_external_tool_count; i++)
        if (g_external_tools[i].loaded)
            loaded_ext_count++;
    int ext_budget = loaded_ext_count > 0 ? loaded_ext_count : 16;
    if (ext_budget > 32)
        ext_budget = 32;
    int ext_written = 0;
    for (int pass = 0; pass < 2 && ext_written < ext_budget; pass++) {
        bool want_loaded = (pass == 0);
        for (int i = 0; i < g_external_tool_count && ext_written < ext_budget; i++) {
            if ((bool)g_external_tools[i].loaded != want_loaded)
                continue;
            if (wrote_any)
                jbuf_append(b, ",");
            bool is_last = want_cache && (emitted == total_tools - 1);
            openai_append_function_tool(b, g_external_tools[i].name,
                                        g_external_tools[i].description,
                                        g_external_tools[i].input_schema_json, is_last);
            wrote_any = true;
            ext_written++;
            emitted++;
        }
    }
    jbuf_append(b, "]");
    return wrote_any;
}

static void openai_append_tool_choice_json(jbuf_t *b, session_state_t *session, bool has_tools) {
    if (!has_tools)
        return;

    const char *choice = (session && session->tool_choice[0]) ? session->tool_choice : "auto";
    if (strcmp(choice, "auto") == 0) {
        jbuf_append(b, ",\"tool_choice\":\"auto\"");
    } else if (strcmp(choice, "any") == 0) {
        jbuf_append(b, ",\"tool_choice\":\"required\"");
    } else if (strcmp(choice, "none") == 0) {
        jbuf_append(b, ",\"tool_choice\":\"none\"");
    } else if (strncmp(choice, "tool:", 5) == 0) {
        jbuf_append(b, ",\"tool_choice\":{\"type\":\"function\",\"function\":{\"name\":");
        jbuf_append_json_str(b, choice + 5);
        jbuf_append(b, "}}");
    }
}

/* openrouter_should_disable_thinking removed — type=disabled is rejected by some models */

/* Native Moonshot routing: kimi-k2.5 is multimodal and defaults to thinking
 * enabled. Tool-calling reliability improves substantially with thinking
 * disabled unless the user explicitly opts in via thinking_budget or picks
 * a *-thinking model. */
/* moonshot_should_disable_thinking excised */

/* Emit text+image content array (skipping tool_use and tool_result blocks) */
static void openai_append_text_content(jbuf_t *b, message_t *m) {
    jbuf_t text;
    jbuf_init(&text, 1024);

    for (int j = 0; j < m->content_count; j++) {
        msg_content_t *mc = &m->content[j];
        if (mc->type && strcmp(mc->type, "text") == 0 && mc->text) {
            if (text.len > 0)
                jbuf_append(&text, "\n");
            jbuf_append(&text, mc->text);
        }
    }

    if (text.len > 0) {
        jbuf_append_json_str(b, text.data);
    } else {
        jbuf_append(b, "\"\"");
    }
    jbuf_free(&text);
}

/* Emit an assistant message with tool_calls in OpenAI format */
static void openai_append_assistant_msg(jbuf_t *b, message_t *m) {
    jbuf_append(b, ",{\"role\":\"assistant\"");

    /* Collect text content and provider-specific reasoning replay content.
     * Moonshot/Kimi multi-step tool calling requires preserving prior
     * reasoning_content in conversation context. We store it internally as a
     * content block of type="thinking" and replay it on resend only for
     * OpenAI-compatible providers that understand reasoning_content. */
    jbuf_t text;
    jbuf_t reasoning;
    jbuf_init(&text, 1024);
    jbuf_init(&reasoning, 1024);
    for (int j = 0; j < m->content_count; j++) {
        msg_content_t *mc = &m->content[j];
        if (mc->type && strcmp(mc->type, "text") == 0 && mc->text) {
            if (text.len > 0)
                jbuf_append(&text, "\n");
            jbuf_append(&text, mc->text);
        } else if (mc->type && strcmp(mc->type, "thinking") == 0 && mc->text) {
            if (reasoning.len > 0)
                jbuf_append(&reasoning, "\n");
            jbuf_append(&reasoning, mc->text);
        }
    }
    if (text.len > 0) {
        jbuf_append(b, ",\"content\":");
        jbuf_append_json_str(b, text.data);
    }
    if (reasoning.len > 0) {
        jbuf_append(b, ",\"reasoning_content\":");
        jbuf_append_json_str(b, reasoning.data);
    }
    jbuf_free(&text);
    jbuf_free(&reasoning);

    /* Emit tool_calls array for tool_use blocks */
    if (msg_has_tool_use(m)) {
        jbuf_append(b, ",\"tool_calls\":[");
        bool first_tool = true;
        for (int j = 0; j < m->content_count; j++) {
            msg_content_t *mc = &m->content[j];
            if (!mc->type || strcmp(mc->type, "tool_use") != 0)
                continue;
            if (!first_tool)
                jbuf_append(b, ",");
            first_tool = false;
            jbuf_append(b, "{\"id\":");
            jbuf_append_json_str(b, mc->tool_id ? mc->tool_id : "call_0");
            jbuf_append(b, ",\"type\":\"function\",\"function\":{\"name\":");
            jbuf_append_json_str(b, mc->tool_name ? mc->tool_name : "unknown");
            jbuf_append(b, ",\"arguments\":");
            /* OpenAI/OpenRouter require arguments as a JSON *string*, not object */
            jbuf_append_json_str(b, mc->tool_input ? mc->tool_input : "{}");
            jbuf_append(b, "}}");
        }
        jbuf_append(b, "]");
    }

    jbuf_append(b, "}");
}

/* Emit tool_result blocks as separate {"role":"tool"} messages (OpenAI format) */
static void openai_append_tool_results(jbuf_t *b, message_t *m) {
    for (int j = 0; j < m->content_count; j++) {
        msg_content_t *mc = &m->content[j];
        if (!mc->type || strcmp(mc->type, "tool_result") != 0)
            continue;
        jbuf_append(b, ",{\"role\":\"tool\",\"tool_call_id\":");
        jbuf_append_json_str(b, mc->tool_id ? mc->tool_id : "call_0");
        jbuf_append(b, ",\"content\":");
        jbuf_append_json_str(b, mc->text ? mc->text : "");
        jbuf_append(b, "}");
    }
}

/* Emit a regular user message (text + images, no tool_results) */
static void openai_append_user_msg(jbuf_t *b, message_t *m) {
    jbuf_append(b, ",{\"role\":\"user\",\"content\":");

    /* Check if we have images */
    bool has_images = false;
    for (int j = 0; j < m->content_count; j++) {
        if (m->content[j].type && strcmp(m->content[j].type, "image") == 0) {
            has_images = true;
            break;
        }
    }

    if (!has_images) {
        /* Simple string content */
        openai_append_text_content(b, m);
    } else {
        /* Array content with text + images */
        jbuf_append(b, "[");
        bool wrote_any = false;

        /* Text block */
        jbuf_t text;
        jbuf_init(&text, 1024);
        for (int j = 0; j < m->content_count; j++) {
            msg_content_t *mc = &m->content[j];
            if (mc->type && strcmp(mc->type, "text") == 0 && mc->text) {
                if (text.len > 0)
                    jbuf_append(&text, "\n");
                jbuf_append(&text, mc->text);
            }
        }
        if (text.len > 0) {
            jbuf_append(b, "{\"type\":\"text\",\"text\":");
            jbuf_append_json_str(b, text.data);
            jbuf_append(b, "}");
            wrote_any = true;
        }
        jbuf_free(&text);

        /* Image blocks — skip any image with neither URL nor real base64.
         * Emitting an empty data-url breaks every provider downstream and
         * kills the fallback chain (grok-4-fast, gemini, gpt-5.4 all reject
         * "data:image/png;base64,"). Silently drop; the text block above
         * already carries the user's prose and any other image blocks in
         * this same turn will still serialize. */
        for (int j = 0; j < m->content_count; j++) {
            msg_content_t *mc = &m->content[j];
            if (!mc->type || strcmp(mc->type, "image") != 0)
                continue;
            bool has_url = (mc->image_url && mc->image_url[0]);
            bool has_data = (mc->image_data && mc->image_data[0]);
            if (!has_url && !has_data)
                continue;
            if (wrote_any)
                jbuf_append(b, ",");
            jbuf_append(b, "{\"type\":\"image_url\",\"image_url\":{\"url\":");
            if (has_url) {
                jbuf_append_json_str(b, mc->image_url);
            } else {
                const char *media_type = mc->image_media_type ? mc->image_media_type : "image/png";
                jbuf_append(b, "\"data:");
                jbuf_append(b, media_type);
                jbuf_append(b, ";base64,");
                jbuf_append(b, mc->image_data);
                jbuf_append(b, "\"");
            }
            jbuf_append(b, "}}");
            wrote_any = true;
        }

        if (!wrote_any) {
            jbuf_append(b, "{\"type\":\"text\",\"text\":\"\"}");
        }
        jbuf_append(b, "]");
    }

    jbuf_append(b, "}");
}

/* OpenRouter forwards Anthropic-style `cache_control` breakpoints to Claude
 * models. In an OpenAI-format request a breakpoint on the SYSTEM message caches
 * the entire static prefix — in Anthropic's canonical order (tools → system →
 * messages) the system block sits after the tools, so caching up to it covers
 * both. Only Claude models honor the marker; other providers reached through
 * this same OpenAI-compat path (xAI, DeepSeek, Mistral, Moonshot, Gemini) would
 * ignore or reject it, so gate strictly on the model id. Override with
 * DSCO_OR_CACHE=0 to disable, or =1 to force on for any model. */
static const char *provider_model_strip_explicit_openrouter_prefix(const char *model);

static bool provider_model_has_any_prefix(const char *m, const char *const *prefixes) {
    if (!m || !prefixes)
        return false;
    for (int i = 0; prefixes[i]; i++)
        if (strncmp(m, prefixes[i], strlen(prefixes[i])) == 0)
            return true;
    return false;
}

static bool provider_model_contains_any(const char *m, const char *const *needles) {
    if (!m || !needles)
        return false;
    for (int i = 0; needles[i]; i++)
        if (strstr(m, needles[i]))
            return true;
    return false;
}

bool provider_model_supports_cache_control(const char *model) {
    const char *force = getenv("DSCO_OR_CACHE");
    if (force && force[0] == '0')
        return false;
    if (force && force[0] == '1')
        return true;
    if (!model)
        return false;
    /* Explicit cache breakpoints: Anthropic/Claude and Qwen-compatible routes. */
    if (strstr(model, "claude") || strstr(model, "anthropic/"))
        return true;
    if (strstr(model, "qwen/") || strstr(model, "qwen3") || strstr(model, "qwen-"))
        return true;
    return false;
}

bool provider_model_supports_prompt_cache_retention(const char *model) {
    if (!model)
        return false;
    const char *m = provider_model_strip_explicit_openrouter_prefix(model);
    static const char *const openai_prefixes[] = {
        "gpt-", "o1", "o3", "o4", "chatgpt-", "openai/gpt-", "openai/o", NULL
    };
    static const char *const azure_prefixes[] = {"azure/", "azure-foundry/", "microsoft/", NULL};
    return provider_model_has_any_prefix(m, openai_prefixes) || provider_model_has_any_prefix(m, azure_prefixes);
}

bool provider_model_supports_prompt_cache_key(const char *model) {
    if (!model)
        return false;
    const char *m = provider_model_strip_explicit_openrouter_prefix(model);
    static const char *const key_prefixes[] = {
        "gpt-", "o1", "o3", "o4", "chatgpt-", "openai/gpt-", "openai/o",
        "azure/", "azure-foundry/", "microsoft/",
        "mistral", "mistralai/", "codestral", "ministral",
        "grok", "xai/", "x-ai/",
        "cerebras/", NULL
    };
    return provider_model_has_any_prefix(m, key_prefixes);
}

bool provider_model_supports_automatic_prompt_cache(const char *model) {
    if (!model)
        return false;
    const char *m = provider_model_strip_explicit_openrouter_prefix(model);
    static const char *const prefixes[] = {
        "gpt-4o", "gpt-4.1", "gpt-5", "o1", "o3", "o4", "chatgpt-",
        "openai/gpt-4o", "openai/gpt-4.1", "openai/gpt-5", "openai/o",
        "azure/", "azure-foundry/", "microsoft/",
        "mistral", "mistralai/", "codestral", "ministral",
        "deepseek", "deepseek/",
        "grok", "xai/", "x-ai/",
        "openai/gpt-oss-", "gpt-oss-",
        "cerebras/", "llama-", NULL
    };
    static const char *const contains[] = {"gpt-oss", NULL};
    return provider_model_has_any_prefix(m, prefixes) || provider_model_contains_any(m, contains);
}


static const char *openai_extra_params_json(void) {
    const char *raw = getenv("DSCO_OPENAI_PARAMS");
    if (!raw || !raw[0])
        raw = getenv("OPENAI_PARAMS");
    if (!raw || !raw[0])
        return NULL;
    while (*raw && isspace((unsigned char)*raw))
        raw++;
    return *raw == '{' ? raw : NULL;
}

static bool openai_extra_has_param(const char *extra, const char *key) {
    if (!extra || !key)
        return false;
    char *raw = json_get_raw(extra, key);
    if (!raw)
        return false;
    free(raw);
    return true;
}

static void openai_append_raw_param(jbuf_t *b, const char *key, const char *raw) {
    if (!b || !key || !raw)
        return;
    const char *v = raw;
    while (*v && isspace((unsigned char)*v))
        v++;
    if (!*v)
        return;
    jbuf_append(b, ",\"");
    jbuf_append(b, key);
    jbuf_append(b, "\":");
    jbuf_append(b, v);
}

static void openai_append_extra_param_if_present(jbuf_t *b, const char *extra, const char *key) {
    char *raw = json_get_raw(extra, key);
    if (!raw)
        return;
    openai_append_raw_param(b, key, raw);
    free(raw);
}

static void openai_append_extra_request_params(jbuf_t *b, const char *extra) {
    if (!extra)
        return;
    /* Chat Completions + current OpenAI-compatible extensions. Ownership fields
     * (model/messages/stream/tools/tool_choice/token limit) are emitted by dsco
     * itself; everything below is safe request policy or output-shaping surface.
     * Use DSCO_OPENAI_PARAMS='{"response_format":{"type":"json_object"},...}'. */
    static const char *const keys[] = {
        "audio",
        "frequency_penalty",
        "function_call",
        "functions",
        "logit_bias",
        "logprobs",
        "metadata",
        "modalities",
        "n",
        "parallel_tool_calls",
        "prediction",
        "presence_penalty",
        "reasoning",
        "reasoning_effort",
        "response_format",
        "seed",
        "service_tier",
        "stop",
        "store",
        "stream_options",
        "temperature",
        "top_logprobs",
        "top_p",
        "user",
        "verbosity",
        "web_search_options",
        NULL,
    };
    for (int i = 0; keys[i]; i++)
        openai_append_extra_param_if_present(b, extra, keys[i]);
}

static bool provider_strip_slash_namespace(const char *model, const char *ns,
                                           const char **stripped) {
    if (!model || !ns || !ns[0])
        return false;
    size_t n = strlen(ns);
    if (strncmp(model, ns, n) == 0 && model[n] == '/' && model[n + 1] != '\0') {
        if (stripped)
            *stripped = model + n + 1;
        return true;
    }
    return false;
}

static const char *provider_strip_profile_namespace(const char *provider_name,
                                                    const char *model) {
    const char *canonical =
        provider_name && provider_name[0] ? provider_profile_canonical_name(provider_name) : NULL;
    if (!canonical || !canonical[0])
        return model;

    const char *stripped = NULL;
    if (provider_strip_slash_namespace(model, canonical, &stripped))
        return stripped;

    const provider_profile_t *profile = provider_profile_find(canonical);
    if (!profile)
        return model;
    for (int i = 0; i < PROVIDER_PROFILE_MAX_ALIASES && profile->aliases[i]; i++) {
        if (provider_strip_slash_namespace(model, profile->aliases[i], &stripped))
            return stripped;
    }
    return model;
}

static const char *provider_request_model_id(const char *provider_name, const char *model) {
    if (!model || !model[0])
        return DEFAULT_MODEL;
    const char *canonical =
        provider_name && provider_name[0] ? provider_profile_canonical_name(provider_name) : NULL;
    if (canonical && strcmp(canonical, "openrouter") == 0)
        return provider_model_strip_explicit_openrouter_prefix(model);
    if (canonical &&
        (strcmp(canonical, "openai") == 0 || strcmp(canonical, "openai-codex") == 0) &&
        strncmp(model, "openai/", 7) == 0) {
        return model + 7;
    }
    if (canonical && canonical[0]) {
        if (strcmp(canonical, "anthropic") == 0 && strncmp(model, "anthropic/", 10) == 0)
            return model + 10;
        if (strcmp(canonical, "xai") == 0 && strncmp(model, "x-ai/", 5) == 0)
            return model + 5;
        if (strcmp(canonical, "google") == 0 && strncmp(model, "google/", 7) == 0)
            return model + 7;
        if (strcmp(canonical, "deepseek") == 0 && strncmp(model, "deepseek/", 9) == 0)
            return model + 9;
        if (strcmp(canonical, "qwen-oauth") == 0 && strncmp(model, "qwen/", 5) == 0)
            return model + 5;
        if (strcmp(canonical, "mistral") == 0 && strncmp(model, "mistralai/", 10) == 0)
            return model + 10;
        if (strcmp(canonical, "moonshot") == 0 && strncmp(model, "moonshotai/", 11) == 0)
            return model + 11;
        if (strcmp(canonical, "sakana") == 0 && strncmp(model, "sakana/", 7) == 0)
            return model + 7;
        if (strcmp(canonical, "cohere") == 0 && strncmp(model, "cohere/", 7) == 0)
            return model + 7;
        if (strcmp(canonical, "perplexity") == 0 && strncmp(model, "perplexity/", 11) == 0)
            return model + 11;
        if (strcmp(canonical, "zai") == 0 && strncmp(model, "z-ai/", 5) == 0)
            return model + 5;
        if (strcmp(canonical, "minimax") == 0 && strncmp(model, "minimax/", 8) == 0)
            return model + 8;
        if (strcmp(canonical, "bedrock") == 0 && strncmp(model, "amazon/", 7) == 0)
            return model + 7;
        if (strcmp(canonical, "azure-foundry") == 0) {
            const char *stripped = NULL;
            if (provider_strip_slash_namespace(model, "microsoft", &stripped))
                return stripped;
        }
        const char *profile_model = provider_strip_profile_namespace(canonical, model);
        if (profile_model != model)
            return profile_model;
    }
    /* Strip a leading "<provider>:" prefix (e.g. "vllm:Qwen2.5" -> "Qwen2.5",
     * "ollama:llama3.3:latest" -> "llama3.3:latest") so self-hosted backends
     * receive the bare served model name. Only the provider's own prefix is
     * stripped; colons inside the model id are preserved. */
    if (canonical && canonical[0]) {
        size_t pn = strlen(canonical);
        if (strncmp(model, canonical, pn) == 0 && model[pn] == ':')
            return model + pn + 1;
        const provider_profile_t *profile = provider_profile_find(canonical);
        if (profile) {
            for (int i = 0; i < PROVIDER_PROFILE_MAX_ALIASES && profile->aliases[i]; i++) {
                size_t an = strlen(profile->aliases[i]);
                if (strncmp(model, profile->aliases[i], an) == 0 && model[an] == ':')
                    return model + an + 1;
            }
        }
    }
    return model;
}

static char *openai_build_request(provider_t *p, conversation_t *conv, session_state_t *session,
                                  int max_tokens, const char *credential) {
    openai_data_t *od = (openai_data_t *)p->data;
    (void)od;
    (void)credential;

    jbuf_t b;
    jbuf_init(&b, 8192);
    const char *extra_params = openai_extra_params_json();

    jbuf_append(&b, "{\"model\":");
    const char *request_model =
        provider_request_model_id(p ? p->name : NULL, session ? session->model : DEFAULT_MODEL);
    jbuf_append_json_str(&b, request_model);
    const char *provider_name = p && p->name ? provider_profile_canonical_name(p->name) : NULL;
    if (extra_params && openai_extra_has_param(extra_params, "max_completion_tokens")) {
        openai_append_extra_param_if_present(&b, extra_params, "max_completion_tokens");
    } else if (extra_params && openai_extra_has_param(extra_params, "max_tokens")) {
        openai_append_extra_param_if_present(&b, extra_params, "max_tokens");
    } else {
        if (provider_name && strcmp(provider_name, "sakana") == 0)
            jbuf_append(&b, ",\"max_completion_tokens\":");
        else
            jbuf_append(&b, ",\"max_tokens\":");
        jbuf_append_int(&b, max_tokens);
    }
    jbuf_append(&b, ",\"stream\":true");

    if (provider_model_supports_prompt_cache_key(session ? session->model : request_model)) {
        const char *cache_key = (session && session->prompt_cache_key[0]) ? session->prompt_cache_key
                              : getenv("DSCO_PROMPT_CACHE_KEY");
        if (!cache_key || !cache_key[0])
            cache_key = "dsco-default";
        jbuf_append(&b, ",\"prompt_cache_key\":");
        jbuf_append_json_str(&b, cache_key);
    }
    if (provider_model_supports_prompt_cache_retention(session ? session->model : request_model)) {
        const char *retention = (session && session->prompt_cache_retention[0])
                                    ? session->prompt_cache_retention
                                    : getenv("DSCO_PROMPT_CACHE_RETENTION");
        if (!retention || !retention[0])
            retention = "24h";
        jbuf_append(&b, ",\"prompt_cache_retention\":");
        jbuf_append_json_str(&b, retention);
    }

    /* Moonshot/Kimi K2.7 Code requires thinking mode and rejects explicit
     * non-thinking payloads. Emit provider-native thinking enabled when using
     * Moonshot-compatible models so tool-calling + reasoning can coexist.
     *
     * K2.7 Code also enforces fixed sampling parameters — temperature=1.0,
     * top_p=0.95, n=1, penalties=0.0 — and rejects any other values with a 400.
     * We inject the server-required values explicitly to avoid any downstream
     * defaults or user overrides causing rejections. */
    bool moonshot_fixed_sampling = model_is_moonshot_compatible(session ? session->model : NULL);
    if (moonshot_fixed_sampling) {
        jbuf_append(&b, ",\"thinking\":{\"type\":\"enabled\"}");
        jbuf_append(&b, ",\"temperature\":1.0");
        jbuf_append(&b, ",\"top_p\":0.95");
        jbuf_append(&b, ",\"n\":1");
        jbuf_append(&b, ",\"presence_penalty\":0.0");
        jbuf_append(&b, ",\"frequency_penalty\":0.0");
    } else {
        if (session && session->temperature >= 0 &&
            !(extra_params && openai_extra_has_param(extra_params, "temperature"))) {
            jbuf_appendf(&b, ",\"temperature\":%.6g", session->temperature);
        }
        if (session && session->top_p >= 0 &&
            !(extra_params && openai_extra_has_param(extra_params, "top_p"))) {
            jbuf_appendf(&b, ",\"top_p\":%.6g", session->top_p);
        }
        if (session && session->effort[0] && strcmp(session->effort, "high") != 0 &&
            !(extra_params && (openai_extra_has_param(extra_params, "reasoning_effort") ||
                               openai_extra_has_param(extra_params, "reasoning")))) {
            jbuf_append(&b, ",\"reasoning_effort\":");
            jbuf_append_json_str(&b, session->effort);
        }
        openai_append_extra_request_params(&b, extra_params);
    }

    /* System message. Build the text once, then emit either as a plain string
     * (default) or as a single-block array carrying a cache_control breakpoint
     * (Claude via OpenRouter) so the static tools+system prefix gets cached. */
    bool cache_ctrl = provider_model_supports_cache_control(session ? session->model : NULL);
    const char *custom = llm_get_custom_system_prompt();
    jbuf_t sys;
    jbuf_init(&sys, 4096);
    if (custom) {
        jbuf_append(&sys, custom);
        jbuf_append(&sys, "\n\n");
    }
    const char *base_system = openai_tools_disabled()
        ? "You are dsco, a concise local CLI assistant. Answer the user's request directly. "
          "Do not mention tools unless the user asks about them."
        : (g_cheap_mode ? SYSTEM_PROMPT_CHEAP : SYSTEM_PROMPT);
    jbuf_append(&sys, base_system);

    jbuf_append(&b, ",\"messages\":[{\"role\":\"system\",\"content\":");
    if (cache_ctrl) {
        jbuf_append(&b, "[{\"type\":\"text\",\"text\":");
        jbuf_append_json_str(&b, sys.data ? sys.data : base_system);
        jbuf_append(&b, ",\"cache_control\":{\"type\":\"ephemeral\"}}]");
    } else {
        jbuf_append_json_str(&b, sys.data ? sys.data : base_system);
    }
    jbuf_free(&sys);
    jbuf_append(&b, "}");

    /* Conversation messages — convert Anthropic format to OpenAI format:
     *   - assistant + tool_use  → {"role":"assistant","tool_calls":[...]}
     *   - user + tool_result    → {"role":"tool","tool_call_id":"...","content":"..."}
     *                             followed by any remaining user text as {"role":"user",...}
     *   - plain user/assistant  → {"role":"user/assistant","content":"..."}
     */
    for (int i = 0; i < conv->count; i++) {
        message_t *m = &conv->msgs[i];

        if (m->role == ROLE_ASSISTANT) {
            openai_append_assistant_msg(&b, m);
        } else {
            /* User message: emit tool_results first, then remaining user content */
            if (msg_has_tool_result(m)) {
                openai_append_tool_results(&b, m);
                /* If there's also text content beyond tool results, emit a user msg */
                bool has_text = false;
                for (int j = 0; j < m->content_count; j++) {
                    msg_content_t *mc = &m->content[j];
                    if (mc->type && strcmp(mc->type, "text") == 0 && mc->text && mc->text[0]) {
                        has_text = true;
                        break;
                    }
                }
                if (has_text) {
                    openai_append_user_msg(&b, m);
                }
            } else {
                openai_append_user_msg(&b, m);
            }
        }
    }
    jbuf_append(&b, "]");

    bool has_tools = openai_append_tools_json(&b, conv, session);
    openai_append_tool_choice_json(&b, session, has_tools);

    /* Top-level automatic cache_control for Anthropic/Qwen via OpenRouter.
     * In "automatic" mode OR/Anthropic caches everything up to the last
     * cacheable block, advancing the breakpoint each turn — covers growing
     * conversation history without per-block markers. OR routes only to
     * Anthropic direct when this field is present (Bedrock/Vertex excluded). */
    if (provider_model_supports_cache_control(session ? session->model : NULL))
        jbuf_append(&b, ",\"cache_control\":{\"type\":\"ephemeral\"}");

    jbuf_append(&b, "}");
    return b.data;
}

static struct curl_slist *openai_build_headers(provider_t *p, const char *api_key) {
    struct curl_slist *hdrs = NULL;
    hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
    const char *canonical = p && p->name ? provider_profile_canonical_name(p->name) : NULL;
    /* Local OpenAI-compatible servers (Ollama, LM Studio, MLX, vLLM, etc.)
     * are keyless by default.  Avoid sending DSCO's synthetic "local" token:
     * some local gateways tolerate it, but Ollama's OpenAI shim does not need
     * auth and custom proxies may reject unexpected Authorization headers. */
    if (!provider_is_local_endpoint(canonical) || (api_key && api_key[0] && strcmp(api_key, "local") != 0)) {
        char auth[512];
        snprintf(auth, sizeof(auth), "Authorization: Bearer %s", api_key ? api_key : "");
        hdrs = curl_slist_append(hdrs, auth);
    }
    if (canonical && strcmp(canonical, "xai") == 0) {
        const char *cache_key = getenv("DSCO_PROMPT_CACHE_KEY");
        if (!cache_key || !cache_key[0])
            cache_key = "dsco-default";
        char hdr[256];
        snprintf(hdr, sizeof(hdr), "x-grok-conv-id: %s", cache_key);
        hdrs = curl_slist_append(hdrs, hdr);
    }
    return hdrs;
}

static void codex_exec_append_message_text(jbuf_t *b, const char *label, const message_t *m) {
    if (!b || !m)
        return;
    jbuf_append(b, "\n\n");
    jbuf_append(b, label);
    jbuf_append(b, ":\n");

    bool wrote = false;
    for (int i = 0; i < m->content_count; i++) {
        const msg_content_t *mc = &m->content[i];
        if (!mc->type)
            continue;
        if (strcmp(mc->type, "text") == 0 && mc->text && mc->text[0]) {
            if (wrote)
                jbuf_append(b, "\n");
            jbuf_append(b, mc->text);
            wrote = true;
        } else if (strcmp(mc->type, "tool_use") == 0) {
            if (wrote)
                jbuf_append(b, "\n");
            jbuf_append(b, "[tool request");
            if (mc->tool_name && mc->tool_name[0]) {
                jbuf_append(b, " ");
                jbuf_append(b, mc->tool_name);
            }
            jbuf_append(b, "] ");
            jbuf_append(b, mc->tool_input && mc->tool_input[0] ? mc->tool_input : "{}");
            wrote = true;
        } else if (strcmp(mc->type, "tool_result") == 0) {
            if (wrote)
                jbuf_append(b, "\n");
            jbuf_append(b, "[tool result");
            if (mc->tool_name && mc->tool_name[0]) {
                jbuf_append(b, " ");
                jbuf_append(b, mc->tool_name);
            }
            jbuf_append(b, "] ");
            jbuf_append(b, mc->text && mc->text[0] ? mc->text : "");
            wrote = true;
        } else if (strcmp(mc->type, "image") == 0) {
            if (wrote)
                jbuf_append(b, "\n");
            jbuf_append(b, "[image omitted by Codex CLI provider]");
            wrote = true;
        } else if (strcmp(mc->type, "document") == 0) {
            if (wrote)
                jbuf_append(b, "\n");
            jbuf_append(b, "[document omitted by Codex CLI provider]");
            wrote = true;
        }
    }

    if (!wrote)
        jbuf_append(b, "(empty)");
}

static char *codex_exec_build_request(provider_t *p, conversation_t *conv, session_state_t *session,
                                      int max_tokens, const char *credential) {
    (void)p;
    (void)max_tokens;
    (void)credential;

    const char *model =
        provider_request_model_id("openai-codex",
                                  session ? session->model : codex_cache_default_model());

    jbuf_t prompt;
    jbuf_init(&prompt, 8192);
    const char *custom = llm_get_custom_system_prompt();
    if (custom && custom[0]) {
        jbuf_append(&prompt, custom);
        jbuf_append(&prompt, "\n\n");
    }
    jbuf_append(&prompt, SYSTEM_PROMPT);
    jbuf_append(&prompt, "\n\nYou are being invoked through the Codex CLI using the user's "
                         "ChatGPT subscription. Answer the latest user turn directly.");

    if (conv) {
        for (int i = 0; i < conv->count; i++) {
            const message_t *m = &conv->msgs[i];
            codex_exec_append_message_text(&prompt, m->role == ROLE_USER ? "User" : "Assistant", m);
        }
    }

    jbuf_t out;
    jbuf_init(&out, prompt.len + 256);
    jbuf_append(&out, "{\"model\":");
    jbuf_append_json_str(&out, model);
    jbuf_append(&out, ",\"prompt\":");
    jbuf_append_json_str(&out, prompt.data ? prompt.data : "");
    jbuf_append(&out, "}");
    jbuf_free(&prompt);
    return out.data;
}

static void codex_exec_make_result(stream_result_t *result, bool ok, int status, const char *text) {
    memset(result, 0, sizeof(*result));
    result->ok = ok;
    result->http_status = ok ? 200 : 500;
    result->parsed.stop_reason = safe_strdup(ok ? "end_turn" : "error");
    result->parsed.blocks = safe_malloc(sizeof(content_block_t));
    memset(result->parsed.blocks, 0, sizeof(content_block_t));
    result->parsed.count = 1;
    result->parsed.blocks[0].type = safe_strdup("text");
    result->parsed.blocks[0].text = safe_strdup(text ? text : "");
    result->usage.output_tokens = rough_token_estimate(text);
    (void)status;
}

static bool codex_exec_write_all(int fd, const char *data) {
    size_t len = data ? strlen(data) : 0;
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, data + off, len - off);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        off += (size_t)n;
    }
    return true;
}

static stream_result_t codex_exec_stream(provider_t *p, const char *api_key,
                                         const char *request_json, stream_text_cb text_cb,
                                         stream_tool_start_cb tool_cb,
                                         stream_thinking_cb thinking_cb, void *cb_ctx) {
    (void)p;
    (void)api_key;
    (void)tool_cb;
    (void)thinking_cb;

    stream_result_t result;
    char *model = json_get_str(request_json, "model");
    char *prompt = json_get_str(request_json, "prompt");
    if (!model || !model[0] || !prompt) {
        codex_exec_make_result(&result, false, 0, "Codex provider request missing model or prompt");
        free(model);
        free(prompt);
        return result;
    }

    char codex_path[1024];
    if (!provider_find_executable("codex", codex_path, sizeof(codex_path))) {
        codex_exec_make_result(&result, false, 0, "codex executable not found in PATH");
        free(model);
        free(prompt);
        return result;
    }

    char out_template[] = "/tmp/dsco-codex-last-XXXXXX";
    int out_fd = mkstemp(out_template);
    if (out_fd < 0) {
        codex_exec_make_result(&result, false, 0, "failed to create Codex output file");
        free(model);
        free(prompt);
        return result;
    }
    close(out_fd);

    int in_pipe[2] = {-1, -1};
    int log_pipe[2] = {-1, -1};
    if (pipe(in_pipe) != 0 || pipe(log_pipe) != 0) {
        if (in_pipe[0] >= 0)
            close(in_pipe[0]);
        if (in_pipe[1] >= 0)
            close(in_pipe[1]);
        if (log_pipe[0] >= 0)
            close(log_pipe[0]);
        if (log_pipe[1] >= 0)
            close(log_pipe[1]);
        unlink(out_template);
        codex_exec_make_result(&result, false, 0, "pipe failed for Codex provider");
        free(model);
        free(prompt);
        return result;
    }

    double t0 = provider_now_sec();
    pid_t pid = fork();
    if (pid == 0) {
        setpgid(0, 0);
        dup2(in_pipe[0], STDIN_FILENO);
        dup2(log_pipe[1], STDOUT_FILENO);
        dup2(log_pipe[1], STDERR_FILENO);
        close(in_pipe[0]);
        close(in_pipe[1]);
        close(log_pipe[0]);
        close(log_pipe[1]);
        execl(codex_path, "codex", "exec", "--color", "never", "--sandbox", "read-only",
              "--ask-for-approval", "never", "--skip-git-repo-check", "-m", model, "-o",
              out_template, "-", (char *)NULL);
        _exit(127);
    }
    if (pid < 0) {
        close(in_pipe[0]);
        close(in_pipe[1]);
        close(log_pipe[0]);
        close(log_pipe[1]);
        unlink(out_template);
        codex_exec_make_result(&result, false, 0, "fork failed for Codex provider");
        free(model);
        free(prompt);
        return result;
    }

    close(in_pipe[0]);
    close(log_pipe[1]);
    int log_flags = fcntl(log_pipe[0], F_GETFL, 0);
    if (log_flags >= 0)
        fcntl(log_pipe[0], F_SETFL, log_flags | O_NONBLOCK);

    void (*old_sigpipe)(int) = signal(SIGPIPE, SIG_IGN);
    bool wrote_prompt = codex_exec_write_all(in_pipe[1], prompt);
    signal(SIGPIPE, old_sigpipe);
    close(in_pipe[1]);

    jbuf_t logs;
    jbuf_init(&logs, 4096);
    int status = 0;
    bool done = false;
    bool timed_out = false;
    int timeout_s = 300;
    const char *timeout_env = getenv("DSCO_CODEX_EXEC_TIMEOUT");
    if (timeout_env && timeout_env[0]) {
        int v = atoi(timeout_env);
        if (v > 0)
            timeout_s = v;
    }

    struct pollfd pfd = {.fd = log_pipe[0], .events = POLLIN};
    while (!done) {
        int ready = poll(&pfd, 1, 100);
        if (ready > 0 && (pfd.revents & (POLLIN | POLLHUP | POLLERR))) {
            char buf[2048];
            ssize_t n;
            while ((n = read(log_pipe[0], buf, sizeof(buf) - 1)) > 0) {
                buf[n] = '\0';
                jbuf_append(&logs, buf);
            }
        }

        pid_t w = waitpid(pid, &status, WNOHANG);
        if (w == pid) {
            done = true;
            break;
        }
        if (w < 0 && errno != EINTR) {
            done = true;
            break;
        }
        if (provider_now_sec() - t0 > timeout_s) {
            timed_out = true;
            kill(-pid, SIGTERM);
            usleep(100000);
            kill(-pid, SIGKILL);
            waitpid(pid, &status, 0);
            done = true;
            break;
        }
    }

    char drain[2048];
    ssize_t dn;
    while ((dn = read(log_pipe[0], drain, sizeof(drain) - 1)) > 0) {
        drain[dn] = '\0';
        jbuf_append(&logs, drain);
    }
    close(log_pipe[0]);

    char *answer = provider_read_text_file(out_template);
    unlink(out_template);

    bool exited_ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    bool ok = wrote_prompt && exited_ok && answer && answer[0] && !timed_out;
    if (!ok) {
        jbuf_t err;
        jbuf_init(&err, 512 + (logs.data ? logs.len : 0));
        if (timed_out) {
            jbuf_append(&err, "Codex provider timed out");
        } else if (!wrote_prompt) {
            jbuf_append(&err, "failed writing prompt to Codex provider");
        } else {
            jbuf_append(&err, "Codex provider failed");
            if (WIFEXITED(status)) {
                jbuf_appendf(&err, " with status %d", WEXITSTATUS(status));
            }
        }
        if (logs.data && logs.data[0]) {
            jbuf_append(&err, "\n");
            jbuf_append(&err, logs.data);
        }
        codex_exec_make_result(&result, false, status, err.data);
        jbuf_free(&err);
    } else {
        dsco_strip_terminal_controls_inplace(answer);
        if (text_cb)
            text_cb(answer, cb_ctx);
        codex_exec_make_result(&result, true, status, answer);
        result.usage.input_tokens = rough_token_estimate(prompt);
    }

    free(answer);
    jbuf_free(&logs);
    free(model);
    free(prompt);
    return result;
}

/* ── OpenAI SSE streaming state ─────────────────────────────────────── */

typedef struct {
    bool used;
    bool announced;
    int index;
    char *tool_name;
    char *tool_id;
    jbuf_t tool_args;
    /* How much of tool_args has been streamed to the user (for live arg rendering) */
    size_t streamed_prefix;
} oai_tool_call_state_t;

typedef struct {
    jbuf_t line_buf;
    jbuf_t text_accum;
    jbuf_t reasoning_accum;
    stream_text_cb text_cb;
    stream_tool_start_cb tool_cb;
    stream_thinking_cb thinking_cb;
    void *cb_ctx;
    usage_t usage;
    char *stop_reason;
    bool got_error;
    bool credit_too_low; /* 402 / "credit balance too low" / insufficient funds */
    char *error_msg;
    time_t credit_reset_at;
    /* OpenRouter-specific */
    char *generation_id;  /* x-generation-id from response */
    char *actual_model;   /* model actually used (may differ from requested) */
    double cost_usd;      /* total cost from usage.cost */
    int cached_tokens;    /* input_tokens_details.cached_tokens */
    int reasoning_tokens; /* output_tokens_details.reasoning_tokens */
    oai_tool_call_state_t tool_calls[MAX_CONTENT_BLOCKS];
    int tool_slots_used;
    /* Result building */
    content_block_t blocks[MAX_CONTENT_BLOCKS];
    int block_count;
    double telemetry_start;
    double telemetry_first_delta;
    double telemetry_first_tool;
    bool telemetry_got_first;
} oai_sse_state_t;

/* Classify an error body/message as a credit/billing failure so the caller
 * can either fall back or show an actionable message instead of a raw 400.
 * Exported via provider.h so both the OpenAI-compat and Anthropic streaming
 * paths can share the same detection. */
bool provider_msg_is_credit_too_low(const char *msg) {
    if (!msg || !msg[0])
        return false;
    /* case-insensitive substring match against known phrases */
    static const char *needles[] = {
        "credit balance is too low",   "credit balance too low", "insufficient_quota",
        "insufficient funds",          "insufficient credit",    "billing_hard_limit_reached",
        "exceeded your current quota", "payment required",       "quota_exceeded",
        "billing not active",          "requires a paid plan",   "subscription window is exhausted",
        "window is exhausted",         "rate_limit_exceeded",    "rate limit exceeded",
        "too many requests",           "temporarily rate limited", "usage limit",
        NULL,
    };
    for (int i = 0; needles[i]; i++) {
        const char *n = needles[i];
        size_t nlen = strlen(n);
        for (const char *p = msg; *p; p++) {
            if (strncasecmp(p, n, nlen) == 0)
                return true;
        }
    }
    return false;
}

static time_t provider_reset_max(time_t a, time_t b) {
    if (b <= 0)
        return a;
    if (a <= 0)
        return b;
    return b > a ? b : a;
}

static void provider_reset_value_copy(const char *value, char *out, size_t out_len) {
    if (!out || out_len == 0)
        return;
    out[0] = '\0';
    if (!value)
        return;
    while (*value && isspace((unsigned char)*value))
        value++;
    size_t n = strlen(value);
    while (n > 0 && isspace((unsigned char)value[n - 1]))
        n--;
    if (n >= 2 && value[0] == '"' && value[n - 1] == '"') {
        value++;
        n -= 2;
    }
    if (n >= out_len)
        n = out_len - 1;
    memcpy(out, value, n);
    out[n] = '\0';
}

static long long provider_days_from_civil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? (unsigned)-3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (long long)era * 146097LL + (long long)doe - 719468LL;
}

static time_t provider_parse_iso_reset_at(const char *value) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0, pos = 0;
    if (sscanf(value, "%4d-%2d-%2dT%2d:%2d:%2d%n", &y, &mo, &d, &h, &mi, &s, &pos) != 6 &&
        sscanf(value, "%4d-%2d-%2d %2d:%2d:%2d%n", &y, &mo, &d, &h, &mi, &s, &pos) != 6)
        return 0;
    if (y < 1970 || mo < 1 || mo > 12 || d < 1 || d > 31 || h < 0 || h > 23 ||
        mi < 0 || mi > 59 || s < 0 || s > 60)
        return 0;

    long long epoch = provider_days_from_civil(y, (unsigned)mo, (unsigned)d) * 86400LL +
                      h * 3600LL + mi * 60LL + s;
    if (value[pos] == 'Z' || value[pos] == '\0')
        return (time_t)epoch;
    if (value[pos] == '+' || value[pos] == '-') {
        int sign = value[pos] == '+' ? 1 : -1;
        int tzh = 0, tzm = 0;
        if (sscanf(value + pos + 1, "%2d:%2d", &tzh, &tzm) >= 1) {
            long long offset = (long long)tzh * 3600LL + (long long)tzm * 60LL;
            epoch -= sign * offset;
            return (time_t)epoch;
        }
    }
    return 0;
}

time_t provider_credit_reset_at_from_value(const char *value, time_t now) {
    char buf[256];
    provider_reset_value_copy(value, buf, sizeof(buf));
    if (!buf[0])
        return 0;

    char *end = NULL;
    double num = strtod(buf, &end);
    if (end && end > buf) {
        while (*end && isspace((unsigned char)*end))
            end++;
        if (strncasecmp(end, "ms", 2) == 0) {
            long long sec = (long long)((num + 999.0) / 1000.0);
            return now + (time_t)(sec > 0 ? sec : 1);
        }
        if (*end == 's' || strncasecmp(end, "sec", 3) == 0) {
            long long sec = (long long)num;
            if ((double)sec < num)
                sec++;
            return now + (time_t)(sec > 0 ? sec : 1);
        }
        if (*end == 'm' || strncasecmp(end, "min", 3) == 0)
            return now + (time_t)(num * 60.0);
        if (*end == 'h')
            return now + (time_t)(num * 3600.0);
        if (*end == 'd')
            return now + (time_t)(num * 86400.0);
        if (*end == '\0') {
            if (num > 100000000000.0)
                return (time_t)(num / 1000.0); /* epoch milliseconds */
            if (num > 1000000000.0)
                return (time_t)num; /* epoch seconds */
            long long sec = (long long)num;
            if ((double)sec < num)
                sec++;
            return now + (time_t)(sec > 0 ? sec : 1);
        }
    }

    time_t iso = provider_parse_iso_reset_at(buf);
    if (iso > 0)
        return iso;

    time_t parsed = curl_getdate(buf, &now);
    if (parsed != (time_t)-1)
        return parsed;

    return 0;
}

static time_t provider_credit_reset_at_from_json_fields(const char *json, time_t now) {
    if (!json || !json[0])
        return 0;
    static const char *fields[] = {
        "resetsAt", "resets_at", "resetAt", "reset_at", "reset_time", "resetTime",
        "retryAfter", "retry_after", "retry_after_seconds", "retryAfterSeconds",
        "availableAt", "available_at", "reopensAt", "reopens_at", "opensAt", "opens_at",
        "until", "window_reset_at", "windowResetAt", NULL,
    };
    time_t best = 0;
    for (int i = 0; fields[i]; i++) {
        char *raw = json_get_raw(json, fields[i]);
        if (!raw)
            continue;
        best = provider_reset_max(best, provider_credit_reset_at_from_value(raw, now));
        free(raw);
    }
    return best;
}

static time_t provider_credit_reset_at_from_embedded_date(const char *text, time_t now) {
    if (!text)
        return 0;
    for (const char *p = text; *p; p++) {
        if (!isdigit((unsigned char)p[0]) || !isdigit((unsigned char)p[1]) ||
            !isdigit((unsigned char)p[2]) || !isdigit((unsigned char)p[3]) ||
            p[4] != '-' || !isdigit((unsigned char)p[5]) || !isdigit((unsigned char)p[6]) ||
            p[7] != '-' || !isdigit((unsigned char)p[8]) || !isdigit((unsigned char)p[9]))
            continue;
        char candidate[80];
        size_t n = 0;
        while (p[n] && n < sizeof(candidate) - 1 && !isspace((unsigned char)p[n]) &&
               p[n] != '"' && p[n] != '\'' && p[n] != ',' && p[n] != ')' && p[n] != ']')
            n++;
        memcpy(candidate, p, n);
        candidate[n] = '\0';
        time_t parsed = provider_credit_reset_at_from_value(candidate, now);
        if (parsed > 0)
            return parsed;
    }
    return 0;
}

time_t provider_credit_reset_at_from_text(const char *text, time_t now) {
    if (!text || !text[0])
        return 0;

    time_t best = provider_credit_reset_at_from_json_fields(text, now);
    static const char *objects[] = {
        "error", "rate_limit", "rateLimit", "limits", "details", "metadata", NULL,
    };
    for (int i = 0; objects[i]; i++) {
        char *raw = json_get_raw(text, objects[i]);
        if (!raw)
            continue;
        best = provider_reset_max(best, provider_credit_reset_at_from_json_fields(raw, now));
        best = provider_reset_max(best, provider_credit_reset_at_from_embedded_date(raw, now));
        free(raw);
    }
    best = provider_reset_max(best, provider_credit_reset_at_from_embedded_date(text, now));
    return best;
}

bool provider_credit_reset_at_from_header_line(const char *line, time_t now,
                                               time_t *reset_at) {
    if (!line || !reset_at)
        return false;
    const char *colon = strchr(line, ':');
    if (!colon)
        return false;

    char key[160];
    size_t kn = (size_t)(colon - line);
    while (kn > 0 && isspace((unsigned char)line[kn - 1]))
        kn--;
    if (kn >= sizeof(key))
        kn = sizeof(key) - 1;
    for (size_t i = 0; i < kn; i++)
        key[i] = (char)tolower((unsigned char)line[i]);
    key[kn] = '\0';

    bool relevant = strcmp(key, "retry-after") == 0 ||
                    ((strstr(key, "ratelimit") || strstr(key, "rate-limit")) &&
                     (strstr(key, "reset") || strstr(key, "retry") || strstr(key, "until")));
    if (!relevant)
        return false;

    time_t parsed = provider_credit_reset_at_from_value(colon + 1, now);
    if (parsed <= 0)
        return false;
    *reset_at = provider_reset_max(*reset_at, parsed);
    return true;
}

static size_t provider_credit_header_cb(char *buffer, size_t size, size_t nitems,
                                        void *userdata) {
    size_t total = size * nitems;
    time_t *reset_at = (time_t *)userdata;
    if (!buffer || !reset_at || total == 0)
        return total;
    char line[512];
    size_t n = total < sizeof(line) - 1 ? total : sizeof(line) - 1;
    memcpy(line, buffer, n);
    line[n] = '\0';
    provider_credit_reset_at_from_header_line(line, time(NULL), reset_at);
    return total;
}

bool provider_msg_is_context_overflow(const char *msg) {
    if (!msg || !msg[0])
        return false;
    /* case-insensitive substring match against cross-provider phrases for a
     * prompt/context-length rejection. Kept specific to avoid false positives
     * on unrelated "too long" errors (e.g. path length). */
    static const char *needles[] = {
        "prompt is too long",
        "prompt is too large",
        "input is too long",
        "input is too large",
        "context length",
        "context_length",
        "context_length_exceeded",
        "maximum context",
        "context window",
        "too many tokens",
        "exceeds the maximum",
        "reduce the length",
        NULL,
    };
    for (int i = 0; needles[i]; i++) {
        const char *n = needles[i];
        size_t nlen = strlen(n);
        for (const char *p = msg; *p; p++) {
            if (strncasecmp(p, n, nlen) == 0)
                return true;
        }
    }
    return false;
}

static oai_tool_call_state_t *oai_tool_state_find_by_id(oai_sse_state_t *s, const char *tool_id) {
    if (!tool_id || !tool_id[0])
        return NULL;
    for (int i = 0; i < s->tool_slots_used; i++) {
        oai_tool_call_state_t *slot = &s->tool_calls[i];
        if (slot->used && slot->tool_id && strcmp(slot->tool_id, tool_id) == 0) {
            return slot;
        }
    }
    return NULL;
}

static oai_tool_call_state_t *oai_tool_state_for(oai_sse_state_t *s, int index,
                                                 const char *tool_id) {
    oai_tool_call_state_t *slot = NULL;

    if (index >= 0 && index < MAX_CONTENT_BLOCKS) {
        slot = &s->tool_calls[index];
        if (!slot->used) {
            memset(slot, 0, sizeof(*slot));
            slot->used = true;
            slot->index = index;
            jbuf_init(&slot->tool_args, 256);
            if (index + 1 > s->tool_slots_used)
                s->tool_slots_used = index + 1;
        }
        return slot;
    }

    slot = oai_tool_state_find_by_id(s, tool_id);
    if (slot)
        return slot;

    if (s->tool_slots_used >= MAX_CONTENT_BLOCKS)
        return NULL;
    slot = &s->tool_calls[s->tool_slots_used];
    memset(slot, 0, sizeof(*slot));
    slot->used = true;
    slot->index = s->tool_slots_used;
    jbuf_init(&slot->tool_args, 256);
    s->tool_slots_used++;
    return slot;
}

static const char *oai_tool_state_id(oai_tool_call_state_t *slot) {
    static char fallback[32];
    if (slot && slot->tool_id && slot->tool_id[0])
        return slot->tool_id;
    snprintf(fallback, sizeof(fallback), "call_%d", slot ? slot->index : 0);
    return fallback;
}

typedef struct {
    oai_sse_state_t *state;
} oai_tool_delta_ctx_t;

static void oai_handle_tool_call_delta(const char *tc_elem, void *ctx) {
    oai_tool_delta_ctx_t *tool_ctx = (oai_tool_delta_ctx_t *)ctx;
    oai_sse_state_t *s = tool_ctx->state;

    char *idx_raw = json_get_raw(tc_elem, "index");
    int idx = idx_raw ? atoi(idx_raw) : -1;
    free(idx_raw);

    char *tid = json_get_str(tc_elem, "id");
    oai_tool_call_state_t *slot = oai_tool_state_for(s, idx, tid);
    if (!slot) {
        free(tid);
        return;
    }
    if (tid && (!slot->tool_id || !slot->tool_id[0])) {
        slot->tool_id = tid;
        tid = NULL;
    }
    free(tid);

    char *fn_raw = json_get_raw(tc_elem, "function");
    if (!fn_raw)
        return;

    char *fname = json_get_str(fn_raw, "name");
    if (fname && (!slot->tool_name || !slot->tool_name[0])) {
        slot->tool_name = fname;
        fname = NULL;
    }
    free(fname);

    char *fargs = json_get_str(fn_raw, "arguments");
    if (fargs && fargs[0]) {
        jbuf_append(&slot->tool_args, fargs);
    }
    free(fargs);
    free(fn_raw);

    if (slot->tool_name && !slot->announced) {
        slot->announced = true;
        if (s->telemetry_first_tool <= 0.0)
            s->telemetry_first_tool = provider_now_sec();
        if (!s->telemetry_got_first) {
            s->telemetry_got_first = true;
            s->telemetry_first_delta = s->telemetry_first_tool;
        }
        if (s->tool_cb) {
            s->tool_cb(slot->tool_name, oai_tool_state_id(slot), s->cb_ctx);
        }
    }

    /* Stream newly-arrived argument bytes as dim text so the user sees
     * tool inputs populate in real time instead of appearing all at once
     * at the end of the turn. We route through text_cb because that is
     * the only always-wired sink on the OpenAI-compat path. The bytes
     * are visually distinguished with a leading "  ⋯ " marker on the
     * first chunk; subsequent chunks just append. */
    if (s->text_cb && slot->tool_args.data && slot->tool_args.len > slot->streamed_prefix) {
        size_t start = slot->streamed_prefix;
        size_t avail = slot->tool_args.len - start;
        /* Cap single-delta size so a giant argument string does not starve
         * the heartbeat and so we don't flood the terminal on one packet. */
        if (avail > 256)
            avail = 256;
        char chunk[320];
        size_t pos = 0;
        if (start == 0) {
            const char *prefix = "\033[2m ⋯ \033[0m";
            size_t plen = strlen(prefix);
            if (plen < sizeof(chunk)) {
                memcpy(chunk, prefix, plen);
                pos = plen;
            }
        }
        if (pos + avail >= sizeof(chunk))
            avail = sizeof(chunk) - pos - 1;
        memcpy(chunk + pos, slot->tool_args.data + start, avail);
        chunk[pos + avail] = '\0';
        s->text_cb(chunk, s->cb_ctx);
        slot->streamed_prefix = start + avail;
    }
}

static void oai_handle_sse_line(oai_sse_state_t *s, const char *line) {
    if (strncmp(line, "data: ", 6) != 0)
        return;
    const char *data = line + 6;
    if (strcmp(data, "[DONE]") == 0)
        return;

    /* Check for top-level error object (OpenRouter sends errors mid-stream) */
    char *err_raw = json_get_raw(data, "error");
    if (err_raw) {
        s->credit_reset_at = provider_reset_max(
            s->credit_reset_at, provider_credit_reset_at_from_text(err_raw, time(NULL)));
        char *err_msg = json_get_str(err_raw, "message");
        int err_code = json_get_int(err_raw, "code", 0);
        if (err_msg) {
            s->got_error = true;
            free(s->error_msg);
            s->error_msg = err_msg;
            if (err_code == 402 || err_code == 429 || provider_msg_is_credit_too_low(err_msg))
                s->credit_too_low = true;
            fprintf(stderr, "  \033[31mAPI error %d: %s\033[0m\n", err_code, err_msg);
        }
        free(err_raw);
        return;
    }

    /* Extract model actually used (first chunk usually has it) */
    if (!s->actual_model) {
        char *model = json_get_str(data, "model");
        if (model)
            s->actual_model = model;
    }

    /* Extract generation ID */
    if (!s->generation_id) {
        char *gid = json_get_str(data, "id");
        if (gid)
            s->generation_id = gid;
    }

    /* Parse usage (may appear in any chunk, usually the last) */
    char *usage_raw = json_get_raw(data, "usage");
    if (usage_raw) {
        /* OpenAI-compatible usage aliases:
         *   - OpenAI Chat Completions: prompt_tokens / completion_tokens
         *   - Responses-style providers (Sakana Fugu): input_tokens / output_tokens
         */
        {
            int prompt_tok = json_get_int(usage_raw, "prompt_tokens", -1);
            int input_tok = json_get_int(usage_raw, "input_tokens", -1);
            int completion_tok = json_get_int(usage_raw, "completion_tokens", -1);
            int output_tok = json_get_int(usage_raw, "output_tokens", -1);
            if (prompt_tok >= 0)
                s->usage.input_tokens = prompt_tok;
            else if (input_tok >= 0)
                s->usage.input_tokens = input_tok;
            if (completion_tok >= 0)
                s->usage.output_tokens = completion_tok;
            else if (output_tok >= 0)
                s->usage.output_tokens = output_tok;
        }

        /* Cost tracking (OpenRouter includes cost in usage) */
        char *cost_str = json_get_str(usage_raw, "cost");
        if (cost_str) {
            s->cost_usd = atof(cost_str);
            free(cost_str);
        }

        /* Token detail breakdowns — three possible locations depending on provider:
         *
         *  1. input_tokens_details.cached_tokens
         *     → OpenAI, xAI/Grok, Groq, Gemini via google endpoint, OpenRouter OR-normalised
         *  2. prompt_tokens_details.cached_tokens
         *     → Some OpenRouter backends normalise here instead of input_tokens_details
         *  3. prompt_cache_hit_tokens  (top-level in usage{})
         *     → DeepSeek direct, Moonshot/Kimi direct, Alibaba/DashScope direct
         *
         * We read all three and take the first non-zero value so no provider is missed.
         * Already-set values from earlier chunks are preserved (take max). */
        char *in_detail = json_get_raw(usage_raw, "input_tokens_details");
        if (in_detail) {
            int v = json_get_int(in_detail, "cached_tokens", 0);
            if (v > s->cached_tokens)
                s->cached_tokens = v;
            free(in_detail);
        }
        /* OpenRouter normalisation alias */
        char *pt_detail = json_get_raw(usage_raw, "prompt_tokens_details");
        if (pt_detail) {
            int v = json_get_int(pt_detail, "cached_tokens", 0);
            if (v > s->cached_tokens)
                s->cached_tokens = v;
            free(pt_detail);
        }
        /* DeepSeek / Moonshot / Alibaba top-level cache hit field */
        {
            int v = json_get_int(usage_raw, "prompt_cache_hit_tokens", 0);
            if (v > s->cached_tokens)
                s->cached_tokens = v;
        }
        char *out_detail = json_get_raw(usage_raw, "output_tokens_details");
        if (out_detail) {
            s->reasoning_tokens = json_get_int(out_detail, "reasoning_tokens", 0);
            free(out_detail);
        }
        /* Sakana Fugu Ultra: orchestration tokens are real billing tokens
         * outside base input/output counts.  Accumulate them so session
         * cost accounting isn't off by 2-5x on multi-agent turns. */
        {
            char *itd2 = json_get_raw(usage_raw, "input_tokens_details");
            if (itd2) {
                int orch_in = json_get_int(itd2, "orchestration_input_tokens", 0);
                int orch_in_cached = json_get_int(itd2, "orchestration_input_cached_tokens", 0);
                s->usage.input_tokens += orch_in;
                if (orch_in_cached > 0)
                    s->usage.cache_read_input_tokens += orch_in_cached;
                free(itd2);
            }
            char *otd2 = json_get_raw(usage_raw, "output_tokens_details");
            if (otd2) {
                int orch_out = json_get_int(otd2, "orchestration_output_tokens", 0);
                s->usage.output_tokens += orch_out;
                free(otd2);
            }
        }
        free(usage_raw);
    }

    /* Parse choices array — extract the first element (an object) */
    char *choices_raw = json_get_raw(data, "choices");
    if (!choices_raw)
        return;
    /* choices_raw is "[{...}]" — skip into the first element object */
    const char *first_choice = choices_raw;
    while (*first_choice &&
           (*first_choice == '[' || *first_choice == ' ' || *first_choice == '\n' ||
            *first_choice == '\r' || *first_choice == '\t'))
        first_choice++;

    /* Check finish_reason (including mid-stream errors and content filters) */
    char *fr = json_get_str(first_choice, "finish_reason");
    if (fr) {
        free(s->stop_reason);
        if (strcmp(fr, "stop") == 0)
            s->stop_reason = safe_strdup("end_turn");
        else if (strcmp(fr, "tool_calls") == 0)
            s->stop_reason = safe_strdup("tool_use");
        else if (strcmp(fr, "length") == 0)
            s->stop_reason = safe_strdup("max_tokens");
        else if (strcmp(fr, "error") == 0) {
            s->stop_reason = safe_strdup("error");
            s->got_error = true;
        } else if (strcmp(fr, "content_filter") == 0) {
            s->stop_reason = safe_strdup("content_filter");
            fprintf(stderr, "  \033[33mContent filter triggered\033[0m\n");
        } else {
            s->stop_reason = fr;
            fr = NULL; /* transferred ownership */
        }
        free(fr);
    }

    /* Extract delta from first choice */
    char *delta_raw = json_get_raw(first_choice, "delta");
    if (!delta_raw) {
        free(choices_raw);
        return;
    }

    /* Content delta — streaming text */
    char *content = json_get_str(delta_raw, "content");
    if (content && content[0]) {
        if (!s->telemetry_got_first) {
            s->telemetry_got_first = true;
            s->telemetry_first_delta = provider_now_sec();
        }
        jbuf_append(&s->text_accum, content);
        if (s->text_cb)
            s->text_cb(content, s->cb_ctx);
    }
    free(content);

    /* Reasoning delta — streaming "thinking"/chain-of-thought output.
     *
     * Different providers emit this under different keys:
     *   - xAI (grok-3-mini, grok-4):       delta.reasoning_content
     *   - OpenRouter reasoning models:     delta.reasoning    (string)
     *                                  or  delta.reasoning    (object with .content)
     *   - Deepseek reasoner:               delta.reasoning_content
     *   - Moonshot thinking:               delta.reasoning_content
     *
     * We parse both, accumulate them into reasoning_accum, and route them
     * through thinking_cb when the caller supplied one. If no thinking_cb
     * is wired, we surface the reasoning as dim text via text_cb so the
     * user still sees *something* during the pre-output thinking phase,
     * instead of a long silent pause. This is intentional "show the
     * thinking even if we throw most of it away" behavior.
     */
    {
        char *reasoning = json_get_str(delta_raw, "reasoning_content");
        if (!reasoning || !reasoning[0]) {
            free(reasoning);
            reasoning = json_get_str(delta_raw, "reasoning");
        }
        if (!reasoning || !reasoning[0]) {
            /* OpenRouter sometimes sends delta.reasoning as an object:
             * {"reasoning":{"content":"..."}}. Handle that shape too. */
            free(reasoning);
            reasoning = NULL;
            char *rraw = json_get_raw(delta_raw, "reasoning");
            if (rraw) {
                reasoning = json_get_str(rraw, "content");
                free(rraw);
            }
        }
        if (reasoning && reasoning[0]) {
            jbuf_append(&s->reasoning_accum, reasoning);
            if (s->thinking_cb) {
                s->thinking_cb(reasoning, s->cb_ctx);
            } else if (s->text_cb) {
                /* Fallback visualization: wrap in dim ANSI so markdown
                 * doesn't try to format it and it visually separates from
                 * final text. */
                char wrapped[1024];
                int n = snprintf(wrapped, sizeof(wrapped), "\033[2m%.960s\033[0m", reasoning);
                if (n > 0)
                    s->text_cb(wrapped, s->cb_ctx);
            }
        }
        free(reasoning);
    }

    /* Tool calls delta — accumulate every streamed call by index/id. */
    char *tool_calls_raw = json_get_raw(delta_raw, "tool_calls");
    if (tool_calls_raw) {
        oai_tool_delta_ctx_t tool_ctx = {.state = s};
        jbuf_t wrapped;
        jbuf_init(&wrapped, strlen(tool_calls_raw) + 32);
        jbuf_append(&wrapped, "{\"tool_calls\":");
        jbuf_append(&wrapped, tool_calls_raw);
        jbuf_append(&wrapped, "}");
        json_array_foreach(wrapped.data, "tool_calls", oai_handle_tool_call_delta, &tool_ctx);
        jbuf_free(&wrapped);
        free(tool_calls_raw);
    }

    free(delta_raw);
    free(choices_raw);
}

static size_t oai_sse_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    size_t total = size * nmemb;
    oai_sse_state_t *s = (oai_sse_state_t *)userdata;

    const char *p = (const char *)ptr;
    for (size_t i = 0; i < total; i++) {
        if (p[i] == '\n') {
            if (s->line_buf.len > 0) {
                oai_handle_sse_line(s, s->line_buf.data);
                jbuf_reset(&s->line_buf);
            }
        } else if (p[i] != '\r') {
            jbuf_append_char(&s->line_buf, p[i]);
        }
    }
    return total;
}

static stream_result_t openai_stream(provider_t *p, const char *api_key, const char *request_json,
                                     stream_text_cb text_cb, stream_tool_start_cb tool_cb,
                                     stream_thinking_cb thinking_cb, void *cb_ctx) {
    openai_data_t *od = (openai_data_t *)p->data;
    stream_result_t result = {0};

    if (!od || !od->api_url[0]) {
        result.ok = false;
        result.http_status = 0;
        result.parsed.stop_reason = safe_strdup("missing_api_base");
        result.parsed.blocks = safe_malloc(sizeof(content_block_t));
        memset(result.parsed.blocks, 0, sizeof(content_block_t));
        result.parsed.count = 1;
        result.parsed.blocks[0].type = safe_strdup("text");
        char msg[512];
        snprintf(msg, sizeof(msg),
                 "Provider '%s' needs a custom API base URL before DSCO can call it.",
                 p && p->name ? p->name : "unknown");
        result.parsed.blocks[0].text = safe_strdup(msg);
        if (text_cb)
            text_cb(msg, cb_ctx);
        return result;
    }

    CURL *curl = od && od->curl ? od->curl : curl_easy_init();
    if (!curl) {
        result.ok = false;
        return result;
    }
    bool owned_curl = !(od && od->curl);
    curl_easy_reset(curl);

    struct curl_slist *hdrs =
        p->build_headers ? p->build_headers(p, api_key) : openai_build_headers(p, api_key);
    hdrs = curl_slist_append(hdrs, "Accept: text/event-stream");

    oai_sse_state_t state = {0};
    jbuf_init(&state.line_buf, 4096);
    jbuf_init(&state.text_accum, 4096);
    jbuf_init(&state.reasoning_accum, 1024);
    state.text_cb = text_cb;
    state.tool_cb = tool_cb;
    state.thinking_cb = thinking_cb;
    state.cb_ctx = cb_ctx;
    state.telemetry_start = provider_now_sec();

    curl_easy_setopt(curl, CURLOPT_URL, od->api_url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request_json);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, oai_sse_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &state);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, provider_credit_header_cb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &state.credit_reset_at);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
    if (provider_is_sakana(p) || dcr_provider_find(p ? p->name : NULL)) {
        /* Slow multi-agent / imported providers can legitimately stay quiet
         * during reasoning. DCR can carry provider-specific idle policy learned
         * from Codex/provider docs; Sakana defaults to 2h. */
        long idle_ms = dcr_provider_stream_idle_timeout_ms(p ? p->name : NULL,
                                                           provider_is_sakana(p) ? 7200000L : 120000L);
        long idle_s = idle_ms > 0 ? (idle_ms + 999L) / 1000L : 120L;
        const char *idle_env = getenv("DSCO_FUGU_STREAM_IDLE_TIMEOUT_S");
        if (provider_is_sakana(p) && idle_env && idle_env[0]) {
            long v = atol(idle_env);
            if (v > 0)
                idle_s = v;
        }
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, idle_s);
    } else {
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 100L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 120L);
    }

    if (provider_env_truthy(getenv("DSCO_DEBUG_REQUEST")))
        llm_debug_save_request(request_json, 0);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    {
        double dns = 0.0, conn = 0.0, tls = 0.0, ttfb = 0.0, total = 0.0;
        curl_easy_getinfo(curl, CURLINFO_NAMELOOKUP_TIME, &dns);
        curl_easy_getinfo(curl, CURLINFO_CONNECT_TIME, &conn);
        curl_easy_getinfo(curl, CURLINFO_APPCONNECT_TIME, &tls);
        curl_easy_getinfo(curl, CURLINFO_STARTTRANSFER_TIME, &ttfb);
        curl_easy_getinfo(curl, CURLINFO_TOTAL_TIME, &total);
        result.telemetry.latency.dns_ms = dns * 1000.0;
        result.telemetry.latency.connect_ms = conn * 1000.0;
        result.telemetry.latency.tls_ms = tls * 1000.0;
        result.telemetry.latency.ttfb_ms = ttfb * 1000.0;
        result.telemetry.latency.total_ms = total * 1000.0;
    }

    curl_slist_free_all(hdrs);
    if (owned_curl)
        curl_easy_cleanup(curl);

    result.http_status = (int)http_code;

    if (res == CURLE_OK && http_code == 200) {
        result.ok = true;

        /* Finalize every accumulated tool call in index order. */
        for (int i = 0; i < state.tool_slots_used; i++) {
            oai_tool_call_state_t *slot = &state.tool_calls[i];
            if (!slot->used || !slot->tool_name)
                continue;
            int bi = state.block_count++;
            if (bi < MAX_CONTENT_BLOCKS) {
                state.blocks[bi].type = safe_strdup("tool_use");
                state.blocks[bi].tool_name = safe_strdup(slot->tool_name);
                state.blocks[bi].tool_id = safe_strdup(oai_tool_state_id(slot));
                state.blocks[bi].tool_input = safe_strdup(
                    (slot->tool_args.data && slot->tool_args.data[0]) ? slot->tool_args.data
                                                                      : "{}");
            }
        }

        /* Add text block if we accumulated text */
        if (state.text_accum.len > 0) {
            int bi = state.block_count++;
            if (bi < MAX_CONTENT_BLOCKS) {
                state.blocks[bi].type = safe_strdup("text");
                state.blocks[bi].text = safe_strdup(state.text_accum.data);
            }
        }

        /* Reasoning-only turn fallback. Some upstreams (notably OpenRouter
         * routing kimi-k2.x to certain providers) fold the *entire* answer
         * into delta.reasoning with an empty delta.content. That leaves us
         * with no text block and no tool_use — the turn renders nothing,
         * the reasoning is thrown away, and an empty assistant message gets
         * appended to the conversation (which can later 400 on resend).
         * Promote the accumulated reasoning to a text block so the answer is
         * preserved in conv and can be surfaced to the user. */
        /* Preserve reasoning_content across turns for providers (notably
         * Moonshot/Kimi) that require replaying prior reasoning during
         * multi-step tool calling. Keep it as an internal 'thinking' block;
         * request builders may serialize it provider-specifically. */
        if (state.reasoning_accum.len > 0) {
            int bi = state.block_count++;
            state.blocks[bi].type = safe_strdup(state.block_count == 1 ? "text" : "thinking");
            state.blocks[bi].text = safe_strdup(state.reasoning_accum.data);
        }

        /* Build result */
        result.parsed.count = state.block_count;
        result.parsed.blocks =
            safe_malloc((state.block_count > 0 ? state.block_count : 1) * sizeof(content_block_t));
        memcpy(result.parsed.blocks, state.blocks, state.block_count * sizeof(content_block_t));
        result.parsed.stop_reason = state.stop_reason;
        result.usage = state.usage;
        /* BUG2 fix: OpenAI/xAI/DeepSeek/Gemini report cached tokens in
         * state.cached_tokens (input_tokens_details.cached_tokens), not in
         * cache_read_input_tokens. Merge into the canonical field so session
         * cost accounting, turn budget, and UI stats all see the savings. */
        if (state.cached_tokens > 0 && result.usage.cache_read_input_tokens == 0)
            result.usage.cache_read_input_tokens = state.cached_tokens;

        /* Surface provider metadata to caller so it can be printed
         * AFTER md_flush completes (avoids partial-echo duplication). */
        result.actual_model     = state.actual_model;   state.actual_model = NULL;
        result.generation_id    = state.generation_id;  state.generation_id = NULL;
        result.reasoning_tokens = state.reasoning_tokens;

        /* Handle mid-stream errors that arrived on HTTP 200 */
        if (state.got_error) {
            result.ok = false;
            if (state.error_msg)
                fprintf(stderr, "dsco: stream error: %s\n", state.error_msg);
        }
    } else {
        result.ok = false;
        /* Extract a parseable error body from the last partial line (HTTP
         * errors arrive as a JSON blob, not as SSE). This lets us surface
         * a structured, actionable message to the agent layer — critical
         * for the credit-too-low fallback path. */
        if (state.line_buf.len > 0 && !state.error_msg) {
            state.credit_reset_at = provider_reset_max(
                state.credit_reset_at,
                provider_credit_reset_at_from_text(state.line_buf.data, time(NULL)));
            char *err_obj = json_get_raw(state.line_buf.data, "error");
            if (err_obj) {
                state.credit_reset_at = provider_reset_max(
                    state.credit_reset_at,
                    provider_credit_reset_at_from_text(err_obj, time(NULL)));
                char *msg = json_get_str(err_obj, "message");
                if (msg) {
                    state.got_error = true;
                    state.error_msg = msg;
                    if (provider_msg_is_credit_too_low(msg) || http_code == 402 ||
                        http_code == 429)
                        state.credit_too_low = true;
                }
                free(err_obj);
            }
        }
        if (http_code == 402 || http_code == 429)
            state.credit_too_low = true;

        if (res != CURLE_OK) {
            fprintf(stderr, "dsco: curl error: %s (HTTP %d, url: %s)\n", curl_easy_strerror(res),
                    (int)http_code, od->api_url);
        } else if (state.credit_too_low) {
            fprintf(stderr, "  \033[31m✗ %s credit/rate-limit error (HTTP %d):\033[0m %s\n",
                    p->name ? p->name : "provider", (int)http_code,
                    state.error_msg ? state.error_msg : "(no message)");
            if (provider_is_sakana(p)) {
                if (provider_sakana_has_payg_key()) {
                    fprintf(stderr,
                            "  \033[2mhint: Fugu PAYG key is configured; retrying metered "
                            "Sakana before cross-provider fallback.\033[0m\n");
                } else {
                    fprintf(stderr,
                            "  \033[2mhint: set FUGU_PAYG_API_KEY for metered Fugu fallback, "
                            "or switch provider with /model.\033[0m\n");
                }
            } else {
                fprintf(stderr,
                        "  \033[2mhint: switch provider with /model, e.g.\033[0m "
                        "\033[36m/model x-ai/grok-4.20-beta\033[0m \033[2m(needs "
                        "OPENROUTER_API_KEY)\033[0m\n");
            }
        } else if (state.got_error && state.error_msg) {
            fprintf(stderr, "dsco: HTTP %d: %s\n", (int)http_code, state.error_msg);
        } else if (state.line_buf.len > 0) {
            fprintf(stderr, "dsco: HTTP %d: %.*s\n", (int)http_code,
                    (int)(state.line_buf.len < 500 ? state.line_buf.len : 500),
                    state.line_buf.data);
        } else {
            fprintf(stderr, "dsco: request failed HTTP %d (url: %s)\n", (int)http_code,
                    od->api_url);
        }
        free(state.stop_reason);
    }

    /* Propagate credit flag to the agent via a sentinel in stop_reason
     * so the caller can detect it without changing the stream_result_t
     * shape (which would ripple through llm.c and swarm.c). */
    if (state.credit_too_low) {
        free(result.parsed.stop_reason);
        result.parsed.stop_reason = safe_strdup("credit_too_low");
        result.credit_reset_at = state.credit_reset_at;
    }

    double telemetry_end = provider_now_sec();
    result.telemetry.total_ms = (telemetry_end - state.telemetry_start) * 1000.0;
    if (state.telemetry_got_first)
        result.telemetry.ttft_ms = (state.telemetry_first_delta - state.telemetry_start) * 1000.0;
    if (state.telemetry_first_tool > 0.0)
        result.telemetry.ttft_tool_ms =
            (state.telemetry_first_tool - state.telemetry_start) * 1000.0;
    if (result.telemetry.total_ms > 0.0 && result.usage.output_tokens > 0)
        result.telemetry.tokens_per_sec =
            result.usage.output_tokens / (result.telemetry.total_ms / 1000.0);
    result.telemetry.thinking_tokens = state.reasoning_accum.len > 0
                                           ? (int)(state.reasoning_accum.len / 4)
                                           : state.reasoning_tokens;

    /* Cleanup OpenRouter-specific state */
    free(state.error_msg);
    /* state.actual_model / state.generation_id: ownership transferred to result above;
     * pointers were set to NULL, so these free() calls are safe no-ops but kept
     * for structural symmetry. */
    free(state.actual_model);
    free(state.generation_id);
    for (int i = 0; i < state.tool_slots_used; i++) {
        free(state.tool_calls[i].tool_name);
        free(state.tool_calls[i].tool_id);
        jbuf_free(&state.tool_calls[i].tool_args);
    }
    jbuf_free(&state.line_buf);
    jbuf_free(&state.text_accum);
    jbuf_free(&state.reasoning_accum);

    return result;
}

/* ════════════════════════════════════════════════════════════════════════
 * Native ChatGPT-subscription provider (Responses API)
 *
 * Talks directly to the ChatGPT backend Responses endpoint
 * (https://chatgpt.com/backend-api/codex/responses) using a ChatGPT OAuth
 * access token + account id resolved by openai_oauth.c. No `codex` subprocess.
 * ════════════════════════════════════════════════════════════════════════ */

#define CHATGPT_RESPONSES_URL "https://chatgpt.com/backend-api/codex/responses"

static const char *chatgpt_backend_url(void) {
    const char *override = getenv("DSCO_CHATGPT_BASE_URL");
    return (override && override[0]) ? override : CHATGPT_RESPONSES_URL;
}

/* Strip a leading "openai/" or "chatgpt/" route prefix from a model id so the
 * backend sees a bare model name (e.g. "openai/gpt-5.5" -> "gpt-5.5"). */
static const char *chatgpt_model_id(session_state_t *session) {
    const char *m = provider_request_model_id("openai-codex",
                                               session ? session->model
                                                       : codex_cache_default_model());
    if (!m)
        return codex_cache_default_model();
    const char *slash = strrchr(m, '/');
    const char *bare = slash ? slash + 1 : m;
    if (!codex_cache_model_supported(bare))
        return codex_cache_default_model();
    return bare;
}

/* Emit one Responses input "message" item wrapping text/image parts for a
 * role. Returns true if anything was written. */
static bool chatgpt_append_message_item(jbuf_t *b, const char *role, const message_t *m,
                                        bool *first) {
    /* Pre-scan for any renderable content parts. */
    bool has_part = false;
    for (int i = 0; i < m->content_count; i++) {
        const msg_content_t *mc = &m->content[i];
        if (!mc->type)
            continue;
        if ((strcmp(mc->type, "text") == 0 && mc->text && mc->text[0]) ||
            strcmp(mc->type, "image") == 0)
            has_part = true;
    }
    if (!has_part)
        return false;

    const char *text_part_type = (strcmp(role, "assistant") == 0) ? "output_text" : "input_text";
    if (!*first)
        jbuf_append(b, ",");
    *first = false;
    jbuf_append(b, "{\"type\":\"message\",\"role\":");
    jbuf_append_json_str(b, role);
    jbuf_append(b, ",\"content\":[");
    bool wrote = false;
    for (int i = 0; i < m->content_count; i++) {
        const msg_content_t *mc = &m->content[i];
        if (!mc->type)
            continue;
        if (strcmp(mc->type, "text") == 0 && mc->text && mc->text[0]) {
            if (wrote)
                jbuf_append(b, ",");
            jbuf_append(b, "{\"type\":");
            jbuf_append_json_str(b, text_part_type);
            jbuf_append(b, ",\"text\":");
            jbuf_append_json_str(b, mc->text);
            jbuf_append(b, "}");
            wrote = true;
        } else if (strcmp(mc->type, "image") == 0 && mc->image_media_type && mc->text) {
            /* mc->text carries base64 payload for image blocks in dsco's model. */
            if (wrote)
                jbuf_append(b, ",");
            jbuf_append(b, "{\"type\":\"input_image\",\"image_url\":");
            jbuf_t durl;
            jbuf_init(&durl, 256);
            jbuf_append(&durl, "data:");
            jbuf_append(&durl, mc->image_media_type);
            jbuf_append(&durl, ";base64,");
            jbuf_append(&durl, mc->text);
            jbuf_append_json_str(b, durl.data ? durl.data : "");
            jbuf_free(&durl);
            jbuf_append(b, "}");
            wrote = true;
        }
    }
    jbuf_append(b, "]}");
    return true;
}

/* Emit top-level function_call / function_call_output items for a message. */
static void chatgpt_append_tool_items(jbuf_t *b, const message_t *m, bool *first) {
    for (int i = 0; i < m->content_count; i++) {
        const msg_content_t *mc = &m->content[i];
        if (!mc->type)
            continue;
        if (strcmp(mc->type, "tool_use") == 0) {
            if (!*first)
                jbuf_append(b, ",");
            *first = false;
            jbuf_append(b, "{\"type\":\"function_call\",\"name\":");
            jbuf_append_json_str(b, mc->tool_name ? mc->tool_name : "");
            jbuf_append(b, ",\"arguments\":");
            jbuf_append_json_str(b, (mc->tool_input && mc->tool_input[0]) ? mc->tool_input : "{}");
            jbuf_append(b, ",\"call_id\":");
            jbuf_append_json_str(b, mc->tool_id ? mc->tool_id : "");
            jbuf_append(b, "}");
        } else if (strcmp(mc->type, "tool_result") == 0) {
            if (!*first)
                jbuf_append(b, ",");
            *first = false;
            jbuf_append(b, "{\"type\":\"function_call_output\",\"call_id\":");
            jbuf_append_json_str(b, mc->tool_id ? mc->tool_id : "");
            jbuf_append(b, ",\"output\":");
            jbuf_append_json_str(b, mc->text ? mc->text : "");
            jbuf_append(b, "}");
        }
    }
}

/* Emit a flat Responses-format tools array reusing the dsco tool catalog. */
static bool chatgpt_append_tools(jbuf_t *b, conversation_t *conv, session_state_t *session) {
    (void)session;
    const char *disable_tools = getenv("DSCO_OR_DISABLE_TOOLS");
    if (disable_tools && disable_tools[0] && strcmp(disable_tools, "0") != 0 &&
        strcasecmp(disable_tools, "false") != 0)
        return false;

    int filtered_count = 0;
    const tool_def_t **filtered =
        tools_get_filtered(openai_last_user_context(conv), 128, &filtered_count);
    if (filtered_count <= 0 && g_external_tool_count <= 0) {
        free((void *)filtered);
        return false;
    }

    jbuf_append(b, ",\"tools\":[");
    bool wrote = false;
    for (int i = 0; i < filtered_count; i++) {
        if (wrote)
            jbuf_append(b, ",");
        jbuf_append(b, "{\"type\":\"function\",\"name\":");
        jbuf_append_json_str(b, filtered[i]->name ? filtered[i]->name : "");
        jbuf_append(b, ",\"description\":");
        jbuf_append_json_str(b, filtered[i]->description ? filtered[i]->description : "");
        jbuf_append(b, ",\"parameters\":");
        jbuf_append(b, filtered[i]->input_schema_json ? filtered[i]->input_schema_json : "{}");
        jbuf_append(b, ",\"strict\":false}");
        wrote = true;
    }
    free((void *)filtered);

    int ext_written = 0;
    for (int i = 0; i < g_external_tool_count && ext_written < 32; i++) {
        if (!g_external_tools[i].loaded)
            continue;
        if (wrote)
            jbuf_append(b, ",");
        jbuf_append(b, "{\"type\":\"function\",\"name\":");
        jbuf_append_json_str(b, g_external_tools[i].name);
        jbuf_append(b, ",\"description\":");
        jbuf_append_json_str(b, g_external_tools[i].description);
        jbuf_append(b, ",\"parameters\":");
        jbuf_append(b, g_external_tools[i].input_schema_json ? g_external_tools[i].input_schema_json
                                                             : "{}");
        jbuf_append(b, ",\"strict\":false}");
        wrote = true;
        ext_written++;
    }
    jbuf_append(b, "]");
    return wrote;
}

static char *chatgpt_native_build_request(provider_t *p, conversation_t *conv,
                                          session_state_t *session, int max_tokens,
                                          const char *credential) {
    (void)p;
    (void)max_tokens;
    (void)credential;

    jbuf_t b;
    jbuf_init(&b, 16384);
    jbuf_append(&b, "{\"model\":");
    jbuf_append_json_str(&b, chatgpt_model_id(session));

    /* instructions = custom prompt + base system prompt */
    jbuf_t sys;
    jbuf_init(&sys, 8192);
    const char *custom = llm_get_custom_system_prompt();
    if (custom && custom[0]) {
        jbuf_append(&sys, custom);
        jbuf_append(&sys, "\n\n");
    }
    jbuf_append(&sys, SYSTEM_PROMPT);
    jbuf_append(&b, ",\"instructions\":");
    jbuf_append_json_str(&b, sys.data ? sys.data : SYSTEM_PROMPT);
    jbuf_free(&sys);

    jbuf_append(&b, ",\"input\":[");
    bool first = true;
    if (conv) {
        for (int i = 0; i < conv->count; i++) {
            const message_t *m = &conv->msgs[i];
            const char *role = (m->role == ROLE_USER) ? "user" : "assistant";
            /* tool_use (assistant) / tool_result (user) become top-level items;
             * the remaining text/image parts become a message item. */
            chatgpt_append_tool_items(&b, m, &first);
            chatgpt_append_message_item(&b, role, m, &first);
        }
    }
    jbuf_append(&b, "]");

    bool has_tools = chatgpt_append_tools(&b, conv, session);
    if (has_tools) {
        const char *choice = (session && session->tool_choice[0]) ? session->tool_choice : "auto";
        if (strcmp(choice, "none") == 0)
            jbuf_append(&b, ",\"tool_choice\":\"none\"");
        else if (strcmp(choice, "any") == 0 || strcmp(choice, "required") == 0)
            jbuf_append(&b, ",\"tool_choice\":\"required\"");
        else
            jbuf_append(&b, ",\"tool_choice\":\"auto\"");
        jbuf_append(&b, ",\"parallel_tool_calls\":false");
    }

    /* Reasoning effort from the session. gpt-5.x reasoning models honor this. */
    const char *effort = (session && session->effort[0])
        ? session->effort
        : codex_cache_default_effort(chatgpt_model_id(session));
    jbuf_append(&b, ",\"reasoning\":{\"effort\":");
    jbuf_append_json_str(&b, effort);
    jbuf_append(&b, "}");

    jbuf_append(&b, ",\"store\":false,\"stream\":true}");
    return b.data;
}

static struct curl_slist *chatgpt_native_build_headers(provider_t *p, const char *api_key) {
    (void)p;
    struct curl_slist *hdrs = NULL;
    hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
    hdrs = curl_slist_append(hdrs, "OpenAI-Beta: responses=experimental");
    hdrs = curl_slist_append(hdrs, "originator: codex_cli_rs");
    char auth[8300];
    snprintf(auth, sizeof(auth), "Authorization: Bearer %s", api_key ? api_key : "");
    hdrs = curl_slist_append(hdrs, auth);
    char account[160];
    if (openai_oauth_account_id(account + 0, sizeof(account) - 32)) {
        char hdr[200];
        snprintf(hdr, sizeof(hdr), "chatgpt-account-id: %s", account);
        hdrs = curl_slist_append(hdrs, hdr);
    }
    char session_hdr[64];
    char uuid[37];
    uuid_v4(uuid);
    snprintf(session_hdr, sizeof(session_hdr), "session_id: %s", uuid);
    hdrs = curl_slist_append(hdrs, session_hdr);
    return hdrs;
}

/* ── Responses API SSE parser ───────────────────────────────────────────── */
typedef struct {
    jbuf_t line_buf;
    jbuf_t text_accum;
    jbuf_t reasoning_accum;
    stream_text_cb text_cb;
    stream_tool_start_cb tool_cb;
    stream_thinking_cb thinking_cb;
    void *cb_ctx;
    usage_t usage;
    char *stop_reason;
    bool got_error;
    bool credit_too_low;
    char *error_msg;
    time_t credit_reset_at;
    content_block_t tool_blocks[MAX_CONTENT_BLOCKS];
    int tool_block_count;
} chatgpt_sse_state_t;

static void chatgpt_handle_event(chatgpt_sse_state_t *s, const char *data) {
    if (!data || !data[0] || strcmp(data, "[DONE]") == 0)
        return;
    char *type = json_get_str(data, "type");
    if (!type)
        return;

    if (strcmp(type, "response.output_text.delta") == 0) {
        char *delta = json_get_str(data, "delta");
        if (delta && delta[0]) {
            jbuf_append(&s->text_accum, delta);
            if (s->text_cb)
                s->text_cb(delta, s->cb_ctx);
        }
        free(delta);
    } else if (strcmp(type, "response.reasoning_summary_text.delta") == 0 ||
               strcmp(type, "response.reasoning_text.delta") == 0) {
        char *delta = json_get_str(data, "delta");
        if (delta && delta[0]) {
            jbuf_append(&s->reasoning_accum, delta);
            if (s->thinking_cb)
                s->thinking_cb(delta, s->cb_ctx);
        }
        free(delta);
    } else if (strcmp(type, "response.output_item.done") == 0 ||
               strcmp(type, "response.output_item.added") == 0) {
        char *item = json_get_raw(data, "item");
        if (item) {
            char *itype = json_get_str(item, "type");
            if (itype && strcmp(itype, "function_call") == 0 &&
                s->tool_block_count < MAX_CONTENT_BLOCKS) {
                char *name = json_get_str(item, "name");
                char *args = json_get_str(item, "arguments");
                char *call_id = json_get_str(item, "call_id");
                /* Only finalize on .done (added may lack arguments). */
                if (strcmp(type, "response.output_item.done") == 0 && name) {
                    content_block_t *blk = &s->tool_blocks[s->tool_block_count++];
                    memset(blk, 0, sizeof(*blk));
                    blk->type = safe_strdup("tool_use");
                    blk->tool_name = safe_strdup(name);
                    blk->tool_id = safe_strdup((call_id && call_id[0]) ? call_id : name);
                    blk->tool_input = safe_strdup((args && args[0]) ? args : "{}");
                    if (s->tool_cb)
                        s->tool_cb(blk->tool_name, blk->tool_id, s->cb_ctx);
                }
                free(name);
                free(args);
                free(call_id);
            }
            free(itype);
            free(item);
        }
    } else if (strcmp(type, "response.completed") == 0) {
        char *resp = json_get_raw(data, "response");
        if (resp) {
            char *usage = json_get_raw(resp, "usage");
            if (usage) {
                s->usage.input_tokens = json_get_int(usage, "input_tokens", s->usage.input_tokens);
                s->usage.output_tokens =
                    json_get_int(usage, "output_tokens", s->usage.output_tokens);
                char *itd = json_get_raw(usage, "input_tokens_details");
                if (itd) {
                    int c = json_get_int(itd, "cached_tokens", 0);
                    if (c > 0)
                        s->usage.cache_read_input_tokens = c;
                    free(itd);
                }
                free(usage);
            }
            free(resp);
        }
        if (!s->stop_reason)
            s->stop_reason = safe_strdup(s->tool_block_count > 0 ? "tool_use" : "end_turn");
    } else if (strcmp(type, "response.failed") == 0 || strcmp(type, "error") == 0) {
        s->credit_reset_at = provider_reset_max(
            s->credit_reset_at, provider_credit_reset_at_from_text(data, time(NULL)));
        char *resp = json_get_raw(data, "response");
        char *err = json_get_raw(resp ? resp : data, "error");
        if (err) {
            s->credit_reset_at = provider_reset_max(
                s->credit_reset_at, provider_credit_reset_at_from_text(err, time(NULL)));
        }
        char *msg = json_get_str(err ? err : data, "message");
        if (msg) {
            s->got_error = true;
            free(s->error_msg);
            s->error_msg = msg;
            if (provider_msg_is_credit_too_low(msg))
                s->credit_too_low = true;
            fprintf(stderr, "  \033[31mChatGPT backend error: %s\033[0m\n", msg);
        }
        free(err);
        free(resp);
    }
    free(type);
}

static void chatgpt_sse_process_line(chatgpt_sse_state_t *s, const char *line) {
    if (strncmp(line, "data:", 5) != 0)
        return;
    const char *data = line + 5;
    while (*data == ' ')
        data++;
    chatgpt_handle_event(s, data);
}

static size_t chatgpt_sse_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    size_t total = size * nmemb;
    chatgpt_sse_state_t *s = (chatgpt_sse_state_t *)userdata;
    const char *p = (const char *)ptr;
    for (size_t i = 0; i < total; i++) {
        if (p[i] == '\n') {
            if (s->line_buf.len > 0) {
                chatgpt_sse_process_line(s, s->line_buf.data);
                jbuf_reset(&s->line_buf);
            }
        } else if (p[i] != '\r') {
            jbuf_append_char(&s->line_buf, p[i]);
        }
    }
    return total;
}

static stream_result_t chatgpt_native_stream(provider_t *p, const char *api_key,
                                             const char *request_json, stream_text_cb text_cb,
                                             stream_tool_start_cb tool_cb,
                                             stream_thinking_cb thinking_cb, void *cb_ctx) {
    stream_result_t result = {0};
    CURL *curl = curl_easy_init();
    dsco_http_pool_apply(curl);
    if (!curl) {
        result.ok = false;
        return result;
    }

    struct curl_slist *hdrs = chatgpt_native_build_headers(p, api_key);
    hdrs = curl_slist_append(hdrs, "Accept: text/event-stream");

    chatgpt_sse_state_t state = {0};
    jbuf_init(&state.line_buf, 4096);
    jbuf_init(&state.text_accum, 4096);
    jbuf_init(&state.reasoning_accum, 1024);
    state.text_cb = text_cb;
    state.tool_cb = tool_cb;
    state.thinking_cb = thinking_cb;
    state.cb_ctx = cb_ctx;

    const char *url = chatgpt_backend_url();
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request_json);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, chatgpt_sse_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &state);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, provider_credit_header_cb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &state.credit_reset_at);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 120L);
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2TLS);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);
    result.http_status = (int)http_code;

    if (res == CURLE_OK && http_code == 200 && !state.got_error) {
        result.ok = true;
        int n = state.tool_block_count + (state.text_accum.len > 0 ? 1 : 0);
        if (n == 0)
            n = 1;
        result.parsed.blocks = safe_malloc((size_t)n * sizeof(content_block_t));
        int bi = 0;
        for (int i = 0; i < state.tool_block_count; i++)
            result.parsed.blocks[bi++] = state.tool_blocks[i];
        if (state.text_accum.len > 0) {
            memset(&result.parsed.blocks[bi], 0, sizeof(content_block_t));
            result.parsed.blocks[bi].type = safe_strdup("text");
            result.parsed.blocks[bi].text = safe_strdup(state.text_accum.data);
            bi++;
        }
        result.parsed.count = bi;
        result.parsed.stop_reason =
            state.stop_reason ? state.stop_reason
                              : safe_strdup(state.tool_block_count > 0 ? "tool_use" : "end_turn");
        state.stop_reason = NULL;
        result.usage = state.usage;
    } else {
        result.ok = false;
        /* free any collected tool blocks we won't return */
        for (int i = 0; i < state.tool_block_count; i++) {
            free(state.tool_blocks[i].type);
            free(state.tool_blocks[i].tool_name);
            free(state.tool_blocks[i].tool_id);
            free(state.tool_blocks[i].tool_input);
        }
        if (http_code == 401 || http_code == 403) {
            fprintf(stderr,
                    "  \033[31m✗ ChatGPT auth failed (HTTP %ld).\033[0m "
                    "\033[2mRun\033[0m \033[36m/login chatgpt\033[0m\n",
                    http_code);
        } else if (res != CURLE_OK) {
            fprintf(stderr, "dsco: ChatGPT request error: %s (HTTP %ld)\n",
                    curl_easy_strerror(res), http_code);
        } else if (state.error_msg) {
            fprintf(stderr, "dsco: ChatGPT HTTP %ld: %s\n", http_code, state.error_msg);
        } else if (state.line_buf.len > 0) {
            fprintf(stderr, "dsco: ChatGPT HTTP %ld: %.*s\n", http_code,
                    (int)(state.line_buf.len < 500 ? state.line_buf.len : 500),
                    state.line_buf.data);
        } else {
            fprintf(stderr, "dsco: ChatGPT request failed (HTTP %ld)\n", http_code);
        }
        free(state.stop_reason);
    }

    if (state.credit_too_low) {
        free(result.parsed.stop_reason);
        result.parsed.stop_reason = safe_strdup("credit_too_low");
        result.credit_reset_at = state.credit_reset_at;
    }

    free(state.error_msg);
    jbuf_free(&state.line_buf);
    jbuf_free(&state.text_accum);
    jbuf_free(&state.reasoning_accum);
    return result;
}

/* ── Provider endpoint table ───────────────────────────────────────────── */

typedef struct {
    const char *name;       /* provider key */
    const char *base_url;   /* API base URL */
    const char *env_key;    /* API key environment variable */
    const char *key_header; /* header format: "Authorization: Bearer" or "x-api-key:" */
} provider_endpoint_t;

static const provider_endpoint_t PROVIDER_ENDPOINTS[] = {
    {"openai", "https://api.openai.com/v1", "OPENAI_API_KEY", "Bearer"},
    {"google", "https://generativelanguage.googleapis.com/v1beta/openai", "GOOGLE_API_KEY",
     "Bearer"},
    {"groq", "https://api.groq.com/openai/v1", "GROQ_API_KEY", "Bearer"},
    {"deepseek", "https://api.deepseek.com/v1", "DEEPSEEK_API_KEY", "Bearer"},
    {"together", "https://api.together.xyz/v1", "TOGETHER_API_KEY", "Bearer"},
    {"mistral", "https://api.mistral.ai/v1", "MISTRAL_API_KEY", "Bearer"},
    {"openrouter", "https://openrouter.ai/api/v1", "OPENROUTER_API_KEY", "Bearer"},
    {"perplexity", "https://api.perplexity.ai", "PERPLEXITY_API_KEY", "Bearer"},
    {"cerebras", "https://api.cerebras.ai/v1", "CEREBRAS_API_KEY", "Bearer"},
    {"xai", "https://api.x.ai/v1", "XAI_API_KEY", "Bearer"},
    {"cohere", "https://api.cohere.com/v2", "COHERE_API_KEY", "Bearer"},
    {"moonshot", "https://api.moonshot.ai/v1", "KIMI_API_KEY", "Bearer"},
    {"sakana", "https://api.sakana.ai/v1", "FUGU_API_KEY", "Bearer"},
    {"alibaba", "https://dashscope-intl.aliyuncs.com/compatible-mode/v1", "DASHSCOPE_API_KEY",
     "Bearer"},
    {"alibaba-coding-plan", "https://coding-intl.dashscope.aliyuncs.com/v1",
     "ALIBABA_CODING_PLAN_API_KEY", "Bearer"},
    {"arcee", "https://api.arcee.ai/api/v1", "ARCEEAI_API_KEY", "Bearer"},
    {"gmi", "https://api.gmi-serving.com/v1", "GMI_API_KEY", "Bearer"},
    {"huggingface", "https://router.huggingface.co/v1", "HF_TOKEN", "Bearer"},
    {"kilocode", "https://api.kilo.ai/api/gateway", "KILOCODE_API_KEY", "Bearer"},
    {"nous", "https://inference.nousresearch.com/v1", "NOUS_API_KEY", "Bearer"},
    {"novita", "https://api.novita.ai/openai/v1", "NOVITA_API_KEY", "Bearer"},
    {"nvidia", "https://integrate.api.nvidia.com/v1", "NVIDIA_API_KEY", "Bearer"},
    {"ollama-cloud", "https://ollama.com/v1", "OLLAMA_API_KEY", "Bearer"},
    {"opencode-zen", "https://opencode.ai/zen/v1", "OPENCODE_ZEN_API_KEY", "Bearer"},
    {"opencode-go", "https://opencode.ai/zen/go/v1", "OPENCODE_GO_API_KEY", "Bearer"},
    {"qwen-oauth", "https://portal.qwen.ai/v1", "QWEN_API_KEY", "Bearer"},
    {"stepfun", "https://api.stepfun.ai/step_plan/v1", "STEPFUN_API_KEY", "Bearer"},
    {"xiaomi", "https://api.xiaomimimo.com/v1", "XIAOMI_API_KEY", "Bearer"},
    {"zai", "https://api.z.ai/api/paas/v4", "GLM_API_KEY", "Bearer"},
    /* ── Local inference (OpenAI-compatible, no auth required) ─────────── */
    {"mlx", "http://localhost:8181/v1", "MLX_API_KEY", "Bearer"},
    {"ollama", "http://localhost:11434/v1", "OLLAMA_API_KEY", "Bearer"},
    {"lmstudio", "http://localhost:1234/v1", "LMSTUDIO_API_KEY", "Bearer"},
    {"vllm", "http://localhost:8000/v1", "VLLM_API_KEY", "Bearer"},
    {"llamacpp", "http://localhost:8080/v1", "LLAMACPP_API_KEY", "Bearer"},
    {"localai", "http://localhost:8080/v1", "LOCALAI_API_KEY", "Bearer"},
    {"jan", "http://localhost:1337/v1", "JAN_API_KEY", "Bearer"},
    {"gpt4all", "http://localhost:4891/v1", "GPT4ALL_API_KEY", "Bearer"},
    {"koboldcpp", "http://localhost:5001/v1", "KOBOLDCPP_API_KEY", "Bearer"},
    {"textgen", "http://localhost:5000/v1", "TEXTGEN_API_KEY", "Bearer"},
    {"tabby", "http://localhost:5000/v1", "TABBY_API_KEY", "Bearer"},
    {"tgi", "http://localhost:3000/v1", "TGI_API_KEY", "Bearer"},
    {"sglang", "http://localhost:30000/v1", "SGLANG_API_KEY", "Bearer"},
    {"llamafile", "http://localhost:8080/v1", "LLAMAFILE_API_KEY", "Bearer"},
    {"local", "http://localhost:8181/v1", "LOCAL_API_KEY", "Bearer"},
    {NULL, NULL, NULL, NULL}};

static provider_endpoint_t dcr_endpoint_scratch;

static const provider_endpoint_t *find_endpoint(const char *name) {
    name = provider_profile_canonical_name(name);
    const dcr_provider_t *dp = dcr_provider_find(name);
    if (dp && dp->base_url[0]) {
        dcr_endpoint_scratch.name = dp->name;
        dcr_endpoint_scratch.base_url = dp->base_url;
        dcr_endpoint_scratch.env_key = dcr_provider_primary_env_var(dp);
        dcr_endpoint_scratch.key_header = "Bearer";
        return &dcr_endpoint_scratch;
    }
    for (int i = 0; PROVIDER_ENDPOINTS[i].name; i++) {
        if (strcmp(name, PROVIDER_ENDPOINTS[i].name) == 0)
            return &PROVIDER_ENDPOINTS[i];
    }
    return NULL;
}

/* True if a provider name maps to a loopback (self-hosted) OpenAI-compatible
 * endpoint — ollama, lmstudio, mlx, vllm, llamacpp, localai, jan, gpt4all,
 * koboldcpp, textgen, tabby, tgi, sglang, llamafile, local. These are keyless,
 * so they are always "usable" regardless of any API-key environment. */
static bool provider_is_local_endpoint(const char *name) {
    const provider_endpoint_t *ep = find_endpoint(name);
    if (!ep || !ep->base_url)
        return false;
    return strstr(ep->base_url, "localhost") != NULL ||
           strstr(ep->base_url, "127.0.0.1") != NULL || strstr(ep->base_url, "[::1]") != NULL;
}

typedef struct {
    const char *provider_name;
    const char *aliases[8];
} provider_env_alias_t;

static const provider_env_alias_t PROVIDER_ENV_ALIASES[] = {
    {"anthropic", {"CLAUDE_API_KEY", NULL}},
    {"openai", {"OPENAI_KEY", "CHATGPT_API_KEY", NULL}},
    {"openrouter", {"OPEN_ROUTER_API_KEY", NULL}},
    {"together", {"TOGETHER_TOKEN", NULL}},
    {"xai", {"GROK_API_KEY", "X_AI_API_KEY", NULL}},
    {"moonshot", {"KIMI_CODING_API_KEY", "MOONSHOT_API_KEY", "MOONSHOTAI_API_KEY", NULL}},
    {"sakana", {"SAKANA_API_KEY", "FISH_API_KEY", "SAKANA_TOKEN", NULL}},
    {"google",
     {"GEMINI_API_KEY", "GOOGLE_AI_API_KEY", "GOOGLE_AI_STUDIO_API_KEY", "GOOGLE_VERTEX_API_KEY",
      NULL}},
    {NULL, {NULL}}};

static void provider_build_env_name(const char *provider_name, const char *suffix, char *out,
                                    size_t out_len) {
    if (!out || out_len == 0)
        return;
    out[0] = '\0';
    if (!provider_name || !provider_name[0] || !suffix || !suffix[0])
        return;

    size_t pos = 0;
    for (const char *p = provider_name; *p && pos + 1 < out_len; p++) {
        unsigned char ch = (unsigned char)*p;
        if (isalnum(ch)) {
            out[pos++] = (char)toupper(ch);
        } else {
            out[pos++] = '_';
        }
    }
    out[pos] = '\0';
    strncat(out, suffix, out_len - strlen(out) - 1);
}

static bool provider_str_ends_with(const char *s, const char *suffix) {
    if (!s || !suffix)
        return false;
    size_t ls = strlen(s);
    size_t lf = strlen(suffix);
    if (lf > ls)
        return false;
    return strcmp(s + ls - lf, suffix) == 0;
}

static const char *provider_getenv_nonempty(const char *name) {
    if (!name || !name[0])
        return NULL;
    const char *value = getenv(name);
    if (value && value[0])
        return value;
    /* When libsodium is built in, sealed_store_init() interns allowlisted
     * provider keys (e.g. OPENROUTER_API_KEY) and zeroes the environment copy
     * in place. In that case getenv(name) still returns a present-but-empty
     * string, so fall back to the sealed store.
     *
     * Important: if getenv(name) is NULL, the variable was explicitly absent
     * or was unset by a test/child environment. Do NOT resurrect a stale sealed
     * value. Routing tests and subprocess isolation rely on unsetenv() meaning
     * "this provider is unavailable" even if the parent process sealed a real
     * key earlier in the session. */
    if (value) {
        const char *sealed = sealed_store_peek(name);
        return (sealed && sealed[0]) ? sealed : NULL;
    }
    return NULL;
}

static const char *provider_sakana_subscription_key(void) {
    static const char *vars[] = {
        "FUGU_API_KEY", "SAKANA_API_KEY", "FISH_API_KEY", "SAKANA_TOKEN", NULL,
    };
    for (int i = 0; vars[i]; i++) {
        const char *value = provider_getenv_nonempty(vars[i]);
        if (value)
            return value;
    }
    return NULL;
}

static const char *provider_sakana_payg_key(void) {
    static const char *vars[] = {
        "FUGU_PAYG_API_KEY", "SAKANA_PAYG_API_KEY", "FISH_PAYG_API_KEY",
        "SAKANA_PAYG_TOKEN", NULL,
    };
    for (int i = 0; vars[i]; i++) {
        const char *value = provider_getenv_nonempty(vars[i]);
        if (value)
            return value;
    }
    return NULL;
}

static bool provider_sakana_has_subscription_key(void) {
    return provider_sakana_subscription_key() != NULL;
}

static bool provider_sakana_prefer_payg(void) {
    const char *cls = getenv("DSCO_SAKANA_KEY_CLASS");
    if (!cls || !cls[0])
        cls = getenv("DSCO_FUGU_KEY_CLASS");
    if (provider_env_matches(cls, "payg", "metered"))
        return true;
    if (provider_env_matches(cls, "subscription", "sub"))
        return false;
    return provider_env_truthy(getenv("DSCO_PREFER_METERED_API")) ||
           provider_env_truthy(getenv("DSCO_API_BILLING_FALLBACK"));
}

static const char *provider_sakana_resolve_key(void) {
    const char *sub = provider_sakana_subscription_key();
    const char *payg = provider_sakana_payg_key();
    if (provider_sakana_prefer_payg() && payg)
        return payg;
    if (sub)
        return sub;
    return payg;
}

bool provider_sakana_current_key_is_subscription(void) {
    const char *resolved = provider_sakana_resolve_key();
    const char *sub = provider_sakana_subscription_key();
    if (!resolved)
        return true;
    return resolved && sub && strcmp(resolved, sub) == 0;
}

bool provider_sakana_has_payg_key(void) {
    return provider_sakana_payg_key() != NULL;
}

const char *provider_sakana_payg_request_key(void) {
    return provider_sakana_payg_key();
}

static const char *provider_resolve_alias_env_key(const char *provider_name) {
    if (!provider_name || !provider_name[0])
        return NULL;
    const dcr_provider_t *dp = dcr_provider_find(provider_name);
    if (dp) {
        for (int i = 0; i < PROVIDER_PROFILE_MAX_ENV_VARS && dp->env_vars[i][0]; i++) {
            const char *value = provider_getenv_nonempty(dp->env_vars[i]);
            if (value)
                return value;
        }
    }
    for (int i = 0; PROVIDER_ENV_ALIASES[i].provider_name; i++) {
        if (strcmp(PROVIDER_ENV_ALIASES[i].provider_name, provider_name) != 0)
            continue;
        for (int j = 0; PROVIDER_ENV_ALIASES[i].aliases[j]; j++) {
            const char *value = provider_getenv_nonempty(PROVIDER_ENV_ALIASES[i].aliases[j]);
            if (value)
                return value;
        }
        break;
    }
    return NULL;
}

static const char *provider_resolve_dynamic_env_key(const char *provider_name) {
    if (!provider_name || !provider_name[0])
        return NULL;
    char env_name[128];
    const char *suffixes[] = {"_API_KEY", "_ACCESS_TOKEN", "_AUTH_TOKEN", "_TOKEN", NULL};
    for (int i = 0; suffixes[i]; i++) {
        provider_build_env_name(provider_name, suffixes[i], env_name, sizeof(env_name));
        const char *value = provider_getenv_nonempty(env_name);
        if (value)
            return value;
    }
    return NULL;
}

bool provider_has_custom_api_base(const char *provider_name) {
    char env_name[128];
    provider_build_env_name(provider_name, "_API_BASE", env_name, sizeof(env_name));
    if (provider_getenv_nonempty(env_name))
        return true;
    provider_build_env_name(provider_name, "_BASE_URL", env_name, sizeof(env_name));
    if (provider_getenv_nonempty(env_name))
        return true;

    const char *canonical = provider_profile_canonical_name(provider_name);
    if (canonical && strcmp(canonical, "sakana") == 0) {
        const char *fugu_base = getenv("FUGU_BASE_URL");
        const char *fugu_api_base = getenv("FUGU_API_BASE");
        if ((fugu_base && fugu_base[0]) || (fugu_api_base && fugu_api_base[0]))
            return true;
    }
    if (canonical && provider_name && strcmp(canonical, provider_name) != 0) {
        provider_build_env_name(canonical, "_API_BASE", env_name, sizeof(env_name));
        if (provider_getenv_nonempty(env_name))
            return true;
        provider_build_env_name(canonical, "_BASE_URL", env_name, sizeof(env_name));
        if (provider_getenv_nonempty(env_name))
            return true;
    }
    const provider_profile_t *profile = provider_profile_find(provider_name);
    if (profile) {
        for (int i = 0; i < PROVIDER_PROFILE_MAX_ALIASES && profile->aliases[i]; i++) {
            provider_build_env_name(profile->aliases[i], "_API_BASE", env_name, sizeof(env_name));
            if (provider_getenv_nonempty(env_name))
                return true;
            provider_build_env_name(profile->aliases[i], "_BASE_URL", env_name, sizeof(env_name));
            if (provider_getenv_nonempty(env_name))
                return true;
        }
    }
    return false;
}

/* ── Provider factory ──────────────────────────────────────────────────── */

static provider_t *create_openai_compat(const char *name, const char *base_url,
                                        const char *env_key) {
    provider_t *p = safe_malloc(sizeof(provider_t));
    memset(p, 0, sizeof(*p));
    p->name = name;
    openai_data_t *od = safe_malloc(sizeof(openai_data_t));
    memset(od, 0, sizeof(*od));

    /* Check for env override */
    char env_base[128];
    const char *canonical_name = provider_profile_canonical_name(name);
    bool is_fugu = canonical_name && strcmp(canonical_name, "sakana") == 0;
    const char *custom_base = NULL;
    if (is_fugu)
        custom_base = getenv("FUGU_BASE_URL");
    if ((!custom_base || !custom_base[0]) && is_fugu)
        custom_base = getenv("FUGU_API_BASE");
    provider_build_env_name(name, "_API_BASE", env_base, sizeof(env_base));
    if (!custom_base || !custom_base[0])
        custom_base = getenv(env_base);
    if (!custom_base || !custom_base[0]) {
        provider_build_env_name(name, "_BASE_URL", env_base, sizeof(env_base));
        custom_base = getenv(env_base);
    }
    if (!custom_base || !custom_base[0]) {
        const provider_profile_t *profile = provider_profile_find(canonical_name);
        if (profile) {
            for (int i = 0; i < PROVIDER_PROFILE_MAX_ALIASES && profile->aliases[i]; i++) {
                provider_build_env_name(profile->aliases[i], "_API_BASE", env_base,
                                        sizeof(env_base));
                custom_base = getenv(env_base);
                if (custom_base && custom_base[0])
                    break;
                provider_build_env_name(profile->aliases[i], "_BASE_URL", env_base,
                                        sizeof(env_base));
                custom_base = getenv(env_base);
                if (custom_base && custom_base[0])
                    break;
            }
        }
    }
    if (!custom_base)
        custom_base = base_url;

    char normalized_base[1024];
    snprintf(normalized_base, sizeof(normalized_base), "%s", custom_base ? custom_base : "");
    size_t blen = strlen(normalized_base);
    while (blen > 0 && normalized_base[blen - 1] == '/')
        normalized_base[--blen] = '\0';
    if (is_fugu && !provider_str_ends_with(normalized_base, "/v1") &&
        strlen(normalized_base) + 3 < sizeof(normalized_base)) {
        strncat(normalized_base, "/v1", sizeof(normalized_base) - strlen(normalized_base) - 1);
    }

    if (normalized_base[0])
        snprintf(od->api_url, sizeof(od->api_url), "%s/chat/completions", normalized_base);
    else
        od->api_url[0] = '\0';
    p->api_url = od->api_url;
    p->data = od;
    p->build_request = openai_build_request;
    p->build_headers = openai_build_headers;
    p->stream = openai_stream;
    (void)env_key; /* key is resolved at request time via detect */
    return p;
}

static provider_t *create_unsupported_provider(const provider_profile_t *profile) {
    provider_t *p = safe_malloc(sizeof(provider_t));
    memset(p, 0, sizeof(*p));
    p->name = profile && profile->name ? profile->name : "unknown";
    p->api_url = profile && profile->base_url ? profile->base_url : "";
    p->data = safe_strdup(
        provider_transport_kind_name(profile ? profile->transport : PROVIDER_TRANSPORT_NONE));
    p->build_request = unsupported_build_request;
    p->build_headers = unsupported_build_headers;
    p->stream = unsupported_stream;
    return p;
}


/* ── Sakana Fugu Provider ───────────────────────────────────────────────
 *
 * Fugu speaks the OpenAI Chat Completions dialect.  Two quirks:
 *
 * 1. reasoning.effort only accepts "high" and "xhigh" (alias "max").
 *    Any other value (low, medium, auto, etc.) is rejected with a 400.
 *    We remap: anything below "high" → "high"; "max" → "xhigh".
 *
 * 2. Fugu Ultra returns orchestration tokens in token_details sub-fields
 *    (orchestration_input_tokens, orchestration_input_cached_tokens,
 *    orchestration_output_tokens).  Unlike OpenAI these ARE real billing
 *    tokens outside the base input/output counts.  We accumulate them
 *    into the usage struct so cost accounting doesn't under-count.
 *
 * Transport: standard OpenAI SSE streaming over /v1/chat/completions.
 * Auth: FUGU_API_KEY (or SAKANA_API_KEY/FISH_API_KEY/SAKANA_TOKEN) as Bearer token.
 */


/* Remap dsco session effort string to Fugu's two-value enum. */
static const char *fugu_remap_effort(const char *effort) {
    if (!effort || !effort[0])
        return "high";
    if (strcmp(effort, "xhigh") == 0 || strcmp(effort, "max") == 0)
        return "xhigh";
    if (strcmp(effort, "high") == 0)
        return "high";
    /* Sakana rejects low/medium/auto. Normalize locally before request. */
    return "high";
}

static bool provider_is_sakana(const provider_t *p) {
    return p && p->name && strcmp(provider_profile_canonical_name(p->name), "sakana") == 0;
}

static char *fugu_build_request(provider_t *p, conversation_t *conv, session_state_t *session,
                                int max_tokens, const char *credential) {
    char *base = openai_build_request(p, conv, session, max_tokens, credential);
    if (!base) return NULL;

    /* Append reasoning block with remapped effort.
     * Strip trailing '}' from the base request to inject. */
    size_t len = strlen(base);
    if (len == 0 || base[len - 1] != '}') return base;
    base[len - 1] = '\0';

    const char *raw_effort = (session && session->effort[0]) ? session->effort : "high";
    char effort_buf[32];
    const char *effort = dcr_reasoning_effort_normalize("sakana",
                                                        session ? session->model : "fugu",
                                                        raw_effort, effort_buf,
                                                        sizeof(effort_buf));
    if (!effort)
        effort = fugu_remap_effort(raw_effort);

    jbuf_t b;
    jbuf_init(&b, len + 128);
    jbuf_append(&b, base);
    free(base);
    jbuf_append(&b, ",\"reasoning\":{\"effort\":");
    jbuf_append_json_str(&b, effort);
    jbuf_append(&b, "}}");
    return b.data;
}

provider_t *provider_create(const char *name) {
    if (!name || !name[0])
        name = "anthropic";
    const provider_profile_t *profile = provider_profile_find(name);
    name = provider_profile_canonical_name(name);
    if (strcmp(name, "anthropic") == 0 || strcmp(name, "claude") == 0) {
        provider_t *p = safe_malloc(sizeof(provider_t));
        memset(p, 0, sizeof(*p));
        p->name = "anthropic";
        p->api_url = API_URL_ANTHROPIC;
        p->build_request = anthropic_build_request;
        p->build_headers = anthropic_build_headers;
        p->stream = anthropic_stream;
        return p;
    }

    /* OpenRouter gets its own header/request builders */
    if (strcmp(name, "openrouter") == 0) {
        const provider_endpoint_t *ep = find_endpoint("openrouter");
        provider_t *p = create_openai_compat(ep->name, ep->base_url, ep->env_key);
        p->build_headers = openrouter_build_headers;
        p->build_request = openrouter_build_request;
        return p;
    }

    if (strcmp(name, "openai-codex") == 0) {
        provider_t *p = safe_malloc(sizeof(provider_t));
        memset(p, 0, sizeof(*p));
        p->name = "openai-codex";
        /* Prefer the native Responses-API path (dsco-resolved ChatGPT OAuth
         * token, no subprocess). Fall back to the legacy `codex` subprocess
         * only when native auth is unavailable but the codex binary is. */
        if (provider_chatgpt_native_ready()) {
            p->api_url = CHATGPT_RESPONSES_URL;
            p->data = safe_strdup("chatgpt_native");
            p->build_request = chatgpt_native_build_request;
            p->build_headers = chatgpt_native_build_headers;
            p->stream = chatgpt_native_stream;
        } else {
            p->api_url = "codex://exec";
            p->data = safe_strdup("codex_exec");
            p->build_request = codex_exec_build_request;
            p->build_headers = unsupported_build_headers;
            p->stream = codex_exec_stream;
        }
        return p;
    }

    /* Moonshot needs the OpenAI dialect plus the Kimi-native thinking
     * toggle, so it gets its own request builder. */
    if (strcmp(name, "moonshot") == 0) {
        const provider_endpoint_t *ep = find_endpoint("moonshot");
        provider_t *p = create_openai_compat(ep->name, ep->base_url, ep->env_key);
        p->build_request = moonshot_build_request;
        return p;
    }

    /* xAI native: OpenAI dialect + reasoning_effort and Live Search. */
    if (strcmp(name, "xai") == 0 || strcmp(name, "grok") == 0) {
        const provider_endpoint_t *ep = find_endpoint("xai");
        provider_t *p = create_openai_compat(ep->name, ep->base_url, ep->env_key);
        p->build_request = xai_build_request;
        return p;
    }

    /* Sakana Fugu: Chat Completions dialect + restricted reasoning effort. */
    if (strcmp(name, "sakana") == 0 || strcmp(name, "fugu") == 0) {
        const provider_endpoint_t *ep = find_endpoint("sakana");
        if (ep) {
            provider_t *p = create_openai_compat(ep->name, ep->base_url, ep->env_key);
            p->build_request = fugu_build_request;
            return p;
        }
    }

    /* All other providers use OpenAI-compatible API */
    const provider_endpoint_t *ep = find_endpoint(name);
    if (ep) {
        return create_openai_compat(ep->name, ep->base_url, ep->env_key);
    }

    if (profile && profile->transport == PROVIDER_TRANSPORT_OPENAI_CHAT) {
        const char *base = profile->transport_base_url;
        return create_openai_compat(profile->name, base ? base : "",
                                    provider_profile_primary_env_var(profile));
    }

    if (profile)
        return create_unsupported_provider(profile);

    /* Fallback: treat as OpenAI-compatible with custom base */
    return create_openai_compat(name, "https://api.openai.com/v1", "OPENAI_API_KEY");
}

void provider_free(provider_t *p) {
    if (!p)
        return;
    provider_reset_connection(p);
    free(p->data);
    free(p);
}

bool provider_prepare(provider_t *p) {
    if (!p)
        return false;
    if (p->data && p->stream == openai_stream) {
        openai_data_t *od = (openai_data_t *)p->data;
        if (!od->curl) {
            od->curl = curl_easy_init();
            if (!od->curl)
                return false;
        }
        od->prepared = true;
        return true;
    }
    return true;
}

stream_result_t provider_stream_reuse(provider_t *p, const char *api_key, const char *request_json,
                                      stream_text_cb text_cb, stream_tool_start_cb tool_cb,
                                      stream_thinking_cb thinking_cb, void *cb_ctx) {
    stream_result_t result = {0};
    if (!p || !p->stream)
        return result;
    if (p->data && p->stream == openai_stream) {
        (void)provider_prepare(p);
    }
    return p->stream(p, api_key, request_json, text_cb, tool_cb, thinking_cb, cb_ctx);
}

void provider_reset_connection(provider_t *p) {
    if (!p || !p->data)
        return;
    if (p->stream == openai_stream) {
        openai_data_t *od = (openai_data_t *)p->data;
        if (od->curl) {
            curl_easy_cleanup(od->curl);
            od->curl = NULL;
        }
        od->prepared = false;
    }
}

/* ── Provider detection from model name ────────────────────────────────── */

static bool provider_model_has_prefix(const char *model, const char *prefix) {
    return model && prefix && strncmp(model, prefix, strlen(prefix)) == 0;
}

static bool provider_model_has_explicit_openrouter_prefix(const char *model) {
    return model &&
           (provider_model_has_prefix(model, "openrouter:") ||
            provider_model_has_prefix(model, "openrouter/") ||
            strcmp(model, "auto") == 0);
}

static const char *provider_model_strip_explicit_openrouter_prefix(const char *model) {
    if (!model)
        return NULL;
    if (provider_model_has_prefix(model, "openrouter:") ||
        provider_model_has_prefix(model, "openrouter/"))
        return model + 11;
    return model;
}

static const char *provider_model_family_from_namespaced(const char *model) {
    if (!model)
        return NULL;
    if (provider_model_has_prefix(model, "anthropic/"))
        return "anthropic";
    if (provider_model_has_prefix(model, "openai/"))
        return "openai";
    if (provider_model_has_prefix(model, "azure/") ||
        provider_model_has_prefix(model, "azure-foundry/") ||
        provider_model_has_prefix(model, "microsoft/"))
        return "azure-foundry";
    if (provider_model_has_prefix(model, "x-ai/"))
        return "xai";
    if (provider_model_has_prefix(model, "xai/"))
        return "xai";
    if (provider_model_has_prefix(model, "google/"))
        return "google";
    if (provider_model_has_prefix(model, "gemini/"))
        return "google";
    if (provider_model_has_prefix(model, "groq/"))
        return "groq";
    if (provider_model_has_prefix(model, "deepseek/"))
        return "deepseek";
    if (provider_model_has_prefix(model, "together/"))
        return "together";
    if (provider_model_has_prefix(model, "cerebras/"))
        return "cerebras";
    if (provider_model_has_prefix(model, "alibaba/") ||
        provider_model_has_prefix(model, "dashscope/") ||
        provider_model_has_prefix(model, "alibaba-cloud/") ||
        provider_model_has_prefix(model, "qwen-dashscope/"))
        return "alibaba";
    if (provider_model_has_prefix(model, "alibaba-coding-plan/") ||
        provider_model_has_prefix(model, "alibaba-coding/") ||
        provider_model_has_prefix(model, "dashscope-coding/"))
        return "alibaba-coding-plan";
    if (provider_model_has_prefix(model, "qwen/"))
        return "qwen";
    if (provider_model_has_prefix(model, "qwen-oauth/") ||
        provider_model_has_prefix(model, "qwen-cli/") ||
        provider_model_has_prefix(model, "qwen-portal/"))
        return "qwen-oauth";
    if (provider_model_has_prefix(model, "mistral/") ||
        provider_model_has_prefix(model, "mistralai/"))
        return "mistral";
    if (provider_model_has_prefix(model, "moonshot/") ||
        provider_model_has_prefix(model, "moonshotai/"))
        return "moonshot";
    if (provider_model_has_prefix(model, "sakana/") ||
        provider_model_has_prefix(model, "sakana-ai/") ||
        provider_model_has_prefix(model, "sakanaai/"))
        return "sakana";
    if (provider_model_has_prefix(model, "arcee/") ||
        provider_model_has_prefix(model, "arcee-ai/") ||
        provider_model_has_prefix(model, "arceeai/"))
        return "arcee";
    if (provider_model_has_prefix(model, "gmi/") ||
        provider_model_has_prefix(model, "gmi-cloud/") ||
        provider_model_has_prefix(model, "gmicloud/"))
        return "gmi";
    if (provider_model_has_prefix(model, "huggingface/") ||
        provider_model_has_prefix(model, "hugging-face/") ||
        provider_model_has_prefix(model, "hf/") ||
        provider_model_has_prefix(model, "huggingface-hub/"))
        return "huggingface";
    if (provider_model_has_prefix(model, "kilocode/") ||
        provider_model_has_prefix(model, "kilo-code/") ||
        provider_model_has_prefix(model, "kilo/") ||
        provider_model_has_prefix(model, "kilo-gateway/"))
        return "kilocode";
    if (provider_model_has_prefix(model, "nous/") ||
        provider_model_has_prefix(model, "nous-portal/") ||
        provider_model_has_prefix(model, "nousresearch/"))
        return "nous";
    if (provider_model_has_prefix(model, "novita/") ||
        provider_model_has_prefix(model, "novita-ai/") ||
        provider_model_has_prefix(model, "novitaai/"))
        return "novita";
    if (provider_model_has_prefix(model, "nvidia/") ||
        provider_model_has_prefix(model, "nvidia-nim/"))
        return "nvidia";
    if (provider_model_has_prefix(model, "ollama-cloud/") ||
        provider_model_has_prefix(model, "ollama_cloud/"))
        return "ollama-cloud";
    if (provider_model_has_prefix(model, "opencode-zen/") ||
        provider_model_has_prefix(model, "opencode/") ||
        provider_model_has_prefix(model, "opencode_zen/") ||
        provider_model_has_prefix(model, "zen/"))
        return "opencode-zen";
    if (provider_model_has_prefix(model, "opencode-go/") ||
        provider_model_has_prefix(model, "opencode_go/"))
        return "opencode-go";
    if (provider_model_has_prefix(model, "stepfun/") ||
        provider_model_has_prefix(model, "step/") ||
        provider_model_has_prefix(model, "stepfun-coding-plan/"))
        return "stepfun";
    if (provider_model_has_prefix(model, "xiaomi/") ||
        provider_model_has_prefix(model, "mimo/") ||
        provider_model_has_prefix(model, "xiaomi-mimo/"))
        return "xiaomi";
    if (provider_model_has_prefix(model, "cohere/"))
        return "cohere";
    if (provider_model_has_prefix(model, "minimax/"))
        return "minimax";
    if (provider_model_has_prefix(model, "zai/") ||
        provider_model_has_prefix(model, "glm/") ||
        provider_model_has_prefix(model, "zhipu/"))
        return "zai";
    if (provider_model_has_prefix(model, "z-ai/"))
        return "zai";
    if (provider_model_has_prefix(model, "meta-llama/"))
        return "meta";
    if (provider_model_has_prefix(model, "amazon/"))
        return "amazon";
    if (provider_model_has_prefix(model, "perplexity/"))
        return "perplexity";
    return NULL;
}

const char *provider_model_family(const char *model) {
    if (!model || !model[0])
        return "anthropic";

    model = provider_model_strip_explicit_openrouter_prefix(model);
    if (!model || !model[0] || strcmp(model, "auto") == 0)
        return "openrouter";

    const char *namespaced = provider_model_family_from_namespaced(model);
    if (namespaced)
        return namespaced;

    if (strstr(model, "claude") || strstr(model, "opus") || strstr(model, "sonnet") ||
        strstr(model, "haiku"))
        return "anthropic";
    if (strstr(model, "gpt") || strncmp(model, "o1", 2) == 0 || strncmp(model, "o3", 2) == 0 ||
        strncmp(model, "o4", 2) == 0 || strstr(model, "codex") || strstr(model, "chatgpt"))
        return "openai";
    if (strstr(model, "grok") || strstr(model, "Grok"))
        return "xai";
    if (strstr(model, "gemini") || strstr(model, "Gemini") || strstr(model, "gem25") ||
        strstr(model, "gem3"))
        return "google";
    if (strstr(model, "deepseek"))
        return "deepseek";
    if (strstr(model, "Qwen") || strstr(model, "qwen"))
        return "qwen";
    if (strstr(model, "mistral") || strstr(model, "codestral") || strstr(model, "pixtral") ||
        strstr(model, "mixtral"))
        return "mistral";
    if (strstr(model, "kimi") || strstr(model, "moonshot"))
        return "moonshot";
    if (strstr(model, "fugu") || strstr(model, "sakana"))
        return "sakana";
    if (strstr(model, "command"))
        return "cohere";
    if (strstr(model, "sonar") || strstr(model, "pplx"))
        return "perplexity";
    if (strstr(model, "glm"))
        return "zai";
    if (strstr(model, "llama"))
        return "meta";
    if (strstr(model, "nova"))
        return "amazon";
    if (strstr(model, "minimax"))
        return "minimax";

    return "other";
}

static bool provider_model_is_code_oriented(const char *model) {
    if (!model || !model[0])
        return false;
    return strstr(model, "codex") || strstr(model, "code");
}

static const char *provider_xai_primary_model(bool prefer_code) {
    if (provider_has_usable_key("xai", NULL))
        return prefer_code ? "grok-code-fast-1" : "grok-4-fast";
    if (provider_has_usable_key("openrouter", NULL))
        return "openrouter/x-ai/grok-4.20-beta";
    return NULL;
}

static const char *provider_openai_primary_model(bool prefer_code) {
    if (!provider_env_truthy(getenv("DSCO_DISABLE_CODEX_OAUTH_DISCOVERY")) &&
        provider_has_usable_key("openai-codex", NULL)) {
        (void)prefer_code;
        static char model[128];
        snprintf(model, sizeof(model), "openai/%s", codex_cache_default_model());
        return model;
    }
    if (provider_has_usable_key("openrouter", NULL))
        return prefer_code ? "openrouter/openai/gpt-5.3-codex" : "openrouter/openai/gpt-5.4";
    if (provider_has_usable_key("openai", NULL))
        return "gpt-4.1";
    return NULL;
}

static const char *provider_family_primary_model(const char *family, bool prefer_code) {
    if (!family || !family[0])
        return NULL;

    if (strcmp(family, "xai") == 0)
        return provider_xai_primary_model(prefer_code);
    if (strcmp(family, "openai") == 0)
        return provider_openai_primary_model(prefer_code);
    if (strcmp(family, "anthropic") == 0) {
        if (provider_has_usable_key("anthropic", NULL))
            return "claude-sonnet-4-6";
        if (provider_has_usable_key("openrouter", NULL))
            return "openrouter/anthropic/claude-sonnet-4.6";
        return NULL;
    }
    if (strcmp(family, "google") == 0) {
        if (provider_has_usable_key("google", NULL))
            return "gemini-2.5-pro";
        if (provider_has_usable_key("openrouter", NULL))
            return "openrouter/google/gemini-2.5-pro";
        return NULL;
    }
    if (strcmp(family, "deepseek") == 0) {
        if (provider_has_usable_key("openrouter", NULL))
            return "openrouter/deepseek/deepseek-chat";
        if (provider_has_usable_key("deepseek", NULL))
            return "deepseek-chat";
        return NULL;
    }
    if (strcmp(family, "qwen") == 0) {
        if (provider_has_usable_key("openrouter", NULL))
            return "openrouter/qwen/qwen3.5-plus-02-15";
        return NULL;
    }
    if (strcmp(family, "mistral") == 0) {
        if (provider_has_usable_key("mistral", NULL))
            return "mistral-large-latest";
        if (provider_has_usable_key("openrouter", NULL))
            return "openrouter/mistralai/mistral-large-2512";
        return NULL;
    }
    if (strcmp(family, "moonshot") == 0) {
        if (prefer_code) {
            if (provider_has_usable_key("moonshot", NULL))
                return "kimi-k2.7-code";
            if (provider_has_usable_key("openrouter", NULL))
                return "openrouter/moonshotai/kimi-k2.7-code";
            return NULL;
        }
        if (provider_has_usable_key("moonshot", NULL))
            return "kimi-k2.7-code";
        if (provider_has_usable_key("openrouter", NULL))
            return "openrouter/moonshotai/kimi-k2.7-code";
        return NULL;
    }
    if (strcmp(family, "sakana") == 0) {
        /* Sakana/Fugu is native-only and should win the global default only for
         * subscription-class Fugu keys. Metered PAYG keys are additive fallback
         * capacity, not a reason to preempt the GLM/Kimi/OpenRouter defaults. */
        if (provider_sakana_has_subscription_key())
            return "fugu";
        return NULL;
    }
    if (strcmp(family, "cohere") == 0) {
        if (provider_has_usable_key("cohere", NULL))
            return "command-a-03-2025";
        if (provider_has_usable_key("openrouter", NULL))
            return "openrouter/cohere/command-a";
        return NULL;
    }
    if (strcmp(family, "perplexity") == 0) {
        if (provider_has_usable_key("perplexity", NULL))
            return "sonar-pro";
        return NULL;
    }
    if (strcmp(family, "zai") == 0) {
        if (provider_has_usable_key("zai", NULL) || provider_has_usable_key("glm", NULL))
            return "glm-5.2";
        if (provider_has_usable_key("openrouter", NULL))
            return "openrouter/z-ai/glm-5.2";
        return NULL;
    }
    if (strcmp(family, "meta") == 0) {
        if (provider_has_usable_key("openrouter", NULL))
            return "openrouter/meta-llama/llama-4-maverick";
        return NULL;
    }
    if (strcmp(family, "minimax") == 0) {
        if (provider_has_usable_key("openrouter", NULL))
            return "openrouter/minimax/minimax-m2.5";
        return NULL;
    }
    if (strcmp(family, "amazon") == 0) {
        if (provider_has_usable_key("openrouter", NULL))
            return "openrouter/amazon/nova-premier-v1";
        return NULL;
    }

    return NULL;
}

static void provider_append_unique_model(char out_models[][128], int *count, int max_models,
                                         const char *current_model, const char *candidate) {
    if (!out_models || !count || max_models <= 0 || !candidate || !candidate[0])
        return;
    if (*count >= max_models)
        return;

    const char *resolved_candidate = model_resolve_alias(candidate);
    const char *resolved_current = current_model ? model_resolve_alias(current_model) : NULL;
    bool candidate_openrouter = provider_model_has_explicit_openrouter_prefix(candidate);
    bool current_openrouter = provider_model_has_explicit_openrouter_prefix(current_model);
    char stored_candidate[128];

    if (candidate_openrouter) {
        snprintf(stored_candidate, sizeof(stored_candidate), "%s", candidate);
    } else {
        snprintf(stored_candidate, sizeof(stored_candidate), "%s", resolved_candidate);
    }

    if (resolved_current && strcmp(resolved_candidate, resolved_current) == 0 &&
        candidate_openrouter == current_openrouter)
        return;

    for (int i = 0; i < *count; i++) {
        const char *resolved_existing = model_resolve_alias(out_models[i]);
        bool existing_openrouter = provider_model_has_explicit_openrouter_prefix(out_models[i]);
        if (strcmp(out_models[i], stored_candidate) == 0 ||
            (strcmp(resolved_existing, resolved_candidate) == 0 &&
             existing_openrouter == candidate_openrouter))
            return;
    }

    snprintf(out_models[*count], 128, "%s", stored_candidate);
    (*count)++;
}

static const char *provider_openai_fallback_model(bool prefer_code) {
    if (provider_has_usable_key("openrouter", NULL))
        return prefer_code ? "openrouter/openai/gpt-5.3-codex" : "openrouter/openai/gpt-5.4";
    return provider_openai_primary_model(prefer_code);
}

int provider_build_default_fallback_models(const char *model, char out_models[][128],
                                           int max_models) {
    if (!out_models || max_models <= 0)
        return 0;
    for (int i = 0; i < max_models; i++)
        out_models[i][0] = '\0';

    const char *requested_model = model ? model : DEFAULT_MODEL;
    const char *resolved_model = model_resolve_alias(requested_model);
    const char *family = provider_model_family(resolved_model);
    bool prefer_code = provider_model_is_code_oriented(resolved_model);
    int count = 0;

    if (strcmp(family, "anthropic") == 0) {
        provider_append_unique_model(out_models, &count, max_models, requested_model,
                                     provider_family_primary_model("anthropic", false));
        provider_append_unique_model(out_models, &count, max_models, requested_model,
                                     provider_xai_primary_model(prefer_code));
        provider_append_unique_model(out_models, &count, max_models, requested_model,
                                     provider_family_primary_model("sakana", prefer_code));
        provider_append_unique_model(out_models, &count, max_models, requested_model,
                                     provider_openai_fallback_model(prefer_code));
        provider_append_unique_model(out_models, &count, max_models, requested_model,
                                     provider_family_primary_model("google", false));
        provider_append_unique_model(out_models, &count, max_models, requested_model,
                                     provider_family_primary_model("deepseek", false));
        return count;
    }

    if (strcmp(family, "openai") == 0) {
        provider_append_unique_model(out_models, &count, max_models, requested_model,
                                     provider_openai_primary_model(prefer_code));
        provider_append_unique_model(out_models, &count, max_models, requested_model,
                                     provider_xai_primary_model(true));
        provider_append_unique_model(out_models, &count, max_models, requested_model,
                                     provider_family_primary_model("sakana", prefer_code));
        provider_append_unique_model(out_models, &count, max_models, requested_model,
                                     provider_family_primary_model("anthropic", false));
        provider_append_unique_model(out_models, &count, max_models, requested_model,
                                     provider_family_primary_model("google", false));
        provider_append_unique_model(out_models, &count, max_models, requested_model,
                                     provider_family_primary_model("deepseek", false));
        return count;
    }

    if (strcmp(family, "xai") == 0) {
        provider_append_unique_model(out_models, &count, max_models, requested_model,
                                     provider_xai_primary_model(prefer_code));
        provider_append_unique_model(out_models, &count, max_models, requested_model,
                                     provider_family_primary_model("anthropic", false));
        provider_append_unique_model(out_models, &count, max_models, requested_model,
                                     provider_family_primary_model("sakana", prefer_code));
        provider_append_unique_model(out_models, &count, max_models, requested_model,
                                     provider_openai_fallback_model(prefer_code));
        provider_append_unique_model(out_models, &count, max_models, requested_model,
                                     provider_family_primary_model("google", false));
        provider_append_unique_model(out_models, &count, max_models, requested_model,
                                     provider_family_primary_model("deepseek", false));
        return count;
    }

    provider_append_unique_model(out_models, &count, max_models, requested_model,
                                 provider_xai_primary_model(prefer_code));
    provider_append_unique_model(out_models, &count, max_models, requested_model,
                                 provider_family_primary_model("anthropic", false));
    provider_append_unique_model(out_models, &count, max_models, requested_model,
                                 provider_family_primary_model("sakana", prefer_code));
    provider_append_unique_model(out_models, &count, max_models, requested_model,
                                 provider_openai_fallback_model(prefer_code));
    provider_append_unique_model(out_models, &count, max_models, requested_model,
                                 provider_family_primary_model("google", false));
    provider_append_unique_model(out_models, &count, max_models, requested_model,
                                 provider_family_primary_model("deepseek", false));
    provider_append_unique_model(out_models, &count, max_models, requested_model,
                                 provider_family_primary_model("qwen", false));
    return count;
}

const char *provider_select_default_primary_model(bool prefer_code) {
    const char *candidate = NULL;

    candidate = provider_family_primary_model("sakana", prefer_code);
    if (candidate)
        return candidate;

    /* A direct native Moonshot subscription/API key is stronger than an
     * OpenRouter code-default route. Keep OpenRouter as the normal code default
     * below so a mere router key still selects the Kimi route. */
    if (prefer_code && provider_has_usable_key("moonshot", NULL)) {
        candidate = provider_family_primary_model("moonshot", true);
        if (candidate)
            return candidate;
    }
    if (!prefer_code &&
        (provider_has_usable_key("zai", NULL) || provider_has_usable_key("glm", NULL))) {
        candidate = provider_family_primary_model("zai", false);
        if (candidate)
            return candidate;
    }

    if (prefer_code) {
        /* Code mode: prefer Kimi K2.7 Code via OpenRouter */
        candidate = provider_family_primary_model("moonshot", true);
        if (candidate)
            return candidate;
    } else {
        /* General mode: prefer GLM (Z.AI) via OpenRouter for cost/quality */
        candidate = provider_family_primary_model("zai", false);
        if (candidate)
            return candidate;
    }

    if (!prefer_code) {
        candidate = provider_xai_primary_model(false);
        if (candidate)
            return candidate;
    } else {
        candidate = provider_openai_primary_model(true);
        if (candidate)
            return candidate;
    }

    candidate = provider_family_primary_model("anthropic", false);
    if (candidate)
        return candidate;

    candidate = provider_xai_primary_model(prefer_code);
    if (candidate)
        return candidate;

    candidate = provider_openai_primary_model(prefer_code);
    if (candidate)
        return candidate;

    candidate = provider_family_primary_model("google", false);
    if (candidate)
        return candidate;

    candidate = provider_family_primary_model("deepseek", false);
    if (candidate)
        return candidate;

    candidate = provider_family_primary_model("mistral", false);
    if (candidate)
        return candidate;

    if (!prefer_code) {
        candidate = provider_family_primary_model("moonshot", false);
        if (candidate)
            return candidate;
    }

    return DEFAULT_MODEL;
}

const char *provider_detect(const char *model, const char *api_key) {
    if (!model && !api_key)
        return "anthropic";

    if (model) {
        /* Explicit provider prefix: "openrouter:model/id" */
        if (provider_model_has_explicit_openrouter_prefix(model))
            return "openrouter";
        /* Generic "<provider>:model" prefix for self-hosted / local backends
         * (e.g. "vllm:Qwen2.5-Coder", "ollama:llama3.3:latest"). The model id
         * itself may contain colons, so only the first segment is the
         * provider. Matches any known OpenAI-compatible endpoint. */
        {
            const char *colon = strchr(model, ':');
            if (colon && colon > model) {
                size_t plen = (size_t)(colon - model);
                char pfx[32];
                if (plen < sizeof(pfx)) {
                    memcpy(pfx, model, plen);
                    pfx[plen] = '\0';
                    /* Return the endpoint table's own (static) name string, not
                     * the stack-local pfx — canonical_name would echo pfx back
                     * and we'd return a dangling pointer. */
                    const provider_endpoint_t *ep = find_endpoint(pfx);
                    if (ep)
                        return ep->name;
                }
            }
        }
        const char *namespaced = provider_model_family_from_namespaced(model);
        if (namespaced)
            return namespaced;
        /* Unknown org/model IDs are OpenRouter slugs unless explicitly claimed
         * by a native provider namespace above. */
        if (strstr(model, "/"))
            return "openrouter";
        /* Anthropic — bare model IDs only (no slash) */
        if (strstr(model, "claude") || strstr(model, "opus") || strstr(model, "sonnet") ||
            strstr(model, "haiku"))
            return "anthropic";
        /* Cerebras provider-prefixed models should beat generic family matches */
        if (strstr(model, "cerebras"))
            return "cerebras";
        /* Moonshot native — bare kimi-* / moonshot-* IDs. */
        if (strstr(model, "kimi") || strstr(model, "moonshot"))
            return "moonshot";
        if (strstr(model, "fugu") || strstr(model, "sakana"))
            return "sakana";
        /* Google Gemini native — bare gemini-* IDs only */
        if (strstr(model, "gemini") || strstr(model, "Gemini"))
            return "google";
        /* OpenAI — bare model IDs only */
        if (strstr(model, "gpt") || strncmp(model, "o1", 2) == 0 || strncmp(model, "o3", 2) == 0 ||
            strncmp(model, "o4", 2) == 0 || strstr(model, "codex-spark") ||
            strstr(model, "chatgpt"))
            return "openai";
        /* Groq — only when no slash (native model IDs have no org prefix) */
        if (!strstr(model, "/") &&
            (strstr(model, "llama") || strstr(model, "mixtral") || strstr(model, "gemma")))
            return "groq";
        /* DeepSeek native */
        if (!strstr(model, "/") && strstr(model, "deepseek"))
            return "deepseek";
        /* Mistral native */
        if (!strstr(model, "/") &&
            (strstr(model, "mistral") || strstr(model, "codestral") || strstr(model, "pixtral")))
            return "mistral";
        /* Together native */
        if (!strstr(model, "/") &&
            (strstr(model, "Qwen") || strstr(model, "qwen") || strstr(model, "together")))
            return "together";
        /* Cohere */
        if (strstr(model, "command"))
            return "cohere";
        /* xAI — bare "grok" IDs route natively; x-ai/... is handled by the
         * known namespace block above. Covers grok-4, grok-4-fast, grok-3,
         * grok-3-mini, grok-code-fast-1. */
        if ((strstr(model, "grok") || strstr(model, "Grok")) && !strstr(model, "/"))
            return "xai";
        /* Perplexity */
        if (strstr(model, "sonar") || strstr(model, "pplx"))
            return "perplexity";
        /* Any remaining slash-based model IDs already caught above */
    }

    /* Check API key patterns */
    if (api_key) {
        if (strncmp(api_key, "sk-ant-", 7) == 0)
            return "anthropic";
        if (strncmp(api_key, "gsk_", 4) == 0)
            return "groq";
        if (strncmp(api_key, "sk-or-", 6) == 0)
            return "openrouter";
        if (strncmp(api_key, "pplx-", 5) == 0)
            return "perplexity";
        if (strncmp(api_key, "xai-", 4) == 0)
            return "xai";
        if (strncmp(api_key, "fish_", 5) == 0)
            return "sakana";
        if (strncmp(api_key, "sk-", 3) == 0)
            return "openai";
    }

    return "anthropic";
}

const char *provider_provider_for_api_key(const char *api_key) {
    if (!api_key || !api_key[0])
        return NULL;
    if (strncmp(api_key, "sk-ant-", 7) == 0)
        return "anthropic";
    if (strncmp(api_key, "gsk_", 4) == 0)
        return "groq";
    if (strncmp(api_key, "sk-or-", 6) == 0)
        return "openrouter";
    if (strncmp(api_key, "pplx-", 5) == 0)
        return "perplexity";
    if (strncmp(api_key, "xai-", 4) == 0)
        return "xai";
    if (strncmp(api_key, "fish_", 5) == 0)
        return "sakana";
    if (strncmp(api_key, "sk-", 3) == 0)
        return "openai";
    return NULL;
}

const char *provider_publish_api_key_env(const char *api_key) {
    const char *prov = provider_provider_for_api_key(api_key);
    if (!prov)
        return NULL;
    const char *env_key = NULL;
    if (strcmp(prov, "anthropic") == 0) {
        env_key = "ANTHROPIC_API_KEY";
    } else {
        const provider_endpoint_t *ep = find_endpoint(prov);
        if (ep)
            env_key = ep->env_key;
    }
    if (env_key && env_key[0] && !provider_getenv_nonempty(env_key))
        setenv(env_key, api_key, 1);
    return prov;
}

const char *provider_primary_model_for(const char *family, bool prefer_code) {
    return provider_family_primary_model(family, prefer_code);
}

/* ── Resolve API key for a provider ────────────────────────────────────── */

static const char *provider_resolve_profile_env_key(const provider_profile_t *profile) {
    if (!profile)
        return NULL;
    for (int i = 0; i < PROVIDER_PROFILE_MAX_ENV_VARS && profile->env_vars[i]; i++) {
        const char *key = provider_getenv_nonempty(profile->env_vars[i]);
        if (key)
            return key;
    }
    return NULL;
}

const char *provider_resolve_api_key(const char *provider_name) {
    if (!provider_name || !provider_name[0])
        return NULL;
    const provider_profile_t *profile = provider_profile_find(provider_name);
    provider_name = provider_profile_canonical_name(provider_name);

    if (strcmp(provider_name, "anthropic") == 0) {
        const char *oauth = provider_resolve_claude_code_oauth_token(false);
        if (oauth && oauth[0])
            return oauth;
        const char *key = provider_resolve_profile_env_key(profile);
        if (key && key[0])
            return key;
        return provider_resolve_alias_env_key("anthropic");
    }

    if (strcmp(provider_name, "sakana") == 0)
        return provider_sakana_resolve_key();

    const provider_endpoint_t *ep = find_endpoint(provider_name);
    if (ep) {
        const char *key = provider_getenv_nonempty(ep->env_key);
        if (key)
            return key;
        key = provider_resolve_profile_env_key(profile);
        if (key)
            return key;
        key = provider_resolve_alias_env_key(provider_name);
        if (key)
            return key;
        return provider_resolve_dynamic_env_key(provider_name);
    }

    const char *key = provider_resolve_profile_env_key(profile);
    if (key)
        return key;
    return provider_resolve_dynamic_env_key(provider_name);
}

static bool provider_key_matches(const char *provider_name, const char *fallback_api_key) {
    if (!provider_name || !provider_name[0] || !fallback_api_key || !fallback_api_key[0])
        return false;
    provider_name = provider_profile_canonical_name(provider_name);
    return strcmp(provider_detect(NULL, fallback_api_key), provider_name) == 0;
}

bool provider_has_usable_key(const char *provider_name, const char *fallback_api_key) {
    if (!provider_name || !provider_name[0])
        return false;
    provider_name = provider_profile_canonical_name(provider_name);

    if (strcmp(provider_name, "openai-codex") == 0)
        return provider_chatgpt_subscription_ready();

    /* Local/self-hosted OpenAI-compatible servers need no API key. */
    if (provider_is_local_endpoint(provider_name))
        return true;

    const char *native_key = provider_resolve_api_key(provider_name);
    if (native_key && native_key[0])
        return true;

    return provider_key_matches(provider_name, fallback_api_key);
}

const char *provider_route_for_model(const char *model, const char *fallback_api_key,
                                     const char *provider_override) {
    if (provider_override && provider_override[0])
        return provider_profile_canonical_name(provider_override);

    const char *provider_name = provider_detect(model, fallback_api_key);
    bool explicit_native_namespace =
        model && !provider_model_has_explicit_openrouter_prefix(model) &&
        provider_model_family_from_namespaced(model) != NULL;

    /* Redirect to openai-codex subscription only when discovery is not
     * explicitly suppressed. DSCO_DISABLE_CODEX_OAUTH_DISCOVERY=1 means
     * "use direct API keys instead of any subscription path". */
    if (strcmp(provider_name, "openai") == 0 &&
        !provider_env_truthy(getenv("DSCO_DISABLE_CODEX_OAUTH_DISCOVERY")) &&
        provider_has_usable_key("openai-codex", NULL) &&
        codex_cache_model_supported(model))
        return "openai-codex";

    if (provider_has_usable_key(provider_name, fallback_api_key))
        return provider_name;

    if (explicit_native_namespace)
        return provider_name;

    if (strcmp(provider_name, "sakana") == 0)
        return provider_name;

    if (strcmp(provider_name, "openrouter") != 0 &&
        provider_has_usable_key("openrouter", fallback_api_key))
        return "openrouter";

    return provider_name;
}

const char *provider_resolve_request_api_key(const char *provider_name,
                                             const char *fallback_api_key) {
    if (!provider_name || !provider_name[0])
        return fallback_api_key;
    provider_name = provider_profile_canonical_name(provider_name);

    if (strcmp(provider_name, "anthropic") == 0) {
        const char *oauth = provider_resolve_claude_code_oauth_token(true);
        if (oauth && oauth[0])
            return oauth;
    }

    if (strcmp(provider_name, "openai-codex") == 0)
        return provider_codex_subscription_credential();

    if (provider_is_local_endpoint(provider_name)) {
        if (fallback_api_key && fallback_api_key[0])
            return fallback_api_key;
        return "local";
    }

    const char *native_key = provider_resolve_api_key(provider_name);
    if (provider_key_matches(provider_name, fallback_api_key))
        return fallback_api_key;

    if (native_key && native_key[0])
        return native_key;

    return NULL;
}

const char *provider_claude_code_oauth_source(void) {
    const char *env = getenv("DSCO_CLAUDE_CODE_OAUTH_TOKEN");
    if (env && env[0])
        return "env";
    env = getenv("CLAUDE_CODE_OAUTH_TOKEN");
    if (env && env[0])
        return "env";

    if (provider_env_truthy(getenv("DSCO_DISABLE_CLAUDE_CODE_OAUTH_DISCOVERY")))
        return "disabled";

    claude_code_oauth_bundle_t bundle;
    if (!provider_load_claude_code_oauth_bundle(&bundle))
        return "missing";

    const char *source = provider_claude_code_oauth_source_name(bundle.source);
    provider_claude_code_oauth_bundle_free(&bundle);
    return source;
}

bool provider_claude_code_get_account_info(char *subscription_type_out, size_t st_len,
                                           char *rate_limit_tier_out, size_t rl_len) {
    if (subscription_type_out && st_len)
        subscription_type_out[0] = '\0';
    if (rate_limit_tier_out && rl_len)
        rate_limit_tier_out[0] = '\0';

    claude_code_oauth_bundle_t bundle;
    if (!provider_load_claude_code_oauth_bundle(&bundle))
        return false;

    bool found = false;
    if (bundle.oauth_json) {
        if (subscription_type_out && st_len) {
            char *v = json_get_str(bundle.oauth_json, "subscriptionType");
            if (v && v[0]) {
                snprintf(subscription_type_out, st_len, "%s", v);
                found = true;
            }
            free(v);
        }
        if (rate_limit_tier_out && rl_len) {
            char *v = json_get_str(bundle.oauth_json, "rateLimitTier");
            if (v && v[0]) {
                snprintf(rate_limit_tier_out, rl_len, "%s", v);
                found = true;
            }
            free(v);
        }
    }
    provider_claude_code_oauth_bundle_free(&bundle);
    return found;
}

void provider_export_child_process_credentials_for_provider(const char *provider_name,
                                                            const char *resolved_key) {
    if (!resolved_key || !resolved_key[0])
        return;
    if (!provider_name || !provider_name[0])
        provider_name = provider_detect(NULL, resolved_key);
    const provider_profile_t *profile = provider_profile_find(provider_name);
    provider_name = provider_profile_canonical_name(provider_name);

    if (strcmp(provider_name, "anthropic") == 0) {
        if (llm_anthropic_uses_claude_code_auth(resolved_key)) {
            unsetenv("ANTHROPIC_API_KEY");
            setenv("DSCO_CLAUDE_CODE_OAUTH_TOKEN", resolved_key, 1);
            setenv("CLAUDE_CODE_OAUTH_TOKEN", resolved_key, 1);
        } else {
            setenv("ANTHROPIC_API_KEY", resolved_key, 1);
            unsetenv("DSCO_CLAUDE_CODE_OAUTH_TOKEN");
            unsetenv("CLAUDE_CODE_OAUTH_TOKEN");
        }
        return;
    }

    if (strcmp(provider_name, "openai-codex") == 0) {
        if (strcmp(resolved_key, "chatgpt-subscription") != 0) {
            setenv("DSCO_CHATGPT_OAUTH_TOKEN", resolved_key, 1);
            setenv("CHATGPT_OAUTH_TOKEN", resolved_key, 1);
        }
        return;
    }

    const provider_endpoint_t *ep = find_endpoint(provider_name);
    if (ep && ep->env_key && ep->env_key[0])
        setenv(ep->env_key, resolved_key, 1);
    else {
        const char *env_key = provider_profile_primary_env_var(profile);
        if (env_key && env_key[0])
            setenv(env_key, resolved_key, 1);
    }
}

void provider_export_child_process_credentials(const char *model, const char *resolved_key) {
    const char *provider_name = NULL;
    if (resolved_key && resolved_key[0])
        provider_name = provider_detect(NULL, resolved_key);
    if ((!provider_name || !provider_name[0]) && model && model[0])
        provider_name = provider_detect(model, NULL);
    provider_export_child_process_credentials_for_provider(provider_name, resolved_key);
}

bool provider_model_is_routable(const char *model, const char *fallback_api_key,
                                const char *provider_override, const char **out_provider_name) {
    const char *provider_name =
        provider_route_for_model(model, fallback_api_key, provider_override);
    if (out_provider_name)
        *out_provider_name = provider_name;
    return provider_has_usable_key(provider_name, fallback_api_key);
}
