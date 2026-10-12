/* Standalone: cc -std=c11 -Iinclude tests/test_kitty_patch_live.c
 * src/kitty_graphics.c -lz -o build/test_kitty_patch_live
 * Default: decode/decompress and verify real patch wire, no terminal output.
 * --live [--acks] [--seconds N] [--log NEW_PATH]: owned Kitty fixture only.
 * Red root; at 2s the middle becomes solid green (1 chunk), then at 6s
 * textured green (13 chunks). Both patches edit the same root-frame rectangle.
 * q exits; default timeout 120 seconds. --acks changes only q=2 to q=0 in
 * captured helper output, exposing protocol errors otherwise suppressed.
 * Protocol: https://sw.kovidgoyal.net/kitty/graphics-protocol/#animation */
#define _DARWIN_C_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#include "kitty_graphics.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>

enum { WIDTH = 320, HEIGHT = 200, PATCH_W = 160, PATCH_H = 100, PATCH_X = 80, PATCH_Y = 50 };
static volatile sig_atomic_t stopped;
static void stop_fixture(int signo) { (void)signo; stopped = 1; }
static double now_seconds(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}
static void green_pixels(unsigned char *pixels) {
    uint32_t state = 0x41a92f37;
    for (size_t i = 0; i < PATCH_W * PATCH_H; i++) {
        state ^= state << 13; state ^= state >> 17; state ^= state << 5;
        pixels[i * 3] = (unsigned char)(state & 31);
        pixels[i * 3 + 1] = (unsigned char)(192 + ((state >> 8) & 63));
        pixels[i * 3 + 2] = (unsigned char)((state >> 16) & 31);
    }
}
static char *patch_wire(uint32_t id, const unsigned char *pixels, size_t *size,
                        kitty_graphics_send_stats_t *stats) {
    char *wire = NULL;
    FILE *stream = open_memstream(&wire, size);
    if (!stream) return NULL;
    bool ok = kitty_graphics_send_rgb_patch(stream, id, 1, PATCH_X, PATCH_Y,
        PATCH_W, PATCH_H, pixels, PATCH_W * PATCH_H * 3, stats);
    ok = kitty_graphics_select_frame(stream, id, 1, NULL) && ok;
    if (fclose(stream) != 0) ok = false;
    if (!ok) { free(wire); return NULL; }
    return wire;
}
static int base64_value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    return c == '+' ? 62 : c == '/' ? 63 : -1;
}
static int wire_case(bool noisy) {
    unsigned char pixels[PATCH_W * PATCH_H * 3];
    if (noisy) green_pixels(pixels);
    else for (size_t i = 0; i < PATCH_W * PATCH_H; i++) {
        pixels[i * 3] = 0; pixels[i * 3 + 1] = 255; pixels[i * 3 + 2] = 0;
    }
    size_t size = 0; kitty_graphics_send_stats_t stats;
    char *wire = patch_wire(77123, pixels, &size, &stats); assert(wire);
    assert((noisy ? stats.chunks > 1 : stats.chunks == 1) && stats.input_bytes == sizeof(pixels));
    char *encoded = calloc(size + 1, 1); assert(encoded);
    size_t encoded_size = 0, chunks = 0, selected = 0;
    const char *p = wire;
    while (*p) {
        assert(!strncmp(p, "\033_G", 3));
        const char *semi = strchr(p + 3, ';'), *end = semi ? strstr(semi + 1, "\033\\") : NULL;
        assert(semi && end && semi - p < 512);
        char header[512]; memcpy(header, p + 3, (size_t)(semi - p - 3)); header[semi - p - 3] = 0;
        size_t bytes = (size_t)(end - semi - 1);
        if (!strncmp(header, "a=a,", 4)) {
            assert(!strcmp(header, "a=a,i=77123,c=1,q=2,m=0") && !bytes);
            selected++;
        } else {
            assert(bytes <= 4096);
            if (strstr(header, "m=1")) assert(bytes % 4 == 0);
            if (chunks == 0) {
                assert(strstr(header, "a=f,t=d,f=24,i=77123,r=1,x=80,y=50,s=160,v=100,X=1,q=2,o=z,m="));
            } else assert(!strcmp(header, "a=f,i=77123,r=1,q=2,m=1") || !strcmp(header, "a=f,i=77123,r=1,q=2,m=0"));
            memcpy(encoded + encoded_size, semi + 1, bytes); encoded_size += bytes;
            chunks++;
        }
        p = end + 2;
    }
    assert(chunks == stats.chunks && selected == 1 && encoded_size == stats.encoded_bytes);
    unsigned char *compressed = malloc(encoded_size); assert(compressed);
    size_t compressed_size = 0; unsigned accumulator = 0; int bits = 0;
    for (size_t i = 0; i < encoded_size && encoded[i] != '='; i++) {
        int value = base64_value(encoded[i]); assert(value >= 0);
        accumulator = (accumulator << 6) | (unsigned)value; bits += 6;
        if (bits >= 8) { bits -= 8; compressed[compressed_size++] = (unsigned char)(accumulator >> bits); }
    }
    unsigned char decoded[sizeof(pixels)]; uLongf decoded_size = sizeof(decoded);
    assert(uncompress(decoded, &decoded_size, compressed, compressed_size) == Z_OK);
    assert(decoded_size == sizeof(pixels) && !memcmp(decoded, pixels, sizeof(pixels)));
    printf("Kitty patch wire: %zu chunks, RGB24 + a=f continuations + zlib/base64 exact round-trip passed\n", chunks);
    free(compressed); free(encoded); free(wire);
    return 0;
}
static void log_rx(const unsigned char *data, size_t size) {
    fputs("RX ", stderr);
    for (size_t i = 0; i < size; i++) {
        unsigned char c = data[i];
        if (c >= 32 && c < 127) fputc(c, stderr);
        else fprintf(stderr, "\\x%02x", c);
    }
    fputc('\n', stderr); fflush(stderr);
}
static int live_fixture(bool acks, unsigned seconds, const char *log_path) {
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        fputs("--live requires this owned fixture's Kitty PTY on stdin/stdout\n", stderr); return 2;
    }
    if (log_path) {
        int fd = open(log_path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
        if (fd < 0) { perror("owned acknowledgement log"); return 2; }
        if (dup2(fd, STDERR_FILENO) < 0) { close(fd); return 2; }
        if (fd != STDERR_FILENO) close(fd);
    }
    struct termios saved, raw;
    if (tcgetattr(STDIN_FILENO, &saved) != 0) return 2;
    raw = saved; raw.c_lflag &= ~(ICANON | ECHO); raw.c_cc[VMIN] = 0; raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) return 2;
    signal(SIGINT, stop_fixture); signal(SIGTERM, stop_fixture);
    uint32_t id = 0x48500000u | ((uint32_t)getpid() & 0x000fffffu);
    unsigned char *red = malloc(WIDTH * HEIGHT * 3), *green = malloc(PATCH_W * PATCH_H * 3);
    int status = 1;
    if (!red || !green) goto done;
    for (size_t i = 0; i < WIDTH * HEIGHT; i++) { red[i * 3] = 220; red[i * 3 + 1] = 16; red[i * 3 + 2] = 16; }
    green_pixels(green);
    fputs("\033[2J\033[HKitty helper fixture: RED; solid GREEN at 2s, textured GREEN at 6s. q exits.\033[3;2H", stdout);
    char control[192];
    snprintf(control, sizeof(control), "a=T,t=d,f=24,s=%d,v=%d,i=%u,c=40,r=12,C=1,z=2,q=0,o=z", WIDTH, HEIGHT, id);
    kitty_graphics_send_options_t options; kitty_graphics_send_options_default(&options);
    options.continuation_control = "q=0";
    if (!kitty_graphics_send_pixels(stdout, control, red, WIDTH * HEIGHT * 3, &options) || fflush(stdout)) goto done;
    fprintf(stderr, "RED image=%u width=%d height=%d patch=%dx%d at %d,%d quiet_override=%d\n", id, WIDTH, HEIGHT, PATCH_W, PATCH_H, PATCH_X, PATCH_Y, acks);
    fflush(stderr);
    double start = now_seconds(); unsigned phase = 0;
    status = 0;
    while (!stopped && now_seconds() - start < seconds) {
        if (phase < 2 && now_seconds() - start >= (phase ? 6.0 : 2.0)) {
            if (!phase) for (size_t i = 0; i < PATCH_W * PATCH_H; i++) {
                green[i * 3] = 0; green[i * 3 + 1] = 255; green[i * 3 + 2] = 0;
            }
            else green_pixels(green);
            size_t size = 0; kitty_graphics_send_stats_t stats;
            char *wire = patch_wire(id, green, &size, &stats);
            if (!wire) { status = 1; break; }
            if (acks) {
                for (char *q = wire; (q = strstr(q, "q=2")); q += 3) q[2] = '0';
            }
            bool sent = fwrite(wire, 1, size, stdout) == size;
            free(wire);
            fprintf(stdout, "\033[1;1H\033[2KKitty helper fixture: %s GREEN patch sent; expect RED border.\033[c", phase ? "MULTI-CHUNK textured" : "SINGLE-CHUNK solid");
            sent = fflush(stdout) == 0 && sent;
            fprintf(stderr, "PATCH phase=%u sent=%d chunks=%llu payload_bytes=%llu root=1 select=1 quiet_override=%d\n", phase + 1, sent,
                (unsigned long long)stats.chunks, (unsigned long long)stats.input_bytes, acks); fflush(stderr);
            phase++;
            if (!sent) { status = 1; break; }
        }
        struct pollfd wait = {.fd = STDIN_FILENO, .events = POLLIN};
        int ready = poll(&wait, 1, 100);
        if (ready < 0 && errno != EINTR) { status = 1; break; }
        if (ready > 0 && (wait.revents & POLLIN)) {
            unsigned char input[2048]; ssize_t got = read(STDIN_FILENO, input, sizeof(input));
            if (got > 0) {
                if (got == 1 && (input[0] == 'q' || input[0] == 'Q')) break;
                log_rx(input, (size_t)got);
            }
        }
    }
    fprintf(stderr, "END phases=%u status=%d\n", phase, status); fflush(stderr);
done:
    fprintf(stdout, "\033_Ga=d,d=I,i=%u,q=2;\033\\\033[17;1HFixture finished.\n", id); fflush(stdout);
    tcsetattr(STDIN_FILENO, TCSANOW, &saved);
    free(red); free(green);
    return status;
}
int main(int argc, char **argv) {
    if (argc == 1) return wire_case(false) || wire_case(true);
    if (strcmp(argv[1], "--live")) return 2;
    bool acks = false; unsigned seconds = 120; const char *log = NULL;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--acks")) acks = true;
        else if (!strcmp(argv[i], "--log") && i + 1 < argc) log = argv[++i];
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) {
            char *end; unsigned long value = strtoul(argv[++i], &end, 10);
            if (*end || value < 7 || value > 600) return 2;
            seconds = (unsigned)value;
        } else return 2;
    }
    return live_fixture(acks, seconds, log);
}
