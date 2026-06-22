#ifndef DSCO_PLOT_H
#define DSCO_PLOT_H

#include <stddef.h>
#include <stdbool.h>

/* ── Unicode plotting engine ──────────────────────────────────────────────
 * Renders data series into Unicode-art charts as strings, so the same call
 * works inline (print it), as a tool result (return it), or as a stored
 * display artifact. Built on the subpixel/Braille primitives in tui.h:
 * Braille (2×4 dots/cell) for line/scatter/area, eighth blocks for bars and
 * gauges, sextant/256-color cells for heatmaps.
 *
 * Every renderer writes a NUL-terminated string into `out` (truncated to
 * `cap`) and returns the number of bytes that would have been written
 * (snprintf semantics), or -1 on error. */

typedef struct {
    const char *title;       /* optional heading line */
    int   width;             /* chart width in cells (0 = auto, ~60) */
    int   height;            /* chart height in cell-rows (0 = auto) */
    bool  color;             /* emit ANSI color */
    bool  axes;              /* draw y-range gutter + frame */
    const char *color_seq;   /* ANSI series color (NULL = cyan) */
    const char *x_label;     /* optional x-axis caption */
    const char *y_label;     /* optional y-axis caption */
} plot_opts_t;

/* Sensible defaults: width 60, height 8, color+axes on, cyan series. */
plot_opts_t plot_opts_default(void);

int plot_line(char *out, size_t cap, const double *y, int n, const plot_opts_t *o);
int plot_line_xy(char *out, size_t cap, const double *x, const double *y, int n,
                 const plot_opts_t *o);
int plot_scatter(char *out, size_t cap, const double *x, const double *y, int n,
                 const plot_opts_t *o);
int plot_area(char *out, size_t cap, const double *y, int n, const plot_opts_t *o);
int plot_bar(char *out, size_t cap, const char *const *labels, const double *v,
             int n, const plot_opts_t *o);
int plot_column(char *out, size_t cap, const char *const *labels, const double *v,
                int n, const plot_opts_t *o);
int plot_hist(char *out, size_t cap, const double *samples, int n, int bins,
              const plot_opts_t *o);
int plot_heatmap(char *out, size_t cap, const double *grid, int rows, int cols,
                 const plot_opts_t *o);
int plot_boxplot(char *out, size_t cap, const double *samples, int n,
                 const plot_opts_t *o);
int plot_candlestick(char *out, size_t cap, const double *open, const double *high,
                     const double *low, const double *close, int n,
                     const plot_opts_t *o);
int plot_gauge(char *out, size_t cap, double value, double lo, double hi,
               const plot_opts_t *o);
int plot_sparkline(char *out, size_t cap, const double *y, int n,
                   const plot_opts_t *o);

/* ── extended display types ───────────────────────────────────────────── */

/* 100% proportion bar split into colored segments + a labeled legend. */
int plot_pie(char *out, size_t cap, const char *const *labels, const double *v,
             int n, const plot_opts_t *o);
/* Cumulative waterfall: each delta floats from the running total; greens up,
 * reds down, with a final total bar. */
int plot_waterfall(char *out, size_t cap, const char *const *labels,
                   const double *deltas, int n, const plot_opts_t *o);
/* Bullet KPI: a measure bar against `target` over qualitative `ranges`
 * (ascending thresholds; nr may be 0). */
int plot_bullet(char *out, size_t cap, const char *label, double value,
                double target, const double *ranges, int nr,
                const plot_opts_t *o);
/* Lollipop / dot plot: a stem to a marker per category. */
int plot_lollipop(char *out, size_t cap, const char *const *labels,
                  const double *v, int n, const plot_opts_t *o);
/* Slope chart: before→after with direction and color per row. */
int plot_slope(char *out, size_t cap, const char *const *labels,
               const double *left, const double *right, int n,
               const plot_opts_t *o);
/* Empirical CDF as a Braille step line over the sorted samples. */
int plot_ecdf(char *out, size_t cap, const double *samples, int n,
              const plot_opts_t *o);
/* GitHub-style calendar heatmap: 7 day-rows × ceil(n/7) week-columns. */
int plot_calendar(char *out, size_t cap, const double *v, int n,
                  const plot_opts_t *o);
/* Ridgeline: one Braille sparkline row per series, stacked with names. */
int plot_ridgeline(char *out, size_t cap, const char *const *names,
                   const double *const *series, const int *lens, int n_series,
                   const plot_opts_t *o);
/* Violin: vertical bins drawn as horizontal mirrored density bands. */
int plot_violin(char *out, size_t cap, const double *samples, int n,
                const plot_opts_t *o);
/* Big-number KPI: render a value in 5-row block digits with a caption. */
int plot_bignum(char *out, size_t cap, double value, const char *caption,
                const plot_opts_t *o);
/* Strange attractor: iterate a 2-D chaotic map (`kind` = "dejong" or
 * "clifford") for `iters` steps and render its orbit in phase space. Uses the
 * full Braille subpixel grid (2x4 dots/cell) for the orbit's fine structure and
 * tints each cell on the viridis ramp by log visit-density. */
int plot_attractor(char *out, size_t cap, const char *kind,
                   double a, double b, double c, double d, long iters,
                   const plot_opts_t *o);
/* Escape-time fractal (`set` = "mandelbrot" or "julia") centered at (cx,cy)
 * with half-view-width `scale`; for julia the constant is (jx,jy). Rendered
 * with truecolor upper-half-block glyphs (two 24-bit pixels per cell, 2×
 * vertical resolution) and continuous smooth-iteration coloring. Falls back to
 * a luminance ASCII ramp when color is off. */
int plot_fractal(char *out, size_t cap, const char *set,
                 double cx, double cy, double scale,
                 double jx, double jy, int iters, const plot_opts_t *o);

/* Parse a JSON request and dispatch to the matching renderer. Schema:
 *   {"type":"line|scatter|area|bar|column|hist|heatmap|box|candlestick|
 *            gauge|sparkline",
 *    "data":[...], "x":[...], "labels":["a","b"],
 *    "open":[],"high":[],"low":[],"close":[],
 *    "title":"...", "width":N,"height":N,"bins":N,"rows":N,"cols":N,
 *    "min":F,"max":F,"value":F,"color":true,"axes":true}
 * Returns bytes written (snprintf semantics), or -1 on bad input. */
int plot_dispatch(const char *input_json, char *out, size_t cap);

#endif /* DSCO_PLOT_H */
