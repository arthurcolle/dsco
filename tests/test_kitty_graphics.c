/* Kitty graphics wire-contract tests.  These are deliberately headless: the
 * test captures APC bytes in a temporary stream and never requires Kitty. */

#include "kitty_graphics.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_pass;
static int g_fail;

#define CHECK(name, condition) do { \
    if (condition) g_pass++; \
    else { g_fail++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, name); } \
} while (0)

static size_t read_stream(FILE *stream, char *buffer, size_t capacity) {
    if (!stream || !buffer || capacity == 0) return 0;
    fflush(stream);
    if (fseek(stream, 0, SEEK_END) != 0) return 0;
    long end = ftell(stream);
    if (end < 0 || (unsigned long)end >= capacity) return 0;
    if (fseek(stream, 0, SEEK_SET) != 0) return 0;
    size_t n = fread(buffer, 1, (size_t)end, stream);
    buffer[n] = '\0';
    return n;
}

static void test_lengths(void) {
    CHECK("base64 zero", kitty_graphics_base64_encoded_length(0) == 0);
    CHECK("base64 one", kitty_graphics_base64_encoded_length(1) == 4);
    CHECK("base64 two", kitty_graphics_base64_encoded_length(2) == 4);
    CHECK("base64 three", kitty_graphics_base64_encoded_length(3) == 4);
    CHECK("base64 four", kitty_graphics_base64_encoded_length(4) == 8);
}

static void test_chunk_framing(void) {
    static const unsigned char payload[] = {0, 1, 2, 3, 4, 5};
    kitty_graphics_send_options_t options;
    kitty_graphics_send_options_default(&options);
    options.compress = false;
    options.chunk_size = 4;
    options.continuation_control = "q=2";

    FILE *stream = tmpfile();
    CHECK("tmpfile", stream != NULL);
    if (!stream) return;
    bool ok = kitty_graphics_send_pixels(stream, "a=t,t=d,f=24,i=7",
                                         payload, sizeof(payload), &options);
    char output[256];
    size_t n = read_stream(stream, output, sizeof(output));
    fclose(stream);

    CHECK("send succeeds", ok);
    CHECK("two APC chunks", n > 0 && strstr(output, "m=1;AAEC") != NULL);
    CHECK("continuation APC", strstr(output, "\033_Gq=2,m=0;AwQF\033\\") != NULL);
    CHECK("chunk terminators", n >= 4 && output[n - 2] == '\033' && output[n - 1] == '\\');
}

static void test_query(void) {
    FILE *stream = tmpfile();
    CHECK("query tmpfile", stream != NULL);
    if (!stream) return;
    bool ok = kitty_graphics_emit_query(stream, 42);
    char output[256];
    size_t n = read_stream(stream, output, sizeof(output));
    fclose(stream);
    CHECK("query succeeds", ok);
    CHECK("query APC", n > 0 && strstr(output,
          "\033_Gi=42,s=1,v=1,a=q,t=d,f=24,q=0;AAAA\033\\") != NULL);
    CHECK("query DA1", n >= 3 && strstr(output, "\033[c") != NULL);
}

static void test_empty_payload(void) {
    FILE *stream = tmpfile();
    CHECK("empty payload tmpfile", stream != NULL);
    if (!stream) return;
    bool ok = kitty_graphics_send_pixels(stream, "a=t,t=d,f=24,i=9",
                                         NULL, 0, NULL);
    char output[256];
    size_t n = read_stream(stream, output, sizeof(output));
    fclose(stream);
    CHECK("empty payload succeeds", ok);
    CHECK("empty payload APC", n > 12 && strstr(output, "\033_Ga=t,t=d,f=24,i=9,m=0;") != NULL);
}

static void test_send_stats(void) {
    static const unsigned char payload[] = {0, 1, 2, 3, 4, 5};
    kitty_graphics_send_options_t options;
    kitty_graphics_send_options_default(&options);
    options.compress = false;
    options.chunk_size = 4;

    FILE *stream = tmpfile();
    CHECK("stats tmpfile", stream != NULL);
    if (!stream) return;
    kitty_graphics_send_stats_t stats;
    bool ok = kitty_graphics_send_pixels_ex(
        stream, "a=t,t=d,f=24,i=17", payload, sizeof(payload), &options, &stats);
    fflush(stream);
    long wire_size = ftell(stream);
    fclose(stream);

    CHECK("instrumented send succeeds", ok);
    CHECK("stats preserve input bytes", stats.input_bytes == sizeof(payload));
    CHECK("stats preserve uncompressed bytes", stats.packed_bytes == sizeof(payload));
    CHECK("stats report base64 bytes", stats.encoded_bytes == 8);
    CHECK("stats report chunks", stats.chunks == 2);
    CHECK("stats report exact wire bytes",
          wire_size > 0 && stats.wire_bytes == (uint64_t)wire_size);
    CHECK("stats report working allocation", stats.peak_heap_bytes >= 9);
    CHECK("stats timings are nonnegative",
          stats.base64_ms >= 0.0 && stats.write_ms >= 0.0 &&
          stats.total_ms >= stats.base64_ms);
}

static void test_rgb_patch_contract(void) {
    static const unsigned char pixels[] = {1, 2, 3, 4, 5, 6};
    FILE *stream = tmpfile();
    CHECK("patch tmpfile", stream != NULL);
    if (!stream) return;
    bool ok = kitty_graphics_send_rgb_patch(
        stream, 77, 1, 3, 4, 2, 1, pixels, sizeof(pixels), NULL);
    ok = ok && kitty_graphics_select_frame(stream, 77, 1, NULL);
    char output[512];
    size_t n = read_stream(stream, output, sizeof(output));
    fclose(stream);

    CHECK("RGB patch sends", ok && n > 0);
    CHECK("RGB patch declares exact Kitty format",
          strstr(output,
                 "\033_Ga=f,t=d,f=24,i=77,r=1,x=3,y=4,s=2,v=1,X=1,q=2,o=z,m=0;") != NULL);
    CHECK("RGB patch re-selects edited frame",
          strstr(output, "\033_Ga=a,i=77,c=1,q=2,m=0;\033\\") != NULL);
    FILE *invalid = tmpfile();
    CHECK("patch validation tmpfile", invalid != NULL);
    if (invalid) {
        CHECK("RGB patch rejects mismatched payload",
              !kitty_graphics_send_rgb_patch(invalid, 77, 1,
                                             0, 0, 2, 1,
                                             pixels, sizeof(pixels) - 1, NULL));
        fclose(invalid);
    }
}

static void test_environment_hint(void) {
    const char *old_graphics = getenv("DSCO_KITTY_GRAPHICS");
    const char *old_pixel = getenv("DSCO_PIXEL_TUI");
    const char *old_term = getenv("TERM");
    const char *old_program = getenv("TERM_PROGRAM");
    const char *old_window = getenv("KITTY_WINDOW_ID");
    char saved_graphics[32], saved_pixel[32], saved_term[128], saved_program[128];
    char saved_window[64];
    bool had_graphics = old_graphics != NULL, had_pixel = old_pixel != NULL;
    bool had_term = old_term != NULL, had_program = old_program != NULL;
    bool had_window = old_window != NULL;
    if (had_graphics) snprintf(saved_graphics, sizeof(saved_graphics), "%s", old_graphics);
    if (had_pixel) snprintf(saved_pixel, sizeof(saved_pixel), "%s", old_pixel);
    if (had_term) snprintf(saved_term, sizeof(saved_term), "%s", old_term);
    if (had_program) snprintf(saved_program, sizeof(saved_program), "%s", old_program);
    if (had_window) snprintf(saved_window, sizeof(saved_window), "%s", old_window);

    unsetenv("DSCO_KITTY_GRAPHICS");
    unsetenv("DSCO_PIXEL_TUI");
    unsetenv("KITTY_WINDOW_ID");
    setenv("TERM", "xterm-256color", 1);
    setenv("TERM_PROGRAM", "Apple_Terminal", 1);
    CHECK("ordinary terminal hint off", !kitty_graphics_environment_hint());
    setenv("TERM_PROGRAM", "Ghostty", 1);
    CHECK("known terminal hint on", kitty_graphics_environment_hint());
    setenv("DSCO_PIXEL_TUI", "0", 1);
    CHECK("pixel TUI opt-out wins", !kitty_graphics_environment_hint());
    unsetenv("DSCO_PIXEL_TUI");
    setenv("DSCO_KITTY_GRAPHICS", "off", 1);
    CHECK("explicit off wins", !kitty_graphics_environment_hint());
    setenv("TERM_PROGRAM", "Apple_Terminal", 1);
    setenv("DSCO_KITTY_GRAPHICS", "force", 1);
    CHECK("force wins", kitty_graphics_environment_hint());

    if (had_graphics) setenv("DSCO_KITTY_GRAPHICS", saved_graphics, 1);
    else unsetenv("DSCO_KITTY_GRAPHICS");
    if (had_pixel) setenv("DSCO_PIXEL_TUI", saved_pixel, 1);
    else unsetenv("DSCO_PIXEL_TUI");
    if (had_term) setenv("TERM", saved_term, 1);
    else unsetenv("TERM");
    if (had_program) setenv("TERM_PROGRAM", saved_program, 1);
    else unsetenv("TERM_PROGRAM");
    if (had_window) setenv("KITTY_WINDOW_ID", saved_window, 1);
    else unsetenv("KITTY_WINDOW_ID");
}

int main(void) {
    test_lengths();
    test_chunk_framing();
    test_query();
    test_empty_payload();
    test_send_stats();
    test_rgb_patch_contract();
    test_environment_hint();
    printf("kitty graphics: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
