#ifndef DSCO_OPENAI_IMAGES_H
#define DSCO_OPENAI_IMAGES_H

#include <stdbool.h>
#include <stddef.h>

#define DSCO_OPENAI_IMAGE_DEFAULT_MODEL "gpt-image-2"

bool tool_openai_image_generate(const char *input_json, char *result, size_t result_len);

#endif /* DSCO_OPENAI_IMAGES_H */
