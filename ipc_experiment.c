/*
 * ipc_experiment.c — Spin up multiple dsco-like IPC-wrapped processes and
 * observe emergent behavior through the shared SQLite IPC bus.
 *
 * Build:
 *   cc -O2 -std=c11 -D_POSIX_C_SOURCE=200809L -Iinclude \
 *      ipc_experiment.c src/ipc.c src/event_loop.c src/json_util.c \
 *      src/arena_alloc.c src/error.c src/trace.c \
 *      -o ipc_experiment -lsqlite3 -lm -lcurl
 *
 * Run:
 *   ./ipc_experiment [num_workers] [num_rounds]
 */

#include "ipc.h"
#include "json_util.h"
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <time.h>
#include <errno.h>

/* ── Helpers ──────────────────────────────────────────────────────────── */

static double now_sec(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec / 1e6;
}

static volatile sig_atomic_t g_stop = 0;
static void sig_handler(int sig) { (void)sig; g_stop = 1; }

/* ── Color output ─────────────────────────────────────────────────────── */

static const char *COLORS[] = {
    "\033[31m", "\033[32m", "\033[33m", "\033[34m",
    "\033[35m", "\033[36m", "\033[91m", "\033[92m",
    "\033[93m", "\033[94m", "\033[95m", "\033[96m",
};
#define N_COLORS (sizeof(COLORS)/sizeof(COLORS[0]))
#define RST  "\033[0m"
#define DIM  "\033[2m"
#define BLD  "\033[1m"

#define WLOG(wid, fmt, ...) \
    fprintf(stderr, "%s[W%d]%s " fmt "\n", \
            COLORS[(wid) % N_COLORS], (wid), RST, ##__VA_ARGS__)

/* ── Task types ───────────────────────────────────────────────────────── */

typedef enum {
    TASK_FIBONACCI, TASK_PRIME_CHECK, TASK_WORD_COUNT, TASK_ECHO,
    TASK_CHAIN, TASK_VOTE, TASK_AGGREGATE, TASK_BROADCAST,
    TASK_COUNT
} task_type_t;

/* ── Task execution ───────────────────────────────────────────────────── */

static long fibonacci(int n) {
    if (n <= 1) return n;
    long a = 0, b = 1;
    for (int i = 2; i <= n; i++) { long t = a + b; a = b; b = t; }
    return b;
}

static int is_prime(long n) {
    if (n < 2) return 0;
    if (n < 4) return 1;
    if (n % 2 == 0 || n % 3 == 0) return 0;
    for (long i = 5; i * i <= n; i += 6)
        if (n % i == 0 || n % (i+2) == 0) return 0;
    return 1;
}

static char *execute_task(int wid, const char *desc) {
    static char result[4096];
    char type[64] = {0}, arg[1024] = {0};
    const char *colon = strchr(desc, ':');
    if (colon) {
        size_t tl = (size_t)(colon - desc);
        if (tl >= sizeof(type)) tl = sizeof(type) - 1;
        memcpy(type, desc, tl);
        snprintf(arg, sizeof(arg), "%s", colon + 1);
    } else {
        snprintf(type, sizeof(type), "%s", desc);
    }

    if (strcmp(type, "fibonacci") == 0) {
        int n = atoi(arg); if (n < 0) n = 0; if (n > 90) n = 90;
        snprintf(result, sizeof(result), "fib(%d) = %ld", n, fibonacci(n));
        WLOG(wid, "  🔢 %s", result);
    } else if (strcmp(type, "prime_check") == 0) {
        long n = atol(arg);
        snprintf(result, sizeof(result), "%ld is %s", n, is_prime(n) ? "prime" : "composite");
        WLOG(wid, "  🔢 %s", result);
    } else if (strcmp(type, "word_count") == 0) {
        int w = 0, in = 0;
        for (const char *c = arg; *c; c++) {
            if (*c == ' ' || *c == '\t' || *c == '\n') in = 0;
            else if (!in) { in = 1; w++; }
        }
        snprintf(result, sizeof(result), "words=%d", w);
        WLOG(wid, "  📝 counted %d words", w);
    } else if (strcmp(type, "echo") == 0) {
        snprintf(result, sizeof(result), "echo: %s", arg);
    } else if (strcmp(type, "chain") == 0) {
        int depth = atoi(arg);
        if (depth > 0) {
            char nd[256];
            snprintf(nd, sizeof(nd), "chain:%d", depth - 1);
            int nid = ipc_task_submit(nd, depth, 0);
            WLOG(wid, "  %s⛓ CHAIN CASCADE%s → task %d (remaining=%d)", BLD, RST, nid, depth-1);
            snprintf(result, sizeof(result), "chain → spawned task %d (depth %d→%d)", nid, depth, depth-1);
        } else {
            WLOG(wid, "  %s⛓ CHAIN TERMINUS%s — reached depth 0", BLD, RST);
            snprintf(result, sizeof(result), "chain complete at depth 0");
        }
    } else if (strcmp(type, "vote") == 0) {
        char vk[128];
        snprintf(vk, sizeof(vk), "vote:%s", ipc_self_id());
        ipc_scratch_put(vk, arg);
        snprintf(result, sizeof(result), "voted: %s", arg);
        WLOG(wid, "  🗳 voted: %s", arg);
    } else if (strcmp(type, "aggregate") == 0) {
        char keys[64][IPC_MAX_KEY];
        int n = ipc_scratch_keys("vote:", keys, 64);
        int yes=0, no=0, other=0;
        for (int i = 0; i < n; i++) {
            char *v = ipc_scratch_get(keys[i]);
            if (v) { if (strcmp(v,"yes")==0) yes++; else if (strcmp(v,"no")==0) no++; else other++; free(v); }
        }
        const char *verdict = yes > no ? "APPROVED" : (no > yes ? "REJECTED" : "TIE");
        snprintf(result, sizeof(result), "%d votes: yes=%d no=%d other=%d → %s", n, yes, no, other, verdict);
        WLOG(wid, "  %s📊 CONSENSUS%s: %s", BLD, RST, result);
        ipc_scratch_put("consensus_result", result);
    } else if (strcmp(type, "broadcast") == 0) {
        ipc_agent_info_t agents[32];
        int n = ipc_list_agents(agents, 32);
        int sent = 0;
        for (int i = 0; i < n; i++) {
            if (strcmp(agents[i].id, ipc_self_id()) != 0 &&
                agents[i].status != IPC_AGENT_DONE && agents[i].status != IPC_AGENT_DEAD) {
                ipc_send(agents[i].id, "broadcast", arg);
                sent++;
            }
        }
        snprintf(result, sizeof(result), "broadcast to %d agents", sent);
        WLOG(wid, "  📡 broadcast to %d agents", sent);
    } else {
        snprintf(result, sizeof(result), "unknown: %s", desc);
    }

    usleep(30000 + (rand() % 80000));  /* 30-110ms work time */
    return result;
}

/* ── Worker process (runs after execvp-like fresh start) ──────────────── */

static void worker_main(int wid, const char *db_path, int rounds) {
    srand((unsigned)(time(NULL) ^ getpid() ^ wid));

    char agent_id[64];
    snprintf(agent_id, sizeof(agent_id), "w%d-%d", wid, getpid());

    /* Force-initialize IPC fresh (we're a new process context after fork) */
    setenv("DSCO_IPC_DB", db_path, 1);
    if (!ipc_init(db_path, agent_id)) {
        fprintf(stderr, "worker %d: ipc_init failed for %s\n", wid, db_path);
        _exit(1);
    }

    char role[32];
    snprintf(role, sizeof(role), "worker-%d", wid);
    ipc_register("coordinator", 1, role, "*");
    ipc_set_status(IPC_AGENT_IDLE, "ready");

    WLOG(wid, "🟢 registered as %s%s%s (pid=%d)", BLD, agent_id, RST, getpid());

    int completed = 0, submitted = 0, msgs_in = 0;
    double work_ms = 0;

    for (int r = 0; r < rounds && !g_stop; r++) {
        ipc_heartbeat();

        /* Check messages */
        ipc_message_t msgs[8];
        int nm = ipc_recv(msgs, 8);
        for (int i = 0; i < nm; i++) {
            WLOG(wid, "📨 from %s [%s]: %.50s",
                 msgs[i].from_agent, msgs[i].topic, msgs[i].body ? msgs[i].body : "");
            msgs_in++;
            if (strcmp(msgs[i].topic, "broadcast") == 0 && rand() % 4 == 0) {
                char rd[128];
                snprintf(rd, sizeof(rd), "echo:ack from %s round %d", agent_id, r);
                ipc_task_submit(rd, 1, 0);
                submitted++;
            }
            free(msgs[i].body);
        }

        /* Claim and execute task */
        ipc_task_t task;
        if (ipc_task_claim(&task)) {
            double t0 = now_sec();
            ipc_set_status(IPC_AGENT_WORKING, task.description);
            ipc_task_start(task.id);

            WLOG(wid, "⚡ task %d: %.50s (pri=%d from=%s)",
                 task.id, task.description, task.priority, task.created_by);

            char *res = execute_task(wid, task.description);
            ipc_task_complete(task.id, res);
            ipc_set_status(IPC_AGENT_IDLE, "");

            double el = (now_sec() - t0) * 1000;
            work_ms += el;
            completed++;
            WLOG(wid, "✅ task %d done (%.0fms)", task.id, el);
        }

        /* Maybe submit new work */
        if (rand() % 3 == 0) {
            task_type_t tt = rand() % TASK_COUNT;
            char desc[256];
            switch (tt) {
                case TASK_FIBONACCI:   snprintf(desc, sizeof(desc), "fibonacci:%d", 10+rand()%80); break;
                case TASK_PRIME_CHECK: snprintf(desc, sizeof(desc), "prime_check:%ld", (long)(rand()%100000)+2); break;
                case TASK_WORD_COUNT:  snprintf(desc, sizeof(desc), "word_count:round %d worker %d data", r, wid); break;
                case TASK_ECHO:        snprintf(desc, sizeof(desc), "echo:w%d r%d", wid, r); break;
                case TASK_CHAIN:       snprintf(desc, sizeof(desc), "chain:%d", 1+rand()%4); break;
                case TASK_VOTE:        snprintf(desc, sizeof(desc), "vote:%s", (rand()%2)?"yes":"no"); break;
                case TASK_AGGREGATE:   snprintf(desc, sizeof(desc), "aggregate:all"); break;
                case TASK_BROADCAST:   snprintf(desc, sizeof(desc), "broadcast:w%d says hi at r%d", wid, r); break;
                default:               snprintf(desc, sizeof(desc), "echo:default"); break;
            }
            int pri = (tt == TASK_AGGREGATE) ? 10 : (1+rand()%5);
            int tid = ipc_task_submit(desc, pri, 0);
            if (tid >= 0) { submitted++; }
        }

        /* Periodic stats to scratchpad */
        if (r % 5 == 0) {
            char k[64], v[256];
            snprintf(k, sizeof(k), "stats:%s", agent_id);
            snprintf(v, sizeof(v), "{\"done\":%d,\"sub\":%d,\"msgs\":%d,\"r\":%d}", completed, submitted, msgs_in, r);
            ipc_scratch_put(k, v);
        }

        usleep(80000 + (rand() % 150000));  /* 80-230ms between rounds */
    }

    char k[64], v[512];
    snprintf(k, sizeof(k), "final:%s", agent_id);
    snprintf(v, sizeof(v), "{\"w\":%d,\"done\":%d,\"sub\":%d,\"msgs\":%d,\"work_ms\":%.0f}",
             wid, completed, submitted, msgs_in, work_ms);
    ipc_scratch_put(k, v);

    ipc_set_status(IPC_AGENT_DONE, "finished");
    WLOG(wid, "%s═══ DONE ══=%s done=%d sub=%d msgs=%d work=%.0fms",
         BLD, RST, completed, submitted, msgs_in, work_ms);
    ipc_shutdown();
    _exit(0);
}

/* ── Print final IPC bus state ────────────────────────────────────────── */

static void print_ipc_status(const char *db_path) {
    sqlite3 *db;
    if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) return;
    sqlite3_exec(db, "PRAGMA journal_mode=WAL", NULL, NULL, NULL);

    fprintf(stderr, "\n%s╔══════════════════════════════════════════════════════════════════╗%s\n", BLD, RST);
    fprintf(stderr, "%s║                     IPC BUS STATE SNAPSHOT                      ║%s\n", BLD, RST);
    fprintf(stderr, "%s╚══════════════════════════════════════════════════════════════════╝%s\n\n", BLD, RST);

    /* Agents */
    fprintf(stderr, "%s── Agents ──────────────────────────────────────────────────────────%s\n", DIM, RST);
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db, "SELECT id, pid, depth, status, role, current_task FROM agents ORDER BY depth, id", -1, &st, NULL) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            const char *s = (const char *)sqlite3_column_text(st, 3);
            const char *c = "\033[37m";
            if (s) { if (strcmp(s,"working")==0) c="\033[33m"; else if (strcmp(s,"idle")==0) c="\033[32m";
                      else if (strcmp(s,"done")==0) c="\033[36m"; else if (strcmp(s,"dead")==0||strcmp(s,"error")==0) c="\033[31m"; }
            fprintf(stderr, "  %s%-24s%s pid=%-6d d=%d %s%-8s%s task=%.30s\n",
                    BLD, sqlite3_column_text(st,0), RST, sqlite3_column_int(st,1),
                    sqlite3_column_int(st,2), c, s?s:"?", RST,
                    sqlite3_column_text(st,5)?(const char*)sqlite3_column_text(st,5):"");
        }
        sqlite3_finalize(st);
    }

    /* Task summary */
    fprintf(stderr, "\n%s── Tasks ───────────────────────────────────────────────────────────%s\n", DIM, RST);
    if (sqlite3_prepare_v2(db, "SELECT status, COUNT(*) FROM tasks GROUP BY status ORDER BY status", -1, &st, NULL) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW)
            fprintf(stderr, "  %-12s %d\n", (const char*)sqlite3_column_text(st,0), sqlite3_column_int(st,1));
        sqlite3_finalize(st);
    }
    int total = 0;
    if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM tasks", -1, &st, NULL) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW) total = sqlite3_column_int(st, 0);
        sqlite3_finalize(st);
    }
    fprintf(stderr, "  %-12s %d\n", "TOTAL", total);

    /* Messages */
    fprintf(stderr, "\n%s── Messages ────────────────────────────────────────────────────────%s\n", DIM, RST);
    int tm = 0, ur = 0;
    if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM messages", -1, &st, NULL) == SQLITE_OK) {
        if (sqlite3_step(st)==SQLITE_ROW) tm = sqlite3_column_int(st,0); sqlite3_finalize(st); }
    if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM messages WHERE read_at IS NULL", -1, &st, NULL) == SQLITE_OK) {
        if (sqlite3_step(st)==SQLITE_ROW) ur = sqlite3_column_int(st,0); sqlite3_finalize(st); }
    fprintf(stderr, "  total: %d  unread: %d\n", tm, ur);

    /* Scratchpad */
    fprintf(stderr, "\n%s── Scratchpad ──────────────────────────────────────────────────────%s\n", DIM, RST);
    if (sqlite3_prepare_v2(db, "SELECT key, value FROM scratchpad ORDER BY key LIMIT 30", -1, &st, NULL) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            fprintf(stderr, "  %-34s = %.50s\n",
                    (const char*)sqlite3_column_text(st,0),
                    sqlite3_column_text(st,1)?(const char*)sqlite3_column_text(st,1):"");
        }
        sqlite3_finalize(st);
    }

    /* Emergent patterns */
    fprintf(stderr, "\n%s── Emergent Patterns ───────────────────────────────────────────────%s\n", DIM, RST);

    int chains = 0;
    if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM tasks WHERE description LIKE 'chain:%'", -1, &st, NULL) == SQLITE_OK) {
        if (sqlite3_step(st)==SQLITE_ROW) chains = sqlite3_column_int(st,0); sqlite3_finalize(st); }
    fprintf(stderr, "  Chain cascade tasks spawned: %s%d%s\n", BLD, chains, RST);

    int votes = 0;
    if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM scratchpad WHERE key LIKE 'vote:%'", -1, &st, NULL) == SQLITE_OK) {
        if (sqlite3_step(st)==SQLITE_ROW) votes = sqlite3_column_int(st,0); sqlite3_finalize(st); }
    fprintf(stderr, "  Votes cast in scratchpad: %s%d%s\n", BLD, votes, RST);

    if (sqlite3_prepare_v2(db, "SELECT value FROM scratchpad WHERE key='consensus_result'", -1, &st, NULL) == SQLITE_OK) {
        if (sqlite3_step(st)==SQLITE_ROW)
            fprintf(stderr, "  Consensus result: %s%s%s\n", BLD, (const char*)sqlite3_column_text(st,0), RST);
        sqlite3_finalize(st);
    }

    /* Task completion distribution */
    fprintf(stderr, "\n  Task completion by worker:\n");
    if (sqlite3_prepare_v2(db, "SELECT assigned_to, COUNT(*) as c FROM tasks WHERE status='done' GROUP BY assigned_to ORDER BY c DESC", -1, &st, NULL) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            const char *a = (const char*)sqlite3_column_text(st,0);
            int c = sqlite3_column_int(st,1);
            fprintf(stderr, "    %-24s ", a?a:"(none)");
            for (int i = 0; i < c && i < 50; i++) fprintf(stderr, "█");
            fprintf(stderr, " %d\n", c);
        }
        sqlite3_finalize(st);
    }

    int cross = 0;
    if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM tasks WHERE status='done' AND created_by!=assigned_to AND created_by!='' AND assigned_to!=''", -1, &st, NULL) == SQLITE_OK) {
        if (sqlite3_step(st)==SQLITE_ROW) cross = sqlite3_column_int(st,0); sqlite3_finalize(st); }
    fprintf(stderr, "\n  %sCross-agent completions: %d%s (created by one agent, done by another)\n", BLD, cross, RST);

    /* Who-created-for-whom matrix */
    fprintf(stderr, "\n  Task flow (creator → executor):\n");
    if (sqlite3_prepare_v2(db, "SELECT created_by, assigned_to, COUNT(*) FROM tasks WHERE status='done' GROUP BY created_by, assigned_to ORDER BY COUNT(*) DESC LIMIT 15", -1, &st, NULL) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            fprintf(stderr, "    %s → %s  (%d tasks)\n",
                    (const char*)sqlite3_column_text(st,0),
                    (const char*)sqlite3_column_text(st,1),
                    sqlite3_column_int(st,2));
        }
        sqlite3_finalize(st);
    }

    fprintf(stderr, "\n%s══════════════════════════════════════════════════════════════════════%s\n\n", DIM, RST);
    sqlite3_close(db);
}

/* ── Main ─────────────────────────────────────────────────────────────── */

int main(int argc, char **argv) {
    int nw = 6, nr = 30;
    if (argc > 1) nw = atoi(argv[1]);
    if (argc > 2) nr = atoi(argv[2]);
    if (nw < 1) nw = 1; if (nw > 16) nw = 16;
    if (nr < 1) nr = 1; if (nr > 200) nr = 200;

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    char db_path[256];
    snprintf(db_path, sizeof(db_path), "/tmp/dsco_ipc_experiment_%d.db", getpid());

    /* Remove stale DB */
    unlink(db_path);
    { char w[300], s[300];
      snprintf(w, sizeof(w), "%s-wal", db_path); unlink(w);
      snprintf(s, sizeof(s), "%s-shm", db_path); unlink(s); }

    fprintf(stderr, "\n%s╔══════════════════════════════════════════════════════════════════╗%s\n", BLD, RST);
    fprintf(stderr, "%s║             DSCO IPC EMERGENT BEHAVIOR EXPERIMENT                ║%s\n", BLD, RST);
    fprintf(stderr, "%s╠══════════════════════════════════════════════════════════════════╣%s\n", BLD, RST);
    fprintf(stderr, "%s║%s  Workers: %-4d  Rounds: %-4d                                    %s║%s\n", BLD, RST, nw, nr, BLD, RST);
    fprintf(stderr, "%s║%s  DB: %-57s%s║%s\n", BLD, RST, db_path, BLD, RST);
    fprintf(stderr, "%s╚══════════════════════════════════════════════════════════════════╝%s\n\n", BLD, RST);

    /* Initialize IPC as coordinator BEFORE forking — but workers will re-init */
    {
        /* Create the DB and schema via direct sqlite3 call */
        sqlite3 *db;
        sqlite3_open(db_path, &db);
        sqlite3_exec(db, "PRAGMA journal_mode=WAL", NULL, NULL, NULL);
        sqlite3_exec(db, "PRAGMA busy_timeout=5000", NULL, NULL, NULL);
        const char *schema =
            "CREATE TABLE IF NOT EXISTS agents (id TEXT PRIMARY KEY, parent_id TEXT, pid INTEGER, depth INTEGER DEFAULT 0, status TEXT DEFAULT 'starting', role TEXT DEFAULT '', current_task TEXT DEFAULT '', toolkit TEXT DEFAULT '*', started_at REAL, last_heartbeat REAL);"
            "CREATE TABLE IF NOT EXISTS messages (id INTEGER PRIMARY KEY AUTOINCREMENT, from_agent TEXT, to_agent TEXT, topic TEXT DEFAULT '', body TEXT DEFAULT '', created_at REAL, read_at REAL);"
            "CREATE INDEX IF NOT EXISTS idx_msg_to ON messages(to_agent, read_at);"
            "CREATE INDEX IF NOT EXISTS idx_msg_topic ON messages(topic);"
            "CREATE TABLE IF NOT EXISTS tasks (id INTEGER PRIMARY KEY AUTOINCREMENT, assigned_to TEXT, created_by TEXT, parent_task_id INTEGER DEFAULT 0, priority INTEGER DEFAULT 0, status TEXT DEFAULT 'pending', description TEXT DEFAULT '', result TEXT, created_at REAL, started_at REAL, completed_at REAL);"
            "CREATE INDEX IF NOT EXISTS idx_task_status ON tasks(status, priority DESC);"
            "CREATE TABLE IF NOT EXISTS scratchpad (key TEXT PRIMARY KEY, value TEXT, agent_id TEXT, updated_at REAL);";
        sqlite3_exec(db, schema, NULL, NULL, NULL);

        /* Seed tasks directly via SQL */
        fprintf(stderr, "%s── Seeding initial tasks ───────────────────────────────────────────%s\n", DIM, RST);
        const char *seeds[] = {
            "fibonacci:20", "fibonacci:45", "fibonacci:78",
            "prime_check:997", "prime_check:99991", "prime_check:7919",
            "word_count:emergent behavior arises from simple rules interacting",
            "chain:5", "chain:3", "chain:4",
            "vote:yes", "vote:no", "vote:yes", "vote:abstain",
            "broadcast:experiment starting!",
            "aggregate:all",
            NULL
        };
        int seed_count = 0;
        for (int i = 0; seeds[i]; i++) {
            char sql[512];
            snprintf(sql, sizeof(sql),
                "INSERT INTO tasks (created_by, priority, status, description, created_at) VALUES ('coordinator', %d, 'pending', '%s', %.6f)",
                (seeds[i][0]=='c' ? 7 : (seeds[i][0]=='a' ? 10 : (seeds[i][0]=='b' ? 8 : 3))),
                seeds[i], now_sec());
            sqlite3_exec(db, sql, NULL, NULL, NULL);
            seed_count++;
        }
        /* Register coordinator */
        {
            char sql[512];
            snprintf(sql, sizeof(sql),
                "INSERT INTO agents (id, pid, depth, status, role, started_at, last_heartbeat) VALUES ('coordinator', %d, 0, 'working', 'coordinator', %.6f, %.6f)",
                getpid(), now_sec(), now_sec());
            sqlite3_exec(db, sql, NULL, NULL, NULL);
        }
        sqlite3_close(db);
        fprintf(stderr, "  Seeded %d tasks (3 chain cascades, 4 votes, 1 broadcast, 1 aggregate)\n\n", seed_count);
    }

    /* Spawn workers — each does its own ipc_init in fresh process */
    fprintf(stderr, "%s── Spawning %d workers ─────────────────────────────────────────────%s\n", DIM, nw, RST);
    pid_t pids[16] = {0};
    for (int i = 0; i < nw; i++) {
        pid_t p = fork();
        if (p < 0) { perror("fork"); continue; }
        if (p == 0) { worker_main(i, db_path, nr); _exit(0); }
        pids[i] = p;
        fprintf(stderr, "  worker %d → pid %d\n", i, p);
    }

    fprintf(stderr, "\n%s── Workers running ─────────────────────────────────────────────────%s\n\n", DIM, RST);

    /* Coordinator loop: inject work, monitor */
    double start = now_sec();
    int alive = nw, tick = 0;

    while (alive > 0 && !g_stop) {
        int status;
        pid_t w;
        while ((w = waitpid(-1, &status, WNOHANG)) > 0) {
            for (int i = 0; i < nw; i++) {
                if (pids[i] == w) { pids[i] = 0; alive--; break; }
            }
        }

        /* Inject more work periodically */
        if (tick % 4 == 2 && alive > 0) {
            sqlite3 *db;
            if (sqlite3_open(db_path, &db) == SQLITE_OK) {
                sqlite3_exec(db, "PRAGMA journal_mode=WAL", NULL, NULL, NULL);
                sqlite3_exec(db, "PRAGMA busy_timeout=5000", NULL, NULL, NULL);
                char sql[256];
                snprintf(sql, sizeof(sql),
                    "INSERT INTO tasks (created_by, priority, status, description, created_at) VALUES ('coordinator', 2, 'pending', 'fibonacci:%d', %.6f)",
                    20+rand()%70, now_sec());
                sqlite3_exec(db, sql, NULL, NULL, NULL);
                if (tick % 8 == 2) {
                    snprintf(sql, sizeof(sql),
                        "INSERT INTO tasks (created_by, priority, status, description, created_at) VALUES ('coordinator', 10, 'pending', 'aggregate:all', %.6f)", now_sec());
                    sqlite3_exec(db, sql, NULL, NULL, NULL);
                }
                sqlite3_close(db);
            }
        }
        tick++;
        usleep(400000);
    }

    for (int i = 0; i < nw; i++) if (pids[i]>0) waitpid(pids[i], NULL, 0);

    double elapsed = now_sec() - start;

    /* Final dump */
    print_ipc_status(db_path);

    fprintf(stderr, "%s╔══════════════════════════════════════════════════════════════════╗%s\n", BLD, RST);
    fprintf(stderr, "%s║                     EXPERIMENT COMPLETE                          ║%s\n", BLD, RST);
    fprintf(stderr, "%s╠══════════════════════════════════════════════════════════════════╣%s\n", BLD, RST);
    fprintf(stderr, "%s║%s  Duration: %.1fs  Workers: %d  Rounds: %d                        %s║%s\n", BLD, RST, elapsed, nw, nr, BLD, RST);
    fprintf(stderr, "%s╚══════════════════════════════════════════════════════════════════╝%s\n\n", BLD, RST);
    fprintf(stderr, "  DB: %s\n  sqlite3 %s\n\n", db_path, db_path);

    return 0;
}
