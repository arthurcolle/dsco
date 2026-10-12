#define _POSIX_C_SOURCE 200809L

#include "font_compat.h"

#include <stdlib.h>

#ifdef __APPLE__

#include <CoreGraphics/CoreGraphics.h>
#include <CoreText/CoreText.h>
#include <math.h>
#include <pthread.h>
#include <string.h>

bool font_compat_available(void) {
    return true;
}

typedef enum {
    FONT_KIND_MONO,
    FONT_KIND_PROSE,
    FONT_KIND_MATH,
} font_kind_t;

typedef struct {
    float point_size;
    bool bold;
    bool italic;
    font_kind_t kind;
    CTFontRef font;
} font_cache_entry_t;

static font_cache_entry_t s_font_cache[48];
static pthread_mutex_t s_font_cache_mutex = PTHREAD_MUTEX_INITIALIZER;

/* CoreText is called for every visible run. Allocating and zeroing a fresh
 * glyph mask for each run created hundreds of allocator round-trips in a
 * single animated frame. Each compositor thread now retains one grow-only
 * scratch mask; the used prefix is cleared before drawing. */
typedef struct {
    uint8_t *bytes;
    size_t capacity;
} font_mask_scratch_t;
static _Thread_local font_mask_scratch_t s_mask_scratch;

static uint8_t *font_mask_acquire(size_t bytes) {
    if (bytes == 0)
        return NULL;
    if (s_mask_scratch.capacity < bytes) {
        uint8_t *grown = realloc(s_mask_scratch.bytes, bytes);
        if (!grown)
            return NULL;
        s_mask_scratch.bytes = grown;
        s_mask_scratch.capacity = bytes;
    }
    memset(s_mask_scratch.bytes, 0, bytes);
    return s_mask_scratch.bytes;
}

/* Cache only immutable raster coverage, never RGB output: colors, opacity,
 * clipping and the destination remain live. A thread owns its cache and frees
 * retained fonts/text/masks on exit. Full keys verify every hash hit. */
#define FONT_RUN_SLOTS 256
#define FONT_RUN_WAYS 4
#define FONT_RUN_MAX_TEXT 2048
#define FONT_RUN_MAX_BYTES (3U * 1024U * 1024U)
typedef struct {
    uint64_t hash;
    CTFontRef font;
    char *text;
    size_t text_bytes;
    int advance;
    int natural_height;
    uint8_t *mask;
    size_t mask_bytes;
    int mask_width;
    int mask_height;
} font_run_entry_t;
typedef struct {
    font_run_entry_t entries[FONT_RUN_SLOTS];
    size_t bytes;
    unsigned evict;
} font_run_cache_t;
static pthread_key_t s_run_cache_key;
static pthread_once_t s_run_cache_once = PTHREAD_ONCE_INIT;
static bool s_run_cache_key_ready;
static _Thread_local font_run_cache_t *s_run_cache;

static void font_run_clear(font_run_cache_t *cache, font_run_entry_t *entry) {
    cache->bytes -= entry->text_bytes + entry->mask_bytes;
    free(entry->text);
    free(entry->mask);
    if (entry->font) CFRelease(entry->font);
    memset(entry, 0, sizeof(*entry));
}
static void font_run_destroy(void *value) {
    font_run_cache_t *cache = value;
    if (!cache) return;
    /* Another thread-exit destructor may call the font bridge afterwards. */
    s_run_cache = NULL;
    for (size_t i = 0; i < FONT_RUN_SLOTS; i++)
        font_run_clear(cache, &cache->entries[i]);
    free(cache);
}
static void font_run_key_init(void) {
    s_run_cache_key_ready = pthread_key_create(&s_run_cache_key, font_run_destroy) == 0;
}
static font_run_cache_t *font_run_cache(void) {
    if (s_run_cache) return s_run_cache;
    pthread_once(&s_run_cache_once, font_run_key_init);
    if (!s_run_cache_key_ready) return NULL;
    font_run_cache_t *cache = calloc(1, sizeof(*cache));
    if (!cache) return NULL;
    if (pthread_setspecific(s_run_cache_key, cache) != 0) {
        free(cache);
        return NULL;
    }
    s_run_cache = cache;
    return cache;
}
static font_run_entry_t *font_run_lookup(CTFontRef font, const char *text, bool insert) {
    size_t bytes = strnlen(text, FONT_RUN_MAX_TEXT + 1);
    if (bytes > FONT_RUN_MAX_TEXT) return NULL;
    font_run_cache_t *cache = font_run_cache();
    if (!cache) return NULL;
    uint64_t hash = (uint64_t)(uintptr_t)font ^ UINT64_C(1469598103934665603);
    for (size_t i = 0; i < bytes; i++)
        hash = (hash ^ (uint8_t)text[i]) * UINT64_C(1099511628211);
    size_t first = (hash % (FONT_RUN_SLOTS / FONT_RUN_WAYS)) * FONT_RUN_WAYS;
    font_run_entry_t *entry = NULL;
    for (size_t i = first; i < first + FONT_RUN_WAYS; i++) {
        font_run_entry_t *candidate = &cache->entries[i];
        if (candidate->font == font && candidate->hash == hash &&
            candidate->text_bytes == bytes + 1 && memcmp(candidate->text, text, bytes + 1) == 0)
            return candidate;
        if (!candidate->font) entry = candidate;
    }
    if (!insert) return NULL;
    if (!entry) entry = &cache->entries[first + cache->evict++ % FONT_RUN_WAYS];
    char *copy = malloc(bytes + 1);
    if (!copy) return NULL;
    memcpy(copy, text, bytes + 1);
    font_run_clear(cache, entry);
    while (cache->bytes + bytes + 1 > FONT_RUN_MAX_BYTES) {
        font_run_entry_t *victim = &cache->entries[cache->evict++ % FONT_RUN_SLOTS];
        if (victim != entry) font_run_clear(cache, victim);
    }
    entry->text = copy;
    entry->text_bytes = bytes + 1;
    entry->hash = hash;
    entry->font = font;
    CFRetain(font);
    cache->bytes += bytes + 1;
    return entry;
}
static void font_run_save_mask(font_run_entry_t *entry, const uint8_t *mask, int width, int height) {
    if (!entry || !mask) return;
    font_run_cache_t *cache = s_run_cache;
    size_t bytes = (size_t)width * (size_t)height;
    if (bytes > FONT_RUN_MAX_BYTES / 4) return;
    free(entry->mask);
    cache->bytes -= entry->mask_bytes;
    entry->mask = NULL;
    entry->mask_bytes = 0;
    while (cache->bytes + bytes > FONT_RUN_MAX_BYTES) {
        font_run_entry_t *victim = &cache->entries[cache->evict++ % FONT_RUN_SLOTS];
        if (victim != entry) font_run_clear(cache, victim);
    }
    entry->mask = malloc(bytes);
    if (!entry->mask) return;
    memcpy(entry->mask, mask, bytes);
    entry->mask_bytes = bytes;
    entry->mask_width = width;
    entry->mask_height = height;
    cache->bytes += bytes;
}
static void font_mask_blend(uint8_t *rgb, int stride, int x, int y, const uint8_t *mask,
                            int mask_w, int mask_h, uint8_t red, uint8_t green, uint8_t blue,
                            float opacity) {
    float alpha_scale = opacity < 0.0f ? 0.0f : (opacity > 1.0f ? 1.0f : opacity);
    for (int my = 0; my < mask_h; my++) {
        uint8_t *dst = rgb + (size_t)(y + my) * (size_t)stride + (size_t)x * 3U;
        const uint8_t *src = mask + (size_t)my * (size_t)mask_w;
        for (int mx = 0; mx < mask_w; mx++, dst += 3) {
            float a = ((float)src[mx] / 255.0f) * alpha_scale;
            if (a <= 0.0f) continue;
            dst[0] = (uint8_t)(dst[0] + ((float)red - dst[0]) * a);
            dst[1] = (uint8_t)(dst[1] + ((float)green - dst[1]) * a);
            dst[2] = (uint8_t)(dst[2] + ((float)blue - dst[2]) * a);
        }
    }
}

static CTFontRef create_font_kind(float point_size, bool bold, bool italic, font_kind_t kind) {
    pthread_mutex_lock(&s_font_cache_mutex);
    for (size_t i = 0; i < sizeof(s_font_cache) / sizeof(s_font_cache[0]); i++) {
        font_cache_entry_t *entry = &s_font_cache[i];
        if (entry->font && fabsf(entry->point_size - point_size) < 0.01f && entry->bold == bold &&
            entry->italic == italic && entry->kind == kind) {
            CFRetain(entry->font);
            pthread_mutex_unlock(&s_font_cache_mutex);
            return entry->font;
        }
    }
    pthread_mutex_unlock(&s_font_cache_mutex);

    const char *configured = getenv(kind == FONT_KIND_MATH ? "DSCO_PIXEL_MATH_FONT"
                                    : kind == FONT_KIND_PROSE ? "DSCO_PIXEL_PROSE_FONT"
                                                             : "DSCO_PIXEL_FONT");
    if (kind == FONT_KIND_PROSE && (!configured || !*configured))
        configured = getenv("DSCO_PIXEL_FONT");
    CTFontRef font = NULL;
    /* STIX Two Math is installed with modern macOS and is a serious OpenType
     * mathematical publishing face: italic variables, optical operators, and
     * an extensive scientific Unicode repertoire. It intentionally does not
     * inherit the terminal's monospace UI font. */
    if (kind == FONT_KIND_MATH && (!configured || !*configured))
        font = CTFontCreateWithName(CFSTR("STIX Two Math"), point_size, NULL);
    if (configured && *configured) {
        CFStringRef name =
            CFStringCreateWithCString(kCFAllocatorDefault, configured, kCFStringEncodingUTF8);
        if (name) {
            font = CTFontCreateWithName(name, point_size, NULL);
            CFRelease(name);
        }
    }
    if (!font && kind == FONT_KIND_PROSE)
        font = CTFontCreateUIFontForLanguage(kCTFontUIFontSystem, point_size, NULL);
    if (!font)
        font = CTFontCreateWithName(CFSTR("Menlo-Regular"), point_size, NULL);
    if (!font)
        font = CTFontCreateUIFontForLanguage(kCTFontUIFontUserFixedPitch, point_size, NULL);
    if (!font) {
        CFStringRef fallback = CFSTR("Menlo-Regular");
        font = CTFontCreateWithName(fallback, point_size, NULL);
    }
    if (font && (bold || italic)) {
        CTFontSymbolicTraits wanted = 0;
        if (bold)
            wanted |= kCTFontBoldTrait;
        if (italic)
            wanted |= kCTFontItalicTrait;
        CTFontRef strong =
            CTFontCreateCopyWithSymbolicTraits(font, point_size, NULL, wanted, wanted);
        if (strong) {
            CFRelease(font);
            font = strong;
        }
    }
    if (font) {
        pthread_mutex_lock(&s_font_cache_mutex);
        for (size_t i = 0; i < sizeof(s_font_cache) / sizeof(s_font_cache[0]); i++) {
            font_cache_entry_t *entry = &s_font_cache[i];
            if (entry->font)
                continue;
            entry->point_size = point_size;
            entry->bold = bold;
            entry->italic = italic;
            entry->kind = kind;
            entry->font = font;
            CFRetain(font);
            break;
        }
        pthread_mutex_unlock(&s_font_cache_mutex);
    }
    return font;
}

static CTFontRef create_font(float point_size, bool bold, bool italic) {
    return create_font_kind(point_size, bold, italic, FONT_KIND_MONO);
}

static int font_compat_measure_kind(const char *utf8, float point_size, bool bold, bool italic,
                                    font_kind_t kind) {
    if (!utf8 || !*utf8 || point_size <= 0.0f)
        return 0;
    CTFontRef font = create_font_kind(point_size, bold, italic, kind);
    font_run_entry_t *cached = font ? font_run_lookup(font, utf8, false) : NULL;
    if (cached) {
        int advance = cached->advance;
        CFRelease(font);
        return advance;
    }
    CFStringRef string =
        CFStringCreateWithCString(kCFAllocatorDefault, utf8, kCFStringEncodingUTF8);
    if (!font || !string) {
        if (font)
            CFRelease(font);
        if (string)
            CFRelease(string);
        return -1;
    }
    const void *keys[] = {kCTFontAttributeName};
    const void *values[] = {font};
    CFDictionaryRef attrs =
        CFDictionaryCreate(kCFAllocatorDefault, keys, values, 1, &kCFTypeDictionaryKeyCallBacks,
                           &kCFTypeDictionaryValueCallBacks);
    CFAttributedStringRef attributed =
        attrs ? CFAttributedStringCreate(kCFAllocatorDefault, string, attrs) : NULL;
    CTLineRef line = attributed ? CTLineCreateWithAttributedString(attributed) : NULL;
    CGFloat ascent = 0, descent = 0, leading = 0;
    int advance = line ? (int)ceil(CTLineGetTypographicBounds(line, &ascent, &descent, &leading)) : -1;
    if (advance >= 0) {
        cached = font_run_lookup(font, utf8, true);
        if (cached) {
            cached->advance = advance;
            cached->natural_height = (int)ceil(ascent + descent + leading) + 4;
        }
    }
    if (line)
        CFRelease(line);
    if (attributed)
        CFRelease(attributed);
    if (attrs)
        CFRelease(attrs);
    CFRelease(string);
    CFRelease(font);
    return advance;
}

int font_compat_measure_utf8(const char *utf8, float point_size, bool bold) {
    return font_compat_measure_kind(utf8, point_size, bold, false, FONT_KIND_MONO);
}

int font_compat_measure_utf8_styled(const char *utf8, float point_size, bool bold, bool italic) {
    return font_compat_measure_kind(utf8, point_size, bold, italic, FONT_KIND_MONO);
}

int font_compat_measure_prose_utf8(const char *utf8, float point_size, bool bold, bool italic) {
    return font_compat_measure_kind(utf8, point_size, bold, italic, FONT_KIND_PROSE);
}

int font_compat_line_height(float point_size, bool bold) {
    CTFontRef font = create_font(point_size, bold, false);
    if (!font)
        return -1;
    int height = (int)ceil(CTFontGetAscent(font) + CTFontGetDescent(font) + CTFontGetLeading(font));
    CFRelease(font);
    return height;
}

int font_compat_prose_line_height(float point_size, bool bold) {
    CTFontRef font = create_font_kind(point_size, bold, false, FONT_KIND_PROSE);
    if (!font)
        return -1;
    int height = (int)ceil(CTFontGetAscent(font) + CTFontGetDescent(font) + CTFontGetLeading(font));
    CFRelease(font);
    return height;
}

int font_compat_draw_rgb(uint8_t *rgb, int width, int height, int stride, int x, int y,
                         int max_width, const char *utf8, float point_size, bool bold, uint8_t red,
                         uint8_t green, uint8_t blue, float opacity) {
    return font_compat_draw_rgb_styled(rgb, width, height, stride, x, y, max_width, utf8,
                                       point_size, bold, false, red, green, blue, opacity);
}

static int font_compat_draw_rgb_kind(uint8_t *rgb, int width, int height, int stride, int x, int y,
                                     int max_width, const char *utf8, float point_size, bool bold,
                                     bool italic, font_kind_t kind, uint8_t red, uint8_t green,
                                     uint8_t blue, float opacity) {
    if (!rgb || width <= 0 || height <= 0 || stride < width * 3 || !utf8 || !*utf8 ||
        max_width <= 0 || point_size <= 0.0f)
        return 0;

    CTFontRef font = create_font_kind(point_size, bold, italic, kind);
    font_run_entry_t *cached = font ? font_run_lookup(font, utf8, false) : NULL;
    if (cached && cached->mask) {
        int mask_w = cached->advance + 4;
        if (mask_w > max_width) mask_w = max_width;
        if (mask_w > width - x) mask_w = width - x;
        int mask_h = cached->natural_height;
        if (mask_h > height - y) mask_h = height - y;
        if (mask_w == cached->mask_width && mask_h == cached->mask_height) {
            font_mask_blend(rgb, stride, x, y, cached->mask, mask_w, mask_h,
                            red, green, blue, opacity);
            int advance = cached->advance;
            CFRelease(font);
            return advance;
        }
    }
    CFStringRef string =
        CFStringCreateWithCString(kCFAllocatorDefault, utf8, kCFStringEncodingUTF8);
    if (!font || !string) {
        if (font)
            CFRelease(font);
        if (string)
            CFRelease(string);
        return -1;
    }

    const void *keys[] = {kCTFontAttributeName, kCTForegroundColorFromContextAttributeName};
    const void *values[] = {font, kCFBooleanTrue};
    CFDictionaryRef attrs =
        CFDictionaryCreate(kCFAllocatorDefault, keys, values, 2, &kCFTypeDictionaryKeyCallBacks,
                           &kCFTypeDictionaryValueCallBacks);
    CFAttributedStringRef attributed =
        attrs ? CFAttributedStringCreate(kCFAllocatorDefault, string, attrs) : NULL;
    CTLineRef line = attributed ? CTLineCreateWithAttributedString(attributed) : NULL;
    if (!line) {
        if (attributed)
            CFRelease(attributed);
        if (attrs)
            CFRelease(attrs);
        CFRelease(string);
        CFRelease(font);
        return -1;
    }

    CGFloat ascent = 0, descent = 0, leading = 0;
    double advance_d = CTLineGetTypographicBounds(line, &ascent, &descent, &leading);
    int advance = (int)ceil(advance_d);
    int mask_w = advance + 4;
    if (mask_w > max_width)
        mask_w = max_width;
    if (mask_w > width - x)
        mask_w = width - x;
    int mask_h = (int)ceil(ascent + descent + leading) + 4;
    if (mask_h > height - y)
        mask_h = height - y;
    if (mask_w <= 0 || mask_h <= 0) {
        CFRelease(line);
        CFRelease(attributed);
        CFRelease(attrs);
        CFRelease(string);
        CFRelease(font);
        return advance;
    }

    size_t mask_bytes = (size_t)mask_w * (size_t)mask_h;
    uint8_t *mask = font_mask_acquire(mask_bytes);
    CGColorSpaceRef gray = mask ? CGColorSpaceCreateDeviceGray() : NULL;
    CGContextRef context =
        gray ? CGBitmapContextCreate(mask, (size_t)mask_w, (size_t)mask_h, 8, (size_t)mask_w, gray,
                                     (CGBitmapInfo)kCGImageAlphaNone)
             : NULL;
    if (context) {
        CGContextSetAllowsAntialiasing(context, true);
        CGContextSetShouldAntialias(context, true);
        CGContextSetGrayFillColor(context, 1.0, 1.0);
        CGContextSetTextMatrix(context, CGAffineTransformIdentity);
        CGContextSetTextPosition(context, 2.0, descent + 2.0);
        CGContextClipToRect(context, CGRectMake(0, 0, mask_w, mask_h));
        CTLineDraw(line, context);

        cached = font_run_lookup(font, utf8, true);
        if (cached) {
            cached->advance = advance;
            cached->natural_height = (int)ceil(ascent + descent + leading) + 4;
            font_run_save_mask(cached, mask, mask_w, mask_h);
        }
        font_mask_blend(rgb, stride, x, y, mask, mask_w, mask_h, red, green, blue, opacity);
        CGContextRelease(context);
    }
    if (gray)
        CGColorSpaceRelease(gray);
    CFRelease(line);
    CFRelease(attributed);
    CFRelease(attrs);
    CFRelease(string);
    CFRelease(font);
    return context ? advance : -1;
}

int font_compat_draw_rgb_styled(uint8_t *rgb, int width, int height, int stride, int x, int y,
                                int max_width, const char *utf8, float point_size, bool bold,
                                bool italic, uint8_t red, uint8_t green, uint8_t blue,
                                float opacity) {
    return font_compat_draw_rgb_kind(rgb, width, height, stride, x, y, max_width, utf8, point_size,
                                     bold, italic, FONT_KIND_MONO, red, green, blue, opacity);
}

int font_compat_draw_prose_rgb(uint8_t *rgb, int width, int height, int stride, int x, int y,
                               int max_width, const char *utf8, float point_size, bool bold,
                               bool italic, uint8_t red, uint8_t green, uint8_t blue,
                               float opacity) {
    return font_compat_draw_rgb_kind(rgb, width, height, stride, x, y, max_width, utf8, point_size,
                                     bold, italic, FONT_KIND_PROSE, red, green, blue, opacity);
}

int font_compat_measure_math_utf8(const char *utf8, float point_size, bool bold) {
    return font_compat_measure_kind(utf8, point_size, bold, false, FONT_KIND_MATH);
}

int font_compat_draw_math_rgb(uint8_t *rgb, int width, int height, int stride, int x, int y,
                              int max_width, const char *utf8, float point_size, bool bold,
                              uint8_t red, uint8_t green, uint8_t blue, float opacity) {
    return font_compat_draw_rgb_kind(rgb, width, height, stride, x, y, max_width, utf8, point_size,
                                     bold, false, FONT_KIND_MATH, red, green, blue, opacity);
}

#else

bool font_compat_available(void) {
    return false;
}

int font_compat_measure_utf8(const char *utf8, float point_size, bool bold) {
    (void)utf8;
    (void)point_size;
    (void)bold;
    return -1;
}

int font_compat_measure_utf8_styled(const char *utf8, float point_size, bool bold, bool italic) {
    (void)utf8;
    (void)point_size;
    (void)bold;
    (void)italic;
    return -1;
}

int font_compat_line_height(float point_size, bool bold) {
    (void)point_size;
    (void)bold;
    return -1;
}

int font_compat_draw_rgb(uint8_t *rgb, int width, int height, int stride, int x, int y,
                         int max_width, const char *utf8, float point_size, bool bold, uint8_t red,
                         uint8_t green, uint8_t blue, float opacity) {
    (void)rgb;
    (void)width;
    (void)height;
    (void)stride;
    (void)x;
    (void)y;
    (void)max_width;
    (void)utf8;
    (void)point_size;
    (void)bold;
    (void)red;
    (void)green;
    (void)blue;
    (void)opacity;
    return -1;
}

int font_compat_draw_rgb_styled(uint8_t *rgb, int width, int height, int stride, int x, int y,
                                int max_width, const char *utf8, float point_size, bool bold,
                                bool italic, uint8_t red, uint8_t green, uint8_t blue,
                                float opacity) {
    (void)rgb;
    (void)width;
    (void)height;
    (void)stride;
    (void)x;
    (void)y;
    (void)max_width;
    (void)utf8;
    (void)point_size;
    (void)bold;
    (void)italic;
    (void)red;
    (void)green;
    (void)blue;
    (void)opacity;
    return -1;
}

int font_compat_measure_math_utf8(const char *utf8, float point_size, bool bold) {
    return font_compat_measure_utf8(utf8, point_size, bold);
}
int font_compat_draw_math_rgb(uint8_t *rgb, int width, int height, int stride, int x, int y,
                              int max_width, const char *utf8, float point_size, bool bold,
                              uint8_t red, uint8_t green, uint8_t blue, float opacity) {
    return font_compat_draw_rgb(rgb, width, height, stride, x, y, max_width, utf8, point_size, bold,
                                red, green, blue, opacity);
}

int font_compat_measure_prose_utf8(const char *utf8, float point_size, bool bold, bool italic) {
    return font_compat_measure_utf8_styled(utf8, point_size, bold, italic);
}

int font_compat_draw_prose_rgb(uint8_t *rgb, int width, int height, int stride, int x, int y,
                               int max_width, const char *utf8, float point_size, bool bold,
                               bool italic, uint8_t red, uint8_t green, uint8_t blue,
                               float opacity) {
    return font_compat_draw_rgb_styled(rgb, width, height, stride, x, y, max_width, utf8,
                                       point_size, bold, italic, red, green, blue, opacity);
}

int font_compat_prose_line_height(float point_size, bool bold) {
    return font_compat_line_height(point_size, bold);
}

#endif
