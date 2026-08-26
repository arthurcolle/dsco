#ifndef DSCO_KITTY_GRAPHICS_H
#define DSCO_KITTY_GRAPHICS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* Shared Kitty graphics transport.  Callers provide the complete first-chunk
 * control string (for example, "a=t,t=d,f=24,...").  The helper owns
 * compression, base64 encoding, and APC chunk framing so every native surface
 * follows the same wire contract. */

#define KITTY_GRAPHICS_DEFAULT_CHUNK_SIZE 4096U

typedef struct {
    size_t chunk_size;
    int compression_level; /* zlib level; < 0 uses Z_BEST_SPEED. */
    bool compress;
    const char *continuation_control; /* defaults to q=2 */
} kitty_graphics_send_options_t;

typedef struct {
    uint64_t input_bytes;
    uint64_t packed_bytes;
    uint64_t encoded_bytes;
    uint64_t wire_bytes;
    uint64_t chunks;
    size_t peak_heap_bytes;
    double compression_ms;
    double base64_ms;
    double write_ms;
    double total_ms;
} kitty_graphics_send_stats_t;

void kitty_graphics_send_options_default(kitty_graphics_send_options_t *options);

/* Send a raw RGB/RGBA payload as one Kitty transmit command.  `control` is
 * emitted on the first APC chunk; continuation_control is emitted on later
 * chunks.  The payload itself is never interpreted, so f=24/f=32/f=100 and
 * animation controls remain caller-owned protocol policy. */
bool kitty_graphics_send_pixels(FILE *out, const char *control,
                                const void *pixels, size_t pixel_bytes,
                                const kitty_graphics_send_options_t *options);

/* Instrumented form used by compositor telemetry. Passing stats adds local
 * timing calls but does not change framing or buffering behavior. */
bool kitty_graphics_send_pixels_ex(FILE *out, const char *control,
                                   const void *pixels, size_t pixel_bytes,
                                   const kitty_graphics_send_options_t *options,
                                   kitty_graphics_send_stats_t *stats);

/* Edit a rectangle in an existing Kitty animation frame. The payload is
 * always tightly packed RGB24. Re-select the frame after all of its dirty
 * rectangles have been sent to make the edits visible atomically. */
bool kitty_graphics_send_rgb_patch(FILE *out, uint32_t image_id,
                                   uint32_t frame,
                                   int x, int y, int width, int height,
                                   const void *pixels, size_t pixel_bytes,
                                   kitty_graphics_send_stats_t *stats);

/* Make one already-transmitted animation frame visible. */
bool kitty_graphics_select_frame(FILE *out, uint32_t image_id,
                                 uint32_t frame,
                                 kitty_graphics_send_stats_t *stats);

/* A conservative, opt-in terminal hint.  This is not a protocol proof: a
 * caller that needs certainty should emit kitty_graphics_emit_query() and
 * parse the terminal response on its TTY. */
bool kitty_graphics_environment_hint(void);

/* Active handshake on the tty behind `out`: sends the graphics query paired
 * with DA1 and reads the acknowledgement, so terminals absent from the env
 * allowlist (iTerm2 ≥ 3.6, konsole, …) are detected by proof rather than by
 * name. Result is cached per process. DSCO_KITTY_PROBE=0 disables. */
bool kitty_graphics_probe(FILE *out);

/* Env hint, or — when the hint is silent on an interactive tty — the active
 * probe. DSCO_KITTY_GRAPHICS=0 forces false, =1/force forces true. */
bool kitty_graphics_available(FILE *out);

/* True when the terminal is known not to implement the kitty graphics
 * protocol (iTerm2, Apple Terminal, plain xterm), so the active APC+DA1 probe
 * should be skipped — its DA1 half otherwise risks echoing "^[[?…c" to the
 * screen if the echo-suppression window races. DSCO_KITTY_PROBE=force
 * overrides. */
bool kitty_graphics_known_unsupported(void);

/* Emit the official graphics query followed by DA1.  The caller may read the
 * TTY response and match the Kitty APC acknowledgement against `image_id`. */
bool kitty_graphics_emit_query(FILE *out, uint32_t image_id);

size_t kitty_graphics_base64_encoded_length(size_t input_bytes);

#endif /* DSCO_KITTY_GRAPHICS_H */
