/* bus_cli.c — `dsco bus`: realtime pub/sub over the durable ipc message bus.
 *
 * Publish/consume on topics. The durable side is ipc's SQLite bus, shared
 * across processes/agents via DSCO_IPC_DB. Cross-machine realtime fan-out
 * happens inside the running runtime: the mesh receive callback wired by
 * dsco_net_node() (net_tool.c) decodes inbound {topic,to,body} envelopes and
 * calls ipc_send(), so a message published on one node lands on this bus on
 * every meshed node. This CLI drives the same bus from the shell.
 */

#include "ipc.h"
#include "durable_agents.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int dsco_bus_cli(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr,
                "usage:\n"
                "  dsco bus pub  <topic> <message...>   publish to a topic\n"
                "  dsco bus recv <topic>                read pending messages once\n"
                "  dsco bus tail <topic>                stream new messages (Ctrl-C to stop)\n"
                "\nshare one bus across processes/agents/nodes with DSCO_IPC_DB=/path/bus.db\n");
        return 2;
    }
    const char *sub = argv[2];
    const char *topic = argv[3];

    char db[4096];
    durable_agents_default_db_path(db, sizeof(db));
    if (!ipc_init(db, "bus-cli")) {
        fprintf(stderr, "dsco bus: could not open the ipc bus\n");
        return 1;
    }

    if (strcmp(sub, "pub") == 0) {
        size_t need = 1;
        for (int i = 4; i < argc; i++)
            need += strlen(argv[i]) + 1;
        char *body = malloc(need);
        if (!body)
            return 1;
        body[0] = '\0';
        for (int i = 4; i < argc; i++) {
            strcat(body, argv[i]);
            if (i + 1 < argc)
                strcat(body, " ");
        }
        bool ok = ipc_send(NULL, topic, body); /* to=NULL → broadcast on this bus */
        printf("%s  topic=%s  body=\"%s\"\n",
               ok ? "\033[32mpublished\033[0m" : "\033[31mFAILED\033[0m", topic, body);
        free(body);
        return ok ? 0 : 1;
    }

    if (strcmp(sub, "recv") == 0 || strcmp(sub, "tail") == 0) {
        int tail = strcmp(sub, "tail") == 0;
        ipc_message_t msgs[64];
        if (tail)
            printf("\033[2mtailing topic '%s' (Ctrl-C to stop)…\033[0m\n", topic);
        do {
            int n = ipc_recv_topic(topic, msgs, 64);
            for (int i = 0; i < n; i++) {
                printf("\033[36m[%s]\033[0m %s: %s\n", topic,
                       msgs[i].from_agent[0] ? msgs[i].from_agent : "?",
                       msgs[i].body ? msgs[i].body : "");
                free(msgs[i].body);
            }
            fflush(stdout);
            if (tail)
                usleep(500 * 1000);
        } while (tail);
        return 0;
    }

    fprintf(stderr, "dsco bus: unknown subcommand '%s' (use pub|recv|tail)\n", sub);
    return 2;
}
