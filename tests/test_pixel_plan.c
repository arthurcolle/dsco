#include "pixel_tui.h"
#include "plan.h"
#include "vm.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int g_cheap_mode = 0;
vm_t g_vm;
volatile int g_interrupted = 0;
double g_cost_budget = 0.0;

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

static int ppm_size(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) return 0;
    char header[64] = {0};
    size_t n = fread(header, 1, sizeof(header) - 1, file);
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return 0;
    }
    long size = ftell(file);
    fclose(file);
    if (n < 15 || strncmp(header, "P6\n640 ", 7) != 0 ||
        !strstr(header, "\n255\n") || size < 0)
        return 0;
    return (int)size;
}

int main(void) {
    const char *tree = "/tmp/dsco-pixel-plan-tree.ppm";
    const char *actions = "/tmp/dsco-pixel-plan-actions.ppm";
    plan_engine_init();
    int plan_id = plan_create("Native release", "Ship a safe native planning surface",
                              PLAN_MODE_HYBRID);
    if (plan_id < 0) return 1;
    int discover = plan_add_step(plan_id, 0, "Discover", STEP_COMPOSITE);
    int build = plan_add_step(plan_id, 0, "Build", STEP_COMPOSITE);
    int verify = plan_add_step(plan_id, 0, "Verify", STEP_ATOMIC);
    if (discover < 0 || build < 0 || verify < 0) return 1;
    step_add_dep(build, discover);
    step_add_dep(verify, build);
    int a1 = step_add_atom(discover, "inspect repository", ATOM_TOOL_CALL);
    int a2 = step_add_atom(build, "compose native view", ATOM_TOOL_CALL);
    int a3 = step_add_atom(verify, "run visual smoke", ATOM_ASSERT);
    if (a1 < 0 || a2 < 0 || a3 < 0) return 1;
    atom_wire(a1, a2, NULL);
    atom_wire(a2, a3, NULL);
    step_set_status(discover, PLAN_DONE);
    step_set_status(build, PLAN_IN_PROGRESS);
    atom_set_result(a1, "{\"ok\":true}");
    plan_rollup_status(plan_id);

    bool tree_ok = pixel_tui_write_plan_view_ppm(tree, plan_id, 640,
                                                  PIXEL_PLAN_VIEW_TREE);
    bool action_ok = pixel_tui_write_plan_view_ppm(actions, plan_id, 640,
                                                    PIXEL_PLAN_VIEW_ACTIONS);
    int tree_bytes = ppm_size(tree);
    int action_bytes = ppm_size(actions);
    uint64_t tree_hash = file_hash(tree);
    uint64_t action_hash = file_hash(actions);
    if (!getenv("DSCO_KEEP_PLAN_ARTIFACT")) {
        remove(tree);
        remove(actions);
    }
    if (!tree_ok || !action_ok || tree_bytes <= 15 || action_bytes <= 15 ||
        !tree_hash || !action_hash || tree_hash == action_hash) {
        fprintf(stderr, "pixel plan: failed distinct tree/action artifacts\n");
        return 1;
    }
    printf("pixel plan: tree/action views passed\n");
    return 0;
}
