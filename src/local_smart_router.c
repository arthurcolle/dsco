#include "local_smart_router.h"
#include "yyjson.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <curl/curl.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define ROUTER_MAX_HEADER (64 * 1024)
#define ROUTER_MAX_BODY (8 * 1024 * 1024)
#define ROUTER_IO_CHUNK (32 * 1024)

typedef struct {
    uint16_t listen_port;
    uint16_t fast_port;
    uint16_t smart_port;
    size_t smart_threshold;
} router_config_t;

typedef struct {
    int fd;
    router_config_t cfg;
} client_arg_t;

typedef struct {
    int client_fd;
    dsco_local_lane_t lane;
    int route_score;
    const char *route_reason;
    bool failover;
    long status;
    bool headers_sent;
    char content_type[128];
} proxy_ctx_t;

static volatile sig_atomic_t g_running = 1;
static int g_listen_fd = -1;
static atomic_ulong g_fast_routes = 0;
static atomic_ulong g_smart_routes = 0;
static atomic_ulong g_failovers = 0;

static const char *find_ci(const char *haystack, const char *needle) {
    if (!haystack || !needle || !needle[0])
        return NULL;
    size_t nlen = strlen(needle);
    for (const char *p = haystack; *p; p++) {
        if (strncasecmp(p, needle, nlen) == 0)
            return p;
    }
    return NULL;
}

static bool contains_ci(const char *haystack, const char *needle) {
    return find_ci(haystack, needle) != NULL;
}

static bool contains_ci_n(const char *haystack, size_t haystack_len, const char *needle) {
    size_t needle_len = strlen(needle);
    if (needle_len == 0 || needle_len > haystack_len)
        return false;
    for (size_t i = 0; i + needle_len <= haystack_len; i++) {
        if (strncasecmp(haystack + i, needle, needle_len) == 0)
            return true;
    }
    return false;
}

static bool send_all(int fd, const void *data, size_t len) {
    const char *p = data;
    while (len > 0) {
        ssize_t n = send(fd, p, len, 0);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return false;
        p += n;
        len -= (size_t)n;
    }
    return true;
}

static char *insert_text(const char *src, size_t pos, const char *text) {
    size_t slen = strlen(src);
    size_t tlen = strlen(text);
    if (pos > slen)
        return NULL;
    char *out = malloc(slen + tlen + 1);
    if (!out)
        return NULL;
    memcpy(out, src, pos);
    memcpy(out + pos, text, tlen);
    memcpy(out + pos + tlen, src + pos, slen - pos + 1);
    return out;
}

static long long json_integer_value(const char *json, const char *key, bool *found) {
    *found = false;
    const char *p = strstr(json, key);
    if (!p)
        return 0;
    p = strchr(p + strlen(key), ':');
    if (!p)
        return 0;
    p++;
    while (isspace((unsigned char)*p))
        p++;
    char *end = NULL;
    long long value = strtoll(p, &end, 10);
    if (end == p)
        return 0;
    *found = true;
    return value;
}

const char *dsco_local_lane_name(dsco_local_lane_t lane) {
    return lane == DSCO_LOCAL_LANE_SMART ? "smart" : "fast";
}

static dsco_local_decision_t forced_decision(dsco_local_lane_t lane, const char *reason) {
    return (dsco_local_decision_t){
        .lane = lane,
        .score = lane == DSCO_LOCAL_LANE_SMART ? 100 : -100,
        .reasoning_budget = lane == DSCO_LOCAL_LANE_SMART ? 96 : 0,
        .reason = reason,
        .forced = true,
    };
}

static void add_signal(dsco_local_decision_t *decision, int *dominant,
                       int points, const char *reason) {
    decision->score += points;
    if (points > *dominant) {
        *dominant = points;
        decision->reason = reason;
    }
}

static bool contains_any(const char *text, size_t text_len, const char *const *terms) {
    if (!text)
        return false;
    for (size_t i = 0; terms[i]; i++) {
        if (contains_ci_n(text, text_len, terms[i]))
            return true;
    }
    return false;
}

static long long json_number_or(yyjson_val *object, const char *key, long long fallback) {
    yyjson_val *value = yyjson_obj_get(object, key);
    if (!value || !yyjson_is_num(value))
        return fallback;
    if (yyjson_is_uint(value)) {
        uint64_t number = yyjson_get_uint(value);
        return number > (uint64_t)INT64_MAX ? INT64_MAX : (long long)number;
    }
    if (yyjson_is_real(value)) {
        double number = yyjson_get_real(value);
        if (number >= (double)INT64_MAX)
            return INT64_MAX;
        if (number <= (double)INT64_MIN)
            return INT64_MIN;
        return (long long)number;
    }
    return yyjson_get_sint(value);
}

static int adaptive_reasoning_budget(int score, long long max_tokens) {
    int budget = score >= 10 ? 96 : score >= 6 ? 64 : 48;
    if (max_tokens >= 0) {
        if (max_tokens < 32)
            return 0;
        long long cap = max_tokens / 2;
        if (cap < budget)
            budget = (int)cap;
        if (budget < 16)
            budget = 16;
    }
    return budget;
}

dsco_local_decision_t dsco_local_smart_decide(const char *body, size_t body_len,
                                               const char *override_lane,
                                               size_t smart_threshold) {
    if (override_lane) {
        if (strcasecmp(override_lane, "fast") == 0)
            return forced_decision(DSCO_LOCAL_LANE_FAST, "header-override");
        if (strcasecmp(override_lane, "smart") == 0)
            return forced_decision(DSCO_LOCAL_LANE_SMART, "header-override");
    }

    yyjson_doc *doc = yyjson_read(body, body_len, 0);
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    if (!root || !yyjson_is_obj(root)) {
        if (doc)
            yyjson_doc_free(doc);
        return (dsco_local_decision_t){
            .lane = DSCO_LOCAL_LANE_SMART,
            .score = 4,
            .reasoning_budget = 48,
            .reason = "invalid-json",
            .forced = false,
        };
    }

    yyjson_val *model_value = yyjson_obj_get(root, "model");
    if (model_value && yyjson_is_str(model_value)) {
        const char *model = yyjson_get_str(model_value);
        if (strcasecmp(model, "fast") == 0 || strcasecmp(model, "dsco-fast") == 0 ||
            contains_ci(model, "2b")) {
            yyjson_doc_free(doc);
            return forced_decision(DSCO_LOCAL_LANE_FAST, "model-override");
        }
        if (strcasecmp(model, "smart") == 0 || strcasecmp(model, "dsco-smart") == 0 ||
            contains_ci(model, "qwythos") || contains_ci(model, "9b")) {
            yyjson_doc_free(doc);
            return forced_decision(DSCO_LOCAL_LANE_SMART, "model-override");
        }
    }

    yyjson_val *kwargs = yyjson_obj_get(root, "chat_template_kwargs");
    yyjson_val *thinking = kwargs && yyjson_is_obj(kwargs) ?
                              yyjson_obj_get(kwargs, "enable_thinking") : NULL;
    if (thinking && yyjson_is_true(thinking)) {
        yyjson_doc_free(doc);
        return forced_decision(DSCO_LOCAL_LANE_SMART, "thinking-requested");
    }

    dsco_local_decision_t decision = {
        .lane = DSCO_LOCAL_LANE_FAST,
        .score = 0,
        .reasoning_budget = 0,
        .reason = "default-fast",
        .forced = false,
    };
    int dominant = 0;
    const char *signal = NULL;
    size_t signal_len = 0;
    int assistant_turns = 0;
    bool tool_history = false;
    bool multimodal = false;

    yyjson_val *messages = yyjson_obj_get(root, "messages");
    if (messages && yyjson_is_arr(messages)) {
        size_t index, max;
        yyjson_val *message;
        yyjson_arr_foreach(messages, index, max, message) {
            if (!yyjson_is_obj(message))
                continue;
            yyjson_val *role_value = yyjson_obj_get(message, "role");
            const char *role = role_value && yyjson_is_str(role_value) ?
                                   yyjson_get_str(role_value) : "";
            if (strcasecmp(role, "assistant") == 0) {
                assistant_turns++;
                yyjson_val *calls = yyjson_obj_get(message, "tool_calls");
                if (calls && yyjson_is_arr(calls) && yyjson_arr_size(calls) > 0)
                    tool_history = true;
            } else if (strcasecmp(role, "tool") == 0) {
                tool_history = true;
            } else if (strcasecmp(role, "user") == 0) {
                yyjson_val *content = yyjson_obj_get(message, "content");
                if (content && yyjson_is_str(content)) {
                    signal = yyjson_get_str(content);
                    signal_len = yyjson_get_len(content);
                } else if (content && yyjson_is_arr(content)) {
                    size_t part_index, part_max;
                    yyjson_val *part;
                    yyjson_arr_foreach(content, part_index, part_max, part) {
                        if (!yyjson_is_obj(part))
                            continue;
                        yyjson_val *type_value = yyjson_obj_get(part, "type");
                        const char *type = type_value && yyjson_is_str(type_value) ?
                                               yyjson_get_str(type_value) : "";
                        if (contains_ci(type, "image"))
                            multimodal = true;
                        yyjson_val *text_value = yyjson_obj_get(part, "text");
                        if (text_value && yyjson_is_str(text_value) &&
                            yyjson_get_len(text_value) >= signal_len) {
                            signal = yyjson_get_str(text_value);
                            signal_len = yyjson_get_len(text_value);
                        }
                    }
                }
            }
        }
    }

    yyjson_val *tools = yyjson_obj_get(root, "tools");
    bool has_tools = tools && yyjson_is_arr(tools) && yyjson_arr_size(tools) > 0;
    yyjson_val *tool_choice = yyjson_obj_get(root, "tool_choice");
    if (tool_choice && ((yyjson_is_str(tool_choice) &&
                         strcasecmp(yyjson_get_str(tool_choice), "required") == 0) ||
                        yyjson_is_obj(tool_choice)))
        add_signal(&decision, &dominant, 6, "forced-tool");

    yyjson_val *response_format = yyjson_obj_get(root, "response_format");
    if (response_format && yyjson_is_obj(response_format)) {
        yyjson_val *type_value = yyjson_obj_get(response_format, "type");
        const char *type = type_value && yyjson_is_str(type_value) ?
                               yyjson_get_str(type_value) : "";
        if (contains_ci(type, "json") || yyjson_obj_get(response_format, "json_schema"))
            add_signal(&decision, &dominant, 4, "structured-output");
    }
    if (multimodal)
        add_signal(&decision, &dominant, 6, "multimodal");
    if (tool_history)
        add_signal(&decision, &dominant, 6, "tool-continuation");
    else if (assistant_turns >= 2)
        add_signal(&decision, &dominant, 2, "multi-turn");
    if (!signal && has_tools)
        add_signal(&decision, &dominant, 4, "tools-without-user");

    static const char *const code_terms[] = {
        "```", "debug", "diagnos", "implement", "refactor", "compile", "stack trace",
        "root cause", "patch", "codebase", "segfault", "exception", NULL,
    };
    static const char *const reasoning_terms[] = {
        "analyz", "architect", "multi-step", "reason", "prove", "derive", "calculate",
        "compare", "review", "trade-off", "tradeoff", "step by step", "evaluate", NULL,
    };
    static const char *const safety_terms[] = {
        "security", "vulnerab", "financial", "legal", "medical", "compliance", "risk", NULL,
    };
    static const char *const agentic_terms[] = {
        "fix ", "fix it", "edit ", "write a file", "run the", "run this", "build ",
        "test it", "search the repo", "inspect the repo", "make it", "deploy", NULL,
    };
    static const char *const simple_terms[] = {
        "reply exactly", "say exactly", "hello", "thanks", "thank you", NULL,
    };

    if (signal) {
        if (signal_len >= smart_threshold)
            add_signal(&decision, &dominant, 6, "long-request");
        else if (signal_len >= 768)
            add_signal(&decision, &dominant, 3, "long-request");
        else if (signal_len >= 384)
            add_signal(&decision, &dominant, 2, "long-request");

        if (contains_any(signal, signal_len, code_terms))
            add_signal(&decision, &dominant, 5, "code-task");
        if (contains_any(signal, signal_len, reasoning_terms))
            add_signal(&decision, &dominant, 4, "reasoning-task");
        if (contains_any(signal, signal_len, safety_terms))
            add_signal(&decision, &dominant, 6, "high-stakes");
        if (contains_any(signal, signal_len, agentic_terms))
            add_signal(&decision, &dominant, 5, "agentic-task");

        int questions = 0;
        int newlines = 0;
        for (size_t i = 0; i < signal_len; i++) {
            questions += signal[i] == '?';
            newlines += signal[i] == '\n';
        }
        if (questions >= 2)
            add_signal(&decision, &dominant, 2, "multi-question");
        if (newlines >= 8)
            add_signal(&decision, &dominant, 2, "structured-request");

        if (decision.score == 0 && signal_len < 256 &&
            contains_any(signal, signal_len, simple_terms)) {
            decision.score = -4;
            decision.reason = "simple-direct";
            dominant = 4;
        }
    }

    long long max_tokens = json_number_or(root, "max_tokens", -1);
    if (max_tokens < 0)
        max_tokens = json_number_or(root, "max_completion_tokens", -1);
    if (max_tokens >= 1024)
        add_signal(&decision, &dominant, 2, "long-generation");
    else if (max_tokens >= 256)
        add_signal(&decision, &dominant, 1, "long-generation");

    decision.lane = decision.score >= 4 ? DSCO_LOCAL_LANE_SMART : DSCO_LOCAL_LANE_FAST;
    if (decision.lane == DSCO_LOCAL_LANE_SMART) {
        decision.reasoning_budget = adaptive_reasoning_budget(decision.score, max_tokens);
        if (strcmp(decision.reason, "default-fast") == 0)
            decision.reason = "combined-complexity";
    }
    long long explicit_budget = json_number_or(root, "reasoning_budget_tokens", -1);
    if (explicit_budget >= 0 && explicit_budget <= INT_MAX)
        decision.reasoning_budget = (int)explicit_budget;
    yyjson_doc_free(doc);
    return decision;
}

dsco_local_lane_t dsco_local_smart_classify(const char *body, size_t body_len,
                                            const char *override_lane,
                                            size_t smart_threshold) {
    return dsco_local_smart_decide(body, body_len, override_lane, smart_threshold).lane;
}

static bool apply_failover(dsco_local_decision_t *decision,
                           bool selected_up, bool alternate_up) {
    if (!decision || decision->forced || selected_up || !alternate_up)
        return false;
    decision->lane = decision->lane == DSCO_LOCAL_LANE_SMART ?
                         DSCO_LOCAL_LANE_FAST : DSCO_LOCAL_LANE_SMART;
    decision->reason = decision->lane == DSCO_LOCAL_LANE_SMART ?
                           "fast-unavailable" : "smart-unavailable";
    decision->reasoning_budget = decision->lane == DSCO_LOCAL_LANE_SMART ? 48 : 0;
    return true;
}

static char *patch_body_with_budget(const char *body, dsco_local_lane_t lane, int budget) {
    if (!body)
        return NULL;
    while (isspace((unsigned char)*body))
        body++;
    if (*body != '{')
        return strdup(body);

    bool has_cache = strstr(body, "\"cache_prompt\"") != NULL;
    bool has_kwargs = strstr(body, "\"chat_template_kwargs\"") != NULL;
    bool has_thinking = strstr(body, "\"enable_thinking\"") != NULL;
    bool has_budget = strstr(body, "\"reasoning_budget_tokens\"") != NULL;
    const char *thinking = lane == DSCO_LOCAL_LANE_SMART ? "true" : "false";

    char prefix[224] = "";
    size_t used = 0;
    if (!has_cache)
        used += (size_t)snprintf(prefix + used, sizeof(prefix) - used,
                                 "\"cache_prompt\":true,");
    if (!has_kwargs)
        used += (size_t)snprintf(prefix + used, sizeof(prefix) - used,
                                 "\"chat_template_kwargs\":{\"enable_thinking\":%s},",
                                 thinking);
    if (lane == DSCO_LOCAL_LANE_SMART && !has_budget)
        used += (size_t)snprintf(prefix + used, sizeof(prefix) - used,
                                 "\"reasoning_budget_tokens\":%d,", budget);

    if (used > 0 && prefix[used - 1] == ',') {
        prefix[--used] = '\0';
        const char *existing = body + 1;
        while (isspace((unsigned char)*existing))
            existing++;
        if (*existing != '}') {
            prefix[used++] = ',';
            prefix[used] = '\0';
        }
    }

    char *patched = used ? insert_text(body, 1, prefix) : strdup(body);
    if (!patched)
        return NULL;

    if (has_kwargs && !has_thinking) {
        const char *key = strstr(patched, "\"chat_template_kwargs\"");
        const char *brace = key ? strchr(key, '{') : NULL;
        if (brace) {
            char field[64];
            const char *existing = brace + 1;
            while (isspace((unsigned char)*existing))
                existing++;
            snprintf(field, sizeof(field), "\"enable_thinking\":%s%s", thinking,
                     *existing == '}' ? "" : ",");
            size_t pos = (size_t)(brace - patched) + 1;
            char *next = insert_text(patched, pos, field);
            if (next) {
                free(patched);
                patched = next;
            }
        }
    }
    return patched;
}

char *dsco_local_smart_patch_body(const char *body, dsco_local_lane_t lane) {
    bool max_found = false;
    long long max_tokens = json_integer_value(body, "\"max_tokens\"", &max_found);
    if (!max_found)
        max_tokens = json_integer_value(body, "\"max_completion_tokens\"", &max_found);
    int budget = adaptive_reasoning_budget(100, max_found ? max_tokens : -1);
    return patch_body_with_budget(body, lane, budget);
}

char *dsco_local_smart_patch_decision(const char *body,
                                      const dsco_local_decision_t *decision) {
    if (!decision)
        return NULL;
    return patch_body_with_budget(body, decision->lane, decision->reasoning_budget);
}

static const char *status_phrase(long status) {
    if (status == 200)
        return "OK";
    if (status == 400)
        return "Bad Request";
    if (status == 404)
        return "Not Found";
    if (status == 500)
        return "Internal Server Error";
    if (status == 502)
        return "Bad Gateway";
    if (status == 503)
        return "Service Unavailable";
    return "Upstream";
}

static bool send_response_headers(proxy_ctx_t *ctx) {
    if (ctx->headers_sent)
        return true;
    long status = ctx->status >= 100 ? ctx->status : 200;
    const char *ctype = ctx->content_type[0] ? ctx->content_type : "application/json";
    char hdr[768];
    int n = snprintf(hdr, sizeof(hdr),
                     "HTTP/1.0 %ld %s\r\n"
                     "Content-Type: %s\r\n"
                     "Cache-Control: no-cache\r\n"
                     "Access-Control-Allow-Origin: *\r\n"
                     "Access-Control-Expose-Headers: X-DSCO-Local-Lane, "
                     "X-DSCO-Route-Score, X-DSCO-Route-Reason, X-DSCO-Failover\r\n"
                     "X-DSCO-Local-Lane: %s\r\n"
                     "X-DSCO-Route-Score: %d\r\n"
                     "X-DSCO-Route-Reason: %s\r\n"
                     "X-DSCO-Failover: %s\r\n"
                     "Connection: close\r\n\r\n",
                     status, status_phrase(status), ctype, dsco_local_lane_name(ctx->lane),
                     ctx->route_score, ctx->route_reason,
                     ctx->failover ? "true" : "false");
    ctx->headers_sent = send_all(ctx->client_fd, hdr, (size_t)n);
    return ctx->headers_sent;
}

static size_t proxy_header_cb(char *data, size_t size, size_t nmemb, void *userdata) {
    size_t len = size * nmemb;
    proxy_ctx_t *ctx = userdata;
    if (len >= 5 && strncasecmp(data, "HTTP/", 5) == 0) {
        const char *space = memchr(data, ' ', len);
        if (space)
            ctx->status = strtol(space + 1, NULL, 10);
    } else if (len >= 13 && strncasecmp(data, "Content-Type:", 13) == 0) {
        const char *p = data + 13;
        const char *end = data + len;
        while (p < end && isspace((unsigned char)*p))
            p++;
        size_t n = (size_t)(end - p);
        while (n && isspace((unsigned char)p[n - 1]))
            n--;
        if (n >= sizeof(ctx->content_type))
            n = sizeof(ctx->content_type) - 1;
        memcpy(ctx->content_type, p, n);
        ctx->content_type[n] = '\0';
    } else if ((len == 2 && data[0] == '\r' && data[1] == '\n') ||
               (len == 1 && data[0] == '\n')) {
        if (ctx->status >= 200)
            (void)send_response_headers(ctx);
    }
    return len;
}

static size_t proxy_write_cb(char *data, size_t size, size_t nmemb, void *userdata) {
    size_t len = size * nmemb;
    proxy_ctx_t *ctx = userdata;
    if (!send_response_headers(ctx) || !send_all(ctx->client_fd, data, len))
        return 0;
    return len;
}

static void send_json(int fd, int status, const char *json) {
    size_t len = strlen(json);
    char hdr[512];
    int n = snprintf(hdr, sizeof(hdr),
                     "HTTP/1.0 %d %s\r\nContent-Type: application/json\r\n"
                     "Content-Length: %zu\r\nAccess-Control-Allow-Origin: *\r\n"
                     "Connection: close\r\n\r\n",
                     status, status_phrase(status), len);
    (void)send_all(fd, hdr, (size_t)n);
    (void)send_all(fd, json, len);
}

static bool backend_up(uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return false;
    struct timeval tv = {.tv_sec = 0, .tv_usec = 250000};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = htonl(0x7f000001u);
    bool ok = connect(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0;
    close(fd);
    return ok;
}

static void header_value(const char *headers, size_t headers_len, const char *name,
                         char *out, size_t out_len) {
    out[0] = '\0';
    size_t name_len = strlen(name);
    const char *p = NULL;
    for (size_t i = 0; i + name_len <= headers_len; i++) {
        if (strncasecmp(headers + i, name, name_len) == 0) {
            p = headers + i;
            break;
        }
    }
    if (!p)
        return;
    const char *end = headers + headers_len;
    p += name_len;
    while (p < end && (*p == ' ' || *p == '\t'))
        p++;
    size_t n = 0;
    while (p + n < end && p[n] != '\r' && p[n] != '\n' && n + 1 < out_len)
        n++;
    memcpy(out, p, n);
    out[n] = '\0';
}

static void *handle_client(void *userdata) {
    client_arg_t *arg = userdata;
    int fd = arg->fd;
    router_config_t cfg = arg->cfg;
    free(arg);

    char *raw = malloc(ROUTER_MAX_HEADER + 1);
    if (!raw) {
        close(fd);
        return NULL;
    }
    size_t used = 0;
    char *header_end = NULL;
    while (used < ROUTER_MAX_HEADER) {
        ssize_t n = recv(fd, raw + used, ROUTER_MAX_HEADER - used, 0);
        if (n <= 0)
            goto done;
        used += (size_t)n;
        raw[used] = '\0';
        header_end = strstr(raw, "\r\n\r\n");
        if (header_end)
            break;
    }
    if (!header_end) {
        send_json(fd, 400, "{\"error\":\"request headers too large\"}");
        goto done;
    }
    size_t header_len = (size_t)(header_end + 4 - raw);

    char method[16] = "";
    char path[256] = "";
    if (sscanf(raw, "%15s %255s", method, path) != 2) {
        send_json(fd, 400, "{\"error\":\"malformed request line\"}");
        goto done;
    }
    char *query = strchr(path, '?');
    if (query)
        *query = '\0';

    if (strcasecmp(method, "GET") == 0 && strcmp(path, "/health") == 0) {
        bool fast = backend_up(cfg.fast_port);
        bool smart = backend_up(cfg.smart_port);
        char body[320];
        snprintf(body, sizeof(body),
                 "{\"status\":\"%s\",\"fast\":%s,\"smart\":%s,"
                 "\"routes\":{\"fast\":%lu,\"smart\":%lu,\"failovers\":%lu}}",
                 fast && smart ? "ok" : "degraded", fast ? "true" : "false",
                 smart ? "true" : "false",
                 atomic_load_explicit(&g_fast_routes, memory_order_relaxed),
                 atomic_load_explicit(&g_smart_routes, memory_order_relaxed),
                 atomic_load_explicit(&g_failovers, memory_order_relaxed));
        send_json(fd, fast && smart ? 200 : 503, body);
        goto done;
    }
    if (strcasecmp(method, "GET") == 0 && strcmp(path, "/v1/models") == 0) {
        send_json(fd, 200,
                  "{\"object\":\"list\",\"data\":["
                  "{\"id\":\"auto\",\"object\":\"model\"},"
                  "{\"id\":\"fast\",\"object\":\"model\"},"
                  "{\"id\":\"smart\",\"object\":\"model\"}]}");
        goto done;
    }
    if (strcasecmp(method, "OPTIONS") == 0) {
        const char *resp = "HTTP/1.0 204 No Content\r\n"
                           "Access-Control-Allow-Origin: *\r\n"
                           "Access-Control-Allow-Headers: Content-Type, X-DSCO-Lane\r\n"
                           "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n\r\n";
        (void)send_all(fd, resp, strlen(resp));
        goto done;
    }
    bool is_chat = strcmp(path, "/v1/chat/completions") == 0;
    bool is_route = strcmp(path, "/v1/route") == 0;
    if (strcasecmp(method, "POST") != 0 || (!is_chat && !is_route)) {
        send_json(fd, 404, "{\"error\":\"not found\"}");
        goto done;
    }

    char length_value[32];
    header_value(raw, header_len, "Content-Length:", length_value, sizeof(length_value));
    unsigned long long parsed = strtoull(length_value, NULL, 10);
    if (!length_value[0] || parsed > ROUTER_MAX_BODY) {
        send_json(fd, 400, "{\"error\":\"missing or oversized Content-Length\"}");
        goto done;
    }
    size_t content_len = (size_t)parsed;
    size_t already = used - header_len;
    char *body = malloc(content_len + 1);
    if (!body)
        goto done;
    if (already > content_len)
        already = content_len;
    memcpy(body, raw + header_len, already);
    while (already < content_len) {
        ssize_t n = recv(fd, body + already, content_len - already, 0);
        if (n <= 0) {
            free(body);
            goto done;
        }
        already += (size_t)n;
    }
    body[content_len] = '\0';

    char override_lane[32];
    header_value(raw, header_len, "X-DSCO-Lane:", override_lane, sizeof(override_lane));
    dsco_local_decision_t decision = dsco_local_smart_decide(
        body, content_len, override_lane[0] ? override_lane : NULL, cfg.smart_threshold);
    if (is_route) {
        char route_body[256];
        snprintf(route_body, sizeof(route_body),
                 "{\"lane\":\"%s\",\"score\":%d,\"reason\":\"%s\","
                 "\"reasoning_budget\":%d,\"forced\":%s}",
                 dsco_local_lane_name(decision.lane), decision.score, decision.reason,
                 decision.reasoning_budget, decision.forced ? "true" : "false");
        send_json(fd, 200, route_body);
        free(body);
        goto done;
    }

    bool failover = false;
    uint16_t selected_port = decision.lane == DSCO_LOCAL_LANE_SMART ?
                                 cfg.smart_port : cfg.fast_port;
    uint16_t alternate_port = decision.lane == DSCO_LOCAL_LANE_SMART ?
                                  cfg.fast_port : cfg.smart_port;
    bool selected_up = backend_up(selected_port);
    bool alternate_up = selected_up ? false : backend_up(alternate_port);
    if (apply_failover(&decision, selected_up, alternate_up)) {
        failover = true;
        atomic_fetch_add_explicit(&g_failovers, 1, memory_order_relaxed);
    }
    if (decision.lane == DSCO_LOCAL_LANE_SMART)
        atomic_fetch_add_explicit(&g_smart_routes, 1, memory_order_relaxed);
    else
        atomic_fetch_add_explicit(&g_fast_routes, 1, memory_order_relaxed);

    char *patched = dsco_local_smart_patch_decision(body, &decision);
    free(body);
    if (!patched) {
        send_json(fd, 500, "{\"error\":\"request patch failed\"}");
        goto done;
    }

    uint16_t upstream_port = decision.lane == DSCO_LOCAL_LANE_SMART ?
                                 cfg.smart_port : cfg.fast_port;
    char url[160];
    snprintf(url, sizeof(url), "http://127.0.0.1:%u/v1/chat/completions", upstream_port);
    proxy_ctx_t proxy = {
        .client_fd = fd,
        .lane = decision.lane,
        .route_score = decision.score,
        .route_reason = decision.reason,
        .failover = failover,
        .status = 0,
        .headers_sent = false,
        .content_type = "",
    };
    CURL *curl = curl_easy_init();
    if (!curl) {
        free(patched);
        send_json(fd, 502, "{\"error\":\"router transport unavailable\"}");
        goto done;
    }
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "Accept: application/json, text/event-stream");
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, patched);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)strlen(patched));
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 2000L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 0L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_TCP_NODELAY, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, proxy_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &proxy);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, proxy_header_cb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &proxy);
    CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK && !proxy.headers_sent) {
        char error[320];
        snprintf(error, sizeof(error), "{\"error\":\"%s backend unavailable: %s\"}",
                 dsco_local_lane_name(decision.lane), curl_easy_strerror(rc));
        send_json(fd, 502, error);
    }
    fprintf(stderr,
            "[local-smart] lane=%s score=%d reason=%s failover=%s bytes=%zu status=%ld\n",
            dsco_local_lane_name(decision.lane), decision.score, decision.reason,
            failover ? "true" : "false", content_len, proxy.status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    free(patched);

done:
    free(raw);
    close(fd);
    return NULL;
}

static void on_signal(int sig) {
    (void)sig;
    g_running = 0;
    if (g_listen_fd >= 0)
        close(g_listen_fd);
    g_listen_fd = -1;
}

static int parse_port(const char *s) {
    char *end = NULL;
    long value = strtol(s, &end, 10);
    return end && *end == '\0' && value > 0 && value < 65536 ? (int)value : -1;
}

static bool set_port(uint16_t *port, const char *value) {
    int parsed = parse_port(value);
    if (parsed < 0)
        return false;
    *port = (uint16_t)parsed;
    return true;
}

static bool set_threshold(size_t *threshold, const char *value) {
    char *end = NULL;
    errno = 0;
    unsigned long long parsed = strtoull(value, &end, 10);
    if (errno || !end || *end != '\0' || parsed == 0 || parsed > SIZE_MAX)
        return false;
    *threshold = (size_t)parsed;
    return true;
}

static int self_test(void) {
    const char *simple = "{\"model\":\"auto\",\"messages\":[{\"role\":\"user\",\"content\":\"hello\"}]}";
    const char *hard = "{\"model\":\"auto\",\"messages\":[{\"role\":\"user\",\"content\":\"debug and prove the root cause\"}]}";
    const char *tools = "{\"model\":\"auto\",\"tools\":[{\"type\":\"function\"}],\"messages\":[]}";
    const char *simple_with_tools =
        "{\"model\":\"auto\",\"messages\":[{\"role\":\"system\",\"content\":"
        "\"analyze and debug tools\"},{\"role\":\"user\",\"content\":\"hello\"}],"
        "\"tools\":[{\"type\":\"function\"}]}";
    const char *structured =
        "{\"model\":\"auto\",\"messages\":[{\"role\":\"user\",\"content\":\"hello\"}],"
        "\"response_format\":{\"type\":\"json_schema\"}}";
    const char *forced_tool =
        "{\"model\":\"auto\",\"messages\":[{\"role\":\"user\",\"content\":\"hello\"}],"
        "\"tools\":[{}],\"tool_choice\":\"required\"}";
    const char *tool_continuation =
        "{\"model\":\"auto\",\"messages\":[{\"role\":\"assistant\","
        "\"tool_calls\":[{}]},{\"role\":\"tool\",\"content\":\"ok\"},"
        "{\"role\":\"user\",\"content\":\"continue\"}]}";
    const char *multimodal =
        "{\"model\":\"auto\",\"messages\":[{\"role\":\"user\",\"content\":["
        "{\"type\":\"text\",\"text\":\"what is this\"},"
        "{\"type\":\"image_url\",\"image_url\":{}}]}]}";
    if (dsco_local_smart_classify(simple, strlen(simple), NULL, 1800) !=
        DSCO_LOCAL_LANE_FAST)
        return 1;
    if (dsco_local_smart_classify(hard, strlen(hard), NULL, 1800) !=
        DSCO_LOCAL_LANE_SMART)
        return 2;
    if (dsco_local_smart_classify(tools, strlen(tools), NULL, 1800) !=
        DSCO_LOCAL_LANE_SMART)
        return 3;
    if (dsco_local_smart_classify(hard, strlen(hard), "fast", 1800) !=
        DSCO_LOCAL_LANE_FAST)
        return 4;
    if (dsco_local_smart_classify(simple_with_tools, strlen(simple_with_tools), NULL, 1800) !=
        DSCO_LOCAL_LANE_FAST)
        return 5;
    dsco_local_decision_t structured_decision =
        dsco_local_smart_decide(structured, strlen(structured), NULL, 1800);
    if (structured_decision.lane != DSCO_LOCAL_LANE_SMART ||
        strcmp(structured_decision.reason, "structured-output") != 0)
        return 6;
    if (dsco_local_smart_classify(forced_tool, strlen(forced_tool), NULL, 1800) !=
        DSCO_LOCAL_LANE_SMART)
        return 7;
    if (dsco_local_smart_classify(tool_continuation, strlen(tool_continuation), NULL, 1800) !=
        DSCO_LOCAL_LANE_SMART)
        return 8;
    if (dsco_local_smart_classify(multimodal, strlen(multimodal), NULL, 1800) !=
        DSCO_LOCAL_LANE_SMART)
        return 9;
    dsco_local_decision_t failover_decision =
        dsco_local_smart_decide(simple, strlen(simple), NULL, 1800);
    if (!apply_failover(&failover_decision, false, true) ||
        failover_decision.lane != DSCO_LOCAL_LANE_SMART ||
        strcmp(failover_decision.reason, "fast-unavailable") != 0)
        return 10;
    dsco_local_decision_t forced_fast =
        dsco_local_smart_decide(hard, strlen(hard), "fast", 1800);
    if (apply_failover(&forced_fast, false, true) ||
        forced_fast.lane != DSCO_LOCAL_LANE_FAST)
        return 11;
    char *fast = dsco_local_smart_patch_body(simple, DSCO_LOCAL_LANE_FAST);
    char *smart = dsco_local_smart_patch_body(simple, DSCO_LOCAL_LANE_SMART);
    char *empty = dsco_local_smart_patch_body("{}", DSCO_LOCAL_LANE_FAST);
    char *empty_kwargs = dsco_local_smart_patch_body(
        "{\"chat_template_kwargs\":{}}", DSCO_LOCAL_LANE_FAST);
    bool ok = fast && smart && strstr(fast, "\"cache_prompt\":true") &&
              strstr(fast, "\"enable_thinking\":false") &&
              strstr(smart, "\"enable_thinking\":true") &&
              strstr(smart, "\"reasoning_budget_tokens\":96") && empty &&
              !strstr(empty, ",}") && empty_kwargs && !strstr(empty_kwargs, ",}");
    free(fast);
    free(smart);
    free(empty);
    free(empty_kwargs);
    if (!ok)
        return 12;
    puts("local-smart-router self-test: ok");
    return 0;
}

int main(int argc, char **argv) {
    router_config_t cfg = {
        .listen_port = 8080,
        .fast_port = 8081,
        .smart_port = 8082,
        .smart_threshold = 1800,
    };
    const char *env;
    if (((env = getenv("DSCO_SMART_PORT")) && !set_port(&cfg.listen_port, env)) ||
        ((env = getenv("DSCO_SMART_FAST_PORT")) && !set_port(&cfg.fast_port, env)) ||
        ((env = getenv("DSCO_SMART_MODEL_PORT")) && !set_port(&cfg.smart_port, env)) ||
        ((env = getenv("DSCO_SMART_THRESHOLD")) &&
         !set_threshold(&cfg.smart_threshold, env))) {
        fprintf(stderr, "local-smart-router: invalid environment configuration\n");
        return 2;
    }

    const char *explain_body = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--self-test") == 0)
            return self_test();
        if (strcmp(argv[i], "--explain") == 0 && i + 1 < argc) {
            explain_body = argv[++i];
        } else if (strcmp(argv[i], "--listen-port") == 0 && i + 1 < argc) {
            if (!set_port(&cfg.listen_port, argv[++i]))
                goto invalid_args;
        } else if (strcmp(argv[i], "--fast-port") == 0 && i + 1 < argc) {
            if (!set_port(&cfg.fast_port, argv[++i]))
                goto invalid_args;
        } else if (strcmp(argv[i], "--smart-port") == 0 && i + 1 < argc) {
            if (!set_port(&cfg.smart_port, argv[++i]))
                goto invalid_args;
        } else if (strcmp(argv[i], "--threshold") == 0 && i + 1 < argc) {
            if (!set_threshold(&cfg.smart_threshold, argv[++i]))
                goto invalid_args;
        } else if (strcmp(argv[i], "--help") == 0) {
            puts("usage: local-smart-router [--listen-port N] [--fast-port N] "
                 "[--smart-port N] [--threshold BYTES] [--self-test] "
                 "[--explain JSON]");
            return 0;
        } else
            goto invalid_args;
    }
    if (!cfg.listen_port || !cfg.fast_port || !cfg.smart_port || !cfg.smart_threshold) {
        fprintf(stderr, "local-smart-router: invalid configuration\n");
        return 2;
    }
    if (explain_body) {
        dsco_local_decision_t decision = dsco_local_smart_decide(
            explain_body, strlen(explain_body), NULL, cfg.smart_threshold);
        printf("{\"lane\":\"%s\",\"score\":%d,\"reason\":\"%s\","
               "\"reasoning_budget\":%d,\"forced\":%s}\n",
               dsco_local_lane_name(decision.lane), decision.score, decision.reason,
               decision.reasoning_budget, decision.forced ? "true" : "false");
        return 0;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);
    curl_global_init(CURL_GLOBAL_DEFAULT);

    g_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_listen_fd < 0) {
        perror("local-smart-router socket");
        return 1;
    }
    int one = 1;
    setsockopt(g_listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#ifdef SO_NOSIGPIPE
    setsockopt(g_listen_fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(cfg.listen_port);
    sa.sin_addr.s_addr = htonl(0x7f000001u);
    if (bind(g_listen_fd, (struct sockaddr *)&sa, sizeof(sa)) != 0 ||
        listen(g_listen_fd, 128) != 0) {
        perror("local-smart-router bind/listen");
        close(g_listen_fd);
        curl_global_cleanup();
        return 1;
    }
    fprintf(stderr,
            "[local-smart] http://127.0.0.1:%u -> fast:%u smart:%u threshold=%zu\n",
            cfg.listen_port, cfg.fast_port, cfg.smart_port, cfg.smart_threshold);

    while (g_running) {
        int fd = accept(g_listen_fd, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
#ifdef SO_NOSIGPIPE
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
        client_arg_t *arg = malloc(sizeof(*arg));
        if (!arg) {
            close(fd);
            continue;
        }
        arg->fd = fd;
        arg->cfg = cfg;
        pthread_t thread;
        if (pthread_create(&thread, NULL, handle_client, arg) != 0) {
            close(fd);
            free(arg);
            continue;
        }
        pthread_detach(thread);
    }
    if (g_listen_fd >= 0)
        close(g_listen_fd);
    return 0;

invalid_args:
    fprintf(stderr, "local-smart-router: invalid arguments (use --help)\n");
    return 2;
}
