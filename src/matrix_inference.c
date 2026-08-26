#define _POSIX_C_SOURCE 200809L

#include "matrix_inference.h"

#include "json_util.h"
#include "tool_call_normalizer.h"

#include <curl/curl.h>
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define MATRIX_HOST_DEFAULT "agent@100.127.90.76"
#define MATRIX_API_DEFAULT "http://100.127.90.76:1234/v1"
#define MATRIX_TARGET_DEFAULT "qwen35-27b-dense"
#define MATRIX_DRAFT_ID_DEFAULT "qwen35-2b-draft"
#define MATRIX_DRAFT_MODEL_DEFAULT "qwen35-2b-draft-text"
#define MATRIX_HEALTH_CMD "cat \"$HOME/Library/Application Support/DSCO/matrix-inference/health.json\""
#define MATRIX_LOG_CMD "tail -n %d \"$HOME/Library/Logs/dsco-matrix-lmstudio-watchdog.log\" 2>/dev/null || tail -n %d \"$HOME/Library/Logs/dsco-matrix-lmstudio.log\""
#define MATRIX_REPAIR_CMD "launchctl kickstart -k gui/$(id -u)/com.dsco.matrix-lmstudio-watchdog"
#define MATRIX_WATCHDOG_CHECK_CMD "launchctl print gui/$(id -u)/com.dsco.matrix-lmstudio-watchdog >/dev/null"
#define MATRIX_TAILNET_CHECK_CMD "\"/Applications/Tailscale.app/Contents/MacOS/Tailscale\" serve status | grep -Fq \"100.127.90.76:1234\""

typedef struct {
    long status;
    CURLcode curl_rc;
    jbuf_t body;
} matrix_http_result_t;

static const char *matrix_env(const char *name, const char *fallback) {
    const char *value = getenv(name);
    return value && value[0] ? value : fallback;
}

static bool matrix_safe_host(const char *host) {
    if (!host || !host[0]) return false;
    for (const unsigned char *p = (const unsigned char *)host; *p; p++) {
        if (!isalnum(*p) && !strchr("@._:-", *p)) return false;
    }
    return true;
}

static size_t matrix_http_write(char *ptr, size_t size, size_t nmemb, void *ctx) {
    size_t n = size * nmemb;
    jbuf_append_len((jbuf_t *)ctx, ptr, n);
    return n;
}

static matrix_http_result_t matrix_http(const char *path, const char *payload,
                                        long timeout_s) {
    matrix_http_result_t result = {.status = 0, .curl_rc = CURLE_FAILED_INIT};
    jbuf_init(&result.body, 4096);
    CURL *curl = curl_easy_init();
    if (!curl) return result;

    jbuf_t url;
    jbuf_init(&url, 256);
    jbuf_append(&url, matrix_env("DSCO_MATRIX_API_BASE", MATRIX_API_DEFAULT));
    jbuf_append(&url, path);
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_URL, url.data);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 8L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_s);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, matrix_http_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &result.body);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    if (payload) {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    }
    result.curl_rc = curl_easy_perform(curl);
    if (result.curl_rc == CURLE_OK)
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result.status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    jbuf_free(&url);
    return result;
}

static void matrix_http_free(matrix_http_result_t *result) {
    if (result) jbuf_free(&result->body);
}

static int matrix_ssh_capture(const char *command, jbuf_t *output) {
    const char *host = matrix_env("DSCO_MATRIX_HOST", MATRIX_HOST_DEFAULT);
    if (!matrix_safe_host(host)) {
        jbuf_append(output, "invalid DSCO_MATRIX_HOST");
        return 2;
    }
    int fds[2];
    if (pipe(fds) != 0) return 2;
    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]); close(fds[1]);
        return 2;
    }
    if (pid == 0) {
        dup2(fds[1], STDOUT_FILENO);
        dup2(fds[1], STDERR_FILENO);
        close(fds[0]); close(fds[1]);
        execlp("ssh", "ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=8",
               host, command, (char *)NULL);
        _exit(127);
    }
    close(fds[1]);
    char buf[4096];
    ssize_t n;
    while ((n = read(fds[0], buf, sizeof(buf))) > 0)
        jbuf_append_len(output, buf, (size_t)n);
    close(fds[0]);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(status) ? WEXITSTATUS(status) : 2;
}

static bool matrix_http_ok(const matrix_http_result_t *r) {
    return r && r->curl_rc == CURLE_OK && r->status >= 200 && r->status < 300;
}

static bool matrix_models_have(const matrix_http_result_t *models, const char *id) {
    return matrix_http_ok(models) && id && strstr(models->body.data, id) != NULL;
}

bool matrix_inference_canary_response_ok(const char *response) {
    normalized_tool_calls_t calls;
    if (!tool_calls_normalize(response, &calls)) return false;
    bool ok = false;
    for (size_t i = 0; i < calls.count; i++) {
        if (!strcmp(calls.calls[i].name, "health_probe") &&
            !strcmp(calls.calls[i].arguments, "{}")) {
            ok = true;
            break;
        }
    }
    tool_calls_normalized_free(&calls);
    return ok;
}

static int matrix_status(bool json) {
    const char *target = matrix_env("DSCO_MATRIX_TARGET_MODEL", MATRIX_TARGET_DEFAULT);
    const char *draft = matrix_env("DSCO_MATRIX_DRAFT_ID", MATRIX_DRAFT_ID_DEFAULT);
    matrix_http_result_t models = matrix_http("/models", NULL, 10L);
    jbuf_t health;
    jbuf_init(&health, 2048);
    int ssh_rc = matrix_ssh_capture(MATRIX_HEALTH_CMD, &health);
    bool api_ok = matrix_http_ok(&models);
    bool target_ok = matrix_models_have(&models, target);
    bool draft_ok = matrix_models_have(&models, draft);
    bool health_ok = ssh_rc == 0 && health.len > 0 && health.data[0] == '{';

    if (json) {
        printf("{\"ok\":%s,\"endpoint\":", api_ok && target_ok && draft_ok && health_ok ? "true" : "false");
        jbuf_t escaped; jbuf_init(&escaped, 128);
        jbuf_append_json_str(&escaped, matrix_env("DSCO_MATRIX_API_BASE", MATRIX_API_DEFAULT));
        printf("%s,\"api_ready\":%s,\"target_resident\":%s,\"draft_resident\":%s,\"health\":%s}\n",
               escaped.data, api_ok ? "true" : "false", target_ok ? "true" : "false",
               draft_ok ? "true" : "false", health_ok ? health.data : "null");
        jbuf_free(&escaped);
    } else {
        printf("Matrix inference\n");
        printf("  endpoint  %s  %s\n", matrix_env("DSCO_MATRIX_API_BASE", MATRIX_API_DEFAULT), api_ok ? "ready" : "down");
        printf("  target    %-24s %s\n", target, target_ok ? "resident" : "missing");
        printf("  draft     %-24s %s\n", draft, draft_ok ? "resident" : "missing");
        printf("  watchdog  %s\n", health_ok ? "reporting" : "unavailable");
        if (health_ok) printf("%s%s", health.data, health.data[health.len - 1] == '\n' ? "" : "\n");
        else if (health.len) fprintf(stderr, "%s", health.data);
    }
    matrix_http_free(&models);
    jbuf_free(&health);
    return api_ok && target_ok && draft_ok && health_ok ? 0 : 1;
}

static int matrix_doctor(void) {
    const char *target = matrix_env("DSCO_MATRIX_TARGET_MODEL", MATRIX_TARGET_DEFAULT);
    const char *draft = matrix_env("DSCO_MATRIX_DRAFT_ID", MATRIX_DRAFT_ID_DEFAULT);
    matrix_http_result_t models = matrix_http("/models", NULL, 10L);
    jbuf_t health;
    jbuf_init(&health, 2048);
    int ssh_rc = matrix_ssh_capture(MATRIX_HEALTH_CMD, &health);
    jbuf_t watchdog_check, tailnet_check;
    jbuf_init(&watchdog_check, 256);
    jbuf_init(&tailnet_check, 256);
    int watchdog_rc = matrix_ssh_capture(MATRIX_WATCHDOG_CHECK_CMD, &watchdog_check);
    int tailnet_rc = matrix_ssh_capture(MATRIX_TAILNET_CHECK_CMD, &tailnet_check);
    char *state = ssh_rc == 0 ? json_get_str(health.data, "status") : NULL;
    struct { const char *name; bool ok; } checks[] = {
        {"tailnet API reachable", matrix_http_ok(&models)},
        {"SSH control channel", ssh_rc == 0},
        {"structured watchdog health", ssh_rc == 0 && health.len && health.data[0] == '{'},
        {"27B target resident", matrix_models_have(&models, target)},
        {"2B draft resident", matrix_models_have(&models, draft)},
        {"launchd watchdog loaded", watchdog_rc == 0},
        {"tailnet-only proxy published", tailnet_rc == 0},
        {"watchdog state valid", state && (!strcmp(state, "ready") || !strcmp(state, "busy"))},
    };
    int failures = 0;
    for (size_t i = 0; i < sizeof(checks) / sizeof(checks[0]); i++) {
        printf("  %s  %s\n", checks[i].ok ? "PASS" : "FAIL", checks[i].name);
        if (!checks[i].ok) failures++;
    }
    printf("doctor: %zu checks, %d failed%s%s\n",
           sizeof(checks) / sizeof(checks[0]), failures,
           state ? ", state=" : "", state ? state : "");
    free(state);
    matrix_http_free(&models);
    jbuf_free(&health);
    jbuf_free(&watchdog_check);
    jbuf_free(&tailnet_check);
    return failures ? 1 : 0;
}

static int matrix_canary(void) {
    jbuf_t health;
    jbuf_init(&health, 2048);
    int ssh_rc = matrix_ssh_capture(MATRIX_HEALTH_CMD, &health);
    char *state = ssh_rc == 0 ? json_get_str(health.data, "status") : NULL;
    if (state && !strcmp(state, "busy")) {
        fprintf(stderr, "matrix canary: refused while production inference is busy\n");
        free(state); jbuf_free(&health);
        return 3;
    }
    free(state); jbuf_free(&health);

    const char *target = matrix_env("DSCO_MATRIX_TARGET_MODEL", MATRIX_TARGET_DEFAULT);
    const char *draft = matrix_env("DSCO_MATRIX_DRAFT_MODEL", MATRIX_DRAFT_MODEL_DEFAULT);
    jbuf_t payload;
    jbuf_init(&payload, 1024);
    jbuf_append(&payload, "{\"model\":"); jbuf_append_json_str(&payload, target);
    jbuf_append(&payload, ",\"draft_model\":"); jbuf_append_json_str(&payload, draft);
    jbuf_append(&payload, ",\"messages\":[{\"role\":\"user\",\"content\":\"Call health_probe with no arguments. Do not answer in prose.\"}],\"tools\":[{\"type\":\"function\",\"function\":{\"name\":\"health_probe\",\"description\":\"Return appliance health.\",\"parameters\":{\"type\":\"object\",\"properties\":{}}}}],\"tool_choice\":\"required\",\"temperature\":0,\"max_tokens\":128}");
    matrix_http_result_t response = matrix_http("/chat/completions", payload.data, 180L);
    bool ok = matrix_http_ok(&response) && matrix_inference_canary_response_ok(response.body.data);
    if (ok) printf("canary: PASS (forced tool call via target=%s draft=%s)\n", target, draft);
    else {
        fprintf(stderr, "canary: FAIL (HTTP %ld, curl=%s)\n%s\n", response.status,
                curl_easy_strerror(response.curl_rc), response.body.data);
    }
    matrix_http_free(&response);
    jbuf_free(&payload);
    return ok ? 0 : 1;
}

static int matrix_logs(const char *value) {
    char *end = NULL;
    long lines = value ? strtol(value, &end, 10) : 120;
    if ((value && (!end || *end)) || lines < 1 || lines > 1000) {
        fprintf(stderr, "matrix logs: line count must be 1..1000\n");
        return 2;
    }
    char command[256];
    snprintf(command, sizeof(command), MATRIX_LOG_CMD, (int)lines, (int)lines);
    jbuf_t output; jbuf_init(&output, 8192);
    int rc = matrix_ssh_capture(command, &output);
    if (output.len) fwrite(output.data, 1, output.len, stdout);
    jbuf_free(&output);
    return rc;
}

static int matrix_repair(void) {
    jbuf_t output; jbuf_init(&output, 2048);
    int rc = matrix_ssh_capture(MATRIX_REPAIR_CMD, &output);
    if (output.len) fwrite(output.data, 1, output.len, rc ? stderr : stdout);
    jbuf_free(&output);
    if (rc != 0) return rc;
    sleep(2);
    return matrix_status(false);
}

static void matrix_usage(FILE *out, const char *prog) {
    fprintf(out, "Usage: %s matrix <status|doctor|canary|logs|repair> [args]\n"
                 "  status [--json]  Show API, resident models, and watchdog health\n"
                 "  doctor           Run non-mutating appliance checks\n"
                 "  canary           Force a real speculative-decoding tool call when idle\n"
                 "  logs [N]         Tail 1..1000 appliance log lines (default 120)\n"
                 "  repair           Kick the watchdog and re-check status\n", prog);
}

int matrix_inference_cli(int argc, char **argv) {
    if (argc < 3 || !strcmp(argv[2], "help") || !strcmp(argv[2], "--help")) {
        matrix_usage(argc > 0 ? stdout : stderr, argc > 0 ? argv[0] : "dsco");
        return argc < 3 ? 2 : 0;
    }
    if (!strcmp(argv[2], "status"))
        return matrix_status(argc >= 4 && !strcmp(argv[3], "--json"));
    if (!strcmp(argv[2], "doctor")) return matrix_doctor();
    if (!strcmp(argv[2], "canary")) return matrix_canary();
    if (!strcmp(argv[2], "logs")) return matrix_logs(argc >= 4 ? argv[3] : NULL);
    if (!strcmp(argv[2], "repair")) return matrix_repair();
    matrix_usage(stderr, argv[0]);
    return 2;
}
