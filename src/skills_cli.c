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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

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

static int skills_usage(void) {
    fprintf(stderr,
            "usage:\n"
            "  dsco skills list                 list installed workspace skills\n"
            "  dsco skills hash <name>          print the content hash of a skill\n"
            "  dsco skills push <name>          publish a skill on the durable bus\n"
            "  dsco skills pull <name>          install a skill from the bus (verified)\n"
            "  dsco skills sync <name>          alias for push\n"
            "\nshare one bus across processes/agents/nodes with DSCO_IPC_DB=/path/bus.db\n");
    return 2;
}

int dsco_skills_cli(int argc, char **argv) {
    const char *sub = argc >= 3 ? argv[2] : "list";
    const char *name = argc >= 4 ? argv[3] : NULL;

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
