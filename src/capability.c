#include "capability.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ── name-set helpers ─────────────────────────────────────────────────────── */

static bool name_in(const char *name, const char *const *set) {
    for (int i = 0; set[i]; i++)
        if (strcmp(name, set[i]) == 0)
            return true;
    return false;
}

static bool contains_ci(const char *hay, const char *needle) {
    if (!hay || !needle)
        return false;
    size_t nl = strlen(needle);
    for (const char *p = hay; *p; p++)
        if (strncasecmp(p, needle, nl) == 0)
            return true;
    return false;
}

/* ── tool → capability classification ─────────────────────────────────────── */

/* Pure read-only local inspection: only ever CAP_FS_READ. */
static const char *const k_read_tools[] = {
    "read_file", "grep_files", "grep", "glob", "list_directory", "list_dir", "symbol_def",
    "symbol_refs", "diagnostics", "hover", "definition", "references", "cat", "head", "tail",
    "stat", "diff", NULL};

/* Local file mutation. Implies read too. */
static const char *const k_write_tools[] = {
    "write_file", "edit_file", "apply_patch", "ast_edit", "create_file", "delete_file",
    "move_file", "rename_file", "patch", NULL};

/* Subprocess / shell execution. Input is inspected for net/secret escalation. */
static const char *const k_exec_tools[] = {
    "bash", "run_command", "sandbox_run", "run_background", "compile", "pkg", "pip", "npm",
    "docker", "docker_compose", "kill_process", "crontab", "make", NULL};

/* External-content ingress with network egress (both untrusted-in and net). */
static const char *const k_net_tools[] = {
    "web_search", "read_url", "fetch", "fetch_url", "http", "http_request", "browser",
    "web_fetch", "curl", "download", NULL};

/* Direct network egress that carries local data outward. */
static const char *const k_egress_tools[] = {"ssh_command", "scp", "rsync", "send_email",
                                             "upload", "post", NULL};

/* Control-plane / self-modification: gating the gate. */
static const char *const k_control_tools[] = {
    "governance", "killswitch", "self_exit", "gate_status", "gov_experiment", "tamper",
    NULL};

/* Tools whose input can name credentials/secret material. */
static bool input_touches_secrets(const char *input) {
    static const char *const marks[] = {".env",        "id_rsa",     ".ssh/",   "credentials",
                                        ".aws",        "keychain",   "secret",  "api_key",
                                        "apikey",      "token",      "password", "private_key",
                                        ".netrc",      "id_ed25519", NULL};
    for (int i = 0; marks[i]; i++)
        if (contains_ci(input, marks[i]))
            return true;
    return false;
}

/* Shell command carries external network egress. */
static bool shell_has_network(const char *cmd) {
    static const char *const net[] = {"curl", "wget", "nc ",   "ncat",  "ssh ",   "scp ",
                                     "rsync", "telnet", "git push", "git clone", "git fetch",
                                     "git pull", "http://", "https://", "ftp://", NULL};
    for (int i = 0; net[i]; i++)
        if (contains_ci(cmd, net[i]))
            return true;
    return false;
}

/* Shell command writes/mutates the filesystem. */
static bool shell_has_write(const char *cmd) {
    static const char *const w[] = {" > ",  " >>", "tee ", "rm ",    "rm -", "mv ",
                                   "cp ",  "mkdir", "touch ", "dd ", "chmod", "chown",
                                   "truncate", NULL};
    for (int i = 0; w[i]; i++)
        if (contains_ci(cmd, w[i]))
            return true;
    return false;
}

unsigned dsco_caps_for_tool(const char *name, const char *input_json) {
    if (!name || !name[0])
        return CAP_EGRESS | CAP_UNTRUSTED_IN; /* unknown: worst case */

    unsigned caps = CAP_NONE;

    if (name_in(name, k_read_tools))
        caps |= CAP_FS_READ;
    if (name_in(name, k_write_tools))
        caps |= CAP_FS_READ | CAP_FS_WRITE;
    if (name_in(name, k_control_tools))
        caps |= CAP_CONTROL;

    if (name_in(name, k_net_tools))
        caps |= CAP_NET | CAP_UNTRUSTED_IN; /* remote content is untrusted */
    if (name_in(name, k_egress_tools))
        caps |= CAP_NET;

    /* MCP / external tools (prefixed) reach out to third-party systems: both an
     * egress and an untrusted-content ingress. */
    if (strncmp(name, "mcp_", 4) == 0 || strncmp(name, "mcp__", 5) == 0)
        caps |= CAP_NET | CAP_UNTRUSTED_IN;

    if (name_in(name, k_exec_tools)) {
        caps |= CAP_EXEC;
        if (input_json) {
            if (shell_has_network(input_json))
                caps |= CAP_NET | CAP_UNTRUSTED_IN;
            if (shell_has_write(input_json))
                caps |= CAP_FS_WRITE;
            caps |= CAP_FS_READ; /* a shell can read anything */
        } else {
            caps |= CAP_NET | CAP_FS_WRITE | CAP_FS_READ; /* worst case */
        }
    }

    if (input_json && input_touches_secrets(input_json))
        caps |= CAP_SECRETS;

    /* An editing tool aimed at the gate's own source is a control operation. */
    if ((caps & CAP_FS_WRITE) && input_json) {
        static const char *const gate_src[] = {"capability.c", "capability.h", "governance.c",
                                              "governance.h", "killswitch.c", "tools.c",
                                              "audit_log.c",  "tamper.c",     NULL};
        for (int i = 0; gate_src[i]; i++) {
            if (contains_ci(input_json, gate_src[i])) {
                caps |= CAP_CONTROL;
                break;
            }
        }
    }

    if (caps == CAP_NONE)
        caps = CAP_FS_READ; /* default: treat unknown builtin as a benign read */
    return caps;
}

/* ── Deno-style grants ────────────────────────────────────────────────────── */

static int env_tristate(const char *key) {
    const char *v = getenv(key);
    if (!v || !v[0])
        return -1; /* unset */
    if (strcmp(v, "0") == 0 || strcasecmp(v, "false") == 0 || strcasecmp(v, "off") == 0 ||
        strcasecmp(v, "deny") == 0)
        return 0;
    return 1;
}

bool dsco_cap_granted(dsco_cap_t cap, const char *tier) {
    if (!tier || !tier[0])
        tier = "standard";

    /* Explicit env overrides map to Deno's flag names. */
    const char *env = NULL;
    switch (cap) {
        case CAP_FS_READ:  env = "DSCO_ALLOW_READ";    break;
        case CAP_FS_WRITE: env = "DSCO_ALLOW_WRITE";   break;
        case CAP_NET:      env = "DSCO_ALLOW_NET";     break;
        case CAP_EXEC:     env = "DSCO_ALLOW_RUN";     break;
        case CAP_SECRETS:  env = "DSCO_ALLOW_SECRETS"; break;
        case CAP_CONTROL:  env = "DSCO_ALLOW_CONTROL"; break;
        default: break;
    }
    if (env) {
        int t = env_tristate(env);
        if (t >= 0)
            return t == 1;
    }

    bool untrusted = strcasecmp(tier, "untrusted") == 0;
    bool trusted = strcasecmp(tier, "trusted") == 0;

    switch (cap) {
        case CAP_FS_READ:
            return true; /* reads are always allowed */
        case CAP_FS_WRITE:
        case CAP_EXEC:
        case CAP_NET:
            return !untrusted; /* standard + trusted */
        case CAP_SECRETS:
            return trusted; /* only the trusted tier by default */
        case CAP_CONTROL:
            return false; /* never by default; requires DSCO_ALLOW_CONTROL=1 */
        default:
            return false;
    }
}

/* ── per-session taint flow ───────────────────────────────────────────────── */

static atomic_int g_ingested_untrusted = 0;
static atomic_int g_accessed_private = 0;

void dsco_flow_reset(void) {
    atomic_store(&g_ingested_untrusted, 0);
    atomic_store(&g_accessed_private, 0);
}

void dsco_flow_note(unsigned caps) {
    if (caps & CAP_UNTRUSTED_IN)
        atomic_store(&g_ingested_untrusted, 1);
    if (caps & CAP_PRIVATE_IN)
        atomic_store(&g_accessed_private, 1);
}

bool dsco_flow_tainted_untrusted(void) {
    return atomic_load(&g_ingested_untrusted) != 0;
}

bool dsco_flow_accessed_private(void) {
    return atomic_load(&g_accessed_private) != 0;
}

bool dsco_flow_would_exfiltrate(unsigned caps) {
    if (!(caps & CAP_EGRESS))
        return false;
    return dsco_flow_tainted_untrusted() && dsco_flow_accessed_private();
}

/* ── top-level gate ───────────────────────────────────────────────────────── */

dsco_cap_decision_t dsco_capability_gate(const char *name, const char *input_json,
                                         const char *tier, char *reason, size_t reason_len) {
    unsigned caps = dsco_caps_for_tool(name, input_json);
    if (!tier || !tier[0])
        tier = "standard";

#define CAP_REASON(...)                                                                            \
    do {                                                                                           \
        if (reason && reason_len)                                                                  \
            snprintf(reason, reason_len, __VA_ARGS__);                                             \
    } while (0)

    /* 1. Control capability: self-modification of the gate/governance. */
    if ((caps & CAP_CONTROL) && !dsco_cap_granted(CAP_CONTROL, tier)) {
        CAP_REASON("control capability denied: '%s' would modify the gate/governance "
                   "(grant DSCO_ALLOW_CONTROL=1 to authorize)",
                   name);
        return CAP_DECISION_DENY;
    }

    /* 2. Lethal trifecta: this call egresses and the session has already
     *    ingested untrusted content AND accessed private data. Fail closed —
     *    this is the exfiltration edge. The operator can acknowledge the risk
     *    for a run with DSCO_ALLOW_EXFIL=1. */
    if (dsco_flow_would_exfiltrate(caps)) {
        if (env_tristate("DSCO_ALLOW_EXFIL") == 1)
            return CAP_DECISION_ALLOW;
        CAP_REASON("lethal-trifecta block: '%s' would egress after untrusted-content "
                   "ingestion + private-data access; set DSCO_ALLOW_EXFIL=1 to override",
                   name);
        return CAP_DECISION_DENY;
    }

    /* 3. Per-capability grants (reads always pass; untrusted-in is benign alone). */
    static const dsco_cap_t checked[] = {CAP_FS_WRITE, CAP_NET, CAP_EXEC, CAP_SECRETS};
    for (size_t i = 0; i < sizeof(checked) / sizeof(checked[0]); i++) {
        if ((caps & checked[i]) && !dsco_cap_granted(checked[i], tier)) {
            char capstr[64];
            dsco_capability_to_string(checked[i], capstr, sizeof(capstr));
            CAP_REASON("capability '%s' not granted for tool '%s' in tier '%s'", capstr, name,
                       tier);
            return CAP_DECISION_DENY;
        }
    }

#undef CAP_REASON
    return CAP_DECISION_ALLOW;
}

void dsco_capability_to_string(unsigned caps, char *out, size_t out_len) {
    if (!out || out_len == 0)
        return;
    out[0] = '\0';
    struct {
        dsco_cap_t cap;
        const char *name;
    } names[] = {
        {CAP_FS_READ, "fs_read"},   {CAP_FS_WRITE, "fs_write"},
        {CAP_NET, "net"},           {CAP_EXEC, "exec"},
        {CAP_SECRETS, "secrets"},   {CAP_UNTRUSTED_IN, "untrusted_in"},
        {CAP_CONTROL, "control"},
    };
    size_t off = 0;
    bool first = true;
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (!(caps & names[i].cap))
            continue;
        int n = snprintf(out + off, out_len - off, "%s%s", first ? "" : "|", names[i].name);
        if (n < 0 || (size_t)n >= out_len - off)
            break;
        off += (size_t)n;
        first = false;
    }
    if (first)
        snprintf(out, out_len, "none");
}
