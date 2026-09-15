#include "directive_store.h"
#include "crypto.h"
#include "durable_agents.h"
#include "ipc.h"
#include "json_util.h"
#include "workspace.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define DIRECTIVE_MAX 16384
#define HISTORY_MAX 32

static char *s_prompt;

static void paths(char current[PATH_MAX], char history[PATH_MAX], char lock[PATH_MAX]) {
    const char *root = dsco_workspace_root();
    snprintf(current, PATH_MAX, "%s/runtime/agent-directive.md", root);
    snprintf(history, PATH_MAX, "%s/runtime/directive-history.jsonl", root);
    snprintf(lock, PATH_MAX, "%s/runtime/directive.lock", root);
}

static bool ensure_runtime_dir(void) {
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/runtime", dsco_workspace_root());
    if (mkdir(path, 0700) == 0 || errno == EEXIST) return true;
    return false;
}

static char *read_bounded(const char *path, size_t limit) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long n = ftell(f);
    if (n < 0 || (size_t)n > limit || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    char *buf = calloc((size_t)n + 1, 1);
    if (!buf) { fclose(f); return NULL; }
    if (n && fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); fclose(f); return NULL; }
    fclose(f);
    return buf;
}

static bool atomic_write(const char *path, const char *content) {
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s.tmp.%ld", path, (long)getpid());
    int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) return false;
    size_t n = strlen(content), off = 0;
    while (off < n) {
        ssize_t w = write(fd, content + off, n - off);
        if (w <= 0) { close(fd); unlink(tmp); return false; }
        off += (size_t)w;
    }
    bool ok = fsync(fd) == 0 && close(fd) == 0 && rename(tmp, path) == 0;
    if (!ok) unlink(tmp);
    return ok;
}

static void now_iso(char out[32]) {
    time_t now = time(NULL);
    struct tm tmv;
    gmtime_r(&now, &tmv);
    strftime(out, 32, "%Y-%m-%dT%H:%M:%SZ", &tmv);
}

static void version_of(const char *content, char out[65]) {
    sha256_hex((const uint8_t *)(content ? content : ""), content ? strlen(content) : 0, out);
}

static int lock_store(const char *lock_path) {
    if (!ensure_runtime_dir()) return -1;
    int fd = open(lock_path, O_RDWR | O_CREAT, 0600);
    if (fd < 0) return -1;
    struct flock lk = {.l_type = F_WRLCK, .l_whence = SEEK_SET};
    if (fcntl(fd, F_SETLKW, &lk) != 0) { close(fd); return -1; }
    return fd;
}

static bool append_history(const char *history, const char *action, const char *version,
                           const char *content, const char *reason) {
    FILE *f = fopen(history, "ab");
    if (!f) return false;
    char when[32]; now_iso(when);
    jbuf_t row; jbuf_init(&row, 256);
    jbuf_append(&row, "{\"timestamp\":"); jbuf_append_json_str(&row, when);
    jbuf_append(&row, ",\"action\":"); jbuf_append_json_str(&row, action);
    jbuf_append(&row, ",\"version\":"); jbuf_append_json_str(&row, version);
    jbuf_append(&row, ",\"reason\":"); jbuf_append_json_str(&row, reason ? reason : "");
    jbuf_append(&row, ",\"content\":"); jbuf_append_json_str(&row, content ? content : "");
    jbuf_append(&row, "}\n");
    bool ok = row.data && fwrite(row.data, 1, row.len, f) == row.len && fflush(f) == 0;
    if (ok) ok = fsync(fileno(f)) == 0;
    fclose(f); jbuf_free(&row);
    return ok;
}

const char *dsco_directive_prompt(void) {
    if (s_prompt) return s_prompt[0] ? s_prompt : NULL;
    char current[PATH_MAX], history[PATH_MAX], lock[PATH_MAX]; paths(current, history, lock);
    s_prompt = read_bounded(current, DIRECTIVE_MAX);
    if (!s_prompt) s_prompt = strdup("");
    return s_prompt && s_prompt[0] ? s_prompt : NULL;
}

void dsco_directive_prompt_invalidate(void) { free(s_prompt); s_prompt = NULL; }

bool dsco_directive_status(char *out, size_t out_len) {
    char current[PATH_MAX], history[PATH_MAX], lock[PATH_MAX]; paths(current, history, lock);
    char *content = read_bounded(current, DIRECTIVE_MAX);
    char version[65]; version_of(content ? content : "", version);
    jbuf_t b; jbuf_init(&b, 256);
    jbuf_appendf(&b, "{\"enabled\":%s,\"chars\":%zu,\"version\":", content && content[0] ? "true" : "false", content ? strlen(content) : 0U);
    jbuf_append_json_str(&b, version); jbuf_append(&b, ",\"path\":"); jbuf_append_json_str(&b, current);
    if (content && content[0]) { jbuf_append(&b, ",\"content\":"); jbuf_append_json_str(&b, content); }
    jbuf_append(&b, "}"); snprintf(out, out_len, "%s", b.data ? b.data : "{}");
    jbuf_free(&b); free(content); return true;
}

bool dsco_directive_set(const char *content, const char *reason, char *out, size_t out_len) {
    if (!content || !content[0] || strlen(content) > DIRECTIVE_MAX) {
        snprintf(out, out_len, "{\"error\":\"content must be 1..%d bytes\"}", DIRECTIVE_MAX); return false;
    }
    char current[PATH_MAX], history[PATH_MAX], lock[PATH_MAX]; paths(current, history, lock);
    int fd = lock_store(lock); if (fd < 0) { snprintf(out, out_len, "{\"error\":\"cannot lock directive store\"}"); return false; }
    char version[65]; version_of(content, version);
    bool ok = append_history(history, "set", version, content, reason) && atomic_write(current, content);
    struct flock unlock = {.l_type = F_UNLCK, .l_whence = SEEK_SET};
    (void)fcntl(fd, F_SETLK, &unlock); close(fd);
    if (!ok) { snprintf(out, out_len, "{\"error\":\"directive persistence failed\"}"); return false; }
    dsco_directive_prompt_invalidate(); dsco_workspace_prompt_invalidate();
    snprintf(out, out_len, "{\"status\":\"set\",\"version\":\"%.64s\",\"chars\":%zu}", version, strlen(content)); return true;
}

bool dsco_directive_clear(const char *reason, char *out, size_t out_len) {
    char current[PATH_MAX], history[PATH_MAX], lock[PATH_MAX]; paths(current, history, lock);
    int fd = lock_store(lock); if (fd < 0) { snprintf(out, out_len, "{\"error\":\"cannot lock directive store\"}"); return false; }
    char version[65]; version_of("", version);
    bool ok = append_history(history, "clear", version, "", reason) && atomic_write(current, "");
    struct flock unlock = {.l_type = F_UNLCK, .l_whence = SEEK_SET};
    (void)fcntl(fd, F_SETLK, &unlock); close(fd);
    if (!ok) { snprintf(out, out_len, "{\"error\":\"directive persistence failed\"}"); return false; }
    dsco_directive_prompt_invalidate(); dsco_workspace_prompt_invalidate();
    snprintf(out, out_len, "{\"status\":\"cleared\",\"version\":\"%.64s\"}", version); return true;
}

bool dsco_directive_history(char *out, size_t out_len) {
    char current[PATH_MAX], history[PATH_MAX], lock[PATH_MAX]; paths(current, history, lock);
    FILE *f = fopen(history, "rb");
    if (!f) { snprintf(out, out_len, "{\"entries\":[]}"); return true; }
    char *lines[HISTORY_MAX] = {0}; int count = 0, next = 0; char *line = NULL; size_t cap = 0;
    while (getline(&line, &cap, f) >= 0) { free(lines[next]); lines[next] = strdup(line); next = (next + 1) % HISTORY_MAX; if (count < HISTORY_MAX) count++; }
    free(line); fclose(f);
    jbuf_t b; jbuf_init(&b, 512); jbuf_append(&b, "{\"entries\":[");
    for (int i = 0; i < count; i++) { int idx = (next - count + i + HISTORY_MAX) % HISTORY_MAX; if (i) jbuf_append(&b, ","); size_t n = strlen(lines[idx]); while (n && (lines[idx][n-1]=='\n'||lines[idx][n-1]=='\r')) lines[idx][--n]='\0'; jbuf_append(&b, lines[idx]); free(lines[idx]); }
    jbuf_append(&b, "]}"); snprintf(out, out_len, "%s", b.data ? b.data : "{}"); jbuf_free(&b); return true;
}

bool dsco_directive_rollback(const char *version, const char *reason, char *out, size_t out_len) {
    if (!version || strlen(version) < 8) { snprintf(out, out_len, "{\"error\":\"rollback requires a version prefix (at least 8 chars)\"}"); return false; }
    char current[PATH_MAX], history[PATH_MAX], lock[PATH_MAX]; paths(current, history, lock);
    FILE *f = fopen(history, "rb"); if (!f) { snprintf(out, out_len, "{\"error\":\"no directive history\"}"); return false; }
    char *line = NULL, *found = NULL; size_t cap = 0;
    while (getline(&line, &cap, f) >= 0) { char *v = json_get_str(line, "version"); if (v && strncmp(v, version, strlen(version)) == 0) { char *c = json_get_str(line, "content"); free(found); found = c; } free(v); }
    free(line); fclose(f);
    if (!found) { snprintf(out, out_len, "{\"error\":\"version not found\"}"); return false; }
    bool ok = found[0] ? dsco_directive_set(found, reason ? reason : "rollback", out, out_len) : dsco_directive_clear(reason ? reason : "rollback", out, out_len);
    free(found); return ok;
}

/* ── standing-objective bridge ─────────────────────────────────────────── */

#define STANDING_AGENT_ID "standing-directive"

bool dsco_directive_objective(char *out, size_t out_len) {
    if (!out || out_len == 0) return false;
    out[0] = '\0';
    char current[PATH_MAX], history[PATH_MAX], lock[PATH_MAX]; paths(current, history, lock);
    char *content = read_bounded(current, DIRECTIVE_MAX);
    if (!content || !content[0]) { free(content); return false; }

    const char *start = NULL;
    const char *p = content;
    while ((p = strstr(p, "## ")) != NULL) {
        if (strncasecmp(p, "## Autonomous Objective", 23) == 0) { start = p; break; }
        p += 3;
    }
    const char *body = start ? start + strlen("## Autonomous Objective") : content;
    while (*body == ' ' || *body == '\n' || *body == '\r') body++;
    size_t n = 0;
    for (const char *q = body; *q; q++) {
        if (q[0] == '\n' && q[1] == '#' && q[2] == '#') break; /* next section */
        if (n + 1 < out_len) out[n++] = *q;
    }
    while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == ' ' || out[n - 1] == '\r')) n--;
    out[n] = '\0';
    free(content);
    return n > 0;
}

bool dsco_directive_standing_status(char *out, size_t out_len) {
    char objective[DIRECTIVE_MAX];
    bool has_objective = dsco_directive_objective(objective, sizeof(objective));
    jbuf_t b; jbuf_init(&b, 256);
    jbuf_append(&b, "{\"agent_id\":"); jbuf_append_json_str(&b, STANDING_AGENT_ID);
    jbuf_appendf(&b, ",\"objective_present\":%s", has_objective ? "true" : "false");
    if (has_objective) {
        jbuf_append(&b, ",\"objective\":");
        jbuf_append_json_str(&b, objective);
    }
    jbuf_append(&b, "}");
    snprintf(out, out_len, "%s", b.data ? b.data : "{}");
    jbuf_free(&b);
    return true;
}

bool dsco_directive_standing_deploy(const char *program, const char *model,
                                    double budget_usd, char *out, size_t out_len) {
    char objective[DIRECTIVE_MAX];
    if (!dsco_directive_objective(objective, sizeof(objective)) || !objective[0]) {
        snprintf(out, out_len,
                 "{\"error\":\"no Autonomous Objective section in persistent directive "
                 "(add '## Autonomous Objective' to it via persistent_directive action=set)\"}");
        return false;
    }
    if (!program || !program[0]) {
        snprintf(out, out_len, "{\"error\":\"runtime program path unavailable\"}");
        return false;
    }
    /* Bounded authority: the detached activation is cost-capped and refuses
     * remote spend without an explicit operator env override (enforced inside
     * `agents activate`). It inherits only the objective text, never secrets. */
    char db[PATH_MAX];
    durable_agents_default_db_path(db, sizeof(db));
    if (!ipc_init(db, STANDING_AGENT_ID)) {
        snprintf(out, out_len, "{\"error\":\"could not open durable agents bus\"}");
        return false;
    }
    bool exists = ipc_get_agent(STANDING_AGENT_ID, &(ipc_agent_info_t){0});
    if (!exists) {
        ipc_agent_binding_t binding = {0};
        binding.budget_usd = budget_usd > 0 ? budget_usd : 5.0;
        if (!ipc_agent_define_bound(STANDING_AGENT_ID, NULL, 0, "standing-objective",
                                    model && model[0] ? model : "claude-sonnet-5", "*",
                                    &binding)) {
            ipc_shutdown();
            snprintf(out, out_len, "{\"error\":\"failed to define standing agent\"}");
            return false;
        }
    }
    int task_id = ipc_task_submit_to(STANDING_AGENT_ID, objective, 5, 0);
    ipc_shutdown();
    if (task_id < 1) {
        snprintf(out, out_len, "{\"error\":\"failed to queue standing objective\"}");
        return false;
    }
    if (!durable_agents_wake(program, STANDING_AGENT_ID, task_id, objective)) {
        snprintf(out, out_len,
                 "{\"error\":\"wake refused (DSCO_DURABLE_AUTOWAKE=0 or fork failure)\","
                 "\"task_id\":%d}", task_id);
        return false;
    }
    jbuf_t b; jbuf_init(&b, 256);
    jbuf_appendf(&b, "{\"status\":\"deployed\",\"agent_id\":\"%s\",\"task_id\":%d,"
                     "\"objective_chars\":%zu,\"budget_usd\":%.2f}",
                 STANDING_AGENT_ID, task_id, strlen(objective),
                 budget_usd > 0 ? budget_usd : 5.0);
    snprintf(out, out_len, "%s", b.data ? b.data : "{}");
    jbuf_free(&b);
    return true;
}
