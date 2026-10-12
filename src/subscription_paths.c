#include "subscription_paths.h"

#include "../vendor/yyjson.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define SUB_PATH_FILE_MAX (2u * 1024u * 1024u)
#define SUB_PATH_ERROR_MAX 256u
#define SUB_PATH_TEXT_MAX 2048u

static bool text_valid(const char *value, size_t maximum, bool allow_empty) {
    if (!value || (!allow_empty && !value[0]) || strlen(value) > maximum)
        return false;
    for (const unsigned char *p = (const unsigned char *)value; *p; p++)
        if (*p < 0x20 || *p == 0x7f)
            return false;
    return true;
}

static bool id_valid(const char *value) {
    if (!text_valid(value, 160, false))
        return false;
    for (const unsigned char *p = (const unsigned char *)value; *p; p++)
        if (!(isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == ':'))
            return false;
    return true;
}

static void add_error(dsco_subscription_path_registry_t *registry,
                      const char *fmt, ...) {
    if (!registry || registry->error_count >= SUB_PATH_ERROR_MAX)
        return;
    char message[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(message, sizeof(message), fmt, ap);
    va_end(ap);
    char **grown = realloc(registry->errors,
                           (registry->error_count + 1) * sizeof(*registry->errors));
    if (!grown)
        return;
    registry->errors = grown;
    registry->errors[registry->error_count] = strdup(message);
    if (registry->errors[registry->error_count])
        registry->error_count++;
}

static char *dup_value(yyjson_val *object, const char *key, const char *fallback,
                       bool required) {
    yyjson_val *value = yyjson_obj_get(object, key);
    if (!value)
        return required ? NULL : strdup(fallback ? fallback : "");
    if (!yyjson_is_str(value) || yyjson_get_len(value) != strlen(yyjson_get_str(value)) ||
        !text_valid(yyjson_get_str(value), SUB_PATH_TEXT_MAX, !required))
        return NULL;
    return strdup(yyjson_get_str(value));
}

static void path_free(dsco_subscription_path_t *path) {
    if (!path)
        return;
    free(path->id); free(path->lane_id); free(path->provider); free(path->product);
    free(path->principal_profile); free(path->transport); free(path->billing);
    free(path->auth_mode); free(path->model); free(path->endpoint);
    free(path->executable); free(path->authenticated); free(path->source);
    if (path->argv) {
        for (size_t i = 0; i < path->argc; i++) free(path->argv[i]);
        free(path->argv);
    }
    memset(path, 0, sizeof(*path));
}

void dsco_subscription_paths_free(dsco_subscription_path_registry_t *registry) {
    if (!registry)
        return;
    for (size_t i = 0; i < registry->count; i++) path_free(&registry->paths[i]);
    for (size_t i = 0; i < registry->error_count; i++) free(registry->errors[i]);
    free(registry->paths); free(registry->errors);
    memset(registry, 0, sizeof(*registry));
}

static bool enum_is(const char *value, const char *const *allowed) {
    for (size_t i = 0; allowed[i]; i++)
        if (strcmp(value, allowed[i]) == 0)
            return true;
    return false;
}

static bool parse_argv(dsco_subscription_path_t *path, yyjson_val *value,
                       const char *source, size_t index,
                       dsco_subscription_path_registry_t *registry) {
    if (!yyjson_is_arr(value) || yyjson_arr_size(value) >= DSCO_SUBSCRIPTION_PATH_ARGV_MAX) {
        add_error(registry, "%s: paths[%zu].argv must be an array with fewer than %d entries",
                  source, index, DSCO_SUBSCRIPTION_PATH_ARGV_MAX);
        return false;
    }
    path->argc = yyjson_arr_size(value);
    path->argv = calloc(path->argc ? path->argc : 1, sizeof(*path->argv));
    if (!path->argv)
        return false;
    size_t prompt_count = 0;
    size_t i, count;
    yyjson_val *arg;
    yyjson_arr_foreach(value, i, count, arg) {
        const char *text = yyjson_get_str(arg);
        if (!yyjson_is_str(arg) || yyjson_get_len(arg) != (text ? strlen(text) : 0) ||
            !text_valid(text, SUB_PATH_TEXT_MAX, false)) {
            add_error(registry, "%s: paths[%zu].argv[%zu] is invalid", source, index, i);
            return false;
        }
        if (strchr(text, '{') && strcmp(text, "{prompt}") != 0 &&
            strcmp(text, "{model}") != 0 && strcmp(text, "{cwd}") != 0) {
            add_error(registry, "%s: paths[%zu].argv[%zu] has an unsupported placeholder",
                      source, index, i);
            return false;
        }
        prompt_count += strcmp(text, "{prompt}") == 0;
        path->argv[i] = strdup(text);
        if (!path->argv[i])
            return false;
    }
    if ((path->prompt_on_stdin && prompt_count != 0) ||
        (!path->prompt_on_stdin && prompt_count != 1)) {
        add_error(registry, "%s: paths[%zu] prompt transport and {prompt} count disagree",
                  source, index);
        return false;
    }
    return true;
}

static bool parse_entry(dsco_subscription_path_registry_t *registry,
                        yyjson_val *entry, const char *source, size_t index) {
    if (!yyjson_is_obj(entry) || registry->count >= DSCO_SUBSCRIPTION_PATH_MAX) {
        add_error(registry, "%s: paths[%zu] is not an object or registry limit reached",
                  source, index);
        return false;
    }
    dsco_subscription_path_t path = {0};
    path.id = dup_value(entry, "id", NULL, true);
    path.lane_id = dup_value(entry, "lane_id", "", false);
    path.provider = dup_value(entry, "provider", NULL, true);
    path.product = dup_value(entry, "product", NULL, true);
    path.principal_profile = dup_value(entry, "principal_profile", "default", false);
    path.transport = dup_value(entry, "transport", "external_process", false);
    path.billing = dup_value(entry, "billing", "included", false);
    path.auth_mode = dup_value(entry, "auth_mode", "external-cli-session", false);
    path.model = dup_value(entry, "model", NULL, true);
    path.endpoint = dup_value(entry, "endpoint", "", false);
    path.executable = dup_value(entry, "executable", NULL, true);
    path.authenticated = dup_value(entry, "authenticated", "unknown", false);
    path.source = strdup(source);
    yyjson_val *prompt_transport = yyjson_obj_get(entry, "prompt_transport");
    const char *prompt_mode = yyjson_get_str(prompt_transport);
    path.prompt_on_stdin = prompt_mode && strcmp(prompt_mode, "stdin") == 0;
    path.enabled = yyjson_get_bool(yyjson_obj_get(entry, "enabled"));
    path.qualified = yyjson_get_bool(yyjson_obj_get(entry, "qualified"));
    path.requested_automatic_eligibility =
        yyjson_get_bool(yyjson_obj_get(entry, "automatic_eligible"));

    static const char *const transports[] = {
        "external_process", "acp_stdio", "mcp_stdio", "http_bridge", NULL,
    };
    static const char *const billing[] = {"included", "prepaid_credits", "metered", "unknown", NULL};
    static const char *const auth[] = {"unknown", "ready", "missing", NULL};
    bool strings = path.id && path.lane_id && path.provider && path.product &&
                   path.principal_profile && path.transport && path.billing && path.auth_mode &&
                   path.model && path.endpoint && path.executable && path.authenticated && path.source;
    if (!strings || !id_valid(path.id) || !id_valid(path.provider) ||
        !id_valid(path.principal_profile) || !id_valid(path.auth_mode) ||
        !enum_is(path.transport, transports) || !enum_is(path.billing, billing) ||
        !enum_is(path.authenticated, auth) || !prompt_mode ||
        (strcmp(prompt_mode, "argv") != 0 && strcmp(prompt_mode, "stdin") != 0)) {
        add_error(registry, "%s: paths[%zu] has invalid identity or enum fields", source, index);
        path_free(&path);
        return false;
    }
    if (!path.lane_id[0]) {
        size_t n = strlen(path.id) + strlen(path.principal_profile) + 20;
        char *generated = malloc(n);
        if (!generated) { path_free(&path); return false; }
        snprintf(generated, n, "subscription:%s:%s", path.id,
                 path.principal_profile);
        free(path.lane_id);
        path.lane_id = generated;
    }
    if (!id_valid(path.lane_id)) {
        add_error(registry, "%s: paths[%zu] has an invalid lane_id", source, index);
        path_free(&path);
        return false;
    }
    if (!parse_argv(&path, yyjson_obj_get(entry, "argv"), source, index, registry)) {
        path_free(&path);
        return false;
    }
    if (registry->count == registry->capacity) {
        size_t capacity = registry->capacity ? registry->capacity * 2 : 64;
        if (capacity > DSCO_SUBSCRIPTION_PATH_MAX)
            capacity = DSCO_SUBSCRIPTION_PATH_MAX;
        dsco_subscription_path_t *grown = realloc(
            registry->paths, capacity * sizeof(*registry->paths));
        if (!grown) { path_free(&path); return false; }
        registry->paths = grown;
        registry->capacity = capacity;
    }
    registry->paths[registry->count++] = path;
    return true;
}

typedef struct {
    const char *value;
    size_t path_index;
} subscription_identity_t;

static int identity_compare(const void *a, const void *b) {
    const subscription_identity_t *left = a, *right = b;
    int compared = strcmp(left->value, right->value);
    if (compared)
        return compared;
    return left->path_index < right->path_index ? -1 : left->path_index > right->path_index;
}

static void validate_unique_identities(dsco_subscription_path_registry_t *registry) {
    if (!registry || registry->count < 2)
        return;
    if (registry->count > SIZE_MAX / 2 ||
        registry->count * 2 > SIZE_MAX / sizeof(subscription_identity_t)) {
        add_error(registry, "subscription path identity index overflow");
        return;
    }
    size_t count = registry->count * 2;
    subscription_identity_t *identities = malloc(count * sizeof(*identities));
    if (!identities) {
        add_error(registry, "subscription path identity index allocation failed");
        return;
    }
    for (size_t i = 0; i < registry->count; i++) {
        identities[i * 2] = (subscription_identity_t){registry->paths[i].id, i};
        identities[i * 2 + 1] = (subscription_identity_t){registry->paths[i].lane_id, i};
    }
    qsort(identities, count, sizeof(*identities), identity_compare);
    for (size_t start = 0; start < count;) {
        size_t end = start + 1;
        bool conflicts = false;
        while (end < count && strcmp(identities[start].value, identities[end].value) == 0) {
            conflicts |= identities[start].path_index != identities[end].path_index;
            end++;
        }
        if (conflicts)
            add_error(registry, "duplicate subscription path identity: %s",
                      identities[start].value);
        start = end;
    }
    free(identities);
}

static char *read_file(const char *path, size_t *length) {
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0 ||
        (unsigned long long)st.st_size > SUB_PATH_FILE_MAX)
        return NULL;
    FILE *file = fopen(path, "rb");
    if (!file)
        return NULL;
    size_t n = (size_t)st.st_size;
    char *data = malloc(n + 1);
    if (!data) { fclose(file); return NULL; }
    size_t got = fread(data, 1, n, file);
    bool ok = got == n && !ferror(file);
    fclose(file);
    if (!ok) { free(data); return NULL; }
    data[n] = '\0';
    if (length) *length = n;
    return data;
}

static void load_file(dsco_subscription_path_registry_t *registry, const char *path) {
    size_t length = 0;
    char *data = read_file(path, &length);
    if (!data) {
        add_error(registry, "%s: cannot read manifest", path);
        return;
    }
    yyjson_doc *doc = yyjson_read(data, length, 0);
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    yyjson_val *schema = yyjson_obj_get(root, "schema");
    yyjson_val *paths = yyjson_obj_get(root, "paths");
    if (!yyjson_is_obj(root) || !yyjson_equals_str(schema, "dsco.subscription-paths.v1") ||
        !yyjson_is_arr(paths)) {
        add_error(registry, "%s: expected dsco.subscription-paths.v1 with paths array", path);
    } else {
        size_t i, count;
        yyjson_val *entry;
        yyjson_arr_foreach(paths, i, count, entry)
            (void)parse_entry(registry, entry, path, i);
    }
    yyjson_doc_free(doc);
    free(data);
}

static int string_compare(const void *a, const void *b) {
    const char *const *left = a, *const *right = b;
    return strcmp(*left, *right);
}

static void load_directory(dsco_subscription_path_registry_t *registry,
                           const char *directory) {
    DIR *dir = opendir(directory);
    if (!dir)
        return;
    char **names = NULL;
    size_t count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && count < SUB_PATH_ERROR_MAX) {
        size_t n = strlen(entry->d_name);
        if (n < 6 || strcmp(entry->d_name + n - 5, ".json") != 0)
            continue;
        char **grown = realloc(names, (count + 1) * sizeof(*names));
        if (!grown) break;
        names = grown;
        names[count] = strdup(entry->d_name);
        if (names[count]) count++;
    }
    closedir(dir);
    qsort(names, count, sizeof(*names), string_compare);
    for (size_t i = 0; i < count; i++) {
        char path[PATH_MAX];
        int n = snprintf(path, sizeof(path), "%s/%s", directory, names[i]);
        if (n > 0 && (size_t)n < sizeof(path)) load_file(registry, path);
        free(names[i]);
    }
    free(names);
}

static void load_location(dsco_subscription_path_registry_t *registry,
                          const char *location) {
    struct stat st;
    if (!location || !location[0] || stat(location, &st) != 0)
        return;
    if (S_ISDIR(st.st_mode)) load_directory(registry, location);
    else if (S_ISREG(st.st_mode)) load_file(registry, location);
}

bool dsco_subscription_paths_load(dsco_subscription_path_registry_t *registry) {
    if (!registry)
        return false;
    memset(registry, 0, sizeof(*registry));
    const char *home = getenv("HOME");
    if (home && home[0]) {
        char file[PATH_MAX], directory[PATH_MAX];
        if (snprintf(file, sizeof(file), "%s/.config/dsco/subscription-paths.json", home) > 0)
            load_location(registry, file);
        if (snprintf(directory, sizeof(directory), "%s/.config/dsco/subscription-paths.d", home) > 0)
            load_location(registry, directory);
    }
    const char *configured = getenv("DSCO_SUBSCRIPTION_PATHS");
    char *copy = configured && configured[0] ? strdup(configured) : NULL;
    char *state = NULL;
    for (char *part = copy ? strtok_r(copy, ":", &state) : NULL;
         part; part = strtok_r(NULL, ":", &state))
        if (part[0]) load_location(registry, part);
    free(copy);
    validate_unique_identities(registry);
    return registry->error_count == 0;
}

const dsco_subscription_path_t *dsco_subscription_path_find(
    const dsco_subscription_path_registry_t *registry, const char *selector) {
    if (!registry || !selector)
        return NULL;
    for (size_t i = 0; i < registry->count; i++)
        if (strcmp(registry->paths[i].id, selector) == 0 ||
            strcmp(registry->paths[i].lane_id, selector) == 0)
            return &registry->paths[i];
    return NULL;
}

static bool executable_at(const char *directory, size_t directory_len,
                          const char *binary, char *out, size_t out_len) {
    if (!directory_len || directory_len > PATH_MAX - 2)
        return false;
    char path[PATH_MAX];
    int n = snprintf(path, sizeof(path), "%.*s/%s", (int)directory_len, directory, binary);
    if (n <= 0 || (size_t)n >= sizeof(path) || (size_t)n >= out_len || access(path, X_OK) != 0)
        return false;
    snprintf(out, out_len, "%s", path);
    return true;
}

bool dsco_subscription_path_resolve_executable(const dsco_subscription_path_t *path,
                                               char *out, size_t out_len) {
    if (!path || !path->executable || !path->executable[0] || !out || out_len == 0)
        return false;
    out[0] = '\0';
    if (strchr(path->executable, '/')) {
        if (path->executable[0] != '/' || access(path->executable, X_OK) != 0 ||
            strlen(path->executable) >= out_len)
            return false;
        snprintf(out, out_len, "%s", path->executable);
        return true;
    }
    const char *search = getenv("PATH");
    for (const char *p = search ? search : ""; *p;) {
        const char *colon = strchr(p, ':');
        size_t n = colon ? (size_t)(colon - p) : strlen(p);
        if (executable_at(p, n, path->executable, out, out_len))
            return true;
        if (!colon) break;
        p = colon + 1;
    }
    return false;
}

bool dsco_subscription_path_ready(const dsco_subscription_path_t *path,
                                  char *reason, size_t reason_len) {
    char binary[PATH_MAX];
    const char *why = "ready";
    bool ready = true;
    if (!path || !path->enabled) { ready = false; why = "disabled"; }
    else if (!dsco_subscription_path_resolve_executable(path, binary, sizeof(binary))) {
        ready = false; why = "executable_missing";
    } else if (strcmp(path->authenticated, "ready") != 0) {
        ready = false; why = strcmp(path->authenticated, "missing") == 0
                                 ? "authentication_missing" : "authentication_unverified";
    }
    if (reason && reason_len) snprintf(reason, reason_len, "%s", why);
    return ready;
}

bool dsco_subscription_path_automatic_eligible(const dsco_subscription_path_t *path) {
    return path && path->requested_automatic_eligibility && path->qualified &&
           dsco_subscription_path_ready(path, NULL, 0);
}

bool dsco_subscription_path_build_argv(const dsco_subscription_path_t *path,
                                       const char *resolved_binary,
                                       const char *prompt, const char *model,
                                       const char *cwd, char **argv,
                                       size_t argv_cap, size_t *argc_out,
                                       char *error, size_t error_len) {
    if (argc_out) *argc_out = 0;
    if (!path || !resolved_binary || !prompt || !argv || argv_cap < path->argc + 2) {
        if (error && error_len) snprintf(error, error_len, "invalid adapter invocation");
        return false;
    }
    memset(argv, 0, argv_cap * sizeof(*argv));
    size_t count = 0;
    argv[count++] = strdup(resolved_binary);
    for (size_t i = 0; argv[0] && i < path->argc; i++) {
        const char *value = path->argv[i];
        if (strcmp(value, "{prompt}") == 0) value = prompt;
        else if (strcmp(value, "{model}") == 0) value = model && model[0] ? model : path->model;
        else if (strcmp(value, "{cwd}") == 0) value = cwd ? cwd : "";
        argv[count] = strdup(value);
        if (!argv[count++]) break;
    }
    if (count != path->argc + 1 || !argv[0]) {
        dsco_subscription_path_argv_free(argv, count);
        if (error && error_len) snprintf(error, error_len, "argument allocation failed");
        return false;
    }
    argv[count] = NULL;
    if (argc_out) *argc_out = count;
    return true;
}

void dsco_subscription_path_argv_free(char **argv, size_t argc) {
    if (!argv) return;
    for (size_t i = 0; i < argc; i++) { free(argv[i]); argv[i] = NULL; }
}
