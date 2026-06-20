#include "sealed_store.h"
#include "tamper.h"
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <sys/mman.h>

#ifdef HAVE_LIBSODIUM
#  include <sodium.h>
#endif

/* ── storage ───────────────────────────────────────────────────────────── */

typedef struct {
    char   key[SEALED_KEY_MAX];
    char   val[SEALED_VAL_MAX];
    size_t val_len;
    int    live;
} sealed_entry_t;

static sealed_entry_t s_entries[SEALED_ENTRIES];
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static int s_inited = 0;

/* ── helpers ───────────────────────────────────────────────────────────── */

static void zero_entry(sealed_entry_t *e) {
#ifdef HAVE_LIBSODIUM
    sodium_memzero(e->val, sizeof(e->val));
    sodium_memzero(e->key, sizeof(e->key));
#else
    /* volatile write prevents the compiler from eliding the clear */
    volatile char *p = (volatile char *)e;
    for (size_t i = 0; i < sizeof(*e); i++) p[i] = 0;
#endif
    e->val_len = 0;
    e->live    = 0;
}

static sealed_entry_t *find(const char *key) {
    for (int i = 0; i < SEALED_ENTRIES; i++)
        if (s_entries[i].live && strcmp(s_entries[i].key, key) == 0)
            return &s_entries[i];
    return NULL;
}

static sealed_entry_t *find_free(void) {
    for (int i = 0; i < SEALED_ENTRIES; i++)
        if (!s_entries[i].live)
            return &s_entries[i];
    return NULL;
}

/* ── tamper wiper registered at init ───────────────────────────────────── */

static void sealed_wiper(void *ctx) {
    (void)ctx;
    sealed_store_wipe_all();
}

/* ── environment bootstrap ─────────────────────────────────────────────── */

static const char *s_env_keys[] = {
    "ANTHROPIC_API_KEY",
    "OPENAI_API_KEY",
    "GEMINI_API_KEY",
    "XAI_API_KEY",
    "DEEPSEEK_API_KEY",
    "GROQ_API_KEY",
    "OPENROUTER_API_KEY",
    "DSCO_MESH_SECRET",
    "DSCO_NET_AUTH_KEY",
    NULL
};

static void load_from_env(void) {
    for (int i = 0; s_env_keys[i]; i++) {
        const char *v = getenv(s_env_keys[i]);
        if (v && v[0]) {
            /* intern into store; ignore overflow, partial store is fine */
            sealed_store_put(s_env_keys[i], v, 0);
#ifdef HAVE_LIBSODIUM
            /* zero the environment copy to reduce exposure */
            char *env_val = getenv(s_env_keys[i]);
            if (env_val) {
                volatile char *p = (volatile char *)env_val;
                for (size_t j = 0; env_val[j]; j++) p[j] = 0;
            }
#endif
        }
    }
}

/* ── SE master key storage (mlock'd, zeroed on wipe) ───────────────────── */

static uint8_t s_master_key[32];   /* set by sealed_store_set_master_key */
static int     s_has_master = 0;

void sealed_store_set_master_key(const uint8_t key[32]) {
    memcpy(s_master_key, key, 32);
    (void)mlock(s_master_key, sizeof(s_master_key));
    s_has_master = 1;
}

bool sealed_store_master_key_copy(uint8_t out[32]) {
    if (!s_has_master || !out) return false;
    memcpy(out, s_master_key, 32);
    return true;
}

/* ── public API ─────────────────────────────────────────────────────────── */

void sealed_store_init(void) {
    pthread_mutex_lock(&s_lock);
    if (s_inited) { pthread_mutex_unlock(&s_lock); return; }

    memset(s_entries, 0, sizeof(s_entries));

    /* Lock the memory pages so they won't be swapped to disk */
    (void)mlock(s_entries, sizeof(s_entries));

    s_inited = 1;
    pthread_mutex_unlock(&s_lock);

    /* Register before loading secrets so wiper fires even on early tamper */
    tamper_register_wiper(sealed_wiper, NULL);

    load_from_env();
}

int sealed_store_put(const char *key, const char *val, size_t val_len) {
    if (!key || !val) return -1;
    if (strlen(key) >= SEALED_KEY_MAX) return -1;
    if (!val_len) val_len = strlen(val);
    if (val_len >= SEALED_VAL_MAX) return -1;

    pthread_mutex_lock(&s_lock);
    sealed_entry_t *e = find(key);
    if (!e) e = find_free();
    if (!e) { pthread_mutex_unlock(&s_lock); return -1; }

    if (!e->live) {
        strncpy(e->key, key, SEALED_KEY_MAX - 1);
        e->key[SEALED_KEY_MAX - 1] = '\0';
    } else {
        /* overwrite: zero old value first */
#ifdef HAVE_LIBSODIUM
        sodium_memzero(e->val, e->val_len);
#endif
    }
    memcpy(e->val, val, val_len);
    e->val[val_len] = '\0';
    e->val_len = val_len;
    e->live    = 1;
    pthread_mutex_unlock(&s_lock);
    return 0;
}

int sealed_store_get(const char *key, char *buf, size_t buf_len) {
    if (!key || !buf || !buf_len) return -1;
    pthread_mutex_lock(&s_lock);
    sealed_entry_t *e = find(key);
    if (!e) { pthread_mutex_unlock(&s_lock); return -1; }
    if (e->val_len + 1 > buf_len) { pthread_mutex_unlock(&s_lock); return -1; }
    memcpy(buf, e->val, e->val_len + 1);
    int len = (int)e->val_len;
    pthread_mutex_unlock(&s_lock);
    return len;
}

void sealed_store_wipe(const char *key) {
    if (!key) return;
    pthread_mutex_lock(&s_lock);
    sealed_entry_t *e = find(key);
    if (e) zero_entry(e);
    pthread_mutex_unlock(&s_lock);
}

void sealed_store_wipe_all(void) {
    pthread_mutex_lock(&s_lock);
    for (int i = 0; i < SEALED_ENTRIES; i++)
        if (s_entries[i].live) zero_entry(&s_entries[i]);
    pthread_mutex_unlock(&s_lock);
}

const char *sealed_getenv(const char *key) {
    static _Thread_local char tls_buf[SEALED_VAL_MAX];
    if (sealed_store_get(key, tls_buf, sizeof(tls_buf)) >= 0 && tls_buf[0])
        return tls_buf;
    const char *v = getenv(key);
    if (v && v[0]) {
        sealed_store_put(key, v, 0);
        /* return from store so the env string exposure window is minimised */
        if (sealed_store_get(key, tls_buf, sizeof(tls_buf)) >= 0)
            return tls_buf;
    }
    return NULL;
}
