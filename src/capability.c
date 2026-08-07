#include "capability.h"
#include "cap_model.h"
#include "cloud_runtime.h"
#include "toolmgmt.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <unistd.h>
#include <time.h>
#include "json_util.h"

/* From tools.c (forward-declared to avoid the heavy tools.h include chain):
 * a builtin tool's declared read-only flag; *found means it is registered. */
bool tools_meta_is_read_only(const char *name, bool *found);

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

/* forward decl: positional case-insensitive find (defined later, near egress scoping) */
static const char *ci_find(const char *hay, const char *needle);

/* NFKD-style homoglyph fold: decode UTF-8, map fullwidth ASCII + accented Latin to
 * their ASCII base and drop combining diacritics, so `.énv` / fullwidth `.ｅｎｖ` /
 * `ｉｄ_ｒｓａ` normalize to the ASCII secret markers before matching. Mirrors the
 * Python gate's _fold() (unicodedata NFKD + combining-mark strip). Caller frees. */
static char *cap_fold(const char *in) {
    if (!in)
        return NULL;
    size_t n = strlen(in);
    char *out = malloc(n * 2 + 1); /* mapped output never exceeds 2x the input */
    if (!out)
        return NULL;
    /* Latin-1 Supplement letters 0xC0..0xFF -> ASCII base (0 = leave untouched, so
     * ligatures/eszett/thorn that do not NFKD-decompose pass through like Python). */
    static const unsigned char L1[64] = {
        'A', 'A', 'A', 'A', 'A', 'A', 0,   'C', 'E', 'E', 'E', 'E', 'I', 'I', 'I', 'I',
        0,   'N', 'O', 'O', 'O', 'O', 'O', 0,   0,   'U', 'U', 'U', 'U', 'Y', 0,   0,
        'a', 'a', 'a', 'a', 'a', 'a', 0,   'c', 'e', 'e', 'e', 'e', 'i', 'i', 'i', 'i',
        0,   'n', 'o', 'o', 'o', 'o', 'o', 0,   0,   'u', 'u', 'u', 'u', 'y', 0,   'y'};
    size_t o = 0;
    const unsigned char *p = (const unsigned char *)in;
    while (*p) {
        unsigned char c = *p;
        unsigned cp;
        int len;
        if (c < 0x80) {
            cp = c;
            len = 1;
        } else if ((c & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
            cp = ((c & 0x1Fu) << 6) | (p[1] & 0x3Fu);
            len = 2;
        } else if ((c & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
            cp = ((c & 0x0Fu) << 12) | ((p[1] & 0x3Fu) << 6) | (p[2] & 0x3Fu);
            len = 3;
        } else if ((c & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 &&
                   (p[3] & 0xC0) == 0x80) {
            cp = ((c & 0x07u) << 18) | ((p[1] & 0x3Fu) << 12) | ((p[2] & 0x3Fu) << 6) |
                 (p[3] & 0x3Fu);
            len = 4;
        } else {
            out[o++] = (char)c; /* malformed lead byte: copy verbatim */
            p++;
            continue;
        }
        p += len;
        if (cp >= 0x0300 && cp <= 0x036F)
            continue; /* combining diacritical marks: drop */
        unsigned m = cp;
        if (cp >= 0xFF01 && cp <= 0xFF5E)
            m = cp - 0xFEE0; /* fullwidth ASCII forms */
        else if (cp == 0x3000)
            m = 0x20; /* ideographic space */
        else if (cp >= 0x00C0 && cp <= 0x00FF && L1[cp - 0x00C0])
            m = L1[cp - 0x00C0];
        if (m < 0x80) {
            out[o++] = (char)m;
        } else if (m < 0x800) {
            out[o++] = (char)(0xC0 | (m >> 6));
            out[o++] = (char)(0x80 | (m & 0x3F));
        } else if (m < 0x10000) {
            out[o++] = (char)(0xE0 | (m >> 12));
            out[o++] = (char)(0x80 | ((m >> 6) & 0x3F));
            out[o++] = (char)(0x80 | (m & 0x3F));
        } else {
            out[o++] = (char)(0xF0 | (m >> 18));
            out[o++] = (char)(0x80 | ((m >> 12) & 0x3F));
            out[o++] = (char)(0x80 | ((m >> 6) & 0x3F));
            out[o++] = (char)(0x80 | (m & 0x3F));
        }
    }
    out[o] = '\0';
    return out;
}

/* ── tool → capability classification ─────────────────────────────────────── */

/* Pure read-only local inspection: only ever CAP_FS_READ. */
static const char *const k_read_tools[] = {
    "read_file", "grep_files", "grep", "glob", "list_directory", "list_dir", "symbol_def",
    "symbol_refs", "diagnostics", "hover", "definition", "references", "cat", "head", "tail",
    "stat", "diff",
    /* Claude-compatible surfaces */
    "Read", "Grep", "Glob", NULL};

/* Local file mutation. Implies read too. */
static const char *const k_write_tools[] = {
    "write_file", "edit_file", "apply_patch", "ast_edit", "create_file", "delete_file",
    "move_file", "rename_file", "patch", "openai_image_generate",
    /* Claude-compatible surfaces */
    "Write", "Edit", NULL};

/* Subprocess / shell execution. Input is inspected for net/secret escalation. */
static const char *const k_exec_tools[] = {
    "bash", "run_command", "sandbox_run", "run_background", "compile", "pkg", "pip", "npm",
    "docker", "docker_compose", "kill_process", "crontab", "make",
    /* Registered spawn/interpreter surfaces (audit 2026-07-12): each starts a
     * subprocess or interprets arbitrary code, so each is an exec egress leg. */
    "python", "dsco-python-3x", "spawn_bg", "swarm", "signal_process", "test_run", "watch_run", "preprocess",
    "hermes_agent", "agent", "Task", "Agent", "KillShell", "kitty_remote", "kitten",
    /* Claude-compatible surface (input inspected for net/write escalation) */
    "Bash", NULL};

/* External-content ingress with network egress (both untrusted-in and net). */
static const char *const k_net_tools[] = {
    "web_search", "read_url", "fetch", "fetch_url", "http", "http_request", "browser",
    "web_fetch", "curl", "download", "openai_image_generate",
    /* Registered network surfaces (audit 2026-07-12): previously fell to the
     * read-only/fs_write catch-all and evaded the trifecta flow guard. */
    "tavily_search", "jina_ai_search", "jina_ai_research", "jina_search", "github_search",
    "parallel_search", "curl_raw", "download_file", "net_probe", "network", "net",
    "graphsub", "openrouter_models", "alpha_vantage", "polymarket", "agentic_commerce",
    "research_probe", "research_compare",
    /* Claude-compatible surfaces */
    "WebFetch", "WebSearch", NULL};

/* Direct network egress that carries local data outward. */
static const char *const k_egress_tools[] = {"ssh_command", "scp", "rsync", "send_email",
                                             "upload", "post", "slack_post", NULL};

/* Sub-agent / clean-room spawn surfaces. Spawning a child is the DESIGNED trifecta
 * mitigation, not an exfiltration leg: the child runs under its own fresh taint
 * state and must not be handed secrets. The gate exempts these from the exfil edge
 * unless the task payload itself names secret material. */
static const char *const k_spawn_tools[] = {
    "agent", "Agent", "Task", "spawn_bg", "swarm", "hermes_agent", NULL};

/* Real integration tools that SEND local data outward (Tool Management API:
 * distributed-publish / notify / webhook / syndicate). Pure egress — the exfil
 * leg — even when registered under bare names rather than an mcp_/tm_ prefix. */
static const char *const k_integration_egress[] = {
    "send_notification", "publish_event", "trigger_webhook", "syndicate_content",
    "queue_message", "create_webhook", "delete_webhook", "substack_publish",
    "substack_post_note", "substack_create_draft", "substack_update_draft",
    "substack_append_to_draft", "send_email", NULL};

/* Real integration tools that INGEST remote/untrusted content (distributed-memory
 * / research / search): a network egress AND an untrusted-content ingress. */
static const char *const k_integration_ingress[] = {
    "semantic_search", "semantic_search_beliefs", "search_all", "recall_episodes",
    "query_beliefs", "find_related_beliefs", "expand_context", "get_queue_messages",
    "list_events", "list_webhooks", "substack_get_posts", "substack_get_drafts", NULL};

/* Control-plane / self-modification: gating the gate. */
static const char *const k_control_tools[] = {
    "governance", "killswitch", "self_exit", "gate_status", "gov_experiment", "tamper",
    "context_control",
    NULL};

/* Tools that consume credentials even when the input JSON does not name them. */
static const char *const k_secret_tools[] = {
    "openai_image_generate", "kitty_remote",
    NULL};

/* Tools whose input can name credentials/secret material. */
static bool input_touches_secrets(const char *input) {
    static const char *const marks[] = {".env",        "id_rsa",     ".ssh/",   "credentials",
                                        ".aws",        "keychain",   "secret",  "api_key",
                                        "apikey",      "token",      "password", "private_key",
                                        ".netrc",      "id_ed25519",
                                        /* DSCO / dotfile secret stores that ".env" misses:
                                         * ~/.dsco/env holds the API-key map; ".config" and
                                         * "/env" catch env files under nested config dirs. */
                                        "dsco/env",    ".dsco/env",  ".config/dsco", "/.env",
                                        ".pem",        ".key",       "vault",   NULL};
    if (!input)
        return false;
    /* Fold homoglyphs/fullwidth/diacritics first so `.énv`, `.ｅｎｖ`, `ｉｄ_ｒｓａ`
     * normalize onto the ASCII markers before matching (mirrors Python _fold). */
    char *folded = cap_fold(input);
    const char *hay = folded ? folded : input;
    bool hit = false;
    for (int i = 0; marks[i]; i++)
        if (contains_ci(hay, marks[i])) {
            hit = true;
            break;
        }
    free(folded);
    return hit;
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

/* Word-boundary case-insensitive token match (\bword\b, word chars = [A-Za-z0-9_]). */
static bool word_present(const char *hay, const char *word) {
    if (!hay || !word)
        return false;
    size_t wl = strlen(word);
    for (const char *p = hay; *p; p++) {
        if (strncasecmp(p, word, wl) != 0)
            continue;
        char before = (p == hay) ? '\0' : p[-1];
        char after = p[wl];
        bool lb = !(isalnum((unsigned char)before) || before == '_');
        bool rb = !(isalnum((unsigned char)after) || after == '_');
        if (lb && rb)
            return true;
    }
    return false;
}

/* Non-obvious network egress verbs the substring net list misses: DNS-tunnel
 * (nslookup/dig), raw sockets (/dev/tcp, socket()/.connect(), TCPSocket, IO::Socket),
 * nc/ncat, openssl s_client, HTTP client libs, and base64-decode obfuscation.
 * Mirrors the Python gate's _NETVERB_RE. */
static bool shell_has_netverb(const char *s) {
    if (!s)
        return false;
    static const char *const subs[] = {"/dev/tcp/",       "/dev/udp/",  "openssl s_client",
                                       "socket(",         ".connect(",  "tcpsocket",
                                       "io::socket",      "requests.get", "requests.post",
                                       "requests.put",    "requests.patch", "urllib",
                                       "httpx",           NULL};
    for (int i = 0; subs[i]; i++)
        if (contains_ci(s, subs[i]))
            return true;
    static const char *const words[] = {"nslookup", "dig", "telnet", "nc", "ncat", "netc",
                                        "netcat", NULL};
    for (int i = 0; words[i]; i++)
        if (word_present(s, words[i]))
            return true;
    if (contains_ci(s, "base64") && (contains_ci(s, "-d") || contains_ci(s, "decode")))
        return true;
    return false;
}

/* A structured tool input naming a network destination is a network op regardless of
 * tool name (closes the unknown-tool hole). Keys kept network-specific; value must
 * carry a scheme (://) or a dot. Mirrors the Python gate's _NET_KEY_RE. */
static bool input_has_netkey(const char *s) {
    if (!s)
        return false;
    static const char *const keys[] = {"url", "host", "hostname", "endpoint",
                                       "uri", "webhook_url", "webhook", NULL};
    for (int i = 0; keys[i]; i++) {
        char pat[24];
        snprintf(pat, sizeof(pat), "\"%s\"", keys[i]);
        for (const char *p = ci_find(s, pat); p; p = ci_find(p + 1, pat)) {
            const char *q = p + strlen(pat);
            while (*q == ' ' || *q == '\t')
                q++;
            if (*q != ':')
                continue;
            q++;
            while (*q == ' ' || *q == '\t')
                q++;
            if (*q != '"')
                continue;
            for (const char *v = q + 1; *v && *v != '"'; v++)
                if (*v == '.' || (v[0] == ':' && v[1] == '/' && v[2] == '/'))
                    return true;
        }
    }
    return false;
}

/* Network egress detectable from the input alone — closes the unknown-tool /
 * non-curl-verb / structured-destination blind spots the tool-name lists miss. */
static bool input_has_network(const char *s) {
    if (!s || !s[0])
        return false;
    return shell_has_network(s) || shell_has_netverb(s) || input_has_netkey(s);
}

/* Exfil intent verbs (\b post|upload|forward|exfil*|transmit|leak|publish|push|send \b).
 * Mirrors the Python gate's _EXFIL_VERB_RE. */
static bool input_has_exfil_verb(const char *s) {
    if (!s)
        return false;
    static const char *const verbs[] = {"post",   "upload",  "forward", "transmit",
                                        "leak",   "publish", "push",    "send", NULL};
    for (int i = 0; verbs[i]; i++)
        if (word_present(s, verbs[i]))
            return true;
    for (const char *p = s; *p; p++) /* exfil\w* : left boundary + "exfil" prefix */
        if (strncasecmp(p, "exfil", 5) == 0) {
            char before = (p == s) ? '\0' : p[-1];
            if (!(isalnum((unsigned char)before) || before == '_'))
                return true;
        }
    return false;
}

/* Raw credential shapes a keyword blocklist misses (AWS/JWT/Stripe/GitHub/Slack/PEM).
 * Case-sensitive, mirroring the Python gate's _CRED_RE. */
static bool looks_like_credential(const char *s) {
    if (!s)
        return false;
    for (const char *p = s; *p; p++) { /* AWS AKIA/ASIA + 12+ [0-9A-Z] */
        if (strncmp(p, "AKIA", 4) == 0 || strncmp(p, "ASIA", 4) == 0) {
            const char *q = p + 4;
            int k = 0;
            while (*q && ((*q >= '0' && *q <= '9') || (*q >= 'A' && *q <= 'Z'))) {
                k++;
                q++;
            }
            if (k >= 12)
                return true;
        }
    }
    for (const char *p = s; (p = strstr(p, "eyJ")) != NULL; p++) { /* JWT: seg.seg.seg */
        const char *q = p + 3;
        int a = 0;
        while (*q && (isalnum((unsigned char)*q) || *q == '_' || *q == '-')) {
            a++;
            q++;
        }
        if (a >= 8 && *q == '.') {
            const char *r = q + 1;
            int b = 0;
            while (*r && (isalnum((unsigned char)*r) || *r == '_' || *r == '-')) {
                b++;
                r++;
            }
            if (b >= 8 && *r == '.') {
                const char *t = r + 1;
                int cc = 0;
                while (*t && (isalnum((unsigned char)*t) || *t == '_' || *t == '-')) {
                    cc++;
                    t++;
                }
                if (cc >= 4)
                    return true;
            }
        }
    }
    static const char *const pfx[] = {"sk", "pk", "rk", NULL}; /* Stripe-style keys */
    static const char *const kind[] = {"live", "test", "proj", NULL};
    for (int i = 0; pfx[i]; i++)
        for (const char *p = s; (p = strstr(p, pfx[i])) != NULL; p++) {
            const char *q = p + 2;
            if (*q != '-' && *q != '_')
                continue;
            for (int j = 0; kind[j]; j++) {
                size_t el = strlen(kind[j]);
                if (strncmp(q + 1, kind[j], el) != 0)
                    continue;
                const char *r = q + 1 + el;
                if (*r != '-' && *r != '_')
                    continue;
                const char *t = r + 1;
                int k = 0;
                while (*t && isalnum((unsigned char)*t)) {
                    k++;
                    t++;
                }
                if (k >= 12)
                    return true;
            }
        }
    for (const char *p = s; (p = strstr(p, "gh")) != NULL; p++) { /* GitHub gh[pousr]_ */
        char c = p[2];
        if ((c == 'p' || c == 'o' || c == 'u' || c == 's' || c == 'r') && p[3] == '_') {
            const char *q = p + 4;
            int k = 0;
            while (*q && isalnum((unsigned char)*q)) {
                k++;
                q++;
            }
            if (k >= 20)
                return true;
        }
    }
    for (const char *p = s; (p = strstr(p, "xox")) != NULL; p++) { /* Slack xox[baprs]- */
        char c = p[3];
        if ((c == 'b' || c == 'a' || c == 'p' || c == 'r' || c == 's') && p[4] == '-') {
            const char *q = p + 5;
            int k = 0;
            while (*q && (isalnum((unsigned char)*q) || *q == '-')) {
                k++;
                q++;
            }
            if (k >= 10)
                return true;
        }
    }
    if (strstr(s, "-----BEGIN ") && strstr(s, "PRIVATE KEY-----"))
        return true;
    return false;
}

/* Sub-agent / clean-room spawn detection: the hardcoded set OR MCP/orchestrator
 * dispatch names (dispatch_agent, sub_agent, spawn, delegate, *_agent). Mirrors the
 * Python gate's is_spawn()/_SPAWN_RE so legit delegation is not exfil-denied. */
static bool is_spawn_name(const char *name) {
    if (!name || !name[0])
        return false;
    if (name_in(name, k_spawn_tools))
        return true;
    if (contains_ci(name, "dispatch_agent") || contains_ci(name, "spawn") ||
        contains_ci(name, "delegate") || contains_ci(name, "sub_agent") ||
        contains_ci(name, "sub-agent") || contains_ci(name, "subagent"))
        return true;
    size_t n = strlen(name); /* (?:^|[_/])agent$ */
    if (n >= 5 && strcasecmp(name + n - 5, "agent") == 0) {
        if (n == 5)
            return true;
        char c = name[n - 6];
        if (c == '_' || c == '/')
            return true;
    }
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
    if (name_in(name, k_secret_tools))
        caps |= CAP_SECRETS;

    if (name_in(name, k_net_tools))
        caps |= CAP_NET | CAP_UNTRUSTED_IN; /* remote content is untrusted */
    if (name_in(name, k_egress_tools))
        caps |= CAP_NET;
    if (name_in(name, k_integration_egress))
        caps |= CAP_NET; /* sends local data outward — the exfil leg */
    if (name_in(name, k_integration_ingress))
        caps |= CAP_NET | CAP_UNTRUSTED_IN;

    /* MCP / external tools (prefixed) reach out to third-party systems: both an
     * egress and an untrusted-content ingress. */
    if (strncmp(name, "mcp_", 4) == 0 || strncmp(name, "mcp__", 5) == 0)
        caps |= CAP_NET | CAP_UNTRUSTED_IN;

    /* ToolManagement's generated names are remote calls too; treating them as
     * local registry metadata let them bypass network and taint controls. */
    if (strncmp(name, "tm__", 4) == 0)
        caps |= CAP_NET | CAP_UNTRUSTED_IN;

    /* Hosted third-party task/research families (audit 2026-07-12): every
     * parallel_ai_* tool calls a remote API and returns remote content. */
    if (strncmp(name, "parallel_ai_", 12) == 0)
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

    /* Kitty commands arrive as JSON argv, not shell text, so tokens such as
     * {"command":"ssh"} are followed by a quote rather than the whitespace
     * expected by shell_has_network(). Classify Kitty's native network lanes
     * explicitly so DSCO_ALLOW_NET=0 and the trifecta guard remain effective. */
    if (input_json && strcmp(name, "kitten") == 0) {
        char *command = json_get_str(input_json, "command");
        if (command && (!strcmp(command, "ssh") || !strcmp(command, "transfer") ||
                        !strcmp(command, "update-self")))
            caps |= CAP_NET | CAP_UNTRUSTED_IN;
        free(command);
    }
    if (input_json && strcmp(name, "kitty_remote") == 0 &&
        (contains_ci(input_json, "\"ssh\"") || contains_ci(input_json, "\"transfer\"") ||
         contains_ci(input_json, "http://") || contains_ci(input_json, "https://")))
        caps |= CAP_NET | CAP_UNTRUSTED_IN;

    if (input_json && input_touches_secrets(input_json))
        caps |= CAP_SECRETS;

    /* Input-driven network detection: egress intent named in the input text itself
     * (netverbs / structured url|host|endpoint keys) makes this a network op even for
     * an unknown/benign-named tool. Mirrors the Python gate's input_has_network(). */
    if (input_json && input_has_network(input_json))
        caps |= CAP_NET | CAP_UNTRUSTED_IN;

    /* Registry catch-all: any REGISTERED tool the name-lists left benign but that
     * is NOT declared read-only gets a conservative fs_write floor — so the long
     * tail of mutating surfaces (Claude aliases, plugins, unlisted builtins) can
     * never slip through classified as a harmless read. */
    if (!(caps & (CAP_FS_WRITE | CAP_NET | CAP_EXEC | CAP_CONTROL))) {
        bool found = false;
        bool ro = tools_meta_is_read_only(name, &found);
        if (found && !ro)
            caps |= CAP_FS_WRITE;
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
    /* Only genuine secrets/credentials count as "private data accessed" for the
     * trifecta. A coding agent reads ordinary source files constantly; treating
     * every CAP_FS_READ as private would taint the session immediately and block
     * all egress. The exfil concern is secrets, not source. */
    if (caps & CAP_SECRETS)
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

/* ── Resource scoping (Deno --allow-read=/path, --allow-net=host) ──────────── */

/* A grant env value is a boolean toggle rather than a scope list. */
static bool cap_env_is_boolean(const char *v) {
    return strcmp(v, "0") == 0 || strcmp(v, "1") == 0 || strcasecmp(v, "true") == 0 ||
           strcasecmp(v, "false") == 0 || strcasecmp(v, "on") == 0 || strcasecmp(v, "off") == 0 ||
           strcasecmp(v, "deny") == 0 || strcasecmp(v, "allow") == 0;
}

/* Make `in` absolute and collapse '.'/'..' lexically (defeats ../ traversal;
 * does not resolve symlinks — a known residual). */
static void cap_path_abs(const char *in, char *out, size_t outlen) {
    char tmp[4096];
    if (in[0] == '/') {
        snprintf(tmp, sizeof(tmp), "%s", in);
    } else {
        char cwd[2048];
        if (!getcwd(cwd, sizeof(cwd)))
            cwd[0] = '\0';
        snprintf(tmp, sizeof(tmp), "%s/%s", cwd, in);
    }
    const char *comps[256];
    int n = 0;
    char *save = NULL;
    for (char *tok = strtok_r(tmp, "/", &save); tok; tok = strtok_r(NULL, "/", &save)) {
        if (strcmp(tok, ".") == 0)
            continue;
        if (strcmp(tok, "..") == 0) {
            if (n > 0)
                n--;
            continue;
        }
        if (n < 256)
            comps[n++] = tok;
    }
    size_t off = 0;
    out[0] = '\0';
    for (int i = 0; i < n; i++) {
        int w = snprintf(out + off, outlen - off, "/%s", comps[i]);
        if (w < 0 || (size_t)w >= outlen - off)
            break;
        off += (size_t)w;
    }
    if (off == 0)
        snprintf(out, outlen, "/");
}

static bool cap_path_in_scope(const char *path, const char *scopelist) {
    char abspath[4096];
    cap_path_abs(path, abspath, sizeof(abspath));
    char list[2048];
    snprintf(list, sizeof(list), "%s", scopelist);
    char *save = NULL;
    for (char *root = strtok_r(list, ",", &save); root; root = strtok_r(NULL, ",", &save)) {
        while (*root == ' ')
            root++;
        if (!*root)
            continue;
        char absroot[4096];
        cap_path_abs(root, absroot, sizeof(absroot));
        size_t rl = strlen(absroot);
        if (rl == 0)
            continue;
        if (strncmp(abspath, absroot, rl) == 0 && (abspath[rl] == '\0' || abspath[rl] == '/'))
            return true;
    }
    return false;
}

/* Extract a lowercased hostname from a url or bare host string (IPv6-aware). */
static bool cap_host_of(const char *val, char *out, size_t outlen) {
    if (!val || !val[0])
        return false;
    const char *p = val;
    const char *scheme = strstr(val, "://");
    if (scheme)
        p = scheme + 3;
    size_t i = 0;
    if (*p == '[') {
        p++;
        while (*p && *p != ']' && i + 1 < outlen)
            out[i++] = (char)tolower((unsigned char)*p++);
    } else {
        /* A host is [A-Za-z0-9.-]. Stop at the first non-host char (/, :, whitespace,
         * quote, shell metachar, JSON delimiter) so a URL embedded in a shell command
         * or JSON blob yields just the host — not the trailing args or closing "}. */
        while (*p && (isalnum((unsigned char)*p) || *p == '.' || *p == '-') && i + 1 < outlen)
            out[i++] = (char)tolower((unsigned char)*p++);
    }
    out[i] = '\0';
    return i > 0;
}

static bool cap_host_in_scope(const char *host, const char *scopelist) {
    char list[2048];
    snprintf(list, sizeof(list), "%s", scopelist);
    char *save = NULL;
    size_t thl = strlen(host);
    for (char *h = strtok_r(list, ",", &save); h; h = strtok_r(NULL, ",", &save)) {
        while (*h == ' ')
            h++;
        if (!*h)
            continue;
        size_t hl = strlen(h);
        if (strcasecmp(host, h) == 0)
            return true;
        if (thl > hl && strcasecmp(host + (thl - hl), h) == 0 && host[thl - hl - 1] == '.')
            return true;
    }
    return false;
}

/* ── exfiltration destination scoping ──────────────────────────────────────── */

/* Case-insensitive substring search returning a pointer (portable). */
static const char *ci_find(const char *hay, const char *needle) {
    if (!hay || !needle || !needle[0])
        return NULL;
    size_t nl = strlen(needle);
    for (const char *p = hay; *p; p++)
        if (strncasecmp(p, needle, nl) == 0)
            return p;
    return NULL;
}

/* Classify a single destination host into a granular egress kind. Loopback / LAN /
 * trusted are not exfiltration targets — you cannot exfiltrate to your own infra. */
static dsco_egress_kind_t host_kind(const char *host) {
    if (!host || !host[0])
        return DSCO_EGRESS_EXTERNAL;
    /* Normalize defensively: drop [] brackets, cut a %zone suffix, lowercase — so an
     * IPv6 literal reaches the range tests the same way whether or not the extractor
     * already unwrapped it. Mirrors Python's h.strip("[]").split("%")[0].lower(). */
    char h[256];
    size_t j = 0;
    for (const char *p = host; *p && j + 1 < sizeof(h); p++) {
        if (*p == '[' || *p == ']')
            continue;
        if (*p == '%')
            break;
        h[j++] = (char)tolower((unsigned char)*p);
    }
    h[j] = '\0';

    /* Cloud-metadata SSRF endpoints are EXTERNAL/high-risk despite the link-local range
     * (169.254/16) or ULA (fd00:ec2::254) they sit in — carve them out first. */
    if (!strcmp(h, "169.254.169.254") || !strcmp(h, "fd00:ec2::254") ||
        !strcmp(h, "metadata.google.internal") || !strcmp(h, "metadata"))
        return DSCO_EGRESS_EXTERNAL;

    if (!strcmp(h, "localhost") || !strcmp(h, "127.0.0.1") || !strcmp(h, "::1") ||
        !strcmp(h, "0:0:0:0:0:0:0:1") || !strcmp(h, "0.0.0.0"))
        return DSCO_EGRESS_LOCAL;
    if (!strncmp(h, "127.", 4))
        return DSCO_EGRESS_LOCAL; /* loopback /8 */

    if (strchr(h, ':')) { /* IPv6 */
        const char *m = strstr(h, "::ffff:");
        if (m)
            return host_kind(m + 7);          /* IPv4-mapped -> classify embedded v4 */
        if (!strncmp(h, "fe80", 4))
            return DSCO_EGRESS_LAN;            /* link-local fe80::/10 */
        if (h[0] == 'f' && (h[1] == 'c' || h[1] == 'd'))
            return DSCO_EGRESS_LAN;            /* unique-local fc00::/7 */
        /* global unicast IPv6 falls through to trusted/external */
    } else {
        if (!strncmp(h, "10.", 3))      return DSCO_EGRESS_LAN;   /* private  /8  */
        if (!strncmp(h, "192.168.", 8)) return DSCO_EGRESS_LAN;   /* private  /16 */
        if (!strncmp(h, "169.254.", 8)) return DSCO_EGRESS_LAN;   /* link-local   */
        if (!strncmp(h, "172.", 4)) {                             /* 172.16/12    */
            int o2 = atoi(h + 4);
            if (o2 >= 16 && o2 <= 31)
                return DSCO_EGRESS_LAN;
        }
        if (!strncmp(h, "100.", 4)) {                             /* CGNAT/Tailscale */
            int o2 = atoi(h + 4);
            if (o2 >= 64 && o2 <= 127)
                return DSCO_EGRESS_LAN;
        }
    }
    size_t hl = strlen(h);
    if (hl > 6 && !strcasecmp(h + hl - 6, ".local"))  return DSCO_EGRESS_LAN;  /* mDNS   */
    if (hl > 7 && !strcasecmp(h + hl - 7, ".ts.net")) return DSCO_EGRESS_LAN;  /* tailnet*/
    const char *trusted = getenv("DSCO_TRUSTED_EGRESS_HOSTS");
    if (trusted && trusted[0] && cap_host_in_scope(h, trusted))
        return DSCO_EGRESS_TRUSTED;
    return DSCO_EGRESS_EXTERNAL;
}

/* Worst-case egress kind of any destination named in a text blob (a shell command
 * or a tool's raw JSON payload). NONE if no network verb; OPAQUE if a network verb
 * is present but no destination can be parsed (fail closed on the exfil edge). */
static dsco_egress_kind_t text_egress_kind(const char *text, char *host_out, size_t hlen) {
    if (host_out && hlen)
        host_out[0] = '\0';
    if (!text || !shell_has_network(text))
        return DSCO_EGRESS_NONE;
    dsco_egress_kind_t worst = DSCO_EGRESS_NONE;
    bool parsed = false;
#define NOTE_HOST(h)                                                                                \
    do {                                                                                            \
        dsco_egress_kind_t _k = host_kind(h);                                                       \
        parsed = true;                                                                              \
        if (_k > worst) {                                                                           \
            worst = _k;                                                                             \
            if (host_out && hlen)                                                                   \
                snprintf(host_out, hlen, "%s", (h));                                                \
        }                                                                                           \
    } while (0)
    static const char *const schemes[] = {"http://", "https://", "ftp://", NULL};
    for (int s = 0; schemes[s]; s++) {
        const char *p = text;
        while ((p = ci_find(p, schemes[s])) != NULL) {
            char host[256];
            if (cap_host_of(p, host, sizeof(host)))
                NOTE_HOST(host);
            p += strlen(schemes[s]);
        }
    }
    for (const char *p = text; *p; p++) {                    /* user@host (ssh/scp/rsync) */
        if (*p != '@')
            continue;
        if (p[1] == '[')
            continue; /* user@[ipv6] handled by the bracket scan below */
        char host[256];
        size_t i = 0;
        for (const char *h = p + 1;
             *h && *h != ' ' && *h != ':' && *h != '/' && *h != '\'' && *h != '"' && i + 1 < sizeof(host);
             h++)
            host[i++] = *h;
        host[i] = '\0';
        if (i)
            NOTE_HOST(host);
    }
    for (const char *p = text; *p; p++) {                    /* bracketed [ipv6] literals */
        if (*p != '[')
            continue;
        char host[256];
        size_t i = 0;
        const char *h = p + 1;
        for (; *h && *h != ']' && i + 1 < sizeof(host); h++) {
            if (isxdigit((unsigned char)*h) || *h == ':' || *h == '.' || *h == '%')
                host[i++] = *h;
            else
                break;
        }
        host[i] = '\0';
        if (*h == ']' && i >= 2 && strchr(host, ':'))
            NOTE_HOST(host);          /* only classify genuine [ipv6], not JSON arrays */
    }
    for (const char *p = text; *p;) {                        /* bare IPv4 literals */
        if (isdigit((unsigned char)*p) && (p == text || !isdigit((unsigned char)p[-1]))) {
            const char *q = p;
            int dots = 0, digits = 0;
            while (*q && (isdigit((unsigned char)*q) || *q == '.')) {
                if (*q == '.') dots++; else digits++;
                q++;
            }
            if (dots == 3 && digits >= 4 && (size_t)(q - p) < 16) {
                char ip[16];
                size_t n = (size_t)(q - p);
                memcpy(ip, p, n);
                ip[n] = '\0';
                NOTE_HOST(ip);
            }
            p = q;
        } else {
            p++;
        }
    }
#undef NOTE_HOST
    if (!parsed)
        return DSCO_EGRESS_OPAQUE;
    return worst;
}

/* Granular egress classification for a whole tool call. */
static dsco_egress_kind_t classify_egress(const char *name, const char *input_json,
                                          unsigned caps, char *host_out, size_t hlen) {
    (void)name;
    if (host_out && hlen)
        host_out[0] = '\0';
    if (!(caps & CAP_NET))
        return DSCO_EGRESS_NONE;
    if (caps & CAP_EXEC)
        return text_egress_kind(input_json, host_out, hlen);
    /* structured network tool: prefer a declared destination */
    if (input_json) {
        char *hv = json_get_str(input_json, "url");
        if (!hv) hv = json_get_str(input_json, "host");
        if (!hv) hv = json_get_str(input_json, "hostname");
        if (!hv) hv = json_get_str(input_json, "endpoint");
        if (hv) {
            char host[256];
            dsco_egress_kind_t k = DSCO_EGRESS_OPAQUE;
            if (cap_host_of(hv, host, sizeof(host))) {
                k = host_kind(host);
                if (host_out && hlen)
                    snprintf(host_out, hlen, "%s", host);
            }
            free(hv);
            return k;
        }
        if (shell_has_network(input_json))   /* e.g. a bridge-exec 'cmd' payload */
            return text_egress_kind(input_json, host_out, hlen);
    }
    return DSCO_EGRESS_OPAQUE;
}

/* Retained wrapper for the boolean sense used by the gate. */
static bool egress_is_external(dsco_egress_kind_t k) {
    return k == DSCO_EGRESS_EXTERNAL || k == DSCO_EGRESS_OPAQUE;
}

static dsco_cap_decision_t rule_gate(const char *name, const char *input_json,
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
    if (dsco_flow_tainted_untrusted() && dsco_flow_accessed_private()) {
        bool exfil_edge = false;
        if (is_spawn_name(name)) {
            /* A clean-room sub-agent spawn is the DESIGNED mitigation, not an exfil
             * edge: the child runs under a fresh taint state. It IS an exfil edge only
             * if the task launders data out — names secret material, carries a raw
             * credential, or instructs an egress verb toward an external/opaque host. */
            exfil_edge = input_json && (input_touches_secrets(input_json) ||
                                        looks_like_credential(input_json));
            if (!exfil_edge && input_json && input_has_exfil_verb(input_json)) {
                char dh[128];
                dsco_egress_kind_t k =
                    classify_egress("__spawn__", input_json, CAP_NET, dh, sizeof(dh));
                exfil_edge = (k == DSCO_EGRESS_EXTERNAL || k == DSCO_EGRESS_OPAQUE);
            }
        } else if (caps & CAP_NET) {
            /* Genuine network egress. Loopback / RFC1918 LAN / trusted hosts are
             * not exfiltration targets — reaching your own inference box is fine. */
            char dh[128];
            exfil_edge = egress_is_external(classify_egress(name, input_json, caps, dh, sizeof(dh)));
        }
        /* Pure local exec (echo, ls, local file work) carries no external egress
         * and is never an exfil edge, even when the session is tainted. */
        if (exfil_edge) {
            if (env_tristate("DSCO_ALLOW_EXFIL") == 1)
                return CAP_DECISION_ALLOW;
            CAP_REASON("lethal-trifecta block: '%s' would exfiltrate to an EXTERNAL "
                       "destination after untrusted-content + secret access. Allowed "
                       "instead: local/LAN/trusted egress (add hosts via "
                       "DSCO_TRUSTED_EGRESS_HOSTS), a clean-room sub-agent spawn (no "
                       "secrets in the task), or DSCO_ALLOW_EXFIL=1 to override",
                       name);
            return CAP_DECISION_DENY;
        }
    }

    /* 3. Deno-style explicit hardening. Tier-based allow/deny for these
     *    capabilities — and untrusted-tier sandbox routing — remains owned by
     *    the existing tier system (tools_is_allowed_for_tier + sandbox route).
     *    Here we only enforce an EXPLICIT operator lockdown, e.g.
     *    DSCO_ALLOW_NET=0 blocks every network tool regardless of tier. */
    static const struct {
        dsco_cap_t cap;
        const char *env;
    } hardening[] = {
        {CAP_FS_WRITE, "DSCO_ALLOW_WRITE"},
        {CAP_NET, "DSCO_ALLOW_NET"},
        {CAP_EXEC, "DSCO_ALLOW_RUN"},
        {CAP_SECRETS, "DSCO_ALLOW_SECRETS"},
    };
    for (size_t i = 0; i < sizeof(hardening) / sizeof(hardening[0]); i++) {
        if ((caps & hardening[i].cap) && env_tristate(hardening[i].env) == 0) {
            char capstr[64];
            dsco_capability_to_string(hardening[i].cap, capstr, sizeof(capstr));
            CAP_REASON("capability '%s' explicitly disabled (%s=0) for tool '%s'", capstr,
                       hardening[i].env, name);
            return CAP_DECISION_DENY;
        }
    }

    /* 4. Resource scoping: when a grant env holds a path/host list (not a bool),
     * the tool's target resource must fall within the declared scope. */
    if (input_json) {
        const char *ws = getenv("DSCO_ALLOW_WRITE");
        const char *rs = getenv("DSCO_ALLOW_READ");
        const char *ns = getenv("DSCO_ALLOW_NET");
        if ((caps & CAP_FS_WRITE) && ws && ws[0] && !cap_env_is_boolean(ws)) {
            char *pth = json_get_str(input_json, "file_path");
            if (!pth)
                pth = json_get_str(input_json, "path");
            if (!pth)
                pth = json_get_str(input_json, "output_path");
            if (!pth)
                pth = json_get_str(input_json, "output");
            if (pth && pth[0] && !cap_path_in_scope(pth, ws)) {
                CAP_REASON("write path outside DSCO_ALLOW_WRITE scope: %.80s", pth);
                free(pth);
                return CAP_DECISION_DENY;
            }
            if ((!pth || !pth[0]) && (caps & CAP_EXEC)) {
                CAP_REASON("scoped write grant requires explicit structured path for exec-capable tool '%s'",
                           name);
                free(pth);
                return CAP_DECISION_DENY;
            }
            free(pth);
        } else if ((caps & CAP_FS_READ) && rs && rs[0] && !cap_env_is_boolean(rs)) {
            char *pth = json_get_str(input_json, "file_path");
            if (!pth)
                pth = json_get_str(input_json, "path");
            if (pth && pth[0] && !cap_path_in_scope(pth, rs)) {
                CAP_REASON("read path outside DSCO_ALLOW_READ scope: %.80s", pth);
                free(pth);
                return CAP_DECISION_DENY;
            }
            if ((!pth || !pth[0]) && (caps & CAP_EXEC)) {
                CAP_REASON("scoped read grant requires explicit structured path for exec-capable tool '%s'",
                           name);
                free(pth);
                return CAP_DECISION_DENY;
            }
            free(pth);
        }
        if ((caps & CAP_NET) && ns && ns[0] && !cap_env_is_boolean(ns)) {
            char *hv = json_get_str(input_json, "url");
            if (!hv)
                hv = json_get_str(input_json, "host");
            if (!hv)
                hv = json_get_str(input_json, "hostname");
            if (!hv && strcmp(name, "openai_image_generate") == 0)
                hv = safe_strdup("api.openai.com");
            char host[256];
            if (hv && cap_host_of(hv, host, sizeof(host)) && !cap_host_in_scope(host, ns)) {
                CAP_REASON("host outside DSCO_ALLOW_NET scope: %.80s", host);
                free(hv);
                return CAP_DECISION_DENY;
            }
            if ((!hv || !hv[0]) && (caps & CAP_EXEC)) {
                CAP_REASON("scoped net grant requires explicit structured host/url for exec-capable tool '%s'",
                           name);
                free(hv);
                return CAP_DECISION_DENY;
            }
            free(hv);
        }
    }

    /* A cloud tenant may only send network traffic to its configured hosts.
     * ToolManagement generated tools have no user-supplied destination, so use
     * their configured registry origin rather than accidentally denying the
     * safe dispatch path or permitting arbitrary input-derived destinations. */
    if (dsco_cloud_runtime_active() && (caps & CAP_NET)) {
        const char *target = NULL;
        char *owned = NULL;
        if (strncmp(name, "tm__", 4) == 0) {
            target = toolmgmt_base_url();
        } else if (input_json) {
            owned = json_get_str(input_json, "url");
            if (!owned) owned = json_get_str(input_json, "host");
            if (!owned) owned = json_get_str(input_json, "hostname");
            target = owned;
        }
        bool allowed = dsco_cloud_destination_allowed(target);
        free(owned);
        if (!allowed) {
            CAP_REASON("cloud runtime destination is missing or outside the tenant allowlist for tool '%s'", name);
            return CAP_DECISION_DENY;
        }
    }

#undef CAP_REASON
    return CAP_DECISION_ALLOW;
}

/* ── granular features, hooks, and the advisory-classifier gate ─────────────── */

static int risk_score(const dsco_cap_features_t *f) {
    int r = 0;
    switch (f->egress) {
        case DSCO_EGRESS_EXTERNAL: r += 40; break;
        case DSCO_EGRESS_OPAQUE:   r += 25; break;
        case DSCO_EGRESS_LAN:      r += 5;  break;
        case DSCO_EGRESS_TRUSTED:  r += 3;  break;
        default: break;
    }
    if (f->tainted_untrusted)             r += 15;
    if (f->accessed_private)              r += 15;
    if (f->input_has_secrets)             r += 15;
    if (f->shell_writes)                  r += 5;
    if (f->is_exec)                       r += 5;
    if (f->is_control)                    r += 30;
    if (f->is_spawn && f->input_has_secrets) r += 15;
    if (r > 100) r = 100;
    if (r < 0)   r = 0;
    return r;
}

dsco_cap_features_t dsco_cap_extract_features(const char *name, const char *input_json,
                                              const char *tier) {
    dsco_cap_features_t f;
    memset(&f, 0, sizeof(f));
    f.tool = name;
    f.tier = (tier && tier[0]) ? tier : "standard";
    f.caps = dsco_caps_for_tool(name, input_json);
    f.is_read      = !!(f.caps & CAP_FS_READ);
    f.is_write     = !!(f.caps & CAP_FS_WRITE);
    f.is_exec      = !!(f.caps & CAP_EXEC);
    f.is_net       = !!(f.caps & CAP_NET);
    f.is_control   = !!(f.caps & CAP_CONTROL);
    f.is_secret_tool = !!(f.caps & CAP_SECRETS);
    f.is_spawn     = is_spawn_name(name);
    f.tainted_untrusted = dsco_flow_tainted_untrusted();
    f.accessed_private  = dsco_flow_accessed_private();
    f.input_has_secrets = input_json && input_touches_secrets(input_json);
    f.shell_writes      = input_json && (f.caps & CAP_EXEC) && shell_has_write(input_json);
    f.egress = classify_egress(name, input_json, f.caps, f.dest_host, sizeof(f.dest_host));
    f.risk   = risk_score(&f);
    return f;
}

static const char *categorize(const dsco_cap_features_t *f, dsco_cap_decision_t d, const char *reason) {
    if (f->is_control)
        return (d == CAP_DECISION_DENY) ? "control-deny" : "control";
    if (d == CAP_DECISION_DENY) {
        if (reason && strstr(reason, "exfiltrate")) return "exfil-external";
        if (reason && strstr(reason, "scope"))      return "scope-deny";
        if (reason && strstr(reason, "disabled"))   return "hardening-deny";
        if (reason && strstr(reason, "cloud"))      return "cloud-deny";
        return "deny";
    }
    if (f->is_spawn)                            return "clean-spawn";
    if (f->egress == DSCO_EGRESS_LOCAL)         return "local-egress";
    if (f->egress == DSCO_EGRESS_LAN)           return "lan-egress";
    if (f->egress == DSCO_EGRESS_TRUSTED)       return "trusted-egress";
    if (f->egress == DSCO_EGRESS_EXTERNAL)      return "external-egress-ok";
    if (f->egress == DSCO_EGRESS_OPAQUE)        return "opaque-egress-ok";
    if (f->is_exec)                             return "local-exec";
    if (f->is_write)                            return "fs-write";
    if (f->is_net)                              return "net-read";
    return "read";
}

static const char *const k_egress_names[] = {"none", "local", "trusted", "lan", "external", "opaque"};

/* Built-in NDJSON logger — one labeled training record per decision to $DSCO_CAP_LOG. */
static void default_hook(const dsco_cap_event_t *ev, void *ud) {
    (void)ud;
    const char *path = getenv("DSCO_CAP_LOG");
    if (!path || !path[0])
        return;
    FILE *fp = fopen(path, "a");
    if (!fp)
        return;
    const dsco_cap_features_t *f = &ev->features;
    const char *dec = ev->decision == CAP_DECISION_ALLOW ? "allow"
                    : ev->decision == CAP_DECISION_APPROVE ? "approve" : "deny";
    fprintf(fp,
            "{\"ts\":%.0f,\"tool\":\"%s\",\"tier\":\"%s\",\"caps\":%u,\"egress\":\"%s\","
            "\"dest\":\"%s\",\"tainted\":%d,\"private\":%d,\"input_secrets\":%d,"
            "\"shell_writes\":%d,\"spawn\":%d,\"exec\":%d,\"net\":%d,\"write\":%d,"
            "\"risk\":%d,\"decision\":\"%s\",\"category\":\"%s\",\"advisor\":%d,"
            "\"advisor_conf\":%d,\"overrode\":%d}\n",
            ev->ts, f->tool ? f->tool : "", f->tier ? f->tier : "", f->caps,
            k_egress_names[f->egress], f->dest_host, f->tainted_untrusted, f->accessed_private,
            f->input_has_secrets, f->shell_writes, f->is_spawn, f->is_exec, f->is_net, f->is_write,
            f->risk, dec, ev->category ? ev->category : "", ev->advisor_vote, ev->advisor_conf,
            ev->advisor_overrode);
    fclose(fp);
}

static dsco_cap_hook_fn     g_hook = NULL;      static void *g_hook_ud = NULL;
static dsco_cap_advisor_fn  g_advisor = NULL;   static void *g_advisor_ud = NULL;

void dsco_cap_set_hook(dsco_cap_hook_fn fn, void *ud)      { g_hook = fn; g_hook_ud = ud; }
void dsco_cap_set_advisor(dsco_cap_advisor_fn fn, void *ud) { g_advisor = fn; g_advisor_ud = ud; }

dsco_cap_decision_t dsco_capability_gate(const char *name, const char *input_json,
                                         const char *tier, char *reason, size_t reason_len) {
    if (!tier || !tier[0])
        tier = "standard";
    char rbuf[512];
    if (!reason || !reason_len) {
        reason = rbuf;
        reason_len = sizeof(rbuf);
    }
    reason[0] = '\0';

    dsco_cap_features_t f = dsco_cap_extract_features(name, input_json, tier);
    dsco_cap_decision_t decision = rule_gate(name, input_json, tier, reason, reason_len);
    const char *category = categorize(&f, decision, reason);

    /* Advisory classifier: shadow (log only) or enforce (bounded override). */
    int vote = -1, conf = 0;
    bool overrode = false;
    const char *mode = getenv("DSCO_CAP_CLASSIFIER");
    if (g_advisor && mode && strcasecmp(mode, "off") != 0 && mode[0]) {
        vote = g_advisor(&f, &conf, g_advisor_ud);
        if (strcasecmp(mode, "enforce") == 0 && conf >= 70) {
            if (vote == 0 && decision == CAP_DECISION_ALLOW) {
                /* tighten: deny what the rule allowed */
                decision = CAP_DECISION_DENY;
                overrode = true;
                category = "classifier-deny";
                snprintf(reason, reason_len, "classifier denied '%s' (confidence %d)", name, conf);
            } else if (vote == 1 && decision == CAP_DECISION_DENY) {
                /* loosen: only non-security denials, and only with explicit opt-in */
                bool hard = strstr(category, "exfil") || strstr(category, "control");
                if (!hard && env_tristate("DSCO_CAP_CLASSIFIER_CAN_ALLOW") == 1) {
                    decision = CAP_DECISION_ALLOW;
                    overrode = true;
                    category = "classifier-allow";
                }
            }
        }
    }

    /* Optional compiled-LR advisory FLOOR (src/cap_model.c, generated from the trained
     * advisor_model_v2.joblib). It can only TIGHTEN, and only on the exfil edge:
     *   final_deny = rule_deny || (cap_model_deny() && egress is external/opaque)
     * The rule gate stays the conservative floor. Shadow-able via DSCO_CAP_MODEL:
     *   unset/"off" -> ignored;  "shadow" -> scored + logged, never changes the outcome;
     *   "1"/"on"/"enforce" -> applies the floor. */
    const char *mmode = getenv("DSCO_CAP_MODEL");
    if (mmode && mmode[0] && strcasecmp(mmode, "off") != 0) {
        char text[2048];
        snprintf(text, sizeof(text), "%s %s", name ? name : "", input_json ? input_json : "");
        int tiercode = strcasecmp(f.tier, "untrusted") == 0 ? 0
                     : strcasecmp(f.tier, "trusted") == 0 ? 2 : 1;
        double p = cap_model_score(text, f.tainted_untrusted, f.accessed_private, tiercode);
        vote = (p >= 0.5) ? 0 : 1;                       /* 0=deny, 1=allow (advisor sense) */
        conf = (int)((p >= 0.5 ? p : 1.0 - p) * 100.0);
        bool ext = (f.egress == DSCO_EGRESS_EXTERNAL || f.egress == DSCO_EGRESS_OPAQUE);
        if (strcasecmp(mmode, "shadow") != 0 && p >= 0.5 && decision == CAP_DECISION_ALLOW && ext) {
            decision = CAP_DECISION_DENY;
            overrode = true;
            category = "model-floor-deny";
            snprintf(reason, reason_len,
                     "cap_model floor denied '%s' (deny prob %d%%, external/opaque egress)",
                     name, conf);
        }
    }

    dsco_cap_event_t ev = {
        .features = f, .decision = decision, .category = category, .reason = reason,
        .advisor_vote = vote, .advisor_conf = conf, .advisor_overrode = overrode,
        .ts = (double)time(NULL),
    };
    if (g_hook)
        g_hook(&ev, g_hook_ud);
    else
        default_hook(&ev, NULL);
    return decision;
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
