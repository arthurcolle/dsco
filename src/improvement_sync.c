/* improvement_sync.c — content-addressed improvement exchange over DHT + mesh.
 *
 * DHT records are provider hints, never trusted content. Bundle bytes are sent
 * in bounded chunks over the encrypted mesh and accepted only after full
 * SHA-256 and Ed25519 verification. Unknown signers remain quarantined and the
 * module deliberately has no apply/execute operation. */

#include "improvement_sync.h"

#include "crypto.h"
#include "dsco_dht.h"
#include "json_util.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

static void info_json(jbuf_t *b, const improvement_bundle_info_t *info) {
    jbuf_append(b, "{\"hash\":"); jbuf_append_json_str(b, info->hash);
    jbuf_append(b, ",\"signer\":"); jbuf_append_json_str(b, info->signer);
    jbuf_append(b, ",\"name\":"); jbuf_append_json_str(b, info->name);
    jbuf_append(b, ",\"kind\":"); jbuf_append_json_str(b, info->kind);
    jbuf_append(b, ",\"base_version\":"); jbuf_append_json_str(b, info->base_version);
    jbuf_append(b, ",\"target_version\":"); jbuf_append_json_str(b, info->target_version);
    jbuf_append(b, ",\"description\":"); jbuf_append_json_str(b, info->description);
    jbuf_append(b, ",\"status\":"); jbuf_append_json_str(b, info->status);
    jbuf_appendf(b, ",\"created_unix\":%llu,\"payload_bytes\":%llu,"
                    "\"signature_valid\":%s,\"trusted\":%s}",
                 (unsigned long long)info->created_unix,
                 (unsigned long long)info->payload_bytes,
                 info->signature_valid ? "true" : "false",
                 info->trusted ? "true" : "false");
}

#ifdef HAVE_LIBSODIUM

#include "mesh.h"
#include <sodium.h>

#define IMPROVEMENT_BUNDLE_MAGIC "DSCB"
#define IMPROVEMENT_BUNDLE_VERSION 1u
#define IMPROVEMENT_BUNDLE_HEADER 128u
#define IMPROVEMENT_SIGNATURE_OFFSET 64u
#define IMPROVEMENT_SIGNATURE_BYTES crypto_sign_BYTES
#define IMPROVEMENT_IDENTITY_BYTES (crypto_sign_PUBLICKEYBYTES + crypto_sign_SECRETKEYBYTES)

#define IMPROVEMENT_WIRE_MAGIC "DSI1"
#define IMPROVEMENT_WIRE_HEADER 60u
#define IMPROVEMENT_CHUNK_BYTES (64u * 1024u)
#define IMPROVEMENT_SEEN_MAX 256

enum improvement_wire_type {
    IMPROVEMENT_WIRE_ADVERTISE = 1,
    IMPROVEMENT_WIRE_REQUEST = 2,
    IMPROVEMENT_WIRE_CHUNK = 3,
};

typedef struct {
    char hash[65];
    uint64_t bytes;
    time_t seen_at;
    uint8_t provider[MESH_PUBKEY_LEN];
} improvement_seen_t;

struct improvement_sync {
    mesh_node_t *mesh;
    char root[PATH_MAX];
    uint8_t signer_pk[crypto_sign_PUBLICKEYBYTES];
    uint8_t signer_sk[crypto_sign_SECRETKEYBYTES];
    bool identity_ready;

    pthread_mutex_t lock;
    pthread_cond_t changed;
    bool running;
    pthread_t announce_thread;
    bool announce_thread_started;

    improvement_seen_t seen[IMPROVEMENT_SEEN_MAX];
    int seen_count;
    uint64_t published;
    uint64_t fetched;
    uint64_t quarantined;
    uint64_t rejected;
    uint64_t served_chunks;
    uint64_t received_chunks;

    bool fetch_active;
    bool fetch_complete;
    bool fetch_failed;
    char fetch_hash[65];
    uint8_t fetch_hash_raw[32];
    uint8_t fetch_peer[MESH_PUBKEY_LEN];
    bool fetch_peer_set;
    uint64_t fetch_total;
    uint64_t fetch_received;
    int fetch_fd;
    char fetch_path[PATH_MAX];
    char fetch_error[192];
};

static improvement_sync_t *g_improvement_sync;

static void put_u16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static uint16_t get_u16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static void put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint32_t get_u32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void put_u64(uint8_t *p, uint64_t v) {
    for (int i = 7; i >= 0; i--) {
        p[i] = (uint8_t)v;
        v >>= 8;
    }
}

static uint64_t get_u64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++)
        v = (v << 8) | p[i];
    return v;
}

static bool hash_is_hex(const char *hash) {
    if (!hash || strlen(hash) != 64)
        return false;
    for (int i = 0; i < 64; i++)
        if (!isxdigit((unsigned char)hash[i]))
            return false;
    return true;
}

static bool mkdir_one(const char *path) {
    if (mkdir(path, 0700) == 0)
        return true;
    if (errno != EEXIST)
        return false;
    struct stat st;
    return lstat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool ensure_store(improvement_sync_t *sync, char *err, size_t err_len) {
    const char *dirs[] = {"objects", "staged", "quarantine", "partials", NULL};
    char parent[PATH_MAX];
    snprintf(parent, sizeof(parent), "%s", sync->root);
    char *slash = strrchr(parent, '/');
    if (slash && slash != parent) {
        *slash = '\0';
        if (!mkdir_one(parent) && errno != EEXIST) {
            snprintf(err, err_len, "cannot create store parent: %s", parent);
            return false;
        }
    }
    if (!mkdir_one(sync->root)) {
        snprintf(err, err_len, "cannot create improvement store: %s", sync->root);
        return false;
    }
    for (int i = 0; dirs[i]; i++) {
        char path[PATH_MAX];
        if (snprintf(path, sizeof(path), "%s/%s", sync->root, dirs[i]) >= (int)sizeof(path) ||
            !mkdir_one(path)) {
            snprintf(err, err_len, "cannot create improvement directory: %s", dirs[i]);
            return false;
        }
    }
    return true;
}

static bool write_all(int fd, const uint8_t *data, size_t len) {
    while (len) {
        ssize_t n = write(fd, data, len);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return false;
        data += (size_t)n;
        len -= (size_t)n;
    }
    return true;
}

static bool read_all_fd(int fd, uint8_t *data, size_t len) {
    while (len) {
        ssize_t n = read(fd, data, len);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return false;
        data += (size_t)n;
        len -= (size_t)n;
    }
    return true;
}

static bool file_matches_bytes(const char *path, const uint8_t *data, size_t len) {
    struct stat st;
    if (lstat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size != (off_t)len)
        return false;
    int fd = open(path, O_RDONLY
#ifdef O_CLOEXEC
                  | O_CLOEXEC
#endif
#ifdef O_NOFOLLOW
                  | O_NOFOLLOW
#endif
    );
    if (fd < 0)
        return false;
    uint8_t buf[4096];
    size_t offset = 0;
    bool ok = true;
    while (offset < len) {
        size_t want = len - offset;
        if (want > sizeof(buf))
            want = sizeof(buf);
        ssize_t n = read(fd, buf, want);
        if (n <= 0 || memcmp(buf, data + offset, (size_t)n) != 0) {
            ok = false;
            break;
        }
        offset += (size_t)n;
    }
    if (close(fd) != 0)
        ok = false;
    return ok && offset == len;
}

static bool atomic_write(const char *path, const uint8_t *data, size_t len,
                         char *err, size_t err_len) {
    if (access(path, F_OK) == 0) {
        bool same = file_matches_bytes(path, data, len);
        if (!same)
            snprintf(err, err_len, "existing content-addressed file differs");
        return same;
    }
    char tmp[PATH_MAX];
    if (snprintf(tmp, sizeof(tmp), "%s.tmp.%ld.%08x", path, (long)getpid(),
                 randombytes_random()) >= (int)sizeof(tmp)) {
        snprintf(err, err_len, "destination path too long");
        return false;
    }
    int flags = O_WRONLY | O_CREAT | O_EXCL;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    int fd = open(tmp, flags, 0600);
    if (fd < 0) {
        snprintf(err, err_len, "cannot create temporary bundle: %s", strerror(errno));
        return false;
    }
    bool ok = write_all(fd, data, len);
    if (ok && fsync(fd) != 0)
        ok = false;
    if (close(fd) != 0)
        ok = false;
    if (!ok) {
        int saved = errno;
        unlink(tmp);
        snprintf(err, err_len, "cannot persist bundle: %s", strerror(saved));
        return false;
    }
    /* Hard-link commit gives create-if-absent semantics; rename(2) would
     * replace a concurrently-created identity or immutable object. */
    if (link(tmp, path) != 0) {
        int saved = errno;
        unlink(tmp);
        if (saved == EEXIST) {
            bool same = file_matches_bytes(path, data, len);
            if (!same)
                snprintf(err, err_len, "concurrent content-address collision");
            return same;
        }
        snprintf(err, err_len, "cannot commit bundle: %s", strerror(saved));
        return false;
    }
    unlink(tmp);
    return true;
}

static bool load_file_bounded(const char *path, uint8_t **out, size_t *out_len,
                              char *err, size_t err_len) {
    *out = NULL;
    *out_len = 0;
    struct stat st;
    if (lstat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        snprintf(err, err_len, "not a regular file: %s", path);
        return false;
    }
    if (st.st_size < 0 || (uint64_t)st.st_size > IMPROVEMENT_MAX_BUNDLE_BYTES) {
        snprintf(err, err_len, "file exceeds %u-byte improvement limit",
                 (unsigned)IMPROVEMENT_MAX_BUNDLE_BYTES);
        return false;
    }
    int fd = open(path, O_RDONLY
#ifdef O_CLOEXEC
                  | O_CLOEXEC
#endif
#ifdef O_NOFOLLOW
                  | O_NOFOLLOW
#endif
    );
    if (fd < 0) {
        snprintf(err, err_len, "cannot open %s: %s", path, strerror(errno));
        return false;
    }
    size_t len = (size_t)st.st_size;
    uint8_t *data = malloc(len ? len : 1);
    bool ok = data && read_all_fd(fd, data, len);
    close(fd);
    if (!ok) {
        free(data);
        snprintf(err, err_len, "cannot read %s", path);
        return false;
    }
    *out = data;
    *out_len = len;
    return true;
}

static bool load_identity(improvement_sync_t *sync, char *err, size_t err_len) {
    char path[PATH_MAX];
    if (snprintf(path, sizeof(path), "%s/identity.ed25519", sync->root) >= (int)sizeof(path)) {
        snprintf(err, err_len, "identity path too long");
        return false;
    }
    uint8_t key[IMPROVEMENT_IDENTITY_BYTES];
    int fd = open(path, O_RDONLY
#ifdef O_CLOEXEC
                  | O_CLOEXEC
#endif
#ifdef O_NOFOLLOW
                  | O_NOFOLLOW
#endif
    );
    if (fd >= 0) {
        struct stat st;
        bool ok = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) &&
                  (st.st_mode & 077) == 0 && st.st_size == (off_t)sizeof(key) &&
                  read_all_fd(fd, key, sizeof(key));
        close(fd);
        if (!ok) {
            sodium_memzero(key, sizeof(key));
            snprintf(err, err_len, "identity key has unsafe mode or invalid size");
            return false;
        }
        memcpy(sync->signer_pk, key, crypto_sign_PUBLICKEYBYTES);
        memcpy(sync->signer_sk, key + crypto_sign_PUBLICKEYBYTES, crypto_sign_SECRETKEYBYTES);
        uint8_t derived[crypto_sign_PUBLICKEYBYTES];
        bool valid = crypto_sign_ed25519_sk_to_pk(derived, sync->signer_sk) == 0 &&
                     sodium_memcmp(derived, sync->signer_pk, sizeof(derived)) == 0;
        sodium_memzero(derived, sizeof(derived));
        sodium_memzero(key, sizeof(key));
        if (!valid) {
            snprintf(err, err_len, "identity keypair failed consistency check");
            return false;
        }
        sync->identity_ready = true;
        return true;
    }
    if (errno != ENOENT) {
        snprintf(err, err_len, "cannot open identity: %s", strerror(errno));
        return false;
    }

    crypto_sign_keypair(sync->signer_pk, sync->signer_sk);
    memcpy(key, sync->signer_pk, crypto_sign_PUBLICKEYBYTES);
    memcpy(key + crypto_sign_PUBLICKEYBYTES, sync->signer_sk, crypto_sign_SECRETKEYBYTES);
    if (!atomic_write(path, key, sizeof(key), err, err_len)) {
        sodium_memzero(key, sizeof(key));
        sodium_memzero(sync->signer_sk, sizeof(sync->signer_sk));
        return false;
    }
    sodium_memzero(key, sizeof(key));
    sodium_memzero(sync->signer_sk, sizeof(sync->signer_sk));
    /* Re-open the committed key so concurrent first-start initialization can
     * never leave memory using a different identity than the file on disk. */
    return load_identity(sync, err, err_len);
}

static uint8_t kind_code(const char *kind) {
    if (!kind || !*kind || strcmp(kind, "patch") == 0)
        return 1;
    if (strcmp(kind, "source") == 0)
        return 2;
    if (strcmp(kind, "binary") == 0)
        return 3;
    if (strcmp(kind, "config") == 0)
        return 4;
    return 0;
}

static const char *kind_name(uint8_t kind) {
    switch (kind) {
        case 1: return "patch";
        case 2: return "source";
        case 3: return "binary";
        case 4: return "config";
        default: return "unknown";
    }
}

static bool signer_trusted(improvement_sync_t *sync, const uint8_t signer[32]) {
    if (sync->identity_ready && sodium_memcmp(signer, sync->signer_pk, 32) == 0)
        return true;
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/trusted_signers", sync->root);
    int fd = open(path, O_RDONLY
#ifdef O_CLOEXEC
                  | O_CLOEXEC
#endif
#ifdef O_NOFOLLOW
                  | O_NOFOLLOW
#endif
    );
    if (fd < 0)
        return false;
    FILE *f = fdopen(fd, "r");
    if (!f) {
        close(fd);
        return false;
    }
    char expected[65];
    hex_encode(signer, 32, expected);
    char line[160];
    bool found = false;
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n #\t")] = '\0';
        if (strcasecmp(line, expected) == 0) {
            found = true;
            break;
        }
    }
    fclose(f);
    return found;
}

static void copy_field(char *dst, size_t dst_len, const uint8_t *src, size_t src_len) {
    if (!dst_len)
        return;
    size_t n = src_len < dst_len - 1 ? src_len : dst_len - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static bool verify_bundle_bytes(improvement_sync_t *sync, uint8_t *data, size_t len,
                                const char *expected_hash, const char *status,
                                improvement_bundle_info_t *out,
                                char *err, size_t err_len) {
    if (len < IMPROVEMENT_BUNDLE_HEADER || len > IMPROVEMENT_MAX_BUNDLE_BYTES ||
        memcmp(data, IMPROVEMENT_BUNDLE_MAGIC, 4) != 0 ||
        data[4] != IMPROVEMENT_BUNDLE_VERSION) {
        snprintf(err, err_len, "invalid improvement bundle header");
        return false;
    }
    uint16_t name_len = get_u16(data + 24);
    uint16_t base_len = get_u16(data + 26);
    uint16_t target_len = get_u16(data + 28);
    uint16_t desc_len = get_u16(data + 30);
    uint64_t payload_len = get_u64(data + 16);
    uint64_t meta_len = (uint64_t)name_len + base_len + target_len + desc_len;
    if (!name_len || name_len > 128 || base_len > 64 || target_len > 64 || desc_len > 512 ||
        payload_len > IMPROVEMENT_MAX_BUNDLE_BYTES ||
        IMPROVEMENT_BUNDLE_HEADER + meta_len + payload_len != len) {
        snprintf(err, err_len, "invalid improvement bundle lengths");
        return false;
    }

    char actual_hash[65];
    sha256_hex(data, len, actual_hash);
    if (expected_hash && strcasecmp(actual_hash, expected_hash) != 0) {
        snprintf(err, err_len, "bundle SHA-256 mismatch");
        return false;
    }

    uint8_t signature[IMPROVEMENT_SIGNATURE_BYTES];
    memcpy(signature, data + IMPROVEMENT_SIGNATURE_OFFSET, sizeof(signature));
    memset(data + IMPROVEMENT_SIGNATURE_OFFSET, 0, sizeof(signature));
    int sig_ok = crypto_sign_verify_detached(signature, data, (unsigned long long)len, data + 32);
    memcpy(data + IMPROVEMENT_SIGNATURE_OFFSET, signature, sizeof(signature));
    sodium_memzero(signature, sizeof(signature));
    if (sig_ok != 0) {
        snprintf(err, err_len, "bundle Ed25519 signature invalid");
        return false;
    }

    if (out) {
        memset(out, 0, sizeof(*out));
        snprintf(out->hash, sizeof(out->hash), "%s", actual_hash);
        hex_encode(data + 32, 32, out->signer);
        const uint8_t *p = data + IMPROVEMENT_BUNDLE_HEADER;
        copy_field(out->name, sizeof(out->name), p, name_len); p += name_len;
        copy_field(out->base_version, sizeof(out->base_version), p, base_len); p += base_len;
        copy_field(out->target_version, sizeof(out->target_version), p, target_len); p += target_len;
        copy_field(out->description, sizeof(out->description), p, desc_len);
        snprintf(out->kind, sizeof(out->kind), "%s", kind_name(data[5]));
        snprintf(out->status, sizeof(out->status), "%s", status ? status : "unknown");
        out->created_unix = get_u64(data + 8);
        out->payload_bytes = payload_len;
        out->signature_valid = true;
        out->trusted = signer_trusted(sync, data + 32);
    }
    return true;
}

static bool bundle_path(improvement_sync_t *sync, const char *status, const char *hash,
                        char out[PATH_MAX]) {
    return snprintf(out, PATH_MAX, "%s/%s/%s.bundle", sync->root, status, hash) < PATH_MAX;
}

static bool inspect_path(improvement_sync_t *sync, const char *path, const char *expected_hash,
                         const char *status, improvement_bundle_info_t *out,
                         char *err, size_t err_len) {
    uint8_t *data = NULL;
    size_t len = 0;
    if (!load_file_bounded(path, &data, &len, err, err_len))
        return false;
    bool ok = verify_bundle_bytes(sync, data, len, expected_hash, status, out, err, err_len);
    free(data);
    return ok;
}

static bool locate_bundle(improvement_sync_t *sync, const char *hash, bool serve_only,
                          char path[PATH_MAX], char status[17]) {
    const char *dirs_all[] = {"objects", "staged", "quarantine", NULL};
    const char *dirs_serve[] = {"objects", "staged", NULL};
    const char **dirs = serve_only ? dirs_serve : dirs_all;
    for (int i = 0; dirs[i]; i++) {
        if (!bundle_path(sync, dirs[i], hash, path))
            return false;
        if (access(path, R_OK) == 0) {
            snprintf(status, 17, "%s", dirs[i][0] == 'o' ? "object" : dirs[i]);
            return true;
        }
    }
    return false;
}

static bool protocol_send(improvement_sync_t *sync, const uint8_t *peer, uint8_t type,
                          const uint8_t hash[32], uint64_t offset, uint64_t total,
                          const uint8_t *chunk, uint32_t chunk_len) {
    if (!sync || !sync->mesh || chunk_len > IMPROVEMENT_CHUNK_BYTES)
        return false;
    size_t len = IMPROVEMENT_WIRE_HEADER + chunk_len;
    uint8_t *wire = malloc(len);
    if (!wire)
        return false;
    memcpy(wire, IMPROVEMENT_WIRE_MAGIC, 4);
    wire[4] = type;
    memset(wire + 5, 0, 3);
    memcpy(wire + 8, hash, 32);
    put_u64(wire + 40, offset);
    put_u64(wire + 48, total);
    put_u32(wire + 56, chunk_len);
    if (chunk_len)
        memcpy(wire + IMPROVEMENT_WIRE_HEADER, chunk, chunk_len);
    bool ok = peer ? mesh_node_send_to(sync->mesh, peer, wire, len)
                   : mesh_node_broadcast(sync->mesh, wire, len) >= 0;
    free(wire);
    return ok;
}

static void record_seen(improvement_sync_t *sync, const uint8_t hash[32], uint64_t bytes,
                        const uint8_t from_pk[32]) {
    char hex[65];
    hex_encode(hash, 32, hex);
    pthread_mutex_lock(&sync->lock);
    int slot = -1;
    for (int i = 0; i < sync->seen_count; i++)
        if (strcmp(sync->seen[i].hash, hex) == 0) { slot = i; break; }
    if (slot < 0) {
        slot = sync->seen_count < IMPROVEMENT_SEEN_MAX ? sync->seen_count++ : 0;
    }
    snprintf(sync->seen[slot].hash, sizeof(sync->seen[slot].hash), "%s", hex);
    sync->seen[slot].bytes = bytes;
    sync->seen[slot].seen_at = time(NULL);
    memcpy(sync->seen[slot].provider, from_pk, 32);
    pthread_mutex_unlock(&sync->lock);
}

static void serve_request(improvement_sync_t *sync, const uint8_t from_pk[32],
                          const uint8_t hash[32], uint64_t offset) {
    char hex[65], path[PATH_MAX], status[17];
    hex_encode(hash, 32, hex);
    if (!locate_bundle(sync, hex, true, path, status))
        return;
    int fd = open(path, O_RDONLY
#ifdef O_CLOEXEC
                  | O_CLOEXEC
#endif
#ifdef O_NOFOLLOW
                  | O_NOFOLLOW
#endif
    );
    if (fd < 0)
        return;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0 ||
        (uint64_t)st.st_size > IMPROVEMENT_MAX_BUNDLE_BYTES || offset >= (uint64_t)st.st_size) {
        close(fd);
        return;
    }
    uint32_t want = (uint32_t)(((uint64_t)st.st_size - offset) > IMPROVEMENT_CHUNK_BYTES
                                   ? IMPROVEMENT_CHUNK_BYTES
                                   : (uint64_t)st.st_size - offset);
    uint8_t *chunk = malloc(want);
    if (!chunk) { close(fd); return; }
    bool ok = lseek(fd, (off_t)offset, SEEK_SET) >= 0 && read_all_fd(fd, chunk, want) &&
              protocol_send(sync, from_pk, IMPROVEMENT_WIRE_CHUNK, hash, offset,
                            (uint64_t)st.st_size, chunk, want);
    close(fd);
    free(chunk);
    if (ok) {
        pthread_mutex_lock(&sync->lock);
        sync->served_chunks++;
        pthread_mutex_unlock(&sync->lock);
    }
}

static void receive_chunk(improvement_sync_t *sync, const uint8_t from_pk[32],
                          const uint8_t hash[32], uint64_t offset, uint64_t total,
                          const uint8_t *chunk, uint32_t chunk_len) {
    uint64_t next = 0;
    bool request_next = false;
    pthread_mutex_lock(&sync->lock);
    if (!sync->fetch_active || sodium_memcmp(hash, sync->fetch_hash_raw, 32) != 0 ||
        !chunk_len || chunk_len > IMPROVEMENT_CHUNK_BYTES ||
        !total || total > IMPROVEMENT_MAX_BUNDLE_BYTES || offset != sync->fetch_received ||
        (sync->fetch_peer_set && sodium_memcmp(from_pk, sync->fetch_peer, 32) != 0) ||
        offset + chunk_len > total) {
        pthread_mutex_unlock(&sync->lock);
        return;
    }
    if (!sync->fetch_peer_set) {
        memcpy(sync->fetch_peer, from_pk, 32);
        sync->fetch_peer_set = true;
        sync->fetch_total = total;
    }
    if (sync->fetch_total != total || !write_all(sync->fetch_fd, chunk, chunk_len)) {
        sync->fetch_failed = true;
        snprintf(sync->fetch_error, sizeof(sync->fetch_error), "failed writing incoming chunk");
        pthread_cond_broadcast(&sync->changed);
        pthread_mutex_unlock(&sync->lock);
        return;
    }
    sync->fetch_received += chunk_len;
    sync->received_chunks++;
    if (sync->fetch_received == total) {
        bool persisted = fsync(sync->fetch_fd) == 0;
        if (close(sync->fetch_fd) != 0)
            persisted = false;
        sync->fetch_fd = -1;
        sync->fetch_complete = persisted;
        sync->fetch_failed = !persisted;
        if (!persisted)
            snprintf(sync->fetch_error, sizeof(sync->fetch_error),
                     "failed persisting incoming bundle");
        pthread_cond_broadcast(&sync->changed);
    } else {
        next = sync->fetch_received;
        request_next = true;
    }
    pthread_mutex_unlock(&sync->lock);
    if (request_next)
        protocol_send(sync, from_pk, IMPROVEMENT_WIRE_REQUEST, hash, next, 0, NULL, 0);
}

static void improvement_mesh_message(const uint8_t *from_pk, const void *payload, size_t len,
                                     void *ctx) {
    improvement_sync_t *sync = ctx;
    const uint8_t *wire = payload;
    if (!sync || !from_pk || !wire || len < IMPROVEMENT_WIRE_HEADER ||
        memcmp(wire, IMPROVEMENT_WIRE_MAGIC, 4) != 0)
        return;
    uint8_t type = wire[4];
    const uint8_t *hash = wire + 8;
    uint64_t offset = get_u64(wire + 40);
    uint64_t total = get_u64(wire + 48);
    uint32_t chunk_len = get_u32(wire + 56);
    if (chunk_len > IMPROVEMENT_CHUNK_BYTES || IMPROVEMENT_WIRE_HEADER + chunk_len != len)
        return;
    if (type == IMPROVEMENT_WIRE_ADVERTISE && chunk_len == 0)
        record_seen(sync, hash, total, from_pk);
    else if (type == IMPROVEMENT_WIRE_REQUEST && chunk_len == 0)
        serve_request(sync, from_pk, hash, offset);
    else if (type == IMPROVEMENT_WIRE_CHUNK)
        receive_chunk(sync, from_pk, hash, offset, total,
                      wire + IMPROVEMENT_WIRE_HEADER, chunk_len);
}

static bool ingest_file(improvement_sync_t *sync, const char *path, const char *expected_hash,
                        improvement_bundle_info_t *out, char *err, size_t err_len) {
    uint8_t *data = NULL;
    size_t len = 0;
    if (!load_file_bounded(path, &data, &len, err, err_len))
        return false;
    improvement_bundle_info_t info;
    if (!verify_bundle_bytes(sync, data, len, expected_hash, "quarantine", &info, err, err_len)) {
        pthread_mutex_lock(&sync->lock); sync->rejected++; pthread_mutex_unlock(&sync->lock);
        free(data);
        return false;
    }
    const char *dir = info.trusted ? "staged" : "quarantine";
    snprintf(info.status, sizeof(info.status), "%s", dir);
    char dest[PATH_MAX];
    if (!bundle_path(sync, dir, info.hash, dest) || !atomic_write(dest, data, len, err, err_len)) {
        free(data);
        return false;
    }
    free(data);
    if (!info.trusted) {
        pthread_mutex_lock(&sync->lock); sync->quarantined++; pthread_mutex_unlock(&sync->lock);
    }
    if (out)
        *out = info;
    return true;
}

improvement_sync_t *improvement_sync_create(void *mesh_node, const char *root_override) {
    if (sodium_init() < 0)
        return NULL;
    improvement_sync_t *sync = calloc(1, sizeof(*sync));
    if (!sync)
        return NULL;
    sync->mesh = mesh_node;
    sync->fetch_fd = -1;
    pthread_mutex_init(&sync->lock, NULL);
    pthread_cond_init(&sync->changed, NULL);
    const char *root = root_override;
    if (!root || !*root)
        root = getenv("DSCO_IMPROVEMENT_ROOT");
    if (root && *root)
        snprintf(sync->root, sizeof(sync->root), "%s", root);
    else {
        const char *home = getenv("HOME");
        snprintf(sync->root, sizeof(sync->root), "%s/.dsco/improvements", home ? home : "/tmp");
    }
    char err[192];
    if (!ensure_store(sync, err, sizeof(err)) || !load_identity(sync, err, sizeof(err))) {
        improvement_sync_destroy(sync);
        return NULL;
    }
    if (sync->mesh)
        mesh_node_set_on_message(sync->mesh, improvement_mesh_message, sync);
    return sync;
}

void improvement_sync_destroy(improvement_sync_t *sync) {
    if (!sync)
        return;
    pthread_mutex_lock(&sync->lock);
    sync->running = false;
    pthread_cond_broadcast(&sync->changed);
    pthread_mutex_unlock(&sync->lock);
    if (sync->announce_thread_started)
        pthread_join(sync->announce_thread, NULL);
    if (sync->fetch_fd >= 0)
        close(sync->fetch_fd);
    if (sync->fetch_path[0])
        unlink(sync->fetch_path);
    sodium_memzero(sync->signer_sk, sizeof(sync->signer_sk));
    pthread_cond_destroy(&sync->changed);
    pthread_mutex_destroy(&sync->lock);
    free(sync);
}

bool improvement_sync_publish(improvement_sync_t *sync, const char *payload_path,
                              const char *kind, const char *name,
                              const char *base_version, const char *target_version,
                              const char *description, improvement_bundle_info_t *out,
                              char *err, size_t err_len) {
    if (!sync || !payload_path || !name || !*name) {
        snprintf(err, err_len, "sync, path, and name are required");
        return false;
    }
    uint8_t kind_id = kind_code(kind);
    size_t nl = strlen(name), bl = strlen(base_version ? base_version : ""),
           tl = strlen(target_version ? target_version : ""), dl = strlen(description ? description : "");
    if (!kind_id || nl > 128 || bl > 64 || tl > 64 || dl > 512) {
        snprintf(err, err_len, "invalid kind or metadata length");
        return false;
    }
    uint8_t *payload = NULL;
    size_t payload_len = 0;
    if (!load_file_bounded(payload_path, &payload, &payload_len, err, err_len))
        return false;
    size_t total = IMPROVEMENT_BUNDLE_HEADER + nl + bl + tl + dl + payload_len;
    if (total > IMPROVEMENT_MAX_BUNDLE_BYTES) {
        free(payload);
        snprintf(err, err_len, "bundle exceeds %u-byte limit", (unsigned)IMPROVEMENT_MAX_BUNDLE_BYTES);
        return false;
    }
    uint8_t *bundle = calloc(1, total);
    if (!bundle) { free(payload); snprintf(err, err_len, "out of memory"); return false; }
    memcpy(bundle, IMPROVEMENT_BUNDLE_MAGIC, 4);
    bundle[4] = IMPROVEMENT_BUNDLE_VERSION;
    bundle[5] = kind_id;
    put_u64(bundle + 8, (uint64_t)time(NULL));
    put_u64(bundle + 16, payload_len);
    put_u16(bundle + 24, (uint16_t)nl);
    put_u16(bundle + 26, (uint16_t)bl);
    put_u16(bundle + 28, (uint16_t)tl);
    put_u16(bundle + 30, (uint16_t)dl);
    memcpy(bundle + 32, sync->signer_pk, 32);
    uint8_t *p = bundle + IMPROVEMENT_BUNDLE_HEADER;
    memcpy(p, name, nl); p += nl;
    memcpy(p, base_version ? base_version : "", bl); p += bl;
    memcpy(p, target_version ? target_version : "", tl); p += tl;
    memcpy(p, description ? description : "", dl); p += dl;
    memcpy(p, payload, payload_len);
    free(payload);
    unsigned long long sig_len = 0;
    uint8_t signature[IMPROVEMENT_SIGNATURE_BYTES];
    if (crypto_sign_detached(signature, &sig_len, bundle, (unsigned long long)total,
                             sync->signer_sk) != 0 ||
        sig_len != IMPROVEMENT_SIGNATURE_BYTES) {
        sodium_memzero(signature, sizeof(signature));
        free(bundle); snprintf(err, err_len, "failed signing bundle"); return false;
    }
    memcpy(bundle + IMPROVEMENT_SIGNATURE_OFFSET, signature, sizeof(signature));
    sodium_memzero(signature, sizeof(signature));
    char hash[65], path[PATH_MAX];
    sha256_hex(bundle, total, hash);
    if (!bundle_path(sync, "objects", hash, path) ||
        !atomic_write(path, bundle, total, err, err_len)) {
        free(bundle); return false;
    }
    improvement_bundle_info_t info;
    bool ok = verify_bundle_bytes(sync, bundle, total, hash, "object", &info, err, err_len);
    free(bundle);
    if (!ok)
        return false;
    pthread_mutex_lock(&sync->lock); sync->published++; pthread_mutex_unlock(&sync->lock);
    uint8_t raw[32];
    hex_decode(hash, 64, raw, sizeof(raw));
    dsco_dht_t *dht = dsco_dht_global();
    if (dht)
        (void)dsco_dht_provide_hash(dht, hash);
    if (sync->mesh)
        protocol_send(sync, NULL, IMPROVEMENT_WIRE_ADVERTISE, raw, 0, total, NULL, 0);
    if (out)
        *out = info;
    return true;
}

bool improvement_sync_inspect(improvement_sync_t *sync, const char *hash_hex,
                              improvement_bundle_info_t *out, char *err, size_t err_len) {
    if (!sync || !hash_is_hex(hash_hex)) {
        snprintf(err, err_len, "hash must be 64 hexadecimal characters");
        return false;
    }
    char path[PATH_MAX], status[17];
    if (!locate_bundle(sync, hash_hex, false, path, status)) {
        snprintf(err, err_len, "bundle not found");
        return false;
    }
    return inspect_path(sync, path, hash_hex, status, out, err, err_len);
}

bool improvement_sync_trust_signer(improvement_sync_t *sync, const char *signer_hex,
                                   char *err, size_t err_len) {
    uint8_t raw[32];
    if (!sync || !hash_is_hex(signer_hex) || hex_decode(signer_hex, 64, raw, sizeof(raw)) != 32) {
        snprintf(err, err_len, "signer must be a 32-byte hexadecimal Ed25519 public key");
        return false;
    }
    if (signer_trusted(sync, raw))
        return true;
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/trusted_signers", sync->root);
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND
#ifdef O_CLOEXEC
                  | O_CLOEXEC
#endif
#ifdef O_NOFOLLOW
                  | O_NOFOLLOW
#endif
                  , 0600);
    if (fd < 0) { snprintf(err, err_len, "cannot open trust store: %s", strerror(errno)); return false; }
    char line[66];
    snprintf(line, sizeof(line), "%s\n", signer_hex);
    bool ok = write_all(fd, (const uint8_t *)line, strlen(line));
    if (ok && fsync(fd) != 0)
        ok = false;
    if (close(fd) != 0)
        ok = false;
    if (!ok)
        snprintf(err, err_len, "cannot update trust store");
    return ok;
}

bool improvement_sync_promote(improvement_sync_t *sync, const char *hash_hex,
                              improvement_bundle_info_t *out, char *err, size_t err_len) {
    if (!sync || !hash_is_hex(hash_hex)) {
        snprintf(err, err_len, "invalid bundle hash");
        return false;
    }
    char from[PATH_MAX], to[PATH_MAX];
    if (!bundle_path(sync, "quarantine", hash_hex, from) || access(from, R_OK) != 0) {
        snprintf(err, err_len, "quarantined bundle not found");
        return false;
    }
    improvement_bundle_info_t info;
    if (!inspect_path(sync, from, hash_hex, "quarantine", &info, err, err_len))
        return false;
    if (!info.trusted) {
        snprintf(err, err_len, "signer is not trusted; trust it explicitly before promotion");
        return false;
    }
    if (!bundle_path(sync, "staged", hash_hex, to)) {
        snprintf(err, err_len, "staged path too long");
        return false;
    }
    if (rename(from, to) != 0 && errno != EEXIST) {
        snprintf(err, err_len, "cannot promote bundle: %s", strerror(errno));
        return false;
    }
    if (access(to, R_OK) == 0)
        unlink(from);
    snprintf(info.status, sizeof(info.status), "staged");
    if (out)
        *out = info;
    return true;
}

bool improvement_sync_materialize(improvement_sync_t *sync, const char *hash_hex,
                                  const char *output_path, char *err, size_t err_len) {
    if (!sync || !hash_is_hex(hash_hex) || !output_path || !*output_path) {
        snprintf(err, err_len, "hash and output_path are required");
        return false;
    }
    char path[PATH_MAX], status[17];
    if (!locate_bundle(sync, hash_hex, true, path, status)) {
        snprintf(err, err_len, "trusted staged/local bundle not found");
        return false;
    }
    uint8_t *data = NULL;
    size_t len = 0;
    improvement_bundle_info_t info;
    if (!load_file_bounded(path, &data, &len, err, err_len) ||
        !verify_bundle_bytes(sync, data, len, hash_hex, status, &info, err, err_len)) {
        free(data); return false;
    }
    if (!info.trusted) { free(data); snprintf(err, err_len, "bundle signer is not trusted"); return false; }
    uint16_t nl = get_u16(data + 24), bl = get_u16(data + 26),
             tl = get_u16(data + 28), dl = get_u16(data + 30);
    size_t payload_offset = IMPROVEMENT_BUNDLE_HEADER + nl + bl + tl + dl;
    int flags = O_WRONLY | O_CREAT | O_EXCL;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    int fd = open(output_path, flags, 0600);
    bool ok = fd >= 0;
    if (ok && !write_all(fd, data + payload_offset, (size_t)info.payload_bytes))
        ok = false;
    if (fd >= 0 && ok && fsync(fd) != 0)
        ok = false;
    if (fd >= 0 && close(fd) != 0)
        ok = false;
    if (!ok) {
        int saved = errno;
        unlink(output_path);
        snprintf(err, err_len, "cannot materialize payload: %s", strerror(saved));
    }
    free(data);
    return ok;
}

bool improvement_sync_fetch(improvement_sync_t *sync, const char *hash_hex, int timeout_seconds,
                            improvement_bundle_info_t *out, char *err, size_t err_len) {
    if (!sync || !sync->mesh || !hash_is_hex(hash_hex)) {
        snprintf(err, err_len, "running mesh and valid bundle hash are required");
        return false;
    }
    if (improvement_sync_inspect(sync, hash_hex, out, err, err_len))
        return true;
    if (timeout_seconds < 1) timeout_seconds = 30;
    if (timeout_seconds > 300) timeout_seconds = 300;

    pthread_mutex_lock(&sync->lock);
    if (sync->fetch_active) {
        pthread_mutex_unlock(&sync->lock);
        snprintf(err, err_len, "another improvement fetch is active");
        return false;
    }
    sync->fetch_active = true;
    sync->fetch_complete = false;
    sync->fetch_failed = false;
    sync->fetch_peer_set = false;
    sync->fetch_total = 0;
    sync->fetch_received = 0;
    sync->fetch_error[0] = '\0';
    snprintf(sync->fetch_hash, sizeof(sync->fetch_hash), "%s", hash_hex);
    hex_decode(hash_hex, 64, sync->fetch_hash_raw, sizeof(sync->fetch_hash_raw));
    snprintf(sync->fetch_path, sizeof(sync->fetch_path), "%s/partials/%s.part.%ld.%08x",
             sync->root, hash_hex, (long)getpid(), randombytes_random());
    sync->fetch_fd = open(sync->fetch_path, O_WRONLY | O_CREAT | O_EXCL
#ifdef O_CLOEXEC
                          | O_CLOEXEC
#endif
#ifdef O_NOFOLLOW
                          | O_NOFOLLOW
#endif
                          , 0600);
    if (sync->fetch_fd < 0) {
        sync->fetch_active = false;
        pthread_mutex_unlock(&sync->lock);
        snprintf(err, err_len, "cannot create partial bundle: %s", strerror(errno));
        return false;
    }
    pthread_mutex_unlock(&sync->lock);

    dsco_dht_t *dht = dsco_dht_global();
    if (dht)
        (void)dsco_dht_find_hash(dht, hash_hex);
    time_t deadline = time(NULL) + timeout_seconds;
    bool done = false, failed = false;
    while (time(NULL) < deadline) {
        pthread_mutex_lock(&sync->lock);
        bool started = sync->fetch_received > 0;
        done = sync->fetch_complete;
        failed = sync->fetch_failed;
        pthread_mutex_unlock(&sync->lock);
        if (done || failed)
            break;
        if (!started)
            protocol_send(sync, NULL, IMPROVEMENT_WIRE_REQUEST, sync->fetch_hash_raw, 0, 0, NULL, 0);
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec++;
        pthread_mutex_lock(&sync->lock);
        if (!sync->fetch_complete && !sync->fetch_failed)
            pthread_cond_timedwait(&sync->changed, &sync->lock, &ts);
        pthread_mutex_unlock(&sync->lock);
    }

    pthread_mutex_lock(&sync->lock);
    done = sync->fetch_complete;
    failed = sync->fetch_failed;
    if (sync->fetch_fd >= 0) {
        close(sync->fetch_fd);
        sync->fetch_fd = -1;
    }
    char partial[PATH_MAX];
    snprintf(partial, sizeof(partial), "%s", sync->fetch_path);
    char failure[192];
    snprintf(failure, sizeof(failure), "%s", sync->fetch_error);
    sync->fetch_active = false;
    sync->fetch_path[0] = '\0';
    pthread_mutex_unlock(&sync->lock);
    if (!done) {
        unlink(partial);
        snprintf(err, err_len, "%s", failed && failure[0] ? failure : "provider lookup/fetch timed out");
        return false;
    }
    bool ok = ingest_file(sync, partial, hash_hex, out, err, err_len);
    unlink(partial);
    if (ok) {
        pthread_mutex_lock(&sync->lock); sync->fetched++; pthread_mutex_unlock(&sync->lock);
    }
    return ok;
}

static int count_bundles(improvement_sync_t *sync, const char *dir) {
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", sync->root, dir);
    DIR *d = opendir(path);
    if (!d) return 0;
    int count = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        size_t n = strlen(ent->d_name);
        if (n == 71 && strcmp(ent->d_name + 64, ".bundle") == 0)
            count++;
    }
    closedir(d);
    return count;
}

bool improvement_sync_status_json(improvement_sync_t *sync, char *out, size_t out_len) {
    if (!sync || !out || !out_len)
        return false;
    char signer[65];
    hex_encode(sync->signer_pk, 32, signer);
    pthread_mutex_lock(&sync->lock);
    uint64_t published = sync->published, fetched = sync->fetched,
             quarantined = sync->quarantined, rejected = sync->rejected,
             served = sync->served_chunks, received = sync->received_chunks;
    int seen = sync->seen_count;
    bool active = sync->fetch_active;
    pthread_mutex_unlock(&sync->lock);
    jbuf_t b;
    jbuf_init(&b, 512);
    jbuf_append(&b, "{\"enabled\":true,\"transport\":\"private-dht+encrypted-mesh\",\"root\":");
    jbuf_append_json_str(&b, sync->root);
    jbuf_append(&b, ",\"signer\":");
    jbuf_append_json_str(&b, signer);
    jbuf_appendf(&b,
                 ",\"objects\":%d,\"staged\":%d,\"quarantined\":%d,"
                 "\"seen\":%d,\"fetch_active\":%s,"
                 "\"session\":{\"published\":%llu,\"fetched\":%llu,"
                 "\"quarantined\":%llu,\"rejected\":%llu,"
                 "\"served_chunks\":%llu,\"received_chunks\":%llu},"
                 "\"auto_apply\":false}",
                 count_bundles(sync, "objects"), count_bundles(sync, "staged"),
                 count_bundles(sync, "quarantine"), seen, active ? "true" : "false",
                 (unsigned long long)published, (unsigned long long)fetched,
                 (unsigned long long)quarantined, (unsigned long long)rejected,
                 (unsigned long long)served, (unsigned long long)received);
    snprintf(out, out_len, "%s", b.data);
    jbuf_free(&b);
    return true;
}

bool improvement_sync_list_json(improvement_sync_t *sync, int limit, char *out, size_t out_len) {
    if (!sync || !out || !out_len)
        return false;
    if (limit < 1) limit = 50;
    if (limit > 256) limit = 256;
    const char *dirs[] = {"objects", "staged", "quarantine", NULL};
    jbuf_t b; jbuf_init(&b, 2048);
    jbuf_append(&b, "{\"bundles\":[");
    int emitted = 0;
    for (int di = 0; dirs[di] && emitted < limit; di++) {
        char dirpath[PATH_MAX];
        snprintf(dirpath, sizeof(dirpath), "%s/%s", sync->root, dirs[di]);
        DIR *d = opendir(dirpath);
        if (!d) continue;
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL && emitted < limit) {
            size_t n = strlen(ent->d_name);
            if (n != 71 || strcmp(ent->d_name + 64, ".bundle") != 0)
                continue;
            char hash[65], path[PATH_MAX], err[192];
            memcpy(hash, ent->d_name, 64); hash[64] = '\0';
            snprintf(path, sizeof(path), "%s/%s", dirpath, ent->d_name);
            improvement_bundle_info_t info;
            const char *status = strcmp(dirs[di], "objects") == 0 ? "object" : dirs[di];
            if (!inspect_path(sync, path, hash, status, &info, err, sizeof(err)))
                continue;
            if (emitted++) jbuf_append_char(&b, ',');
            info_json(&b, &info);
        }
        closedir(d);
    }
    jbuf_appendf(&b, "],\"count\":%d,\"limit\":%d}", emitted, limit);
    snprintf(out, out_len, "%s", b.data);
    jbuf_free(&b);
    return true;
}

void improvement_sync_announce_all(improvement_sync_t *sync) {
    if (!sync)
        return;
    const char *dirs[] = {"objects", "staged", NULL};
    for (int di = 0; dirs[di]; di++) {
        char dirpath[PATH_MAX];
        snprintf(dirpath, sizeof(dirpath), "%s/%s", sync->root, dirs[di]);
        DIR *d = opendir(dirpath);
        if (!d) continue;
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            size_t n = strlen(ent->d_name);
            if (n != 71 || strcmp(ent->d_name + 64, ".bundle") != 0)
                continue;
            char hash[65], path[PATH_MAX];
            memcpy(hash, ent->d_name, 64); hash[64] = '\0';
            snprintf(path, sizeof(path), "%s/%s", dirpath, ent->d_name);
            struct stat st;
            if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0)
                continue;
            dsco_dht_t *dht = dsco_dht_global();
            if (dht)
                (void)dsco_dht_provide_hash(dht, hash);
            if (sync->mesh) {
                uint8_t raw[32];
                if (hex_decode(hash, 64, raw, sizeof(raw)) == 32)
                    protocol_send(sync, NULL, IMPROVEMENT_WIRE_ADVERTISE, raw, 0,
                                  (uint64_t)st.st_size, NULL, 0);
            }
        }
        closedir(d);
    }
}

static void *announce_loop(void *arg) {
    improvement_sync_t *sync = arg;
    improvement_sync_announce_all(sync);
    pthread_mutex_lock(&sync->lock);
    while (sync->running) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 30;
        pthread_cond_timedwait(&sync->changed, &sync->lock, &ts);
        bool running = sync->running;
        pthread_mutex_unlock(&sync->lock);
        if (running)
            improvement_sync_announce_all(sync);
        pthread_mutex_lock(&sync->lock);
    }
    pthread_mutex_unlock(&sync->lock);
    return NULL;
}

static void improvement_sync_stop_network(improvement_sync_t *sync) {
    if (!sync)
        return;
    pthread_mutex_lock(&sync->lock);
    sync->running = false;
    if (sync->fetch_active) {
        sync->fetch_failed = true;
        snprintf(sync->fetch_error, sizeof(sync->fetch_error), "network shutdown");
    }
    pthread_cond_broadcast(&sync->changed);
    pthread_mutex_unlock(&sync->lock);
    if (sync->announce_thread_started) {
        pthread_join(sync->announce_thread, NULL);
        sync->announce_thread_started = false;
    }
    /* Existing mesh reader callbacks still own sync until mesh teardown, but
     * no callback or announcer may initiate another outbound send. */
    sync->mesh = NULL;
}

bool improvement_sync_global_init(void *mesh_node) {
    if (g_improvement_sync)
        return true;
    g_improvement_sync = improvement_sync_create(mesh_node, NULL);
    if (!g_improvement_sync)
        return false;
    g_improvement_sync->running = true;
    if (pthread_create(&g_improvement_sync->announce_thread, NULL, announce_loop,
                       g_improvement_sync) == 0)
        g_improvement_sync->announce_thread_started = true;
    return true;
}

void improvement_sync_global_shutdown(void) {
    improvement_sync_t *sync = g_improvement_sync;
    g_improvement_sync = NULL;
    improvement_sync_destroy(sync);
}

void improvement_sync_global_stop_network(void) {
    improvement_sync_stop_network(g_improvement_sync);
}

improvement_sync_t *improvement_sync_global(void) {
    return g_improvement_sync;
}

#else /* !HAVE_LIBSODIUM */

struct improvement_sync { int unavailable; };
static improvement_sync_t *g_improvement_sync;

improvement_sync_t *improvement_sync_create(void *mesh_node, const char *root_override) {
    (void)mesh_node; (void)root_override; return NULL;
}
void improvement_sync_destroy(improvement_sync_t *sync) { (void)sync; }
bool improvement_sync_publish(improvement_sync_t *s, const char *p, const char *k, const char *n,
                              const char *b, const char *t, const char *d,
                              improvement_bundle_info_t *o, char *e, size_t el) {
    (void)s;(void)p;(void)k;(void)n;(void)b;(void)t;(void)d;(void)o;
    snprintf(e, el, "improvement sync requires libsodium"); return false;
}
bool improvement_sync_fetch(improvement_sync_t *s,const char *h,int t,improvement_bundle_info_t *o,char *e,size_t el){(void)s;(void)h;(void)t;(void)o;snprintf(e,el,"improvement sync requires libsodium");return false;}
bool improvement_sync_inspect(improvement_sync_t *s,const char *h,improvement_bundle_info_t *o,char *e,size_t el){(void)s;(void)h;(void)o;snprintf(e,el,"improvement sync requires libsodium");return false;}
bool improvement_sync_trust_signer(improvement_sync_t *s,const char *h,char *e,size_t el){(void)s;(void)h;snprintf(e,el,"improvement sync requires libsodium");return false;}
bool improvement_sync_promote(improvement_sync_t *s,const char *h,improvement_bundle_info_t *o,char *e,size_t el){(void)s;(void)h;(void)o;snprintf(e,el,"improvement sync requires libsodium");return false;}
bool improvement_sync_materialize(improvement_sync_t *s,const char *h,const char *p,char *e,size_t el){(void)s;(void)h;(void)p;snprintf(e,el,"improvement sync requires libsodium");return false;}
bool improvement_sync_status_json(improvement_sync_t *s,char *o,size_t n){(void)s;snprintf(o,n,"{\"enabled\":false,\"error\":\"libsodium not compiled in\"}");return true;}
bool improvement_sync_list_json(improvement_sync_t *s,int l,char *o,size_t n){(void)s;(void)l;snprintf(o,n,"{\"enabled\":false,\"bundles\":[]}");return true;}
void improvement_sync_announce_all(improvement_sync_t *s){(void)s;}
bool improvement_sync_global_init(void *mesh_node){(void)mesh_node;return false;}
void improvement_sync_global_stop_network(void){}
void improvement_sync_global_shutdown(void){g_improvement_sync=NULL;}
improvement_sync_t *improvement_sync_global(void){return g_improvement_sync;}

#endif /* HAVE_LIBSODIUM */

static bool emit_tool_error(char *result, size_t result_len, const char *message) {
    jbuf_t b; jbuf_init(&b, 256);
    jbuf_append(&b, "{\"ok\":false,\"error\":");
    jbuf_append_json_str(&b, message ? message : "unknown error");
    jbuf_append_char(&b, '}');
    snprintf(result, result_len, "%s", b.data);
    jbuf_free(&b);
    return false;
}

bool tool_improvement_catalog(const char *input_json, char *result, size_t result_len) {
    improvement_sync_t *sync = improvement_sync_global();
    if (!sync)
        return emit_tool_error(result, result_len, "improvement sync is not initialized");
    char *action = json_get_str(input_json, "action");
    bool ok;
    if (!action || strcmp(action, "status") == 0)
        ok = improvement_sync_status_json(sync, result, result_len);
    else if (strcmp(action, "list") == 0)
        ok = improvement_sync_list_json(sync, json_get_int(input_json, "limit", 50), result, result_len);
    else if (strcmp(action, "inspect") == 0) {
        char *hash = json_get_str(input_json, "hash");
        char err[256]; improvement_bundle_info_t info;
        ok = improvement_sync_inspect(sync, hash, &info, err, sizeof(err));
        if (ok) {
            jbuf_t b; jbuf_init(&b, 1024); jbuf_append(&b, "{\"ok\":true,\"bundle\":");
            info_json(&b, &info); jbuf_append_char(&b, '}');
            snprintf(result, result_len, "%s", b.data); jbuf_free(&b);
        } else emit_tool_error(result, result_len, err);
        free(hash);
    } else {
        ok = emit_tool_error(result, result_len, "action must be status, list, or inspect");
    }
    free(action);
    return ok;
}

bool tool_improvement_sync(const char *input_json, char *result, size_t result_len) {
    improvement_sync_t *sync = improvement_sync_global();
    if (!sync)
        return emit_tool_error(result, result_len, "improvement sync is not initialized");
    char *action = json_get_str(input_json, "action");
    if (!action)
        return emit_tool_error(result, result_len, "action is required");
    char err[256] = {0}; improvement_bundle_info_t info; bool ok = false;
    if (strcmp(action, "publish") == 0) {
        char *path=json_get_str(input_json,"path"), *kind=json_get_str(input_json,"kind"),
             *name=json_get_str(input_json,"name"), *base=json_get_str(input_json,"base_version"),
             *target=json_get_str(input_json,"target_version"), *desc=json_get_str(input_json,"description");
        ok = improvement_sync_publish(sync,path,kind,name,base,target,desc,&info,err,sizeof(err));
        free(path);free(kind);free(name);free(base);free(target);free(desc);
    } else if (strcmp(action, "fetch") == 0) {
        char *hash=json_get_str(input_json,"hash");
        ok=improvement_sync_fetch(sync,hash,json_get_int(input_json,"timeout_seconds",30),&info,err,sizeof(err));
        free(hash);
    } else if (strcmp(action, "trust") == 0) {
        char *signer=json_get_str(input_json,"signer");
        ok=improvement_sync_trust_signer(sync,signer,err,sizeof(err)); free(signer);
        if (ok) snprintf(result,result_len,"{\"ok\":true,\"trusted\":true}");
    } else if (strcmp(action, "promote") == 0) {
        char *hash=json_get_str(input_json,"hash");
        ok=improvement_sync_promote(sync,hash,&info,err,sizeof(err)); free(hash);
    } else if (strcmp(action, "materialize") == 0) {
        char *hash=json_get_str(input_json,"hash"), *path=json_get_str(input_json,"output_path");
        ok=improvement_sync_materialize(sync,hash,path,err,sizeof(err)); free(hash);free(path);
        if (ok) snprintf(result,result_len,"{\"ok\":true,\"materialized\":true}");
    } else if (strcmp(action, "announce") == 0) {
        improvement_sync_announce_all(sync); ok=true;
        snprintf(result,result_len,"{\"ok\":true,\"announced\":true}");
    } else {
        snprintf(err,sizeof(err),"action must be publish, fetch, trust, promote, materialize, or announce");
    }
    if (ok && (strcmp(action,"publish")==0 || strcmp(action,"fetch")==0 || strcmp(action,"promote")==0)) {
        jbuf_t b; jbuf_init(&b,1024); jbuf_append(&b,"{\"ok\":true,\"bundle\":");
        info_json(&b,&info); jbuf_append_char(&b,'}'); snprintf(result,result_len,"%s",b.data); jbuf_free(&b);
    } else if (!ok) {
        emit_tool_error(result,result_len,err[0]?err:"improvement sync operation failed");
    }
    free(action);
    return ok;
}
