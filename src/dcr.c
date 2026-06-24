#include "dcr.h"
#include "json_util.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define DCR_MAX_PROVIDERS 128
#define DCR_MAX_MODELS    256

static dcr_provider_t g_providers[DCR_MAX_PROVIDERS];
static size_t g_provider_count;
static dcr_model_t g_models[DCR_MAX_MODELS];
static size_t g_model_count;
static bool g_loaded;

static const char *dcr_home(void) {
    const char *override = getenv("DSCO_CONFIG_HOME");
    if (override && override[0])
        return override;
    const char *home = getenv("HOME");
    if (!home || !home[0])
        return ".";
    static char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/.dsco/config", home);
    return path;
}

static bool dcr_mkdir_p(const char *path) {
    if (!path || !path[0])
        return false;
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", path);
    size_t len = strlen(tmp);
    if (len == 0)
        return false;
    if (tmp[len - 1] == '/')
        tmp[len - 1] = '\0';
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0700) != 0 && errno != EEXIST)
                return false;
            *p = '/';
        }
    }
    return mkdir(tmp, 0700) == 0 || errno == EEXIST;
}

static bool dcr_is_space_line(const char *s) {
    if (!s)
        return true;
    while (*s) {
        if (!isspace((unsigned char)*s))
            return false;
        s++;
    }
    return true;
}

static char *dcr_trim(char *s) {
    if (!s)
        return s;
    while (isspace((unsigned char)*s))
        s++;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1]))
        *--end = '\0';
    return s;
}

static void dcr_strip_comment(char *s) {
    bool in_quote = false;
    for (char *p = s; p && *p; p++) {
        if (*p == '"' && (p == s || p[-1] != '\\'))
            in_quote = !in_quote;
        if (*p == '#' && !in_quote) {
            *p = '\0';
            return;
        }
    }
}

static void dcr_copy(char *dst, size_t dst_len, const char *src) {
    if (!dst || dst_len == 0)
        return;
    snprintf(dst, dst_len, "%s", src ? src : "");
}

static bool dcr_parse_bool(const char *v, bool def) {
    if (!v || !v[0])
        return def;
    if (strcasecmp(v, "true") == 0 || strcmp(v, "1") == 0 || strcasecmp(v, "yes") == 0 ||
        strcasecmp(v, "on") == 0)
        return true;
    if (strcasecmp(v, "false") == 0 || strcmp(v, "0") == 0 || strcasecmp(v, "no") == 0 ||
        strcasecmp(v, "off") == 0)
        return false;
    return def;
}

static bool dcr_unquote(const char *in, char *out, size_t out_len) {
    if (!out || out_len == 0)
        return false;
    out[0] = '\0';
    if (!in)
        return false;
    while (isspace((unsigned char)*in))
        in++;
    size_t len = strlen(in);
    while (len > 0 && isspace((unsigned char)in[len - 1]))
        len--;
    if (len >= 2 && in[0] == '"' && in[len - 1] == '"') {
        size_t j = 0;
        for (size_t i = 1; i + 1 < len && j + 1 < out_len; i++) {
            if (in[i] == '\\' && i + 1 < len - 1)
                i++;
            out[j++] = in[i];
        }
        out[j] = '\0';
        return true;
    }
    size_t n = len < out_len - 1 ? len : out_len - 1;
    memcpy(out, in, n);
    out[n] = '\0';
    return n > 0;
}

static size_t dcr_parse_string_array(const char *v, char out[][64], size_t max_count) {
    if (!v || !out || max_count == 0)
        return 0;
    const char *p = strchr(v, '[');
    const char *end = strrchr(v, ']');
    if (!p || !end || end <= p)
        return 0;
    p++;
    size_t count = 0;
    while (p < end && count < max_count) {
        while (p < end && (isspace((unsigned char)*p) || *p == ','))
            p++;
        if (p >= end)
            break;
        if (*p == '"') {
            p++;
            size_t j = 0;
            while (p < end && *p != '"' && j + 1 < 64) {
                if (*p == '\\' && p + 1 < end)
                    p++;
                out[count][j++] = *p++;
            }
            out[count][j] = '\0';
            if (*p == '"')
                p++;
            if (out[count][0])
                count++;
        } else {
            size_t j = 0;
            while (p < end && *p != ',' && j + 1 < 64)
                out[count][j++] = *p++;
            out[count][j] = '\0';
            char *t = dcr_trim(out[count]);
            if (t != out[count])
                memmove(out[count], t, strlen(t) + 1);
            if (out[count][0])
                count++;
        }
    }
    return count;
}

static provider_api_mode_t dcr_api_mode_from_wire(const char *wire) {
    if (!wire)
        return PROVIDER_API_CHAT_COMPLETIONS;
    if (strcmp(wire, "chat_completions") == 0 || strcmp(wire, "openai_chat") == 0)
        return PROVIDER_API_CHAT_COMPLETIONS;
    if (strcmp(wire, "responses") == 0 || strcmp(wire, "codex_responses") == 0)
        return PROVIDER_API_CODEX_RESPONSES;
    if (strcmp(wire, "anthropic_messages") == 0)
        return PROVIDER_API_ANTHROPIC_MESSAGES;
    if (strcmp(wire, "bedrock_converse") == 0)
        return PROVIDER_API_BEDROCK_CONVERSE;
    if (strcmp(wire, "acp") == 0)
        return PROVIDER_API_ACP;
    return PROVIDER_API_CHAT_COMPLETIONS;
}

static provider_transport_kind_t dcr_transport_from_wire(const char *wire) {
    if (!wire)
        return PROVIDER_TRANSPORT_OPENAI_CHAT;
    if (strcmp(wire, "responses") == 0 || strcmp(wire, "codex_responses") == 0)
        return PROVIDER_TRANSPORT_CODEX_RESPONSES;
    if (strcmp(wire, "anthropic_messages") == 0)
        return PROVIDER_TRANSPORT_ANTHROPIC_MESSAGES;
    if (strcmp(wire, "bedrock_converse") == 0)
        return PROVIDER_TRANSPORT_BEDROCK_CONVERSE;
    if (strcmp(wire, "acp") == 0)
        return PROVIDER_TRANSPORT_ACP;
    return PROVIDER_TRANSPORT_OPENAI_CHAT;
}

static void dcr_finalize_provider(dcr_provider_t *p) {
    if (!p || !p->name[0])
        return;
    p->profile.name = p->name;
    p->profile.display_name = p->display_name[0] ? p->display_name : p->name;
    p->profile.description = p->description[0] ? p->description : "DSCO dynamic provider config";
    p->profile.base_url = p->base_url;
    p->profile.models_url = p->models_url[0] ? p->models_url : NULL;
    p->profile.transport_base_url = p->transport_base_url[0] ? p->transport_base_url : p->base_url;
    for (int i = 0; i < PROVIDER_PROFILE_MAX_ENV_VARS; i++)
        p->profile.env_vars[i] = p->env_vars[i][0] ? p->env_vars[i] : NULL;
    for (int i = 0; i < PROVIDER_PROFILE_MAX_ALIASES; i++)
        p->profile.aliases[i] = p->aliases[i][0] ? p->aliases[i] : NULL;
    p->profile.default_model = p->default_model[0] ? p->default_model : NULL;
    p->profile.default_aux_model = p->default_aux_model[0] ? p->default_aux_model : NULL;
    p->profile.api_mode = dcr_api_mode_from_wire(p->wire_api);
    p->profile.transport = dcr_transport_from_wire(p->wire_api);
    p->profile.auth_type = PROVIDER_AUTH_API_KEY;
    p->profile.caps = PROVIDER_CAP_MULTITURN | PROVIDER_CAP_STREAMING | PROVIDER_CAP_REASONING;
}

static bool dcr_provider_valid(const dcr_provider_t *p, char *err, size_t err_len) {
    if (!p || !p->name[0]) {
        dcr_copy(err, err_len, "missing provider id/name");
        return false;
    }
    if (!p->base_url[0]) {
        dcr_copy(err, err_len, "missing base_url");
        return false;
    }
    if (!(strncmp(p->base_url, "https://", 8) == 0 || strncmp(p->base_url, "http://localhost", 16) == 0 ||
          strncmp(p->base_url, "http://127.0.0.1", 16) == 0)) {
        dcr_copy(err, err_len, "base_url must be https or loopback http");
        return false;
    }
    if (p->stream_idle_timeout_ms < 0 || p->stream_idle_timeout_ms > 86400000L) {
        dcr_copy(err, err_len, "stream_idle_timeout_ms out of bounds");
        return false;
    }
    if (p->stream_max_retries < 0 || p->stream_max_retries > 20 || p->request_max_retries < 0 ||
        p->request_max_retries > 20) {
        dcr_copy(err, err_len, "retry count out of bounds");
        return false;
    }
    return true;
}

static dcr_provider_t *dcr_provider_slot(const char *name) {
    if (!name || !name[0])
        return NULL;
    for (size_t i = 0; i < g_provider_count; i++) {
        if (strcmp(g_providers[i].name, name) == 0)
            return &g_providers[i];
    }
    if (g_provider_count >= DCR_MAX_PROVIDERS)
        return NULL;
    dcr_provider_t *p = &g_providers[g_provider_count++];
    memset(p, 0, sizeof(*p));
    dcr_copy(p->name, sizeof(p->name), name);
    dcr_copy(p->wire_api, sizeof(p->wire_api), "chat_completions");
    p->confidence = 0.80;
    p->loaded = true;
    return p;
}

static bool dcr_load_provider_toml(const char *path, char *err, size_t err_len) {
    FILE *f = fopen(path, "r");
    if (!f) {
        snprintf(err, err_len, "open failed: %s", strerror(errno));
        return false;
    }
    dcr_provider_t local;
    memset(&local, 0, sizeof(local));
    dcr_copy(local.wire_api, sizeof(local.wire_api), "chat_completions");
    local.confidence = 0.80;
    local.stream_idle_timeout_ms = 0;

    char section[128] = "";
    char line[2048];
    while (fgets(line, sizeof(line), f)) {
        dcr_strip_comment(line);
        char *s = dcr_trim(line);
        if (!s[0] || dcr_is_space_line(s))
            continue;
        size_t slen = strlen(s);
        if (s[0] == '[' && slen > 2 && s[slen - 1] == ']') {
            s[slen - 1] = '\0';
            dcr_copy(section, sizeof(section), s + 1);
            if (strncmp(section, "provider.", 9) == 0 && !local.name[0])
                dcr_copy(local.name, sizeof(local.name), section + 9);
            continue;
        }
        char *eq = strchr(s, '=');
        if (!eq)
            continue;
        *eq = '\0';
        char *key = dcr_trim(s);
        char *val = dcr_trim(eq + 1);
        char tmp[512];
        bool in_provider = strncmp(section, "provider.", 9) == 0 || strcmp(section, "provider") == 0;
        bool in_prov = strstr(section, ".provenance") != NULL || strcmp(section, "provenance") == 0;
        if (!in_provider && !in_prov)
            continue;
        if (strcmp(key, "id") == 0 || strcmp(key, "name") == 0) {
            if (dcr_unquote(val, tmp, sizeof(tmp)))
                dcr_copy(local.name, sizeof(local.name), tmp);
        } else if (strcmp(key, "display_name") == 0) {
            if (dcr_unquote(val, tmp, sizeof(tmp)))
                dcr_copy(local.display_name, sizeof(local.display_name), tmp);
        } else if (strcmp(key, "description") == 0) {
            if (dcr_unquote(val, tmp, sizeof(tmp)))
                dcr_copy(local.description, sizeof(local.description), tmp);
        } else if (strcmp(key, "base_url") == 0) {
            if (dcr_unquote(val, tmp, sizeof(tmp)))
                dcr_copy(local.base_url, sizeof(local.base_url), tmp);
        } else if (strcmp(key, "models_url") == 0) {
            if (dcr_unquote(val, tmp, sizeof(tmp)))
                dcr_copy(local.models_url, sizeof(local.models_url), tmp);
        } else if (strcmp(key, "transport_base_url") == 0) {
            if (dcr_unquote(val, tmp, sizeof(tmp)))
                dcr_copy(local.transport_base_url, sizeof(local.transport_base_url), tmp);
        } else if (strcmp(key, "wire_api") == 0) {
            if (dcr_unquote(val, tmp, sizeof(tmp)))
                dcr_copy(local.wire_api, sizeof(local.wire_api), tmp);
        } else if (strcmp(key, "env_keys") == 0 || strcmp(key, "env_vars") == 0) {
            dcr_parse_string_array(val, local.env_vars, PROVIDER_PROFILE_MAX_ENV_VARS);
        } else if (strcmp(key, "aliases") == 0) {
            dcr_parse_string_array(val, local.aliases, PROVIDER_PROFILE_MAX_ALIASES);
        } else if (strcmp(key, "default_model") == 0) {
            if (dcr_unquote(val, tmp, sizeof(tmp)))
                dcr_copy(local.default_model, sizeof(local.default_model), tmp);
        } else if (strcmp(key, "default_aux_model") == 0) {
            if (dcr_unquote(val, tmp, sizeof(tmp)))
                dcr_copy(local.default_aux_model, sizeof(local.default_aux_model), tmp);
        } else if (strcmp(key, "stream_idle_timeout_ms") == 0) {
            local.stream_idle_timeout_ms = atol(val);
        } else if (strcmp(key, "stream_max_retries") == 0) {
            local.stream_max_retries = atoi(val);
        } else if (strcmp(key, "request_max_retries") == 0) {
            local.request_max_retries = atoi(val);
        } else if (strcmp(key, "idempotent_retries") == 0) {
            local.idempotent_retries = dcr_parse_bool(val, false);
        } else if (strcmp(key, "source") == 0) {
            if (dcr_unquote(val, tmp, sizeof(tmp)))
                dcr_copy(local.provenance_source, sizeof(local.provenance_source), tmp);
        } else if (strcmp(key, "url") == 0 || strcmp(key, "source_url") == 0) {
            if (dcr_unquote(val, tmp, sizeof(tmp)))
                dcr_copy(local.provenance_url, sizeof(local.provenance_url), tmp);
        } else if (strcmp(key, "confidence") == 0) {
            local.confidence = atof(val);
        }
    }
    fclose(f);

    char verr[256];
    if (!dcr_provider_valid(&local, verr, sizeof(verr))) {
        snprintf(err, err_len, "%s: %s", path, verr);
        return false;
    }
    dcr_provider_t *slot = dcr_provider_slot(local.name);
    if (!slot) {
        snprintf(err, err_len, "provider registry full");
        return false;
    }
    *slot = local;
    slot->loaded = true;
    dcr_finalize_provider(slot);
    return true;
}

static bool dcr_model_matches(const dcr_model_t *m, const char *q) {
    if (!m || !q || !q[0])
        return false;
    if (strcmp(m->id, q) == 0 || strcmp(m->wire_model, q) == 0)
        return true;
    for (size_t i = 0; i < m->alias_count; i++) {
        if (strcmp(m->aliases[i], q) == 0)
            return true;
    }
    return false;
}

static bool dcr_load_model_json(const char *path, char *err, size_t err_len) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(err, err_len, "open failed: %s", strerror(errno));
        return false;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > 1024 * 1024) {
        fclose(f);
        snprintf(err, err_len, "model file size invalid");
        return false;
    }
    char *buf = safe_malloc((size_t)n + 1);
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        free(buf);
        snprintf(err, err_len, "read failed");
        return false;
    }
    fclose(f);
    buf[n] = '\0';

    if (g_model_count >= DCR_MAX_MODELS) {
        free(buf);
        snprintf(err, err_len, "model registry full");
        return false;
    }
    dcr_model_t m;
    memset(&m, 0, sizeof(m));
    char *s;
    s = json_get_str(buf, "id");
    if (s) { dcr_copy(m.id, sizeof(m.id), s); free(s); }
    s = json_get_str(buf, "provider");
    if (s) { dcr_copy(m.provider, sizeof(m.provider), s); free(s); }
    s = json_get_str(buf, "display_name");
    if (s) { dcr_copy(m.display_name, sizeof(m.display_name), s); free(s); }
    s = json_get_str(buf, "wire_model");
    if (s) { dcr_copy(m.wire_model, sizeof(m.wire_model), s); free(s); }
    if (!m.wire_model[0])
        dcr_copy(m.wire_model, sizeof(m.wire_model), m.id);
    m.context_window = json_get_int(buf, "context_window", 0);
    m.max_output_tokens = json_get_int(buf, "max_output_tokens", 0);
    m.supports_reasoning = json_get_bool(buf, "supports_reasoning", false);
    m.confidence = 0.80;
    m.loaded = true;
    if (!m.id[0] || !m.provider[0]) {
        free(buf);
        snprintf(err, err_len, "model requires id and provider");
        return false;
    }
    g_models[g_model_count++] = m;
    free(buf);
    return true;
}

static void dcr_load_dir(const char *dir, bool providers) {
    DIR *d = opendir(dir);
    if (!d)
        return;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.')
            continue;
        const char *ext = strrchr(de->d_name, '.');
        if (providers && (!ext || strcmp(ext, ".toml") != 0))
            continue;
        if (!providers && (!ext || strcmp(ext, ".json") != 0))
            continue;
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
        char err[512];
        if (providers)
            dcr_load_provider_toml(path, err, sizeof(err));
        else
            dcr_load_model_json(path, err, sizeof(err));
    }
    closedir(d);
}

void dcr_init(void) {
    if (g_loaded)
        return;
    dcr_reload();
}

void dcr_reload(void) {
    memset(g_providers, 0, sizeof(g_providers));
    memset(g_models, 0, sizeof(g_models));
    g_provider_count = 0;
    g_model_count = 0;
    const char *home = dcr_home();
    char providers[PATH_MAX], models[PATH_MAX];
    snprintf(providers, sizeof(providers), "%s/providers.d", home);
    snprintf(models, sizeof(models), "%s/models.d", home);
    dcr_load_dir(providers, true);
    dcr_load_dir(models, false);
    g_loaded = true;
}

bool dcr_is_loaded(void) { return g_loaded; }

size_t dcr_provider_count(void) { dcr_init(); return g_provider_count; }
const dcr_provider_t *dcr_provider_at(size_t idx) { dcr_init(); return idx < g_provider_count ? &g_providers[idx] : NULL; }

const dcr_provider_t *dcr_provider_find(const char *name_or_alias) {
    dcr_init();
    if (!name_or_alias || !name_or_alias[0])
        return NULL;
    for (size_t i = 0; i < g_provider_count; i++) {
        const dcr_provider_t *p = &g_providers[i];
        if (strcmp(p->name, name_or_alias) == 0)
            return p;
        for (int j = 0; j < PROVIDER_PROFILE_MAX_ALIASES && p->aliases[j][0]; j++) {
            if (strcmp(p->aliases[j], name_or_alias) == 0)
                return p;
        }
    }
    return NULL;
}

const provider_profile_t *dcr_provider_profile_find(const char *name_or_alias) {
    const dcr_provider_t *p = dcr_provider_find(name_or_alias);
    return p ? &p->profile : NULL;
}

const char *dcr_provider_primary_env_var(const dcr_provider_t *p) {
    return p && p->env_vars[0][0] ? p->env_vars[0] : NULL;
}

bool dcr_provider_has_env_var(const dcr_provider_t *p, const char *env_var) {
    if (!p || !env_var || !env_var[0])
        return false;
    for (int i = 0; i < PROVIDER_PROFILE_MAX_ENV_VARS && p->env_vars[i][0]; i++) {
        if (strcmp(p->env_vars[i], env_var) == 0)
            return true;
    }
    return false;
}

const char *dcr_provider_wire_api(const char *provider) {
    const dcr_provider_t *p = dcr_provider_find(provider);
    return p && p->wire_api[0] ? p->wire_api : NULL;
}

long dcr_provider_stream_idle_timeout_ms(const char *provider, long fallback_ms) {
    const dcr_provider_t *p = dcr_provider_find(provider);
    return p && p->stream_idle_timeout_ms > 0 ? p->stream_idle_timeout_ms : fallback_ms;
}

int dcr_provider_stream_max_retries(const char *provider, int fallback) {
    const dcr_provider_t *p = dcr_provider_find(provider);
    return p && p->stream_max_retries > 0 ? p->stream_max_retries : fallback;
}

int dcr_provider_request_max_retries(const char *provider, int fallback) {
    const dcr_provider_t *p = dcr_provider_find(provider);
    return p && p->request_max_retries > 0 ? p->request_max_retries : fallback;
}

size_t dcr_model_count(void) { dcr_init(); return g_model_count; }
const dcr_model_t *dcr_model_at(size_t idx) { dcr_init(); return idx < g_model_count ? &g_models[idx] : NULL; }
const dcr_model_t *dcr_model_find(const char *id_or_alias) {
    dcr_init();
    for (size_t i = 0; i < g_model_count; i++) {
        if (dcr_model_matches(&g_models[i], id_or_alias))
            return &g_models[i];
    }
    return NULL;
}

const char *dcr_reasoning_effort_normalize(const char *provider, const char *model, const char *effort,
                                           char *out, size_t out_len) {
    if (!out || out_len == 0)
        return NULL;
    const char *e = effort && effort[0] ? effort : "high";
    const dcr_model_t *m = dcr_model_find(model);
    if (m) {
        for (size_t i = 0; i < m->effort_alias_count; i++) {
            if (strcmp(m->effort_alias_from[i], e) == 0) {
                dcr_copy(out, out_len, m->effort_alias_to[i]);
                return out;
            }
        }
    }
    if (provider && strcmp(provider, "sakana") == 0) {
        if (strcmp(e, "max") == 0 || strcmp(e, "xhigh") == 0)
            dcr_copy(out, out_len, "xhigh");
        else
            dcr_copy(out, out_len, "high");
        return out;
    }
    dcr_copy(out, out_len, e);
    return out;
}

static int dcr_write_default_sakana(void) {
    const char *home = dcr_home();
    char providers[PATH_MAX], models[PATH_MAX], imports[PATH_MAX], path[PATH_MAX];
    snprintf(providers, sizeof(providers), "%s/providers.d", home);
    snprintf(models, sizeof(models), "%s/models.d", home);
    snprintf(imports, sizeof(imports), "%s/imports", home);
    dcr_mkdir_p(providers);
    dcr_mkdir_p(models);
    dcr_mkdir_p(imports);
    snprintf(path, sizeof(path), "%s/sakana.toml", providers);
    FILE *f = fopen(path, "wx");
    if (!f && errno == EEXIST)
        return 0;
    if (!f) {
        fprintf(stderr, "dcr: cannot write %s: %s\n", path, strerror(errno));
        return 1;
    }
    fprintf(f,
            "[provider.sakana]\n"
            "id = \"sakana\"\n"
            "display_name = \"Sakana API\"\n"
            "description = \"Sakana Fugu multi-agent model provider\"\n"
            "base_url = \"https://api.sakana.ai/v1\"\n"
            "wire_api = \"chat_completions\"\n"
            "env_keys = [\"FUGU_API_KEY\", \"SAKANA_API_KEY\", \"FISH_API_KEY\", \"SAKANA_TOKEN\"]\n"
            "aliases = [\"fugu\", \"sakana-ai\"]\n"
            "default_model = \"fugu\"\n"
            "stream_idle_timeout_ms = 7200000\n"
            "stream_max_retries = 5\n"
            "request_max_retries = 4\n"
            "idempotent_retries = true\n"
            "\n[provider.sakana.provenance]\n"
            "source = \"sakana_fugu_docs\"\n"
            "url = \"https://api.sakana.ai\"\n"
            "confidence = 0.95\n");
    fclose(f);
    return 0;
}

static int dcr_ingest_codex(bool dry_run) {
    const char *home = getenv("HOME");
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/.codex/config.toml", home ? home : ".");
    FILE *f = fopen(path, "r");
    if (!f) {
        printf("{\"ok\":true,\"source\":\"codex\",\"found\":false,\"path\":\"");
        for (const char *p = path; *p; p++) { if (*p == '"' || *p == '\\') putchar('\\'); putchar(*p); }
        printf("\"}\n");
        return 0;
    }
    bool in_sakana = false;
    char line[1024];
    char base_url[512] = "";
    char env_key[64] = "";
    char wire_api[64] = "";
    long idle = 0;
    int stream_retries = 0, request_retries = 0;
    while (fgets(line, sizeof(line), f)) {
        dcr_strip_comment(line);
        char *s = dcr_trim(line);
        if (!s[0])
            continue;
        if (s[0] == '[') {
            in_sakana = strstr(s, "model_providers.sakana") != NULL;
            continue;
        }
        if (!in_sakana)
            continue;
        char *eq = strchr(s, '=');
        if (!eq)
            continue;
        *eq = '\0';
        char *key = dcr_trim(s);
        char *val = dcr_trim(eq + 1);
        char tmp[512];
        if (strcmp(key, "base_url") == 0 && dcr_unquote(val, tmp, sizeof(tmp)))
            dcr_copy(base_url, sizeof(base_url), tmp);
        else if (strcmp(key, "env_key") == 0 && dcr_unquote(val, tmp, sizeof(tmp)))
            dcr_copy(env_key, sizeof(env_key), tmp);
        else if (strcmp(key, "wire_api") == 0 && dcr_unquote(val, tmp, sizeof(tmp)))
            dcr_copy(wire_api, sizeof(wire_api), tmp);
        else if (strcmp(key, "stream_idle_timeout_ms") == 0)
            idle = atol(val);
        else if (strcmp(key, "stream_max_retries") == 0)
            stream_retries = atoi(val);
        else if (strcmp(key, "request_max_retries") == 0)
            request_retries = atoi(val);
    }
    fclose(f);
    printf("{\"ok\":true,\"source\":\"codex\",\"found\":true,\"dry_run\":%s,"
           "\"provider\":\"sakana\",\"base_url\":\"%s\",\"env_key\":\"%s\","
           "\"wire_api\":\"%s\",\"stream_idle_timeout_ms\":%ld,"
           "\"stream_max_retries\":%d,\"request_max_retries\":%d}\n",
           dry_run ? "true" : "false", base_url, env_key, wire_api, idle, stream_retries,
           request_retries);
    return 0;
}

int dcr_cli(int argc, char **argv) {
    const char *cmd = argc >= 3 ? argv[2] : "show";
    if (strcmp(cmd, "init") == 0) {
        int rc = dcr_write_default_sakana();
        dcr_reload();
        printf("{\"ok\":%s,\"config_home\":\"%s\",\"providers\":%zu,\"models\":%zu}\n",
               rc == 0 ? "true" : "false", dcr_home(), dcr_provider_count(), dcr_model_count());
        return rc;
    }
    if (strcmp(cmd, "validate") == 0 || strcmp(cmd, "compile") == 0) {
        dcr_reload();
        printf("{\"ok\":true,\"providers\":%zu,\"models\":%zu}\n", dcr_provider_count(), dcr_model_count());
        return 0;
    }
    if (strcmp(cmd, "ingest") == 0) {
        const char *source = argc >= 4 ? argv[3] : "codex";
        bool dry = false;
        for (int i = 4; i < argc; i++)
            if (strcmp(argv[i], "--dry-run") == 0)
                dry = true;
        if (strcmp(source, "codex") == 0 || strcmp(source, "codex-fugu") == 0)
            return dcr_ingest_codex(dry);
        fprintf(stderr, "dcr: unknown ingest source '%s'\n", source);
        return 2;
    }
    if (strcmp(cmd, "explain") == 0) {
        const char *key = argc >= 4 ? argv[3] : "";
        dcr_reload();
        if (strstr(key, "provider.sakana.stream_idle_timeout_ms")) {
            long v = dcr_provider_stream_idle_timeout_ms("sakana", 7200000L);
            printf("provider.sakana.stream_idle_timeout_ms = %ld\nsource = DSCO DCR / Sakana Fugu docs\nreason = slow multi-agent streams need long idle tolerance\n", v);
            return 0;
        }
        fprintf(stderr, "dcr: no explanation for '%s'\n", key);
        return 1;
    }
    dcr_reload();
    printf("{\"ok\":true,\"config_home\":\"%s\",\"providers\":[", dcr_home());
    for (size_t i = 0; i < g_provider_count; i++) {
        const dcr_provider_t *p = &g_providers[i];
        printf("%s{\"id\":\"%s\",\"base_url\":\"%s\",\"wire_api\":\"%s\",\"stream_idle_timeout_ms\":%ld}",
               i ? "," : "", p->name, p->base_url, p->wire_api, p->stream_idle_timeout_ms);
    }
    printf("],\"models\":%zu}\n", g_model_count);
    return 0;
}
