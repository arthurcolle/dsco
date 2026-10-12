/* skills_cli.c — `dsco skills`: distribute workspace skills over the durable bus.
 *
 * A skill is a `SKILL.md` under <workspace>/skills/<name>/. Today each process,
 * agent, and node rediscovers skills only from its own disk, so a skill authored
 * on one tree never reaches the others and a swarm cannot agree on a shared
 * standard. This closes that loop using primitives dsco already ships:
 *
 *   push <name>  read the local SKILL.md, content-hash it, publish it on the ipc
 *                bus topic "skills/<name>" (to=NULL → broadcast). Meshed nodes
 *                receive it on their own bus via the net receive callback.
 *   pull <name>  read the newest message on "skills/<name>", verify the hash, and
 *                write it into the local workspace (overwrite).
 *   list         local workspace skills.
 *   hash <name>  the content hash a push would advertise (reproducibility pin).
 *   sync <name>  push, for symmetry with a future auto-pull on workers.
 *
 * Content addressing uses a stable FNV-1a 64-bit hash of the SKILL.md bytes: not
 * cryptographic, just enough for two hosts to prove they applied the same skill
 * and for pull to reject a corrupted transfer.
 */

#include "ipc.h"
#include "durable_agents.h"
#include "workspace.h"
#include "http_pool.h"

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <errno.h>

#define SKILLS_TOPIC_PREFIX "skills/"
/* Local read buffer. Bus transfers are capped separately by IPC_MAX_BODY. */
#define SKILLS_READ_BUF (128 * 1024)

static uint64_t skills_fnv1a(const char *s, size_t n) {
    uint64_t h = 1469598103934665603ULL; /* FNV offset basis */
    for (size_t i = 0; i < n; i++) {
        h ^= (unsigned char)s[i];
        h *= 1099511628211ULL; /* FNV prime */
    }
    return h;
}

static int skills_open_bus(void) {
    char db[4096];
    durable_agents_default_db_path(db, sizeof(db));
    if (!ipc_init(db, "skills-cli")) {
        fprintf(stderr, "dsco skills: could not open the ipc bus\n");
        return -1;
    }
    return 0;
}

static void skills_topic(char *out, size_t out_len, const char *name) {
    snprintf(out, out_len, "%s%s", SKILLS_TOPIC_PREFIX, name);
}


/* ---------------------------------------------------------------- install
 * `dsco skills install <owner>/<repo>[/path][@ref] [--name N] [--dry-run]`
 *
 * Fetches a SKILL.md from a public git forge over HTTPS and installs it into
 * <workspace>/skills/<name>/SKILL.md. This is the gap that forced operators to
 * shell out to a third-party Node installer that has no dsco agent target and
 * writes to another agent's directory.
 *
 * Deliberate constraints:
 *   - HTTPS only, host allow-list. No arbitrary URL fetch, no shell, no tar.
 *   - Writes exactly one regular file inside the workspace skills dir.
 *   - Skill name is sanitized; no separators, no dotfiles, no traversal.
 *   - Refuses to clobber an existing skill unless --force.
 *   - Prints the FNV-1a content hash so an install can be pinned/compared,
 *     matching what `push`/`pull`/`hash` already advertise.
 */

typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} skills_http_buf_t;

static size_t skills_http_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    size_t total = size * nmemb;
    skills_http_buf_t *b = (skills_http_buf_t *)userdata;
    if (total > SIZE_MAX - b->len - 1)
        return 0;
    if (b->len + total + 1 > b->cap) {
        size_t cap = (b->len + total) * 2 + 1;
        char *data = realloc(b->data, cap);
        if (!data)
            return 0;
        b->data = data;
        b->cap = cap;
    }
    memcpy(b->data + b->len, ptr, total);
    b->len += total;
    b->data[b->len] = '\0';
    return total;
}

/* Only these hosts may serve a skill body. */
static bool skills_host_allowed(const char *url) {
    static const char *ok[] = {
        "https://raw.githubusercontent.com/",
        "https://gitlab.com/",
        "https://codeberg.org/",
        NULL
    };
    for (int i = 0; ok[i]; i++)
        if (strncmp(url, ok[i], strlen(ok[i])) == 0)
            return true;
    return false;
}

/* A skill name must be one safe path component. */
static bool skills_name_ok(const char *n) {
    if (!n || !*n || strlen(n) > 100)
        return false;
    if (n[0] == '.' || n[0] == '-')
        return false;
    for (const char *p = n; *p; p++) {
        bool alnum = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9');
        if (!alnum && *p != '-' && *p != '_' && *p != '.')
            return false;
        if (*p == '.' && p[1] == '.')
            return false;
    }
    return true;
}

static int skills_fetch(const char *url, skills_http_buf_t *out, long *status) {
    if (!skills_host_allowed(url)) {
        fprintf(stderr, "dsco skills: refusing non-allow-listed URL: %s\n", url);
        return -1;
    }
    CURL *curl = curl_easy_init();
    if (!curl)
        return -1;
    dsco_http_pool_apply(curl);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "dsco-skills/1.0");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, skills_http_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, out);
    CURLcode rc = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, status);
    curl_easy_cleanup(curl);
    if (rc != CURLE_OK) {
        fprintf(stderr, "dsco skills: fetch failed: %s\n", curl_easy_strerror(rc));
        return -1;
    }
    return 0;
}

/* owner/repo[/sub/path][@ref] -> raw URL candidates. */
static int skills_build_urls(const char *spec, char urls[][1024], char *skill_name, size_t name_len, int max) {
    char work[512];
    snprintf(work, sizeof(work), "%s", spec);

    char *ref = strchr(work, '@');
    if (ref) { *ref = '\0'; ref++; }
    if (!ref || !*ref)
        ref = (char *)"main";

    char *owner = work;
    char *slash = strchr(work, '/');
    if (!slash)
        return 0;
    *slash = '\0';
    char *repo = slash + 1;
    char *sub = strchr(repo, '/');
    if (sub) { *sub = '\0'; sub++; }

    const char *leaf = sub && *sub ? sub : repo;
    const char *last = strrchr(leaf, '/');
    snprintf(skill_name, name_len, "%s", last ? last + 1 : leaf);

    int n = 0;
    if (sub && *sub) {
        if (n < max) snprintf(urls[n++], 1024, "https://raw.githubusercontent.com/%s/%s/%s/%s/SKILL.md", owner, repo, ref, sub);
        if (n < max) snprintf(urls[n++], 1024, "https://raw.githubusercontent.com/%s/%s/%s/%s", owner, repo, ref, sub);
    } else {
        /* Conventional layouts, cheapest-first. */
        if (n < max) snprintf(urls[n++], 1024, "https://raw.githubusercontent.com/%s/%s/%s/SKILL.md", owner, repo, ref);
        if (n < max) snprintf(urls[n++], 1024, "https://raw.githubusercontent.com/%s/%s/%s/skills/%s/SKILL.md", owner, repo, ref, repo);
    }
    return n;
}

static int skills_install(int argc, char **argv) {
    const char *spec = NULL, *override = NULL;
    bool force = false, dry = false;
    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--force") == 0) force = true;
        else if (strcmp(argv[i], "--dry-run") == 0) dry = true;
        else if (strcmp(argv[i], "--name") == 0 && i + 1 < argc) override = argv[++i];
        else if (argv[i][0] != '-') spec = argv[i];
    }
    if (!spec) {
        fprintf(stderr, "usage: dsco skills install <owner>/<repo>[/path][@ref] [--name N] [--force] [--dry-run]\n");
        return 2;
    }

    char urls[4][1024];
    char derived[128] = {0};
    int n = skills_build_urls(spec, urls, derived, sizeof(derived), 4);
    if (n == 0) {
        fprintf(stderr, "dsco skills: could not parse '%s' (want owner/repo[/path][@ref])\n", spec);
        return 2;
    }

    const char *name = override ? override : derived;
    if (!skills_name_ok(name)) {
        fprintf(stderr, "dsco skills: unsafe skill name '%s'\n", name);
        return 2;
    }
    if (dsco_workspace_skill_exists(name) && !force && !dry) {
        fprintf(stderr, "dsco skills: '%s' already installed (use --force to overwrite)\n", name);
        return 1;
    }

    skills_http_buf_t buf = {0};
    long status = 0;
    int got = -1;
    for (int i = 0; i < n; i++) {
        free(buf.data);
        memset(&buf, 0, sizeof(buf));
        status = 0;
        if (skills_fetch(urls[i], &buf, &status) == 0 && status == 200 && buf.len > 0) {
            printf("source: %s\n", urls[i]);
            got = 0;
            break;
        }
    }
    if (got != 0) {
        fprintf(stderr, "dsco skills: no SKILL.md found for '%s' (last HTTP %ld)\n", spec, status);
        free(buf.data);
        return 1;
    }

    uint64_t hash = skills_fnv1a(buf.data, buf.len);
    if (dry) {
        printf("would install '%s'  fnv1a:%016llx  (%zu bytes)\n",
               name, (unsigned long long)hash, buf.len);
        free(buf.data);
        return 0;
    }

    /* dsco_workspace_create_skill: 1 = written, 0 = existed and not overwritten,
     * -1 = invalid name or I/O failure. Only 1 means this install wrote bytes. */
    int rc = dsco_workspace_create_skill(name, buf.data, force);
    if (rc == 0) {
        fprintf(stderr, "dsco skills: '%s' already installed (use --force to overwrite)\n", name);
        free(buf.data);
        return 1;
    }
    if (rc < 0) {
        fprintf(stderr, "dsco skills: failed to write skill '%s'\n", name);
        free(buf.data);
        return 1;
    }
    printf("\033[32minstalled\033[0m skill '%s'  fnv1a:%016llx  (%zu bytes)\n",
           name, (unsigned long long)hash, buf.len);
    printf("  %s/skills/%s/SKILL.md\n", dsco_workspace_root(), name);
    free(buf.data);
    return 0;
}

static int skills_usage(void) {
    fprintf(stderr,
            "usage:\n"
            "  dsco skills install <spec>       install a skill from a git forge over HTTPS\n"
            "  dsco skills list                 list installed workspace skills\n"
            "  dsco skills hash <name>          print the content hash of a skill\n"
            "  dsco skills push <name>          publish a skill on the durable bus\n"
            "  dsco skills pull <name>          install a skill from the bus (verified)\n"
            "  dsco skills sync <name>          alias for push\n"
            "\nspec: <owner>/<repo>[/path][@ref]   e.g. typesafe-ai/skills/skills/typesafe-ai\n"
            "  flags: --name <n>  --force  --dry-run\n"
            "\nshare one bus across processes/agents/nodes with DSCO_IPC_DB=/path/bus.db\n");
    return 2;
}

int dsco_skills_cli(int argc, char **argv) {
    const char *sub = argc >= 3 ? argv[2] : "list";
    const char *name = argc >= 4 ? argv[3] : NULL;

    if (strcmp(sub, "install") == 0)
        return skills_install(argc, argv);

    if (strcmp(sub, "list") == 0) {
        char *buf = malloc(SKILLS_READ_BUF);
        if (!buf)
            return 1;
        int n = dsco_workspace_list_skills(buf, SKILLS_READ_BUF);
        if (buf[0]) {
            fputs(buf, stdout);
            if (buf[strlen(buf) - 1] != '\n')
                putchar('\n');
        }
        free(buf);
        return n >= 0 ? 0 : 1;
    }

    if (!name)
        return skills_usage();

    if (strcmp(sub, "hash") == 0) {
        char *buf = malloc(SKILLS_READ_BUF);
        if (!buf)
            return 1;
        if (dsco_workspace_show_skill(name, buf, SKILLS_READ_BUF) != 0) {
            fprintf(stderr, "dsco skills: %s\n", buf);
            free(buf);
            return 1;
        }
        size_t clen = strlen(buf);
        printf("fnv1a:%016llx  %s  (%zu bytes)\n",
               (unsigned long long)skills_fnv1a(buf, clen), name, clen);
        free(buf);
        return 0;
    }

    if (strcmp(sub, "push") == 0 || strcmp(sub, "sync") == 0) {
        char *content = malloc(SKILLS_READ_BUF);
        if (!content)
            return 1;
        if (dsco_workspace_show_skill(name, content, SKILLS_READ_BUF) != 0) {
            fprintf(stderr, "dsco skills: %s\n", content);
            free(content);
            return 1;
        }
        size_t clen = strlen(content);
        uint64_t h = skills_fnv1a(content, clen);

        /* frame: "fnv1a:<16 hex>\n" + raw SKILL.md */
        char header[32];
        int hlen = snprintf(header, sizeof(header), "fnv1a:%016llx\n", (unsigned long long)h);
        size_t need = (size_t)hlen + clen + 1;
        if (need > IPC_MAX_BODY) {
            fprintf(stderr,
                    "dsco skills: '%s' is %zu bytes; exceeds bus body cap %d "
                    "(chunked transfer is not implemented yet)\n",
                    name, clen, IPC_MAX_BODY);
            free(content);
            return 1;
        }
        char *body = malloc(need);
        if (!body) {
            free(content);
            return 1;
        }
        memcpy(body, header, (size_t)hlen);
        memcpy(body + hlen, content, clen + 1);

        if (skills_open_bus() != 0) {
            free(content);
            free(body);
            return 1;
        }
        char topic[IPC_MAX_TOPIC];
        skills_topic(topic, sizeof(topic), name);
        bool ok = ipc_send(NULL, topic, body);
        printf("%s skill '%s' → topic %s  fnv1a:%016llx  (%zu bytes)\n",
               ok ? "\033[32mpushed\033[0m" : "\033[31mFAILED\033[0m", name, topic,
               (unsigned long long)h, clen);
        free(content);
        free(body);
        return ok ? 0 : 1;
    }

    if (strcmp(sub, "pull") == 0) {
        if (skills_open_bus() != 0)
            return 1;
        char topic[IPC_MAX_TOPIC];
        skills_topic(topic, sizeof(topic), name);
        ipc_message_t msgs[16];
        int n = ipc_recv_topic(topic, msgs, 16);
        if (n <= 0) {
            fprintf(stderr,
                    "dsco skills: nothing on topic %s — push it from another "
                    "node/process sharing this bus first\n",
                    topic);
            return 1;
        }
        /* Newest wins: the last message on the topic. */
        char *msg = msgs[n - 1].body;
        int rc = 1;
        char *nl = msg ? strchr(msg, '\n') : NULL;
        if (nl && strncmp(msg, "fnv1a:", 6) == 0) {
            uint64_t declared = strtoull(msg + 6, NULL, 16);
            const char *content = nl + 1;
            uint64_t got = skills_fnv1a(content, strlen(content));
            if (got != declared) {
                fprintf(stderr,
                        "dsco skills: hash mismatch on '%s' (declared %016llx, got "
                        "%016llx) — refusing to install\n",
                        name, (unsigned long long)declared, (unsigned long long)got);
            } else {
                int w = dsco_workspace_create_skill(name, content, true);
                if (w >= 0) {
                    printf("\033[32mpulled\033[0m skill '%s' ← topic %s  fnv1a:%016llx  (%zu bytes)\n",
                           name, topic, (unsigned long long)got, strlen(content));
                    rc = 0;
                } else {
                    fprintf(stderr, "dsco skills: failed to write skill '%s'\n", name);
                }
            }
        } else {
            fprintf(stderr, "dsco skills: malformed skill message on %s\n", topic);
        }
        for (int i = 0; i < n; i++)
            free(msgs[i].body);
        return rc;
    }

    return skills_usage();
}
