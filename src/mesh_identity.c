#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE 1
#endif
#include "mesh_identity.h"

#include <sodium.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define MESH_IDENTITY_BYTES (MESH_PUBKEY_LEN * 2)
#define MESH_HEX_LEN (MESH_PUBKEY_LEN * 2)

static const char *identity_path(void) {
    const char *p = getenv("DSCO_MESH_IDENTITY_FILE");
    if (p && *p) return p;
    const char *home = getenv("HOME");
    if (!home || !*home) return NULL;
    static _Thread_local char path[PATH_MAX];
    if (snprintf(path, sizeof(path), "%s/.dsco/mesh.identity", home) >= (int)sizeof(path))
        return NULL;
    return path;
}

static bool valid_keypair(const uint8_t *pk, const uint8_t *sk) {
    uint8_t derived[MESH_PUBKEY_LEN];
    bool ok = crypto_scalarmult_base(derived, sk) == 0 &&
              sodium_memcmp(derived, pk, MESH_PUBKEY_LEN) == 0;
    sodium_memzero(derived, sizeof(derived));
    return ok;
}

static bool read_identity(const char *path, uint8_t *pk, uint8_t *sk) {
    struct stat st;
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0 || fstat(fd, &st) < 0) { if (fd >= 0) close(fd); return false; }
    bool ok = S_ISREG(st.st_mode) && st.st_uid == geteuid() && st.st_size == MESH_IDENTITY_BYTES &&
              (st.st_mode & (S_IRWXG | S_IRWXO)) == 0;
    if (ok) {
        uint8_t bytes[MESH_IDENTITY_BYTES];
        ssize_t got = 0, n;
        while (got < (ssize_t)sizeof(bytes) &&
               (n = read(fd, bytes + got, sizeof(bytes) - (size_t)got)) > 0) got += n;
        ok = got == (ssize_t)sizeof(bytes) && valid_keypair(bytes, bytes + MESH_PUBKEY_LEN);
        if (ok) { memcpy(pk, bytes, MESH_PUBKEY_LEN); memcpy(sk, bytes + MESH_PUBKEY_LEN, MESH_PUBKEY_LEN); }
        sodium_memzero(bytes, sizeof(bytes));
    }
    close(fd);
    return ok;
}

static bool create_identity(const char *path, uint8_t *pk, uint8_t *sk) {
    uint8_t bytes[MESH_IDENTITY_BYTES];
    crypto_box_keypair(bytes, bytes + MESH_PUBKEY_LEN);
    char tmp[PATH_MAX];
    if (snprintf(tmp, sizeof(tmp), "%s.tmp.%ld", path, (long)getpid()) >= (int)sizeof(tmp)) goto fail;
    int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) goto fail;
    bool ok = fchmod(fd, 0600) == 0;
    ssize_t off = 0;
    while (ok && off < (ssize_t)sizeof(bytes)) {
        ssize_t n = write(fd, bytes + off, sizeof(bytes) - (size_t)off);
        if (n <= 0) { if (errno == EINTR) continue; ok = false; break; }
        off += n;
    }
    if (ok) ok = fsync(fd) == 0;
    close(fd);
    if (!ok || link(tmp, path) != 0) { unlink(tmp); goto fail; }
    unlink(tmp);
    memcpy(pk, bytes, MESH_PUBKEY_LEN); memcpy(sk, bytes + MESH_PUBKEY_LEN, MESH_PUBKEY_LEN);
    sodium_memzero(bytes, sizeof(bytes));
    return true;
fail:
    sodium_memzero(bytes, sizeof(bytes));
    return false;
}

bool mesh_identity_load(uint8_t public_key[MESH_PUBKEY_LEN], uint8_t secret_key[MESH_PUBKEY_LEN]) {
    if (!public_key || !secret_key || sodium_init() < 0) return false;
    const char *path = identity_path();
    if (!path) return false;
    if (!getenv("DSCO_MESH_IDENTITY_FILE")) {
        char dir[PATH_MAX];
        const char *slash = strrchr(path, '/');
        size_t len = slash ? (size_t)(slash - path) : 0;
        if (!slash || len == 0 || len >= sizeof(dir)) return false;
        memcpy(dir, path, len); dir[len] = '\0';
        if (mkdir(dir, 0700) != 0 && errno != EEXIST) return false;
        struct stat dst;
        if (stat(dir, &dst) != 0 || !S_ISDIR(dst.st_mode) ||
            (dst.st_mode & (S_IRWXG | S_IRWXO)) != 0) return false;
    }
    if (read_identity(path, public_key, secret_key)) return true;
    if (errno != ENOENT) return false;
    return create_identity(path, public_key, secret_key);
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool mesh_identity_allowed(const uint8_t public_key[MESH_PUBKEY_LEN]) {
    if (!public_key) return false;
    const char *path = getenv("DSCO_MESH_ALLOWLIST");
    if (!path || !*path) {
        const char *home = getenv("HOME");
        if (!home || !*home) return false;
        static _Thread_local char fallback[PATH_MAX];
        if (snprintf(fallback, sizeof(fallback), "%s/.dsco/mesh.allowed", home) >= (int)sizeof(fallback)) return false;
        path = fallback;
    }
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return false;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
        (st.st_mode & (S_IWGRP | S_IWOTH))) { close(fd); return false; }
    FILE *f = fdopen(fd, "r");
    if (!f) { close(fd); return false; }
    char line[256]; uint8_t candidate[MESH_PUBKEY_LEN]; bool allowed = false;
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        char *comment = strchr(p, '#'); if (comment) *comment = '\0';
        size_t len = strlen(p);
        while (len && (p[len-1] == ' ' || p[len-1] == '\t' || p[len-1] == '\r' || p[len-1] == '\n')) p[--len] = '\0';
        if (len != MESH_HEX_LEN) continue;
        bool valid = true;
        for (size_t i = 0; i < MESH_PUBKEY_LEN; i++) {
            int hi = hex_value(p[i*2]), lo = hex_value(p[i*2+1]);
            if (hi < 0 || lo < 0) { valid = false; break; }
            candidate[i] = (uint8_t)((hi << 4) | lo);
        }
        if (valid && sodium_memcmp(candidate, public_key, MESH_PUBKEY_LEN) == 0) { allowed = true; break; }
    }
    sodium_memzero(candidate, sizeof(candidate));
    fclose(f);
    return allowed;
}
