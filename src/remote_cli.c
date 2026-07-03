/* remote_cli.c — direct SSH fleet control for dsco.
 *
 * `dsco remote <peer> [cmd...]` and `dsco fleet` give usable point-to-point
 * control of fleet machines without any of the ~/bridge spool machinery
 * (no launchd daemon, no rsync outbox/inbox, no signed-exec files, no Python
 * bus). We reuse ONLY the flat host registry at ~/bridge/fleet/<peer>.host
 * (NAME/USER/ADDR) and drive plain SSH. Pure libc — always compiled, no
 * libsodium/mbedtls/curl dependency.
 */

#include "remote_cli.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <stdbool.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/select.h>

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static const char *bridge_home(void) {
    const char *h = getenv("HOME");
    return (h && *h) ? h : "/tmp";
}

/* Pull a KEY="value" field out of a fleet .host line into dst. */
static void host_field(const char *line, const char *key, char *dst, size_t cap) {
    char pat[64];
    snprintf(pat, sizeof(pat), "%s=\"", key);
    const char *v = strstr(line, pat);
    if (!v)
        return;
    v += strlen(pat);
    size_t i = 0;
    while (*v && *v != '"' && i + 1 < cap)
        dst[i++] = *v++;
    dst[i] = '\0';
}

/* Read ~/bridge/fleet/<peer>.host → user/addr. Returns true if the file
 * exists (addr may still be empty if malformed). */
static bool fleet_lookup(const char *peer, char *user, size_t ul, char *addr, size_t al) {
    char path[600];
    snprintf(path, sizeof(path), "%s/bridge/fleet/%s.host", bridge_home(), peer);
    FILE *f = fopen(path, "r");
    if (!f)
        return false;
    user[0] = '\0';
    addr[0] = '\0';
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        host_field(line, "USER", user, ul);
        host_field(line, "ADDR", addr, al);
    }
    fclose(f);
    return true;
}

/* Public wrapper so the compute fabric's "fleet" backend can resolve peers. */
bool dsco_fleet_resolve(const char *peer, char *user, size_t ul, char *addr, size_t al) {
    return fleet_lookup(peer, user, ul, addr, al);
}

/* Non-blocking TCP reachability probe with a millisecond timeout. */
static bool tcp_reachable(const char *addr, int port, int timeout_ms) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return false;
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, addr, &sa.sin_addr) != 1) {
        close(fd);
        return false;
    }
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    bool ok = false;
    int r = connect(fd, (struct sockaddr *)&sa, sizeof(sa));
    if (r == 0) {
        ok = true;
    } else if (errno == EINPROGRESS) {
        fd_set wf;
        FD_ZERO(&wf);
        FD_SET(fd, &wf);
        struct timeval tv = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
        if (select(fd + 1, NULL, &wf, NULL, &tv) > 0) {
            int err = 0;
            socklen_t el = sizeof(err);
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el);
            ok = (err == 0);
        }
    }
    close(fd);
    return ok;
}

/* ── `dsco remote <peer> [cmd...]` ───────────────────────────────────────── */

int dsco_remote_cli(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage:\n"
                        "  dsco remote <peer> <command...>   run a command on <peer>\n"
                        "  dsco remote <peer>                open an interactive shell\n"
                        "  dsco fleet                        list peers + reachability\n"
                        "\nexample: dsco remote matrix uname -a\n");
        return 2;
    }

    const char *peer = argv[2];
    char user[128] = "", addr[128] = "";
    if (!fleet_lookup(peer, user, sizeof(user), addr, sizeof(addr))) {
        fprintf(stderr,
                "dsco remote: unknown peer '%s' (no ~/bridge/fleet/%s.host)\n"
                "  run 'dsco fleet' to list configured peers\n",
                peer, peer);
        return 3;
    }
    if (!addr[0]) {
        fprintf(stderr, "dsco remote: no ADDR for peer '%s'\n", peer);
        return 3;
    }

    char target[300];
    snprintf(target, sizeof(target), "%s@%s", user[0] ? user : "agent", addr);

    /* Assemble ssh argv. For a command we join argv[3..] into one remote
     * command string and run non-interactively; with no command we open an
     * interactive shell (allocating a tty). */
    char *sv[16];
    int n = 0;
    sv[n++] = "ssh";
    sv[n++] = "-o";
    sv[n++] = "ConnectTimeout=8";
    sv[n++] = "-o";
    sv[n++] = "StrictHostKeyChecking=accept-new";

    char *joined = NULL;
    if (argc > 3) {
        sv[n++] = "-o";
        sv[n++] = "BatchMode=yes";
        sv[n++] = target;
        size_t need = 1;
        for (int i = 3; i < argc; i++)
            need += strlen(argv[i]) + 1;
        joined = malloc(need);
        if (!joined) {
            fprintf(stderr, "dsco remote: out of memory\n");
            return 1;
        }
        joined[0] = '\0';
        for (int i = 3; i < argc; i++) {
            strcat(joined, argv[i]);
            if (i + 1 < argc)
                strcat(joined, " ");
        }
        sv[n++] = joined;
    } else {
        sv[n++] = "-t"; /* force a tty for interactive use */
        sv[n++] = target;
    }
    sv[n] = NULL;

    /* Replace this process with ssh: stdout/stderr/tty and exit code all
     * pass straight through to the caller. */
    execvp("ssh", sv);
    perror("dsco remote: exec ssh");
    free(joined);
    return 127;
}

/* ── `dsco fleet` ────────────────────────────────────────────────────────── */

int dsco_fleet_cli(int argc, char **argv) {
    (void)argc;
    (void)argv;
    char dir[600];
    snprintf(dir, sizeof(dir), "%s/bridge/fleet", bridge_home());
    DIR *d = opendir(dir);
    if (!d) {
        printf("No fleet configured (%s not found).\n", dir);
        return 0;
    }

    printf("\033[1m%-12s %-16s %-22s %-22s %s\033[0m\n", "PEER", "ADDR", "ROLES", "LAST SEEN",
           "STATUS");

    struct dirent *e;
    int count = 0;
    while ((e = readdir(d)) != NULL) {
        size_t nl = strlen(e->d_name);
        if (nl < 6 || strcmp(e->d_name + nl - 5, ".host") != 0)
            continue;

        char path[1400];
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        FILE *f = fopen(path, "r");
        if (!f)
            continue;
        char name[128] = "", addr[128] = "", roles[128] = "", seen[64] = "";
        char line[512];
        while (fgets(line, sizeof(line), f)) {
            host_field(line, "NAME", name, sizeof(name));
            host_field(line, "ADDR", addr, sizeof(addr));
            host_field(line, "ROLES", roles, sizeof(roles));
            host_field(line, "LAST_SEEN", seen, sizeof(seen));
        }
        fclose(f);

        bool up = addr[0] && tcp_reachable(addr, 22, 700);
        printf("%-12s %-16s %-22s %-22s %s%s\033[0m\n", name[0] ? name : e->d_name,
               addr[0] ? addr : "-", roles[0] ? roles : "-", seen[0] ? seen : "-",
               up ? "\033[32m" : "\033[31m", up ? "UP" : "down");
        count++;
    }
    closedir(d);
    if (!count)
        printf("(no peers in %s)\n", dir);
    return 0;
}
