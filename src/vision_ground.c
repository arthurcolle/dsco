#include "vision_ground.h"
#include "crypto.h"
#include "json_util.h"
#include "../vendor/yyjson.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#ifndef DSCO_VISION_GROUND_NO_NET
#include <curl/curl.h>
#endif

static void say(char *detail, size_t len, const char *why) {
    if (detail && len) snprintf(detail, len, "%s", why);
}

bool vision_ground_available(void) {
    const char *off = getenv("DSCO_VISION_GROUND");
    if (off && (!strcmp(off, "0") || !strcasecmp(off, "false") || !strcasecmp(off, "off")))
        return false;
    const char *key = getenv("TYPESAFE_API_KEY");
    return key && *key;
}

static __attribute__((unused)) const char *image_format(const unsigned char *b, size_t n) {
    if (n >= 8 && !memcmp(b, "\211PNG\r\n\032\n", 8)) return "png";
    if (n >= 3 && b[0] == 0xff && b[1] == 0xd8 && b[2] == 0xff) return "jpg";
    return NULL;
}

bool vision_ground_build_body(const unsigned char *image, size_t image_len,
                              const char *format, const char *target,
                              const char *model, const char *ref,
                              char **body_out, size_t *body_len) {
    if (!image || !image_len || !format || !target || !*target || !model || !ref || !body_out)
        return false;
    size_t b64_cap = 4 * ((image_len + 2) / 3) + 1;
    char *b64 = malloc(b64_cap);
    if (!b64) return false;
    size_t b64_len = base64_encode(image, image_len, b64, b64_cap);
    if (!b64_len) { free(b64); return false; }
    b64[b64_len] = '\0';

    jbuf_t body;
    jbuf_init(&body, b64_len + 1024);
    jbuf_append(&body, "{\"model\":");
    jbuf_append_json_str(&body, model);
    /* The reference is plain text substituted wherever it appears, so it is a
     * random token that cannot collide with words in the target description. */
    jbuf_append(&body, ",\"state\":{\"screen\":");
    jbuf_append_json_str(&body, ref);
    jbuf_append(&body, "},\"assets\":{");
    jbuf_append_json_str(&body, ref);
    jbuf_append(&body, ":{\"type\":\"image\",\"format\":");
    jbuf_append_json_str(&body, format);
    jbuf_append(&body, ",\"data_base64\":\"");
    jbuf_append(&body, b64);
    jbuf_append(&body, "\"}},\"questions\":{\"target\":{\"type\":\"box\",\"instructions\":");
    jbuf_t instructions;
    jbuf_init(&instructions, strlen(target) + 32);
    jbuf_append(&instructions, target);
    jbuf_append(&instructions, " in `screen`");
    jbuf_append_json_str(&body, instructions.data);
    jbuf_free(&instructions);
    jbuf_append(&body, "}}}");
    free(b64);
    if (!body.data) return false;
    *body_out = body.data;
    if (body_len) *body_len = body.len;
    return true;
}

static bool fraction(yyjson_val *v, double *out) {
    if (!yyjson_is_num(v)) return false;
    double d = yyjson_get_num(v);
    if (!isfinite(d) || d < 0.0 || d > 1.0) return false;
    *out = d;
    return true;
}

bool vision_ground_parse(const char *response, size_t len, vision_box_t *out,
                         char *detail, size_t detail_len) {
    if (!response || !out) { say(detail, detail_len, "no_response"); return false; }
    yyjson_doc *doc = yyjson_read(response, len, 0);
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    if (!yyjson_is_obj(root)) {
        yyjson_doc_free(doc);
        say(detail, detail_len, "response_not_json");
        return false;
    }
    yyjson_val *answer = yyjson_obj_get(yyjson_obj_get(root, "answers"), "target");
    yyjson_val *box = yyjson_obj_get(answer, "box");
    vision_box_t b = {0};
    bool ok = yyjson_equals_str(yyjson_obj_get(answer, "type"), "box") && yyjson_is_obj(box) &&
              fraction(yyjson_obj_get(box, "left"), &b.left) &&
              fraction(yyjson_obj_get(box, "top"), &b.top) &&
              fraction(yyjson_obj_get(box, "right"), &b.right) &&
              fraction(yyjson_obj_get(box, "bottom"), &b.bottom) &&
              fraction(yyjson_obj_get(answer, "confidence"), &b.confidence) &&
              b.left <= b.right && b.top <= b.bottom;
    if (!ok) {
        const char *message = yyjson_get_str(yyjson_obj_get(yyjson_obj_get(root, "detail"), "message"));
        if (detail && detail_len)
            snprintf(detail, detail_len, "%s%s", message ? "api_error: " : "invalid_box_answer",
                     message ? message : "");
    }
    yyjson_doc_free(doc);
    if (ok) *out = b;
    return ok;
}

#ifndef DSCO_VISION_GROUND_NO_NET
typedef struct { char *data; size_t len, cap; } sink_t;

static size_t sink_write(char *p, size_t size, size_t n, void *user) {
    sink_t *s = user;
    size_t add = size * n;
    if (s->len + add + 1 > s->cap) return 0; /* bounded response */
    memcpy(s->data + s->len, p, add);
    s->len += add;
    s->data[s->len] = '\0';
    return add;
}
#endif

bool vision_ground_box(const char *image_path, const char *target,
                       vision_box_t *out, char *detail, size_t detail_len) {
    if (!image_path || !target || !*target || !out) { say(detail, detail_len, "invalid_request"); return false; }
    if (strlen(target) > VISION_GROUND_TARGET_MAX) { say(detail, detail_len, "target_too_long"); return false; }
    if (!vision_ground_available()) { say(detail, detail_len, "vision_unavailable"); return false; }
#ifdef DSCO_VISION_GROUND_NO_NET
    (void)image_path;
    say(detail, detail_len, "network_disabled_in_build");
    return false;
#else
    struct stat st;
    if (stat(image_path, &st) || st.st_size <= 8 || (size_t)st.st_size > VISION_GROUND_IMAGE_MAX) {
        say(detail, detail_len, "image_unreadable_or_too_large");
        return false;
    }
    FILE *f = fopen(image_path, "rb");
    unsigned char *image = f ? malloc((size_t)st.st_size) : NULL;
    bool read_ok = image && fread(image, 1, (size_t)st.st_size, f) == (size_t)st.st_size;
    if (f) fclose(f);
    const char *format = read_ok ? image_format(image, (size_t)st.st_size) : NULL;
    if (!format) { free(image); say(detail, detail_len, "image_not_png_or_jpeg"); return false; }

    unsigned char nonce[8];
    char ref[40] = "img-";
    crypto_random_bytes(nonce, sizeof(nonce));
    for (size_t i = 0; i < sizeof(nonce); i++) snprintf(ref + 4 + 2 * i, 3, "%02x", nonce[i]);
    strcat(ref, "-screen");

    const char *model = getenv("DSCO_VISION_GROUND_MODEL");
    if (!model || !*model) model = VISION_GROUND_DEFAULT_MODEL;
    char *body = NULL;
    size_t body_len = 0;
    bool built = vision_ground_build_body(image, (size_t)st.st_size, format, target, model, ref,
                                          &body, &body_len);
    free(image);
    if (!built) { say(detail, detail_len, "request_build_failed"); return false; }

    CURL *curl = curl_easy_init();
    if (!curl) { free(body); say(detail, detail_len, "curl_init_failed"); return false; }
    char auth[512];
    snprintf(auth, sizeof(auth), "Authorization: Bearer %s", getenv("TYPESAFE_API_KEY"));
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, auth);
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "TypeSafe-Danger-Zone-Images: true");
    char response[16384];
    sink_t sink = {response, 0, sizeof(response)};
    response[0] = '\0';
    curl_easy_setopt(curl, CURLOPT_URL, VISION_GROUND_ENDPOINT);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)body_len);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 3000L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 20000L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, sink_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink);
    CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    memset(auth, 0, sizeof(auth));
    free(body);
    if (rc != CURLE_OK) {
        if (detail && detail_len) snprintf(detail, detail_len, "transport: %s", curl_easy_strerror(rc));
        return false;
    }
    if (status != 200) {
        char why[160];
        if (!vision_ground_parse(response, sink.len, out, why, sizeof(why)) && detail && detail_len)
            snprintf(detail, detail_len, "http_%ld %s", status, why);
        return false;
    }
    return vision_ground_parse(response, sink.len, out, detail, detail_len);
#endif
}
