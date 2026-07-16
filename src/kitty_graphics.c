#define _POSIX_C_SOURCE 200809L

#include "kitty_graphics.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
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

static double monotonic_ms(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0.0;
    return (double)now.tv_sec * 1000.0 + (double)now.tv_nsec / 1000000.0;
}

static bool send_finish(kitty_graphics_send_stats_t *stats,
                        kitty_graphics_send_stats_t *measured,
                        double started_ms, bool ok) {
    if (stats) {
        measured->total_ms = monotonic_ms() - started_ms;
        if (measured->total_ms < 0.0) measured->total_ms = 0.0;
        *stats = *measured;
    }
    return ok;
}

bool kitty_graphics_send_pixels_ex(FILE *out, const char *control,
                                   const void *pixels, size_t pixel_bytes,
                                   const kitty_graphics_send_options_t *requested,
                                   kitty_graphics_send_stats_t *stats) {
    kitty_graphics_send_stats_t measured = {0};
    measured.input_bytes = pixel_bytes;
    double started_ms = stats ? monotonic_ms() : 0.0;
    if (!out || !control || !*control || (!pixels && pixel_bytes > 0))
        return send_finish(stats, &measured, started_ms, false);

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
    size_t packed_allocation = 0;
    if (options->compress) {
        double compression_started = stats ? monotonic_ms() : 0.0;
        uLongf capacity = compressBound((uLong)pixel_bytes);
        if (capacity > SIZE_MAX)
            return send_finish(stats, &measured, started_ms, false);
        packed = malloc((size_t)capacity ? (size_t)capacity : 1U);
        if (!packed)
            return send_finish(stats, &measured, started_ms, false);
        packed_allocation = (size_t)capacity ? (size_t)capacity : 1U;
        measured.peak_heap_bytes = packed_allocation;
        uLongf compressed = capacity;
        int level = options->compression_level < 0 ? Z_BEST_SPEED :
                    options->compression_level;
        if (compress2(packed, &compressed, raw, (uLong)pixel_bytes, level) != Z_OK) {
            free(packed);
            return send_finish(stats, &measured, started_ms, false);
        }
        if (stats) measured.compression_ms = monotonic_ms() - compression_started;
        packed_len = (size_t)compressed;
        raw = packed;
    }
    measured.packed_bytes = packed_len;

    size_t encoded_len = 0;
    double base64_started = stats ? monotonic_ms() : 0.0;
    char *encoded = base64_encode(raw, packed_len, &encoded_len);
    if (stats && encoded)
        measured.base64_ms = monotonic_ms() - base64_started;
    size_t encoded_heap = encoded_len + 1U;
    if (encoded) {
        if (packed_allocation <= SIZE_MAX - encoded_heap &&
            packed_allocation + encoded_heap > measured.peak_heap_bytes)
            measured.peak_heap_bytes = packed_allocation + encoded_heap;
    }
    free(packed);
    if (!encoded) return send_finish(stats, &measured, started_ms, false);
    measured.encoded_bytes = encoded_len;

    /* Even a zero-byte payload must produce one APC with an empty body. */
    size_t offset = 0;
    bool first = true;
    double write_started = stats ? monotonic_ms() : 0.0;
    do {
        size_t remaining = encoded_len - offset;
        size_t count = remaining > chunk_size ? chunk_size : remaining;
        bool more = offset + count < encoded_len;
        const char *prefix = first ? control : continuation;
        int written = fprintf(out, "\033_G%s,m=%d;%.*s\033\\", prefix, more,
                              (int)count, encoded + offset);
        if (written < 0) {
            free(encoded);
            return send_finish(stats, &measured, started_ms, false);
        }
        measured.wire_bytes += (uint64_t)written;
        measured.chunks++;
        offset += count;
        first = false;
    } while (offset < encoded_len || first);
    if (stats) measured.write_ms = monotonic_ms() - write_started;

    free(encoded);
    return send_finish(stats, &measured, started_ms, !ferror(out));
}

bool kitty_graphics_send_pixels(FILE *out, const char *control,
                                const void *pixels, size_t pixel_bytes,
                                const kitty_graphics_send_options_t *requested) {
    return kitty_graphics_send_pixels_ex(out, control, pixels, pixel_bytes,
                                         requested, NULL);
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

/* Active protocol handshake: emit the official graphics query paired with
 * DA1 on the tty and read the reply. Terminals that implement the protocol
 * (kitty, ghostty, wezterm, konsole, iTerm2 ≥ 3.6, …) acknowledge the APC
 * with `_G…;OK` before answering DA1; everything answers DA1, so a DA1 reply
 * without the acknowledgement is a definitive "no" rather than a timeout.
 * The result is cached for the process — the tty's identity cannot change. */
#define KITTY_PROBE_IMAGE_ID 0x4B505251u /* "KPRQ" */
#define KITTY_PROBE_TIMEOUT_MS 400

static int kitty_probe_run(FILE *out) {
    int wfd = fileno(out);
    if (wfd < 0 || !isatty(wfd)) return 0;

    /* Replies arrive on terminal input, which may not be `out`. */
    int rfd = open("/dev/tty", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    bool close_rfd = rfd >= 0;
    if (rfd < 0) {
        if (!isatty(STDIN_FILENO)) return 0;
        rfd = STDIN_FILENO;
    }

    struct termios saved, raw;
    bool restore = tcgetattr(rfd, &saved) == 0;
    if (restore) {
        raw = saved;
        raw.c_lflag &= (tcflag_t)~(ICANON | ECHO);
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 0;
        tcsetattr(rfd, TCSANOW, &raw);
    }

    int result = 0;
    if (kitty_graphics_emit_query(out, KITTY_PROBE_IMAGE_ID) &&
        fflush(out) == 0) {
        char buf[512];
        size_t len = 0;
        int elapsed = 0;
        while (elapsed < KITTY_PROBE_TIMEOUT_MS && len < sizeof(buf) - 1) {
            struct pollfd pfd = {.fd = rfd, .events = POLLIN};
            int step = 50;
            int rc = poll(&pfd, 1, step);
            elapsed += step;
            if (rc < 0 && errno != EINTR) break;
            if (rc <= 0 || !(pfd.revents & POLLIN)) continue;
            ssize_t n = read(rfd, buf + len, sizeof(buf) - 1 - len);
            if (n <= 0) continue;
            len += (size_t)n;
            buf[len] = '\0';
            char idtag[32];
            snprintf(idtag, sizeof(idtag), "i=%u", KITTY_PROBE_IMAGE_ID);
            const char *apc = strstr(buf, "\033_G");
            if (apc && strstr(apc, idtag) && strstr(apc, ";OK")) {
                result = 1;
                break;
            }
            /* DA1 reply (CSI ? … c) closes the handshake either way. */
            const char *da1 = strstr(buf, "\033[?");
            if (da1 && strchr(da1, 'c')) break;
        }
    }

    if (restore) tcsetattr(rfd, TCSANOW, &saved);
    if (close_rfd) close(rfd);
    return result;
}

bool kitty_graphics_probe(FILE *out) {
    static int cached = -1;
    if (cached < 0) {
        if (env_false("DSCO_KITTY_PROBE"))
            cached = 0;
        else
            cached = kitty_probe_run(out);
    }
    return cached == 1;
}

bool kitty_graphics_available(FILE *out) {
    if (!out) return false;
    int fd = fileno(out);
    if (fd < 0 || !isatty(fd)) return false;
    if (env_false("DSCO_KITTY_GRAPHICS") || env_false("DSCO_PIXEL_TUI"))
        return false;
    if (kitty_graphics_environment_hint()) return true;
    return kitty_graphics_probe(out);
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
