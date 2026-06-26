#include "hlc.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

int main(void) {
    hlc_reset();

    hlc_t a = hlc_now();
    hlc_t b = hlc_now();
    assert(hlc_compare(a, b) <= 0);

    char encoded[64];
    hlc_encode(b, encoded, sizeof(encoded));
    hlc_t decoded = {0, 0};
    assert(hlc_decode(encoded, &decoded));
    assert(hlc_compare(b, decoded) == 0);

    uint64_t packed = hlc_pack(b);
    hlc_t unpacked = hlc_unpack(packed);
    assert(hlc_compare(b, unpacked) == 0);

    bool did_clamp = false;
    hlc_t remote = {hlc_phys_now_ms() + 120000, 7};
    hlc_t updated = hlc_update(remote, &did_clamp);
    assert(did_clamp);
    assert(updated.pt_ms <= hlc_phys_now_ms() + 60000);

    hlc_reset();
    hlc_t zero = hlc_peek();
    assert(zero.pt_ms == 0);
    assert(zero.logical == 0);

    puts("test_hlc: PASS");
    return 0;
}
