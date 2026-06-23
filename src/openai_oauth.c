/* Native ChatGPT-subscription OAuth (Codex-compatible PKCE flow). */
#include "openai_oauth.h"

#include "crypto.h"
#include "json_util.h"

#include <arpa/inet.h>
#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* ── OAuth endpoint configuration (Codex CLI public client) ─────────────── */
#define OAI_OAUTH_ISSUER       "https://auth.openai.com"
#define OAI_OAUTH_AUTHORIZE    "https://auth.openai.com/oauth/authorize"
#define OAI_OAUTH_TOKEN_URL    "https://auth.openai.com/oauth/token"
#define OAI_OAUTH_CLIENT_ID    "app_EMoamEEZ73f0CkXaXp7hrann"
#define OAI_OAUTH_REDIRECT     "http://localhost:1455/auth/callback"
#define OAI_OAUTH_PORT         1455
#define OAI_OAUTH_SCOPE        "openid profile email offline_access"
#define OAI_OAUTH_EXPIRY_BUFFER_MS (5LL * 60LL * 1000LL)

static const char *oai_env(const char *name, const char *fallback) {
    const char *v = getenv(name);
    return (v && v[0]) ? v : fallback;
}

static long long oai_now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000LL + tv.tv_usec / 1000LL;
}

/* ── HTTP plumbing (small response buffer) ──────────────────────────────── */
typedef struct {
    char *data;
    size_t len;
} oai_http_buf_t;

static size_t oai_http_write_cb(char *ptr, size_t size, size_t nmemb, void *ud) {
    size_t total = size * nmemb;
    oai_http_buf_t *b = (oai_http_buf_t *)ud;
    char *grown = realloc(b->data, b->len + total + 1);
    if (!grown)
        return 0;
    b->data = grown;
    memcpy(b->data + b->len, ptr, total);
    b->len += total;
    b->data[b->len] = '\0';
    return total;
}

/* POST application/x-www-form-urlencoded. Returns malloc'd body or NULL.
 * http_code_out receives the response status. */
static char *oai_http_post_form(const char *url, const char *form, long *http_code_out) {
    CURL *curl = curl_easy_init();
    if (!curl)
        return NULL;
    oai_http_buf_t buf = {0};
    struct curl_slist *hdrs = NULL;
    hdrs = curl_slist_append(hdrs, "Content-Type: application/x-www-form-urlencoded");
    hdrs = curl_slist_append(hdrs, "Accept: application/json");
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, form);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, oai_http_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2TLS);
    CURLcode res = curl_easy_perform(curl);
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);
    if (http_code_out)
        *http_code_out = code;
    if (res != CURLE_OK) {
        free(buf.data);
        return NULL;
    }
    return buf.data;
}

/* ── PKCE ───────────────────────────────────────────────────────────────── */
static void oai_pkce_verifier(char out[128]) {
    uint8_t raw[48];
    crypto_random_bytes(raw, sizeof(raw));
    base64url_encode(raw, sizeof(raw), out, 128);
}

static void oai_pkce_challenge(const char *verifier, char out[64]) {
    uint8_t hash[32];
    sha256_ctx_t ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, (const uint8_t *)verifier, strlen(verifier));
    sha256_final(&ctx, hash);
    base64url_encode(hash, sizeof(hash), out, 64);
}

/* ── id_token (JWT) account-id extraction ───────────────────────────────── */
static void oai_extract_account_id(const char *id_token, char *out, size_t out_len) {
    out[0] = '\0';
    if (!id_token || !id_token[0])
        return;
    /* payload = segment between the two dots */
    const char *first = strchr(id_token, '.');
    if (!first)
        return;
    const char *second = strchr(first + 1, '.');
    if (!second)
        return;
    size_t payload_len = (size_t)(second - (first + 1));
    if (payload_len == 0 || payload_len > 16384)
        return;
    uint8_t *decoded = malloc(payload_len + 4);
    if (!decoded)
        return;
    size_t n = base64url_decode(first + 1, payload_len, decoded, payload_len + 4);
    if (n == 0) {
        free(decoded);
        return;
    }
    decoded[n] = '\0';
    char *acct = json_get_str((const char *)decoded, "chatgpt_account_id");
    if (acct) {
        snprintf(out, out_len, "%s", acct);
        free(acct);
    }
    free(decoded);
}

/* ── Cache file paths ───────────────────────────────────────────────────── */
static bool oai_dsco_cache_path(char *out, size_t out_len) {
    const char *home = getenv("HOME");
    if (!home || !home[0])
        return false;
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/.dsco", home);
    mkdir(dir, 0700);
    snprintf(out, out_len, "%s/.dsco/chatgpt-oauth.json", home);
    return true;
}

static bool oai_codex_auth_path(char *out, size_t out_len) {
    /* Codex honors CODEX_HOME (defaults to ~/.codex) per the OpenAI docs. */
    const char *codex_home = getenv("CODEX_HOME");
    if (codex_home && codex_home[0]) {
        snprintf(out, out_len, "%s/auth.json", codex_home);
        return true;
    }
    const char *home = getenv("HOME");
    if (!home || !home[0])
        return false;
    snprintf(out, out_len, "%s/.codex/auth.json", home);
    return true;
}

static char *oai_read_file(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp)
        return NULL;
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    if (sz < 0 || sz > 1024 * 1024) {
        fclose(fp);
        return NULL;
    }
    rewind(fp);
    char *buf = malloc((size_t)sz + 1);
    if (!buf) {
        fclose(fp);
        return NULL;
    }
    size_t got = fread(buf, 1, (size_t)sz, fp);
    fclose(fp);
    buf[got] = '\0';
    return buf;
}

static void oai_json_append_str(FILE *fp, const char *s) {
    fputc('"', fp);
    for (const char *p = s; p && *p; p++) {
        if (*p == '"' || *p == '\\')
            fputc('\\', fp);
        fputc(*p, fp);
    }
    fputc('"', fp);
}

static bool oai_write_cache(const openai_oauth_bundle_t *b) {
    char path[1024];
    if (!oai_dsco_cache_path(path, sizeof(path)))
        return false;
    FILE *fp = fopen(path, "wb");
    if (!fp)
        return false;
    fchmod(fileno(fp), 0600);
    fputs("{\n  \"access_token\": ", fp);
    oai_json_append_str(fp, b->access_token);
    fputs(",\n  \"refresh_token\": ", fp);
    oai_json_append_str(fp, b->refresh_token);
    fputs(",\n  \"id_token\": ", fp);
    oai_json_append_str(fp, b->id_token);
    fputs(",\n  \"account_id\": ", fp);
    oai_json_append_str(fp, b->account_id);
    fprintf(fp, ",\n  \"expires_at_ms\": %lld\n}\n", b->expires_at_ms);
    fclose(fp);
    return true;
}

/* ── Bundle loaders ─────────────────────────────────────────────────────── */
static bool oai_load_from_env(openai_oauth_bundle_t *out) {
    const char *tok = getenv("DSCO_CHATGPT_OAUTH_TOKEN");
    if (!tok || !tok[0])
        tok = getenv("CHATGPT_OAUTH_TOKEN");
    if (!tok || !tok[0])
        return false;
    memset(out, 0, sizeof(*out));
    out->source = OPENAI_OAUTH_SOURCE_ENV;
    snprintf(out->access_token, sizeof(out->access_token), "%s", tok);
    const char *acct = getenv("DSCO_CHATGPT_ACCOUNT_ID");
    if (acct && acct[0])
        snprintf(out->account_id, sizeof(out->account_id), "%s", acct);
    return true;
}

static bool oai_load_from_dsco_cache(openai_oauth_bundle_t *out) {
    char path[1024];
    if (!oai_dsco_cache_path(path, sizeof(path)))
        return false;
    char *json = oai_read_file(path);
    if (!json)
        return false;
    memset(out, 0, sizeof(*out));
    out->source = OPENAI_OAUTH_SOURCE_DSCO_CACHE;
    char *access = json_get_str(json, "access_token");
    char *refresh = json_get_str(json, "refresh_token");
    char *idt = json_get_str(json, "id_token");
    char *acct = json_get_str(json, "account_id");
    out->expires_at_ms = (long long)json_get_int(json, "expires_at_ms", 0);
    if (access)
        snprintf(out->access_token, sizeof(out->access_token), "%s", access);
    if (refresh)
        snprintf(out->refresh_token, sizeof(out->refresh_token), "%s", refresh);
    if (idt)
        snprintf(out->id_token, sizeof(out->id_token), "%s", idt);
    if (acct)
        snprintf(out->account_id, sizeof(out->account_id), "%s", acct);
    free(access);
    free(refresh);
    free(idt);
    free(acct);
    free(json);
    return out->access_token[0] != '\0';
}

static bool oai_load_from_codex(openai_oauth_bundle_t *out) {
    char path[1024];
    if (!oai_codex_auth_path(path, sizeof(path)))
        return false;
    char *json = oai_read_file(path);
    if (!json)
        return false;
    char *tokens = json_get_raw(json, "tokens");
    const char *scope = tokens ? tokens : json;
    memset(out, 0, sizeof(*out));
    out->source = OPENAI_OAUTH_SOURCE_CODEX;
    char *access = json_get_str(scope, "access_token");
    char *refresh = json_get_str(scope, "refresh_token");
    char *idt = json_get_str(scope, "id_token");
    char *acct = json_get_str(scope, "account_id");
    if (access)
        snprintf(out->access_token, sizeof(out->access_token), "%s", access);
    if (refresh)
        snprintf(out->refresh_token, sizeof(out->refresh_token), "%s", refresh);
    if (idt)
        snprintf(out->id_token, sizeof(out->id_token), "%s", idt);
    if (acct && acct[0])
        snprintf(out->account_id, sizeof(out->account_id), "%s", acct);
    else if (out->id_token[0])
        oai_extract_account_id(out->id_token, out->account_id, sizeof(out->account_id));
    free(access);
    free(refresh);
    free(idt);
    free(acct);
    free(tokens);
    free(json);
    return out->access_token[0] != '\0';
}

bool openai_oauth_load(openai_oauth_bundle_t *out) {
    if (!out)
        return false;
    if (oai_load_from_env(out))
        return true;
    if (oai_load_from_dsco_cache(out))
        return true;
    if (oai_load_from_codex(out))
        return true;
    memset(out, 0, sizeof(*out));
    out->source = OPENAI_OAUTH_SOURCE_MISSING;
    return false;
}

/* ── Token exchange / refresh ───────────────────────────────────────────── */
static bool oai_apply_token_response(openai_oauth_bundle_t *b, const char *json) {
    char *access = json_get_str(json, "access_token");
    char *refresh = json_get_str(json, "refresh_token");
    char *idt = json_get_str(json, "id_token");
    int expires_in = json_get_int(json, "expires_in", 0);
    bool ok = access && access[0];
    if (ok) {
        snprintf(b->access_token, sizeof(b->access_token), "%s", access);
        if (refresh && refresh[0])
            snprintf(b->refresh_token, sizeof(b->refresh_token), "%s", refresh);
        if (idt && idt[0]) {
            snprintf(b->id_token, sizeof(b->id_token), "%s", idt);
            char acct[128];
            oai_extract_account_id(idt, acct, sizeof(acct));
            if (acct[0])
                snprintf(b->account_id, sizeof(b->account_id), "%s", acct);
        }
        if (expires_in > 0)
            b->expires_at_ms = oai_now_ms() + (long long)expires_in * 1000LL;
    }
    free(access);
    free(refresh);
    free(idt);
    return ok;
}

bool openai_oauth_refresh(openai_oauth_bundle_t *bundle) {
    if (!bundle || !bundle->refresh_token[0])
        return false;
    if (bundle->source == OPENAI_OAUTH_SOURCE_ENV)
        return false;
    const char *client_id = oai_env("DSCO_CHATGPT_OAUTH_CLIENT_ID", OAI_OAUTH_CLIENT_ID);
    const char *token_url = oai_env("DSCO_CHATGPT_OAUTH_TOKEN_URL", OAI_OAUTH_TOKEN_URL);

    CURL *esc = curl_easy_init();
    char *enc_refresh = esc ? curl_easy_escape(esc, bundle->refresh_token, 0) : NULL;
    char *enc_client = esc ? curl_easy_escape(esc, client_id, 0) : NULL;
    char *enc_scope = esc ? curl_easy_escape(esc, OAI_OAUTH_SCOPE, 0) : NULL;
    char form[16384];
    snprintf(form, sizeof(form),
             "grant_type=refresh_token&refresh_token=%s&client_id=%s&scope=%s",
             enc_refresh ? enc_refresh : "", enc_client ? enc_client : "",
             enc_scope ? enc_scope : "");
    if (enc_refresh)
        curl_free(enc_refresh);
    if (enc_client)
        curl_free(enc_client);
    if (enc_scope)
        curl_free(enc_scope);
    if (esc)
        curl_easy_cleanup(esc);

    long code = 0;
    char *resp = oai_http_post_form(token_url, form, &code);
    if (!resp || code != 200) {
        free(resp);
        return false;
    }
    bool ok = oai_apply_token_response(bundle, resp);
    free(resp);
    if (ok)
        (void)oai_write_cache(bundle);
    return ok;
}

static bool oai_expired(long long expires_at_ms) {
    if (expires_at_ms <= 0)
        return false; /* unknown — assume valid, let the API 401 if not */
    return oai_now_ms() + OAI_OAUTH_EXPIRY_BUFFER_MS >= expires_at_ms;
}

const char *openai_oauth_access_token(bool allow_refresh) {
    static char token[8192];
    token[0] = '\0';
    openai_oauth_bundle_t b;
    if (!openai_oauth_load(&b))
        return NULL;
    if (allow_refresh && b.source != OPENAI_OAUTH_SOURCE_ENV && b.refresh_token[0] &&
        oai_expired(b.expires_at_ms)) {
        (void)openai_oauth_refresh(&b);
    }
    if (!b.access_token[0])
        return NULL;
    snprintf(token, sizeof(token), "%s", b.access_token);
    return token;
}

bool openai_oauth_account_id(char *out, size_t out_len) {
    if (!out || out_len == 0)
        return false;
    out[0] = '\0';
    openai_oauth_bundle_t b;
    if (!openai_oauth_load(&b))
        return false;
    if (!b.account_id[0] && b.id_token[0])
        oai_extract_account_id(b.id_token, b.account_id, sizeof(b.account_id));
    if (!b.account_id[0])
        return false;
    snprintf(out, out_len, "%s", b.account_id);
    return true;
}

bool openai_oauth_available(void) {
    openai_oauth_bundle_t b;
    return openai_oauth_load(&b);
}

const char *openai_oauth_source_name(void) {
    openai_oauth_bundle_t b;
    if (!openai_oauth_load(&b))
        return "missing";
    switch (b.source) {
    case OPENAI_OAUTH_SOURCE_ENV:
        return "env";
    case OPENAI_OAUTH_SOURCE_DSCO_CACHE:
        return "dsco-cache";
    case OPENAI_OAUTH_SOURCE_CODEX:
        return "codex";
    default:
        return "missing";
    }
}

int openai_oauth_logout(void) {
    char path[1024];
    if (!oai_dsco_cache_path(path, sizeof(path)))
        return -1;
    if (unlink(path) != 0 && errno != ENOENT)
        return -1;
    return 0;
}

/* ── Browser + loopback callback server ─────────────────────────────────── */
static void oai_open_browser(const char *url) {
#if defined(__APPLE__)
    const char *opener = "open";
#else
    const char *opener = "xdg-open";
#endif
    /* Quote-safe enough: the URL contains no shell metacharacters beyond &,?,=
     * which we neutralise by wrapping in single quotes (URL has no single
     * quotes). */
    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "%s '%s' >/dev/null 2>&1 &", opener, url);
    int rc = system(cmd);
    (void)rc;
}

/* Wait for a single GET request on the loopback socket, parse code+state out
 * of the request line, send a friendly HTML page, and close. Returns malloc'd
 * code on success (caller frees), and fills state_out. */
static char *oai_wait_for_callback(int listen_fd, char *state_out, size_t state_len,
                                   int timeout_secs) {
    time_t deadline = time(NULL) + timeout_secs;
    char *code = NULL;
    while (!code && time(NULL) < deadline) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(listen_fd, &rfds);
        struct timeval tv = {1, 0};
        int sel = select(listen_fd + 1, &rfds, NULL, NULL, &tv);
        if (sel <= 0)
            continue;
        int client = accept(listen_fd, NULL, NULL);
        if (client < 0)
            continue;
        char req[8192];
        ssize_t n = recv(client, req, sizeof(req) - 1, 0);
        if (n <= 0) {
            close(client);
            continue;
        }
        req[n] = '\0';
        /* request line: GET /auth/callback?code=...&state=... HTTP/1.1 */
        char *q = strstr(req, "code=");
        if (q) {
            q += 5;
            size_t i = 0;
            char *buf = malloc(2048);
            while (q[i] && q[i] != '&' && q[i] != ' ' && i < 2047) {
                buf[i] = q[i];
                i++;
            }
            buf[i] = '\0';
            code = buf;
        }
        char *st = strstr(req, "state=");
        if (st && state_out) {
            st += 6;
            size_t i = 0;
            while (st[i] && st[i] != '&' && st[i] != ' ' && i < state_len - 1) {
                state_out[i] = st[i];
                i++;
            }
            state_out[i] = '\0';
        }
        const char *ok_body =
            "<html><head><title>dsco</title></head>"
            "<body style='font-family:-apple-system,sans-serif;background:#0b0b0e;"
            "color:#e6e6e6;text-align:center;padding-top:18vh'>"
            "<h2>\xe2\x9c\x93 ChatGPT account linked to dsco</h2>"
            "<p style='color:#9aa'>You can close this tab and return to the terminal.</p>"
            "</body></html>";
        char resp[1024];
        int rl = snprintf(resp, sizeof(resp),
                          "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n"
                          "Content-Length: %zu\r\nConnection: close\r\n\r\n",
                          strlen(ok_body));
        (void)(write(client, resp, (size_t)rl) + write(client, ok_body, strlen(ok_body)));
        close(client);
    }
    return code;
}

int openai_oauth_login(void) {
    const char *client_id = oai_env("DSCO_CHATGPT_OAUTH_CLIENT_ID", OAI_OAUTH_CLIENT_ID);
    const char *authorize = oai_env("DSCO_CHATGPT_OAUTH_AUTHORIZE_URL", OAI_OAUTH_AUTHORIZE);
    const char *token_url = oai_env("DSCO_CHATGPT_OAUTH_TOKEN_URL", OAI_OAUTH_TOKEN_URL);

    /* 1. PKCE + state */
    char verifier[128], challenge[64], state[64];
    oai_pkce_verifier(verifier);
    oai_pkce_challenge(verifier, challenge);
    uint8_t state_raw[16];
    crypto_random_bytes(state_raw, sizeof(state_raw));
    base64url_encode(state_raw, sizeof(state_raw), state, sizeof(state));

    /* 2. loopback listener on 127.0.0.1:1455 */
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        fprintf(stderr, "dsco: login: socket() failed: %s\n", strerror(errno));
        return -1;
    }
    int one = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(0x7f000001); /* 127.0.0.1 (INADDR_LOOPBACK) */
    addr.sin_port = htons(OAI_OAUTH_PORT);
    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        fprintf(stderr,
                "dsco: login: cannot bind 127.0.0.1:%d (%s).\n"
                "      Is another login or a codex session already running?\n",
                OAI_OAUTH_PORT, strerror(errno));
        close(listen_fd);
        return -1;
    }
    if (listen(listen_fd, 1) != 0) {
        close(listen_fd);
        return -1;
    }

    /* 3. authorize URL */
    CURL *esc = curl_easy_init();
    char *e_client = curl_easy_escape(esc, client_id, 0);
    char *e_redirect = curl_easy_escape(esc, OAI_OAUTH_REDIRECT, 0);
    char *e_scope = curl_easy_escape(esc, OAI_OAUTH_SCOPE, 0);
    char *e_challenge = curl_easy_escape(esc, challenge, 0);
    char *e_state = curl_easy_escape(esc, state, 0);
    char url[4096];
    snprintf(url, sizeof(url),
             "%s?response_type=code&client_id=%s&redirect_uri=%s&scope=%s"
             "&code_challenge=%s&code_challenge_method=S256&state=%s"
             "&id_token_add_organizations=true&codex_cli_simplified_flow=true",
             authorize, e_client ? e_client : "", e_redirect ? e_redirect : "",
             e_scope ? e_scope : "", e_challenge ? e_challenge : "", e_state ? e_state : "");
    curl_free(e_client);
    curl_free(e_redirect);
    curl_free(e_scope);
    curl_free(e_challenge);
    curl_free(e_state);
    if (esc)
        curl_easy_cleanup(esc);

    fprintf(stderr, "\n  Opening your browser to sign in with ChatGPT…\n");
    fprintf(stderr, "  If it doesn't open, paste this URL:\n\n  %s\n\n", url);
    oai_open_browser(url);

    /* 4. wait for redirect */
    char got_state[64] = {0};
    char *code = oai_wait_for_callback(listen_fd, got_state, sizeof(got_state), 300);
    close(listen_fd);
    if (!code) {
        fprintf(stderr, "dsco: login timed out or was cancelled.\n");
        return -1;
    }
    if (got_state[0] && strcmp(got_state, state) != 0) {
        /* State mismatch can happen when the browser completes a previous
         * flow after dsco restarted (new state generated, old callback
         * lands).  Since the real token path is ~/.codex/auth.json, check
         * that first before giving up. */
        openai_oauth_bundle_t existing;
        if (openai_oauth_load(&existing) && existing.access_token[0]) {
            fprintf(stderr,
                    "  \033[32m✓ Signed in\033[0m (token loaded from %s).\n\n",
                    openai_oauth_source_name());
            free(code);
            return 0;
        }
        /* No existing token — warn but proceed with the exchange anyway;
         * the PKCE verifier still protects the code. */
        fprintf(stderr,
                "dsco: login: state mismatch — proceeding with token exchange.\n");
    }

    /* 5. exchange code (urldecode the code first) */
    char *decoded_code = NULL;
    {
        CURL *d = curl_easy_init();
        int outlen = 0;
        char *u = d ? curl_easy_unescape(d, code, 0, &outlen) : NULL;
        if (u) {
            decoded_code = strdup(u);
            curl_free(u);
        }
        if (d)
            curl_easy_cleanup(d);
    }
    free(code);
    const char *use_code = decoded_code ? decoded_code : "";

    CURL *e2 = curl_easy_init();
    char *enc_code = curl_easy_escape(e2, use_code, 0);
    char *enc_redirect = curl_easy_escape(e2, OAI_OAUTH_REDIRECT, 0);
    char *enc_client = curl_easy_escape(e2, client_id, 0);
    char *enc_verifier = curl_easy_escape(e2, verifier, 0);
    char form[8192];
    snprintf(form, sizeof(form),
             "grant_type=authorization_code&code=%s&redirect_uri=%s"
             "&client_id=%s&code_verifier=%s",
             enc_code ? enc_code : "", enc_redirect ? enc_redirect : "",
             enc_client ? enc_client : "", enc_verifier ? enc_verifier : "");
    curl_free(enc_code);
    curl_free(enc_redirect);
    curl_free(enc_client);
    curl_free(enc_verifier);
    if (e2)
        curl_easy_cleanup(e2);
    free(decoded_code);

    long http_code = 0;
    char *resp = oai_http_post_form(token_url, form, &http_code);
    if (!resp || http_code != 200) {
        fprintf(stderr, "dsco: token exchange failed (HTTP %ld)%s%s\n", http_code,
                resp ? ": " : "", resp ? resp : "");
        free(resp);
        return -1;
    }

    openai_oauth_bundle_t bundle;
    memset(&bundle, 0, sizeof(bundle));
    bundle.source = OPENAI_OAUTH_SOURCE_DSCO_CACHE;
    bool ok = oai_apply_token_response(&bundle, resp);
    free(resp);
    if (!ok) {
        fprintf(stderr, "dsco: token response had no access_token.\n");
        return -1;
    }
    if (!oai_write_cache(&bundle)) {
        fprintf(stderr, "dsco: warning: could not persist token cache.\n");
    }
    fprintf(stderr, "  \033[32m✓ Signed in.\033[0m ChatGPT account%s%s linked.\n\n",
            bundle.account_id[0] ? " " : "", bundle.account_id);
    return 0;
}
