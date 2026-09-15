/* Standalone: cc -std=c11 -D_DARWIN_C_SOURCE -Iinclude \
 *   tests/test_mesh_identity.c src/mesh_identity.c \
 *   $(pkg-config --cflags --libs libsodium) -o /tmp/test_mesh_identity */
#include "mesh_identity.h"
#include <sodium.h>
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void hex(const uint8_t *in, char *out) {
    static const char h[] = "0123456789abcdef";
    for (size_t i = 0; i < 32; i++) { out[i * 2] = h[in[i] >> 4]; out[i * 2 + 1] = h[in[i] & 15]; }
    out[64] = '\0';
}

int main(void) {
    char dir[] = "/tmp/dsco-mesh-identity-XXXXXX";
    assert(mkdtemp(dir));
    char identity[512], allow[512];
    snprintf(identity, sizeof(identity), "%s/identity", dir);
    snprintf(allow, sizeof(allow), "%s/allowed", dir);
    assert(setenv("DSCO_MESH_IDENTITY_FILE", identity, 1) == 0);
    assert(setenv("DSCO_MESH_ALLOWLIST", allow, 1) == 0);

    uint8_t pk[32], sk[32], pk2[32], sk2[32];
    assert(mesh_identity_load(pk, sk));
    assert(mesh_identity_load(pk2, sk2));
    assert(memcmp(pk, pk2, 32) == 0 && memcmp(sk, sk2, 32) == 0);
    struct stat st;
    assert(stat(identity, &st) == 0 && (st.st_mode & 0777) == 0600 && st.st_size == 64);
    assert(chmod(identity, 0640) == 0);
    assert(!mesh_identity_load(pk2, sk2));
    assert(chmod(identity, 0600) == 0);
    assert(mesh_identity_load(pk2, sk2));

    FILE *f = fopen(allow, "w"); assert(f);
    char pubhex[65]; hex(pk, pubhex);
    assert(fprintf(f, "# pinned peer\n%s # comment\n", pubhex) > 0);
    fclose(f);
    assert(mesh_identity_allowed(pk));
    uint8_t other[32]; randombytes_buf(other, sizeof(other));
    assert(!mesh_identity_allowed(other));

    int fd = open(identity, O_WRONLY | O_TRUNC); assert(fd >= 0);
    uint8_t bad[64] = {0}; assert(write(fd, bad, sizeof(bad)) == (ssize_t)sizeof(bad)); close(fd);
    assert(!mesh_identity_load(pk2, sk2)); /* corrupt identity is rejected, never replaced */

    unlink(identity); unlink(allow); rmdir(dir);
    puts("mesh identity tests passed");
    return 0;
}
