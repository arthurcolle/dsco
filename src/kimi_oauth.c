#include "kimi_oauth.h"
#include "http_pool.h"
#include "json_util.h"

#include <curl/curl.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define KIMI_TOKEN_MAX 8192
#define KIMI_REFRESH_SKEW_S 120
#define KIMI_OAUTH_CLIENT_ID "17e5f671-d194-4dfb-9706-5516cb48c098"
#define KIMI_OAUTH_HOST "https://auth.kimi.com"

static char g_token[KIMI_TOKEN_MAX];
static time_t g_mtime;
static ino_t g_inode;
static long long g_expires_at_s;
static pthread_mutex_t g_token_mutex = PTHREAD_MUTEX_INITIALIZER;
static _Thread_local char s_return_token[KIMI_TOKEN_MAX];

typedef struct {
    char *data;
    size_t len;
} kimi_http_buf_t;

static void kimi_secure_free(char *value) {
    if (!value)
        return;
    memset(value, 0, strlen(value));
    free(value);
}

static void kimi_cache_reset(void) {
    memset(g_token, 0, sizeof(g_token));
    g_mtime = 0;
    g_inode = 0;
    g_expires_at_s = 0;
}

static const char *kimi_env_token(void) {
    const char *token = getenv("DSCO_KIMI_CODE_OAUTH_TOKEN");
    if (!token || !token[0])
        token = getenv("KIMI_CODE_OAUTH_TOKEN");
    return token && token[0] ? token : NULL;
}

static bool kimi_credentials_path(char *out, size_t len) {
    const char *home = getenv("KIMI_CODE_HOME");
    char base[1024];
    if (home && home[0]) snprintf(base, sizeof(base), "%s", home);
    else {
        home = getenv("HOME");
        if (!home || !home[0]) return false;
        snprintf(base, sizeof(base), "%s/.kimi-code", home);
    }
    return snprintf(out, len, "%s/credentials/kimi-code.json", base) > 0;
}

static bool kimi_load(void) {
    char path[1200];
    if (!kimi_credentials_path(path, sizeof(path))) return false;
    struct stat st;
    if (stat(path, &st) != 0 || (st.st_mode & 077) != 0) return false;
    if (g_token[0] && st.st_mtime == g_mtime && st.st_ino == g_inode) return true;
    FILE *fp = fopen(path, "rb");
    if (!fp) return false;
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return false; }
    long n = ftell(fp);
    if (n <= 0 || n > 65536 || fseek(fp, 0, SEEK_SET) != 0) { fclose(fp); return false; }
    char *json = malloc((size_t)n + 1);
    if (!json) { fclose(fp); return false; }
    size_t got = fread(json, 1, (size_t)n, fp); fclose(fp); json[got] = '\0';
    char *token = json_get_str(json, "access_token");
    long long expires = (long long)json_get_double(json, "expires_at", 0);
    free(json);
    if (!token || !token[0] || strlen(token) >= sizeof(g_token)) { free(token); return false; }
    snprintf(g_token, sizeof(g_token), "%s", token); free(token);
    g_expires_at_s = expires; /* Kimi stores epoch seconds. */
    g_mtime = st.st_mtime;
    g_inode = st.st_ino;
    return true;
}

static char *kimi_read_file(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp)
        return NULL;
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return NULL;
    }
    long n = ftell(fp);
    if (n <= 0 || n > 65536 || fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return NULL;
    }
    char *json = malloc((size_t)n + 1);
    if (!json) {
        fclose(fp);
        return NULL;
    }
    size_t got = fread(json, 1, (size_t)n, fp);
    fclose(fp);
    json[got] = '\0';
    return json;
}

static size_t kimi_http_write_cb(char *ptr, size_t size, size_t nmemb, void *ctx) {
    size_t total = size * nmemb;
    kimi_http_buf_t *buf = ctx;
    if (!buf || total > 65536 || buf->len + total > 65536)
        return 0;
    char *grown = realloc(buf->data, buf->len + total + 1);
    if (!grown)
        return 0;
    buf->data = grown;
    memcpy(buf->data + buf->len, ptr, total);
    buf->len += total;
    buf->data[buf->len] = '\0';
    return total;
}

static char *kimi_token_url(char *out, size_t len) {
    const char *explicit_url = getenv("DSCO_KIMI_CODE_OAUTH_TOKEN_URL");
    if (explicit_url && explicit_url[0]) {
        snprintf(out, len, "%s", explicit_url);
        return out;
    }
    const char *host = getenv("KIMI_CODE_OAUTH_HOST");
    if (!host || !host[0])
        host = getenv("KIMI_OAUTH_HOST");
    if (!host || !host[0])
        host = KIMI_OAUTH_HOST;
    size_t host_len = strlen(host);
    snprintf(out, len, "%s%sapi/oauth/token", host,
             host_len > 0 && host[host_len - 1] == '/' ? "" : "/");
    return out;
}

static char *kimi_http_refresh(const char *refresh_token) {
    if (!refresh_token || !refresh_token[0])
        return NULL;
    const char *client_id = getenv("DSCO_KIMI_CODE_OAUTH_CLIENT_ID");
    if (!client_id || !client_id[0])
        client_id = KIMI_OAUTH_CLIENT_ID;

    CURL *curl = curl_easy_init();
    if (!curl)
        return NULL;
    dsco_http_pool_apply(curl);
    char *encoded_refresh = curl_easy_escape(curl, refresh_token, 0);
    char *encoded_client = curl_easy_escape(curl, client_id, 0);
    if (!encoded_refresh || !encoded_client) {
        if (encoded_refresh)
            curl_free(encoded_refresh);
        if (encoded_client)
            curl_free(encoded_client);
        curl_easy_cleanup(curl);
        return NULL;
    }

    char form[20000];
    snprintf(form, sizeof(form),
             "grant_type=refresh_token&refresh_token=%s&client_id=%s",
             encoded_refresh, encoded_client);
    memset(encoded_refresh, 0, strlen(encoded_refresh));
    curl_free(encoded_refresh);
    curl_free(encoded_client);

    char url[1400];
    kimi_token_url(url, sizeof(url));
    kimi_http_buf_t response = {0};
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/x-www-form-urlencoded");
    headers = curl_slist_append(headers, "Accept: application/json");
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, form);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, kimi_http_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2TLS);
    CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    memset(form, 0, sizeof(form));
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (rc != CURLE_OK || status != 200) {
        kimi_secure_free(response.data);
        return NULL;
    }
    return response.data;
}

static bool kimi_write_credentials(const char *path, const char *access_token,
                                   const char *refresh_token, long long expires_at,
                                   int expires_in, const char *scope,
                                   const char *token_type) {
    jbuf_t json;
    jbuf_init(&json, 4096);
    jbuf_append(&json, "{\n  \"access_token\": ");
    jbuf_append_json_str(&json, access_token);
    jbuf_append(&json, ",\n  \"refresh_token\": ");
    jbuf_append_json_str(&json, refresh_token);
    jbuf_append(&json, ",\n  \"expires_at\": ");
    char expires_buf[32];
    snprintf(expires_buf, sizeof(expires_buf), "%lld", expires_at);
    jbuf_append(&json, expires_buf);
    jbuf_append(&json, ",\n  \"scope\": ");
    jbuf_append_json_str(&json, scope && scope[0] ? scope : "kimi-code");
    jbuf_append(&json, ",\n  \"token_type\": ");
    jbuf_append_json_str(&json, token_type && token_type[0] ? token_type : "Bearer");
    jbuf_append(&json, ",\n  \"expires_in\": ");
    jbuf_append_int(&json, expires_in);
    jbuf_append(&json, "\n}\n");

    char tmp[1500];
    snprintf(tmp, sizeof(tmp), "%s.tmp.XXXXXX", path);
    int fd = mkstemp(tmp);
    bool ok = fd >= 0;
    if (ok)
        ok = fchmod(fd, 0600) == 0;
    size_t written = 0;
    while (ok && written < json.len) {
        ssize_t n = write(fd, json.data + written, json.len - written);
        if (n <= 0)
            ok = false;
        else
            written += (size_t)n;
    }
    if (ok)
        ok = fsync(fd) == 0;
    if (fd >= 0 && close(fd) != 0)
        ok = false;
    if (ok)
        ok = rename(tmp, path) == 0;
    if (!ok)
        unlink(tmp);
    if (json.data)
        memset(json.data, 0, json.len);
    jbuf_free(&json);
    return ok;
}

static bool kimi_refresh_locked(const char *rejected_token) {
    char path[1200];
    if (!kimi_credentials_path(path, sizeof(path)))
        return false;
    char lock_path[1300];
    snprintf(lock_path, sizeof(lock_path), "%s.lock", path);
    int lock_fd = open(lock_path, O_CREAT | O_RDWR, 0600);
    struct flock lock = {.l_type = F_WRLCK, .l_whence = SEEK_SET};
    if (lock_fd < 0 || fcntl(lock_fd, F_SETLKW, &lock) != 0) {
        if (lock_fd >= 0)
            close(lock_fd);
        return false;
    }

    char rejected[KIMI_TOKEN_MAX] = "";
    if (rejected_token)
        snprintf(rejected, sizeof(rejected), "%s", rejected_token);
    char *json = kimi_read_file(path);
    char *access = json ? json_get_str(json, "access_token") : NULL;
    char *refresh = json ? json_get_str(json, "refresh_token") : NULL;
    char *old_scope = json ? json_get_str(json, "scope") : NULL;
    char *old_type = json ? json_get_str(json, "token_type") : NULL;
    long long expires_at = json ? (long long)json_get_double(json, "expires_at", 0) : 0;
    bool another_process_refreshed = rejected[0] && access && access[0] &&
                                     strcmp(access, rejected) != 0 &&
                                     expires_at > (long long)time(NULL) + KIMI_REFRESH_SKEW_S;
    bool ok = another_process_refreshed;
    if (!ok && refresh && refresh[0]) {
        char *response = kimi_http_refresh(refresh);
        char *new_access = response ? json_get_str(response, "access_token") : NULL;
        char *new_refresh = response ? json_get_str(response, "refresh_token") : NULL;
        char *new_scope = response ? json_get_str(response, "scope") : NULL;
        char *new_type = response ? json_get_str(response, "token_type") : NULL;
        int expires_in = response ? json_get_int(response, "expires_in", 0) : 0;
        const char *persist_refresh = new_refresh && new_refresh[0] ? new_refresh : refresh;
        if (new_access && new_access[0] && strlen(new_access) < KIMI_TOKEN_MAX &&
            persist_refresh && persist_refresh[0] && strlen(persist_refresh) < KIMI_TOKEN_MAX &&
            expires_in > 0) {
            expires_at = (long long)time(NULL) + expires_in;
            ok = kimi_write_credentials(path, new_access, persist_refresh, expires_at,
                                        expires_in,
                                        new_scope && new_scope[0] ? new_scope : old_scope,
                                        new_type && new_type[0] ? new_type : old_type);
        }
        kimi_secure_free(new_access);
        kimi_secure_free(new_refresh);
        free(new_scope);
        free(new_type);
        kimi_secure_free(response);
    }

    kimi_secure_free(access);
    kimi_secure_free(refresh);
    free(old_scope);
    free(old_type);
    kimi_secure_free(json);
    memset(rejected, 0, sizeof(rejected));
    lock.l_type = F_UNLCK;
    (void)fcntl(lock_fd, F_SETLK, &lock);
    close(lock_fd);
    if (!ok)
        return false;
    kimi_cache_reset();
    return kimi_load();
}

const char *kimi_oauth_access_token(bool allow_refresh) {
    const char *env = kimi_env_token();
    if (env) {
        if (strlen(env) >= sizeof(s_return_token))
            return NULL;
        snprintf(s_return_token, sizeof(s_return_token), "%s", env);
        return s_return_token;
    }
    pthread_mutex_lock(&g_token_mutex);
    if (!kimi_load()) {
        pthread_mutex_unlock(&g_token_mutex);
        return NULL;
    }
    if (g_expires_at_s > 0 && (long long)time(NULL) + KIMI_REFRESH_SKEW_S >= g_expires_at_s) {
        if (!allow_refresh || !kimi_refresh_locked(NULL)) {
            pthread_mutex_unlock(&g_token_mutex);
            return NULL;
        }
    }
    snprintf(s_return_token, sizeof(s_return_token), "%s", g_token);
    pthread_mutex_unlock(&g_token_mutex);
    return s_return_token;
}

const char *kimi_oauth_refresh_after_unauthorized(const char *rejected_token) {
    if (kimi_env_token())
        return NULL;
    pthread_mutex_lock(&g_token_mutex);
    bool ok = kimi_refresh_locked(rejected_token);
    if (ok)
        snprintf(s_return_token, sizeof(s_return_token), "%s", g_token);
    pthread_mutex_unlock(&g_token_mutex);
    return ok ? s_return_token : NULL;
}

bool kimi_oauth_available(void) { return kimi_oauth_access_token(false) != NULL; }
const char *kimi_oauth_source_name(void) {
    if (kimi_env_token())
        return "env";
    return kimi_oauth_available() ? "kimi-code-oauth" : "none";
}
