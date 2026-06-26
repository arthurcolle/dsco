#ifndef DSCO_HLC_H
#define DSCO_HLC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    int64_t pt_ms;
    uint32_t logical;
} hlc_t;

int64_t hlc_phys_now_ms(void);
hlc_t hlc_now(void);
hlc_t hlc_update(hlc_t remote, bool *did_clamp);
int hlc_compare(hlc_t a, hlc_t b);
void hlc_encode(hlc_t ts, char *out, size_t out_len);
bool hlc_decode(const char *s, hlc_t *out);
uint64_t hlc_pack(hlc_t ts);
hlc_t hlc_unpack(uint64_t packed);
hlc_t hlc_peek(void);
void hlc_reset(void);

#endif /* DSCO_HLC_H */
