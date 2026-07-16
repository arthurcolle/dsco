#include "kitty_lab.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint64_t file_hash(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) return 0;
    uint64_t hash = 1469598103934665603ULL;
    unsigned char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), file)) > 0)
        for (size_t i = 0; i < n; i++) {
            hash ^= buf[i];
            hash *= 1099511628211ULL;
        }
    fclose(file);
    return hash;
}

int main(void) {
    const char *path = "/tmp/dsco-kitty-lab-test.ppm";
    const char *other = "/tmp/dsco-kitty-lab-test-other.ppm";
    const char *plan = "/tmp/dsco-kitty-lab-test-plan.ppm";
    const char *actions = "/tmp/dsco-kitty-lab-test-actions.ppm";
    if (!kitty_lab_write_ppm(path, 640, 360, 3, 12) ||
        !kitty_lab_write_ppm(other, 640, 360, 8, 12) ||
        !kitty_lab_write_ppm_view(plan, 640, 360, 3, 12, KITTY_LAB_VIEW_PLAN) ||
        !kitty_lab_write_ppm_view(actions, 640, 360, 3, 12, KITTY_LAB_VIEW_ACTIONS)) {
        fprintf(stderr, "kitty lab: failed to write PPM\n");
        return 1;
    }
    FILE *file = fopen(path, "rb");
    if (!file) return 1;
    char header[64] = {0};
    size_t n = fread(header, 1, sizeof(header) - 1, file);
    fclose(file);
    FILE *other_file = fopen(other, "rb");
    if (!other_file) return 1;
    if (fseek(other_file, 0, SEEK_END) != 0) return 1;
    long other_size = ftell(other_file);
    if (other_size < 16 || fseek(other_file, 15, SEEK_SET) != 0) return 1;
    unsigned char other_pixel = 0;
    if (fread(&other_pixel, 1, 1, other_file) != 1) return 1;
    fclose(other_file);
    uint64_t overview_hash = file_hash(path);
    uint64_t plan_hash = file_hash(plan);
    uint64_t actions_hash = file_hash(actions);
    remove(path);
    remove(other);
    remove(plan);
    remove(actions);
    if (n < 16 || strncmp(header, "P6\n640 360\n255\n", 15) != 0) {
        fprintf(stderr, "kitty lab: invalid PPM header\n");
        return 1;
    }
    if (other_size != 15L + 640L * 360L * 3L || other_pixel == 0) {
        fprintf(stderr, "kitty lab: invalid frame artifact\n");
        return 1;
    }
    if (!overview_hash || !plan_hash || !actions_hash ||
        overview_hash == plan_hash || overview_hash == actions_hash ||
        plan_hash == actions_hash) {
        fprintf(stderr, "kitty lab: planning views are not distinct\n");
        return 1;
    }
    printf("kitty lab: deterministic PPM passed\n");
    return 0;
}
