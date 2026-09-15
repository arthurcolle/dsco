#include "tool_content.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#define CONTENT_LIMIT (24u * 1024u * 1024u)
static pthread_key_t content_key;
static pthread_once_t content_once = PTHREAD_ONCE_INIT;
void tool_content_free(tool_content_t *p) {
    while (p) { tool_content_t *next = p->next; free(p->mime_type); free(p->data); free(p); p = next; }
}
static void content_destroy(void *p) { tool_content_free(p); }
static void content_init(void) { pthread_key_create(&content_key, content_destroy); }
static tool_content_t *content_get(void) {
    pthread_once(&content_once, content_init); return pthread_getspecific(content_key);
}
tool_content_t *tool_content_take(void) {
    tool_content_t *p = content_get(); pthread_setspecific(content_key, NULL); return p;
}
void tool_content_clear(void) { tool_content_free(tool_content_take()); }
static bool content_fits(tool_content_t *p, size_t size) {
    unsigned count = 0;
    for (; p; p = p->next) { size += strlen(p->data); count++; }
    return count < 8 && size <= CONTENT_LIMIT;
}
bool tool_content_merge(tool_content_t *items) {
    bool complete = true;
    while (items) {
        tool_content_t *next = items->next;
        items->next = NULL;
        tool_content_t *head = content_get();
        if (!content_fits(head, strlen(items->data))) { tool_content_free(items); complete = false; }
        else if (!head) pthread_setspecific(content_key, items);
        else { while (head->next) head = head->next; head->next = items; }
        items = next;
    }
    return complete;
}
bool tool_content_add_image_base64(const char *data, const char *mime) {
    if (!data || !mime || strncmp(mime, "image/", 6) || strlen(mime) > 63) return false;
    size_t size = strnlen(data, CONTENT_LIMIT + 1);
    if (!size || size % 4 || !content_fits(content_get(), size)) return false;
    for (size_t i = 0; i < size; i++) {
        unsigned char c = (unsigned char)data[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '+' || c == '/' ||
              (c == '=' && i >= size - 2))) return false;
        if (c == '=' && i + 1 < size && data[i + 1] != '=') return false;
    }
    tool_content_t *p = calloc(1, sizeof(*p));
    if (!p) return false;
    p->mime_type = strdup(mime); p->data = strdup(data);
    if (!p->mime_type || !p->data) { tool_content_free(p); return false; }
    return tool_content_merge(p);
}
bool tool_content_add_image_file(const char *path, const char *mime) {
    FILE *f = path ? fopen(path, "rb") : NULL;
    if (!f) return false;
    struct stat st;
    if (fstat(fileno(f), &st) || !S_ISREG(st.st_mode) || st.st_size <= 0 || st.st_size > 8 * 1024 * 1024) {
        fclose(f); return false;
    }
    size_t n = (size_t)st.st_size;
    unsigned char *raw = malloc(n);
    char *b64 = malloc(((n + 2) / 3) * 4 + 1);
    if (!raw || !b64 || fread(raw, 1, n, f) != n) {
        free(raw); free(b64); fclose(f); return false;
    }
    fclose(f);
    static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t j = 0;
    for (size_t i = 0; i < n; i += 3) {
        unsigned v = (unsigned)raw[i] << 16;
        if (i + 1 < n) v |= (unsigned)raw[i + 1] << 8;
        if (i + 2 < n) v |= raw[i + 2];
        b64[j++] = table[v >> 18]; b64[j++] = table[(v >> 12) & 63];
        b64[j++] = i + 1 < n ? table[(v >> 6) & 63] : '=';
        b64[j++] = i + 2 < n ? table[v & 63] : '=';
    }
    b64[j] = 0;
    bool ok = tool_content_add_image_base64(b64, mime);
    free(raw); free(b64); return ok;
}
