/* oauth_clients.h — operator-supplied OAuth client identifiers.
 *
 * Third-party OAuth client identifiers and the Claude Code billing salt are
 * not compiled into the source tree. They resolve, in order, from:
 *
 *   1. the named environment variable (for example DSCO_CHATGPT_OAUTH_CLIENT_ID);
 *   2. the same key in a local JSON object file, by default
 *      ~/.dsco/oauth_clients.json (override with DSCO_OAUTH_CLIENTS_FILE), e.g.
 *        { "DSCO_CHATGPT_OAUTH_CLIENT_ID": "...", "DSCO_CLAUDE_CODE_BILLING_SALT": "..." }
 *   3. hardened builds only: the embedded secrets table (dsco_secret()).
 *
 * When none is configured the lookup returns NULL and prints a one-time hint
 * naming the variable. Values are never printed.
 *
 * Header-only on purpose: several standalone test binaries compile llm.c and
 * friends without the full object set.
 */
#ifndef DSCO_OAUTH_CLIENTS_H
#define DSCO_OAUTH_CLIENTS_H

#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef DSCO_USE_OBF_SECRETS
const char *dsco_secret(const char *key);
#endif

#define DSCO_OAUTH_CLIENTS_FILE_ENV "DSCO_OAUTH_CLIENTS_FILE"
#define DSCO_OAUTH_CLIENTS_DEFAULT_REL "/.dsco/oauth_clients.json"
#define DSCO_OAUTH_CLIENT_VALUE_MAX 512
#define DSCO_OAUTH_CLIENT_SLOTS 8
#define DSCO_OAUTH_CLIENTS_FILE_MAX (64 * 1024)

typedef struct {
    char key[96];
    char value[DSCO_OAUTH_CLIENT_VALUE_MAX];
    bool found;
    bool warned;
} dsco_oauth_client_slot_t;

/* Extract a flat string value for "key" from a small JSON object. Escapes are
 * kept minimal: \" and \\ are decoded, other escapes are copied verbatim. */
static inline bool dsco_oauth_clients_json_str(const char *json, const char *key, char *out,
                                               size_t out_len) {
    size_t klen = strlen(key);
    const char *p = json;
    while ((p = strchr(p, '"')) != NULL) {
        p++;
        if (strncmp(p, key, klen) == 0 && p[klen] == '"') {
            const char *q = p + klen + 1;
            while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n')
                q++;
            if (*q != ':')
                continue;
            q++;
            while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n')
                q++;
            if (*q != '"')
                return false;
            q++;
            size_t n = 0;
            while (*q && *q != '"' && n + 1 < out_len) {
                if (*q == '\\' && (q[1] == '"' || q[1] == '\\'))
                    q++;
                out[n++] = *q++;
            }
            out[n] = '\0';
            return *q == '"' && n > 0;
        }
        /* skip to the end of this string token */
        while (*p && *p != '"') {
            if (*p == '\\' && p[1])
                p++;
            p++;
        }
        if (*p)
            p++;
    }
    return false;
}

static inline bool dsco_oauth_clients_file_lookup(const char *key, char *out, size_t out_len) {
    char path[1024];
    const char *override = getenv(DSCO_OAUTH_CLIENTS_FILE_ENV);
    if (override && override[0]) {
        snprintf(path, sizeof(path), "%s", override);
    } else {
        const char *home = getenv("HOME");
        if (!home || !home[0])
            return false;
        snprintf(path, sizeof(path), "%s%s", home, DSCO_OAUTH_CLIENTS_DEFAULT_REL);
    }
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    char *buf = (char *)malloc(DSCO_OAUTH_CLIENTS_FILE_MAX + 1);
    if (!buf) {
        fclose(f);
        return false;
    }
    size_t n = fread(buf, 1, DSCO_OAUTH_CLIENTS_FILE_MAX, f);
    fclose(f);
    buf[n] = '\0';
    bool ok = dsco_oauth_clients_json_str(buf, key, out, out_len);
    free(buf);
    return ok;
}

/* Resolve an operator-supplied OAuth client value. `env_name` doubles as the
 * JSON key in the local file. Returns NULL (with a one-time stderr hint that
 * names the variable, never a value) when it is not configured. */
static inline const char *dsco_oauth_client_value(const char *env_name, const char *purpose) {
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    static dsco_oauth_client_slot_t slots[DSCO_OAUTH_CLIENT_SLOTS];

    const char *env = getenv(env_name);
    if (env && env[0])
        return env;

    const char *result = NULL;
    pthread_mutex_lock(&lock);
    dsco_oauth_client_slot_t *slot = NULL;
    for (int i = 0; i < DSCO_OAUTH_CLIENT_SLOTS; i++) {
        if (slots[i].key[0] && strcmp(slots[i].key, env_name) == 0) {
            slot = &slots[i];
            break;
        }
        if (!slots[i].key[0] && !slot) {
            slot = &slots[i];
            snprintf(slot->key, sizeof(slot->key), "%s", env_name);
            break;
        }
    }
    if (slot) {
        if (!slot->found)
            slot->found = dsco_oauth_clients_file_lookup(env_name, slot->value,
                                                         sizeof(slot->value));
#ifdef DSCO_USE_OBF_SECRETS
        if (!slot->found) {
            const char *s = dsco_secret(env_name);
            if (s && s[0]) {
                snprintf(slot->value, sizeof(slot->value), "%s", s);
                slot->found = true;
            }
        }
#endif
        if (slot->found) {
            result = slot->value;
        } else if (!slot->warned && !getenv("DSCO_OAUTH_CLIENTS_QUIET")) {
            slot->warned = true;
            fprintf(stderr,
                    "dsco: %s is not configured. Set %s in the environment or add it to "
                    "~/.dsco/oauth_clients.json (see .env.example).\n",
                    purpose ? purpose : "OAuth client", env_name);
        }
    }
    pthread_mutex_unlock(&lock);
    return result;
}

#endif /* DSCO_OAUTH_CLIENTS_H */
