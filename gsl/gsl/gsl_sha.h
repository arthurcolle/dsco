/* gsl_sha.h — SHA Hash Functions */
#ifndef __GSL_SHA_H__
#define __GSL_SHA_H__

#include <stdint.h>

typedef struct {
    uint32_t state[5];
    uint64_t count;
    uint8_t buffer[64];
} gsl_sha1_ctx;

void gsl_sha1_init(gsl_sha1_ctx *ctx);
void gsl_sha1_update(gsl_sha1_ctx *ctx, const uint8_t *data, size_t len);
void gsl_sha1_final(gsl_sha1_ctx *ctx, uint8_t digest[20]);

typedef struct {
    uint32_t state[8];
    uint64_t count;
    uint8_t buffer[64];
} gsl_sha256_ctx;

void gsl_sha256_init(gsl_sha256_ctx *ctx);
void gsl_sha256_update(gsl_sha256_ctx *ctx, const uint8_t *data, size_t len);
void gsl_sha256_final(gsl_sha256_ctx *ctx, uint8_t digest[32]);

#endif /* __GSL_SHA_H__ */