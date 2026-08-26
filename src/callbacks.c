#include "callbacks.h"

#include "chronicle.h"
#include "json_util.h"
#include "http_pool.h"
#include "crypto.h"
#include "webhook_security.h"

#include <curl/curl.h>

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

static const char *nz(const char *s) { return s ? s : ""; }
static char *read_entire_file(const char *path);

static bool mkdir_p(const char *path) {
    if (!path || !path[0]) return false;
    char tmp[2048];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return false;
            *p = '/';
        }
    }
    return mkdir(tmp, 0755) == 0 || errno == EEXIST;
}

static bool write_file_atomic(const char *path, const char *body) {
    char tmp[2300];
    snprintf(tmp, sizeof(tmp), "%s.tmp.%ld", path, (long)getpid());
    int fd = open(tmp, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd < 0) return false;
    size_t len = body ? strlen(body) : 0;
    const char *p = body ? body : "";
    while (len) {
        ssize_t n = write(fd, p, len);
        if (n < 0) { if (errno == EINTR) continue; close(fd); unlink(tmp); return false; }
        p += n; len -= (size_t)n;
    }
    if (fsync(fd) != 0) { close(fd); unlink(tmp); return false; }
    if (close(fd) != 0) { unlink(tmp); return false; }
    return rename(tmp, path) == 0;
}

static bool has_suffix(const char *s, const char *suffix) {
    if (!s || !suffix) return false;
    size_t n = strlen(s), m = strlen(suffix);
    return n >= m && strcmp(s + n - m, suffix) == 0;
}

static bool json_file_has_string_field(const char *path, const char *key, const char *want) {
    if (!path || !key || !want || !want[0]) return false;
    char *body = read_entire_file(path);
    if (!body) return false;
    char *got = json_get_str(body, key);
    bool ok = got && strcmp(got, want) == 0;
    free(got);
    free(body);
    return ok;
}

static bool outbox_has_idempotency_key(const char *outbox, const char *key) {
    if (!outbox || !key || !key[0]) return false;
    DIR *d = opendir(outbox);
    if (!d) return false;
    bool found = false;
    struct dirent *ent;
    while (!found && (ent = readdir(d))) {
        if (ent->d_name[0] == '.' || !has_suffix(ent->d_name, ".json")) continue;
        char path[2300];
        snprintf(path, sizeof(path), "%s/%s", outbox, ent->d_name);
        found = json_file_has_string_field(path, "idempotency_key", key);
    }
    closedir(d);
    return found;
}

static bool safe_run_id(const char *run) {
    if (!run || !run[0]) return false;
    if (strcmp(run, ".") == 0 || strcmp(run, "..") == 0) return false;
    for (const unsigned char *p = (const unsigned char *)run; *p; p++) {
        if (!(isalnum(*p) || *p == '-' || *p == '_' || *p == '.')) return false;
    }
    return true;
}

bool callback_policy_from_env(callback_policy_t *out) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    const char *url = getenv("DSCO_CALLBACK_URL");
    if (!url || !url[0]) return false;
    out->enabled = true;
    snprintf(out->url, sizeof(out->url), "%s", url);
    snprintf(out->events, sizeof(out->events), "%s", getenv("DSCO_CALLBACK_EVENTS") ? getenv("DSCO_CALLBACK_EVENTS") : "run.*,tool.*,turn.checkpoint,budget.*,permission.*,approval.*");
    snprintf(out->mode, sizeof(out->mode), "%s", getenv("DSCO_CALLBACK_MODE") ? getenv("DSCO_CALLBACK_MODE") : "redacted");
    out->max_payload_bytes = getenv("DSCO_CALLBACK_MAX_BYTES") ? atoi(getenv("DSCO_CALLBACK_MAX_BYTES")) : 65536;
    if (out->max_payload_bytes <= 0) out->max_payload_bytes = 65536;
    return true;
}

static bool one_pattern_matches(const char *pat, const char *event) {
    if (!pat || !event) return false;
    while (isspace((unsigned char)*pat)) pat++;
    size_t n = strlen(pat);
    while (n && isspace((unsigned char)pat[n - 1])) n--;
    if (n == 1 && pat[0] == '*') return true;
    if (n >= 2 && pat[n - 1] == '*' && pat[n - 2] == '.') {
        return strncmp(event, pat, n - 1) == 0;
    }
    return strlen(event) == n && strncmp(event, pat, n) == 0;
}

bool callback_event_matches(const callback_policy_t *policy, const char *event_name) {
    if (!policy || !policy->enabled || !event_name) return false;
    const char *p = policy->events;
    while (*p) {
        const char *comma = strchr(p, ',');
        size_t n = comma ? (size_t)(comma - p) : strlen(p);
        char pat[128];
        if (n >= sizeof(pat)) n = sizeof(pat) - 1;
        memcpy(pat, p, n); pat[n] = 0;
        if (one_pattern_matches(pat, event_name)) return true;
        if (!comma) break;
        p = comma + 1;
    }
    return false;
}

static bool callback_outbox_enqueue_in_dir(const callback_policy_t *policy,
                                           const char *run_dir,
                                           const char *run_id,
                                           const char *event_name,
                                           const char *event_json,
                                           const char *idempotency_key,
                                           bool *deduped_out) {
    if (deduped_out) *deduped_out = false;
    if (!callback_event_matches(policy, event_name) || !event_json) return false;
    if (!run_dir || !run_dir[0]) return false;
    char outbox[2048];
    snprintf(outbox, sizeof(outbox), "%s/outbox", run_dir);
    if (!mkdir_p(outbox)) return false;
    if (idempotency_key && idempotency_key[0] && outbox_has_idempotency_key(outbox, idempotency_key)) {
        if (deduped_out) *deduped_out = true;
        return true;
    }
    char id[37];
    chronicle_new_id(id, sizeof(id));
    char path[2300];
    snprintf(path, sizeof(path), "%s/%s.json", outbox, id);
    jbuf_t b;
    jbuf_init(&b, (size_t)(strlen(event_json) + 640));
    jbuf_append(&b, "{\"schema\":\"dsco.callback_outbox.v1\",\"delivery_id\":");
    jbuf_append_json_str(&b, id);
    jbuf_append(&b, ",\"run_id\":"); jbuf_append_json_str(&b, nz(run_id));
    if (idempotency_key && idempotency_key[0]) {
        jbuf_append(&b, ",\"idempotency_key\":");
        jbuf_append_json_str(&b, idempotency_key);
    }
    jbuf_append(&b, ",\"url\":"); jbuf_append_json_str(&b, policy->url);
    jbuf_append(&b, ",\"mode\":"); jbuf_append_json_str(&b, policy->mode);
    jbuf_append(&b, ",\"event_name\":"); jbuf_append_json_str(&b, event_name);
    jbuf_append(&b, ",\"event\":"); jbuf_append(&b, event_json);
    jbuf_append(&b, "}");
    bool ok = write_file_atomic(path, b.data);
    if (ok) {
        char st[2300];
        snprintf(st, sizeof(st), "%s/%s.state", outbox, id);
        write_file_atomic(st, "{\"state\":\"pending\",\"attempts\":0}\n");
    }
    jbuf_free(&b);
    return ok;
}

bool callback_outbox_enqueue(const callback_policy_t *policy,
                             const char *run_id,
                             const char *event_name,
                             const char *event_json) {
    return callback_outbox_enqueue_in_dir(policy, chronicle_run_dir(), run_id, event_name, event_json, NULL, NULL);
}

static void run_dir_from_arg(const char *run_id, char *out, size_t out_len) {
    if (!out || out_len == 0) return;
    out[0] = 0;
    if (!safe_run_id(run_id)) return;
    const char *base = getenv("DSCO_RUNS_DIR");
    if (base && base[0]) snprintf(out, out_len, "%s/%s", base, run_id);
    else {
        const char *home = getenv("HOME");
        snprintf(out, out_len, "%s/.dsco/runs/%s", home && home[0] ? home : ".", run_id);
    }
}


typedef struct { char *data; size_t len; size_t cap; } cb_resp_t;

static size_t cb_write(void *ptr, size_t size, size_t nmemb, void *userdata) {
    cb_resp_t *b = (cb_resp_t *)userdata;
    size_t n = size * nmemb;
    if (!b) return n;
    if (b->len + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 1024;
        while (nc < b->len + n + 1) nc *= 2;
        char *nd = (char *)realloc(b->data, nc);
        if (!nd) return 0;
        b->data = nd; b->cap = nc;
    }
    memcpy(b->data + b->len, ptr, n);
    b->len += n; b->data[b->len] = 0;
    return n;
}

static char *read_entire_file(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return NULL; }
    long n = ftell(fp);
    if (n < 0 || n > 32L * 1024L * 1024L) { fclose(fp); return NULL; }
    rewind(fp);
    char *buf = (char *)malloc((size_t)n + 1);
    if (!buf) { fclose(fp); return NULL; }
    size_t got = fread(buf, 1, (size_t)n, fp);
    fclose(fp);
    buf[got] = 0;
    return buf;
}


static long long cb_now_ms(void) { return (long long)time(NULL) * 1000LL; }

static int state_attempts(const char *st) {
    if (!st) return 0;
    char *v = json_get_str(st, "attempts");
    int n = v ? atoi(v) : 0;
    free(v);
    return n < 0 ? 0 : n;
}

static long long state_next_attempt(const char *st) {
    if (!st) return 0;
    const char *p = strstr(st, "\"next_attempt_ms\"");
    if (!p) return 0;
    p = strchr(p, ':');
    return p ? atoll(p + 1) : 0;
}

static bool state_is(const char *st, const char *state) {
    if (!st || !state) return false;
    char pat[96];
    snprintf(pat, sizeof(pat), "\"state\":\"%s\"", state);
    return strstr(st, pat) != NULL;
}

static void signature_headers(struct curl_slist **hdrs, const char *secret, const char *body) {
    long long ts = cb_now_ms();
    char tsbuf[64];
    snprintf(tsbuf, sizeof(tsbuf), "%lld", ts);
    char msg_prefix[80];
    snprintf(msg_prefix, sizeof(msg_prefix), "%s.", tsbuf);
    size_t blen = body ? strlen(body) : 0;
    size_t mlen = strlen(msg_prefix) + blen;
    char *msg = (char *)malloc(mlen + 1);
    if (!msg) return;
    memcpy(msg, msg_prefix, strlen(msg_prefix));
    if (blen) memcpy(msg + strlen(msg_prefix), body, blen);
    msg[mlen] = 0;
    char mac[65];
    hmac_sha256_hex((const uint8_t *)secret, strlen(secret), (const uint8_t *)msg, mlen, mac);
    free(msg);
    char hts[96], hsig[128];
    snprintf(hts, sizeof(hts), "X-DSCO-Timestamp: %s", tsbuf);
    snprintf(hsig, sizeof(hsig), "X-DSCO-Signature: v1=%s", mac);
    *hdrs = curl_slist_append(*hdrs, hts);
    *hdrs = curl_slist_append(*hdrs, hsig);
}

static long post_callback_json(const char *url, const char *body, const char *delivery_id, char *err, size_t err_len) {
    if (err_len) err[0] = 0;
    CURL *curl = curl_easy_init();
    if (!curl) { snprintf(err, err_len, "curl init failed"); return 0; }
    dsco_http_pool_apply(curl);
    struct curl_slist *hdrs = NULL;
    hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
    hdrs = curl_slist_append(hdrs, "User-Agent: dsco-callbacks/1");
    const char *secret = getenv("DSCO_CALLBACK_SECRET");
    if (secret && secret[0]) signature_headers(&hdrs, secret, body ? body : "{}");
    char h_event[128];
    snprintf(h_event, sizeof(h_event), "X-DSCO-Delivery-ID: %s", delivery_id ? delivery_id : "");
    hdrs = curl_slist_append(hdrs, h_event);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body ? body : "{}");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, cb_write);
    cb_resp_t resp = {0};
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 3000L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 8000L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    CURLcode rc = curl_easy_perform(curl);
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    if (rc != CURLE_OK) snprintf(err, err_len, "%s", curl_easy_strerror(rc));
    free(resp.data);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);
    return code;
}

static char *json_get_string_dup_simple(const char *json, const char *key) {
    return json_get_str(json, key);
}

static int callbacks_drain_run(const char *run) {
    char dir[2048];
    run_dir_from_arg(run, dir, sizeof(dir));
    char outbox[2300];
    snprintf(outbox, sizeof(outbox), "%s/outbox", dir);
    DIR *d = opendir(outbox);
    if (!d) { fprintf(stderr, "no outbox: %s\n", outbox); return 1; }
    int delivered = 0, failed = 0, skipped = 0;
    struct dirent *ent;
    while ((ent = readdir(d))) {
        if (!strstr(ent->d_name, ".json")) continue;
        char path[2300];
        snprintf(path, sizeof(path), "%s/%s", outbox, ent->d_name);
        char state[2300];
        snprintf(state, sizeof(state), "%s", path);
        char *dot = strrchr(state, '.');
        if (dot) strcpy(dot, ".state");
        char *st = read_entire_file(state);
        int attempts = state_attempts(st);
        long long next_ms = state_next_attempt(st);
        if (state_is(st, "delivered") || state_is(st, "dead_lettered")) { free(st); skipped++; continue; }
        if (next_ms > cb_now_ms()) { free(st); skipped++; continue; }
        free(st);
        char *body = read_entire_file(path);
        if (!body) { failed++; continue; }
        char *url = json_get_string_dup_simple(body, "url");
        char *delivery_id = json_get_string_dup_simple(body, "delivery_id");
        if (!url || !url[0]) { free(body); free(url); free(delivery_id); failed++; continue; }
        char err[256];
        const char *allow_private = getenv("DSCO_WEBHOOK_ALLOW_PRIVATE");
        bool private_override = allow_private &&
                                (strcmp(allow_private, "1") == 0 ||
                                 strcasecmp(allow_private, "true") == 0);
        if (!private_override && !webhook_egress_url_allowed(url, err, sizeof(err))) {
            jbuf_t denied; jbuf_init(&denied, 512);
            attempts++;
            jbuf_appendf(&denied,
                         "{\"state\":\"dead_lettered\",\"attempts\":%d,"
                         "\"last_http_status\":0,\"next_attempt_ms\":0,\"last_error\":",
                         attempts);
            jbuf_append_json_str(&denied, err[0] ? err : "callback URL denied by SSRF policy");
            jbuf_append(&denied, "}\n");
            write_file_atomic(state, denied.data);
            jbuf_free(&denied);
            free(body); free(url); free(delivery_id);
            failed++;
            continue;
        }
        long code = post_callback_json(url, body, delivery_id, err, sizeof(err));
        jbuf_t sb; jbuf_init(&sb, 512);
        attempts++;
        if (code >= 200 && code < 300) {
            jbuf_appendf(&sb, "{\"state\":\"delivered\",\"attempts\":%d,\"last_http_status\":%ld,\"delivered_at_ms\":%lld}\n", attempts, code, cb_now_ms());
            write_file_atomic(state, sb.data);
            delivered++;
        } else {
            int max_attempts = getenv("DSCO_CALLBACK_MAX_ATTEMPTS") ? atoi(getenv("DSCO_CALLBACK_MAX_ATTEMPTS")) : 12;
            if (max_attempts <= 0) max_attempts = 12;
            bool dead = attempts >= max_attempts || code == 401 || code == 403;
            long long delay = 1000LL << (attempts > 8 ? 8 : attempts);
            if (delay > 300000LL) delay = 300000LL;
            long long next = cb_now_ms() + delay;
            jbuf_appendf(&sb, "{\"state\":\"%s\",\"attempts\":%d,\"last_http_status\":%ld,\"next_attempt_ms\":%lld,\"last_error\":", dead ? "dead_lettered" : "retry_scheduled", attempts, code, dead ? 0LL : next);
            jbuf_append_json_str(&sb, err[0] ? err : "http error");
            jbuf_append(&sb, "}\n");
            write_file_atomic(state, sb.data);
            failed++;
        }
        jbuf_free(&sb);
        free(body); free(url); free(delivery_id);
    }
    closedir(d);
    printf("{\"delivered\":%d,\"failed\":%d,\"skipped\":%d}\n", delivered, failed, skipped);
    return failed ? 1 : 0;
}


static uint32_t cb_le32(const unsigned char b[4]) { return (uint32_t)b[0] | ((uint32_t)b[1]<<8) | ((uint32_t)b[2]<<16) | ((uint32_t)b[3]<<24); }
static uint32_t cb_crc32(uint32_t crc, const void *data, size_t len) {
    static uint32_t table[256]; static bool ready=false;
    if(!ready){ for(uint32_t i=0;i<256;i++){ uint32_t c=i; for(int j=0;j<8;j++) c=(c&1)?(0xEDB88320U^(c>>1)):(c>>1); table[i]=c;} ready=true; }
    crc=~crc; const unsigned char *p=(const unsigned char*)data; for(size_t i=0;i<len;i++) crc=table[(crc^p[i])&0xffU]^(crc>>8); return ~crc;
}
static char *extract_record_type(const char *rec) {
    char *t = json_get_str(rec, "type");
    return t;
}
static char *extract_payload_object_copy(const char *rec) {
    const char *p = strstr(rec, "\"payload\""); if(!p) return strdup("{}");
    p = strchr(p, ':'); if(!p) return strdup("{}"); p++;
    while(*p && isspace((unsigned char)*p)) p++;
    if(*p!='{' && *p!='[') return strdup("{}");
    char open=*p, close=(open=='{')?'}':']'; int depth=0; bool str=false, esc=false; const char *start=p;
    for(; *p; p++){
        char c=*p;
        if(str){ if(esc) esc=false; else if(c=='\\') esc=true; else if(c=='"') str=false; continue; }
        if(c=='"'){str=true; continue;} if(c==open) depth++; else if(c==close){ depth--; if(depth==0){ size_t n=(size_t)(p-start+1); char *out=malloc(n+1); if(!out) return NULL; memcpy(out,start,n); out[n]=0; return out; }}
    }
    return strdup("{}");
}
static bool journal_record_read(FILE *fp, char **buf_out, uint32_t *len_out) {
    if (buf_out) *buf_out = NULL;
    if (len_out) *len_out = 0;
    unsigned char hdr[8];
    size_t got = fread(hdr, 1, 8, fp);
    if (got == 0) return false;
    if (got != 8) return false;
    uint32_t len = cb_le32(hdr), want = cb_le32(hdr + 4);
    if (!len || len > 32U * 1024U * 1024U) return false;
    char *buf = (char *)malloc((size_t)len + 1);
    if (!buf) return false;
    got = fread(buf, 1, len, fp);
    if (got != len) { free(buf); return false; }
    buf[len] = 0;
    if (cb_crc32(0, buf, len) != want) { free(buf); return false; }
    if (buf_out) *buf_out = buf; else free(buf);
    if (len_out) *len_out = len;
    return true;
}

static void callback_reconcile_key(const char *run, const char *rec, const char *typ, char out[65]) {
    char *idem = json_get_str(rec, "idempotency_key");
    if (idem && idem[0]) {
        sha256_hex((const uint8_t *)idem, strlen(idem), out);
        free(idem);
        return;
    }
    free(idem);
    long long seq = json_get_i64(rec, "seq", -1);
    if (seq >= 0) {
        char tmp[512];
        snprintf(tmp, sizeof(tmp), "%s:%lld:%s", nz(run), seq, nz(typ));
        sha256_hex((const uint8_t *)tmp, strlen(tmp), out);
    } else {
        sha256_hex((const uint8_t *)rec, strlen(rec), out);
    }
}

static int callbacks_reconcile_run(const char *run) {
    callback_policy_t pol;
    if (!callback_policy_from_env(&pol)) { fprintf(stderr, "DSCO_CALLBACK_URL not set\n"); return 2; }
    char dir[2048]; run_dir_from_arg(run, dir, sizeof(dir));
    char jp[2300]; snprintf(jp, sizeof(jp), "%s/journal.wal", dir);
    FILE *fp = fopen(jp, "rb");
    if (!fp) { fprintf(stderr, "no journal: %s\n", jp); return 1; }
    int enq = 0, bad = 0, seen = 0, deduped = 0;
    for (;;) {
        char *buf = NULL;
        long pos = ftell(fp);
        if (!journal_record_read(fp, &buf, NULL)) {
            if (feof(fp)) break;
            bad++;
            break;
        }
        (void)pos;
        seen++;
        char *typ = extract_record_type(buf);
        if (typ && callback_event_matches(&pol, typ)) {
            char *payload = extract_payload_object_copy(buf);
            if (payload) {
                char key[65];
                callback_reconcile_key(run, buf, typ, key);
                bool was_deduped = false;
                if (callback_outbox_enqueue_in_dir(&pol, dir, run, typ, payload, key, &was_deduped)) {
                    if (was_deduped) deduped++;
                    else enq++;
                }
                free(payload);
            }
        }
        free(typ);
        free(buf);
    }
    fclose(fp);
    printf("{\"seen\":%d,\"enqueued\":%d,\"deduped\":%d,\"bad_tail\":%d}\n", seen, enq, deduped, bad);
    return bad ? 1 : 0;
}

int callbacks_cli(int argc, char **argv) {
    const char *cmd = argc >= 3 ? argv[2] : "list";
    const char *run = NULL;
    for (int i = 3; i + 1 < argc; i++) if (strcmp(argv[i], "--run") == 0) run = argv[i + 1];
    if (!run && argc >= 4 && strcmp(cmd, "list") == 0) run = argv[3];
    if (!run) { fprintf(stderr, "usage: dsco callbacks list --run <run-id>\n"); return 2; }
    char dir[2048];
    run_dir_from_arg(run, dir, sizeof(dir));
    char outbox[2300];
    snprintf(outbox, sizeof(outbox), "%s/outbox", dir);
    if (strcmp(cmd, "list") == 0 || strcmp(cmd, "ls") == 0) {
        DIR *d = opendir(outbox);
        if (!d) { fprintf(stderr, "no outbox: %s\n", outbox); return 1; }
        struct dirent *ent;
        while ((ent = readdir(d))) {
            if (ent->d_name[0] == '.') continue;
            printf("%s\n", ent->d_name);
        }
        closedir(d);
        return 0;
    }
    if (strcmp(cmd, "drain") == 0) {
        return callbacks_drain_run(run);
    }
    if (strcmp(cmd, "reconcile") == 0) {
        return callbacks_reconcile_run(run);
    }
    fprintf(stderr, "usage: dsco callbacks [list|drain|reconcile] --run <run-id>\n");
    return 2;
}
