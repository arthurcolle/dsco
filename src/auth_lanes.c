#include "auth_lanes.h"

#include "config.h"
#include "json_util.h"
#include "provider.h"
#include "provider_profiles.h"
#include "subscription_bench.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define AUTH_LANE_PATH_MAX 2048
#define AUTH_LANE_PROFILE_MAX 96
#define AUTH_LANE_DISCOVER_MAX 64

typedef enum {
    AUTH_CLI_GROK = 0,
    AUTH_CLI_KIMI,
} auth_cli_kind_t;

typedef struct {
    char names[AUTH_LANE_DISCOVER_MAX][AUTH_LANE_PROFILE_MAX];
    size_t count;
} auth_profile_names_t;

bool dsco_auth_profile_name_valid(const char *profile) {
    if (!profile || !profile[0] || strlen(profile) >= AUTH_LANE_PROFILE_MAX)
        return false;
    if (strcmp(profile, ".") == 0 || strcmp(profile, "..") == 0)
        return false;
    for (const unsigned char *p = (const unsigned char *)profile; *p; p++) {
        if (!(isalnum(*p) || *p == '-' || *p == '_' || *p == '.'))
            return false;
    }
    return true;
}

static bool auth_join(char *out, size_t out_len, const char *a,
                      const char *b, const char *c) {
    if (!out || out_len == 0 || !a || !a[0])
        return false;
    int n = c ? snprintf(out, out_len, "%s/%s/%s", a, b, c)
              : b ? snprintf(out, out_len, "%s/%s", a, b)
                  : snprintf(out, out_len, "%s", a);
    return n > 0 && (size_t)n < out_len;
}

static bool auth_profile_root(auth_cli_kind_t kind, char *out, size_t out_len) {
    const char *override = getenv(kind == AUTH_CLI_GROK
                                      ? "DSCO_GROK_PROFILES_ROOT"
                                      : "DSCO_KIMI_PROFILES_ROOT");
    if (override && override[0])
        return auth_join(out, out_len, override, NULL, NULL);
    const char *home = getenv("HOME");
    return home && home[0] &&
           auth_join(out, out_len, home, ".dsco/auth",
                     kind == AUTH_CLI_GROK ? "grok" : "kimi");
}

static bool auth_cli_home(auth_cli_kind_t kind, const char *profile,
                          char *out, size_t out_len) {
    if (profile && profile[0]) {
        if (!dsco_auth_profile_name_valid(profile))
            return false;
        char root[AUTH_LANE_PATH_MAX];
        return auth_profile_root(kind, root, sizeof(root)) &&
               auth_join(out, out_len, root, profile, NULL);
    }

    const char *configured = getenv(kind == AUTH_CLI_GROK
                                         ? "DSCO_GROK_HOME"
                                         : "DSCO_KIMI_HOME");
    if (!configured || !configured[0])
        configured = getenv(kind == AUTH_CLI_GROK ? "GROK_HOME" : "KIMI_CODE_HOME");
    if (configured && configured[0])
        return auth_join(out, out_len, configured, NULL, NULL);
    const char *home = getenv("HOME");
    return home && home[0] &&
           auth_join(out, out_len, home,
                     kind == AUTH_CLI_GROK ? ".grok" : ".kimi-code", NULL);
}

bool dsco_auth_grok_home(const char *profile, char *out, size_t out_len) {
    return auth_cli_home(AUTH_CLI_GROK, profile, out, out_len);
}

bool dsco_auth_kimi_home(const char *profile, char *out, size_t out_len) {
    return auth_cli_home(AUTH_CLI_KIMI, profile, out, out_len);
}

static bool auth_regular_nonempty(const char *path) {
    struct stat st;
    return path && stat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0;
}

static bool auth_file_contains(const char *path, const char *needle) {
    if (!path || !needle)
        return false;
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    char buf[65537];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    return strstr(buf, needle) != NULL;
}

static bool auth_mkdir_private(const char *path) {
    if (!path || !path[0])
        return false;
    char copy[AUTH_LANE_PATH_MAX];
    int n = snprintf(copy, sizeof(copy), "%s", path);
    if (n <= 0 || (size_t)n >= sizeof(copy))
        return false;
    for (char *p = copy + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = '\0';
        if (mkdir(copy, 0700) != 0 && errno != EEXIST)
            return false;
        *p = '/';
    }
    return mkdir(copy, 0700) == 0 || errno == EEXIST;
}

static bool auth_cli_ready(auth_cli_kind_t kind, const char *profile) {
    char home[AUTH_LANE_PATH_MAX], state[AUTH_LANE_PATH_MAX];
    if (!auth_cli_home(kind, profile, home, sizeof(home)))
        return false;
    if (!auth_join(state, sizeof(state), home,
                   kind == AUTH_CLI_GROK ? "auth.json" : "config.toml", NULL))
        return false;
    if (!auth_regular_nonempty(state))
        return false;
    /* A Kimi config can also describe metered Moonshot/API-key providers.
     * Only the managed Kimi Code OAuth section proves this is a membership
     * lane; file existence alone would misclassify a PAYG-only profile. */
    if (kind == AUTH_CLI_KIMI)
        return auth_file_contains(state, "[providers.\"managed:kimi-code\".oauth]");
    return true;
}

bool dsco_auth_grok_profile_ready(const char *profile) {
    return auth_cli_ready(AUTH_CLI_GROK, profile);
}

bool dsco_auth_kimi_profile_ready(const char *profile) {
    return auth_cli_ready(AUTH_CLI_KIMI, profile);
}

static bool auth_apply_cli_profile(auth_cli_kind_t kind, const char *profile) {
    char home[AUTH_LANE_PATH_MAX];
    if (!auth_cli_home(kind, profile, home, sizeof(home)))
        return false;
    if (kind == AUTH_CLI_GROK) {
        setenv("GROK_HOME", home, 1);
        /* The official CLI gives API keys precedence over OAuth.  Subscription
         * lanes are strict: inherited metered credentials must not shadow the
         * selected account profile. */
        unsetenv("XAI_API_KEY");
        unsetenv("GROK_API_KEY");
        unsetenv("X_AI_API_KEY");
    } else {
        setenv("KIMI_CODE_HOME", home, 1);
        unsetenv("KIMI_API_KEY");
        unsetenv("MOONSHOT_API_KEY");
    }
    return true;
}

bool dsco_auth_apply_grok_profile(const char *profile) {
    return auth_apply_cli_profile(AUTH_CLI_GROK, profile);
}

bool dsco_auth_apply_kimi_profile(const char *profile) {
    return auth_apply_cli_profile(AUTH_CLI_KIMI, profile);
}

static bool auth_name_seen(const auth_profile_names_t *names, const char *name) {
    for (size_t i = 0; names && i < names->count; i++) {
        if (strcmp(names->names[i], name) == 0)
            return true;
    }
    return false;
}

static void auth_name_add(auth_profile_names_t *names, const char *name) {
    if (!names || !dsco_auth_profile_name_valid(name) ||
        names->count >= AUTH_LANE_DISCOVER_MAX || auth_name_seen(names, name))
        return;
    snprintf(names->names[names->count++], AUTH_LANE_PROFILE_MAX, "%s", name);
}

static void auth_discover_profiles(auth_cli_kind_t kind, auth_profile_names_t *names) {
    char root[AUTH_LANE_PATH_MAX];
    if (!auth_profile_root(kind, root, sizeof(root)))
        return;
    DIR *dir = opendir(root);
    if (!dir)
        return;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && names->count < AUTH_LANE_DISCOVER_MAX) {
        if (!dsco_auth_profile_name_valid(ent->d_name))
            continue;
        char path[AUTH_LANE_PATH_MAX];
        struct stat st;
        if (!auth_join(path, sizeof(path), root, ent->d_name, NULL) ||
            stat(path, &st) != 0 || !S_ISDIR(st.st_mode))
            continue;
        auth_name_add(names, ent->d_name);
    }
    closedir(dir);
}

static void auth_lane_append(jbuf_t *json, bool *first, const char *id,
                             const char *provider, const char *product,
                             const char *profile, const char *transport,
                             const char *billing, const char *auth_mode,
                             const char *endpoint, const char *model,
                             bool ready, const char *reason) {
    if (!*first)
        jbuf_append(json, ",");
    *first = false;
    jbuf_append(json, "{\"id\":");
    jbuf_append_json_str(json, id);
    jbuf_append(json, ",\"provider\":");
    jbuf_append_json_str(json, provider);
    jbuf_append(json, ",\"product\":");
    jbuf_append_json_str(json, product);
    jbuf_append(json, ",\"principal_profile\":");
    jbuf_append_json_str(json, profile);
    jbuf_append(json, ",\"transport\":");
    jbuf_append_json_str(json, transport);
    jbuf_append(json, ",\"billing\":");
    jbuf_append_json_str(json, billing);
    jbuf_append(json, ",\"auth_mode\":");
    jbuf_append_json_str(json, auth_mode);
    jbuf_append(json, ",\"endpoint\":");
    jbuf_append_json_str(json, endpoint);
    jbuf_append(json, ",\"model\":");
    jbuf_append_json_str(json, model);
    jbuf_append(json, ",\"ready\":");
    jbuf_append(json, ready ? "true" : "false");
    jbuf_append(json, ",\"reason\":");
    jbuf_append_json_str(json, reason);
    jbuf_append(json, "}");
}

static void auth_append_subscription_lanes(jbuf_t *json, bool *first,
                                           int *ready_count, int *total_count) {
    for (size_t i = 0; i < dsco_subscription_lane_count(); i++) {
        const dsco_subscription_lane_spec_t *lane = dsco_subscription_lane_at(i);
        char mode[64], endpoint[512], reason[96], id[192];
        bool ready = dsco_subscription_lane_native_ready(
            lane, NULL, mode, sizeof(mode), endpoint, sizeof(endpoint), reason, sizeof(reason));
        snprintf(id, sizeof(id), "%s:subscription:native:default", lane->provider);
        auth_lane_append(json, first, id, lane->provider, lane->label, "default",
                         "native_http", "included", mode, endpoint, lane->model,
                         ready, reason);
        (*total_count)++;
        if (ready) (*ready_count)++;
    }
}

static void auth_append_external_profiles(jbuf_t *json, bool *first,
                                          auth_cli_kind_t kind,
                                          int *ready_count, int *total_count) {
    auth_profile_names_t names = {0};
    auth_discover_profiles(kind, &names);
    /* The default profile remains visible even when no named profiles exist. */
    for (size_t i = 0; i <= names.count; i++) {
        const char *profile = i == 0 ? "default" : names.names[i - 1];
        const char *selector = i == 0 ? "" : names.names[i - 1];
        bool ready = auth_cli_ready(kind, selector);
        char id[256];
        snprintf(id, sizeof(id), "%s:subscription:cli:%s",
                 kind == AUTH_CLI_GROK ? "xai-grok" : "kimi-code", profile);
        auth_lane_append(
            json, first, id,
            kind == AUTH_CLI_GROK ? "xai-grok" : "kimi-code",
            kind == AUTH_CLI_GROK ? "Grok account subscription" : "Kimi Code membership",
            profile, "official_cli", "included",
            kind == AUTH_CLI_GROK ? "grok-cli-oauth" : "kimi-device-oauth",
            kind == AUTH_CLI_GROK ? "https://cli-chat-proxy.grok.com"
                                  : "https://api.kimi.com/coding/v1",
            kind == AUTH_CLI_GROK ? "grok-4.6" : KIMI_CODE_DEFAULT_MODEL,
            ready, ready ? "ready" : "profile_not_logged_in");
        (*total_count)++;
        if (ready) (*ready_count)++;
    }
}

static bool auth_profile_is_special(const char *name) {
    static const char *special[] = {
        "anthropic", "openai-codex", "kimi-code", "sakana", "zai", NULL,
    };
    for (int i = 0; special[i]; i++) {
        if (strcmp(name, special[i]) == 0)
            return true;
    }
    return false;
}

static void auth_append_metered_profiles(jbuf_t *json, bool *first,
                                         int *ready_count, int *total_count) {
    for (size_t i = 0; i < provider_profile_count(); i++) {
        const provider_profile_t *profile = provider_profile_at(i);
        if (!profile || auth_profile_is_special(profile->name) ||
            provider_is_local_endpoint(profile->name))
            continue;
        const char *credential = provider_resolve_api_key(profile->name);
        const char *mode = provider_auth_mode(profile->name, credential);
        bool included = credential && provider_usage_is_included(profile->name, credential);
        bool ready = credential && credential[0] &&
                     provider_profile_transport_supported(profile);
        char id[192];
        snprintf(id, sizeof(id), "%s:%s:default", profile->name,
                 included ? "subscription" : "api");
        auth_lane_append(json, first, id, profile->name,
                         profile->display_name ? profile->display_name : profile->name,
                         "default", provider_transport_kind_name(profile->transport),
                         included ? "included" : "metered", mode,
                         profile->transport_base_url ? profile->transport_base_url : "",
                         profile->default_model ? profile->default_model : "",
                         ready, !credential ? "credential_missing"
                                            : !provider_profile_transport_supported(profile)
                                                  ? "transport_unimplemented"
                                                  : "ready");
        (*total_count)++;
        if (ready) (*ready_count)++;
    }

    /* Sakana is intentionally two independently selectable billing paths.
     * The generic resolver prefers the subscription, so PAYG must be listed
     * explicitly or it becomes invisible whenever both keys exist. */
    const char *payg = provider_sakana_payg_request_key();
    auth_lane_append(json, first, "sakana:api:payg", "sakana", "Sakana PAYG",
                     "default", "openai_chat", "metered", "sakana-payg-api-key",
                     "https://api.sakana.ai/v1", "fugu-ultra", payg && payg[0],
                     payg && payg[0] ? "ready" : "credential_missing");
    (*total_count)++;
    if (payg && payg[0]) (*ready_count)++;
}

static void auth_cli_usage(FILE *out, const char *argv0) {
    fprintf(out,
            "Usage:\n"
            "  %s auth lanes\n"
            "  %s auth login grok[@PROFILE]\n"
            "  %s auth login kimi[@PROFILE]\n"
            "\n"
            "Named profiles isolate paid account principals. Run them with\n"
            "`%s -e grok@PROFILE ...` or `%s -e kimi@PROFILE ...`.\n",
            argv0, argv0, argv0, argv0, argv0);
}

static int auth_login_profile(const char *target) {
    const char *at = strchr(target, '@');
    size_t product_len = at ? (size_t)(at - target) : strlen(target);
    const char *profile = at ? at + 1 : "";
    if ((at && !dsco_auth_profile_name_valid(profile)) || product_len == 0) {
        fprintf(stderr, "error: invalid auth profile selector '%s'\n", target);
        return 2;
    }

    if (product_len == 4 && strncmp(target, "grok", 4) == 0) {
        if (!dsco_auth_apply_grok_profile(profile)) {
            fprintf(stderr, "error: could not resolve Grok profile '%s'\n", profile);
            return 1;
        }
        if (profile[0] && !auth_mkdir_private(getenv("GROK_HOME"))) {
            fprintf(stderr, "error: could not create isolated Grok profile home\n");
            return 1;
        }
        execlp("grok", "grok", "login", "--device-auth", (char *)NULL);
        perror("grok");
        return 127;
    }
    if (product_len == 4 && strncmp(target, "kimi", 4) == 0) {
        if (!dsco_auth_apply_kimi_profile(profile)) {
            fprintf(stderr, "error: could not resolve Kimi profile '%s'\n", profile);
            return 1;
        }
        if (profile[0] && !auth_mkdir_private(getenv("KIMI_CODE_HOME"))) {
            fprintf(stderr, "error: could not create isolated Kimi profile home\n");
            return 1;
        }
        const char *home = getenv("HOME");
        char bundled[AUTH_LANE_PATH_MAX];
        if (home && auth_join(bundled, sizeof(bundled), home,
                              ".kimi-code/bin", "kimi") && access(bundled, X_OK) == 0)
            execl(bundled, "kimi", "login", (char *)NULL);
        execlp("kimi", "kimi", "login", (char *)NULL);
        perror("kimi");
        return 127;
    }
    fprintf(stderr, "error: unsupported auth login target '%s'\n", target);
    return 2;
}

int dsco_auth_lanes_cli(int argc, char **argv) {
    const char *action = argc >= 3 ? argv[2] : "lanes";
    if (strcmp(action, "-h") == 0 || strcmp(action, "--help") == 0 ||
        strcmp(action, "help") == 0) {
        auth_cli_usage(stdout, argv[0]);
        return 0;
    }
    if (strcmp(action, "login") == 0) {
        if (argc < 4) {
            auth_cli_usage(stderr, argv[0]);
            return 2;
        }
        return auth_login_profile(argv[3]);
    }
    if (strcmp(action, "lanes") != 0 && strcmp(action, "status") != 0) {
        auth_cli_usage(stderr, argv[0]);
        return 2;
    }

    jbuf_t json;
    jbuf_init(&json, 16384);
    jbuf_append(&json, "{\"schema\":\"dsco.auth_lanes.v1\","
                       "\"status_basis\":\"local_auth_only\",\"dimensions\":["
                       "\"provider\",\"product\",\"principal_profile\",\"transport\","
                       "\"billing\",\"auth_mode\",\"model\"],\"lanes\":[");
    bool first = true;
    int ready_count = 0, total_count = 0;
    auth_append_subscription_lanes(&json, &first, &ready_count, &total_count);
    auth_append_external_profiles(&json, &first, AUTH_CLI_GROK,
                                  &ready_count, &total_count);
    auth_append_external_profiles(&json, &first, AUTH_CLI_KIMI,
                                  &ready_count, &total_count);
    auth_append_metered_profiles(&json, &first, &ready_count, &total_count);
    jbuf_appendf(&json, "],\"ready_count\":%d,\"total_count\":%d}",
                 ready_count, total_count);
    fprintf(stdout, "%s\n", json.data ? json.data : "{}");
    jbuf_free(&json);
    return 0;
}
