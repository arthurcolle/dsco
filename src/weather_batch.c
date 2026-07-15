#include "weather_batch.h"

#include "http_pool.h"
#include "json_util.h"
#include "tools.h"

#include <curl/curl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WEATHER_BATCH_MAX_LOCATIONS 1000
#define WEATHER_BATCH_DEFAULT_CONCURRENCY 8
#define WEATHER_BATCH_MAX_CONCURRENCY 32

typedef struct {
    char *location;
    char *url;
    char *body;
    size_t body_len;
    size_t body_cap;
    long status;
    CURLcode curl_code;
} weather_batch_item_t;

typedef struct {
    weather_batch_item_t *items;
    int count;
    bool invalid;
} weather_batch_parse_t;

static size_t weather_batch_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    weather_batch_item_t *item = userdata;
    size_t total = size * nmemb;
    if (total > SIZE_MAX - item->body_len - 1)
        return 0;
    size_t need = item->body_len + total + 1;
    if (need > item->body_cap) {
        size_t cap = item->body_cap ? item->body_cap : 256;
        while (cap < need) {
            if (cap > SIZE_MAX / 2) {
                cap = need;
                break;
            }
            cap *= 2;
        }
        char *body = realloc(item->body, cap);
        if (!body)
            return 0;
        item->body = body;
        item->body_cap = cap;
    }
    memcpy(item->body + item->body_len, ptr, total);
    item->body_len += total;
    item->body[item->body_len] = '\0';
    return total;
}

static void weather_batch_location_cb(const char *element, void *ctx) {
    weather_batch_parse_t *parse = ctx;
    if (!parse || parse->invalid)
        return;
    if (parse->count >= WEATHER_BATCH_MAX_LOCATIONS) {
        parse->invalid = true;
        return;
    }
    /* json_array_foreach hands this callback the element itself. Wrap a scalar
     * string as an object because json_get_str intentionally parses objects. */
    size_t len = strlen(element);
    char *wrapped = malloc(len + 11); /* {\"value\": + element + } + NUL */
    if (!wrapped) {
        parse->invalid = true;
        return;
    }
    snprintf(wrapped, len + 11, "{\"value\":%s}", element);
    char *location = json_get_str(wrapped, "value");
    free(wrapped);
    if (!location || !location[0]) {
        free(location);
        parse->invalid = true;
        return;
    }
    parse->items[parse->count++].location = location;
}

static bool weather_batch_error_is_retryable(CURLcode code, long status) {
    return code != CURLE_OK || status == 408 || status == 429 || status >= 500;
}

static void weather_batch_append_json_string(char *result, size_t rlen, size_t *used,
                                             const char *value) {
    if (*used >= rlen)
        return;
    const char *src = value ? value : "";
    int n = snprintf(result + *used, rlen - *used, "\"");
    if (n < 0 || (size_t)n >= rlen - *used) {
        *used = rlen;
        return;
    }
    *used += (size_t)n;
    for (; *src && *used + 2 < rlen; src++) {
        unsigned char c = (unsigned char)*src;
        if (c == '\\' || c == '\"') {
            result[(*used)++] = '\\';
            result[(*used)++] = (char)c;
        } else if (c == '\n') {
            memcpy(result + *used, "\\n", 2);
            *used += 2;
        } else if (c == '\r') {
            memcpy(result + *used, "\\r", 2);
            *used += 2;
        } else if (c == '\t') {
            memcpy(result + *used, "\\t", 2);
            *used += 2;
        } else if (c >= 0x20) {
            result[(*used)++] = (char)c;
        }
    }
    if (*used < rlen)
        result[(*used)++] = '\"';
}

static void weather_batch_append_record(char *result, size_t rlen, size_t *used,
                                        const weather_batch_item_t *item, bool *truncated) {
    if (*used + 128 >= rlen) {
        *truncated = true;
        return;
    }
    int n = snprintf(result + *used, rlen - *used, "{\"location\":");
    if (n < 0 || (size_t)n >= rlen - *used) {
        *truncated = true;
        return;
    }
    *used += (size_t)n;
    weather_batch_append_json_string(result, rlen, used, item->location);

    if (item->curl_code == CURLE_OK && item->status == 200 && item->body) {
        char *name = json_get_str(item->body, "name");
        char *sys = json_get_raw(item->body, "sys");
        char *country = sys ? json_get_str(sys, "country") : NULL;
        char *main = json_get_raw(item->body, "main");
        char *weather = json_get_raw(item->body, "weather");
        const char *first = weather ? strchr(weather, '{') : NULL;
        char *condition = first ? json_get_str(first, "main") : NULL;
        double temp = main ? json_get_double(main, "temp", 0.0) : 0.0;
        n = snprintf(result + *used, rlen - *used,
                     ",\"status\":\"ok\",\"resolved_location\":");
        if (n > 0 && (size_t)n < rlen - *used) {
            *used += (size_t)n;
            if (name && country) {
                char resolved[256];
                snprintf(resolved, sizeof(resolved), "%s, %s", name, country);
                weather_batch_append_json_string(result, rlen, used, resolved);
            } else {
                weather_batch_append_json_string(result, rlen, used, name ? name : "");
            }
            n = snprintf(result + *used, rlen - *used, ",\"temperature_c\":%.1f,\"condition\":",
                         temp);
            if (n > 0 && (size_t)n < rlen - *used) {
                *used += (size_t)n;
                weather_batch_append_json_string(result, rlen, used, condition ? condition : "");
            }
        }
        free(name);
        free(sys);
        free(country);
        free(main);
        free(weather);
        free(condition);
    } else {
        const char *kind = weather_batch_error_is_retryable(item->curl_code, item->status)
                               ? "retryable_error"
                               : "error";
        n = snprintf(result + *used, rlen - *used, ",\"status\":\"%s\",\"http_status\":%ld",
                     kind, item->status);
        if (n > 0 && (size_t)n < rlen - *used)
            *used += (size_t)n;
        if (item->body && item->body[0] && *used + 16 < rlen) {
            n = snprintf(result + *used, rlen - *used, ",\"error\":");
            if (n > 0 && (size_t)n < rlen - *used) {
                *used += (size_t)n;
                weather_batch_append_json_string(result, rlen, used, item->body);
            }
        }
    }
    if (*used + 2 < rlen)
        result[(*used)++] = '}';
    else
        *truncated = true;
}

bool tool_weather_batch(const char *input, char *result, size_t rlen) {
    if (!result || rlen == 0)
        return false;
    result[0] = '\0';
    const char *api_key = getenv("OPENWEATHERMAP_API_KEY");
    weather_batch_item_t items[WEATHER_BATCH_MAX_LOCATIONS] = {{0}};
    weather_batch_parse_t parse = {.items = items};
    int parsed = json_array_foreach(input ? input : "{}", "locations", weather_batch_location_cb,
                                    &parse);
    int concurrency = json_get_int(input ? input : "{}", "concurrency",
                                   WEATHER_BATCH_DEFAULT_CONCURRENCY);
    if (parse.invalid || parsed != parse.count || parse.count == 0) {
        snprintf(result, rlen,
                 "weather_batch requires locations: a non-empty array of strings (maximum %d)",
                 WEATHER_BATCH_MAX_LOCATIONS);
        goto cleanup;
    }
    if (concurrency < 1)
        concurrency = 1;
    if (concurrency > WEATHER_BATCH_MAX_CONCURRENCY)
        concurrency = WEATHER_BATCH_MAX_CONCURRENCY;
    if (!api_key || !api_key[0]) {
        snprintf(result, rlen, "missing API key: set OPENWEATHERMAP_API_KEY for OpenWeatherMap");
        goto cleanup;
    }

    CURLM *multi = curl_multi_init();
    if (!multi) {
        snprintf(result, rlen, "weather_batch: failed to initialize curl multi handle");
        goto cleanup;
    }
    int next = 0, active = 0, completed = 0;
    while (completed < parse.count) {
        while (next < parse.count && active < concurrency) {
            CURL *easy = curl_easy_init();
            char *escaped = easy ? curl_easy_escape(easy, items[next].location, 0) : NULL;
            if (!easy || !escaped) {
                if (easy)
                    curl_easy_cleanup(easy);
                free(escaped);
                items[next].curl_code = CURLE_FAILED_INIT;
                completed++;
                next++;
                continue;
            }
            size_t url_len = strlen(escaped) + strlen(api_key) + 128;
            items[next].url = malloc(url_len);
            if (!items[next].url) {
                curl_free(escaped);
                curl_easy_cleanup(easy);
                items[next].curl_code = CURLE_OUT_OF_MEMORY;
                completed++;
                next++;
                continue;
            }
            snprintf(items[next].url, url_len,
                     "https://api.openweathermap.org/data/2.5/weather?q=%s&appid=%s&units=metric",
                     escaped, api_key);
            curl_free(escaped);
            dsco_http_pool_apply(easy);
            curl_easy_setopt(easy, CURLOPT_URL, items[next].url);
            curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, weather_batch_write_cb);
            curl_easy_setopt(easy, CURLOPT_WRITEDATA, &items[next]);
            curl_easy_setopt(easy, CURLOPT_PRIVATE, &items[next]);
            curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
            curl_easy_setopt(easy, CURLOPT_TIMEOUT, 20L);
            curl_multi_add_handle(multi, easy);
            next++;
            active++;
        }
        int running = 0;
        CURLMcode mcode;
        do {
            mcode = curl_multi_perform(multi, &running);
        } while (mcode == CURLM_CALL_MULTI_PERFORM);
        if (mcode != CURLM_OK) {
            snprintf(result, rlen, "weather_batch: curl multi failure: %s", curl_multi_strerror(mcode));
            curl_multi_cleanup(multi);
            multi = NULL;
            goto cleanup;
        }
        int msg_count = 0;
        CURLMsg *msg;
        while ((msg = curl_multi_info_read(multi, &msg_count)) != NULL) {
            if (msg->msg != CURLMSG_DONE)
                continue;
            weather_batch_item_t *item = NULL;
            curl_easy_getinfo(msg->easy_handle, CURLINFO_PRIVATE, &item);
            if (item) {
                item->curl_code = msg->data.result;
                curl_easy_getinfo(msg->easy_handle, CURLINFO_RESPONSE_CODE, &item->status);
            }
            curl_multi_remove_handle(multi, msg->easy_handle);
            curl_easy_cleanup(msg->easy_handle);
            active--;
            completed++;
        }
        if (active > 0)
            curl_multi_poll(multi, NULL, 0, 1000, NULL);
    }
    curl_multi_cleanup(multi);

    size_t used = 0;
    bool truncated = false;
    int ok_count = 0;
    for (int i = 0; i < parse.count; i++)
        if (items[i].curl_code == CURLE_OK && items[i].status == 200)
            ok_count++;
    int n = snprintf(result, rlen,
                     "{\"requested\":%d,\"completed\":%d,\"ok\":%d,\"concurrency\":%d,\"results\":[",
                     parse.count, completed, ok_count, concurrency);
    used = n > 0 ? (size_t)n : 0;
    for (int i = 0; i < parse.count && !truncated; i++) {
        if (i && used + 1 < rlen)
            result[used++] = ',';
        weather_batch_append_record(result, rlen, &used, &items[i], &truncated);
    }
    if (used + 32 < rlen) {
        n = snprintf(result + used, rlen - used, "],\"truncated\":%s}",
                     truncated ? "true" : "false");
        if (n > 0)
            used += (size_t)n;
    }
    if (used >= rlen)
        result[rlen - 1] = '\0';

cleanup:
    for (int i = 0; i < parse.count; i++) {
        free(items[i].location);
        free(items[i].url);
        free(items[i].body);
    }
    return result[0] == '{';
}
