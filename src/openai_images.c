#define _GNU_SOURCE 1

#include "openai_images.h"
#include "crypto.h"
#include "json_util.h"
#include "provider.h"
#include "tools.h"

#include <curl/curl.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define OPENAI_IMAGE_GENERATIONS_URL "https://api.openai.com/v1/images/generations"
#define OPENAI_IMAGE_MAX_B64 (64u * 1024u * 1024u)

static size_t openai_image_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    jbuf_t *buf = (jbuf_t *)userdata;
    size_t n = size * nmemb;
    if (n > 0)
        jbuf_append_len(buf, (const char *)ptr, n);
    return n;
}

static bool openai_image_is_blank(const char *s) {
    if (!s)
        return true;
    while (*s) {
        if (*s != ' ' && *s != '\t' && *s != '\n' && *s != '\r')
            return false;
        s++;
    }
    return true;
}

static char *openai_image_json_string_value(const char *json, const char *key) {
    if (!json || !key)
        return NULL;

    jbuf_t needle;
    jbuf_init(&needle, strlen(key) + 4);
    jbuf_append_char(&needle, '"');
    jbuf_append(&needle, key);
    jbuf_append_char(&needle, '"');

    const char *p = strstr(json, needle.data);
    jbuf_free(&needle);
    if (!p)
        return NULL;

    p = strchr(p, ':');
    if (!p)
        return NULL;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
        p++;
    if (*p != '"')
        return NULL;
    p++;

    jbuf_t out;
    jbuf_init(&out, 4096);
    while (*p) {
        unsigned char c = (unsigned char)*p++;
        if (c == '"')
            return out.data;
        if (c == '\\') {
            char esc = *p++;
            switch (esc) {
                case '"':  jbuf_append_char(&out, '"'); break;
                case '\\': jbuf_append_char(&out, '\\'); break;
                case '/':  jbuf_append_char(&out, '/'); break;
                case 'b':  jbuf_append_char(&out, '\b'); break;
                case 'f':  jbuf_append_char(&out, '\f'); break;
                case 'n':  jbuf_append_char(&out, '\n'); break;
                case 'r':  jbuf_append_char(&out, '\r'); break;
                case 't':  jbuf_append_char(&out, '\t'); break;
                default:   jbuf_append_char(&out, esc); break;
            }
        } else {
            jbuf_append_char(&out, (char)c);
        }
    }

    jbuf_free(&out);
    return NULL;
}

static const char *openai_image_ext_for_format(const char *format) {
    if (format && strcmp(format, "jpeg") == 0)
        return "jpg";
    if (format && strcmp(format, "webp") == 0)
        return "webp";
    return "png";
}

static char *openai_image_default_path(const char *format) {
    char buf[256];
    snprintf(buf, sizeof(buf), "openai_image_%ld.%s", (long)getpid(),
             openai_image_ext_for_format(format));
    return safe_strdup(buf);
}

static bool openai_image_ensure_parent_dirs(const char *path, char *err, size_t err_len) {
    const char *slash = path ? strrchr(path, '/') : NULL;
    if (!slash)
        return true;

    size_t len = (size_t)(slash - path);
    if (len == 0)
        return true;

    char *dir = safe_malloc(len + 1);
    memcpy(dir, path, len);
    dir[len] = '\0';

    for (char *p = dir + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = '\0';
        if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
            snprintf(err, err_len, "cannot create directory %s: %s", dir, strerror(errno));
            free(dir);
            return false;
        }
        *p = '/';
    }

    if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
        snprintf(err, err_len, "cannot create directory %s: %s", dir, strerror(errno));
        free(dir);
        return false;
    }
    free(dir);
    return true;
}

static bool openai_image_write_atomic(const char *path, const unsigned char *data, size_t len,
                                      char *err, size_t err_len) {
    if (!openai_image_ensure_parent_dirs(path, err, err_len))
        return false;

    char tmp[4096];
    int n = snprintf(tmp, sizeof(tmp), "%s.tmp.%ld", path, (long)getpid());
    if (n < 0 || (size_t)n >= sizeof(tmp)) {
        snprintf(err, err_len, "output path too long");
        return false;
    }

    FILE *f = fopen(tmp, "wb");
    if (!f) {
        snprintf(err, err_len, "cannot open %s: %s", tmp, strerror(errno));
        return false;
    }
    size_t wr = fwrite(data, 1, len, f);
    if (wr != len || fclose(f) != 0) {
        snprintf(err, err_len, "failed writing %s: %s", tmp, strerror(errno));
        unlink(tmp);
        return false;
    }
    if (rename(tmp, path) != 0) {
        snprintf(err, err_len, "cannot move %s to %s: %s", tmp, path, strerror(errno));
        unlink(tmp);
        return false;
    }
    return true;
}

static void openai_image_append_optional_string(jbuf_t *b, const char *key, const char *value) {
    if (openai_image_is_blank(value))
        return;
    jbuf_append(b, ",");
    jbuf_append_json_str(b, key);
    jbuf_append(b, ":");
    jbuf_append_json_str(b, value);
}

bool tool_openai_image_generate(const char *input_json, char *result, size_t result_len) {
    if (!result || result_len == 0)
        return false;
    result[0] = '\0';

    const char *input = input_json && input_json[0] ? input_json : "{}";
    char *prompt = json_get_str(input, "prompt");
    if (openai_image_is_blank(prompt)) {
        snprintf(result, result_len, "error: prompt required");
        free(prompt);
        return false;
    }

    char *model = json_get_str(input, "model");
    if (openai_image_is_blank(model)) {
        free(model);
        const char *env_model = getenv("DSCO_OPENAI_IMAGE_MODEL");
        model = safe_strdup((env_model && env_model[0]) ? env_model : DSCO_OPENAI_IMAGE_DEFAULT_MODEL);
    }

    char *size = json_get_str(input, "size");
    if (openai_image_is_blank(size)) {
        free(size);
        size = safe_strdup("auto");
    }

    char *quality = json_get_str(input, "quality");
    if (openai_image_is_blank(quality)) {
        free(quality);
        quality = safe_strdup("auto");
    }

    char *format = json_get_str(input, "output_format");
    if (!format)
        format = json_get_str(input, "format");
    if (openai_image_is_blank(format)) {
        free(format);
        format = safe_strdup("png");
    }

    char *background = json_get_str(input, "background");
    char *moderation = json_get_str(input, "moderation");
    int output_compression = json_get_int(input, "output_compression", -1);

    int count = json_get_int(input, "n", 1);
    if (count != 1) {
        snprintf(result, result_len, "error: openai_image_generate currently supports n=1");
        free(prompt);
        free(model);
        free(size);
        free(quality);
        free(format);
        free(background);
        free(moderation);
        return false;
    }

    char *output_path = json_get_str(input, "output_path");
    if (!output_path)
        output_path = json_get_str(input, "path");
    if (openai_image_is_blank(output_path)) {
        free(output_path);
        output_path = openai_image_default_path(format);
    }

    const char *api_key = provider_resolve_request_api_key("openai", tools_runtime_api_key());
    if (!api_key || !api_key[0]) {
        snprintf(result, result_len, "error: OPENAI_API_KEY is required for openai_image_generate");
        free(prompt);
        free(model);
        free(size);
        free(quality);
        free(format);
        free(background);
        free(moderation);
        free(output_path);
        return false;
    }

    jbuf_t body;
    jbuf_init(&body, strlen(prompt) + 256);
    jbuf_append(&body, "{\"model\":");
    jbuf_append_json_str(&body, model);
    jbuf_append(&body, ",\"prompt\":");
    jbuf_append_json_str(&body, prompt);
    jbuf_append(&body, ",\"n\":1");
    openai_image_append_optional_string(&body, "size", size);
    openai_image_append_optional_string(&body, "quality", quality);
    openai_image_append_optional_string(&body, "output_format", format);
    openai_image_append_optional_string(&body, "background", background);
    openai_image_append_optional_string(&body, "moderation", moderation);
    if (output_compression >= 0)
        jbuf_appendf(&body, ",\"output_compression\":%d", output_compression);
    jbuf_append(&body, "}");

    jbuf_t auth;
    jbuf_init(&auth, strlen(api_key) + 32);
    jbuf_append(&auth, "Authorization: Bearer ");
    jbuf_append(&auth, api_key);

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, auth.data);

    jbuf_t resp;
    jbuf_init(&resp, 1024 * 1024);
    CURL *curl = curl_easy_init();
    if (!curl) {
        snprintf(result, result_len, "error: curl_easy_init failed");
        curl_slist_free_all(headers);
        jbuf_free(&resp);
        jbuf_free(&auth);
        jbuf_free(&body);
        free(prompt);
        free(model);
        free(size);
        free(quality);
        free(format);
        free(background);
        free(moderation);
        free(output_path);
        return false;
    }

    curl_easy_setopt(curl, CURLOPT_URL, OPENAI_IMAGE_GENERATIONS_URL);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.data);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, openai_image_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 300L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "dsco-openai-images/1.0");
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2TLS);

    CURLcode rc = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);
    curl_slist_free_all(headers);

    bool ok = false;
    if (rc != CURLE_OK) {
        snprintf(result, result_len, "error: OpenAI image request failed: %s",
                 curl_easy_strerror(rc));
        goto done;
    }
    if (http_code < 200 || http_code >= 300) {
        char *msg = openai_image_json_string_value(resp.data, "message");
        snprintf(result, result_len, "error: OpenAI image request HTTP %ld%s%s", http_code,
                 msg ? ": " : "", msg ? msg : "");
        free(msg);
        goto done;
    }

    char *b64 = openai_image_json_string_value(resp.data, "b64_json");
    if (!b64 || !b64[0]) {
        snprintf(result, result_len, "error: OpenAI image response did not include b64_json");
        free(b64);
        goto done;
    }

    size_t b64_len = strlen(b64);
    if (b64_len > OPENAI_IMAGE_MAX_B64) {
        snprintf(result, result_len, "error: generated image payload too large (%zu base64 bytes)",
                 b64_len);
        free(b64);
        goto done;
    }

    size_t raw_cap = (b64_len * 3u) / 4u + 8u;
    unsigned char *raw = safe_malloc(raw_cap);
    size_t raw_len = base64_decode(b64, b64_len, raw, raw_cap);
    free(b64);
    if (raw_len == 0) {
        snprintf(result, result_len, "error: generated image base64 decoded to zero bytes");
        free(raw);
        goto done;
    }

    char write_err[512] = "";
    if (!openai_image_write_atomic(output_path, raw, raw_len, write_err, sizeof(write_err))) {
        snprintf(result, result_len, "error: %s", write_err[0] ? write_err : "image write failed");
        free(raw);
        goto done;
    }
    free(raw);

    jbuf_t out;
    jbuf_init(&out, 512);
    jbuf_append(&out, "{\"ok\":true,\"provider\":\"openai\",\"endpoint\":\"/v1/images/generations\",");
    jbuf_append(&out, "\"model\":");
    jbuf_append_json_str(&out, model);
    jbuf_append(&out, ",\"path\":");
    jbuf_append_json_str(&out, output_path);
    jbuf_appendf(&out, ",\"bytes\":%zu,\"n\":1", raw_len);
    jbuf_append(&out, ",\"size\":");
    jbuf_append_json_str(&out, size);
    jbuf_append(&out, ",\"quality\":");
    jbuf_append_json_str(&out, quality);
    jbuf_append(&out, ",\"output_format\":");
    jbuf_append_json_str(&out, format);
    jbuf_append(&out, "}");
    snprintf(result, result_len, "%s", out.data);
    jbuf_free(&out);
    ok = true;

done:
    jbuf_free(&resp);
    jbuf_free(&auth);
    jbuf_free(&body);
    free(prompt);
    free(model);
    free(size);
    free(quality);
    free(format);
    free(background);
    free(moderation);
    free(output_path);
    return ok;
}
