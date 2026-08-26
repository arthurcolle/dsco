#include "model_pricing.h"
#include "http_pool.h"

#include <curl/curl.h>
#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define OPENAI_PRICING_URL "https://developers.openai.com/api/docs/pricing.md"
#define PRICING_TTL_SECONDS (6 * 60 * 60)
#define MAX_PRICES 256

typedef struct {
    char *model;
    model_price_t price;
} price_entry_t;

static price_entry_t g_prices[MAX_PRICES];
static int g_count;
static pthread_rwlock_t g_lock = PTHREAD_RWLOCK_INITIALIZER;
static pthread_t g_thread;
static int g_thread_started;
static char g_cache_path[1024];

typedef struct { char *data; size_t len; } http_buf_t;

static size_t write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    size_t n = size * nmemb;
    http_buf_t *b = userdata;
    char *p = realloc(b->data, b->len + n + 1);
    if (!p) return 0;
    b->data = p;
    memcpy(b->data + b->len, ptr, n);
    b->len += n;
    b->data[b->len] = '\0';
    return n;
}

static char *read_file(const char *path, size_t *len_out) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long n = ftell(f);
    if (n < 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    char *p = malloc((size_t)n + 1);
    if (!p) { fclose(f); return NULL; }
    size_t got = fread(p, 1, (size_t)n, f);
    fclose(f);
    p[got] = '\0';
    if (len_out) *len_out = got;
    return p;
}

static int write_atomic(const char *path, const char *data, size_t len) {
    char tmp[1100];
    snprintf(tmp, sizeof(tmp), "%s.tmp.%ld", path, (long)getpid());
    FILE *f = fopen(tmp, "wb");
    if (!f) return -1;
    int ok = fwrite(data, 1, len, f) == len && fflush(f) == 0 && fsync(fileno(f)) == 0;
    if (fclose(f) != 0) ok = 0;
    if (!ok || rename(tmp, path) != 0) { unlink(tmp); return -1; }
    return 0;
}

static int parse_money(const char *s, double *out) {
    while (*s && isspace((unsigned char)*s)) s++;
    if (*s++ != '$') return 0;
    errno = 0;
    char *end = NULL;
    double v = strtod(s, &end);
    if (errno || end == s || v < 0.0) return 0;
    while (*end && isspace((unsigned char)*end)) end++;
    if (*end && *end != '|') return 0;
    *out = v;
    return 1;
}

static char *trim_dup(const char *start, size_t len) {
    while (len && isspace((unsigned char)*start)) { start++; len--; }
    while (len && isspace((unsigned char)start[len - 1])) len--;
    if (!len || len > 255) return NULL;
    char *s = malloc(len + 1);
    if (!s) return NULL;
    memcpy(s, start, len);
    s[len] = '\0';
    return s;
}

static int split_row(const char *line, size_t len, char **cells, int max) {
    int n = 0;
    size_t start = 0;
    if (len && line[0] == '|') start = 1;
    for (size_t i = start; i <= len && n < max; i++) {
        if (i == len || line[i] == '|') {
            cells[n++] = trim_dup(line + start, i - start);
            start = i + 1;
        }
    }
    return n;
}

static void free_cells(char **cells, int n) {
    for (int i = 0; i < n; i++) free(cells[i]);
}

static void replace_prices(price_entry_t *next, int count) {
    pthread_rwlock_wrlock(&g_lock);
    for (int i = 0; i < g_count; i++) free(g_prices[i].model);
    memcpy(g_prices, next, (size_t)count * sizeof(*next));
    g_count = count;
    pthread_rwlock_unlock(&g_lock);
}

int model_pricing_load_openai_markdown(const char *markdown, size_t len) {
    if (!markdown || !len) return 0;
    price_entry_t next[MAX_PRICES] = {0};
    int count = 0, in_table = 0;
    int model_col = -1, input_col = -1, cached_col = -1, output_col = -1;

    const char *p = markdown, *end = markdown + len;
    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t line_len = nl ? (size_t)(nl - p) : (size_t)(end - p);
        char *cells[12] = {0};
        int n = split_row(p, line_len, cells, 12);

        if (n >= 3 && cells[0] && strcmp(cells[0], "Model") == 0) {
            model_col = input_col = cached_col = output_col = -1;
            for (int i = 0; i < n; i++) {
                if (!cells[i]) continue;
                if (strcmp(cells[i], "Model") == 0) model_col = i;
                else if (strcmp(cells[i], "Input") == 0) input_col = i;
                else if (strcmp(cells[i], "Cached input") == 0) cached_col = i;
                else if (strcmp(cells[i], "Output") == 0) output_col = i;
            }
            in_table = model_col >= 0 && input_col >= 0 && output_col >= 0;
        } else if (in_table && n > output_col && cells[model_col]) {
            if (cells[model_col][0] == '-' || cells[model_col][0] == '\0') {
                /* Markdown separator or end of table. */
            } else {
                double input = 0.0, cached = 0.0, output = 0.0;
                if (parse_money(cells[input_col], &input) &&
                    parse_money(cells[output_col], &output)) {
                    if (cached_col >= 0 && cached_col < n)
                        (void)parse_money(cells[cached_col], &cached);
                    if (count < MAX_PRICES) {
                        next[count].model = strdup(cells[model_col]);
                        if (next[count].model) {
                            next[count].price = (model_price_t){input, cached, 0.0, output};
                            count++;
                        }
                    }
                } else if (cells[0][0] != '-') {
                    in_table = 0;
                }
            }
        } else if (in_table && line_len && p[0] != '|') {
            in_table = 0;
        }
        free_cells(cells, n);
        p = nl ? nl + 1 : end;
    }

    if (!count) return 0; /* Never replace good data with an invalid response. */
    replace_prices(next, count);
    return count;
}

static int fetch_openai(void) {
    CURL *c = curl_easy_init();
    if (!c) return 0;
    http_buf_t b = {0};
    curl_easy_setopt(c, CURLOPT_URL, OPENAI_PRICING_URL);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &b);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "dsco/1.0 model-pricing");
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 3L);
    dsco_http_pool_apply(c);
    CURLcode rc = curl_easy_perform(c);
    long status = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(c);
    int loaded = 0;
    if (rc == CURLE_OK && status == 200 && b.data)
        loaded = model_pricing_load_openai_markdown(b.data, b.len);
    if (loaded) (void)write_atomic(g_cache_path, b.data, b.len);
    free(b.data);
    return loaded;
}

static void *refresh_thread(void *unused) {
    (void)unused;
    (void)fetch_openai();
    return NULL;
}

void model_pricing_init(void) {
    const char *override = getenv("DSCO_OPENAI_PRICING_FILE");
    if (override && *override) {
        snprintf(g_cache_path, sizeof(g_cache_path), "%s", override);
    } else {
        const char *home = getenv("HOME");
        snprintf(g_cache_path, sizeof(g_cache_path), "%s/.dsco/openai_pricing.md",
                 home && *home ? home : ".");
    }
    size_t len = 0;
    char *cached = read_file(g_cache_path, &len);
    if (cached) {
        (void)model_pricing_load_openai_markdown(cached, len);
        free(cached);
    }
    if (getenv("DSCO_PRICING_OFFLINE")) return;
    struct stat st;
    int fresh = stat(g_cache_path, &st) == 0 && time(NULL) - st.st_mtime < PRICING_TTL_SECONDS;
    if (!fresh && pthread_create(&g_thread, NULL, refresh_thread, NULL) == 0)
        g_thread_started = 1;
}

void model_pricing_shutdown(void) {
    if (g_thread_started) {
        pthread_join(g_thread, NULL);
        g_thread_started = 0;
    }
}

static int lookup_exact(const char *model, model_price_t *out) {
    int found = 0;
    pthread_rwlock_rdlock(&g_lock);
    for (int i = 0; i < g_count; i++) {
        if (strcmp(g_prices[i].model, model) == 0) {
            *out = g_prices[i].price;
            found = 1;
            break;
        }
    }
    pthread_rwlock_unlock(&g_lock);
    return found;
}

int model_pricing_lookup(const char *provider, const char *model_id,
                         model_price_t *out) {
    if (!provider || !model_id || !out || strcmp(provider, "openai") != 0) return 0;
    const char *id = strncmp(model_id, "openai/", 7) == 0 ? model_id + 7 : model_id;
    if (lookup_exact(id, out)) return 1;

    /* Dated API IDs inherit the stable model's current price. Prefer the
     * longest matching stable ID to avoid gpt-5 matching gpt-5-mini. */
    int best = -1;
    size_t best_len = 0;
    pthread_rwlock_rdlock(&g_lock);
    for (int i = 0; i < g_count; i++) {
        size_t n = strlen(g_prices[i].model);
        if (n > best_len && strncmp(id, g_prices[i].model, n) == 0 && id[n] == '-') {
            best = i;
            best_len = n;
        }
    }
    if (best >= 0) *out = g_prices[best].price;
    pthread_rwlock_unlock(&g_lock);
    return best >= 0;
}
