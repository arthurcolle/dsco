#include "mcp.h"
#include "json_util.h"
#include "config.h"
#include "mcp_names.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/time.h>
#include <pthread.h>
#include <curl/curl.h>

/* When set, suppress connection/discovery progress lines. The TUI calls this
 * before running mcp_init on a background thread so output doesn't smear
 * over the input panel rows. */
static volatile int g_mcp_silent = 0;
void mcp_set_silent(bool silent) { g_mcp_silent = silent ? 1 : 0; }

#define MCP_LOG(...) do { if (!g_mcp_silent) fprintf(stderr, __VA_ARGS__); } while (0)

/* ── Small helpers ─────────────────────────────────────────────────────── */

static double mcp_now_sec(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec / 1e6;
}

static bool starts_http(const char *s) {
    return s && (strncmp(s, "http://", 7) == 0 ||
                 strncmp(s, "https://", 8) == 0);
}

static int mcp_timeout_ms(int def_ms) {
    const char *env = getenv("DSCO_MCP_TIMEOUT_MS");
    if (!env || !*env) return def_ms;
    char *end = NULL;
    long v = strtol(env, &end, 10);
    if (end == env || v <= 0) return def_ms;
    if (v < 250) v = 250;
    if (v > 120000) v = 120000;
    return (int)v;
}

static char *trim_inplace(char *s) {
    while (*s && (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')) s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\n' || e[-1] == '\r'))
        *--e = '\0';
    return s;
}

static void copy_str(char *dst, size_t dst_len, const char *src) {
    if (!dst || dst_len == 0) return;
    if (!src) src = "";
    snprintf(dst, dst_len, "%s", src);
}

static void sanitize_name(const char *in, char *out, size_t out_len) {
    dsco_mcp_normalize_name(in, out, out_len);
}

static void normalize_http_url(const char *raw, char *out, size_t out_len) {
    copy_str(out, out_len, raw);
    size_t n = strlen(out);
    if (n >= 4 && strcmp(out + n - 4, "/sse") == 0) {
        out[n - 4] = '\0';
    }
}

static char *read_file_limited(const char *path, size_t max_size) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz <= 0 || (size_t)sz > max_size) { fclose(f); return NULL; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    char *data = safe_malloc((size_t)sz + 1);
    size_t nr = fread(data, 1, (size_t)sz, f);
    data[nr] = '\0';
    fclose(f);
    return data;
}

/* ── JSON-RPC helpers ──────────────────────────────────────────────────── */

static char *rpc_build(int id, const char *method, const char *params) {
    jbuf_t b;
    jbuf_init(&b, 1024);
    jbuf_append(&b, "{\"jsonrpc\":\"2.0\"");
    if (id > 0) {
        jbuf_append(&b, ",\"id\":");
        jbuf_append_int(&b, id);
    }
    jbuf_append(&b, ",\"method\":");
    jbuf_append_json_str(&b, method);
    if (params) {
        jbuf_append(&b, ",\"params\":");
        jbuf_append(&b, params);
    }
    jbuf_append(&b, "}\n");
    return b.data;
}

static const char *skip_ws_local(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

static const char *skip_json_string_local(const char *p) {
    if (*p != '"') return p;
    p++;
    while (*p) {
        if (*p == '\\' && p[1]) { p += 2; continue; }
        if (*p == '"') return p + 1;
        p++;
    }
    return p;
}

static const char *skip_json_value_local(const char *p) {
    p = skip_ws_local(p);
    if (*p == '"') return skip_json_string_local(p);
    if (*p == '{' || *p == '[') {
        char open = *p;
        char close = open == '{' ? '}' : ']';
        int depth = 1;
        p++;
        while (*p && depth > 0) {
            if (*p == '"') {
                p = skip_json_string_local(p);
                continue;
            }
            if (*p == open) depth++;
            else if (*p == close) depth--;
            p++;
        }
        return p;
    }
    while (*p && *p != ',' && *p != '}' && *p != ']' &&
           *p != '\n' && *p != '\r' && *p != '\t' && *p != ' ') {
        p++;
    }
    return p;
}

static char *copy_json_value(const char *p) {
    p = skip_ws_local(p);
    const char *end = skip_json_value_local(p);
    size_t n = (size_t)(end - p);
    char *out = safe_malloc(n + 1);
    memcpy(out, p, n);
    out[n] = '\0';
    return out;
}

static char *parse_json_string_local_at(const char **pp) {
    const char *p = skip_ws_local(*pp);
    if (*p != '"') return NULL;
    p++;
    jbuf_t b;
    jbuf_init(&b, 128);
    while (*p && *p != '"') {
        if (*p == '\\' && p[1]) {
            p++;
            switch (*p) {
                case '"': case '\\': case '/': jbuf_append_char(&b, *p); break;
                case 'b': jbuf_append_char(&b, '\b'); break;
                case 'f': jbuf_append_char(&b, '\f'); break;
                case 'n': jbuf_append_char(&b, '\n'); break;
                case 'r': jbuf_append_char(&b, '\r'); break;
                case 't': jbuf_append_char(&b, '\t'); break;
                default: jbuf_append_char(&b, *p); break;
            }
        } else {
            jbuf_append_char(&b, *p);
        }
        p++;
    }
    if (*p == '"') p++;
    *pp = p;
    return b.data;
}

typedef void (*json_pair_cb)(const char *key, const char *raw_value, void *ctx);

static void json_object_foreach_local(const char *obj, json_pair_cb cb, void *ctx) {
    const char *p = skip_ws_local(obj);
    if (*p != '{') return;
    p++;
    while (*p) {
        p = skip_ws_local(p);
        if (*p == '}') break;
        if (*p != '"') { p++; continue; }
        char *key = parse_json_string_local_at(&p);
        p = skip_ws_local(p);
        if (*p != ':') { free(key); break; }
        p++;
        const char *value_start = skip_ws_local(p);
        const char *value_end = skip_json_value_local(value_start);
        size_t n = (size_t)(value_end - value_start);
        char *raw = safe_malloc(n + 1);
        memcpy(raw, value_start, n);
        raw[n] = '\0';
        if (key) cb(key, raw, ctx);
        free(raw);
        free(key);
        p = skip_ws_local(value_end);
        if (*p == ',') p++;
    }
}

static int parse_json_string_array(const char *raw, char out[][256], int max) {
    int count = 0;
    const char *p = skip_ws_local(raw);
    if (*p != '[') return 0;
    p++;
    while (*p && *p != ']' && count < max) {
        p = skip_ws_local(p);
        if (*p == '"') {
            char *s = parse_json_string_local_at(&p);
            if (s) {
                copy_str(out[count], 256, s);
                count++;
                free(s);
            }
        } else {
            p = skip_json_value_local(p);
        }
        p = skip_ws_local(p);
        if (*p == ',') p++;
    }
    return count;
}

typedef struct {
    mcp_server_t *srv;
    bool headers;
} kv_map_ctx_t;

static void add_server_kv(mcp_server_t *srv, bool header,
                          const char *key, const char *val) {
    if (!key || !val) return;
    if (header) {
        if (srv->headerc >= MCP_MAX_HEADERS) return;
        copy_str(srv->header_keys[srv->headerc], sizeof(srv->header_keys[0]), key);
        copy_str(srv->header_vals[srv->headerc], sizeof(srv->header_vals[0]), val);
        srv->headerc++;
    } else {
        if (srv->envc >= MCP_MAX_ENV) return;
        copy_str(srv->env_keys[srv->envc], sizeof(srv->env_keys[0]), key);
        copy_str(srv->env_vals[srv->envc], sizeof(srv->env_vals[0]), val);
        srv->envc++;
    }
}

static void parse_json_kv_pair(const char *key, const char *raw_value, void *ctx) {
    kv_map_ctx_t *km = (kv_map_ctx_t *)ctx;
    const char *p = skip_ws_local(raw_value);
    char *val = NULL;
    if (*p == '"') {
        val = parse_json_string_local_at(&p);
    } else {
        val = copy_json_value(p);
        char *t = trim_inplace(val);
        if (t != val) memmove(val, t, strlen(t) + 1);
    }
    add_server_kv(km->srv, km->headers, key, val);
    free(val);
}

static void parse_json_string_map(const char *raw, mcp_server_t *srv, bool headers) {
    kv_map_ctx_t ctx = { .srv = srv, .headers = headers };
    json_object_foreach_local(raw, parse_json_kv_pair, &ctx);
}

/* Read a single JSON-RPC response line from fd (blocking with timeout). */
static char *rpc_read_response(int fd, int timeout_ms) {
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    jbuf_t line;
    jbuf_init(&line, 4096);

    if (timeout_ms <= 0) timeout_ms = mcp_timeout_ms(10000);
    double deadline = mcp_now_sec() + (timeout_ms / 1000.0);

    while (1) {
        double now = mcp_now_sec();
        int remain_ms = (int)((deadline - now) * 1000.0);
        if (remain_ms <= 0) { jbuf_free(&line); return NULL; }
        int rc = poll(&pfd, 1, remain_ms);
        if (rc < 0) {
            if (errno == EINTR) continue;
            jbuf_free(&line);
            return NULL;
        }
        if (rc == 0) { jbuf_free(&line); return NULL; }

        char buf[4096];
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            jbuf_free(&line);
            return NULL;
        }
        if (n == 0) { jbuf_free(&line); return NULL; }

        for (ssize_t i = 0; i < n; i++) {
            if (buf[i] == '\n') {
                char *t = trim_inplace(line.data);
                if (t[0] == '{' && t[strlen(t) - 1] == '}') {
                    char *out = safe_strdup(t);
                    jbuf_free(&line);
                    return out;
                }
                jbuf_reset(&line);
                continue;
            }
            jbuf_append_char(&line, buf[i]);
        }

        char *t = trim_inplace(line.data);
        if (line.len > 2 && t[0] == '{' && t[strlen(t) - 1] == '}') {
            char *out = safe_strdup(t);
            jbuf_free(&line);
            return out;
        }
    }
}

/* ── HTTP transport ────────────────────────────────────────────────────── */

typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} http_buf_t;

static size_t http_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    size_t total = size * nmemb;
    http_buf_t *b = (http_buf_t *)userdata;
    if (b->len + total + 1 > b->cap) {
        size_t nc = (b->len + total + 1) * 2;
        char *np = realloc(b->data, nc);
        if (!np) return 0;
        b->data = np;
        b->cap = nc;
    }
    memcpy(b->data + b->len, ptr, total);
    b->len += total;
    b->data[b->len] = '\0';
    return total;
}

static size_t http_header_cb(char *buffer, size_t size, size_t nitems, void *userdata) {
    size_t total = size * nitems;
    mcp_server_t *srv = (mcp_server_t *)userdata;
    const char prefix[] = "mcp-session-id:";
    if (total > sizeof(prefix) - 1 &&
        strncasecmp(buffer, prefix, sizeof(prefix) - 1) == 0) {
        size_t n = total - (sizeof(prefix) - 1);
        if (n >= sizeof(srv->session_id)) n = sizeof(srv->session_id) - 1;
        memcpy(srv->session_id, buffer + sizeof(prefix) - 1, n);
        srv->session_id[n] = '\0';
        char *t = trim_inplace(srv->session_id);
        if (t != srv->session_id) memmove(srv->session_id, t, strlen(t) + 1);
    }
    return total;
}

static char *extract_first_json_object(const char *data) {
    if (!data) return NULL;
    const char *p = strchr(data, '{');
    while (p) {
        const char *q = p;
        int depth = 0;
        while (*q) {
            if (*q == '"') {
                q = skip_json_string_local(q);
                continue;
            }
            if (*q == '{') depth++;
            else if (*q == '}') {
                depth--;
                if (depth == 0) {
                    size_t n = (size_t)(q + 1 - p);
                    char *out = safe_malloc(n + 1);
                    memcpy(out, p, n);
                    out[n] = '\0';
                    return out;
                }
            }
            q++;
        }
        p = strchr(p + 1, '{');
    }
    return NULL;
}

static char *http_post_rpc(mcp_server_t *srv, const char *payload, int timeout_ms) {
    CURL *curl = curl_easy_init();
    if (!curl) return NULL;

    http_buf_t resp = {0};
    resp.cap = 8192;
    resp.data = calloc(1, resp.cap);
    if (!resp.data) { curl_easy_cleanup(curl); return NULL; }

    struct curl_slist *hdrs = NULL;
    hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
    hdrs = curl_slist_append(hdrs, "Accept: application/json, text/event-stream");
    hdrs = curl_slist_append(hdrs, "MCP-Protocol-Version: 2024-11-05");
    if (srv->session_id[0]) {
        char h[320];
        snprintf(h, sizeof(h), "Mcp-Session-Id: %s", srv->session_id);
        hdrs = curl_slist_append(hdrs, h);
    }
    for (int i = 0; i < srv->headerc; i++) {
        char h[1200];
        snprintf(h, sizeof(h), "%s: %s", srv->header_keys[i], srv->header_vals[i]);
        hdrs = curl_slist_append(hdrs, h);
    }

    curl_easy_setopt(curl, CURLOPT_URL, srv->url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, http_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, http_header_cb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, srv);
    int effective_timeout = timeout_ms > 0 ? timeout_ms : mcp_timeout_ms(10000);
    long timeout_s = (long)((effective_timeout + 999) / 1000);
    if (timeout_s < 1) timeout_s = 1;
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, timeout_s);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_s);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "dsco/" DSCO_VERSION);
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2TLS);

    CURLcode res = curl_easy_perform(curl);
    long code = 0;
    if (res == CURLE_OK) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK || code < 200 || code >= 300) {
        free(resp.data);
        return NULL;
    }

    char *json = extract_first_json_object(resp.data);
    free(resp.data);
    return json;
}

static char *send_rpc_request(mcp_server_t *srv, const char *req, int timeout_ms) {
    if (srv->transport == MCP_TRANSPORT_HTTP) {
        return http_post_rpc(srv, req, timeout_ms);
    }
    ssize_t w = write(srv->stdin_fd, req, strlen(req));
    if (w <= 0) return NULL;
    return rpc_read_response(srv->stdout_fd, timeout_ms);
}

static void send_rpc_notification(mcp_server_t *srv, const char *method) {
    char *req = rpc_build(0, method, NULL);
    if (srv->transport == MCP_TRANSPORT_HTTP) {
        char *resp = http_post_rpc(srv, req, mcp_timeout_ms(10000));
        free(resp);
    } else if (srv->stdin_fd >= 0) {
        (void)write(srv->stdin_fd, req, strlen(req));
    }
    free(req);
}

/* ── Server lifecycle ──────────────────────────────────────────────────── */

static bool spawn_server(mcp_server_t *srv) {
    int in_pipe[2], out_pipe[2];
    if (pipe(in_pipe) < 0 || pipe(out_pipe) < 0) return false;

    pid_t pid = fork();
    if (pid < 0) {
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        return false;
    }

    if (pid == 0) {
        (void)setsid();
        dup2(in_pipe[0], STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);

        int efd = -1;
        const char *home = getenv("HOME");
        if (home && srv->name[0]) {
            char dsco_dir[1024];
            char debug_dir[1024];
            snprintf(dsco_dir, sizeof(dsco_dir), "%s/.dsco", home);
            snprintf(debug_dir, sizeof(debug_dir), "%s/.dsco/debug", home);
            (void)mkdir(dsco_dir, 448);
            (void)mkdir(debug_dir, 448);

            char log_path[1024];
            snprintf(log_path, sizeof(log_path), "%s/.dsco/debug/mcp-%s.err", home, srv->name);
            efd = open(log_path, O_WRONLY | O_CREAT | O_APPEND, 384);
        }
        if (efd < 0) efd = open("/dev/null", O_WRONLY);
        if (efd >= 0) {
            dup2(efd, STDERR_FILENO);
            close(efd);
        }

        for (int i = 0; i < srv->envc; i++) {
            if (srv->env_keys[i][0]) setenv(srv->env_keys[i], srv->env_vals[i], 1);
        }
        if (srv->cwd[0]) (void)chdir(srv->cwd);

        char *argv[MCP_MAX_ARGS + 2];
        argv[0] = srv->command;
        int argc = srv->argc;
        if (argc > MCP_MAX_ARGS) argc = MCP_MAX_ARGS;
        for (int i = 0; i < argc; i++)
            argv[i + 1] = srv->args[i];
        argv[argc + 1] = NULL;

        execvp(srv->command, argv);
        _exit(127);
    }

    close(in_pipe[0]);
    close(out_pipe[1]);
    srv->pid = pid;
    srv->stdin_fd = in_pipe[1];
    srv->stdout_fd = out_pipe[0];

    int flags = fcntl(srv->stdout_fd, F_GETFL, 0);
    if (flags >= 0) fcntl(srv->stdout_fd, F_SETFL, flags | O_NONBLOCK);

    return true;
}

static bool initialize_server(mcp_server_t *srv) {
    srv->rpc_id = 1;
    char *init_req = rpc_build(srv->rpc_id++, "initialize",
        "{\"protocolVersion\":\"2024-11-05\","
        "\"capabilities\":{\"tools\":{}},"
        "\"clientInfo\":{\"name\":\"dsco\",\"version\":\"" DSCO_VERSION "\"}}");

    char *resp = send_rpc_request(srv, init_req, mcp_timeout_ms(10000));
    free(init_req);
    if (!resp) return false;
    free(resp);

    send_rpc_notification(srv, "notifications/initialized");
    srv->initialized = true;
    return true;
}

static void stop_server(mcp_server_t *srv) {
    if (srv->stdin_fd >= 0) {
        close(srv->stdin_fd);
        srv->stdin_fd = -1;
    }
    if (srv->stdout_fd >= 0) {
        close(srv->stdout_fd);
        srv->stdout_fd = -1;
    }
    if (srv->pid > 0) {
        kill(srv->pid, SIGTERM);
        int status;
        waitpid(srv->pid, &status, WNOHANG);
    }
    srv->pid = 0;
    srv->initialized = false;
}

/* ── Tool discovery ────────────────────────────────────────────────────── */

typedef struct {
    mcp_registry_t *reg;
    int server_idx;
} tools_list_ctx_t;

static bool mcp_tool_name_exists(mcp_registry_t *reg, const char *name) {
    for (int i = 0; i < reg->tool_count; i++) {
        if (strcmp(reg->tools[i].name, name) == 0) return true;
    }
    return false;
}

static void parse_tool_entry(const char *json, void *ctx) {
    tools_list_ctx_t *tlc = (tools_list_ctx_t *)ctx;
    mcp_registry_t *reg = tlc->reg;
    if (reg->tool_count >= MCP_MAX_TOOLS) return;

    mcp_tool_t *tool = &reg->tools[reg->tool_count];
    memset(tool, 0, sizeof(*tool));

    char *name = json_get_str(json, "name");
    char *desc = json_get_str(json, "description");
    char *schema = json_get_raw(json, "inputSchema");

    if (!name || !name[0]) {
        free(name); free(desc); free(schema);
        return;
    }

    char clean_tool[128];
    sanitize_name(name, clean_tool, sizeof(clean_tool));
    copy_str(tool->remote_name, sizeof(tool->remote_name), name);
    char base_name[128];
    dsco_mcp_build_tool_name(reg->servers[tlc->server_idx].name, clean_tool,
                             base_name, sizeof(base_name));
    copy_str(tool->name, sizeof(tool->name), base_name);
    for (int suffix = 2; mcp_tool_name_exists(reg, tool->name) && suffix < 1000; suffix++)
        snprintf(tool->name, sizeof(tool->name), "%.120s_%d", base_name, suffix);
    free(name);

    if (desc) {
        snprintf(tool->description, sizeof(tool->description), "%s", desc);
        free(desc);
    } else {
        snprintf(tool->description, sizeof(tool->description),
                 "MCP tool from %s", reg->servers[tlc->server_idx].name);
    }
    if (schema) {
        snprintf(tool->input_schema, sizeof(tool->input_schema), "%s", schema);
        free(schema);
    } else {
        snprintf(tool->input_schema, sizeof(tool->input_schema),
                 "{\"type\":\"object\",\"properties\":{}}");
    }
    tool->server_idx = tlc->server_idx;
    reg->tool_count++;
}

static int discover_tools(mcp_registry_t *reg, int server_idx) {
    mcp_server_t *srv = &reg->servers[server_idx];
    if (!srv->initialized) return 0;

    char *req = rpc_build(srv->rpc_id++, "tools/list", "{}");
    char *resp = send_rpc_request(srv, req, mcp_timeout_ms(10000));
    free(req);
    if (!resp) return 0;

    char *result = json_get_raw(resp, "result");
    if (!result) { free(resp); return 0; }

    tools_list_ctx_t ctx = { .reg = reg, .server_idx = server_idx };
    int before = reg->tool_count;
    json_array_foreach(result, "tools", parse_tool_entry, &ctx);
    int discovered = reg->tool_count - before;

    free(result);
    free(resp);
    return discovered;
}

/* ── Config import ─────────────────────────────────────────────────────── */

static bool same_server_config(const mcp_server_t *a, const mcp_server_t *b) {
    if (strcmp(a->name, b->name) != 0) return false;
    if (a->transport != b->transport) return false;
    if (strcmp(a->command, b->command) != 0) return false;
    if (strcmp(a->url, b->url) != 0) return false;
    if (a->argc != b->argc) return false;
    for (int i = 0; i < a->argc; i++)
        if (strcmp(a->args[i], b->args[i]) != 0) return false;
    return true;
}

static bool duplicate_exact(mcp_registry_t *reg, const mcp_server_t *srv) {
    for (int i = 0; i < reg->server_count; i++) {
        if (same_server_config(&reg->servers[i], srv)) return true;
    }
    return false;
}

static void uniquify_server_name(mcp_registry_t *reg, mcp_server_t *srv) {
    char base[128];
    copy_str(base, sizeof(base), srv->name);
    for (int suffix = 2; suffix < 100; suffix++) {
        bool found = false;
        for (int i = 0; i < reg->server_count; i++) {
            if (strcmp(reg->servers[i].name, srv->name) == 0) {
                found = true;
                break;
            }
        }
        if (!found) return;
        snprintf(srv->name, sizeof(srv->name), "%s_%d", base, suffix);
    }
}

/* ── Parallel connect ──────────────────────────────────────────────────────
 * Connecting MCP servers serially means one slow or dead endpoint (a stopped
 * local daemon, a cold-starting `uv run`/`npx`, an unreachable Modal URL)
 * blocks every server queued behind it for the full per-RPC timeout. With ~30
 * servers configured that serialises into minutes of "connecting…", so the
 * status line shows only the first one or two that happened to be fast.
 *
 * Instead we split init into two phases: parse all config files into a pending
 * list (single-threaded, cheap), then connect every server concurrently from a
 * bounded worker pool. Live servers register their tools within one RPC
 * timeout regardless of how many dead ones sit alongside them. */

typedef struct {
    mcp_server_t *items;
    int           count;
    int           cap;
} pending_list_t;

/* Non-NULL only between mcp_init's parse and connect phases; start_configured_server
 * collects into it instead of connecting inline. mcp_init never runs concurrently
 * with itself (the /mcp reload path joins the bg loader first), so a file-static
 * is safe. */
static pending_list_t *g_collect = NULL;

static bool pending_has_dup(const pending_list_t *pl, const mcp_server_t *srv) {
    for (int i = 0; i < pl->count; i++)
        if (same_server_config(&pl->items[i], srv)) return true;
    return false;
}

static void pending_uniquify(const pending_list_t *pl, mcp_server_t *srv) {
    char base[128];
    copy_str(base, sizeof(base), srv->name);
    for (int suffix = 2; suffix < 100; suffix++) {
        bool found = false;
        for (int i = 0; i < pl->count; i++) {
            if (strcmp(pl->items[i].name, srv->name) == 0) { found = true; break; }
        }
        if (!found) return;
        snprintf(srv->name, sizeof(srv->name), "%s_%d", base, suffix);
    }
}

static void start_configured_server(mcp_registry_t *reg, const mcp_server_t *cfg) {
    if (!cfg->command[0] && !cfg->url[0]) return;

    mcp_server_t srv = *cfg;
    srv.stdin_fd = -1;
    srv.stdout_fd = -1;
    srv.pid = 0;
    srv.initialized = false;
    srv.rpc_id = 1;

    if (srv.transport == MCP_TRANSPORT_HTTP) {
        if (!srv.url[0]) normalize_http_url(srv.command, srv.url, sizeof(srv.url));
        if (!srv.command[0]) copy_str(srv.command, sizeof(srv.command), srv.url);
    }

    /* Collection phase: stash for the parallel connect pool and return. */
    if (g_collect) {
        pending_list_t *pl = g_collect;
        if (pl->count >= MCP_MAX_SERVERS) return;
        if (pending_has_dup(pl, &srv)) return;
        pending_uniquify(pl, &srv);
        if (pl->count >= pl->cap) {
            int ncap = pl->cap ? pl->cap * 2 : 16;
            if (ncap > MCP_MAX_SERVERS) ncap = MCP_MAX_SERVERS;
            mcp_server_t *grown = realloc(pl->items, (size_t)ncap * sizeof(*grown));
            if (!grown) return;
            pl->items = grown;
            pl->cap = ncap;
        }
        pl->items[pl->count++] = srv;
        return;
    }

    /* Inline (serial) fallback — used if collection is not active. */
    if (reg->server_count >= MCP_MAX_SERVERS) return;
    reg->configured_count++;
    if (duplicate_exact(reg, &srv)) return;
    uniquify_server_name(reg, &srv);

    int idx = reg->server_count;
    reg->servers[idx] = srv;

    const char *endpoint = srv.transport == MCP_TRANSPORT_HTTP ? srv.url : srv.command;
    MCP_LOG("  \033[2mmcp: connecting %s (%s)\033[0m\n", srv.name, endpoint);

    bool ok = false;
    if (srv.transport == MCP_TRANSPORT_HTTP) {
        ok = initialize_server(&reg->servers[idx]);
    } else {
        ok = spawn_server(&reg->servers[idx]) && initialize_server(&reg->servers[idx]);
    }

    if (ok) {
        int n = discover_tools(reg, idx);
        MCP_LOG("  \033[2mmcp: %s: %d tools discovered\033[0m\n",
                reg->servers[idx].name, n);
        reg->server_count++;
    } else {
        MCP_LOG("  \033[31mmcp: failed to connect %s\033[0m\n", srv.name);
        reg->failed_count++;
        stop_server(&reg->servers[idx]);
        memset(&reg->servers[idx], 0, sizeof(reg->servers[idx]));
    }
}

#define MCP_CONNECT_WORKERS 12

typedef struct {
    mcp_registry_t *reg;
    pending_list_t *pending;
    int             next;        /* atomic cursor into pending->items */
    pthread_mutex_t spawn_lock;  /* serialises fork()+exec setup only */
    pthread_mutex_t merge_lock;  /* guards registry mutation + tool discovery */
} connect_pool_t;

static void *mcp_connect_worker(void *arg) {
    connect_pool_t *p = arg;
    for (;;) {
        int i = __atomic_fetch_add(&p->next, 1, __ATOMIC_RELAXED);
        if (i >= p->pending->count) break;

        mcp_server_t srv = p->pending->items[i];
        srv.stdin_fd = -1;
        srv.stdout_fd = -1;
        srv.pid = 0;
        srv.initialized = false;
        srv.rpc_id = 1;

        const char *endpoint = srv.transport == MCP_TRANSPORT_HTTP ? srv.url : srv.command;
        MCP_LOG("  \033[2mmcp: connecting %s (%s)\033[0m\n", srv.name, endpoint);

        /* Slow work — process spawn + the initialize handshake / network
         * roundtrip — runs without the merge lock so dead servers don't stall
         * live ones. fork()+exec is serialised on its own lock to avoid the
         * concurrent-fork-from-threads malloc-lock hazard; it is fast. */
        bool ok;
        if (srv.transport == MCP_TRANSPORT_HTTP) {
            ok = initialize_server(&srv);
        } else {
            pthread_mutex_lock(&p->spawn_lock);
            bool spawned = spawn_server(&srv);
            pthread_mutex_unlock(&p->spawn_lock);
            ok = spawned && initialize_server(&srv);
        }

        pthread_mutex_lock(&p->merge_lock);
        mcp_registry_t *reg = p->reg;
        reg->configured_count++;
        if (ok && reg->server_count < MCP_MAX_SERVERS) {
            int idx = reg->server_count;
            reg->servers[idx] = srv;
            reg->server_count++;   /* publish slot before discovery uses it */
            int n = discover_tools(reg, idx);
            MCP_LOG("  \033[2mmcp: %s: %d tools discovered\033[0m\n",
                    reg->servers[idx].name, n);
        } else {
            if (!ok)
                MCP_LOG("  \033[31mmcp: failed to connect %s\033[0m\n", srv.name);
            reg->failed_count++;
            stop_server(&srv);   /* failed, or connected with no slot left */
        }
        pthread_mutex_unlock(&p->merge_lock);
    }
    return NULL;
}

static void mcp_curl_global_init_once(void) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

static void mcp_connect_all(mcp_registry_t *reg, pending_list_t *pending) {
    if (pending->count <= 0) return;

    /* Ensure libcurl's global state is initialised once before any worker calls
     * curl_easy_init concurrently (the implicit lazy init is not thread-safe). */
    static pthread_once_t curl_once = PTHREAD_ONCE_INIT;
    pthread_once(&curl_once, mcp_curl_global_init_once);

    connect_pool_t pool;
    pool.reg = reg;
    pool.pending = pending;
    pool.next = 0;
    pthread_mutex_init(&pool.spawn_lock, NULL);
    pthread_mutex_init(&pool.merge_lock, NULL);

    int nthreads = pending->count < MCP_CONNECT_WORKERS
                 ? pending->count : MCP_CONNECT_WORKERS;
    pthread_t threads[MCP_CONNECT_WORKERS];
    int started = 0;
    for (int i = 0; i < nthreads; i++) {
        if (pthread_create(&threads[i], NULL, mcp_connect_worker, &pool) == 0)
            started++;
    }
    if (started == 0) {
        /* Could not spawn any worker — connect on the calling thread. */
        mcp_connect_worker(&pool);
    } else {
        for (int i = 0; i < started; i++) pthread_join(threads[i], NULL);
    }

    pthread_mutex_destroy(&pool.spawn_lock);
    pthread_mutex_destroy(&pool.merge_lock);
}

static void parse_server_common(mcp_registry_t *reg, const char *raw_name,
                                const char *obj, const char *source) {
    mcp_server_t srv;
    memset(&srv, 0, sizeof(srv));
    srv.stdin_fd = -1;
    srv.stdout_fd = -1;
    srv.transport = MCP_TRANSPORT_STDIO;
    sanitize_name(raw_name, srv.name, sizeof(srv.name));
    copy_str(srv.source, sizeof(srv.source), source);

    if (json_get_bool(obj, "disabled", false)) return;
    char *enabled_raw = json_get_raw(obj, "enabled");
    if (enabled_raw) {
        char *t = trim_inplace(enabled_raw);
        bool disabled = strncmp(t, "false", 5) == 0;
        free(enabled_raw);
        if (disabled) return;
    }

    char *cmd = json_get_str(obj, "command");
    char *url = json_get_str(obj, "url");
    char *type = json_get_str(obj, "type");
    char *transport = json_get_str(obj, "transport");
    char *cwd = json_get_str(obj, "cwd");
    if (cmd) copy_str(srv.command, sizeof(srv.command), cmd);
    if (cwd) copy_str(srv.cwd, sizeof(srv.cwd), cwd);
    if (url) normalize_http_url(url, srv.url, sizeof(srv.url));
    if (type && (strcasecmp(type, "http") == 0 || strcasecmp(type, "sse") == 0))
        srv.transport = MCP_TRANSPORT_HTTP;
    if (transport && (strcasecmp(transport, "http") == 0 || strcasecmp(transport, "sse") == 0))
        srv.transport = MCP_TRANSPORT_HTTP;
    if (starts_http(srv.command)) {
        normalize_http_url(srv.command, srv.url, sizeof(srv.url));
        srv.transport = MCP_TRANSPORT_HTTP;
    }
    if (srv.url[0]) srv.transport = MCP_TRANSPORT_HTTP;
    if (!srv.command[0] && srv.url[0]) copy_str(srv.command, sizeof(srv.command), srv.url);

    char *args = json_get_raw(obj, "args");
    if (args) {
        srv.argc = parse_json_string_array(args, srv.args, MCP_MAX_ARGS);
        free(args);
    }
    char *env = json_get_raw(obj, "env");
    if (env) {
        parse_json_string_map(env, &srv, false);
        free(env);
    }
    char *headers = json_get_raw(obj, "headers");
    if (headers) {
        parse_json_string_map(headers, &srv, true);
        free(headers);
    }
    char *http_headers = json_get_raw(obj, "http_headers");
    if (http_headers) {
        parse_json_string_map(http_headers, &srv, true);
        free(http_headers);
    }

    free(cmd); free(url); free(type); free(transport); free(cwd);
    if (!srv.command[0] && !srv.url[0]) return;
    start_configured_server(reg, &srv);
}

typedef struct {
    mcp_registry_t *reg;
    const char     *source;
} server_map_ctx_t;

static void parse_server_map_entry(const char *name, const char *raw_value, void *ctx) {
    server_map_ctx_t *sm = (server_map_ctx_t *)ctx;
    const char *v = skip_ws_local(raw_value);
    if (*v != '{') return;
    parse_server_common(sm->reg, name, v, sm->source);
}

static void import_servers_object(mcp_registry_t *reg, const char *servers_raw,
                                  const char *source) {
    server_map_ctx_t ctx = { .reg = reg, .source = source };
    json_object_foreach_local(servers_raw, parse_server_map_entry, &ctx);
}

static char *value_after_json_key(const char *p) {
    const char *q = skip_json_string_local(p);
    q = skip_ws_local(q);
    if (*q != ':') return NULL;
    q++;
    q = skip_ws_local(q);
    if (*q != '{') return NULL;
    return copy_json_value(q);
}

static void scan_json_key(mcp_registry_t *reg, const char *data, const char *key,
                          const char *source) {
    char needle[96];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = data;
    while ((p = strstr(p, needle)) != NULL) {
        char *raw = value_after_json_key(p);
        if (raw) {
            import_servers_object(reg, raw, source);
            free(raw);
        }
        p += strlen(needle);
    }
}

static void import_top_level_json_key(mcp_registry_t *reg, const char *data,
                                      const char *key, const char *source) {
    char *raw = json_get_raw(data, key);
    if (!raw) return;
    import_servers_object(reg, raw, source);
    free(raw);
}

static void load_json_config(mcp_registry_t *reg, const char *path,
                             const char *source, bool scan_servers_key,
                             bool recursive) {
    char *data = read_file_limited(path, 2 * 1024 * 1024);
    if (!data) return;

    if (recursive) {
        scan_json_key(reg, data, "mcpServers", source);
        scan_json_key(reg, data, "mcp_servers", source);
        if (scan_servers_key) scan_json_key(reg, data, "servers", source);
    } else {
        import_top_level_json_key(reg, data, "mcpServers", source);
        import_top_level_json_key(reg, data, "mcp_servers", source);
        if (scan_servers_key) import_top_level_json_key(reg, data, "servers", source);
    }

    free(data);
}

static bool project_path_matches_cwd(const char *project_path, const char *cwd) {
    if (!project_path || !cwd || !project_path[0]) return false;
    size_t n = strlen(project_path);
    if (strcmp(project_path, cwd) == 0) return true;
    return strncmp(cwd, project_path, n) == 0 && cwd[n] == '/';
}

typedef struct {
    mcp_registry_t *reg;
    const char     *source;
    const char     *cwd;
} claude_project_ctx_t;

static void parse_matching_claude_project(const char *project_path,
                                          const char *raw_value,
                                          void *ctx) {
    claude_project_ctx_t *cp = (claude_project_ctx_t *)ctx;
    if (!project_path_matches_cwd(project_path, cp->cwd)) return;
    char *raw = json_get_raw(raw_value, "mcpServers");
    if (raw) {
        import_servers_object(cp->reg, raw, cp->source);
        free(raw);
    }
    raw = json_get_raw(raw_value, "mcp_servers");
    if (raw) {
        import_servers_object(cp->reg, raw, cp->source);
        free(raw);
    }
}

static void load_claude_json_config(mcp_registry_t *reg, const char *path,
                                    const char *source) {
    char *data = read_file_limited(path, 2 * 1024 * 1024);
    if (!data) return;

    import_top_level_json_key(reg, data, "mcpServers", source);
    import_top_level_json_key(reg, data, "mcp_servers", source);

    char cwd[1024];
    if (getcwd(cwd, sizeof(cwd))) {
        char *projects = json_get_raw(data, "projects");
        if (projects) {
            claude_project_ctx_t ctx = { .reg = reg, .source = source, .cwd = cwd };
            json_object_foreach_local(projects, parse_matching_claude_project, &ctx);
            free(projects);
        }
    }

    free(data);
}

static char *parse_toml_string_at(const char **pp) {
    const char *p = skip_ws_local(*pp);
    if (*p == '"') return parse_json_string_local_at(pp);
    const char *start = p;
    while (*p && *p != '#' && *p != ',' && *p != '}' && *p != '\n' && *p != '\r') p++;
    size_t n = (size_t)(p - start);
    char *out = safe_malloc(n + 1);
    memcpy(out, start, n);
    out[n] = '\0';
    char *t = trim_inplace(out);
    if (t != out) memmove(out, t, strlen(t) + 1);
    *pp = p;
    return out;
}

static void parse_toml_array_strings(const char *raw, mcp_server_t *srv) {
    const char *p = skip_ws_local(raw);
    if (*p != '[') return;
    p++;
    while (*p && *p != ']' && srv->argc < MCP_MAX_ARGS) {
        p = skip_ws_local(p);
        if (*p == '"') {
            char *s = parse_toml_string_at(&p);
            if (s) {
                copy_str(srv->args[srv->argc], sizeof(srv->args[0]), s);
                srv->argc++;
                free(s);
            }
        } else {
            while (*p && *p != ',' && *p != ']') p++;
        }
        p = skip_ws_local(p);
        if (*p == ',') p++;
    }
}

static void parse_toml_inline_map(const char *raw, mcp_server_t *srv, bool headers) {
    const char *p = skip_ws_local(raw);
    if (*p != '{') return;
    p++;
    while (*p && *p != '}') {
        p = skip_ws_local(p);
        const char *ks = p;
        while (*p && *p != '=' && *p != '}' && *p != ',') p++;
        if (*p != '=') break;
        size_t kn = (size_t)(p - ks);
        char key[128];
        if (kn >= sizeof(key)) kn = sizeof(key) - 1;
        memcpy(key, ks, kn);
        key[kn] = '\0';
        char *kt = trim_inplace(key);
        p++;
        char *val = parse_toml_string_at(&p);
        add_server_kv(srv, headers, kt, val);
        free(val);
        p = skip_ws_local(p);
        if (*p == ',') p++;
    }
}

static int find_or_add_toml_server(mcp_server_t servers[], int *count, const char *raw_name,
                                   const char *source) {
    char clean[128];
    sanitize_name(raw_name, clean, sizeof(clean));
    for (int i = 0; i < *count; i++) {
        if (strcmp(servers[i].name, clean) == 0) return i;
    }
    if (*count >= MCP_MAX_SERVERS) return -1;
    int idx = (*count)++;
    memset(&servers[idx], 0, sizeof(servers[idx]));
    copy_str(servers[idx].name, sizeof(servers[idx].name), clean);
    copy_str(servers[idx].source, sizeof(servers[idx].source), source);
    servers[idx].stdin_fd = -1;
    servers[idx].stdout_fd = -1;
    servers[idx].transport = MCP_TRANSPORT_STDIO;
    return idx;
}

static void load_toml_config(mcp_registry_t *reg, const char *path, const char *source) {
    char *data = read_file_limited(path, 512 * 1024);
    if (!data) return;

    mcp_server_t *parsed = calloc(MCP_MAX_SERVERS, sizeof(*parsed));
    if (!parsed) {
        free(data);
        return;
    }
    int parsed_count = 0;
    int current = -1;
    enum { SEC_NONE, SEC_ROOT, SEC_ENV, SEC_HEADERS } section = SEC_NONE;

    char *save = NULL;
    for (char *line = strtok_r(data, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *s = trim_inplace(line);
        if (!*s || *s == '#') continue;
        if (*s == '[') {
            section = SEC_NONE;
            current = -1;
            char *end = strchr(s, ']');
            if (!end) continue;
            *end = '\0';
            char *sec = s + 1;
            const char *prefixes[] = { "mcp_servers.", "mcpServers." };
            const char *body = NULL;
            for (int i = 0; i < 2; i++) {
                size_t plen = strlen(prefixes[i]);
                if (strncmp(sec, prefixes[i], plen) == 0) {
                    body = sec + plen;
                    break;
                }
            }
            if (!body || !*body) continue;
            char name[128];
            const char *dot = strchr(body, '.');
            size_t nn = dot ? (size_t)(dot - body) : strlen(body);
            if (nn >= sizeof(name)) nn = sizeof(name) - 1;
            memcpy(name, body, nn);
            name[nn] = '\0';
            current = find_or_add_toml_server(parsed, &parsed_count, name, source);
            if (current < 0) continue;
            if (!dot) section = SEC_ROOT;
            else if (strcmp(dot + 1, "env") == 0) section = SEC_ENV;
            else if (strcmp(dot + 1, "headers") == 0 ||
                     strcmp(dot + 1, "http_headers") == 0) section = SEC_HEADERS;
            else section = SEC_NONE;
            continue;
        }

        if (current < 0 || section == SEC_NONE) continue;
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = trim_inplace(s);
        char *val = trim_inplace(eq + 1);
        mcp_server_t *srv = &parsed[current];

        if (section == SEC_ENV || section == SEC_HEADERS) {
            const char *vp = val;
            char *v = parse_toml_string_at(&vp);
            add_server_kv(srv, section == SEC_HEADERS, key, v);
            free(v);
            continue;
        }

        if (strcmp(key, "command") == 0 || strcmp(key, "cmd") == 0) {
            const char *vp = val;
            char *v = parse_toml_string_at(&vp);
            copy_str(srv->command, sizeof(srv->command), v);
            free(v);
        } else if (strcmp(key, "url") == 0) {
            const char *vp = val;
            char *v = parse_toml_string_at(&vp);
            normalize_http_url(v, srv->url, sizeof(srv->url));
            srv->transport = MCP_TRANSPORT_HTTP;
            if (!srv->command[0]) copy_str(srv->command, sizeof(srv->command), srv->url);
            free(v);
        } else if (strcmp(key, "cwd") == 0) {
            const char *vp = val;
            char *v = parse_toml_string_at(&vp);
            copy_str(srv->cwd, sizeof(srv->cwd), v);
            free(v);
        } else if (strcmp(key, "args") == 0) {
            parse_toml_array_strings(val, srv);
        } else if (strcmp(key, "env") == 0) {
            parse_toml_inline_map(val, srv, false);
        } else if (strcmp(key, "headers") == 0 || strcmp(key, "http_headers") == 0) {
            parse_toml_inline_map(val, srv, true);
        } else if (strcmp(key, "type") == 0 || strcmp(key, "transport") == 0) {
            const char *vp = val;
            char *v = parse_toml_string_at(&vp);
            if (strcasecmp(v, "http") == 0 || strcasecmp(v, "sse") == 0)
                srv->transport = MCP_TRANSPORT_HTTP;
            free(v);
        } else if (strcmp(key, "disabled") == 0) {
            if (strncmp(val, "true", 4) == 0) srv->command[0] = '\0';
        }
        if (starts_http(srv->command)) {
            normalize_http_url(srv->command, srv->url, sizeof(srv->url));
            srv->transport = MCP_TRANSPORT_HTTP;
        }
    }

    for (int i = 0; i < parsed_count; i++) {
        if (parsed[i].transport == MCP_TRANSPORT_HTTP && !parsed[i].url[0])
            normalize_http_url(parsed[i].command, parsed[i].url, sizeof(parsed[i].url));
        start_configured_server(reg, &parsed[i]);
    }

    free(parsed);
    free(data);
}

/* ── Public API ────────────────────────────────────────────────────────── */

int mcp_init(mcp_registry_t *reg) {
    memset(reg, 0, sizeof(*reg));

    const char *home = getenv("HOME");
    if (!home) return 0;

    char path[1024];

    /* Phase 1: parse every config file into a pending list (cheap, serial).
     * start_configured_server appends here instead of connecting inline. */
    pending_list_t collect = {0};
    g_collect = &collect;

    snprintf(path, sizeof(path), "%s/.dsco/mcp.json", home);
    load_json_config(reg, path, "dsco:mcp", true, true);
    snprintf(path, sizeof(path), "%s/.dsco/config.json", home);
    load_json_config(reg, path, "dsco:config", true, true);
    load_json_config(reg, ".mcp.json", "project:mcp", true, true);
    load_json_config(reg, ".dsco/mcp.json", "project:dsco-mcp", true, true);
    load_json_config(reg, ".dsco/config.json", "project:dsco-config", true, true);
    load_json_config(reg, ".claude/settings.json", "project:claude", false, false);
    load_json_config(reg, ".claude/settings.local.json", "project:claude-local", false, false);

    snprintf(path, sizeof(path), "%s/.claude.json", home);
    load_claude_json_config(reg, path, "claude:global");
    snprintf(path, sizeof(path), "%s/.claude/settings.json", home);
    load_json_config(reg, path, "claude:settings", false, false);
    snprintf(path, sizeof(path), "%s/.claude/settings.local.json", home);
    load_json_config(reg, path, "claude:settings-local", false, false);
    snprintf(path, sizeof(path), "%s/Library/Application Support/Claude/claude_desktop_config.json", home);
    load_json_config(reg, path, "claude:desktop", false, false);
    snprintf(path, sizeof(path), "%s/Library/Application Support/Claude/config.json", home);
    load_json_config(reg, path, "claude:desktop-config", false, false);

    snprintf(path, sizeof(path), "%s/.codex/config.toml", home);
    load_toml_config(reg, path, "codex:global");
    load_toml_config(reg, ".codex/config.toml", "project:codex");

    /* Phase 2: connect every collected server concurrently so one slow or dead
     * endpoint can't stall the rest behind a per-RPC timeout. */
    g_collect = NULL;
    mcp_connect_all(reg, &collect);
    free(collect.items);

    reg->loaded = true;
    return reg->tool_count;
}

void mcp_shutdown(mcp_registry_t *reg) {
    for (int i = 0; i < reg->server_count; i++) {
        stop_server(&reg->servers[i]);
    }
    reg->server_count = 0;
    reg->configured_count = 0;
    reg->failed_count = 0;
    reg->tool_count = 0;
}

const mcp_tool_t *mcp_get_tools(mcp_registry_t *reg, int *count) {
    *count = reg->tool_count;
    return reg->tools;
}

static char *extract_mcp_content_text(const char *content_raw) {
    const char *p = skip_ws_local(content_raw);
    if (*p == '{') {
        return json_get_str(p, "text");
    }
    if (*p != '[') return NULL;

    p++;
    jbuf_t b;
    jbuf_init(&b, 256);
    bool any = false;
    while (*p) {
        p = skip_ws_local(p);
        if (*p == ']') break;
        if (*p == '{') {
            char *text = json_get_str(p, "text");
            if (text && text[0]) {
                if (any) jbuf_append_char(&b, '\n');
                jbuf_append(&b, text);
                any = true;
            }
            free(text);
        }
        const char *next = skip_json_value_local(p);
        if (next <= p) break;
        p = skip_ws_local(next);
        if (*p == ',') p++;
    }
    if (any) return b.data;
    jbuf_free(&b);
    return NULL;
}

char *mcp_call_tool(mcp_registry_t *reg, const char *tool_name,
                    const char *arguments_json) {
    int tool_idx = -1;
    for (int i = 0; i < reg->tool_count; i++) {
        if (strcmp(reg->tools[i].name, tool_name) == 0) {
            tool_idx = i;
            break;
        }
    }
    if (tool_idx < 0) return NULL;

    mcp_tool_t *tool = &reg->tools[tool_idx];
    mcp_server_t *srv = &reg->servers[tool->server_idx];
    if (!srv->initialized) return NULL;

    const char *orig_name = tool->remote_name[0] ? tool->remote_name : tool_name;

    jbuf_t params;
    jbuf_init(&params, 1024);
    jbuf_append(&params, "{\"name\":");
    jbuf_append_json_str(&params, orig_name);
    jbuf_append(&params, ",\"arguments\":");
    jbuf_append(&params, arguments_json ? arguments_json : "{}");
    jbuf_append(&params, "}");

    char *req = rpc_build(srv->rpc_id++, "tools/call", params.data);
    jbuf_free(&params);

    char *resp = send_rpc_request(srv, req, mcp_timeout_ms(30000));
    free(req);
    if (!resp) return NULL;

    char *result = json_get_raw(resp, "result");
    if (!result) {
        char *err = json_get_raw(resp, "error");
        free(resp);
        if (err) return err;
        return safe_strdup("{\"error\":\"no result\"}");
    }

    char *content_text = NULL;
    char *content_raw = json_get_raw(result, "content");
    if (content_raw) {
        content_text = extract_mcp_content_text(content_raw);
        if (!content_text) content_text = safe_strdup(content_raw);
        free(content_raw);
    }
    if (!content_text) content_text = safe_strdup(result);

    free(result);
    free(resp);
    return content_text;
}
