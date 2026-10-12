#ifndef DSCO_COMPOSITOR_STREAM_BENCH_H
#define DSCO_COMPOSITOR_STREAM_BENCH_H

#include "pixel_tui_perf.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

typedef struct {
    int chunks;
    int interval_us;
    const char *model;
} compositor_stream_bench_config_t;

void compositor_stream_bench_config_default(compositor_stream_bench_config_t *config);

/* Run a deterministic provider-like text stream through the live native
 * compositor. This intentionally requires a Kitty-compatible TTY. */
int compositor_stream_bench_run(FILE *summary,
                                const compositor_stream_bench_config_t *config);

/* Separate JSON seam keeps benchmark reporting testable without a TTY. */
bool compositor_stream_bench_write_json(
    FILE *out, const compositor_stream_bench_config_t *config,
    size_t source_bytes, double elapsed_ms,
    const pixel_tui_perf_snapshot_t *perf);

#endif /* DSCO_COMPOSITOR_STREAM_BENCH_H */
