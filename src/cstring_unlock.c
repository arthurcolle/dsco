/* cstring_unlock.c — load-time decryptor for the encrypted __cstring section.
 *
 * Built only under -DDSCO_HARDEN_CSTRING (make harden-max). The hardened link
 * relocates __TEXT,__cstring into the writable __DATA segment; a post-link tool
 * (scripts/encrypt_cstring.py) encrypts those bytes with an HMAC-SHA256-CTR
 * keystream. This constructor runs before main() and decrypts the section in
 * place, so string literals are ciphertext at rest but plaintext once live.
 *
 * CRITICAL: this TU is SELF-CONTAINED and STACK-PROTECTOR-FREE. It runs at
 * constructor priority 101 — earlier than libSystem's __stack_chk_guard setup —
 * so it must not (a) call into another TU (crypto.c is -fstack-protector-strong,
 * whose canary check would SIGTRAP with an uninitialized guard), (b) reference
 * any __cstring literal (still encrypted at this point), or (c) use libc beyond
 * the Mach-O section lookup. The Makefile compiles it with -fno-stack-protector.
 */
#ifdef DSCO_HARDEN_CSTRING

#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <stdint.h>
#include <sys/mman.h>
#include <unistd.h>

#include "cstring_key.gen.h"   /* CSTR_S0..S3, CSTR_NONCE */

extern const struct mach_header_64 _mh_execute_header;

/* Manually locate __cstring by walking load commands (getsectiondata returns
 * NULL for re-signed images — a known quirk). Returns the runtime pointer and
 * size, or NULL. No libc, no string literals beyond the 2 fixed segment names. */
static uint8_t *cu_find_cstring(unsigned long *out_size)
{
    const struct mach_header_64 *mh = &_mh_execute_header;
    intptr_t slide = _dyld_get_image_vmaddr_slide(0);
    const uint8_t *lc = (const uint8_t *)mh + sizeof(struct mach_header_64);
    for (uint32_t i = 0; i < mh->ncmds; i++) {
        const struct load_command *cmd = (const struct load_command *)lc;
        if (cmd->cmd == LC_SEGMENT_64) {
            const struct segment_command_64 *seg = (const struct segment_command_64 *)lc;
            const struct section_64 *sec =
                (const struct section_64 *)(lc + sizeof(struct segment_command_64));
            for (uint32_t s = 0; s < seg->nsects; s++) {
                const char *n = sec[s].sectname;
                /* strcmp(n, "__cstring") without libc */
                if (n[0] == '_' && n[1] == '_' && n[2] == 'c' && n[3] == 's' && n[4] == 't' &&
                    n[5] == 'r' && n[6] == 'i' && n[7] == 'n' && n[8] == 'g' && n[9] == '\0') {
                    *out_size = (unsigned long)sec[s].size;
                    return (uint8_t *)(sec[s].addr + slide);
                }
            }
        }
        lc += cmd->cmdsize;
    }
    *out_size = 0;
    return 0;
}

/* ── Inlined SHA-256 (FIPS 180-4), byte-identical to crypto.c / hashlib ─────── */
#define CU_ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static const uint32_t CU_K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

static void cu_block(uint32_t st[8], const uint8_t *p)
{
    uint32_t w[64], a, b, c, d, e, f, g, h, t1, t2;
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
               ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = CU_ROR(w[i - 15], 7) ^ CU_ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = CU_ROR(w[i - 2], 17) ^ CU_ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = st[0]; b = st[1]; c = st[2]; d = st[3];
    e = st[4]; f = st[5]; g = st[6]; h = st[7];
    for (int i = 0; i < 64; i++) {
        uint32_t ep1 = CU_ROR(e, 6) ^ CU_ROR(e, 11) ^ CU_ROR(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        t1 = h + ep1 + ch + CU_K[i] + w[i];
        uint32_t ep0 = CU_ROR(a, 2) ^ CU_ROR(a, 13) ^ CU_ROR(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        t2 = ep0 + maj;
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    st[0] += a; st[1] += b; st[2] += c; st[3] += d;
    st[4] += e; st[5] += f; st[6] += g; st[7] += h;
}

/* SHA-256 over a contiguous message of len <= 111 bytes (fits our HMAC uses:
 * 64-byte pad + 24-byte counter block, and 64-byte pad + 32-byte inner hash). */
static void cu_sha256(const uint8_t *msg, uint32_t len, uint8_t out[32])
{
    uint32_t st[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                      0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    uint8_t buf[128];
    for (uint32_t i = 0; i < 128; i++)
        buf[i] = 0;
    for (uint32_t i = 0; i < len; i++)
        buf[i] = msg[i];
    buf[len] = 0x80;
    uint64_t bits = (uint64_t)len * 8u;
    /* len <= 111 → total padded length is exactly 128 (two blocks). */
    for (int i = 0; i < 8; i++)
        buf[120 + i] = (uint8_t)(bits >> ((7 - i) * 8));
    cu_block(st, buf);
    cu_block(st, buf + 64);
    for (int i = 0; i < 8; i++) {
        out[i * 4] = (uint8_t)(st[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(st[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(st[i] >> 8);
        out[i * 4 + 3] = (uint8_t)(st[i]);
    }
}

/* HMAC-SHA256 with a 32-byte key over a short message (msg len <= 47). */
static void cu_hmac(const uint8_t key[32], const uint8_t *msg, uint32_t mlen, uint8_t out[32])
{
    uint8_t ipad[64 + 47], opad[64 + 32];
    for (int i = 0; i < 64; i++) {
        uint8_t k = (i < 32) ? key[i] : 0;
        ipad[i] = k ^ 0x36;
        opad[i] = k ^ 0x5c;
    }
    for (uint32_t i = 0; i < mlen; i++)
        ipad[64 + i] = msg[i];
    uint8_t inner[32];
    cu_sha256(ipad, 64 + mlen, inner);
    for (int i = 0; i < 32; i++)
        opad[64 + i] = inner[i];
    cu_sha256(opad, 96, out);
}

/* HMAC-SHA256-CTR keystream XOR — must match keystream_xor() in bake scripts. */
static void cu_ks_xor(const uint8_t key[32], const uint8_t nonce[16],
                      uint8_t *buf, unsigned long len)
{
    unsigned long off = 0;
    uint64_t ctr = 0;
    while (off < len) {
        uint8_t in[24];
        for (int i = 0; i < 16; i++)
            in[i] = nonce[i];
        for (int i = 0; i < 8; i++)
            in[16 + i] = (uint8_t)((ctr >> (8 * i)) & 0xff);
        uint8_t ks[32];
        cu_hmac(key, in, 24, ks);
        unsigned long n = (len - off < 32) ? (len - off) : 32;
        for (unsigned long i = 0; i < n; i++)
            buf[off + i] ^= ks[i];
        off += n;
        ctr++;
    }
}

#ifdef DSCO_CSTRING_SELFTEST
/* Standalone keystream check: reads key||nonce(48 bytes) from argv[1], XORs
 * stdin→stdout. Lets a Python-encrypt / this-decrypt round-trip prove the
 * inlined SHA-256/HMAC is byte-identical to the bake scripts. Not in prod. */
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char **argv)
{
    if (argc < 2) return 2;
    FILE *kf = fopen(argv[1], "rb");
    if (!kf) return 3;
    uint8_t km[48];
    if (fread(km, 1, 48, kf) != 48) return 4;
    fclose(kf);
    uint8_t buf[1 << 20];
    size_t n = fread(buf, 1, sizeof(buf), stdin);
    cu_ks_xor(km, km + 32, buf, n);
    fwrite(buf, 1, n, stdout);
    return 0;
}
#endif

__attribute__((constructor(101), used)) static void dsco_cstring_unlock(void)
{
    unsigned long size = 0;
    uint8_t *sect = cu_find_cstring(&size);
    if (!sect || !size)
        return;
    /* Make the section writable: codesign may map its containing region as
     * read-only. Round to page boundaries. Failure is non-fatal (the region may
     * already be RW); the XOR below will fault only if it's genuinely locked. */
    long pg = sysconf(_SC_PAGESIZE);
    if (pg <= 0)
        pg = 4096;
    uintptr_t start = (uintptr_t)sect & ~((uintptr_t)pg - 1);
    uintptr_t end = ((uintptr_t)sect + size + pg - 1) & ~((uintptr_t)pg - 1);
    mprotect((void *)start, (size_t)(end - start), PROT_READ | PROT_WRITE);
    uint8_t key[32];
    for (int i = 0; i < 32; i++)
        key[i] = CSTR_S0[i] ^ CSTR_S1[i] ^ CSTR_S2[i] ^ CSTR_S3[i];
    cu_ks_xor(key, CSTR_NONCE, sect, size);
    for (int i = 0; i < 32; i++)
        key[i] = 0;
    /* Leave the region readable+writable. Restoring PROT_READ here can fault
     * later code paths that legitimately hold interior pointers, and the pages
     * are already dirty COW copies unique to this process. */
    (void)start;
    (void)end;
}

#endif /* DSCO_HARDEN_CSTRING */
