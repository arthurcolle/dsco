#include "activation_lease.h"

#include <stdio.h>

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s LEASE_FILE\n", argv[0]);
        return 2;
    }
    activation_lease_t lease;
    char err[256] = {0};
    activation_lease_status_t status =
        activation_lease_load_file_verified(argv[1], &lease, err, sizeof(err));
    if (status != ACTIVATION_LEASE_OK) {
        fprintf(stderr, "activation lease smoke failed: %s\n", err[0] ? err : "rejected");
        return 1;
    }
    puts("activation lease signature: valid");
    return 0;
}
