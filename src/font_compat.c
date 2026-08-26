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

typedef struct {
    float point_size;
    bool bold;
    bool italic;
    bool math;
    CTFontRef font;
} font_cache_entry_t;

static font_cache_entry_t s_font_cache[20];
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

static CTFontRef create_font_kind(float point_size, bool bold, bool italic, bool math) {
    pthread_mutex_lock(&s_font_cache_mutex);
    for (size_t i = 0; i < sizeof(s_font_cache) / sizeof(s_font_cache[0]); i++) {
        font_cache_entry_t *entry = &s_font_cache[i];
        if (entry->font && fabsf(entry->point_size - point_size) < 0.01f && entry->bold == bold &&
            entry->italic == italic && entry->math == math) {
            CFRetain(entry->font);
            pthread_mutex_unlock(&s_font_cache_mutex);
            return entry->font;
        }
    }
    pthread_mutex_unlock(&s_font_cache_mutex);

    const char *configured = getenv(math ? "DSCO_PIXEL_MATH_FONT" : "DSCO_PIXEL_FONT");
    CTFontRef font = NULL;
    /* STIX Two Math is installed with modern macOS and is a serious OpenType
     * mathematical publishing face: italic variables, optical operators, and
     * an extensive scientific Unicode repertoire. It intentionally does not
     * inherit the terminal's monospace UI font. */
    if (math && (!configured || !*configured))
        font = CTFontCreateWithName(CFSTR("STIX Two Math"), point_size, NULL);
    if (configured && *configured) {
        CFStringRef name =
            CFStringCreateWithCString(kCFAllocatorDefault, configured, kCFStringEncodingUTF8);
        if (name) {
            font = CTFontCreateWithName(name, point_size, NULL);
            CFRelease(name);
        }
    }
    if (!font) {
        /* Iosevka Term is deliberately narrow: it gives the native transcript
         * terminal density while retaining the Nerd Font private-use glyphs. */
        font = CTFontCreateWithName(CFSTR("IosevkaTerm Nerd Font Mono"), point_size, NULL);
    }
    if (!font)
        font = CTFontCreateUIFontForLanguage(kCTFontUIFontUserFixedPitch, point_size, NULL);
    if (!font) {
        CFStringRef fallback = CFSTR("Menlo");
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
            entry->math = math;
            entry->font = font;
            CFRetain(font);
            break;
        }
        pthread_mutex_unlock(&s_font_cache_mutex);
    }
    return font;
}

static CTFontRef create_font(float point_size, bool bold, bool italic) {
    return create_font_kind(point_size, bold, italic, false);
}

int font_compat_measure_utf8(const char *utf8, float point_size, bool bold) {
    return font_compat_measure_utf8_styled(utf8, point_size, bold, false);
}

int font_compat_measure_utf8_styled(const char *utf8, float point_size, bool bold, bool italic) {
    if (!utf8 || !*utf8 || point_size < 4.0f)
        return 0;
    CTFontRef font = create_font(point_size, bold, italic);
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
    int advance = line ? (int)ceil(CTLineGetTypographicBounds(line, NULL, NULL, NULL)) : -1;
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

int font_compat_line_height(float point_size, bool bold) {
    CTFontRef font = create_font(point_size, bold, false);
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
                                     bool italic, bool math, uint8_t red, uint8_t green,
                                     uint8_t blue, float opacity) {
    if (!rgb || width <= 0 || height <= 0 || stride < width * 3 || !utf8 || !*utf8 ||
        max_width <= 0 || point_size < 4.0f)
        return 0;

    CTFontRef font = create_font_kind(point_size, bold, italic, math);
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

        float alpha_scale = opacity < 0.0f ? 0.0f : (opacity > 1.0f ? 1.0f : opacity);
        for (int my = 0; my < mask_h; my++) {
            uint8_t *dst = rgb + (size_t)(y + my) * (size_t)stride + (size_t)x * 3U;
            const uint8_t *src = mask + (size_t)my * (size_t)mask_w;
            for (int mx = 0; mx < mask_w; mx++, dst += 3) {
                float a = ((float)src[mx] / 255.0f) * alpha_scale;
                if (a <= 0.0f)
                    continue;
                dst[0] = (uint8_t)(dst[0] + ((float)red - dst[0]) * a);
                dst[1] = (uint8_t)(dst[1] + ((float)green - dst[1]) * a);
                dst[2] = (uint8_t)(dst[2] + ((float)blue - dst[2]) * a);
            }
        }
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
                                     bold, italic, false, red, green, blue, opacity);
}

int font_compat_measure_math_utf8(const char *utf8, float point_size, bool bold) {
    if (!utf8 || !*utf8 || point_size < 4.0f)
        return 0;
    CTFontRef font = create_font_kind(point_size, bold, false, true);
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
    int advance = line ? (int)ceil(CTLineGetTypographicBounds(line, NULL, NULL, NULL)) : -1;
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

int font_compat_draw_math_rgb(uint8_t *rgb, int width, int height, int stride, int x, int y,
                              int max_width, const char *utf8, float point_size, bool bold,
                              uint8_t red, uint8_t green, uint8_t blue, float opacity) {
    return font_compat_draw_rgb_kind(rgb, width, height, stride, x, y, max_width, utf8, point_size,
                                     bold, false, true, red, green, blue, opacity);
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

#endif
