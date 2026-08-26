#include "cloud_runtime.h"
#include "activation_lease.h"
#include "generated/runtime_spec_contract.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

/* macOS SDKs built with a strict POSIX feature level omit mkdtemp's BSD
 * declaration even though the libc symbol is available. */
#if defined(__APPLE__)
extern char *mkdtemp(char *);
#endif

static bool s_active;
static char s_lease_id[DSCO_ACTIVATION_LEASE_ID_MAX];

#ifndef DSCO_RUNTIME_SPEC_SHA256
#define DSCO_RUNTIME_SPEC_SHA256 ""
#endif

#ifndef DSCO_CLOUD_ALLOWED_HOSTS
#define DSCO_CLOUD_ALLOWED_HOSTS ""
#endif
#ifndef DSCO_CLOUD_ALLOWED_PROVIDERS
#define DSCO_CLOUD_ALLOWED_PROVIDERS ""
#endif
#ifndef DSCO_CLOUD_ALLOWED_MODELS
#define DSCO_CLOUD_ALLOWED_MODELS ""
#endif
#ifndef DSCO_CLOUD_ALLOWED_TOOLS
#define DSCO_CLOUD_ALLOWED_TOOLS ""
#endif
#ifndef DSCO_CLOUD_TOOL_ALLOWLIST
#define DSCO_CLOUD_TOOL_ALLOWLIST ""
#endif
#ifndef DSCO_CLOUD_PRIMARY_PROVIDER
#define DSCO_CLOUD_PRIMARY_PROVIDER ""
#endif
#ifndef DSCO_CLOUD_PRIMARY_MODEL
#define DSCO_CLOUD_PRIMARY_MODEL ""
#endif
#ifndef DSCO_CLOUD_DISABLE_CROSS_PROVIDER_ROUTING
#define DSCO_CLOUD_DISABLE_CROSS_PROVIDER_ROUTING ""
#endif
#ifndef DSCO_CLOUD_SESSION_BUDGET_USD
#define DSCO_CLOUD_SESSION_BUDGET_USD ""
#endif
#ifndef DSCO_CLOUD_PLUGIN_CAPABILITIES
#define DSCO_CLOUD_PLUGIN_CAPABILITIES ""
#endif

static bool truthy(const char *v) {
    return v && v[0] && (!strcmp(v, "1") || !strcasecmp(v, "true") ||
                         !strcasecmp(v, "yes") || !strcasecmp(v, "on"));
}

bool dsco_cloud_runtime_requested(int argc, char **argv) {
    if (truthy(getenv("DSCO_CLOUD_RUNTIME")) || truthy(getenv("DSCO_BYOK_CLOUD")) ||
        (getenv("DSCO_RUNTIME_PROFILE") && !strcasecmp(getenv("DSCO_RUNTIME_PROFILE"), "cloud")))
        return true;
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--cloud-runtime") || !strcmp(argv[i], "--byok-cloud"))
            return true;
    return false;
}

bool dsco_cloud_runtime_active(void) { return s_active; }
const char *dsco_cloud_lease_id(void) { return s_lease_id[0] ? s_lease_id : NULL; }

static bool compiled_cloud_tool_policies_are_supported(void) {
    /* The current RuntimeSpec schema has no tenant-owned command sandbox,
     * filesystem-root, or plugin-capability contract. Do not let a manually
     * assembled build advertise a shell/write/dynamic tool it will correctly
     * deny at dispatch time. */
    const char *p = DSCO_CLOUD_ALLOWED_TOOLS;
    bool saw_policy = false;
    while (*p) {
        while (*p == ',' || isspace((unsigned char)*p)) p++;
        const char *start = p;
        while (*p && *p != ',') p++;
        const char *end = p;
        while (end > start && isspace((unsigned char)end[-1])) end--;
        size_t len = (size_t)(end - start);
        if (len == 0) continue;
        bool known =
            (len == strlen(DSCO_RUNTIME_SPEC_TOOL_REPOSITORY) &&
             !strncmp(start, DSCO_RUNTIME_SPEC_TOOL_REPOSITORY, len)) ||
            (len == strlen(DSCO_RUNTIME_SPEC_TOOL_WEB) &&
             !strncmp(start, DSCO_RUNTIME_SPEC_TOOL_WEB, len));
        if (!known) return false;
        saw_policy = true;
    }
    return saw_policy;
}

bool dsco_cloud_runtime_apply_compiled_ceilings(char *err, size_t err_len) {
    if (!DSCO_CLOUD_ALLOWED_HOSTS[0] || !DSCO_CLOUD_ALLOWED_PROVIDERS[0] ||
        !DSCO_CLOUD_ALLOWED_MODELS[0] || !DSCO_CLOUD_ALLOWED_TOOLS[0] ||
        !DSCO_CLOUD_TOOL_ALLOWLIST[0] || !DSCO_CLOUD_PRIMARY_PROVIDER[0] ||
        !DSCO_CLOUD_PRIMARY_MODEL[0] || !DSCO_CLOUD_DISABLE_CROSS_PROVIDER_ROUTING[0] ||
        !DSCO_CLOUD_SESSION_BUDGET_USD[0]) {
        if (err && err_len) snprintf(err, err_len, "cloud RuntimeSpec ceilings were not compiled in");
        return false;
    }
    if (!compiled_cloud_tool_policies_are_supported()) {
        if (err && err_len) snprintf(err, err_len,
                                     "compiled cloud tool policy requires a tenant sandbox contract");
        return false;
    }
    /* All environment values here are build-time constants derived from the
     * signed RuntimeSpec. Overwrite, rather than default, so an operator
     * cannot widen provider/model/tool/network policy at process start. */
    setenv("DSCO_CLOUD_ALLOWED_HOSTS", DSCO_CLOUD_ALLOWED_HOSTS, 1);
    setenv("DSCO_CLOUD_ALLOWED_PROVIDERS", DSCO_CLOUD_ALLOWED_PROVIDERS, 1);
    setenv("DSCO_CLOUD_ALLOWED_MODELS", DSCO_CLOUD_ALLOWED_MODELS, 1);
    setenv("DSCO_CLOUD_ALLOWED_TOOLS", DSCO_CLOUD_ALLOWED_TOOLS, 1);
    setenv("DSCO_ALLOW_NET", DSCO_CLOUD_ALLOWED_HOSTS, 1);
    setenv("DSCO_OR_PROVIDER_ONLY", DSCO_CLOUD_ALLOWED_PROVIDERS, 1);
    setenv("DSCO_OR_FALLBACK_MODELS", DSCO_CLOUD_ALLOWED_MODELS, 1);
    setenv("DSCO_TOOL_ALLOWLIST", DSCO_CLOUD_TOOL_ALLOWLIST, 1);
    setenv("DSCO_MODEL", DSCO_CLOUD_PRIMARY_MODEL, 1);
    setenv("DSCO_EXEC", DSCO_CLOUD_PRIMARY_PROVIDER, 1);
    setenv("DSCO_DISABLE_CROSS_PROVIDER_ROUTING", DSCO_CLOUD_DISABLE_CROSS_PROVIDER_ROUTING, 1);
    setenv("DSCO_DEFAULT_SESSION_BUDGET", DSCO_CLOUD_SESSION_BUDGET_USD, 1);
    setenv("DSCO_EXEC_BUDGET_CEILING", DSCO_CLOUD_SESSION_BUDGET_USD, 1);
    /* Cloud bundles never discover a host user's plugins or reuse browser and
     * IPC state. A future RuntimeSpec plugin capability field can compile a
     * non-empty allowlist; current bundles intentionally expose none. */
    setenv("DSCO_CLOUD_PLUGIN_CAPABILITIES", DSCO_CLOUD_PLUGIN_CAPABILITIES, 1);
    setenv("DSCO_CLOUD_SKIP_PLUGIN_INIT", "1", 1);
    setenv("DSCO_CLOUD_SKIP_BROWSER_PROFILES", "1", 1);
    setenv("DSCO_CLOUD_SKIP_IPC_INIT", "1", 1);
    return true;
}

static bool configure_isolated_cloud_state(char *err, size_t err_len) {
    /* Do this only after verification: a cloud runtime must never use the
     * host user's ~/.dsco state. mkdtemp creates an owner-only state root and
     * redirects legacy HOME-derived persistence before Chronicle starts. */
    char state_template[] = "/tmp/dsco-cloud.XXXXXX";
    char *state_root = mkdtemp(state_template);
    if (!state_root) {
        if (err && err_len) snprintf(err, err_len, "could not create isolated cloud state");
        return false;
    }
    if (chmod(state_root, S_IRWXU) != 0 || setenv("HOME", state_root, 1) != 0 ||
        setenv("DSCO_CLOUD_STATE_DIR", state_root, 1) != 0) {
        if (err && err_len) snprintf(err, err_len, "could not isolate cloud runtime home");
        return false;
    }
    char chronicle_dir[512], runs_dir[512], ipc_db[512], browser_db[512];
    int n = snprintf(chronicle_dir, sizeof(chronicle_dir), "%s/chronicle", state_root);
    if (n < 0 || (size_t)n >= sizeof(chronicle_dir)) return false;
    n = snprintf(runs_dir, sizeof(runs_dir), "%s/runs", state_root);
    if (n < 0 || (size_t)n >= sizeof(runs_dir)) return false;
    n = snprintf(ipc_db, sizeof(ipc_db), "%s/ipc.sqlite", state_root);
    if (n < 0 || (size_t)n >= sizeof(ipc_db)) return false;
    n = snprintf(browser_db, sizeof(browser_db), "%s/browser-hosts.tsv", state_root);
    if (n < 0 || (size_t)n >= sizeof(browser_db)) return false;
    if (setenv("DSCO_CHRONICLE_DIR", chronicle_dir, 1) != 0 ||
        setenv("DSCO_RUNS_DIR", runs_dir, 1) != 0 ||
        setenv("DSCO_IPC_DB", ipc_db, 1) != 0 ||
        setenv("DSCO_BROWSER_HOST_DB", browser_db, 1) != 0) {
        if (err && err_len) snprintf(err, err_len, "could not configure isolated cloud state");
        return false;
    }
    return true;
}

static bool token_eq(const char *start, size_t n, const char *value) {
    size_t m = strlen(value);
    return n == m && !strncasecmp(start, value, n);
}

bool dsco_cloud_destination_allowed(const char *url_or_host) {
    if (!s_active) return true;
    const char *allow = getenv("DSCO_CLOUD_ALLOWED_HOSTS");
    if (!allow || !allow[0] || !url_or_host || !url_or_host[0]) return false;
    const char *host = url_or_host;
    const char *scheme = strstr(host, "://");
    if (scheme) host = scheme + 3;
    if (*host == '[') { /* IPv6 literals are intentionally unsupported here. */
        return false;
    }
    char clean[256]; size_t n = 0;
    while (host[n] && host[n] != '/' && host[n] != ':' && host[n] != '?' && host[n] != '#' &&
           !isspace((unsigned char)host[n]) && n + 1 < sizeof(clean)) n++;
    if (!n || n + 1 >= sizeof(clean)) return false;
    memcpy(clean, host, n); clean[n] = '\0';
    for (const char *p = allow; *p;) {
        while (*p == ',' || isspace((unsigned char)*p)) p++;
        const char *end = p;
        while (*end && *end != ',') end++;
        while (end > p && isspace((unsigned char)end[-1])) end--;
        if (token_eq(p, (size_t)(end - p), clean)) return true;
        p = *end ? end + 1 : end;
    }
    return false;
}

bool dsco_cloud_runtime_init(int argc, char **argv, char *err, size_t err_len) {
    if (!dsco_cloud_runtime_requested(argc, argv)) return true;
    s_active = true;
    /* These are ceilings, deliberately overwritten after any caller env. */
    setenv("DSCO_CLOUD_RUNTIME", "1", 1);
    setenv("DSCO_SETUP_NO_AUTO_BOOTSTRAP", "1", 1);
    setenv("DSCO_CLOUD_SKIP_SAVED_ENV", "1", 1);
    setenv("DSCO_CREDENTIAL_DISCOVERY_NO_PROMPT", "1", 1);
    setenv("DSCO_SECURE_STORE_NO_PROMPT", "1", 1);
    setenv("DSCO_DISABLE_SHARED_HOME_OAUTH", "1", 1);
    setenv("DSCO_ALLOW_WRITE", "0", 1);
    setenv("DSCO_ALLOW_RUN", "0", 1);
    setenv("DSCO_ALLOW_SECRETS", "0", 1);
    setenv("DSCO_ALLOW_CONTROL", "0", 1);
    setenv("DSCO_GOV_BYPASS", "0", 1);
    setenv("DSCO_GOV_MODEL", "standard", 1);
    setenv("DSCO_TRUST_TIER", "untrusted", 1);
    setenv("DSCO_APPROVAL_MODE", "strict", 1);
    setenv("DSCO_APPROVAL_NEVER", "0", 1);

    const char *lease_path = getenv("DSCO_ACTIVATION_LEASE_PATH");
    if (!lease_path || lease_path[0] != '/') {
        if (err && err_len) snprintf(err, err_len, "absolute DSCO_ACTIVATION_LEASE_PATH is required");
        return false;
    }
    activation_lease_t lease;
    char lease_err[256] = {0};
    activation_lease_status_t st = activation_lease_load_file_verified(lease_path, &lease,
                                                                         lease_err, sizeof(lease_err));
    if (st != ACTIVATION_LEASE_OK) {
        if (err && err_len) snprintf(err, err_len, "activation lease rejected: %s", lease_err);
        return false;
    }
    if (!DSCO_RUNTIME_SPEC_SHA256[0] ||
        strcmp(lease.runtime_spec_sha256, DSCO_RUNTIME_SPEC_SHA256) != 0) {
        if (err && err_len) snprintf(err, err_len, "activation lease RuntimeSpec binding rejected");
        return false;
    }
    if (!dsco_cloud_runtime_apply_compiled_ceilings(err, err_len))
        return false;
    snprintf(s_lease_id, sizeof(s_lease_id), "%s", lease.lease_id);
    if (!lease.scopes[0] || !strstr(lease.scopes, "cloud")) {
        if (err && err_len) snprintf(err, err_len, "activation lease lacks cloud scope");
        return false;
    }
    /* The issuer-signed lease is the cloud EntityCapsule admission binding:
     * subject owns the organization, lease_id identifies this deployment, and
     * the exact RuntimeSpec digest is the enforced policy revision. Overwrite
     * operator-provided values so runtime identity cannot be widened at boot. */
    setenv("DSCO_ORGANIZATION_ID", lease.subject, 1);
    setenv("DSCO_DEPLOYMENT_ID", lease.lease_id, 1);
    setenv("DSCO_POLICY_SHA256", DSCO_RUNTIME_SPEC_SHA256, 1);
    setenv("DSCO_AGENT_ID", lease.lease_id, 1);
    setenv("DSCO_ROLE_ID", "runtime-root", 1);
    setenv("GRAPHSUB_TENANT_ID", lease.subject, 1);
    return configure_isolated_cloud_state(err, err_len);
}
