/* cluster.c — distributed-inference cluster primitives (strict superset of exo).
 *
 * Reimplemented from the algorithm (no upstream code):
 *   1. cluster inventory   — probe local + ~/bridge/fleet peers for live
 *                            topology: cores, memory, accelerator.
 *   2. memory-weighted ring partition — split a model's layers across the
 *                            heterogeneous nodes proportional to available
 *                            memory, in pipeline/ring order.
 *   3. placement preview   — per-node layer range, memory delta, feasibility.
 */

#include "cluster.h"
#include "remote_cli.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <dirent.h>

#define GIB 1073741824.0
#define CLUSTER_MAX 64

typedef struct {
    char id[64];
    char addr[64];
    char arch[16];
    char model[80];
    int cores;
    long mem_bytes; /* total physical */
    char accel[16]; /* "metal" | "cuda" | "rocm" | "vulkan" | "cpu" */
    int up;
    int is_local;
} cnode_t;

/* Run a command, capture stdout into a malloc'd string. */
static char *cap(const char *cmd) {
    FILE *fp = popen(cmd, "r");
    if (!fp)
        return NULL;
    size_t cap = 512, len = 0;
    char *out = malloc(cap);
    if (!out) {
        pclose(fp);
        return NULL;
    }
    size_t r;
    char buf[512];
    while ((r = fread(buf, 1, sizeof(buf), fp)) > 0) {
        if (len + r + 1 > cap) {
            cap = (len + r + 1) * 2;
            char *nb = realloc(out, cap);
            if (!nb)
                break;
            out = nb;
        }
        memcpy(out + len, buf, r);
        len += r;
    }
    out[len] = '\0';
    pclose(fp);
    return out;
}

static void trim(char *s) {
    if (!s)
        return;
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' || s[n - 1] == '\t'))
        s[--n] = '\0';
}

static void set_accel(cnode_t *n) {
    int arm = strstr(n->arch, "arm") != NULL || strstr(n->arch, "aarch64") != NULL;
    snprintf(n->accel, sizeof(n->accel), "%s", arm ? "metal" : "cpu");
}

/* Any usable GPU backend, regardless of vendor (Apple/NVIDIA/AMD/cross). */
static int is_gpu(const char *a) {
    return a && (!strcmp(a, "metal") || !strcmp(a, "cuda") || !strcmp(a, "rocm") ||
                 !strcmp(a, "vulkan"));
}

/* A shell snippet (for a remote launch) that picks the ggml device for THIS
 * node's accelerator: NVIDIA→CUDA0, AMD→ROCm0, Apple→MTL0, else CPU. Emitted
 * so each peer's rpc-server self-selects — no central per-node config. */
#define DSCO_DEV_DETECT                                                                            \
    "D=CPU; if command -v nvidia-smi >/dev/null 2>&1; then D=CUDA0; "                              \
    "elif command -v rocm-smi >/dev/null 2>&1; then D=ROCm0; "                                     \
    "elif [ \"$(uname -s)\" = Darwin ]; then D=MTL0; fi; "

static void probe_local(cnode_t *n) {
    memset(n, 0, sizeof(*n));
    n->is_local = 1;
    n->up = 1;
    char host[64] = "local";
    gethostname(host, sizeof(host));
    /* strip .local suffix for a stable id */
    char *dot = strchr(host, '.');
    if (dot)
        *dot = '\0';
    snprintf(n->id, sizeof(n->id), "%s", host);
    snprintf(n->addr, sizeof(n->addr), "127.0.0.1");
    char *a = cap("uname -m 2>/dev/null");
    trim(a);
    snprintf(n->arch, sizeof(n->arch), "%s", (a && a[0]) ? a : "?");
    free(a);
    char *c = cap("sysctl -n hw.ncpu 2>/dev/null");
    n->cores = c ? atoi(c) : 0;
    free(c);
    char *m = cap("sysctl -n hw.memsize 2>/dev/null");
    n->mem_bytes = m ? atoll(m) : 0;
    free(m);
    char *md = cap("sysctl -n hw.model 2>/dev/null");
    trim(md);
    snprintf(n->model, sizeof(n->model), "%s", (md && md[0]) ? md : "?");
    free(md);
    set_accel(n);
}

static void probe_remote(const char *peer, cnode_t *n) {
    memset(n, 0, sizeof(*n));
    snprintf(n->id, sizeof(n->id), "%s", peer);
    char user[128] = "", addr[128] = "";
    if (!dsco_fleet_resolve(peer, user, sizeof(user), addr, sizeof(addr)) || !addr[0]) {
        n->up = 0;
        return;
    }
    snprintf(n->addr, sizeof(n->addr), "%s", addr);
    /* Portable probe → arch, cores, mem_bytes, accel (works on macOS/Metal,
     * NVIDIA/CUDA via nvidia-smi VRAM, AMD/ROCm, and plain Linux/CPU). */
    char cmd[1000];
    snprintf(
        cmd, sizeof(cmd),
        "ssh -o BatchMode=yes -o ConnectTimeout=6 -o StrictHostKeyChecking=accept-new %s@%s "
        "'A=cpu; C=0; MEM=0; M=$(uname -m); "
        "if command -v nvidia-smi >/dev/null 2>&1; then A=cuda; C=$(nproc 2>/dev/null||echo 0); "
        "V=$(nvidia-smi --query-gpu=memory.total --format=csv,noheader,nounits 2>/dev/null|head "
        "-1); "
        "MEM=$(( ${V:-0} * 1048576 )); "
        "elif command -v rocm-smi >/dev/null 2>&1; then A=rocm; C=$(nproc 2>/dev/null||echo 0); "
        "MEM=$(awk \"/MemTotal/{print \\$2*1024}\" /proc/meminfo 2>/dev/null||echo 0); "
        "elif [ \"$(uname -s)\" = Darwin ]; then A=metal; C=$(sysctl -n hw.ncpu); MEM=$(sysctl -n "
        "hw.memsize); "
        "else C=$(nproc 2>/dev/null||echo 0); MEM=$(awk \"/MemTotal/{print \\$2*1024}\" "
        "/proc/meminfo 2>/dev/null||echo 0); fi; "
        "printf \"%%s\\n%%s\\n%%s\\n%%s\\n\" \"$M\" \"$C\" \"$MEM\" \"$A\"' 2>/dev/null",
        user[0] ? user : "agent", addr);
    char *o = cap(cmd);
    if (o && o[0]) {
        char *save = NULL;
        char *l1 = strtok_r(o, "\n", &save);    /* arch */
        char *l2 = strtok_r(NULL, "\n", &save); /* cores */
        char *l3 = strtok_r(NULL, "\n", &save); /* mem bytes */
        char *l4 = strtok_r(NULL, "\n", &save); /* accel */
        if (l1)
            snprintf(n->arch, sizeof(n->arch), "%s", l1);
        if (l2)
            n->cores = atoi(l2);
        if (l3)
            n->mem_bytes = atoll(l3);
        if (l4 && l4[0])
            snprintf(n->accel, sizeof(n->accel), "%s", l4);
        snprintf(n->model, sizeof(n->model), "%s", n->arch);
    }
    free(o);
    n->up = (n->cores > 0 && n->mem_bytes > 0);
    if (!n->accel[0])
        set_accel(n);
}

static int cluster_inventory(cnode_t *out, int max) {
    int n = 0;
    if (n < max)
        probe_local(&out[n++]);
    char localid[64] = "";
    if (n > 0)
        snprintf(localid, sizeof(localid), "%s", out[0].id);

    const char *home = getenv("HOME");
    char dir[512];
    snprintf(dir, sizeof(dir), "%s/bridge/fleet", home ? home : "/tmp");
    DIR *d = opendir(dir);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL && n < max) {
            size_t l = strlen(e->d_name);
            if (l < 6 || strcmp(e->d_name + l - 5, ".host") != 0)
                continue;
            char peer[128];
            snprintf(peer, sizeof(peer), "%.*s", (int)(l - 5), e->d_name);
            if (localid[0] && strcasecmp(peer, localid) == 0)
                continue; /* skip self */
            probe_remote(peer, &out[n++]);
        }
        closedir(d);
    }
    return n;
}

/* ── Model presets (fp16 GB, layer count) ─────────────────────────────────── */
typedef struct {
    const char *name;
    double gb_fp16;
    int layers;
} model_preset_t;

static const model_preset_t PRESETS[] = {
    {"llama3-8b", 16.0, 32},  {"llama3-70b", 140.0, 80},        {"llama3-405b", 810.0, 126},
    {"qwen2-72b", 145.0, 80}, {"qwen2-7b", 15.0, 28},           {"mixtral-8x7b", 93.0, 32},
    {"gemma2-27b", 54.0, 46}, {"deepseek-r1-671b", 1342.0, 61}, {"mistral-7b", 15.0, 32},
};

static const model_preset_t *preset_find(const char *name) {
    for (size_t i = 0; i < sizeof(PRESETS) / sizeof(PRESETS[0]); i++)
        if (strcasecmp(PRESETS[i].name, name) == 0)
            return &PRESETS[i];
    return NULL;
}

/* ── Memory-weighted ring partition ───────────────────────────────────────── */
static void cluster_plan(cnode_t *nodes, int n, double model_bytes, int layers, int quant,
                         const char *label) {
    double qf = (quant == 4) ? 0.25 : (quant == 8) ? 0.5 : 1.0;
    double eff = model_bytes * qf;
    double per_layer = eff / layers;

    int idx[CLUSTER_MAX], m = 0;
    double total_usable = 0;
    for (int i = 0; i < n && m < CLUSTER_MAX; i++) {
        if (nodes[i].up && nodes[i].mem_bytes > 0) {
            idx[m++] = i;
            total_usable += nodes[i].mem_bytes * 0.85; /* OS headroom */
        }
    }
    if (m == 0) {
        printf("\n  no reachable nodes to place on.\n");
        return;
    }

    /* Proportional layer counts, largest-remainder rounding to sum == layers. */
    int alloc[CLUSTER_MAX];
    double frac[CLUSTER_MAX];
    int sum = 0;
    for (int j = 0; j < m; j++) {
        double usable = nodes[idx[j]].mem_bytes * 0.85;
        double raw = layers * usable / total_usable;
        alloc[j] = (int)raw;
        frac[j] = raw - alloc[j];
        sum += alloc[j];
    }
    for (int rem = layers - sum; rem > 0; rem--) {
        int best = 0;
        double bf = -1;
        for (int j = 0; j < m; j++)
            if (frac[j] > bf) {
                bf = frac[j];
                best = j;
            }
        alloc[best]++;
        frac[best] = -1;
    }

    printf("\n\033[1mPARTITION\033[0m — %s · %d layers · %.0fGB fp16 → %d-bit (%.0fGB weights)\n",
           label, layers, model_bytes / GIB, quant, eff / GIB);
    printf("  %-12s %-16s %5s  %9s  %9s  %s\n", "NODE", "LAYERS", "N", "NEEDS", "AVAIL", "FIT");

    int start = 0, all_fit = 1;
    double total_need = 0;
    for (int j = 0; j < m; j++) {
        int a = start, b = start + alloc[j];
        start = b;
        double need = alloc[j] * per_layer * 1.15; /* +15% activations/KV cache */
        double avail = nodes[idx[j]].mem_bytes * 0.85;
        int fit = need <= avail;
        if (!fit)
            all_fit = 0;
        total_need += need;
        char range[24];
        snprintf(range, sizeof(range), "[%d..%d)", a, b);
        printf("  %-12s %-16s %5d  %7.1fGB  %7.1fGB  %s\n", nodes[idx[j]].id, range, alloc[j],
               need / GIB, avail / GIB, fit ? "\033[32m✓\033[0m" : "\033[31m✗ OVER\033[0m");
    }
    printf("  ─────────────────────────────────────────────────────────────\n");
    printf("  %d layers across %d node%s · total weights %.1fGB · cluster mem %.0fGB\n", layers, m,
           m == 1 ? "" : "s", total_need / GIB, total_usable / GIB);
    printf("  model fits cluster: %s\n",
           all_fit ? "\033[32mYES\033[0m" : "\033[31mNO — add memory/nodes or lower quant\033[0m");
    printf("  pipeline ring: ");
    for (int j = 0; j < m; j++)
        printf("%s%s", nodes[idx[j]].id, j + 1 < m ? " → " : "");
    printf(" → (wrap)\n");
}

/* ── CLI ──────────────────────────────────────────────────────────────────── */
/* ══════════════════════════════════════════════════════════════════════════
 * DISTRIBUTED-INFERENCE ENGINES  (pluggable transport behind one command)
 *   llamacpp — llama.cpp RPC backend (proven here): rpc-server per peer + a
 *              local llama-completion --rpc that splits layers memory-weighted.
 *   exo      — exo / MLX-distributed (peer-to-peer, Thunderbolt/RDMA-native).
 * The planner (cluster inventory + memory-weighted split) feeds either.
 * ══════════════════════════════════════════════════════════════════════════ */
static const char *llamacpp_dir(void) {
    const char *d = getenv("DSCO_LLAMACPP_DIR");
    if (d && d[0])
        return d;
    static char buf[512];
    const char *home = getenv("HOME");
    snprintf(buf, sizeof(buf), "%s/native_tools/llama.cpp/build/bin", home ? home : ".");
    return buf;
}
static int have_llamacpp(void) {
    char p[600];
    snprintf(p, sizeof(p), "%s/llama-completion", llamacpp_dir());
    return access(p, X_OK) == 0;
}
static int have_exo(void) {
    return system("command -v exo >/dev/null 2>&1") == 0;
}

static int cluster_run(int argc, char **argv) {
    const char *engine = "auto", *model = NULL, *prompt = "The capital of France is",
               *peers_arg = NULL;
    int ntok = 64, dry = 0, port = 50052;
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--engine") && i + 1 < argc)
            engine = argv[++i];
        else if (!strcmp(argv[i], "--model") && i + 1 < argc)
            model = argv[++i];
        else if ((!strcmp(argv[i], "--prompt") || !strcmp(argv[i], "-p")) && i + 1 < argc)
            prompt = argv[++i];
        else if (!strcmp(argv[i], "--peers") && i + 1 < argc)
            peers_arg = argv[++i];
        else if ((!strcmp(argv[i], "-n") || !strcmp(argv[i], "--tokens")) && i + 1 < argc)
            ntok = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--port") && i + 1 < argc)
            port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--dry-run") || !strcmp(argv[i], "--dry"))
            dry = 1;
    }

    int llc = have_llamacpp(), exo = have_exo();
    if (!strcmp(engine, "auto"))
        engine = llc ? "llamacpp" : exo ? "exo" : "none";
    printf("\033[1mdsco cluster run\033[0m — engine=%s  \033[2m(available: llamacpp=%s "
           "exo=%s)\033[0m\n",
           engine, llc ? "yes" : "no", exo ? "yes" : "no");

    if (!strcmp(engine, "exo")) {
        if (!exo) {
            printf("exo not installed.\n  install: pipx install exo  (or "
                   "https://github.com/exo-explore/exo)\n"
                   "  exo is peer-to-peer + MLX-distributed; run `exo` on each Mac and they "
                   "auto-discover.\n");
            return 3;
        }
        char cmd[1024];
        snprintf(cmd, sizeof(cmd), "exo run %s --prompt %s -n %d", model ? model : "", prompt,
                 ntok);
        printf("→ %s\n", cmd);
        return dry ? 0 : system(cmd);
    }
    if (strcmp(engine, "llamacpp") != 0) {
        printf("no distributed engine available — build llama.cpp (RPC) or install exo.\n");
        return 3;
    }
    if (!model) {
        printf("usage: dsco cluster run --model <path.gguf> [--prompt \"…\"] [--peers a,b] "
               "[--engine llamacpp|exo|auto] [-n N] [--dry-run]\n");
        return 2;
    }

    /* Resolve peers: explicit --peers, else reachable GPU fleet peers (any vendor). */
    char peernames[16][64];
    int npn = 0;
    if (peers_arg && peers_arg[0]) {
        char *dup = strdup(peers_arg);
        for (char *t = strtok(dup, ","); t && npn < 16; t = strtok(NULL, ","))
            snprintf(peernames[npn++], 64, "%s", t);
        free(dup);
    } else {
        cnode_t nodes[CLUSTER_MAX];
        int n = cluster_inventory(nodes, CLUSTER_MAX);
        for (int i = 0; i < n && npn < 16; i++)
            if (nodes[i].up && !nodes[i].is_local && is_gpu(nodes[i].accel))
                snprintf(peernames[npn++], 64, "%s", nodes[i].id);
    }

    const char *remote_dir = getenv("DSCO_LLAMACPP_REMOTE_DIR");
    if (!remote_dir)
        remote_dir = "~/dsco-llama";
    char rpc_list[1024] = "";
    int npeers = 0;
    for (int i = 0; i < npn; i++) {
        char user[128] = "", addr[128] = "";
        if (!dsco_fleet_resolve(peernames[i], user, sizeof(user), addr, sizeof(addr)) || !addr[0]) {
            printf("  skip %-10s (unresolved in ~/bridge/fleet)\n", peernames[i]);
            continue;
        }
        printf("  peer %-10s %s:%d  → ensure ggml-rpc-server (Metal, cache)\n", peernames[i], addr,
               port);
        if (!dry) {
            char ssh[1400];
            snprintf(ssh, sizeof(ssh),
                     "ssh -o BatchMode=yes -o ConnectTimeout=8 %s@%s "
                     "'pgrep -f ggml-rpc-server >/dev/null || (cd %s && " DSCO_DEV_DETECT
                     "DYLD_LIBRARY_PATH=$PWD LD_LIBRARY_PATH=$PWD "
                     "nohup ./ggml-rpc-server -H 0.0.0.0 -p %d -d \"$D\" -c >/tmp/dsco_rpc.log "
                     "2>&1 & sleep 3)' "
                     "2>/dev/null",
                     user[0] ? user : "agent", addr, remote_dir, port);
            int r = system(ssh);
            (void)r;
        }
        char entry[160];
        snprintf(entry, sizeof(entry), "%s%s:%d", rpc_list[0] ? "," : "", addr, port);
        strncat(rpc_list, entry, sizeof(rpc_list) - strlen(rpc_list) - 1);
        npeers++;
    }
    if (npeers == 0)
        printf("  (no reachable peers — running LOCAL on this node only)\n");

    char bin[600];
    snprintf(bin, sizeof(bin), "%s/llama-completion", llamacpp_dir());
    char ntokbuf[16];
    snprintf(ntokbuf, sizeof(ntokbuf), "%d", ntok);
    setenv("DYLD_LIBRARY_PATH", llamacpp_dir(), 1);

    char *av[24];
    int ac = 0;
    av[ac++] = bin;
    av[ac++] = "-m";
    av[ac++] = (char *)model;
    if (npeers > 0) {
        av[ac++] = "--rpc";
        av[ac++] = rpc_list;
    }
    av[ac++] = "-ngl";
    av[ac++] = "99";
    av[ac++] = "-p";
    av[ac++] = (char *)prompt;
    av[ac++] = "-n";
    av[ac++] = ntokbuf;
    av[ac++] = "--no-warmup";
    av[ac] = NULL;

    printf("\n\033[2m→ %s -m %s %s%s -ngl 99 -p \"%s\" -n %d\033[0m\n", bin, model,
           npeers ? "--rpc " : "", npeers ? rpc_list : "(local)", prompt, ntok);
    if (dry) {
        printf("(--dry-run: not executing)\n");
        return 0;
    }
    printf("─────────── distributed generation ───────────\n");
    execvp(bin, av);
    perror("dsco cluster run: exec llama-completion");
    return 127;
}

/* Deploy the local ggml-rpc-server + dylibs to fleet peers (version-matched):
 * scp the binary + libs, patch the rpath to @loader_path, ad-hoc re-sign so
 * it runs under a different user, and verify it launches. */
static int cluster_deploy(int argc, char **argv) {
    const char *peers_arg = NULL;
    for (int i = 3; i < argc; i++)
        if (!strcmp(argv[i], "--peers") && i + 1 < argc)
            peers_arg = argv[++i];

    if (!have_llamacpp()) {
        printf("no local llama.cpp build at %s\n  (set DSCO_LLAMACPP_DIR to the build/bin path)\n",
               llamacpp_dir());
        return 3;
    }

    char peernames[16][64];
    int npn = 0;
    if (peers_arg && peers_arg[0]) {
        char *dup = strdup(peers_arg);
        for (char *t = strtok(dup, ","); t && npn < 16; t = strtok(NULL, ","))
            snprintf(peernames[npn++], 64, "%s", t);
        free(dup);
    } else {
        cnode_t nodes[CLUSTER_MAX];
        int n = cluster_inventory(nodes, CLUSTER_MAX);
        for (int k = 0; k < n && npn < 16; k++)
            if (nodes[k].up && !nodes[k].is_local && is_gpu(nodes[k].accel))
                snprintf(peernames[npn++], 64, "%s", nodes[k].id);
    }
    if (npn == 0) {
        printf("no reachable GPU peers to deploy to (see: dsco fleet)\n");
        return 0;
    }

    const char *remote_dir = getenv("DSCO_LLAMACPP_REMOTE_DIR");
    if (!remote_dir)
        remote_dir = "~/dsco-llama";
    const char *lb = llamacpp_dir();
    printf("\033[1mdsco cluster deploy\033[0m — ggml-rpc-server + dylibs → %d peer(s)\n", npn);

    int ok = 0;
    for (int k = 0; k < npn; k++) {
        char user[128] = "", addr[128] = "";
        if (!dsco_fleet_resolve(peernames[k], user, sizeof(user), addr, sizeof(addr)) || !addr[0]) {
            printf("  skip %-10s (unresolved)\n", peernames[k]);
            continue;
        }
        const char *u = user[0] ? user : "agent";
        printf("  → %-10s %-16s ", peernames[k], addr);
        fflush(stdout);
        /* The local bundle (Mach-O ggml-rpc-server + .dylib) only runs on another
         * Apple Silicon Mac. A Linux CUDA/ROCm peer needs a native llama.cpp build
         * (GGML_CUDA / GGML_HIP) — we can't cross-push binaries. Detect and guide. */
        char osq[256];
        snprintf(osq, sizeof(osq),
                 "ssh -o BatchMode=yes -o ConnectTimeout=6 %s@%s 'uname -s' 2>/dev/null", u, addr);
        char *os = cap(osq);
        int darwin = os && strstr(os, "Darwin");
        free(os);
        if (!darwin) {
            printf("\033[33m⚠ non-macOS — build llama.cpp natively "
                   "(-DGGML_CUDA=ON or -DGGML_HIP=ON) into %s\033[0m\n",
                   remote_dir);
            continue;
        }
        char cmd[2600];
        snprintf(
            cmd, sizeof(cmd),
            "ssh -o BatchMode=yes -o ConnectTimeout=8 %s@%s 'mkdir -p %s' 2>/dev/null && "
            "scp -q %s/ggml-rpc-server %s/libggml*.dylib %s@%s:%s/ 2>/dev/null && "
            "ssh -o BatchMode=yes %s@%s 'cd %s && install_name_tool -add_rpath @loader_path "
            "./ggml-rpc-server 2>/dev/null; codesign -s - -f ./ggml-rpc-server >/dev/null 2>&1; "
            "./ggml-rpc-server --help >/dev/null 2>&1 && echo DEPLOY_OK' 2>/dev/null",
            u, addr, remote_dir, lb, lb, u, addr, remote_dir, u, addr, remote_dir);
        char *out = cap(cmd);
        if (out && strstr(out, "DEPLOY_OK")) {
            printf("\033[32m✓ ready\033[0m\n");
            ok++;
        } else {
            printf("\033[31m✗ failed\033[0m\n");
        }
        free(out);
    }
    printf("deployed to %d/%d peer(s).  next: dsco cluster run --model <gguf> --peers <names>\n",
           ok, npn);
    return ok == npn ? 0 : 1;
}

int dsco_cluster_rpc_endpoints(const char *peers_csv, char *out, size_t outlen, int ensure) {
    if (out && outlen)
        out[0] = '\0';
    if (!peers_csv || !peers_csv[0])
        return 0;
    int port = 50052;
    const char *pe = getenv("DSCO_LLAMACPP_RPC_PORT");
    if (pe && atoi(pe) > 0)
        port = atoi(pe);
    const char *remote_dir = getenv("DSCO_LLAMACPP_REMOTE_DIR");
    if (!remote_dir)
        remote_dir = "~/dsco-llama";

    char *dup = strdup(peers_csv);
    if (!dup)
        return 0;
    int n = 0;
    for (char *t = strtok(dup, ","); t; t = strtok(NULL, ",")) {
        char host[160] = "";
        int p = port;
        if (strchr(t, ':')) {
            char *colon = strrchr(t, ':');
            *colon = '\0';
            snprintf(host, sizeof(host), "%s", t);
            p = atoi(colon + 1);
            if (p <= 0)
                p = port;
        } else {
            char user[128] = "", addr[128] = "";
            if (!dsco_fleet_resolve(t, user, sizeof(user), addr, sizeof(addr)) || !addr[0])
                continue;
            snprintf(host, sizeof(host), "%s", addr);
            if (ensure) {
                const char *u = user[0] ? user : "agent";
                char ssh[1400];
                snprintf(
                    ssh, sizeof(ssh),
                    "ssh -o BatchMode=yes -o ConnectTimeout=8 %s@%s "
                    "'pgrep -f ggml-rpc-server >/dev/null || (cd %s && " DSCO_DEV_DETECT
                    "DYLD_LIBRARY_PATH=$PWD LD_LIBRARY_PATH=$PWD "
                    "nohup ./ggml-rpc-server -H 0.0.0.0 -p %d -d \"$D\" -c >/tmp/dsco_rpc.log 2>&1 "
                    "& sleep 3)' 2>/dev/null",
                    u, host, remote_dir, p);
                int r = system(ssh);
                (void)r;
            }
        }
        if (!host[0])
            continue;
        char entry[200];
        snprintf(entry, sizeof(entry), "%s%s:%d", (out && out[0]) ? "," : "", host, p);
        if (out)
            strncat(out, entry, outlen - strlen(out) - 1);
        n++;
    }
    free(dup);
    return n;
}

int dsco_cluster_cli(int argc, char **argv) {
    if (argc >= 3 && strcmp(argv[2], "run") == 0)
        return cluster_run(argc, argv);
    if (argc >= 3 && strcmp(argv[2], "deploy") == 0)
        return cluster_deploy(argc, argv);
    int plan = (argc >= 3 && strcmp(argv[2], "plan") == 0);

    printf("\033[1mcluster inventory\033[0m (live topology · local + ~/bridge/fleet)\n");
    printf("  %-12s %-16s %-8s %5s  %8s  %-6s %s\n", "NODE", "ADDR", "ARCH", "CORES", "MEM",
           "ACCEL", "STATUS");

    cnode_t nodes[CLUSTER_MAX];
    int n = cluster_inventory(nodes, CLUSTER_MAX);
    int up = 0, cores = 0;
    double mem = 0, gpu_mem = 0;
    for (int i = 0; i < n; i++) {
        cnode_t *nd = &nodes[i];
        char mems[16];
        snprintf(mems, sizeof(mems), "%.0f GB", nd->mem_bytes / GIB);
        printf("  %-12s %-16s %-8s %5d  %8s  %-6s %s%s\033[0m%s\n", nd->id, nd->addr,
               nd->arch[0] ? nd->arch : "?", nd->cores, nd->mem_bytes ? mems : "-", nd->accel,
               nd->up ? "\033[32m" : "\033[31m", nd->up ? "up" : "down",
               nd->is_local ? " (local)" : "");
        if (nd->up) {
            up++;
            cores += nd->cores;
            mem += nd->mem_bytes / GIB;
            if (is_gpu(nd->accel))
                gpu_mem += nd->mem_bytes / GIB;
        }
    }
    printf("  ─────────────────────────────────────────────────────────────\n");
    printf("  %d node%s up · %d cores · %.0f GB aggregate mem · %.0f GB GPU-accelerated\n", up,
           up == 1 ? "" : "s", cores, mem, gpu_mem);

    if (!plan) {
        printf("\nplan a model:  dsco cluster plan --model llama3-70b   (or --bytes GB --layers N "
               "--quant 4|8|16)\n");
        return 0;
    }

    const char *model = NULL;
    double gb = 0;
    int layers = 0, quant = 16;
    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--model") == 0 && i + 1 < argc)
            model = argv[++i];
        else if (strcmp(argv[i], "--bytes") == 0 && i + 1 < argc)
            gb = atof(argv[++i]);
        else if (strcmp(argv[i], "--layers") == 0 && i + 1 < argc)
            layers = atoi(argv[++i]);
        else if (strcmp(argv[i], "--quant") == 0 && i + 1 < argc)
            quant = atoi(argv[++i]);
    }
    char label[96];
    if (model) {
        const model_preset_t *p = preset_find(model);
        if (!p) {
            printf("\nunknown model '%s'. presets:", model);
            for (size_t i = 0; i < sizeof(PRESETS) / sizeof(PRESETS[0]); i++)
                printf(" %s", PRESETS[i].name);
            printf("\n");
            return 2;
        }
        gb = p->gb_fp16;
        layers = p->layers;
        snprintf(label, sizeof(label), "%s", model);
    } else {
        snprintf(label, sizeof(label), "custom");
    }
    if (gb <= 0 || layers <= 0) {
        printf("\nspecify --model <name>, or --bytes <GB> --layers <N>.\n");
        return 2;
    }
    cluster_plan(nodes, n, gb * GIB, layers, quant, label);
    return 0;
}
