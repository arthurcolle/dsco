#define _POSIX_C_SOURCE 200809L

#include "kitty_graphics.h"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>
#include <unistd.h>
#include <zlib.h>

static bool env_false(const char *name) {
    const char *value = getenv(name);
    return value && (!strcasecmp(value, "0") || !strcasecmp(value, "false") ||
                     !strcasecmp(value, "no") || !strcasecmp(value, "off"));
}

static bool env_true(const char *name) {
    const char *value = getenv(name);
    return value && (*value == '1' || !strcasecmp(value, "true") ||
                     !strcasecmp(value, "yes") || !strcasecmp(value, "on"));
}

static bool contains_ci(const char *haystack, const char *needle) {
    if (!haystack || !needle || !*needle) return false;
    size_t n = strlen(needle);
    for (const char *p = haystack; *p; p++) {
        size_t i = 0;
        while (i < n && p[i] &&
               tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i]))
            i++;
        if (i == n) return true;
    }
    return false;
}

size_t kitty_graphics_base64_encoded_length(size_t input_bytes) {
    if (input_bytes > (SIZE_MAX - 2U) / 4U * 3U)
        return SIZE_MAX;
    return 4U * ((input_bytes + 2U) / 3U);
}

void kitty_graphics_send_options_default(kitty_graphics_send_options_t *options) {
    if (!options) return;
    *options = (kitty_graphics_send_options_t){
        .chunk_size = KITTY_GRAPHICS_DEFAULT_CHUNK_SIZE,
        .compression_level = Z_BEST_SPEED,
        .compress = true,
        .continuation_control = "q=2",
    };
}

static char *base64_encode(const uint8_t *src, size_t len, size_t *encoded_len) {
    static const char table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t out_len = kitty_graphics_base64_encoded_length(len);
    if (out_len == SIZE_MAX) return NULL;
    char *out = malloc(out_len + 1U);
    if (!out) return NULL;
    size_t i = 0, j = 0;
    while (i < len) {
        size_t remain = len - i;
        uint32_t a = src[i++];
        uint32_t b = remain > 1U ? src[i++] : 0U;
        uint32_t c = remain > 2U ? src[i++] : 0U;
        uint32_t triple = (a << 16) | (b << 8) | c;
        out[j++] = table[(triple >> 18) & 63U];
        out[j++] = table[(triple >> 12) & 63U];
        out[j++] = remain > 1U ? table[(triple >> 6) & 63U] : '=';
        out[j++] = remain > 2U ? table[triple & 63U] : '=';
    }
    out[out_len] = '\0';
    if (encoded_len) *encoded_len = out_len;
    return out;
}

bool kitty_graphics_send_pixels(FILE *out, const char *control,
                                const void *pixels, size_t pixel_bytes,
                                const kitty_graphics_send_options_t *requested) {
    if (!out || !control || !*control || (!pixels && pixel_bytes > 0)) return false;

    kitty_graphics_send_options_t defaults;
    kitty_graphics_send_options_default(&defaults);
    const kitty_graphics_send_options_t *options = requested ? requested : &defaults;
    size_t chunk_size = options->chunk_size ? options->chunk_size :
                        KITTY_GRAPHICS_DEFAULT_CHUNK_SIZE;
    if (chunk_size > 1024U * 1024U) chunk_size = 1024U * 1024U;
    const char *continuation = options->continuation_control &&
                               *options->continuation_control ?
                               options->continuation_control : "q=2";

    const uint8_t *raw = (const uint8_t *)pixels;
    uint8_t *packed = NULL;
    size_t packed_len = pixel_bytes;
    if (options->compress) {
        uLongf capacity = compressBound((uLong)pixel_bytes);
        if (capacity > SIZE_MAX) return false;
        packed = malloc((size_t)capacity ? (size_t)capacity : 1U);
        if (!packed) return false;
        uLongf compressed = capacity;
        int level = options->compression_level < 0 ? Z_BEST_SPEED :
                    options->compression_level;
        if (compress2(packed, &compressed, raw, (uLong)pixel_bytes, level) != Z_OK) {
            free(packed);
            return false;
        }
        packed_len = (size_t)compressed;
        raw = packed;
    }

    size_t encoded_len = 0;
    char *encoded = base64_encode(raw, packed_len, &encoded_len);
    free(packed);
    if (!encoded) return false;

    /* Even a zero-byte payload must produce one APC with an empty body. */
    size_t offset = 0;
    bool first = true;
    do {
        size_t remaining = encoded_len - offset;
        size_t count = remaining > chunk_size ? chunk_size : remaining;
        bool more = offset + count < encoded_len;
        const char *prefix = first ? control : continuation;
        if (fprintf(out, "\033_G%s,m=%d;%.*s\033\\", prefix, more,
                    (int)count, encoded + offset) < 0) {
            free(encoded);
            return false;
        }
        offset += count;
        first = false;
    } while (offset < encoded_len || first);

    free(encoded);
    return !ferror(out);
}

bool kitty_graphics_environment_hint(void) {
    if (env_false("DSCO_KITTY_GRAPHICS") || env_false("DSCO_PIXEL_TUI"))
        return false;
    const char *override = getenv("DSCO_KITTY_GRAPHICS");
    if (env_true("DSCO_KITTY_GRAPHICS") ||
        (override && !strcasecmp(override, "force")))
        return true;
    if (getenv("KITTY_WINDOW_ID")) return true;

    const char *term = getenv("TERM");
    const char *program = getenv("TERM_PROGRAM");
    return contains_ci(term, "kitty") || contains_ci(term, "ghostty") ||
           contains_ci(term, "wezterm") || contains_ci(program, "kitty") ||
           contains_ci(program, "ghostty") || contains_ci(program, "wezterm");
}

bool kitty_graphics_available(FILE *out) {
    if (!out || !kitty_graphics_environment_hint()) return false;
    int fd = fileno(out);
    return fd >= 0 && isatty(fd);
}

bool kitty_graphics_emit_query(FILE *out, uint32_t image_id) {
    if (!out || image_id == 0) return false;
    /* Kitty's query must be paired with DA1 so unsupported terminals can be
     * distinguished from a terminal that simply has not flushed its reply. */
    if (fprintf(out, "\033_Gi=%u,s=1,v=1,a=q,t=d,f=24,q=0;AAAA\033\\\033[c",
                image_id) < 0)
        return false;
    return !ferror(out);
}
