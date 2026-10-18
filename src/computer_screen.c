#include "computer_screen.h"
#include "process_capture.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if defined(__APPLE__) && !defined(DSCO_COMPUTER_SCREEN_NO_MACOS)
#define COMPUTER_SCREEN_HAS_MACOS 1
#include <ApplicationServices/ApplicationServices.h>
#else
#define COMPUTER_SCREEN_HAS_MACOS 0
#endif

void computer_space_fit(int display_w, int display_h, int max_long, int max_short,
                        computer_space_t *out) {
    if (display_w < 1) display_w = 1;
    if (display_h < 1) display_h = 1;
    if (max_long < COMPUTER_SCREEN_MIN_EDGE) max_long = COMPUTER_SCREEN_MIN_EDGE;
    if (max_short < COMPUTER_SCREEN_MIN_EDGE) max_short = COMPUTER_SCREEN_MIN_EDGE;
    bool landscape = display_w >= display_h;
    double long_edge = landscape ? display_w : display_h;
    double short_edge = landscape ? display_h : display_w;
    double scale = fmin(1.0, fmin(max_long / long_edge, max_short / short_edge));
    out->display_w = display_w;
    out->display_h = display_h;
    out->shot_w = (int)lround(display_w * scale);
    out->shot_h = (int)lround(display_h * scale);
    if (out->shot_w < 1) out->shot_w = 1;
    if (out->shot_h < 1) out->shot_h = 1;
}

void computer_space_current(int display_w, int display_h, computer_space_t *out) {
    int max_long = COMPUTER_SCREEN_DEFAULT_LONG, max_short = COMPUTER_SCREEN_DEFAULT_SHORT;
    const char *env = getenv("DSCO_COMPUTER_SCREENSHOT_MAX");
    int a = 0, b = 0;
    if (env && sscanf(env, "%dx%d", &a, &b) == 2 && a > 0 && b > 0) {
        max_long = a > b ? a : b;
        max_short = a > b ? b : a;
    }
    computer_space_fit(display_w, display_h, max_long, max_short, out);
}

static int clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

void computer_space_to_display(const computer_space_t *s, int x, int y, int *px, int *py) {
    *px = clamp((int)lround((double)x * s->display_w / s->shot_w), 0, s->display_w - 1);
    *py = clamp((int)lround((double)y * s->display_h / s->shot_h), 0, s->display_h - 1);
}

void computer_space_to_shot(const computer_space_t *s, int px, int py, int *x, int *y) {
    *x = (int)lround((double)px * s->shot_w / s->display_w);
    *y = (int)lround((double)py * s->shot_h / s->display_h);
}

#if COMPUTER_SCREEN_HAS_MACOS
static bool run(char *const argv[], int timeout_ms, process_capture_t *keep) {
    process_capture_t c;
    bool ok = process_capture(argv[0], argv, timeout_ms, 4096, &c) && c.exit_code == 0;
    if (keep && ok) *keep = c;
    else process_capture_free(&c);
    return ok;
}

static bool image_size(const char *path, int *w, int *h) {
    process_capture_t c;
    char *argv[] = {"/usr/bin/sips", "-g", "pixelWidth", "-g", "pixelHeight", (char *)path, NULL};
    if (!run(argv, 5000, &c)) return false;
    const char *pw = c.output ? strstr(c.output, "pixelWidth:") : NULL;
    const char *ph = c.output ? strstr(c.output, "pixelHeight:") : NULL;
    bool ok = pw && ph && sscanf(pw, "pixelWidth: %d", w) == 1 && sscanf(ph, "pixelHeight: %d", h) == 1 &&
              *w > 0 && *h > 0;
    process_capture_free(&c);
    return ok;
}

static bool temp_path(char *buf, size_t len, const char *pattern) {
    if (snprintf(buf, len, "%s", pattern) >= (int)len) return false;
    int fd = mkstemp(buf);
    if (fd < 0) return false;
    close(fd);
    return true;
}

/* Full-resolution PNG of the main display. */
static bool capture_native(char *path, size_t len) {
    if (!CGPreflightScreenCaptureAccess() || !temp_path(path, len, "/tmp/dsco_capture_XXXXXX")) return false;
    char *argv[] = {"/usr/sbin/screencapture", "-x", "-t", "png", "-D", "1", path, NULL};
    if (run(argv, 8000, NULL)) return true;
    unlink(path);
    return false;
}

static bool export_jpeg(const char *src, int w, int h, char *out, size_t len) {
    if (!temp_path(out, len, "/tmp/dsco_shot_XXXXXX")) return false;
    char ws[16], hs[16];
    snprintf(ws, sizeof(ws), "%d", w);
    snprintf(hs, sizeof(hs), "%d", h);
    /* sips picks the encoder from -s format; the temp name has no extension. */
    char *argv[] = {"/usr/bin/sips", "-s", "format", "jpeg", "-s", "formatOptions", "82",
                    "-z", hs, ws, (char *)src, "--out", out, NULL};
    int got_w = 0, got_h = 0;
    if (run(argv, 8000, NULL) && image_size(out, &got_w, &got_h) && got_w == w && got_h == h) return true;
    unlink(out);
    return false;
}

bool computer_screen_capture(const computer_space_t *s, char *path, size_t path_len) {
    char native[64];
    if (!capture_native(native, sizeof(native))) return false;
    bool ok = export_jpeg(native, s->shot_w, s->shot_h, path, path_len);
    unlink(native);
    return ok;
}

bool computer_screen_zoom(const computer_space_t *s, int x1, int y1, int x2, int y2,
                          char *path, size_t path_len, int *out_w, int *out_h) {
    if (x2 < x1) { int t = x1; x1 = x2; x2 = t; }
    if (y2 < y1) { int t = y1; y1 = y2; y2 = t; }
    x1 = clamp(x1, 0, s->shot_w - 1); x2 = clamp(x2, x1 + 1, s->shot_w);
    y1 = clamp(y1, 0, s->shot_h - 1); y2 = clamp(y2, y1 + 1, s->shot_h);
    char native[64];
    if (!capture_native(native, sizeof(native))) return false;
    int nw = 0, nh = 0;
    bool ok = image_size(native, &nw, &nh);
    if (ok) {
        /* Region in native pixels (Retina captures are 2x display points). */
        int cx = (int)floor((double)x1 * nw / s->shot_w), cy = (int)floor((double)y1 * nh / s->shot_h);
        int cw = (int)ceil((double)(x2 - x1) * nw / s->shot_w), ch = (int)ceil((double)(y2 - y1) * nh / s->shot_h);
        cw = clamp(cw, 1, nw - cx);
        ch = clamp(ch, 1, nh - cy);
        char ws[16], hs[16], xs[16], ys[16];
        snprintf(ws, sizeof(ws), "%d", cw); snprintf(hs, sizeof(hs), "%d", ch);
        snprintf(xs, sizeof(xs), "%d", cx); snprintf(ys, sizeof(ys), "%d", cy);
        char *crop[] = {"/usr/bin/sips", "-c", hs, ws, "--cropOffset", ys, xs, native, NULL};
        ok = run(crop, 8000, NULL);
        if (ok) {
            computer_space_t fit;
            computer_space_current(cw, ch, &fit); /* only downscales */
            ok = export_jpeg(native, fit.shot_w, fit.shot_h, path, path_len);
            if (ok) { *out_w = fit.shot_w; *out_h = fit.shot_h; }
        }
    }
    unlink(native);
    return ok;
}
#else
bool computer_screen_capture(const computer_space_t *s, char *path, size_t path_len) {
    (void)s; (void)path; (void)path_len;
    return false;
}
bool computer_screen_zoom(const computer_space_t *s, int x1, int y1, int x2, int y2,
                          char *path, size_t path_len, int *out_w, int *out_h) {
    (void)s; (void)x1; (void)y1; (void)x2; (void)y2; (void)path; (void)path_len; (void)out_w; (void)out_h;
    return false;
}
#endif
