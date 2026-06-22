#ifndef DSCO_IMG_UTIL_H
#define DSCO_IMG_UTIL_H

#include <stdbool.h>

/* Decode `in_path` (PNG/JPG/BMP/GIF/PSD/TGA/HDR/PIC via stb_image), downscale
 * so the longest side is <= max_dim (aspect preserved, never upscaled), and
 * write a quality-85 JPEG to `out_path`. Returns true on success.
 *
 * Formats stb_image cannot decode (HEIC/WEBP/AVIF/TIFF) return false, letting
 * the caller fall back to a platform tool (e.g. `sips` on macOS). This replaces
 * the previous unconditional `sips` shell-out so image attachments downscale
 * in-process and portably off macOS. */
bool dsco_image_downscale_jpeg(const char *in_path, int max_dim,
                               const char *out_path);

#endif /* DSCO_IMG_UTIL_H */
