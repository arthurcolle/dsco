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

void kitty_graphics_send_options_default(kitty_graphics_send_options_t *options);

/* Send a raw RGB/RGBA payload as one Kitty transmit command.  `control` is
 * emitted on the first APC chunk; continuation_control is emitted on later
 * chunks.  The payload itself is never interpreted, so f=24/f=32/f=100 and
 * animation controls remain caller-owned protocol policy. */
bool kitty_graphics_send_pixels(FILE *out, const char *control,
                                const void *pixels, size_t pixel_bytes,
                                const kitty_graphics_send_options_t *options);

/* A conservative, opt-in terminal hint.  This is not a protocol proof: a
 * caller that needs certainty should emit kitty_graphics_emit_query() and
 * parse the terminal response on its TTY. */
bool kitty_graphics_environment_hint(void);
bool kitty_graphics_available(FILE *out);

/* Emit the official graphics query followed by DA1.  The caller may read the
 * TTY response and match the Kitty APC acknowledgement against `image_id`. */
bool kitty_graphics_emit_query(FILE *out, uint32_t image_id);

size_t kitty_graphics_base64_encoded_length(size_t input_bytes);

#endif /* DSCO_KITTY_GRAPHICS_H */
