#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1
#include "company_account.h"
#include "crypto.h"
#include "json_util.h"
#include <curl/curl.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <time.h>
#include <unistd.h>

#define ACCOUNT_ORIGIN "https://api.distributed.systems"
#define ACCOUNT_ISSUER "https://auth.distributed.systems"
#define ACCOUNT_SCOPE "dsco:account"
#define ACCOUNT_MAX_BODY 65536u
#define ACCOUNT_PENDING "company-account.pending.json"
#define ACCOUNT_CACHE "company-account.json"
#define ACCOUNT_POLICY "company-account-policy.json"

typedef struct { char *data; size_t len; } account_body_t;
static const char *account_origin(void) {
#ifdef DSCO_INTERNAL_TESTS
    const char *test = getenv("DSCO_ACCOUNT_TEST_ORIGIN");
    if (test && strncmp(test, "http://127.0.0.1:", 17) == 0) {
        char *end = NULL; long port = strtol(test + 17, &end, 10);
        if (port > 0 && port < 65536 && end && *end == '\0') return test;
    }
#endif
    return ACCOUNT_ORIGIN;
}
static int account_error(const char *code, const char *message) {
    fprintf(stderr, "DSCO account [%s]: %s\n", code, message);
    return 1;
}
static bool path_for(const char *name, char *out, size_t len) {
    const char *home = getenv("HOME");
    return home && *home && snprintf(out, len, "%s/.dsco/%s", home, name) < (int)len;
}
static char *read_private(const char *name) {
    char path[PATH_MAX];
    if (!path_for(name, path, sizeof(path))) return NULL;
    int fd = open(path, O_RDONLY | O_NOFOLLOW);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != getuid() ||
        (st.st_mode & 077) || st.st_size <= 0 || st.st_size > ACCOUNT_MAX_BODY) {
        close(fd); return NULL;
    }
    size_t len = (size_t)st.st_size, used = 0;
    char *data = malloc(len + 1);
    if (!data) { close(fd); return NULL; }
    while (used < len) {
        ssize_t n = read(fd, data + used, len - used);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { free(data); close(fd); return NULL; }
        used += (size_t)n;
    }
    data[len] = '\0'; close(fd);
    if (!json_is_valid_container(data)) { free(data); return NULL; }
    return data;
}
static bool write_private(const char *name, const char *data) {
    char path[PATH_MAX], dir[PATH_MAX], tmp[PATH_MAX];
    if (!path_for(name, path, sizeof(path)) || !path_for("", dir, sizeof(dir))) return false;
    size_t dl = strlen(dir); if (dl && dir[dl-1] == '/') dir[dl-1] = '\0';
    if (mkdir(dir, 0700) && errno != EEXIST) return false;
    struct stat st;
    if (lstat(dir, &st) || !S_ISDIR(st.st_mode) || st.st_uid != getuid() || (st.st_mode & 022)) return false;
    if (lstat(path, &st) == 0 && (!S_ISREG(st.st_mode) || st.st_uid != getuid())) return false;
    if (snprintf(tmp, sizeof(tmp), "%s/.company-account-XXXXXX", dir) >= (int)sizeof(tmp)) return false;
    int fd = mkstemp(tmp); if (fd < 0) return false;
    bool ok = fchmod(fd, 0600) == 0;
    size_t size = strlen(data), used = 0;
    while (ok && used < size) {
        ssize_t n = write(fd, data + used, size - used);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { ok = false; break; }
        used += (size_t)n;
    }
    if (ok && fsync(fd)) ok = false;
    if (close(fd)) ok = false;
    if (ok && rename(tmp, path)) ok = false;
    if (!ok) unlink(tmp);
    if (ok) {
        int dfd = open(dir, O_RDONLY | O_DIRECTORY);
        if (dfd < 0 || fsync(dfd)) ok = false;
        if (dfd >= 0) close(dfd);
    }
    return ok;
}
static bool remove_private(const char *name) {
    char path[PATH_MAX];
    if (!path_for(name,path,sizeof(path))) return false;
    return unlink(path)==0 || errno==ENOENT;
}
static int account_lock(void) {
    char dir[PATH_MAX],path[PATH_MAX];
    if (!path_for("",dir,sizeof(dir)) || !path_for("company-account.lock",path,sizeof(path))) return -1;
    size_t n=strlen(dir); if (n && dir[n-1]=='/') dir[n-1]='\0';
    if (mkdir(dir,0700) && errno!=EEXIST) return -1;
    struct stat st;
    if (lstat(dir,&st) || !S_ISDIR(st.st_mode) || st.st_uid!=getuid() || (st.st_mode&022)) return -1;
    int fd=open(path,O_RDWR|O_CREAT|O_NOFOLLOW,0600);
    if (fd<0) return -1;
    if (fstat(fd,&st) || !S_ISREG(st.st_mode) || st.st_uid!=getuid() || fchmod(fd,0600) || flock(fd,LOCK_EX|LOCK_NB)) { close(fd); return -1; }
    return fd;
}
static size_t account_receive(char *ptr, size_t size, size_t nmemb, void *ctx) {
    account_body_t *b = ctx;
    if (size && nmemb > ACCOUNT_MAX_BODY / size) return 0;
    size_t n = size * nmemb;
    if (n > ACCOUNT_MAX_BODY - b->len) return 0;
    char *next = realloc(b->data, b->len + n + 1); if (!next) return 0;
    b->data = next; memcpy(b->data + b->len, ptr, n); b->len += n; b->data[b->len] = '\0';
    return n;
}
static bool account_http(const char *path, const char *payload, const char *token,
                         long *status, char **response) {
    *response = NULL; *status = 0;
    CURL *curl = curl_easy_init(); if (!curl) return false;
    char url[1024], auth[512]; account_body_t body = {0};
    if (snprintf(url, sizeof(url), "%s%s", account_origin(), path) >= (int)sizeof(url)) { curl_easy_cleanup(curl); return false; }
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Accept: application/json");
    headers = curl_slist_append(headers, "Content-Type: application/json");
    if (token) {
        if (snprintf(auth, sizeof(auth), "Authorization: Bearer %s", token) >= (int)sizeof(auth)) { curl_slist_free_all(headers); curl_easy_cleanup(curl); return false; }
        headers = curl_slist_append(headers, auth);
    }
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L); /* Never forward account credentials across redirects. */
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, account_receive);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "dsco/company-account-v1");
    if (payload) { curl_easy_setopt(curl, CURLOPT_POST, 1L); curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload); }
    CURLcode result = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, status);
    curl_slist_free_all(headers); curl_easy_cleanup(curl);
    if (result != CURLE_OK || !body.data || !json_is_valid_container(body.data)) { free(body.data); return false; }
    *response = body.data; return true;
}
static bool urlsafe(const char *s, size_t min, size_t max) {
    if (!s || strlen(s) < min || strlen(s) > max) return false;
    for (; *s; ++s) if (!( (*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || *s == '-' || *s == '_')) return false;
    return true;
}
static void b64url(const unsigned char *src, size_t n, char *dst) {
    static const char abc[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    unsigned accum = 0, bits = 0; size_t j = 0;
    for (size_t i = 0; i < n; ++i) {
        accum = (accum << 8) | src[i]; bits += 8;
        while (bits >= 6) { bits -= 6; dst[j++] = abc[(accum >> bits) & 63]; }
    }
    if (bits) dst[j++] = abc[(accum << (6-bits)) & 63];
    dst[j] = '\0';
}
static bool random_verifier(char out[44]) {
    unsigned char data[32]; size_t used = 0;
    int fd = open("/dev/urandom", O_RDONLY); if (fd < 0) return false;
    while (used < sizeof(data)) {
        ssize_t n = read(fd, data + used, sizeof(data) - used);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { close(fd); return false; }
        used += (size_t)n;
    }
    close(fd); b64url(data, sizeof(data), out); memset(data, 0, sizeof(data)); return true;
}
static char *cache_token_anytime(const char *cache) {
    if (!cache) return NULL;
    char *token = json_get_str(cache, "account_token");
    if (!token || strncmp(token, "dsco-acct-", 10) || !urlsafe(token+10, 43, 43)) { free(token); return NULL; }
    return token;
}
static char *cache_token(const char *cache) {
    if (!cache || json_get_i64(cache, "expires_at", 0) <= (long long)time(NULL)) return NULL;
    return cache_token_anytime(cache);
}
static bool user_code_ok(const char *s) {
    if (!s || strlen(s)!=9 || s[4]!='-') return false;
    for (int i=0;i<9;i++) if (i!=4 && !strchr("ABCDEFGHJKLMNPQRSTUVWXYZ23456789",s[i])) return false;
    return true;
}
bool company_account_enrolled(void) {
    char *cache = read_private(ACCOUNT_CACHE), *token = cache_token(cache);
    bool ok = token != NULL; free(token); free(cache); return ok;
}
int company_account_require(void) {
    char *policy = read_private(ACCOUNT_POLICY);
    bool exempt = policy && !json_get_bool(policy, "registration_required", true);
    free(policy);
    if (exempt || company_account_enrolled()) return 0;
    fprintf(stderr, "DSCO account required for this new user session.\n"
                    "Run: dsco account signup\n"
                    "Sign in through auth.distributed.systems; enrollment collects your name and email.\n"
                    "This registration step does not upload local files or provider credentials.\n");
    return 78;
}
static char *verified_profile(const char *token) {
    long status; char *body = NULL;
    if (!account_http("/api/auth/dsco/me", NULL, token, &status, &body) || status != 200) { free(body); return NULL; }
    json_view_t *v = json_view_open(body);
    const char *issuer = json_view_str(v,"issuer"), *subject = json_view_str(v,"subject"), *id = json_view_str(v,"account_id");
    const char *email = json_view_str(v,"email"), *name = json_view_str(v,"name"), *scope = json_view_str(v,"scope");
    bool valid = issuer && !strcmp(issuer,ACCOUNT_ISSUER) && subject && *subject && id && *id &&
                 email && *email && name && scope && !strcmp(scope,ACCOUNT_SCOPE) && json_view_bool(v,"email_verified",false);
    char *result = NULL;
    if (valid) {
        jbuf_t b; jbuf_init(&b,512); jbuf_append(&b,"{\"account_id\":"); jbuf_append_json_str(&b,id);
        jbuf_append(&b,",\"issuer\":"); jbuf_append_json_str(&b,issuer); jbuf_append(&b,",\"subject\":"); jbuf_append_json_str(&b,subject);
        jbuf_append(&b,",\"name\":"); jbuf_append_json_str(&b,name); jbuf_append(&b,",\"email\":"); jbuf_append_json_str(&b,email);
        jbuf_append(&b,",\"email_verified\":true,\"scope\":\"dsco:account\"}"); result = strdup(b.data); jbuf_free(&b);
    }
    json_view_close(v); free(body); return result;
}
static int account_finish(const char *token, long long expires) {
    char *profile = verified_profile(token);
    if (!profile) return account_error("IDENTITY_UNVERIFIED", "The account service did not verify this identity. No local enrollment was saved.");
    jbuf_t b; jbuf_init(&b,1024); jbuf_append(&b,"{\"account_token\":"); jbuf_append_json_str(&b,token);
    jbuf_appendf(&b,",\"expires_at\":%lld,\"profile\":%s}", expires, profile);
    bool ok = write_private(ACCOUNT_CACHE,b.data); jbuf_free(&b);
    if (ok) { remove_private(ACCOUNT_PENDING); printf("{\"authenticated\":true,\"profile\":%s}\n",profile); }
    free(profile);
    return ok ? 0 : account_error("SAVE_FAILED", "The server approved enrollment, but the private local credential file could not be saved.");
}
static int account_login(bool start_only) {
    char *existing = read_private(ACCOUNT_CACHE), *existing_token = cache_token(existing);
    free(existing);
    if (existing_token) {
        char *profile = verified_profile(existing_token); free(existing_token);
        if (!profile) return account_error("EXISTING_ACCOUNT_UNVERIFIED", "An existing credential could not be verified. Use dsco account status or explicitly log out before replacing it.");
        printf("{\"authenticated\":true,\"profile\":%s}\n", profile); free(profile); return 0;
    }
    char *pending = read_private(ACCOUNT_PENDING);
    char *device = pending ? json_get_str(pending,"device_code") : NULL;
    char *verifier = pending ? json_get_str(pending,"code_verifier") : NULL;
    char *user = pending ? json_get_str(pending,"user_code") : NULL;
    long long expires = pending ? json_get_i64(pending,"expires_at",0) : 0;
    int interval = pending ? json_get_int(pending,"interval",5) : 5;
    free(pending);
    if (expires <= (long long)time(NULL) || !urlsafe(device,32,128) || !urlsafe(verifier,43,128) || !user_code_ok(user)) {
        free(device); free(verifier); free(user); device=verifier=user=NULL;
        char secret[44], challenge[44]; unsigned char digest[32];
        if (!random_verifier(secret)) return account_error("RANDOM_FAILED","Secure enrollment randomness unavailable; no request sent.");
        sha256_ctx_t sha; sha256_init(&sha);
        sha256_update(&sha,(const uint8_t *)secret,strlen(secret)); sha256_final(&sha,digest);
        b64url(digest,sizeof(digest),challenge);
        jbuf_t request; jbuf_init(&request,256); jbuf_append(&request,"{\"code_challenge\":"); jbuf_append_json_str(&request,challenge);
#ifdef __APPLE__
        jbuf_append(&request,",\"platform\":\"macos\"}");
#elif defined(__linux__)
        jbuf_append(&request,",\"platform\":\"linux\"}");
#else
        jbuf_append(&request,",\"platform\":\"other\"}");
#endif
        long status; char *response=NULL;
        bool ok=account_http("/api/auth/dsco/device",request.data,NULL,&status,&response); jbuf_free(&request);
        if (!ok || status!=200) { free(response); return account_error("ENROLLMENT_UNAVAILABLE","Could not start signup. Check connectivity and retry; no account was approved."); }
        device=json_get_str(response,"device_code"); user=json_get_str(response,"user_code");
        long long ttl=json_get_i64(response,"expires_in",0); interval=json_get_int(response,"interval",5); free(response);
        if (!urlsafe(device,32,128) || !user_code_ok(user) || ttl<=0 || ttl>900) {
            free(device); free(user); return account_error("INVALID_ENROLLMENT_RESPONSE","Account service returned an invalid enrollment contract.");
        }
        verifier=strdup(secret); memset(secret,0,sizeof(secret)); expires=(long long)time(NULL)+ttl;
        if (interval<5) interval=5;
        if (interval>60) interval=60;
        jbuf_t cache; jbuf_init(&cache,512); jbuf_append(&cache,"{\"device_code\":"); jbuf_append_json_str(&cache,device);
        jbuf_append(&cache,",\"code_verifier\":"); jbuf_append_json_str(&cache,verifier); jbuf_append(&cache,",\"user_code\":"); jbuf_append_json_str(&cache,user);
        jbuf_appendf(&cache,",\"expires_at\":%lld,\"interval\":%d}",expires,interval);
        ok=write_private(ACCOUNT_PENDING,cache.data); jbuf_free(&cache);
        if (!ok) { free(device); free(verifier); free(user); return account_error("SAVE_FAILED","Could not save private enrollment state; no polling started."); }
    }
    if (interval<5) interval=5;
    if (interval>60) interval=60;
    /* Fixed first-party URL; never follow an arbitrary verification URI from a response. */
    fprintf(stderr,"Create/sign in to your DSCO account through auth.distributed.systems.\n"
                   "Open: %s/account/dsco?code=%s\nCode: %s\n"
                   "Approve only the code shown in this terminal. Name/email are collected; local files and provider credentials are not uploaded by registration.\n",account_origin(),user,user);
    if (start_only) {
        printf("{\"status\":\"authorization_pending\",\"user_code\":\"%s\",\"expires_at\":%lld}\n",user,expires);
        free(device); free(verifier); free(user); return 0;
    }
    jbuf_t request; jbuf_init(&request,512); jbuf_append(&request,"{\"device_code\":"); jbuf_append_json_str(&request,device);
    jbuf_append(&request,",\"code_verifier\":"); jbuf_append_json_str(&request,verifier); jbuf_append(&request,"}");
    int result=1, transient=0;
    while ((long long)time(NULL)<expires) {
        long status; char *response=NULL;
        if (!account_http("/api/auth/dsco/token",request.data,NULL,&status,&response)) {
            free(response); if (++transient>2) { account_error("CONNECTION_FAILED","Polling stopped. Run dsco account login to resume this enrollment."); break; }
            sleep((unsigned)interval); continue;
        }
        if (status>=500) {
            free(response);
            if (++transient>2) { account_error("SERVICE_UNAVAILABLE","Polling paused after repeated server errors; enrollment state is retained for dsco account login."); break; }
            sleep((unsigned)interval); continue;
        }
        transient=0;
        if (status==200) {
            char *token=json_get_str(response,"account_token"), *scope=json_get_str(response,"scope");
            long long ttl=json_get_i64(response,"expires_in",0);
            if (token && !strncmp(token,"dsco-acct-",10) && urlsafe(token+10,43,43) && scope && !strcmp(scope,ACCOUNT_SCOPE) && ttl>0 && ttl<=31*86400LL)
                result=account_finish(token,(long long)time(NULL)+ttl);
            else account_error("INVALID_TOKEN_RESPONSE","No valid account-only credential was returned.");
            free(token); free(scope); free(response); break;
        }
        char *error=json_get_str(response,"error"); free(response);
        bool wait=(status==400 || status==429) && error && (!strcmp(error,"authorization_pending") || !strcmp(error,"slow_down") || !strcmp(error,"rate_limited"));
        if (error && (!strcmp(error,"slow_down") || !strcmp(error,"rate_limited")) && interval<60) interval+=5;
        free(error);
        if (!wait) { remove_private(ACCOUNT_PENDING); account_error("ENROLLMENT_STOPPED","The code was denied, expired, or already used. Run dsco account signup for a new code."); break; }
        sleep((unsigned)interval);
    }
    if ((long long)time(NULL)>=expires) { remove_private(ACCOUNT_PENDING); account_error("CODE_EXPIRED","Enrollment expired without completion. Run dsco account signup again."); }
    jbuf_free(&request); free(device); free(verifier); free(user); return result;
}
static int account_cli_impl(int argc, char **argv) {
    const char *action=argc>2?argv[2]:"status";
    if (!strcmp(action,"signup") || !strcmp(action,"login")) {
        if (argc>4 || (argc==4 && strcmp(argv[3],"--no-wait"))) return account_error("USAGE","dsco account signup|login [--no-wait]");
        return account_login(argc==4);
    }
    if (!strcmp(action,"status")) {
        char *cache=read_private(ACCOUNT_CACHE), *token=cache_token(cache), *profile=token?verified_profile(token):NULL;
        if (profile) printf("{\"authenticated\":true,\"profile\":%s}\n",profile);
        else printf("{\"authenticated\":false,\"next_step\":\"dsco account signup\"}\n");
        bool ok=profile!=NULL; free(profile); free(token); free(cache); return ok?0:1;
    }
    if (!strcmp(action,"logout")) {
        char *cache=read_private(ACCOUNT_CACHE), *token=cache_token_anytime(cache), *response=NULL; long status=0;
        char cache_path[PATH_MAX]; struct stat st;
        if (!cache && path_for(ACCOUNT_CACHE,cache_path,sizeof(cache_path)) && lstat(cache_path,&st)==0)
            return account_error("CACHE_UNREADABLE","An existing credential file could not be read safely. Fix its ownership/permissions before revocation; no logout success is claimed.");
        if (token && (!account_http("/api/auth/dsco/logout","{}",token,&status,&response) || status!=200)) {
            free(response); free(token); free(cache); return account_error("REVOCATION_UNCONFIRMED","Credentials retained locally so revocation can be retried; no logout success is claimed.");
        }
        free(response); free(token); free(cache);
        bool cleared=remove_private(ACCOUNT_CACHE); cleared=remove_private(ACCOUNT_PENDING) && cleared;
        if (!cleared) return account_error("LOCAL_CLEAR_FAILED","Server revocation completed, but local credential files could not be removed. Inspect local permissions; no complete logout is claimed.");
        puts("{\"authenticated\":false,\"logged_out\":true}"); return 0;
    }
    return account_error("USAGE","dsco account signup|login [--no-wait] | status | logout");
}

int company_account_cli(int argc, char **argv) {
    const char *action=argc>2?argv[2]:"status";
    bool mutation=!strcmp(action,"signup") || !strcmp(action,"login") || !strcmp(action,"logout");
    int lock=-1;
    if (mutation) {
        lock=account_lock();
        if (lock<0) return account_error("ACCOUNT_OPERATION_BUSY_OR_UNSAFE", "Another account operation is active, or the private directory/lock is unsafe. Finish or cancel the other sign-in before retrying.");
    }
    int result=account_cli_impl(argc,argv);
    if (lock>=0) close(lock);
    return result;
}
