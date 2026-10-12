/* GET-only, bounded vendor documentation metadata for /model reporting.
 * Published limits do not replace a session's transport-specific active cap. */
#define _POSIX_C_SOURCE 200809L
#include "model_official_metadata.h"
#include "http_pool.h"

#include <ctype.h>
#include <curl/curl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define DOC_LIMIT (512u * 1024u)
#define ANTHROPIC_DOC "https://platform.claude.com/docs/en/about-claude/models/overview.md"
#define OPENAI_DOC_PREFIX "https://developers.openai.com/api/docs/models/"

typedef struct { char *bytes; size_t len; } doc_buf_t;

static size_t doc_write(void *ptr, size_t size, size_t count, void *opaque) {
    doc_buf_t *b = opaque;
    if (size && count > (DOC_LIMIT - b->len) / size) return 0;
    size_t n = size * count;
    char *grown = realloc(b->bytes, b->len + n + 1);
    if (!grown) return 0;
    b->bytes = grown;
    memcpy(b->bytes + b->len, ptr, n);
    b->len += n;
    b->bytes[b->len] = 0;
    return n;
}

static char *doc_get(const char *url) {
    CURL *easy = curl_easy_init();
    if (!easy) return NULL;
    dsco_http_pool_apply(easy);
    doc_buf_t b = {0};
    curl_easy_setopt(easy, CURLOPT_URL, url);
    curl_easy_setopt(easy, CURLOPT_USERAGENT, "dsco-model-metadata/1.0");
    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, doc_write);
    curl_easy_setopt(easy, CURLOPT_WRITEDATA, &b);
    curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT_MS, 2500L);
    curl_easy_setopt(easy, CURLOPT_TIMEOUT_MS, 8000L);
    curl_easy_setopt(easy, CURLOPT_MAXREDIRS, 2L);
    curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(easy, CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(easy, CURLOPT_REDIR_PROTOCOLS_STR, "https");
    CURLcode rc = curl_easy_perform(easy);
    long status = 0;
    curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(easy);
    if (rc != CURLE_OK || status != 200 || !b.bytes) {
        free(b.bytes);
        return NULL;
    }
    return b.bytes;
}

static int token_count(const char *text) {
    while (*text && !isdigit((unsigned char)*text)) text++;
    if (!*text) return 0;
    long long n = 0;
    while (isdigit((unsigned char)*text) || *text == ',') {
        if (*text != ',') {
            n = n * 10 + (*text - '0');
            if (n > INT_MAX) return 0;
        }
        text++;
    }
    if (*text == 'M' || *text == 'm') n *= 1000000;
    else if (*text == 'K' || *text == 'k') n *= 1000;
    return n > 0 && n <= INT_MAX ? (int)n : 0;
}

static bool parse_openai(const char *model, const char *body,
                         model_official_metadata_t *out) {
    char expected[192];
    if (snprintf(expected, sizeof(expected), "Model ID: `%s`", model) >= (int)sizeof(expected) ||
        !strstr(body, expected)) return false;
    /* Only parse numeric spec bullets, not prose mentioning a context window
     * (which can contain unrelated model version numbers). */
    for (const char *line = body; *line;) {
        const char *end = strchr(line, '\n');
        if (!end) end = line + strlen(line);
        if (end - line > 3 && line[0] == '-' && line[1] == ' ' &&
            isdigit((unsigned char)line[2])) {
            char bullet[240];
            size_t n = (size_t)(end - line);
            if (n < sizeof(bullet)) {
                memcpy(bullet, line, n);
                bullet[n] = 0;
                if (strstr(bullet, " context window"))
                    out->context_tokens = token_count(bullet + 2);
                if (strstr(bullet, " max output tokens"))
                    out->max_output_tokens = token_count(bullet + 2);
            }
        } else if (end - line > 23 && strncmp(line, "- Maximum input tokens:", 23) == 0)
            out->max_input_tokens = token_count(line + 23);
        line = *end ? end + 1 : end;
    }
    return out->context_tokens > 0;
}

static int split_cells(char *line, char **cells, int max) {
    int n = 0;
    char *p = strchr(line, '|');
    if (!p) return 0;
    p++;
    while (*p && n < max) {
        char *next = strchr(p, '|');
        if (!next) break;
        *next = 0;
        while (isspace((unsigned char)*p)) p++;
        char *end = p + strlen(p);
        while (end > p && isspace((unsigned char)end[-1])) *--end = 0;
        cells[n++] = p;
        p = next + 1;
    }
    return n;
}

static bool parse_anthropic(const char *model, const char *body,
                            model_official_metadata_t *out) {
    /* Docs table column alignment is authoritative only if its Claude API ID
     * row matches the exact slug. Never infer a model from its family name. */
    char *copy = strdup(body);
    if (!copy) return false;
    int column = -1;
    char *context_row = NULL, *output_row = NULL;
    for (char *line = strtok(copy, "\n"); line; line = strtok(NULL, "\n")) {
        if (strstr(line, "| Claude API ID ")) {
            char *cells[32]; int n = split_cells(line, cells, 32);
            for (int i = 1; i < n; i++) {
                char id[160];
                if (snprintf(id, sizeof(id), "`%s`", model) < (int)sizeof(id) &&
                    strcmp(cells[i], id) == 0) { column = i; break; }
            }
        } else if (strstr(line, "| [Context window]")) {
            context_row = strdup(line);
        } else if (strstr(line, "| Max output ")) {
            output_row = strdup(line);
        }
    }
    if (column >= 0 && context_row) {
        char *cells[32]; int n = split_cells(context_row, cells, 32);
        if (column < n) out->context_tokens = token_count(cells[column]);
    }
    if (column >= 0 && output_row) {
        char *cells[32]; int n = split_cells(output_row, cells, 32);
        if (column < n) out->max_output_tokens = token_count(cells[column]);
    }
    free(context_row);
    free(output_row);
    free(copy);
    return out->context_tokens > 0;
}

bool model_official_metadata_parse(const char *model, const char *body,
                                   bool anthropic, model_official_metadata_t *out) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (!model || !body || strlen(model) > 110) return false;
    bool ok = anthropic ? parse_anthropic(model, body, out) : parse_openai(model, body, out);
    if (ok) {
        out->observed_at = time(NULL);
        snprintf(out->source, sizeof(out->source), "%s", anthropic ?
                 ANTHROPIC_DOC : OPENAI_DOC_PREFIX);
    }
    return ok;
}

bool model_official_metadata_refresh(const char *model,
                                     model_official_metadata_t *out) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (!model || !*model || strlen(model) > 110) return false;
    bool anthropic = strncmp(model, "claude-", 7) == 0;
    if (!anthropic && strncmp(model, "gpt-", 4) != 0 &&
        strncmp(model, "o1", 2) != 0 && strncmp(model, "o3", 2) != 0 &&
        strncmp(model, "o4", 2) != 0) return false;
    for (const char *p = model; *p; p++)
        if (!isalnum((unsigned char)*p) && *p != '-' && *p != '.') return false;
    char url[280];
    if (anthropic) snprintf(url, sizeof(url), "%s", ANTHROPIC_DOC);
    else snprintf(url, sizeof(url), "%s%s.md", OPENAI_DOC_PREFIX, model);
    char *body = doc_get(url);
    if (!body) return false;
    bool ok = model_official_metadata_parse(model, body, anthropic, out);
    if (ok && !anthropic) snprintf(out->source, sizeof(out->source), "%s", url);
    free(body);
    return ok;
}
