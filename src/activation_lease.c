#include "activation_lease.h"
#include "crypto.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#endif

/* A deployment must pin this at build time.  An empty default is intentional:
 * accepting a key from a mutable runtime environment would make the lease
 * signature meaningless for a tenant-side binary. */
#ifndef DSCO_ACTIVATION_ISSUER_P256_B64
#define DSCO_ACTIVATION_ISSUER_P256_B64 ""
#endif

static void lease_set_err(char *err, size_t err_len, const char *msg) {
    if (!err || err_len == 0) return;
    snprintf(err, err_len, "%s", msg ? msg : "activation lease error");
}

static int64_t lease_now(void) {
    return (int64_t)time(NULL);
}

static bool lease_copy(char *dst, size_t dst_len, const char *src) {
    if (!dst || dst_len == 0) return false;
    if (!src) src = "";
    size_t n = strlen(src);
    if (n >= dst_len) {
        memcpy(dst, src, dst_len - 1);
        dst[dst_len - 1] = '\0';
        return false;
    }
    memcpy(dst, src, n + 1);
    return true;
}

static bool lease_mkdir_p_parent(const char *path) {
    char tmp[DSCO_ACTIVATION_PATH_MAX];
    if (!path || !lease_copy(tmp, sizeof(tmp), path)) return false;
    char *slash = strrchr(tmp, '/');
    if (!slash) return true;
    if (slash == tmp) return true;
    *slash = '\0';

    for (char *p = tmp + 1; *p; ++p) {
        if (*p != '/') continue;
        *p = '\0';
        if (mkdir(tmp, 0700) != 0 && errno != EEXIST) return false;
        *p = '/';
    }
    return mkdir(tmp, 0700) == 0 || errno == EEXIST;
}

static void lease_fsync_parent(const char *path) {
    char tmp[DSCO_ACTIVATION_PATH_MAX];
    if (!path || !lease_copy(tmp, sizeof(tmp), path)) return;
    char *slash = strrchr(tmp, '/');
    if (!slash || slash == tmp) return;
    *slash = '\0';
    int fd = open(tmp, O_RDONLY | O_DIRECTORY);
    if (fd >= 0) {
        (void)fsync(fd);
        close(fd);
    }
}

static int lease_json_escape(const char *src, char *out, size_t out_len) {
    size_t used = 0;
    if (!src || !out || out_len == 0) return -1;
    for (const unsigned char *p = (const unsigned char *)src; *p; ++p) {
        const char *rep = NULL;
        char buf[8];
        switch (*p) {
            case '\\': rep = "\\\\"; break;
            case '"': rep = "\\\""; break;
            case '\n': rep = "\\n"; break;
            case '\r': rep = "\\r"; break;
            case '\t': rep = "\\t"; break;
            default:
                if (*p < 0x20) {
                    snprintf(buf, sizeof(buf), "\\u%04x", *p);
                    rep = buf;
                }
                break;
        }
        if (rep) {
            size_t n = strlen(rep);
            if (used + n + 1 > out_len) return -1;
            memcpy(out + used, rep, n);
            used += n;
        } else {
            if (used + 2 > out_len) return -1;
            out[used++] = (char)*p;
        }
    }
    out[used] = '\0';
    return (int)used;
}

static const char *lease_find_key(const char *json, const char *key) {
    char needle[96];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = strstr(json, needle);
    if (!p) return NULL;
    p += strlen(needle);
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != ':') return NULL;
    p++;
    while (*p && isspace((unsigned char)*p)) p++;
    return p;
}

static bool lease_json_string(const char *json, const char *key, char *out, size_t out_len) {
    const char *p = lease_find_key(json, key);
    if (!p || *p != '"' || !out || out_len == 0) return false;
    p++;
    size_t used = 0;
    while (*p && *p != '"') {
        char c = *p++;
        if (c == '\\') {
            c = *p++;
            switch (c) {
                case 'n': c = '\n'; break;
                case 'r': c = '\r'; break;
                case 't': c = '\t'; break;
                case '\\': case '"': case '/': break;
                case 'u':
                    /* Skeleton parser: preserve unsupported unicode escapes as '?'. */
                    for (int i = 0; i < 4 && isxdigit((unsigned char)*p); ++i) p++;
                    c = '?';
                    break;
                default: break;
            }
        }
        if (used + 1 >= out_len) return false;
        out[used++] = c;
    }
    if (*p != '"') return false;
    out[used] = '\0';
    return true;
}

static bool lease_json_i64(const char *json, const char *key, int64_t *out) {
    const char *p = lease_find_key(json, key);
    if (!p || !out) return false;
    char *end = NULL;
    errno = 0;
    long long v = strtoll(p, &end, 10);
    if (errno != 0 || end == p) return false;
    *out = (int64_t)v;
    return true;
}

static bool lease_json_int(const char *json, const char *key, int *out) {
    int64_t v = 0;
    if (!lease_json_i64(json, key, &v)) return false;
    *out = (int)v;
    return true;
}

static bool lease_sha256_hex(const char *value) {
    if (!value || strlen(value) != 64) return false;
    for (size_t i = 0; i < 64; i++)
        if (!isxdigit((unsigned char)value[i])) return false;
    return true;
}

bool activation_lease_default_path(char *out, size_t out_len) {
    if (!out || out_len == 0) return false;
    const char *override = getenv("DSCO_ACTIVATION_LEASE_PATH");
    if (override && *override) return lease_copy(out, out_len, override);
    const char *home = getenv("HOME");
    if (!home || !*home) return false;
    int n = snprintf(out, out_len, "%s/.dsco/activation/lease.json", home);
    return n > 0 && (size_t)n < out_len;
}

activation_lease_status_t activation_lease_validate(const activation_lease_t *lease,
                                                    int64_t now_unix, char *err,
                                                    size_t err_len) {
    if (!lease) {
        lease_set_err(err, err_len, "lease is null");
        return ACTIVATION_LEASE_ERR_INVALID;
    }
    if (lease->schema_version != DSCO_ACTIVATION_LEASE_SCHEMA_VERSION) {
        lease_set_err(err, err_len, "unsupported lease schema_version");
        return ACTIVATION_LEASE_ERR_INVALID;
    }
    if (!lease->lease_id[0] || !lease->subject[0] || !lease->plan[0]) {
        lease_set_err(err, err_len, "lease_id, subject, and plan are required");
        return ACTIVATION_LEASE_ERR_INVALID;
    }
    if (lease->issued_at < 0 || lease->expires_at < 0) {
        lease_set_err(err, err_len, "lease timestamps must be non-negative");
        return ACTIVATION_LEASE_ERR_INVALID;
    }
    if (lease->expires_at && lease->issued_at && lease->expires_at < lease->issued_at) {
        lease_set_err(err, err_len, "lease expires before issue time");
        return ACTIVATION_LEASE_ERR_INVALID;
    }
    if (lease->runtime_spec_sha256[0] && !lease_sha256_hex(lease->runtime_spec_sha256)) {
        lease_set_err(err, err_len, "runtime_spec_sha256 must be a SHA-256 hex digest");
        return ACTIVATION_LEASE_ERR_INVALID;
    }
    if (now_unix > 0 && lease->expires_at > 0 && lease->expires_at <= now_unix) {
        lease_set_err(err, err_len, "lease expired");
        return ACTIVATION_LEASE_ERR_EXPIRED;
    }
    lease_set_err(err, err_len, "ok");
    return ACTIVATION_LEASE_OK;
}

static bool lease_signed_payload(const activation_lease_t *lease, char *out, size_t out_len) {
    if (!lease || !out || !out_len) return false;
    /* Length-prefixed canonical encoding avoids JSON serialization ambiguity. */
    int n = snprintf(out, out_len, "v1|%zu:%s|%zu:%s|%zu:%s|%zu:%s|%zu:%s|%lld|%lld",
                     strlen(lease->lease_id), lease->lease_id,
                     strlen(lease->subject), lease->subject,
                     strlen(lease->plan), lease->plan,
                     strlen(lease->issuer), lease->issuer,
                     strlen(lease->scopes), lease->scopes,
                     (long long)lease->issued_at, (long long)lease->expires_at);
    if (n <= 0 || (size_t)n >= out_len) return false;
    /* Keep v1 local leases verifiable while binding every cloud lease that
     * carries a RuntimeSpec digest into its issuer-signed payload. */
    if (lease->runtime_spec_sha256[0]) {
        size_t used = (size_t)n;
        n = snprintf(out + used, out_len - used, "|%zu:%s",
                     strlen(lease->runtime_spec_sha256), lease->runtime_spec_sha256);
        if (n <= 0 || (size_t)n >= out_len - used) return false;
        return true;
    }
    return true;
}

activation_lease_status_t activation_lease_verify_signature(const activation_lease_t *lease,
                                                            char *err, size_t err_len) {
    if (activation_lease_validate(lease, lease_now(), err, err_len) != ACTIVATION_LEASE_OK)
        return ACTIVATION_LEASE_ERR_INVALID;
    if (!lease->signature[0] || !DSCO_ACTIVATION_ISSUER_P256_B64[0]) {
        lease_set_err(err, err_len, "issuer key or detached signature missing");
        return ACTIVATION_LEASE_ERR_SIGNATURE;
    }
    uint8_t pub[80], sig[128];
    size_t pub_len = base64url_decode(DSCO_ACTIVATION_ISSUER_P256_B64,
                                      strlen(DSCO_ACTIVATION_ISSUER_P256_B64), pub, sizeof(pub));
    size_t sig_len = base64url_decode(lease->signature, strlen(lease->signature), sig, sizeof(sig));
    /* Security's ECDSASignature*X962 algorithms consume ASN.1 DER ECDSA
     * signatures, not the RFC 4754 fixed-width r||s representation. */
    if (pub_len != 65 || pub[0] != 0x04 || sig_len < 8 || sig_len > 80) {
        lease_set_err(err, err_len, "invalid pinned P-256 key or signature encoding");
        return ACTIVATION_LEASE_ERR_SIGNATURE;
    }
    char payload[2048];
    if (!lease_signed_payload(lease, payload, sizeof(payload))) {
        lease_set_err(err, err_len, "lease signed payload invalid");
        return ACTIVATION_LEASE_ERR_SIGNATURE;
    }
#if defined(__APPLE__)
    const void *keys[] = {kSecAttrKeyType, kSecAttrKeyClass, kSecAttrKeySizeInBits};
    const void *vals[] = {kSecAttrKeyTypeECSECPrimeRandom, kSecAttrKeyClassPublic,
                          CFNumberCreate(NULL, kCFNumberIntType, &(int){256})};
    CFDictionaryRef attrs = CFDictionaryCreate(NULL, keys, vals, 3,
                                               &kCFTypeDictionaryKeyCallBacks,
                                               &kCFTypeDictionaryValueCallBacks);
    CFRelease(vals[2]);
    CFDataRef key_data = CFDataCreate(NULL, pub, (CFIndex)pub_len);
    CFErrorRef cferr = NULL;
    SecKeyRef key = SecKeyCreateWithData(key_data, attrs, &cferr);
    CFRelease(key_data); CFRelease(attrs);
    if (!key) {
        if (cferr) CFRelease(cferr);
        lease_set_err(err, err_len, "pinned issuer key rejected by Security framework");
        return ACTIVATION_LEASE_ERR_SIGNATURE;
    }
    CFDataRef message = CFDataCreate(NULL, (const UInt8 *)payload, (CFIndex)strlen(payload));
    CFDataRef signature = CFDataCreate(NULL, sig, (CFIndex)sig_len);
    Boolean ok = SecKeyVerifySignature(key, kSecKeyAlgorithmECDSASignatureMessageX962SHA256,
                                       message, signature, &cferr);
    CFRelease(message); CFRelease(signature); CFRelease(key);
    if (cferr) CFRelease(cferr);
    if (!ok) {
        lease_set_err(err, err_len, "activation lease signature verification failed");
        return ACTIVATION_LEASE_ERR_SIGNATURE;
    }
    lease_set_err(err, err_len, "ok");
    return ACTIVATION_LEASE_OK;
#else
    (void)payload;
    lease_set_err(err, err_len, "P-256 lease verification unavailable on this platform");
    return ACTIVATION_LEASE_ERR_SIGNATURE;
#endif
}

int activation_lease_to_json(const activation_lease_t *lease, char *out, size_t out_len) {
    if (!lease || !out || out_len == 0) return -1;
    char id[DSCO_ACTIVATION_LEASE_ID_MAX * 2];
    char subject[DSCO_ACTIVATION_SUBJECT_MAX * 2];
    char plan[DSCO_ACTIVATION_PLAN_MAX * 2];
    char issuer[DSCO_ACTIVATION_ISSUER_MAX * 2];
    char scopes[DSCO_ACTIVATION_SCOPE_MAX * 2];
    char runtime_spec_sha256[DSCO_RUNTIME_SPEC_SHA256_MAX * 2];
    char sig[DSCO_ACTIVATION_SIG_MAX * 2];
    if (lease_json_escape(lease->lease_id, id, sizeof(id)) < 0 ||
        lease_json_escape(lease->subject, subject, sizeof(subject)) < 0 ||
        lease_json_escape(lease->plan, plan, sizeof(plan)) < 0 ||
        lease_json_escape(lease->issuer, issuer, sizeof(issuer)) < 0 ||
        lease_json_escape(lease->scopes, scopes, sizeof(scopes)) < 0 ||
        lease_json_escape(lease->runtime_spec_sha256, runtime_spec_sha256, sizeof(runtime_spec_sha256)) < 0 ||
        lease_json_escape(lease->signature, sig, sizeof(sig)) < 0) {
        return -1;
    }
    return snprintf(out, out_len,
                    "{\n"
                    "  \"$schema\": \"%s\",\n"
                    "  \"schema_version\": %d,\n"
                    "  \"lease_id\": \"%s\",\n"
                    "  \"subject\": \"%s\",\n"
                    "  \"plan\": \"%s\",\n"
                    "  \"issuer\": \"%s\",\n"
                    "  \"scopes\": \"%s\",\n"
                    "  \"issued_at\": %lld,\n"
                    "  \"expires_at\": %lld,\n"
                    "  \"runtime_spec_sha256\": \"%s\",\n"
                    "  \"signature\": \"%s\"\n"
                    "}\n",
                    DSCO_ACTIVATION_LEASE_SCHEMA_ID,
                    lease->schema_version, id, subject, plan, issuer, scopes,
                    (long long)lease->issued_at, (long long)lease->expires_at,
                    runtime_spec_sha256, sig);
}

activation_lease_status_t activation_lease_from_json(const char *json,
                                                     activation_lease_t *out) {
    if (!json || !out) return ACTIVATION_LEASE_ERR_INVALID;
    memset(out, 0, sizeof(*out));
    if (!lease_json_int(json, "schema_version", &out->schema_version)) return ACTIVATION_LEASE_ERR_PARSE;
    if (!lease_json_string(json, "lease_id", out->lease_id, sizeof(out->lease_id))) return ACTIVATION_LEASE_ERR_PARSE;
    if (!lease_json_string(json, "subject", out->subject, sizeof(out->subject))) return ACTIVATION_LEASE_ERR_PARSE;
    (void)lease_json_string(json, "plan", out->plan, sizeof(out->plan));
    (void)lease_json_string(json, "issuer", out->issuer, sizeof(out->issuer));
    (void)lease_json_string(json, "scopes", out->scopes, sizeof(out->scopes));
    if (!lease_json_i64(json, "issued_at", &out->issued_at)) return ACTIVATION_LEASE_ERR_PARSE;
    if (!lease_json_i64(json, "expires_at", &out->expires_at)) return ACTIVATION_LEASE_ERR_PARSE;
    (void)lease_json_string(json, "runtime_spec_sha256", out->runtime_spec_sha256,
                            sizeof(out->runtime_spec_sha256));
    (void)lease_json_string(json, "signature", out->signature, sizeof(out->signature));
    return ACTIVATION_LEASE_OK;
}

activation_lease_status_t activation_lease_save_file(const char *path,
                                                     const activation_lease_t *lease) {
    if (!path || !lease) return ACTIVATION_LEASE_ERR_INVALID;
    activation_lease_status_t st = activation_lease_validate(lease, 0, NULL, 0);
    if (st != ACTIVATION_LEASE_OK) return st;
    if (!lease_mkdir_p_parent(path)) return ACTIVATION_LEASE_ERR_IO;

    char json[4096];
    int n = activation_lease_to_json(lease, json, sizeof(json));
    if (n < 0) return ACTIVATION_LEASE_ERR_INVALID;
    if ((size_t)n >= sizeof(json)) return ACTIVATION_LEASE_ERR_TRUNCATED;

    char tmp[DSCO_ACTIVATION_PATH_MAX + 32];
    int tn = snprintf(tmp, sizeof(tmp), "%s.tmp.%ld", path, (long)getpid());
    if (tn <= 0 || (size_t)tn >= sizeof(tmp)) return ACTIVATION_LEASE_ERR_INVALID;

    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return ACTIVATION_LEASE_ERR_IO;
    const char *p = json;
    size_t left = (size_t)n;
    while (left > 0) {
        ssize_t w = write(fd, p, left);
        if (w < 0) {
            if (errno == EINTR) continue;
            close(fd);
            unlink(tmp);
            return ACTIVATION_LEASE_ERR_IO;
        }
        p += w;
        left -= (size_t)w;
    }
    if (fsync(fd) != 0) {
        close(fd);
        unlink(tmp);
        return ACTIVATION_LEASE_ERR_IO;
    }
    if (close(fd) != 0) {
        unlink(tmp);
        return ACTIVATION_LEASE_ERR_IO;
    }
    if (rename(tmp, path) != 0) {
        unlink(tmp);
        return ACTIVATION_LEASE_ERR_IO;
    }
    lease_fsync_parent(path);
    return ACTIVATION_LEASE_OK;
}

activation_lease_status_t activation_lease_load_file(const char *path,
                                                     activation_lease_t *out,
                                                     char *err, size_t err_len) {
    if (!path || !out) return ACTIVATION_LEASE_ERR_INVALID;
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        lease_set_err(err, err_len, strerror(errno));
        return ACTIVATION_LEASE_ERR_IO;
    }
    char buf[4096];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    int saved_errno = errno;
    close(fd);
    if (n < 0) {
        errno = saved_errno;
        lease_set_err(err, err_len, strerror(errno));
        return ACTIVATION_LEASE_ERR_IO;
    }
    if ((size_t)n >= sizeof(buf) - 1) {
        lease_set_err(err, err_len, "lease file too large");
        return ACTIVATION_LEASE_ERR_TRUNCATED;
    }
    buf[n] = '\0';
    activation_lease_status_t st = activation_lease_from_json(buf, out);
    if (st != ACTIVATION_LEASE_OK) {
        lease_set_err(err, err_len, "could not parse lease json");
        return st;
    }
    return activation_lease_validate(out, lease_now(), err, err_len);
}

activation_lease_status_t activation_lease_load_file_verified(const char *path,
                                                              activation_lease_t *out,
                                                              char *err, size_t err_len) {
    activation_lease_status_t st = activation_lease_load_file(path, out, err, err_len);
    if (st != ACTIVATION_LEASE_OK) return st;
    return activation_lease_verify_signature(out, err, err_len);
}

activation_lease_status_t activation_lease_remove_file(const char *path) {
    if (!path) return ACTIVATION_LEASE_ERR_INVALID;
    if (unlink(path) != 0 && errno != ENOENT) return ACTIVATION_LEASE_ERR_IO;
    lease_fsync_parent(path);
    return ACTIVATION_LEASE_OK;
}

activation_lease_status_t activation_lease_acquire_file(const char *path,
                                                       const activation_lease_t *lease,
                                                       bool replace_expired,
                                                       char *err, size_t err_len) {
    if (!path || !lease) return ACTIVATION_LEASE_ERR_INVALID;
    activation_lease_t current;
    char load_err[128];
    activation_lease_status_t st = activation_lease_load_file(path, &current, load_err, sizeof(load_err));
    if (st == ACTIVATION_LEASE_OK) {
        lease_set_err(err, err_len, "active lease already exists");
        return ACTIVATION_LEASE_ERR_INVALID;
    }
    if (st == ACTIVATION_LEASE_ERR_EXPIRED) {
        if (!replace_expired) {
            lease_set_err(err, err_len, "expired lease exists");
            return st;
        }
        return activation_lease_save_file(path, lease);
    }
    if (st != ACTIVATION_LEASE_ERR_IO) {
        lease_set_err(err, err_len, load_err);
        return st;
    }
    st = activation_lease_save_file(path, lease);
    lease_set_err(err, err_len, st == ACTIVATION_LEASE_OK ? "ok" : "could not write lease");
    return st;
}

activation_lease_status_t activation_lease_renew_file(const char *path,
                                                     const char *lease_id,
                                                     int64_t new_expires_at,
                                                     char *err, size_t err_len) {
    if (!path || new_expires_at < 0) return ACTIVATION_LEASE_ERR_INVALID;
    activation_lease_t lease;
    activation_lease_status_t st = activation_lease_load_file(path, &lease, err, err_len);
    if (st != ACTIVATION_LEASE_OK) return st;
    if (lease_id && *lease_id && strcmp(lease.lease_id, lease_id) != 0) {
        lease_set_err(err, err_len, "lease_id mismatch");
        return ACTIVATION_LEASE_ERR_INVALID;
    }
    lease.expires_at = new_expires_at;
    st = activation_lease_save_file(path, &lease);
    lease_set_err(err, err_len, st == ACTIVATION_LEASE_OK ? "ok" : "could not renew lease");
    return st;
}

activation_lease_status_t activation_lease_release_file(const char *path,
                                                       const char *lease_id,
                                                       char *err, size_t err_len) {
    if (!path) return ACTIVATION_LEASE_ERR_INVALID;
    if (lease_id && *lease_id) {
        activation_lease_t lease;
        activation_lease_status_t st = activation_lease_load_file(path, &lease, err, err_len);
        if (st != ACTIVATION_LEASE_OK && st != ACTIVATION_LEASE_ERR_EXPIRED) return st;
        if (strcmp(lease.lease_id, lease_id) != 0) {
            lease_set_err(err, err_len, "lease_id mismatch");
            return ACTIVATION_LEASE_ERR_INVALID;
        }
    }
    activation_lease_status_t st = activation_lease_remove_file(path);
    lease_set_err(err, err_len, st == ACTIVATION_LEASE_OK ? "ok" : "could not release lease");
    return st;
}
