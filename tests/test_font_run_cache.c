/* Byte-for-byte reference comparison and a repeated transcript benchmark.
 * The runner links an independently compiled pre-cache font implementation. */
#include "../src/font_compat.c"
#include <assert.h>
#include <stdio.h>
#include <time.h>

extern int baseline_font_compat_measure_utf8_styled(const char *, float, bool, bool);
extern int baseline_font_compat_measure_prose_utf8(const char *, float, bool, bool);
extern int baseline_font_compat_measure_math_utf8(const char *, float, bool);
extern int baseline_font_compat_draw_rgb_styled(uint8_t *, int, int, int, int, int, int,
    const char *, float, bool, bool, uint8_t, uint8_t, uint8_t, float);
extern int baseline_font_compat_draw_prose_rgb(uint8_t *, int, int, int, int, int, int,
    const char *, float, bool, bool, uint8_t, uint8_t, uint8_t, float);
extern int baseline_font_compat_draw_math_rgb(uint8_t *, int, int, int, int, int, int,
    const char *, float, bool, uint8_t, uint8_t, uint8_t, float);

#ifdef __APPLE__
static int measure(bool old, int kind, const char *text, float size, bool bold, bool italic) {
    if (kind == FONT_KIND_MATH)
        return old ? baseline_font_compat_measure_math_utf8(text, size, bold)
                   : font_compat_measure_math_utf8(text, size, bold);
    if (kind == FONT_KIND_PROSE)
        return old ? baseline_font_compat_measure_prose_utf8(text, size, bold, italic)
                   : font_compat_measure_prose_utf8(text, size, bold, italic);
    return old ? baseline_font_compat_measure_utf8_styled(text, size, bold, italic)
               : font_compat_measure_utf8_styled(text, size, bold, italic);
}
static int draw(bool old, int kind, uint8_t *pixels, const char *text, float size,
                bool bold, bool italic, int clip, int height, int x, int y,
                uint8_t color, float opacity) {
#define DRAW_ARGS pixels, 1024, height, 3072, x, y, clip, text, size, bold
    if (kind == FONT_KIND_MATH)
        return old ? baseline_font_compat_draw_math_rgb(DRAW_ARGS, color, 177, 210, opacity)
                   : font_compat_draw_math_rgb(DRAW_ARGS, color, 177, 210, opacity);
    if (kind == FONT_KIND_PROSE)
        return old ? baseline_font_compat_draw_prose_rgb(DRAW_ARGS, italic, color, 177, 210, opacity)
                   : font_compat_draw_prose_rgb(DRAW_ARGS, italic, color, 177, 210, opacity);
    return old ? baseline_font_compat_draw_rgb_styled(DRAW_ARGS, italic, color, 177, 210, opacity)
               : font_compat_draw_rgb_styled(DRAW_ARGS, italic, color, 177, 210, opacity);
#undef DRAW_ARGS
}
static const char *samples[] = {
    "Readable prose — native fonts, 73°F.",
    "iii WWW 0123 -> code_call(x);", "∫₀¹ x² dx = ⅓", "日本語 العربية café 👩🏽‍💻", "", "\xff"
};
static void pixel_equivalence(void) {
    uint8_t old[1024 * 96 * 3], current[sizeof(old)];
    float sizes[] = {18.0f, 18.005f, 18.02f, 25.5f};
    for (int kind=0; kind<3; kind++) for (int style=0; style<4; style++)
        for (size_t size=0; size<sizeof(sizes)/sizeof(sizes[0]); size++)
        for (size_t t=0; t<sizeof(samples)/sizeof(samples[0]); t++) {
            const char *text = samples[t];
            assert(measure(false, kind, text, sizes[size], style&1, style&2) ==
                   measure(true, kind, text, sizes[size], style&1, style&2));
            for (int repeat=0; repeat<6; repeat++) {
                memset(old, 53, sizeof(old));
                memset(current, 53, sizeof(current));
                int clip = repeat < 2 ? 900 : (repeat < 4 ? 101 : 19);
                int height = repeat < 4 ? 96 : 24;
                float opacity = repeat%2 ? 0.37f : 1.0f;
                int a=draw(true, kind, old, text, sizes[size], style&1, style&2,
                           clip, height, 7, 3, 231-repeat, opacity);
                int b=draw(false, kind, current, text, sizes[size], style&1, style&2,
                           clip, height, 7, 3, 231-repeat, opacity);
                assert(a == b && memcmp(old, current, sizeof(old)) == 0);
            }
        }
    for (int n=0; n<1400; n++) {
        char text[2048];
        snprintf(text, sizeof(text), "%d. Cache pressure: ", n);
        size_t offset = strlen(text);
        memset(text+offset, 'W', sizeof(text)-offset-1);
        text[sizeof(text)-1]='\0';
        draw(false, FONT_KIND_PROSE, current, text, 25.5f, false, false, 1024, 96, 0, 0, 240, 1);
        assert(s_run_cache->bytes <= FONT_RUN_MAX_BYTES);
    }
}
static void *thread_check(void *unused) {
    (void)unused;
    uint8_t pixels[1024*96*3] = {0};
    for (int i=0; i<150; i++) {
        int expected = measure(true, FONT_KIND_PROSE, samples[0], 18, false, false);
        assert(measure(false, FONT_KIND_PROSE, samples[0], 18, false, false) == expected);
        assert(draw(false, FONT_KIND_PROSE, pixels, samples[0], 18, false, false,
                    1000, 96, 0, 0, 240, 1) == expected);
    }
    return NULL;
}
static double now(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}
static double bench(bool old, bool drawing) {
    uint8_t pixels[1024 * 96 * 3] = {0};
    char lines[64][128];
    for (int i=0; i<64; i++) snprintf(lines[i], sizeof(lines[i]),
        "Transcript row %02d. Native streaming preserves words and punctuation.", i);
    double start=now();
    volatile int result=0;
    for (int frame=0; frame<300; frame++) for (int row=0; row<64; row++) {
        if (drawing) result += draw(old, FONT_KIND_PROSE, pixels, lines[row], 18.0f,
                                     false, false, 1000, 96, 0, 0, 240, 0.85f);
        else result += measure(old, FONT_KIND_PROSE, lines[row], 18.0f, false, false);
    }
    (void)result;
    return (now()-start)*1000;
}
#endif
int main(void) {
#ifdef __APPLE__
    pixel_equivalence();
    pthread_t threads[4];
    for (size_t i=0; i<4; i++) assert(!pthread_create(&threads[i], NULL, thread_check, NULL));
    for (size_t i=0; i<4; i++) assert(!pthread_join(threads[i], NULL));
    double old_measure=bench(true,false), new_measure=bench(false,false);
    double old_draw=bench(true,true), new_draw=bench(false,true);
    printf("{\"pixels_identical\":true,\"thread_tests\":4,\"cache_bytes\":%zu,"
           "\"measurement_before_ms\":%.3f,\"measurement_after_ms\":%.3f,"
           "\"render_before_ms\":%.3f,\"render_after_ms\":%.3f}\n",
           s_run_cache->bytes,old_measure,new_measure,old_draw,new_draw);
#else
    puts("SKIP: CoreText cache requires macOS");
#endif
    return 0;
}
