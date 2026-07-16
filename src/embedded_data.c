/* embedded_data.c — lazy decryptor for baked (encrypted) data blobs.
 *
 * scripts/bake_data.py encrypts every data/ blob with an HMAC-SHA256-CTR
 * keystream and scatters the key into four XOR shares (embedded_key.gen.h).
 * At runtime the key is reconstructed only inside this TU, on a locked page's
 * worth of stack, and each blob is decrypted at most once (cached for the
 * process lifetime). Callers see plaintext and must treat it as read-only.
 */
#define DSCO_EMBEDDED_DATA_IMPL
#include "embedded_data_registry.h"
#include "embedded_key.gen.h"
#include "crypto.h"

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define DSCO_EMBEDDED_MAX 64

static const unsigned char *s_plain[DSCO_EMBEDDED_MAX];
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;

static void reconstruct_key(uint8_t key[32])
{
    for (int i = 0; i < 32; i++)
        key[i] = DSCO_EMB_S0[i] ^ DSCO_EMB_S1[i] ^ DSCO_EMB_S2[i] ^ DSCO_EMB_S3[i];
}

/* HMAC-SHA256-CTR keystream XOR — must match keystream_xor() in bake_data.py. */
static void ks_xor(const uint8_t key[32], const uint8_t nonce[16], uint8_t *buf, size_t len)
{
    size_t off = 0;
    uint64_t ctr = 0;
    while (off < len) {
        uint8_t in[24];
        memcpy(in, nonce, 16);
        for (int i = 0; i < 8; i++)
            in[16 + i] = (uint8_t)((ctr >> (8 * i)) & 0xff);
        uint8_t ks[32];
        hmac_sha256(key, 32, in, sizeof(in), ks);
        size_t n = (len - off < 32) ? (len - off) : 32;
        for (size_t i = 0; i < n; i++)
            buf[off + i] ^= ks[i];
        off += n;
        ctr++;
    }
}

const unsigned char *embedded_data_get(const char *name, size_t *out_len)
{
    int idx = -1;
    for (int i = 0; i < DSCO_EMBEDDED_MAX && dsco_embedded_registry_table[i].name; i++) {
        if (strcmp(dsco_embedded_registry_table[i].name, name) == 0) {
            idx = i;
            break;
        }
    }
    if (idx < 0) {
        if (out_len)
            *out_len = 0;
        return NULL;
    }

    size_t len = *dsco_embedded_registry_table[idx].len;
    if (out_len)
        *out_len = len;

    pthread_mutex_lock(&s_lock);
    if (!s_plain[idx]) {
        unsigned char *buf = (unsigned char *)malloc(len ? len : 1);
        if (buf) {
            memcpy(buf, dsco_embedded_registry_table[idx].data, len);
            uint8_t key[32];
            reconstruct_key(key);
            ks_xor(key, DSCO_EMB_NONCE, buf, len);
            memset(key, 0, sizeof(key)); /* don't leave the key on the stack */
            s_plain[idx] = buf;
        }
    }
    const unsigned char *result = s_plain[idx];
    pthread_mutex_unlock(&s_lock);
    return result;
}

/* ── Obfuscated inline secrets (KEY=VALUE table in data/dsco_secrets.txt) ─────
 * Lets sensitive string literals live encrypted in the binary instead of plain
 * in __cstring. Parsed once from the (decrypted) blob into a null-terminated
 * cache; returns "" for a missing key so callers never deref NULL. */
#define DSCO_SECRETS_MAX 128
static struct {
    char *key;
    char *val;
} s_secrets[DSCO_SECRETS_MAX];
static int s_secret_count = -1;
static pthread_mutex_t s_secret_lock = PTHREAD_MUTEX_INITIALIZER;

static void dsco_secrets_parse(void)
{
    s_secret_count = 0;
    size_t len = 0;
    const unsigned char *blob = embedded_data_get("dsco_secrets.txt", &len);
    if (!blob || !len)
        return;
    char *buf = (char *)malloc(len + 1);
    if (!buf)
        return;
    memcpy(buf, blob, len);
    buf[len] = '\0';
    char *save = NULL;
    for (char *line = strtok_r(buf, "\n", &save);
         line && s_secret_count < DSCO_SECRETS_MAX;
         line = strtok_r(NULL, "\n", &save)) {
        char *eq = strchr(line, '=');
        if (!eq)
            continue;
        *eq = '\0';
        s_secrets[s_secret_count].key = strdup(line);
        s_secrets[s_secret_count].val = strdup(eq + 1);
        if (s_secrets[s_secret_count].key && s_secrets[s_secret_count].val)
            s_secret_count++;
    }
    free(buf);
}

const char *dsco_secret(const char *key)
{
    pthread_mutex_lock(&s_secret_lock);
    if (s_secret_count < 0)
        dsco_secrets_parse();
    const char *result = "";
    for (int i = 0; i < s_secret_count; i++) {
        if (strcmp(s_secrets[i].key, key) == 0) {
            result = s_secrets[i].val;
            break;
        }
    }
    pthread_mutex_unlock(&s_secret_lock);
    return result;
}
