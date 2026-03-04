#include "setup.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

typedef struct {
    char *key;
    char *value;
} kv_entry_t;

typedef struct {
    kv_entry_t *items;
    int count;
    int cap;
} kv_list_t;

extern char **environ;

static char s_env_path[PATH_MAX] = {0};
static char s_profile[128] = {0};

static const char *k_known_env_keys[] = {
    "ANTHROPIC_API_KEY",
    "OPENAI_API_KEY",
    "JINA_API_KEY",
    "PARALLEL_API_KEY",
    "OPENROUTER_API_KEY",
    "TOGETHER_API_KEY",
    "GROQ_API_KEY",
    "DEEPSEEK_API_KEY",
    "MISTRAL_API_KEY",
    "COHERE_API_KEY",
    "XAI_API_KEY",
    "CEREBRAS_API_KEY",
    "GOOGLE_API_KEY",
    "GOOGLE_AI_API_KEY",
    "GEMINI_API_KEY",
    "AZURE_OPENAI_API_KEY",
    "PERPLEXITY_API_KEY",
    "REPLICATE_API_TOKEN",
    "HF_TOKEN",
    "GITHUB_TOKEN",
    "AWS_ACCESS_KEY_ID",
    "AWS_SECRET_ACCESS_KEY",
    "TAVILY_API_KEY",
    "SERPAPI_API_KEY",
    "FIRECRAWL_API_KEY",
    "BRAVE_API_KEY",
    "ELEVENLABS_API_KEY",
    "ASSEMBLYAI_API_KEY",
    "PINECONE_API_KEY",
    "WEAVIATE_API_KEY",
    "SUPABASE_API_KEY",
    "NEON_API_KEY",
    "UPSTASH_REDIS_REST_TOKEN",
    "NOTION_API_KEY",
    "SLACK_BOT_TOKEN",
    "DISCORD_TOKEN",
    "TWILIO_AUTH_TOKEN",
    "STRIPE_API_KEY",
    "MAPBOX_API_KEY",
    "OPENWEATHERMAP_API_KEY",
    "ALPHA_VANTAGE_API_KEY",
    "FRED_API_KEY",
    "DSCO_MODEL",
    "DSCO_BASELINE_DB",
    "DSCO_TIMELINE_PORT",
    NULL
};

typedef struct {
    const char *canonical;
    const char *aliases[8];
} alias_map_t;

static const alias_map_t k_aliases[] = {
    { "ANTHROPIC_API_KEY", { "CLAUDE_API_KEY", NULL } },
    { "OPENAI_API_KEY", { "OPENAI_KEY", "CHATGPT_API_KEY", NULL } },
    { "JINA_API_KEY", { "JINA_AUTH_TOKEN", NULL } },
    { "PARALLEL_API_KEY", { "PARALLEL_AUTH_TOKEN", "PARALLEL_TOKEN", NULL } },
    { "GITHUB_TOKEN", { "GH_TOKEN", NULL } },
    { "GOOGLE_API_KEY", { "GEMINI_API_KEY", "GOOGLE_AI_API_KEY", NULL } },
    { "HF_TOKEN", { "HUGGINGFACE_TOKEN", NULL } },
    { "OPENROUTER_API_KEY", { "OPEN_ROUTER_API_KEY", NULL } },
    { "TOGETHER_API_KEY", { "TOGETHER_TOKEN", NULL } },
    { NULL, { NULL } }
};

static bool has_suffix(const char *s, const char *suffix) {
    if (!s || !suffix) return false;
    size_t ls = strlen(s), lf = strlen(suffix);
    if (lf > ls) return false;
    return strcmp(s + (ls - lf), suffix) == 0;
}

static const char *resolve_alias_value(const char *canonical, const char **alias_used) {
    if (alias_used) *alias_used = NULL;
    if (!canonical) return NULL;
    for (int i = 0; k_aliases[i].canonical; i++) {
        if (strcmp(k_aliases[i].canonical, canonical) != 0) continue;
        for (int j = 0; k_aliases[i].aliases[j]; j++) {
            const char *alias = k_aliases[i].aliases[j];
            const char *val = getenv(alias);
            if (val && val[0]) {
                if (alias_used) *alias_used = alias;
                return val;
            }
        }
        break;
    }
    return NULL;
}

static const char *canonical_from_alias(const char *name) {
    if (!name) return NULL;
    for (int i = 0; k_aliases[i].canonical; i++) {
        for (int j = 0; k_aliases[i].aliases[j]; j++) {
            if (strcmp(k_aliases[i].aliases[j], name) == 0) {
                return k_aliases[i].canonical;
            }
        }
    }
    return NULL;
}

static bool should_capture_generic_key(const char *name) {
    if (!name || !*name) return false;

    /* Skip known noisy shell internals. */
    if (strcmp(name, "_") == 0) return false;
    if (strcmp(name, "SHLVL") == 0) return false;

    if (has_suffix(name, "_API_KEY") ||
        has_suffix(name, "_ACCESS_TOKEN") ||
        has_suffix(name, "_AUTH_TOKEN") ||
        has_suffix(name, "_SECRET") ||
        has_suffix(name, "_TOKEN")) {
        return true;
    }
    return false;
}

static bool is_valid_env_name(const char *name) {
    if (!name || !*name) return false;
    if (!(isalpha((unsigned char)name[0]) || name[0] == '_')) return false;
    for (const char *p = name + 1; *p; p++) {
        if (!(isalnum((unsigned char)*p) || *p == '_')) return false;
    }
    return true;
}

static char *trim_in_place(char *s) {
    if (!s) return s;
    while (*s && isspace((unsigned char)*s)) s++;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) end--;
    *end = '\0';
    return s;
}

static void sbuf_appendf(char *out, size_t out_len, size_t *pos, const char *fmt, ...) {
    if (!out || out_len == 0 || !pos || *pos >= out_len) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(out + *pos, out_len - *pos, fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    size_t wrote = (size_t)n;
    if (*pos + wrote >= out_len) {
        *pos = out_len - 1;
    } else {
        *pos += wrote;
    }
}

static void mask_value(const char *val, char *out, size_t out_len) {
    if (!out || out_len == 0) return;
    if (!val || !val[0]) {
        snprintf(out, out_len, "(unset)");
        return;
    }
    size_t n = strlen(val);
    if (n <= 6) {
        snprintf(out, out_len, "**** (%zu chars)", n);
        return;
    }
    char prefix[5] = {0};
    char suffix[5] = {0};
    memcpy(prefix, val, 3);
    memcpy(suffix, val + n - 3, 3);
    snprintf(out, out_len, "%s...%s (%zu chars)", prefix, suffix, n);
}

static void kv_list_init(kv_list_t *l) {
    l->items = NULL;
    l->count = 0;
    l->cap = 0;
}

static void kv_list_free(kv_list_t *l) {
    if (!l) return;
    for (int i = 0; i < l->count; i++) {
        free(l->items[i].key);
        free(l->items[i].value);
    }
    free(l->items);
    l->items = NULL;
    l->count = 0;
    l->cap = 0;
}

static int kv_list_find(const kv_list_t *l, const char *key) {
    for (int i = 0; i < l->count; i++) {
        if (strcmp(l->items[i].key, key) == 0) return i;
    }
    return -1;
}

static bool kv_list_reserve(kv_list_t *l, int need) {
    if (need <= l->cap) return true;
    int ncap = l->cap > 0 ? l->cap * 2 : 32;
    while (ncap < need) ncap *= 2;
    kv_entry_t *n = realloc(l->items, (size_t)ncap * sizeof(*n));
    if (!n) return false;
    l->items = n;
    l->cap = ncap;
    return true;
}

static bool kv_list_set(kv_list_t *l, const char *key, const char *value, bool *was_update) {
    if (!is_valid_env_name(key)) return false;

    int idx = kv_list_find(l, key);
    if (idx >= 0) {
        if (was_update) *was_update = true;
        char *nv = strdup(value ? value : "");
        if (!nv) return false;
        free(l->items[idx].value);
        l->items[idx].value = nv;
        return true;
    }

    if (!kv_list_reserve(l, l->count + 1)) return false;
    l->items[l->count].key = strdup(key);
    l->items[l->count].value = strdup(value ? value : "");
    if (!l->items[l->count].key || !l->items[l->count].value) {
        free(l->items[l->count].key);
        free(l->items[l->count].value);
        return false;
    }
    l->count++;
    if (was_update) *was_update = false;
    return true;
}

static int cmp_kv_by_key(const void *a, const void *b) {
    const kv_entry_t *ka = (const kv_entry_t *)a;
    const kv_entry_t *kb = (const kv_entry_t *)b;
    return strcmp(ka->key, kb->key);
}

static bool mkdir_p(const char *path) {
    char tmp[PATH_MAX];
    size_t n = strlen(path);
    if (n == 0 || n >= sizeof(tmp)) return false;
    memcpy(tmp, path, n + 1);

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return false;
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return false;
    return true;
}

static bool ensure_parent_dir(const char *file_path) {
    char tmp[PATH_MAX];
    size_t n = strlen(file_path);
    if (n == 0 || n >= sizeof(tmp)) return false;
    memcpy(tmp, file_path, n + 1);

    char *slash = strrchr(tmp, '/');
    if (!slash) return true;
    *slash = '\0';
    if (tmp[0] == '\0') return true;
    return mkdir_p(tmp);
}

static bool is_valid_profile_name(const char *s) {
    if (!s || !*s) return false;
    for (const char *p = s; *p; p++) {
        if (!(isalnum((unsigned char)*p) || *p == '_' || *p == '-' || *p == '.')) {
            return false;
        }
    }
    return true;
}

const char *dsco_setup_profile_name(void) {
    if (s_profile[0]) return s_profile;
    const char *profile = getenv("DSCO_PROFILE");
    if (!profile || !profile[0]) profile = "default";
    if (!is_valid_profile_name(profile)) profile = "default";
    snprintf(s_profile, sizeof(s_profile), "%s", profile);
    return s_profile;
}

static const char *resolve_env_path(void) {
    if (s_env_path[0]) return s_env_path;

    const char *override = getenv("DSCO_ENV_FILE");
    if (override && override[0]) {
        snprintf(s_env_path, sizeof(s_env_path), "%s", override);
        return s_env_path;
    }

    const char *home = getenv("HOME");
    const char *profile = dsco_setup_profile_name();
    if (home && home[0]) {
        if (strcmp(profile, "default") == 0) {
            snprintf(s_env_path, sizeof(s_env_path), "%s/.dsco/env", home);
        } else {
            snprintf(s_env_path, sizeof(s_env_path), "%s/.dsco/profiles/%s.env", home, profile);
        }
    } else {
        if (strcmp(profile, "default") == 0) {
            snprintf(s_env_path, sizeof(s_env_path), ".dsco/env");
        } else {
            snprintf(s_env_path, sizeof(s_env_path), ".dsco/%s.env", profile);
        }
    }
    return s_env_path;
}

const char *dsco_setup_env_path(void) {
    return resolve_env_path();
}

static bool load_kv_file(const char *path, kv_list_t *out) {
    FILE *f = fopen(path, "r");
    if (!f) return false;

    char line[8192];
    while (fgets(line, sizeof(line), f)) {
        char *p = trim_in_place(line);
        if (*p == '\0' || *p == '#') continue;

        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';

        char *key = trim_in_place(p);
        char *val = eq + 1;

        size_t vlen = strlen(val);
        while (vlen > 0 && (val[vlen - 1] == '\n' || val[vlen - 1] == '\r')) {
            val[--vlen] = '\0';
        }

        if ((val[0] == '"' && vlen >= 2 && val[vlen - 1] == '"') ||
            (val[0] == '\'' && vlen >= 2 && val[vlen - 1] == '\'')) {
            val[vlen - 1] = '\0';
            val++;
        }

        if (!is_valid_env_name(key)) continue;
        bool updated = false;
        if (!kv_list_set(out, key, val, &updated)) {
            fclose(f);
            return false;
        }
    }

    fclose(f);
    return true;
}

static bool write_kv_file(const char *path, kv_list_t *l) {
    if (!ensure_parent_dir(path)) return false;

    qsort(l->items, (size_t)l->count, sizeof(l->items[0]), cmp_kv_by_key);

    char tmp_path[PATH_MAX];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.%d", path, (int)getpid());

    FILE *f = fopen(tmp_path, "w");
    if (!f) return false;

    time_t now = time(NULL);
    fprintf(f, "# dsco setup env file\n");
    fprintf(f, "# generated at %ld\n", (long)now);
    fprintf(f, "# DO NOT commit this file; it contains secrets.\n\n");

    for (int i = 0; i < l->count; i++) {
        fprintf(f, "%s=%s\n", l->items[i].key, l->items[i].value);
    }

    if (fclose(f) != 0) {
        unlink(tmp_path);
        return false;
    }

    chmod(tmp_path, 0600);

    if (rename(tmp_path, path) != 0) {
        unlink(tmp_path);
        return false;
    }

    chmod(path, 0600);
    return true;
}

static bool process_env_candidate(kv_list_t *l, const char *key, const char *val,
                                  bool overwrite, int *added, int *updated, int *unchanged) {
    if (!key || !*key || !val || !*val) return true;

    int idx = kv_list_find(l, key);
    if (idx < 0) {
        bool was_update = false;
        if (!kv_list_set(l, key, val, &was_update)) return false;
        (*added)++;
        return true;
    }

    if (overwrite && strcmp(l->items[idx].value, val) != 0) {
        bool was_update = false;
        if (!kv_list_set(l, key, val, &was_update)) return false;
        (*updated)++;
    } else {
        (*unchanged)++;
    }
    return true;
}

int dsco_setup_load_saved_env(void) {
    kv_list_t l;
    kv_list_init(&l);

    if (!load_kv_file(resolve_env_path(), &l)) {
        kv_list_free(&l);
        return 0;
    }

    int loaded = 0;
    for (int i = 0; i < l.count; i++) {
        if (!getenv(l.items[i].key)) {
            setenv(l.items[i].key, l.items[i].value, 1);
            loaded++;
        }
    }

    kv_list_free(&l);
    return loaded;
}

int dsco_setup_autopopulate(bool overwrite, bool include_generic,
                            char *summary, size_t summary_len) {
    kv_list_t l;
    kv_list_init(&l);
    kv_list_t seen;
    kv_list_init(&seen);

    const char *path = resolve_env_path();
    bool had_file = (access(path, F_OK) == 0);
    if (had_file) {
        if (!load_kv_file(path, &l)) {
            if (summary && summary_len > 0)
                snprintf(summary, summary_len, "setup failed: could not parse %s", path);
            kv_list_free(&l);
            kv_list_free(&seen);
            return -1;
        }
    }

    int discovered = 0;
    int added = 0, updated = 0, unchanged = 0, alias_mapped = 0;

    for (int i = 0; k_known_env_keys[i]; i++) {
        const char *name = k_known_env_keys[i];
        const char *val = getenv(name);
        const char *alias_used = NULL;
        if (!val || !val[0]) {
            val = resolve_alias_value(name, &alias_used);
        }
        if (val && val[0]) {
            if (kv_list_find(&seen, name) < 0) {
                bool was_update = false;
                if (!kv_list_set(&seen, name, "", &was_update)) {
                    if (summary && summary_len > 0)
                        snprintf(summary, summary_len, "setup failed: out of memory while processing %s", name);
                    kv_list_free(&l);
                    kv_list_free(&seen);
                    return -1;
                }
                discovered++;
            }
            if (alias_used) alias_mapped++;
            if (!process_env_candidate(&l, name, val, overwrite, &added, &updated, &unchanged)) {
                if (summary && summary_len > 0)
                    snprintf(summary, summary_len, "setup failed: out of memory while processing %s", name);
                kv_list_free(&l);
                kv_list_free(&seen);
                return -1;
            }
        }
    }

    if (include_generic) {
        for (char **e = environ; e && *e; e++) {
            const char *pair = *e;
            const char *eq = strchr(pair, '=');
            if (!eq) continue;

            size_t klen = (size_t)(eq - pair);
            if (klen == 0 || klen >= 256) continue;

            char key[256];
            memcpy(key, pair, klen);
            key[klen] = '\0';

            if (!should_capture_generic_key(key)) continue;
            if (!is_valid_env_name(key)) continue;
            if (canonical_from_alias(key)) continue;

            const char *val = eq + 1;
            if (!val || !*val) continue;

            bool first_seen = false;
            if (kv_list_find(&seen, key) < 0) {
                bool was_update = false;
                if (!kv_list_set(&seen, key, "", &was_update)) {
                    if (summary && summary_len > 0)
                        snprintf(summary, summary_len, "setup failed: out of memory while processing %s", key);
                    kv_list_free(&l);
                    kv_list_free(&seen);
                    return -1;
                }
                discovered++;
                first_seen = true;
            }
            if (!first_seen) {
                /* Avoid double processing keys already handled in known list. */
                continue;
            }
            if (!process_env_candidate(&l, key, val, overwrite, &added, &updated, &unchanged)) {
                if (summary && summary_len > 0)
                    snprintf(summary, summary_len, "setup failed: out of memory while processing %s", key);
                kv_list_free(&l);
                kv_list_free(&seen);
                return -1;
            }
        }
    }

    bool should_write = (!had_file) || added > 0 || updated > 0;
    if (should_write) {
        if (!write_kv_file(path, &l)) {
            if (summary && summary_len > 0)
                snprintf(summary, summary_len, "setup failed: could not write %s", path);
            kv_list_free(&l);
            kv_list_free(&seen);
            return -1;
        }
    }

    if (summary && summary_len > 0) {
        snprintf(summary, summary_len,
                 "setup complete: profile=%s discovered=%d aliases=%d added=%d updated=%d unchanged=%d file=%s",
                 dsco_setup_profile_name(), discovered, alias_mapped, added, updated, unchanged, path);
    }

    kv_list_free(&l);
    kv_list_free(&seen);
    return discovered;
}

int dsco_setup_bootstrap_from_env(char *summary, size_t summary_len) {
    const char *path = resolve_env_path();
    if (access(path, F_OK) == 0) {
        if (summary && summary_len > 0) {
            snprintf(summary, summary_len, "bootstrap skipped: config exists at %s", path);
        }
        return 0;
    }

    char tmp[512];
    int discovered = dsco_setup_autopopulate(false, true, tmp, sizeof(tmp));
    if (discovered < 0) {
        if (summary && summary_len > 0) snprintf(summary, summary_len, "%s", tmp);
        return -1;
    }

    if (discovered == 0) {
        unlink(path);
        if (summary && summary_len > 0) {
            snprintf(summary, summary_len,
                     "bootstrap skipped: no API keys/tokens found in current env");
        }
        return 0;
    }

    if (summary && summary_len > 0) {
        snprintf(summary, summary_len,
                 "bootstrap created dsco env from current environment (profile=%s, %d keys): %s",
                 dsco_setup_profile_name(), discovered, path);
    }
    return 1;
}

int dsco_setup_report(char *out, size_t out_len) {
    if (!out || out_len == 0) return -1;
    out[0] = '\0';
    size_t pos = 0;

    kv_list_t saved;
    kv_list_init(&saved);
    const char *path = resolve_env_path();
    bool has_saved_file = load_kv_file(path, &saved);

    sbuf_appendf(out, out_len, &pos, "dsco setup report\n");
    sbuf_appendf(out, out_len, &pos, "profile: %s\n", dsco_setup_profile_name());
    sbuf_appendf(out, out_len, &pos, "env file: %s (%s)\n",
                 path, has_saved_file ? "present" : "missing");

    int available = 0;
    int missing = 0;

    for (int i = 0; k_known_env_keys[i]; i++) {
        const char *name = k_known_env_keys[i];
        const char *runtime = getenv(name);
        const char *alias_used = NULL;
        const char *alias_val = NULL;
        int saved_idx = kv_list_find(&saved, name);
        const char *saved_val = (saved_idx >= 0) ? saved.items[saved_idx].value : NULL;

        if (!runtime || !runtime[0]) {
            alias_val = resolve_alias_value(name, &alias_used);
        }

        const char *effective = (runtime && runtime[0]) ? runtime : alias_val;
        char masked[128];
        mask_value(effective ? effective : saved_val, masked, sizeof(masked));

        const char *source = "missing";
        if (runtime && runtime[0]) {
            if (saved_val && strcmp(runtime, saved_val) == 0) source = "saved+runtime";
            else source = "runtime";
        } else if (alias_val) {
            source = "runtime-alias";
        } else if (saved_val && saved_val[0]) {
            source = "saved";
        }

        bool is_available = (effective && effective[0]) || (saved_val && saved_val[0]);
        if (is_available) available++;
        else missing++;

        if (alias_used) {
            sbuf_appendf(out, out_len, &pos, "  %-28s : %-14s %-24s alias=%s\n",
                         name, source, masked, alias_used);
        } else {
            sbuf_appendf(out, out_len, &pos, "  %-28s : %-14s %s\n",
                         name, source, masked);
        }
    }

    int extra_saved = 0;
    for (int i = 0; i < saved.count; i++) {
        bool known = false;
        for (int j = 0; k_known_env_keys[j]; j++) {
            if (strcmp(saved.items[i].key, k_known_env_keys[j]) == 0) {
                known = true;
                break;
            }
        }
        if (!known) extra_saved++;
    }

    sbuf_appendf(out, out_len, &pos, "\nsummary: available=%d missing=%d extra_saved=%d\n",
                 available, missing, extra_saved);

    kv_list_free(&saved);
    return (int)pos;
}
