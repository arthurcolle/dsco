/* Portable image decode/resize/encode backed by the stb single-file libraries
 * (public domain). Used to downscale attached/pasted images before base64 +
 * upload — replaces the macOS-only `sips` shell-out in agent.c. */

#include "img_util.h"

#include <stdlib.h>

/* The stb headers are warning-noisy under -Wall -Wextra; isolate them. */
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wunused-but-set-variable"
#pragma clang diagnostic ignored "-Wconversion"
#pragma clang diagnostic ignored "-Wsign-conversion"
#pragma clang diagnostic ignored "-Wcast-qual"
#pragma clang diagnostic ignored "-Wdouble-promotion"
#pragma clang diagnostic ignored "-Wcast-align"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wcast-qual"
#pragma GCC diagnostic ignored "-Wdouble-promotion"
#endif

#define STB_IMAGE_IMPLEMENTATION
#include "../vendor/stb_image.h"

#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "../vendor/stb_image_resize2.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../vendor/stb_image_write.h"

#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

bool dsco_image_downscale_jpeg(const char *in_path, int max_dim, const char *out_path) {
    if (!in_path || !out_path)
        return false;

    int w = 0, h = 0, comp = 0;
    unsigned char *pix = stbi_load(in_path, &w, &h, &comp, 0);
    if (!pix)
        return false; /* undecodable → caller falls back */
    if (w <= 0 || h <= 0 || (comp != 1 && comp != 3 && comp != 4)) {
        stbi_image_free(pix);
        return false;
    }

    /* Target dimensions: shrink the longest side to max_dim, never upscale. */
    int nw = w, nh = h;
    int longest = w > h ? w : h;
    if (max_dim > 0 && longest > max_dim) {
        double s = (double)max_dim / (double)longest;
        nw = (int)((double)w * s);
        if (nw < 1)
            nw = 1;
        nh = (int)((double)h * s);
        if (nh < 1)
            nh = 1;
    }

    unsigned char *resized = pix;
    bool resized_owned = false;
    if (nw != w || nh != h) {
        resized = (unsigned char *)malloc((size_t)nw * (size_t)nh * (size_t)comp);
        if (!resized) {
            stbi_image_free(pix);
            return false;
        }
        stbir_pixel_layout layout = (comp == 1)   ? STBIR_1CHANNEL
                                    : (comp == 3) ? STBIR_RGB
                                                  : STBIR_RGBA;
        unsigned char *r = stbir_resize_uint8_srgb(pix, w, h, 0, resized, nw, nh, 0, layout);
        if (!r) {
            free(resized);
            stbi_image_free(pix);
            return false;
        }
        resized_owned = true;
    }

    /* JPEG has no alpha channel — drop it to RGB if present. */
    int jcomp = comp;
    unsigned char *jpix = resized;
    unsigned char *jpix_owned = NULL;
    if (comp == 4) {
        jcomp = 3;
        jpix_owned = (unsigned char *)malloc((size_t)nw * (size_t)nh * 3);
        if (!jpix_owned) {
            if (resized_owned)
                free(resized);
            stbi_image_free(pix);
            return false;
        }
        long n = (long)nw * (long)nh;
        for (long i = 0; i < n; i++) {
            jpix_owned[i * 3 + 0] = resized[i * 4 + 0];
            jpix_owned[i * 3 + 1] = resized[i * 4 + 1];
            jpix_owned[i * 3 + 2] = resized[i * 4 + 2];
        }
        jpix = jpix_owned;
    }

    int ok = stbi_write_jpg(out_path, nw, nh, jcomp, jpix, 85);

    if (jpix_owned)
        free(jpix_owned);
    if (resized_owned)
        free(resized);
    stbi_image_free(pix);
    return ok != 0;
}
