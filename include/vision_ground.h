#ifndef DSCO_VISION_GROUND_H
#define DSCO_VISION_GROUND_H

#include <stdbool.h>
#include <stddef.h>

/* TypeSafe System One Box grounding: "where is <target> in this image?"
 * Edges are fractions of the image size measured from its top-left corner. */
typedef struct {
    double left, top, right, bottom;
    double confidence;
} vision_box_t;

#define VISION_GROUND_DEFAULT_MODEL "jev-1.14.0"
#define VISION_GROUND_ENDPOINT "https://api.typesafe.ai/v1/systemone"
#define VISION_GROUND_TARGET_MAX 512
#define VISION_GROUND_IMAGE_MAX (16u * 1024u * 1024u) /* API per-image limit */

/* True when a key is configured and DSCO_VISION_GROUND is not "0". */
bool vision_ground_available(void);

/* Ask one Box question about a PNG/JPEG file. The caller's tool call must
 * already have been admitted by the governance gate with CAP_NET declared.
 * detail receives a short machine-readable failure reason. */
bool vision_ground_box(const char *image_path, const char *target,
                       vision_box_t *out, char *detail, size_t detail_len);

/* Pure helpers, exposed for offline tests. */
bool vision_ground_build_body(const unsigned char *image, size_t image_len,
                              const char *format, const char *target,
                              const char *model, const char *ref,
                              char **body_out, size_t *body_len);
bool vision_ground_parse(const char *response, size_t len, vision_box_t *out,
                         char *detail, size_t detail_len);

#endif
