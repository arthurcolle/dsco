#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE 1
#endif
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#include "browser_session.h"
#include "json_util.h"
#include "tool_content.h"

#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CDP_LIMIT (32u * 1024u * 1024u)
typedef struct {
    pid_t pid;
    int tx, rx, sequence;
    char id[96], profile[PATH_MAX], load_id[128], load_session[128];
    bool headless, offline;
    jbuf_t pending;
} browser_t;
static browser_t browser = {.tx = -1, .rx = -1};
static pthread_mutex_t browser_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t cleanup_once = PTHREAD_ONCE_INIT;
static unsigned long generation;

static long long clock_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static bool fail(char *out, size_t cap, const char *message) {
    jbuf_t b; jbuf_init(&b, 256);
    jbuf_append(&b, "{\"ok\":false,\"error\":");
    jbuf_append_json_str(&b, message); jbuf_append(&b, "}");
    if (out && cap) snprintf(out, cap, "%s", b.len < cap ? b.data : "{}");
    jbuf_free(&b);
    return false;
}

static int remove_profile_entry(const char *path, const struct stat *st, int type,
                               struct FTW *walk) {
    (void)st; (void)walk;
    return type == FTW_DP ? rmdir(path) : unlink(path);
}

/* Only our mkdtemp directory is removed; nftw never follows profile symlinks. */
static void dispose(void) {
    if (browser.tx >= 0) close(browser.tx);
    if (browser.rx >= 0) close(browser.rx);
    if (browser.pid > 0) {
        kill(-browser.pid, SIGTERM);
        long long until = clock_ms() + 1000;
        while (waitpid(browser.pid, NULL, WNOHANG) == 0 && clock_ms() < until) {
            struct timespec ts = {.tv_nsec = 10000000}; nanosleep(&ts, NULL);
        }
        /* The original process group also owns Chrome helpers. */
        kill(-browser.pid, SIGKILL);
        while (waitpid(browser.pid, NULL, 0) < 0 && errno == EINTR) {}
    }
    if (browser.profile[0]) nftw(browser.profile, remove_profile_entry, 24, FTW_DEPTH | FTW_PHYS);
    jbuf_free(&browser.pending);
    memset(&browser, 0, sizeof(browser)); browser.tx = browser.rx = -1;
}

static void register_cleanup(void) { atexit(dispose); }

static bool wait_fd(int fd, short events, long long deadline, char *error, size_t cap) {
    for (;;) {
        long long left = deadline - clock_ms();
        if (left <= 0) { snprintf(error, cap, "browser operation timed out"); return false; }
        struct pollfd p = {.fd = fd, .events = events};
        int ready = poll(&p, 1, (int)left);
        if (ready < 0 && errno == EINTR) continue;
        if (ready == 0) { snprintf(error, cap, "browser operation timed out"); return false; }
        if (ready < 0 || (p.revents & (POLLERR | POLLNVAL)) ||
            ((p.revents & POLLHUP) && !(p.revents & events))) {
            snprintf(error, cap, "owned browser pipe closed"); return false;
        }
        if (p.revents & events) return true;
    }
}

static ssize_t pipe_write(int fd, const char *data, size_t size) {
    sigset_t blocked, old, pending;
    sigemptyset(&blocked); sigaddset(&blocked, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &blocked, &old);
    sigpending(&pending);
    ssize_t count = write(fd, data, size);
    int saved = errno;
    if (count < 0 && saved == EPIPE && !sigismember(&pending, SIGPIPE)) {
        int signal_number; sigwait(&blocked, &signal_number);
    }
    pthread_sigmask(SIG_SETMASK, &old, NULL); errno = saved;
    return count;
}

static char *read_message(long long deadline, char *error, size_t cap) {
    for (;;) {
        char *end = browser.pending.len ? memchr(browser.pending.data, 0, browser.pending.len) : NULL;
        if (end) {
            size_t size = (size_t)(end - browser.pending.data);
            char *message = strndup(browser.pending.data, size);
            size_t used = size + 1;
            memmove(browser.pending.data, browser.pending.data + used, browser.pending.len - used);
            browser.pending.len -= used; browser.pending.data[browser.pending.len] = 0;
            return message;
        }
        if (!wait_fd(browser.rx, POLLIN, deadline, error, cap)) return NULL;
        char chunk[16384]; ssize_t n = read(browser.rx, chunk, sizeof(chunk));
        if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (n <= 0) { snprintf(error, cap, "owned browser exited or closed its pipe"); return NULL; }
        if (browser.pending.len + (size_t)n > CDP_LIMIT) {
            snprintf(error, cap, "browser response exceeded 32 MiB transport bound"); return NULL;
        }
        jbuf_append_len(&browser.pending, chunk, (size_t)n);
    }
}

static void observe_event(const char *message) {
    char *method = json_get_str(message, "method");
    if (method && !strcmp(method, "Page.lifecycleEvent")) {
        char *params = json_get_raw(message, "params");
        char *name = params ? json_get_str(params, "name") : NULL;
        if (name && !strcmp(name, "load")) {
            char *loader = json_get_str(params, "loaderId");
            char *session = json_get_str(message, "sessionId");
            if (loader && session) {
                snprintf(browser.load_id, sizeof(browser.load_id), "%s", loader);
                snprintf(browser.load_session, sizeof(browser.load_session), "%s", session);
            }
            free(loader); free(session);
        }
        free(params); free(name);
    }
    free(method);
}

/* Sequential requests plus ID matching drain unsolicited events and stale
 * timed-out responses without assigning them to a subsequent action. */
static char *cdp(const char *method, const char *params, const char *session,
                 long long deadline, char *error, size_t cap) {
    int id = ++browser.sequence;
    jbuf_t request; jbuf_init(&request, 512);
    jbuf_appendf(&request, "{\"id\":%d,\"method\":", id);
    jbuf_append_json_str(&request, method);
    jbuf_append(&request, ",\"params\":"); jbuf_append(&request, params ? params : "{}");
    if (session) { jbuf_append(&request, ",\"sessionId\":"); jbuf_append_json_str(&request, session); }
    jbuf_append(&request, "}");
    size_t sent = 0, total = request.len + 1;
    while (sent < total) {
        if (!wait_fd(browser.tx, POLLOUT, deadline, error, cap)) { jbuf_free(&request); return NULL; }
        ssize_t n = pipe_write(browser.tx, request.data + sent, total - sent);
        if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (n <= 0) { snprintf(error, cap, "cannot write owned browser pipe"); jbuf_free(&request); return NULL; }
        sent += (size_t)n;
    }
    jbuf_free(&request);
    for (;;) {
        char *message = read_message(deadline, error, cap);
        if (!message) return NULL;
        if (json_get_int(message, "id", -1) == id) {
            char *problem = json_get_raw(message, "error");
            if (problem) {
                snprintf(error, cap, "CDP %s: %.300s", method, problem);
                free(problem); free(message); return NULL;
            }
            char *result = json_get_raw(message, "result"); free(message);
            if (!result) snprintf(error, cap, "CDP response omitted result");
            return result;
        }
        observe_event(message); free(message);
    }
}

static char *parameter(const char *key, const char *value) {
    jbuf_t b; jbuf_init(&b, 128); jbuf_append_char(&b, '{');
    jbuf_append_json_str(&b, key); jbuf_append_char(&b, ':');
    jbuf_append_json_str(&b, value); jbuf_append_char(&b, '}'); return b.data;
}

static char *evaluate(const char *expression, const char *session, long long deadline,
                      char *error, size_t cap) {
    jbuf_t p; jbuf_init(&p, 512); jbuf_append(&p, "{\"expression\":");
    jbuf_append_json_str(&p, expression);
    jbuf_appendf(&p, ",\"returnByValue\":true,\"awaitPromise\":true,\"timeout\":%lld}",
                 deadline > clock_ms() ? deadline - clock_ms() : 1);
    char *reply = cdp("Runtime.evaluate", p.data, session, deadline, error, cap); jbuf_free(&p);
    if (!reply) return NULL;
    char *exception = json_get_raw(reply, "exceptionDetails");
    if (exception) {
        snprintf(error, cap, "JavaScript exception: %.350s", exception); free(exception); free(reply); return NULL;
    }
    char *object = json_get_raw(reply, "result"); free(reply);
    char *value = object ? json_get_raw(object, "value") : NULL; free(object);
    return value ? value : strdup("null");
}

static const char *chrome_path(void) {
    const char *configured = getenv("DSCO_CHROME_PATH");
    if (configured && *configured) return access(configured, X_OK) == 0 ? configured : NULL;
    static const char *paths[] = {
        "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
        "/Applications/Chromium.app/Contents/MacOS/Chromium",
        "/usr/bin/google-chrome", "/usr/bin/chromium", "/usr/bin/chromium-browser", NULL};
    for (int i = 0; paths[i]; i++) if (!access(paths[i], X_OK)) return paths[i];
    return NULL;
}

static bool launch(bool headless, bool offline, char *error, size_t cap) {
    const char *binary = chrome_path();
    if (!binary) { snprintf(error, cap, "Chrome unavailable; configure DSCO_CHROME_PATH"); return false; }
    char profile[] = "/tmp/dsco-browser-XXXXXX";
    if (!mkdtemp(profile)) { snprintf(error, cap, "cannot create isolated browser profile"); return false; }
    snprintf(browser.profile, sizeof(browser.profile), "%s", profile);
    int in[2] = {-1,-1}, out[2] = {-1,-1}, fds[4] = {-1,-1,-1,-1};
    if (pipe(in) || pipe(out)) goto failed;
    /* Move all sources above fd4 so dup actions cannot overwrite each other. */
    int originals[] = {in[0],in[1],out[0],out[1]};
    for (int i = 0; i < 4; i++) {
        fds[i] = fcntl(originals[i], F_DUPFD_CLOEXEC, 10);
        if (fds[i] < 0) goto failed;
    }
    for (int i = 0; i < 2; i++) { close(in[i]); in[i] = -1; close(out[i]); out[i] = -1; }
    posix_spawn_file_actions_t actions; posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, fds[0], 3);
    posix_spawn_file_actions_adddup2(&actions, fds[3], 4);
    for (int i = 0; i < 4; i++) posix_spawn_file_actions_addclose(&actions, fds[i]);
    posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&actions, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&actions, 2, "/dev/null", O_WRONLY, 0);
    posix_spawnattr_t attrs; posix_spawnattr_init(&attrs);
    short flags = POSIX_SPAWN_SETPGROUP;
#ifdef POSIX_SPAWN_CLOEXEC_DEFAULT
    flags |= POSIX_SPAWN_CLOEXEC_DEFAULT;
#endif
    posix_spawnattr_setflags(&attrs, flags); posix_spawnattr_setpgroup(&attrs, 0);
    char profile_arg[PATH_MAX + 32], home_env[PATH_MAX + 8];
    snprintf(profile_arg, sizeof(profile_arg), "--user-data-dir=%s", profile);
    /* Preserve the operator's HOME. Isolation is Chrome's --user-data-dir;
     * never repurpose HOME or manipulate a user/system keychain. */
    const char *operator_home = getenv("HOME");
    snprintf(home_env, sizeof(home_env), "HOME=%s", operator_home ? operator_home : "");
    char *argv[24] = {(char *)binary, "--remote-debugging-pipe", profile_arg,
        "--no-first-run", "--no-default-browser-check", "--disable-background-networking",
        "--disable-component-update", "--disable-sync", "--disable-extensions",
        "--disable-default-apps", "--metrics-recording-only", "--disable-breakpad",
        "--password-store=basic", "--window-size=1280,900", NULL};
    int at = 14; if (headless) argv[at++] = "--headless=new";
    if (offline) {
        argv[at++] = "--host-resolver-rules=MAP * ~NOTFOUND";
        argv[at++] = "--proxy-server=http://127.0.0.1:9";
        argv[at++] = "--proxy-bypass-list=<-loopback>";
    }
    argv[at++] = "about:blank"; argv[at] = NULL;
    char *env[] = {"PATH=/usr/bin:/bin:/usr/sbin:/sbin", home_env,
                   "TMPDIR=/tmp", "LANG=en_US.UTF-8", NULL};
    int rc = posix_spawn(&browser.pid, binary, &actions, &attrs, argv, env);
    posix_spawn_file_actions_destroy(&actions); posix_spawnattr_destroy(&attrs);
    if (rc) { errno = rc; browser.pid = 0; goto failed; }
    close(fds[0]); close(fds[3]);
    browser.tx = fds[1]; browser.rx = fds[2];
    fcntl(browser.tx, F_SETFL, O_NONBLOCK); fcntl(browser.rx, F_SETFL, O_NONBLOCK);
    browser.headless = headless; browser.offline = offline;
    snprintf(browser.id, sizeof(browser.id), "browser-%ld-%lu-%lld", (long)browser.pid, ++generation, clock_ms());
    jbuf_init(&browser.pending, 16384); pthread_once(&cleanup_once, register_cleanup);
    return true;
failed:
    for (int i = 0; i < 2; i++) { if (in[i] >= 0) close(in[i]); if (out[i] >= 0) close(out[i]); }
    for (int i = 0; i < 4; i++) if (fds[i] >= 0) close(fds[i]);
    snprintf(error, cap, "cannot launch isolated browser: %s", strerror(errno)); dispose(); return false;
}

typedef struct { jbuf_t *out; int count; } list_ctx_t;
static void append_tab(const char *element, void *opaque) {
    list_ctx_t *ctx = opaque;
    char *type = json_get_str(element, "type");
    if (!type || strcmp(type, "page") || ctx->count >= 128) { free(type); return; }
    free(type);
    char *id = json_get_str(element, "targetId"), *url = json_get_str(element, "url"),
         *title = json_get_str(element, "title");
    if (ctx->count++) jbuf_append_char(ctx->out, ',');
    jbuf_append(ctx->out, "{\"tab_id\":"); jbuf_append_json_str(ctx->out, id ? id : "");
    jbuf_append(ctx->out, ",\"title\":"); jbuf_append_json_str(ctx->out, title ? title : "");
    jbuf_append(ctx->out, ",\"url\":"); jbuf_append_json_str(ctx->out, url ? url : "");
    jbuf_append_char(ctx->out, '}'); free(id); free(url); free(title);
}

static char *tabs(long long deadline, char *error, size_t cap) {
    char *reply = cdp("Target.getTargets", "{}", NULL, deadline, error, cap);
    if (!reply) return NULL;
    jbuf_t b; jbuf_init(&b, 1024); jbuf_append(&b, "{\"tabs\":[");
    list_ctx_t ctx = {.out = &b}; json_array_foreach(reply, "targetInfos", append_tab, &ctx);
    jbuf_append(&b, "],\"limit\":128}"); free(reply); return b.data;
}

static char *attach(const char *tab, long long deadline, char *error, size_t cap) {
    char *p = parameter("targetId", tab);
    char *info = cdp("Target.getTargetInfo", p, NULL, deadline, error, cap);
    if (!info) { free(p); return NULL; }
    char *target = json_get_raw(info, "targetInfo"), *type = target ? json_get_str(target, "type") : NULL;
    bool page = type && !strcmp(type, "page"); free(info); free(target); free(type);
    if (!page) { free(p); snprintf(error, cap, "tab_id must identify a page in this owned browser"); return NULL; }
    p[strlen(p)-1] = 0;
    jbuf_t b; jbuf_init(&b, 256); jbuf_append(&b, p); jbuf_append(&b, ",\"flatten\":true}"); free(p);
    char *reply = cdp("Target.attachToTarget", b.data, NULL, deadline, error, cap); jbuf_free(&b);
    char *session = reply ? json_get_str(reply, "sessionId") : NULL; free(reply);
    return session;
}

static char *navigate(const char *url, const char *session, long long deadline, char *error, size_t cap) {
    if (strncmp(url, "https://", 8) && strncmp(url, "http://", 7) && strncmp(url, "data:", 5) && strcmp(url, "about:blank")) {
        snprintf(error, cap, "navigate supports http, https, data and about:blank URLs"); return NULL;
    }
    char *reply = cdp("Page.enable", "{}", session, deadline, error, cap);
    if (!reply) return NULL; free(reply);
    reply = cdp("Page.setLifecycleEventsEnabled", "{\"enabled\":true}", session, deadline, error, cap);
    if (!reply) return NULL; free(reply);
    browser.load_id[0] = browser.load_session[0] = 0;
    char *p = parameter("url", url); reply = cdp("Page.navigate", p, session, deadline, error, cap); free(p);
    if (!reply) return NULL;
    char *problem = json_get_str(reply, "errorText"), *loader = json_get_str(reply, "loaderId");
    if (problem && *problem) { snprintf(error, cap, "navigation failed: %.300s", problem); free(problem); free(loader); free(reply); return NULL; }
    free(problem);
    while (loader && (strcmp(browser.load_id, loader) || strcmp(browser.load_session, session))) {
        char *event = read_message(deadline, error, cap);
        if (!event) { free(loader); free(reply); return NULL; }
        observe_event(event); free(event);
    }
    free(loader); free(reply);
    return evaluate("({url:location.href,title:document.title,ready_state:document.readyState})", session, deadline, error, cap);
}

static void append_ax(const char *element, void *opaque) {
    list_ctx_t *ctx = opaque;
    if (ctx->count >= 160 || json_get_bool(element, "ignored", false)) return;
    char *role = json_get_raw(element, "role"), *name = json_get_raw(element, "name");
    char *rv = role ? json_get_str(role, "value") : NULL, *nv = name ? json_get_str(name, "value") : NULL;
    if (rv) {
        if (nv && strlen(nv) > 512) {
            size_t n = 512; while (n && ((unsigned char)nv[n] & 0xc0) == 0x80) n--;
            nv[n] = 0;
        }
        if (ctx->count++) jbuf_append_char(ctx->out, ',');
        jbuf_append(ctx->out, "{\"role\":"); jbuf_append_json_str(ctx->out, rv);
        jbuf_append(ctx->out, ",\"name\":"); jbuf_append_json_str(ctx->out, nv ? nv : "");
        jbuf_append_char(ctx->out, '}');
    }
    free(role); free(name); free(rv); free(nv);
}

static char *snapshot(int max_chars, const char *session, long long deadline, char *error, size_t cap) {
    jbuf_t js; jbuf_init(&js, 2048);
    jbuf_appendf(&js,
        "(()=>{const limit=%d;const selector=e=>{if(e.id&&document.querySelectorAll('#'+CSS.escape(e.id)).length===1)return '#'+CSS.escape(e.id);"
        "const p=[];while(e&&e.nodeType===1){let s=e.localName;const a=e.parentElement;if(a){const kids=[...a.children].filter(x=>x.localName===s);"
        "if(kids.length>1)s+=':nth-of-type('+(kids.indexOf(e)+1)+')';}p.unshift(s);e=a;}return p.join(' > ')};"
        "return {url:location.href,title:document.title,text:(document.body?.innerText||'').slice(0,limit),"
        "text_truncated:(document.body?.innerText||'').length>limit,"
        "elements:[...document.querySelectorAll('a,button,input,textarea,select,[role],[contenteditable=true]')]"
        ".filter(e=>{const r=e.getBoundingClientRect();return r.width>0&&r.height>0&&getComputedStyle(e).visibility!=='hidden'})"
        ".slice(0,100).map(e=>({selector:selector(e),tag:e.localName,role:e.getAttribute('role'),type:e.getAttribute('type'),"
        "text:(e.innerText||e.getAttribute('aria-label')||e.getAttribute('placeholder')||'').slice(0,160),disabled:!!e.disabled}))};})()", max_chars);
    char *dom = evaluate(js.data, session, deadline, error, cap); jbuf_free(&js);
    if (!dom) return NULL;
    char *ax = cdp("Accessibility.getFullAXTree", "{}", session, deadline, error, cap);
    if (!ax) { free(dom); return NULL; }
    jbuf_t out; jbuf_init(&out, strlen(dom) + 1024); jbuf_append(&out, "{\"dom\":"); jbuf_append(&out, dom);
    jbuf_append(&out, ",\"accessibility\":["); list_ctx_t ctx = {.out = &out};
    json_array_foreach(ax, "nodes", append_ax, &ctx);
    jbuf_append(&out, "],\"element_limit\":100,\"accessibility_limit\":160}");
    free(dom); free(ax); return out.data;
}

static char *interact(const char *selector, const char *text, bool clear, const char *session,
                      long long deadline, char *error, size_t cap) {
    jbuf_t js; jbuf_init(&js, 1024); jbuf_append(&js, "(()=>{const a=document.querySelectorAll(");
    jbuf_append_json_str(&js, selector);
    jbuf_append(&js, ");if(a.length!==1)throw Error('selector must match exactly one element; matched '+a.length);"
        "const e=a[0];if(e.disabled)throw Error('element disabled');e.scrollIntoView({block:'center',inline:'center'});"
        "const r=e.getBoundingClientRect();if(!r.width||!r.height||getComputedStyle(e).visibility==='hidden')throw Error('element not visible');"
        "const hit=document.elementFromPoint(r.left+r.width/2,r.top+r.height/2);if(!hit||!(hit===e||e.contains(hit)))throw Error('element center is obscured');");
    if (text) {
        jbuf_append(&js, "if(!(e instanceof HTMLInputElement||e instanceof HTMLTextAreaElement||e.isContentEditable)||e.readOnly)throw Error('element is not editable');e.focus();");
        if (clear) jbuf_append(&js, "if(e.isContentEditable){const r=document.createRange();r.selectNodeContents(e);const s=getSelection();s.removeAllRanges();s.addRange(r);}else e.select();");
    }
    jbuf_append(&js, "return {x:r.left+r.width/2,y:r.top+r.height/2}})()");
    char *point = evaluate(js.data, session, deadline, error, cap); jbuf_free(&js);
    if (!point) return NULL;
    char *reply = NULL;
    if (text) {
        char *params = parameter("text", text); reply = cdp("Input.insertText", params, session, deadline, error, cap); free(params);
    } else {
        double x = json_get_double(point, "x", -1), y = json_get_double(point, "y", -1);
        char p[256];
        snprintf(p, sizeof(p), "{\"type\":\"mousePressed\",\"button\":\"left\",\"clickCount\":1,\"x\":%.3f,\"y\":%.3f}", x, y);
        reply = cdp("Input.dispatchMouseEvent", p, session, deadline, error, cap);
        if (reply) {
            free(reply); snprintf(p, sizeof(p), "{\"type\":\"mouseReleased\",\"button\":\"left\",\"clickCount\":1,\"x\":%.3f,\"y\":%.3f}", x, y);
            reply = cdp("Input.dispatchMouseEvent", p, session, deadline, error, cap);
        }
    }
    free(point); return reply;
}

bool tool_browser_session(const char *input, char *out, size_t cap) {
    if (!input || !json_is_valid_container(input)) return fail(out, cap, "expected a JSON object");
    char *action = json_get_str(input, "action"), *id = json_get_str(input, "session_id"),
         *tab = json_get_str(input, "tab_id");
    if (!action) { free(id); free(tab); return fail(out, cap, "action is required"); }
    int timeout = json_get_int(input, "timeout_ms", 15000), max_chars = json_get_int(input, "max_chars", 8000);
    if (timeout < 100) timeout = 100; if (timeout > 60000) timeout = 60000;
    if (max_chars < 256) max_chars = 256; if (max_chars > 32768) max_chars = 32768;
    pthread_mutex_lock(&browser_mutex);
    char error[512] = "browser operation failed", *value = NULL, *session = NULL;
    long long deadline = clock_ms() + timeout;
    if (browser.pid > 0 && waitpid(browser.pid, NULL, WNOHANG) == browser.pid) dispose();
    if (!strcmp(action, "status")) {
        jbuf_t b; jbuf_init(&b, 256);
        jbuf_appendf(&b, "{\"active\":%s,\"owned\":true,\"headless\":%s,\"offline\":%s,\"process_id\":%ld}", browser.pid > 0 ? "true" : "false", browser.headless ? "true" : "false", browser.offline ? "true" : "false", (long)browser.pid); value = b.data;
    } else if (!strcmp(action, "launch")) {
        if (browser.pid > 0) snprintf(error, sizeof(error), "owned browser already active; use its session_id or close it first");
        else if (launch(json_get_bool(input, "headless", true), json_get_bool(input, "offline", false), error, sizeof(error))) {
            char *version = cdp("Browser.getVersion", "{}", NULL, deadline, error, sizeof(error));
            if (version) { free(version); value = tabs(deadline, error, sizeof(error)); }
            if (!value) dispose();
        }
    } else if (!browser.pid || !id || strcmp(id, browser.id)) {
        snprintf(error, sizeof(error), "an exact active session_id is required");
    } else if (!strcmp(action, "tabs")) {
        value = tabs(deadline, error, sizeof(error));
    } else if (!strcmp(action, "close") && (!tab || !*tab)) {
        dispose(); value = strdup("{\"closed\":true}");
    } else if (!tab || !*tab) {
        snprintf(error, sizeof(error), "an exact tab_id from tabs is required");
    } else if ((session = attach(tab, deadline, error, sizeof(error)))) {
        if (!strcmp(action, "navigate")) {
            char *url = json_get_str(input, "url");
            if (url) value = navigate(url, session, deadline, error, sizeof(error));
            else snprintf(error, sizeof(error), "navigate requires url"); free(url);
        } else if (!strcmp(action, "snapshot")) {
            value = snapshot(max_chars, session, deadline, error, sizeof(error));
        } else if (!strcmp(action, "click") || !strcmp(action, "type")) {
            char *selector = json_get_str(input, "selector"), *text = json_get_str(input, "text");
            if (!selector || (!strcmp(action, "type") && !text)) snprintf(error, sizeof(error), "click/type requires selector; type also requires text");
            else value = interact(selector, !strcmp(action, "type") ? text : NULL, json_get_bool(input, "clear", true), session, deadline, error, sizeof(error));
            free(selector); free(text);
        } else if (!strcmp(action, "evaluate")) {
            char *expression = json_get_str(input, "expression");
            if (expression) value = evaluate(expression, session, deadline, error, sizeof(error));
            else snprintf(error, sizeof(error), "evaluate requires expression"); free(expression);
        } else if (!strcmp(action, "screenshot")) {
            char *reply = cdp("Page.captureScreenshot", "{\"format\":\"png\",\"captureBeyondViewport\":false}", session, deadline, error, sizeof(error));
            char *png = reply ? json_get_str(reply, "data") : NULL;
            if (png && tool_content_add_image_base64(png, "image/png")) value = strdup("{\"image\":true,\"mime_type\":\"image/png\"}");
            else if (png) snprintf(error, sizeof(error), "cannot attach screenshot image content");
            free(png); free(reply);
        } else if (!strcmp(action, "close")) {
            char *p = parameter("targetId", tab); value = cdp("Target.closeTarget", p, NULL, deadline, error, sizeof(error)); free(p);
        } else snprintf(error, sizeof(error), "unknown browser action");
        if (strcmp(action, "close") && clock_ms() < deadline) {
            char ignored[512]; char *p = parameter("sessionId", session);
            char *reply = cdp("Target.detachFromTarget", p, NULL, deadline, ignored, sizeof(ignored)); free(p); free(reply);
        }
    }
    bool ok = value != NULL;
    if (ok) {
        jbuf_t b; jbuf_init(&b, strlen(value) + 256); jbuf_append(&b, "{\"ok\":true,\"session_id\":");
        jbuf_append_json_str(&b, browser.id[0] ? browser.id : (id ? id : ""));
        if (tab) { jbuf_append(&b, ",\"tab_id\":"); jbuf_append_json_str(&b, tab); }
        jbuf_append(&b, ",\"result\":"); jbuf_append(&b, value); jbuf_append_char(&b, '}');
        if (!out || b.len >= cap) { ok = false; snprintf(error, sizeof(error), "browser result exceeds tool output capacity; reduce max_chars or narrow evaluate"); }
        else memcpy(out, b.data, b.len + 1);
        jbuf_free(&b);
    }
    if (!ok) fail(out, cap, error);
    free(value); free(session); pthread_mutex_unlock(&browser_mutex);
    free(action); free(id); free(tab); return ok;
}
