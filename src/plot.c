/* ── Unicode plotting engine ──────────────────────────────────────────────
 * String-rendering charts built on the subpixel/Braille primitives in tui.h.
 * See include/plot.h for the public surface. */

#include "plot.h"
#include "tui.h"
#include "json_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>

/* ── color palette (ANSI) ─────────────────────────────────────────────── */
#define P_RESET "\033[0m"
#define P_DIM   "\033[2m"
#define P_CYAN  "\033[36m"
#define P_BCYAN "\033[96m"
#define P_GREEN "\033[92m"
#define P_RED   "\033[91m"
#define P_YEL   "\033[93m"
#define P_BLUE  "\033[94m"
#define P_MAG   "\033[95m"
#define P_GREY  "\033[38;5;244m"

/* ── bounded string buffer ────────────────────────────────────────────── */
typedef struct { char *base; char *p; char *end; bool ovf; } pbuf_t;

static void pb_init(pbuf_t *b, char *out, size_t cap) {
    b->base = out; b->p = out;
    b->end = cap ? out + cap - 1 : out;
    b->ovf = false;
    if (cap) out[0] = '\0';
}
static void pb_puts(pbuf_t *b, const char *s) {
    if (!s) return;
    while (*s) {
        if (b->p >= b->end) { b->ovf = true; if (b->base != b->end) *b->p = '\0'; return; }
        *b->p++ = *s++;
    }
    *b->p = '\0';
}
static void pb_putf(pbuf_t *b, const char *fmt, ...) {
    char tmp[1024];
    va_list a; va_start(a, fmt);
    vsnprintf(tmp, sizeof tmp, fmt, a);
    va_end(a);
    pb_puts(b, tmp);
}
static int pb_done(pbuf_t *b) { return (int)(b->p - b->base); }

/* Conditional color: only emit when opts.color is on. */
static void pb_col(pbuf_t *b, bool on, const char *seq) { if (on && seq) pb_puts(b, seq); }
static void pb_reset(pbuf_t *b, bool on) { if (on) pb_puts(b, P_RESET); }

/* ── eighth-block horizontal bar (subpixel, 1/8-cell edge) ────────────── */
static const char *const EIGHTHS_L[8] = { "", "▏","▎","▍","▌","▋","▊","▉" };
static const char *const EIGHTHS_V[9] = { " ","▁","▂","▃","▄","▅","▆","▇","█" };

static void pb_hbar(pbuf_t *b, double frac, int cells,
                    bool color, const char *fc) {
    if (frac < 0) frac = 0; if (frac > 1) frac = 1;
    double ex = frac * cells;
    int full = (int)ex;
    int e = (int)((ex - full) * 8.0 + 0.5);
    if (e >= 8) { full++; e = 0; }
    if (full > cells) { full = cells; e = 0; }
    pb_col(b, color, fc ? fc : P_CYAN);
    int used = 0;
    for (; used < full; used++) pb_puts(b, "█");
    if (used < cells && e > 0) { pb_puts(b, EIGHTHS_L[e]); used++; }
    pb_col(b, color, P_DIM);
    for (; used < cells; used++) pb_puts(b, "░");
    pb_reset(b, color);
}

/* ── render a Braille canvas into the buffer (with optional y gutter) ──── */
static void pb_braille(pbuf_t *b, tui_braille_t *bc, bool color,
                       const char *col, bool axes, double ymin, double ymax) {
    char *mb = NULL; size_t ms = 0;
    FILE *f = open_memstream(&mb, &ms);
    if (!f) return;
    tui_braille_render(bc, f, NULL);   /* no color: we wrap rows ourselves */
    fclose(f);
    /* mb is rows joined by '\n'; reframe with a y-axis gutter. */
    int row = 0, rows = bc->h_cells;
    char *line = mb, *nl;
    while (line && *line) {
        nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        if (axes) {
            double yv = ymax - (ymax - ymin) * row / (rows > 1 ? rows - 1 : 1);
            pb_col(b, color, P_GREY);
            pb_putf(b, "%8.3g │", yv);   /* right-aligned label + │ */
            pb_reset(b, color);
        }
        pb_col(b, color, col ? col : P_BCYAN);
        pb_puts(b, line);
        pb_reset(b, color);
        pb_puts(b, "\n");
        row++;
        if (!nl) break;
        line = nl + 1;
    }
    free(mb);
}

static void pb_title(pbuf_t *b, const plot_opts_t *o) {
    if (o->title && o->title[0]) {
        pb_col(b, o->color, "\033[1m");
        pb_puts(b, o->title);
        pb_reset(b, o->color);
        pb_puts(b, "\n");
    }
}

static void minmax(const double *v, int n, double *mn, double *mx) {
    *mn = *mx = v[0];
    for (int i = 1; i < n; i++) { if (v[i] < *mn) *mn = v[i]; if (v[i] > *mx) *mx = v[i]; }
}

plot_opts_t plot_opts_default(void) {
    plot_opts_t o = {0};
    o.width = 60; o.height = 8; o.color = true; o.axes = true; o.color_seq = P_BCYAN;
    return o;
}

static plot_opts_t norm(const plot_opts_t *o) {
    plot_opts_t r = o ? *o : plot_opts_default();
    if (r.width  <= 0) r.width  = 60;
    if (r.height <= 0) r.height = 8;
    if (r.width  > 200) r.width = 200;
    if (r.height > 40) r.height = 40;
    return r;
}

/* ── line / scatter / area (Braille) ──────────────────────────────────── */

int plot_line_xy(char *out, size_t cap, const double *x, const double *y, int n,
                 const plot_opts_t *opt) {
    if (!out || !y || n <= 0) { if (out && cap) out[0] = '\0'; return -1; }
    plot_opts_t o = norm(opt);
    pbuf_t b; pb_init(&b, out, cap);
    pb_title(&b, &o);

    double ymn, ymx; minmax(y, n, &ymn, &ymx);
    if (ymx - ymn < 1e-12) { ymx += 1; ymn -= 1; }
    double xmn = 0, xmx = n - 1;
    if (x) minmax(x, n, &xmn, &xmx);
    if (xmx - xmn < 1e-12) xmx = xmn + 1;

    tui_braille_t bc;
    tui_braille_init(&bc, o.width * 2, o.height * 4);
    int prev_px = 0, prev_py = 0;
    for (int i = 0; i < n; i++) {
        double xf = x ? (x[i] - xmn) / (xmx - xmn) : (n == 1 ? 0 : (double)i / (n - 1));
        int px = (int)(xf * (bc.px_w - 1) + 0.5);
        int py = (int)((1.0 - (y[i] - ymn) / (ymx - ymn)) * (bc.px_h - 1) + 0.5);
        if (i == 0) tui_braille_set(&bc, px, py);
        else        tui_braille_line(&bc, prev_px, prev_py, px, py);
        prev_px = px; prev_py = py;
    }
    pb_braille(&b, &bc, o.color, o.color_seq, o.axes, ymn, ymx);
    tui_braille_free(&bc);
    if (o.axes && x) {
        pb_col(&b, o.color, P_GREY);
        pb_putf(&b, "%9s └", "");
        for (int i = 0; i < o.width; i++) pb_puts(&b, "─");
        pb_putf(&b, "\n%9s %-*.4g%*.4g\n", "", o.width/2, xmn, o.width/2, xmx);
        pb_reset(&b, o.color);
    }
    return b.ovf ? -1 : pb_done(&b);
}

int plot_line(char *out, size_t cap, const double *y, int n, const plot_opts_t *o) {
    return plot_line_xy(out, cap, NULL, y, n, o);
}

int plot_scatter(char *out, size_t cap, const double *x, const double *y, int n,
                 const plot_opts_t *opt) {
    if (!out || !x || !y || n <= 0) { if (out && cap) out[0] = '\0'; return -1; }
    plot_opts_t o = norm(opt);
    pbuf_t b; pb_init(&b, out, cap);
    pb_title(&b, &o);
    double xmn, xmx, ymn, ymx;
    minmax(x, n, &xmn, &xmx); minmax(y, n, &ymn, &ymx);
    if (xmx - xmn < 1e-12) xmx = xmn + 1;
    if (ymx - ymn < 1e-12) { ymx += 1; ymn -= 1; }
    tui_braille_t bc;
    tui_braille_init(&bc, o.width * 2, o.height * 4);
    for (int i = 0; i < n; i++) {
        int px = (int)((x[i] - xmn) / (xmx - xmn) * (bc.px_w - 1) + 0.5);
        int py = (int)((1.0 - (y[i] - ymn) / (ymx - ymn)) * (bc.px_h - 1) + 0.5);
        tui_braille_set(&bc, px, py);
    }
    pb_braille(&b, &bc, o.color, o.color_seq, o.axes, ymn, ymx);
    tui_braille_free(&bc);
    return b.ovf ? -1 : pb_done(&b);
}

int plot_area(char *out, size_t cap, const double *y, int n, const plot_opts_t *opt) {
    if (!out || !y || n <= 0) { if (out && cap) out[0] = '\0'; return -1; }
    plot_opts_t o = norm(opt);
    pbuf_t b; pb_init(&b, out, cap);
    pb_title(&b, &o);
    double ymn, ymx; minmax(y, n, &ymn, &ymx);
    if (ymn > 0) ymn = 0;                       /* area is referenced to 0 */
    if (ymx - ymn < 1e-12) ymx = ymn + 1;
    tui_braille_t bc;
    tui_braille_init(&bc, o.width * 2, o.height * 4);
    for (int col = 0; col < bc.px_w; col++) {
        double xf = bc.px_w > 1 ? (double)col / (bc.px_w - 1) : 0;
        double fi = xf * (n - 1);
        int i0 = (int)fi; int i1 = i0 + 1 < n ? i0 + 1 : i0;
        double t = fi - i0;
        double val = y[i0] * (1 - t) + y[i1] * t;
        int top = (int)((1.0 - (val - ymn) / (ymx - ymn)) * (bc.px_h - 1) + 0.5);
        for (int py = top; py < bc.px_h; py++) tui_braille_set(&bc, col, py);
    }
    pb_braille(&b, &bc, o.color, o.color_seq, o.axes, ymn, ymx);
    tui_braille_free(&bc);
    return b.ovf ? -1 : pb_done(&b);
}

/* ── horizontal bar chart (subpixel) ──────────────────────────────────── */
int plot_bar(char *out, size_t cap, const char *const *labels, const double *v,
             int n, const plot_opts_t *opt) {
    if (!out || !v || n <= 0) { if (out && cap) out[0] = '\0'; return -1; }
    plot_opts_t o = norm(opt);
    pbuf_t b; pb_init(&b, out, cap);
    pb_title(&b, &o);
    double mn, mx; minmax(v, n, &mn, &mx);
    double base = mn < 0 ? mn : 0;
    double span = mx - base; if (span < 1e-12) span = 1;
    int lblw = 0;
    for (int i = 0; i < n; i++) {
        int l = labels && labels[i] ? (int)strlen(labels[i]) : 0;
        if (l > lblw) lblw = l;
    }
    if (lblw > 18) lblw = 18;
    int barw = o.width - lblw - 12; if (barw < 8) barw = 8;
    const char *palette[] = { P_BCYAN, P_GREEN, P_YEL, P_MAG, P_BLUE, P_RED };
    for (int i = 0; i < n; i++) {
        pb_col(&b, o.color, P_GREY);
        pb_putf(&b, "%*.*s ", lblw, lblw, labels && labels[i] ? labels[i] : "");
        pb_reset(&b, o.color);
        pb_hbar(&b, (v[i] - base) / span, barw, o.color, palette[i % 6]);
        pb_col(&b, o.color, P_DIM);
        pb_putf(&b, " %.4g", v[i]);
        pb_reset(&b, o.color);
        pb_puts(&b, "\n");
    }
    return b.ovf ? -1 : pb_done(&b);
}

/* ── vertical column chart (eighth-block) ─────────────────────────────── */
static int plot_columns_impl(pbuf_t *b, const char *const *labels,
                             const double *v, int n, const plot_opts_t *o) {
    double mn, mx; minmax(v, n, &mn, &mx);
    double base = mn < 0 ? mn : 0;
    double span = mx - base; if (span < 1e-12) span = 1;
    int rows = o->height;
    int colw = 2;                              /* each column 2 cells wide */
    /* grid[r][i] glyph */
    for (int r = 0; r < rows; r++) {
        for (int i = 0; i < n; i++) {
            double frac = (v[i] - base) / span;
            double fill_rows = frac * rows;     /* how many rows tall */
            double this_row_from_bottom = rows - 1 - r;
            double cell_fill = fill_rows - this_row_from_bottom; /* 0..1 within cell */
            const char *g;
            if (cell_fill >= 1.0) g = "█";
            else if (cell_fill <= 0.0) g = " ";
            else g = EIGHTHS_V[(int)(cell_fill * 8.0 + 0.5)];
            pb_col(b, o->color, P_BCYAN);
            for (int c = 0; c < colw; c++) pb_puts(b, g);
            pb_reset(b, o->color);
            pb_puts(b, " ");
        }
        pb_puts(b, "\n");
    }
    /* axis + labels */
    pb_col(b, o->color, P_GREY);
    for (int i = 0; i < n; i++) { pb_puts(b, "──"); pb_puts(b, " "); }
    pb_puts(b, "\n");
    if (labels) {
        for (int i = 0; i < n; i++)
            pb_putf(b, "%-2.2s ", labels[i] ? labels[i] : "");
        pb_puts(b, "\n");
    }
    pb_reset(b, o->color);
    return 0;
}

int plot_column(char *out, size_t cap, const char *const *labels, const double *v,
                int n, const plot_opts_t *opt) {
    if (!out || !v || n <= 0) { if (out && cap) out[0] = '\0'; return -1; }
    plot_opts_t o = norm(opt);
    pbuf_t b; pb_init(&b, out, cap);
    pb_title(&b, &o);
    plot_columns_impl(&b, labels, v, n, &o);
    return b.ovf ? -1 : pb_done(&b);
}

/* ── histogram ────────────────────────────────────────────────────────── */
int plot_hist(char *out, size_t cap, const double *samples, int n, int bins,
              const plot_opts_t *opt) {
    if (!out || !samples || n <= 0) { if (out && cap) out[0] = '\0'; return -1; }
    plot_opts_t o = norm(opt);
    if (bins <= 0) bins = (int)(sqrt((double)n) + 0.5);
    if (bins < 1) bins = 1; if (bins > 64) bins = 64;
    double mn, mx; minmax(samples, n, &mn, &mx);
    double span = mx - mn; if (span < 1e-12) span = 1;
    double counts[64] = {0};
    char   labels[64][8];
    const char *labptr[64];
    for (int i = 0; i < n; i++) {
        int bi = (int)((samples[i] - mn) / span * bins);
        if (bi >= bins) bi = bins - 1; if (bi < 0) bi = 0;
        counts[bi] += 1;
    }
    for (int i = 0; i < bins; i++) {
        snprintf(labels[i], sizeof labels[i], "%.2g", mn + span * (i + 0.5) / bins);
        labptr[i] = labels[i];
    }
    pbuf_t b; pb_init(&b, out, cap);
    pb_title(&b, &o);
    plot_bar(b.p, (size_t)(b.end - b.p), labptr, counts, bins, &(plot_opts_t){
        .width = o.width, .color = o.color, .axes = false });
    /* advance cursor past what plot_bar wrote */
    b.p += strlen(b.p);
    return b.ovf ? -1 : pb_done(&b);
}

/* ── heatmap (256-color cells, viridis-ish ramp) ──────────────────────── */
static int viridis256(double t) {
    /* crude blue→green→yellow ramp over xterm-256 */
    if (t < 0) t = 0; if (t > 1) t = 1;
    static const int ramp[] = { 17,18,19,20,26,32,38,44,49,48,47,46,82,118,154,190,226,220 };
    int k = (int)(t * (int)(sizeof(ramp)/sizeof(ramp[0]) - 1) + 0.5);
    return ramp[k];
}

int plot_heatmap(char *out, size_t cap, const double *grid, int rows, int cols,
                 const plot_opts_t *opt) {
    if (!out || !grid || rows <= 0 || cols <= 0) { if (out && cap) out[0] = '\0'; return -1; }
    plot_opts_t o = norm(opt);
    pbuf_t b; pb_init(&b, out, cap);
    pb_title(&b, &o);
    double mn, mx; minmax(grid, rows * cols, &mn, &mx);
    double span = mx - mn; if (span < 1e-12) span = 1;
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            double t = (grid[r * cols + c] - mn) / span;
            if (o.color) {
                pb_putf(&b, "\033[48;5;%dm  ", viridis256(t));
            } else {
                const char *sh = t < .2 ? " " : t < .4 ? "░" : t < .6 ? "▒" : t < .8 ? "▓" : "█";
                pb_puts(&b, sh); pb_puts(&b, sh);
            }
        }
        pb_reset(&b, o.color);
        pb_puts(&b, "\n");
    }
    if (o.color) {
        pb_col(&b, true, P_GREY);
        pb_putf(&b, "%.3g ", mn);
        for (int i = 0; i < 16; i++) pb_putf(&b, "\033[48;5;%dm ", viridis256(i / 15.0));
        pb_putf(&b, "%s %.3g\n", P_RESET, mx);
    }
    return b.ovf ? -1 : pb_done(&b);
}

/* ── box-and-whisker ──────────────────────────────────────────────────── */
static int dcmp(const void *a, const void *c) {
    double x = *(const double *)a, y = *(const double *)c;
    return x < y ? -1 : x > y ? 1 : 0;
}
static double quantile(const double *sorted, int n, double q) {
    if (n == 1) return sorted[0];
    double pos = q * (n - 1);
    int i = (int)pos; double f = pos - i;
    return i + 1 < n ? sorted[i] * (1 - f) + sorted[i + 1] * f : sorted[i];
}

int plot_boxplot(char *out, size_t cap, const double *samples, int n,
                 const plot_opts_t *opt) {
    if (!out || !samples || n <= 0) { if (out && cap) out[0] = '\0'; return -1; }
    plot_opts_t o = norm(opt);
    pbuf_t b; pb_init(&b, out, cap);
    pb_title(&b, &o);
    double *s = malloc((size_t)n * sizeof(double));
    if (!s) return -1;
    memcpy(s, samples, (size_t)n * sizeof(double));
    qsort(s, n, sizeof(double), dcmp);
    double mn = s[0], mx = s[n - 1];
    double q1 = quantile(s, n, 0.25), med = quantile(s, n, 0.5), q3 = quantile(s, n, 0.75);
    free(s);
    double span = mx - mn; if (span < 1e-12) span = 1;
    int W = o.width;
    int p_mn = 0, p_q1 = (int)((q1 - mn)/span*(W-1)), p_md = (int)((med-mn)/span*(W-1));
    int p_q3 = (int)((q3 - mn)/span*(W-1)), p_mx = W - 1;
    pb_col(&b, o.color, o.color_seq ? o.color_seq : P_BCYAN);
    for (int i = 0; i < W; i++) {
        if (i == p_md) pb_puts(&b, "┃");
        else if (i == p_q1 || i == p_q3) pb_puts(&b, "│");
        else if (i > p_q1 && i < p_q3) pb_puts(&b, "▓");
        else if (i == p_mn || i == p_mx) pb_puts(&b, "├");
        else if ((i > p_mn && i < p_q1) || (i > p_q3 && i < p_mx)) pb_puts(&b, "─");
        else pb_puts(&b, " ");
    }
    pb_reset(&b, o.color);
    pb_puts(&b, "\n");
    pb_col(&b, o.color, P_GREY);
    pb_putf(&b, "min %.3g  Q1 %.3g  med %.3g  Q3 %.3g  max %.3g\n", mn, q1, med, q3, mx);
    pb_reset(&b, o.color);
    return b.ovf ? -1 : pb_done(&b);
}

/* ── candlestick (OHLC) ───────────────────────────────────────────────── */
int plot_candlestick(char *out, size_t cap, const double *op, const double *hi,
                     const double *lo, const double *cl, int n,
                     const plot_opts_t *opt) {
    if (!out || !op || !hi || !lo || !cl || n <= 0) { if (out && cap) out[0] = '\0'; return -1; }
    plot_opts_t o = norm(opt);
    pbuf_t b; pb_init(&b, out, cap);
    pb_title(&b, &o);
    double mn = lo[0], mx = hi[0];
    for (int i = 1; i < n; i++) { if (lo[i] < mn) mn = lo[i]; if (hi[i] > mx) mx = hi[i]; }
    double span = mx - mn; if (span < 1e-12) span = 1;
    int rows = o.height;
    /* For each cell-row (top→bottom) and candle, decide wick/body glyph. */
    for (int r = 0; r < rows; r++) {
        double hy = mx - span * r / (rows > 1 ? rows - 1 : 1);        /* row's price (top) */
        double ly = mx - span * (r + 1) / (rows > 1 ? rows - 1 : 1);  /* row's price (bottom) */
        for (int i = 0; i < n; i++) {
            bool up = cl[i] >= op[i];
            double bodyhi = up ? cl[i] : op[i];
            double bodylo = up ? op[i] : cl[i];
            const char *g = " ";
            if (hi[i] >= ly && lo[i] <= hy) {                 /* wick present */
                if (bodyhi >= ly && bodylo <= hy) g = "█";    /* body */
                else g = "│";                                  /* wick only */
            }
            pb_col(&b, o.color, up ? P_GREEN : P_RED);
            pb_puts(&b, g);
            pb_reset(&b, o.color);
        }
        if (o.axes) {
            pb_col(&b, o.color, P_GREY);
            pb_putf(&b, " %.4g", hy);
            pb_reset(&b, o.color);
        }
        pb_puts(&b, "\n");
    }
    return b.ovf ? -1 : pb_done(&b);
}

/* ── gauge ────────────────────────────────────────────────────────────── */
int plot_gauge(char *out, size_t cap, double value, double lo, double hi,
               const plot_opts_t *opt) {
    if (!out) return -1;
    plot_opts_t o = norm(opt);
    if (hi - lo < 1e-12) hi = lo + 1;
    double frac = (value - lo) / (hi - lo);
    if (frac < 0) frac = 0; if (frac > 1) frac = 1;
    pbuf_t b; pb_init(&b, out, cap);
    pb_title(&b, &o);
    const char *col = frac < 0.5 ? P_GREEN : frac < 0.8 ? P_YEL : P_RED;
    pb_col(&b, o.color, P_GREY); pb_putf(&b, "%.4g ", lo); pb_reset(&b, o.color);
    pb_puts(&b, "[");
    pb_hbar(&b, frac, o.width, o.color, col);
    pb_puts(&b, "]");
    pb_col(&b, o.color, P_GREY); pb_putf(&b, " %.4g", hi); pb_reset(&b, o.color);
    pb_col(&b, o.color, col);
    pb_putf(&b, "  %.4g (%.0f%%)\n", value, frac * 100);
    pb_reset(&b, o.color);
    return b.ovf ? -1 : pb_done(&b);
}

/* ── inline sparkline (Braille, 1 row) ────────────────────────────────── */
int plot_sparkline(char *out, size_t cap, const double *y, int n, const plot_opts_t *opt) {
    if (!out || !y || n <= 0) { if (out && cap) out[0] = '\0'; return -1; }
    plot_opts_t o = norm(opt);
    pbuf_t b; pb_init(&b, out, cap);
    tui_braille_t bc;
    tui_braille_init(&bc, n * 2, 4);
    tui_braille_plot(&bc, y, n);
    char *mb = NULL; size_t ms = 0;
    FILE *f = open_memstream(&mb, &ms);
    if (f) {
        if (o.color) fputs(o.color_seq ? o.color_seq : P_BCYAN, f);
        tui_braille_render(&bc, f, NULL);
        if (o.color) fputs(P_RESET, f);
        fclose(f);
        pb_puts(&b, mb);
    }
    free(mb);
    tui_braille_free(&bc);
    return b.ovf ? -1 : pb_done(&b);
}

/* ── extended display types ───────────────────────────────────────────── */

static const char *const PALETTE[6] = { P_BCYAN, P_GREEN, P_YEL, P_MAG, P_BLUE, P_RED };
static const char *const LEGEND_MARK = "■";

int plot_pie(char *out, size_t cap, const char *const *labels, const double *v,
             int n, const plot_opts_t *opt) {
    if (!out || !v || n <= 0) { if (out && cap) out[0] = '\0'; return -1; }
    plot_opts_t o = norm(opt);
    pbuf_t b; pb_init(&b, out, cap);
    pb_title(&b, &o);
    double total = 0; for (int i = 0; i < n; i++) total += v[i] > 0 ? v[i] : 0;
    if (total < 1e-12) total = 1;
    int W = o.width;
    /* segment widths via cumulative rounding so they sum to exactly W */
    int prev = 0; double acc = 0;
    for (int i = 0; i < n; i++) {
        acc += (v[i] > 0 ? v[i] : 0) / total;
        int edge = (int)(acc * W + 0.5);
        if (edge > W) edge = W;
        pb_col(&b, o.color, PALETTE[i % 6]);
        for (int c = prev; c < edge; c++) pb_puts(&b, "█");
        pb_reset(&b, o.color);
        prev = edge;
    }
    pb_puts(&b, "\n");
    for (int i = 0; i < n; i++) {
        pb_col(&b, o.color, PALETTE[i % 6]);
        pb_putf(&b, "%s ", LEGEND_MARK);
        pb_reset(&b, o.color);
        pb_putf(&b, "%-12.12s ", labels && labels[i] ? labels[i] : "");
        pb_col(&b, o.color, P_DIM);
        pb_putf(&b, "%.4g (%.1f%%)\n", v[i], 100.0 * (v[i] > 0 ? v[i] : 0) / total);
        pb_reset(&b, o.color);
    }
    return b.ovf ? -1 : pb_done(&b);
}

int plot_waterfall(char *out, size_t cap, const char *const *labels,
                   const double *d, int n, const plot_opts_t *opt) {
    if (!out || !d || n <= 0) { if (out && cap) out[0] = '\0'; return -1; }
    plot_opts_t o = norm(opt);
    pbuf_t b; pb_init(&b, out, cap);
    pb_title(&b, &o);
    /* cumulative path incl. the zero baseline */
    double cum = 0, mn = 0, mx = 0;
    for (int i = 0; i < n; i++) { cum += d[i]; if (cum < mn) mn = cum; if (cum > mx) mx = cum; }
    double span = mx - mn; if (span < 1e-12) span = 1;
    int W = o.width;
    cum = 0;
    int lblw = 0;
    for (int i = 0; i < n; i++) { int l = labels && labels[i] ? (int)strlen(labels[i]) : 0; if (l > lblw) lblw = l; }
    if (lblw > 12) lblw = 12;
    for (int i = 0; i < n; i++) {
        double start = cum, endv = cum + d[i];
        double a = (start < endv ? start : endv), c2 = (start < endv ? endv : start);
        int p0 = (int)((a - mn) / span * (W - 1) + 0.5);
        int p1 = (int)((c2 - mn) / span * (W - 1) + 0.5);
        pb_col(&b, o.color, P_GREY);
        pb_putf(&b, "%*.*s ", lblw, lblw, labels && labels[i] ? labels[i] : "");
        pb_reset(&b, o.color);
        pb_col(&b, o.color, d[i] >= 0 ? P_GREEN : P_RED);
        for (int x = 0; x < W; x++) pb_puts(&b, (x >= p0 && x <= p1) ? "█" : " ");
        pb_reset(&b, o.color);
        pb_col(&b, o.color, P_DIM);
        pb_putf(&b, " %+.4g\n", d[i]);
        pb_reset(&b, o.color);
        cum = endv;
    }
    /* total */
    int pt = (int)((cum - mn) / span * (W - 1) + 0.5);
    pb_col(&b, o.color, P_GREY);
    pb_putf(&b, "%*.*s ", lblw, lblw, "total");
    pb_reset(&b, o.color);
    pb_col(&b, o.color, "\033[1m");
    for (int x = 0; x < W; x++) pb_puts(&b, x <= pt ? "▓" : " ");
    pb_putf(&b, " %.4g\n", cum);
    pb_reset(&b, o.color);
    return b.ovf ? -1 : pb_done(&b);
}

int plot_bullet(char *out, size_t cap, const char *label, double value,
                double target, const double *ranges, int nr,
                const plot_opts_t *opt) {
    if (!out) return -1;
    plot_opts_t o = norm(opt);
    pbuf_t b; pb_init(&b, out, cap);
    double hi = value; if (target > hi) hi = target;
    for (int i = 0; i < nr; i++) if (ranges[i] > hi) hi = ranges[i];
    if (hi < 1e-12) hi = 1;
    int W = o.width;
    int pv = (int)(value / hi * (W - 1) + 0.5);
    int pt = (int)(target / hi * (W - 1) + 0.5);
    if (label && label[0]) { pb_col(&b, o.color, P_GREY); pb_putf(&b, "%-12.12s ", label); pb_reset(&b, o.color); }
    for (int x = 0; x < W; x++) {
        if (x == pt) { pb_col(&b, o.color, P_RED); pb_puts(&b, "┃"); pb_reset(&b, o.color); continue; }
        if (x <= pv) { pb_col(&b, o.color, P_BCYAN); pb_puts(&b, "█"); pb_reset(&b, o.color); continue; }
        /* qualitative band shade */
        double frac = (double)x / (W - 1) * hi;
        int band = 0; for (int i = 0; i < nr; i++) if (frac >= ranges[i]) band = i + 1;
        const char *sh = band == 0 ? "░" : band == 1 ? "▒" : "▓";
        pb_col(&b, o.color, P_DIM); pb_puts(&b, sh); pb_reset(&b, o.color);
    }
    pb_col(&b, o.color, P_DIM);
    pb_putf(&b, " %.4g (target %.4g)\n", value, target);
    pb_reset(&b, o.color);
    return b.ovf ? -1 : pb_done(&b);
}

int plot_lollipop(char *out, size_t cap, const char *const *labels,
                  const double *v, int n, const plot_opts_t *opt) {
    if (!out || !v || n <= 0) { if (out && cap) out[0] = '\0'; return -1; }
    plot_opts_t o = norm(opt);
    pbuf_t b; pb_init(&b, out, cap);
    pb_title(&b, &o);
    double mn, mx; minmax(v, n, &mn, &mx);
    double base = mn < 0 ? mn : 0, span = mx - base; if (span < 1e-12) span = 1;
    int lblw = 0;
    for (int i = 0; i < n; i++) { int l = labels && labels[i] ? (int)strlen(labels[i]) : 0; if (l > lblw) lblw = l; }
    if (lblw > 14) lblw = 14;
    int W = o.width - lblw - 10; if (W < 6) W = 6;
    for (int i = 0; i < n; i++) {
        int p = (int)((v[i] - base) / span * (W - 1) + 0.5);
        pb_col(&b, o.color, P_GREY);
        pb_putf(&b, "%*.*s ", lblw, lblw, labels && labels[i] ? labels[i] : "");
        pb_reset(&b, o.color);
        pb_col(&b, o.color, P_DIM);
        for (int x = 0; x < p; x++) pb_puts(&b, "·");
        pb_reset(&b, o.color);
        pb_col(&b, o.color, PALETTE[i % 6]);
        pb_puts(&b, "●");
        pb_reset(&b, o.color);
        pb_col(&b, o.color, P_DIM);
        pb_putf(&b, " %.4g\n", v[i]);
        pb_reset(&b, o.color);
    }
    return b.ovf ? -1 : pb_done(&b);
}

int plot_slope(char *out, size_t cap, const char *const *labels,
               const double *l, const double *r, int n, const plot_opts_t *opt) {
    if (!out || !l || !r || n <= 0) { if (out && cap) out[0] = '\0'; return -1; }
    plot_opts_t o = norm(opt);
    pbuf_t b; pb_init(&b, out, cap);
    pb_title(&b, &o);
    for (int i = 0; i < n; i++) {
        double dlt = r[i] - l[i];
        const char *arrow = dlt > 1e-9 ? "↑" : dlt < -1e-9 ? "↓" : "→";
        const char *col = dlt > 1e-9 ? P_GREEN : dlt < -1e-9 ? P_RED : P_DIM;
        pb_col(&b, o.color, P_GREY);
        pb_putf(&b, "%-12.12s ", labels && labels[i] ? labels[i] : "");
        pb_reset(&b, o.color);
        pb_putf(&b, "%8.4g ", l[i]);
        pb_col(&b, o.color, col);
        pb_putf(&b, "%s %-8.4g", arrow, r[i]);
        pb_putf(&b, " (%+.3g)\n", dlt);
        pb_reset(&b, o.color);
    }
    return b.ovf ? -1 : pb_done(&b);
}

int plot_ecdf(char *out, size_t cap, const double *samples, int n,
              const plot_opts_t *opt) {
    if (!out || !samples || n <= 0) { if (out && cap) out[0] = '\0'; return -1; }
    plot_opts_t o = norm(opt);
    double *s = malloc((size_t)n * sizeof(double));
    if (!s) return -1;
    memcpy(s, samples, (size_t)n * sizeof(double));
    qsort(s, n, sizeof(double), dcmp);
    double *y = malloc((size_t)n * sizeof(double));
    if (!y) { free(s); return -1; }
    for (int i = 0; i < n; i++) y[i] = (double)(i + 1) / n;   /* 0..1 cumulative */
    plot_opts_t lo = o; lo.title = NULL;
    pbuf_t b; pb_init(&b, out, cap);
    pb_title(&b, &o);
    int rc = plot_line_xy(b.p, (size_t)(b.end - b.p), s, y, n, &lo);
    if (rc > 0) b.p += strlen(b.p);
    free(s); free(y);
    return b.ovf ? -1 : pb_done(&b);
}

int plot_calendar(char *out, size_t cap, const double *v, int n,
                  const plot_opts_t *opt) {
    if (!out || !v || n <= 0) { if (out && cap) out[0] = '\0'; return -1; }
    plot_opts_t o = norm(opt);
    pbuf_t b; pb_init(&b, out, cap);
    pb_title(&b, &o);
    double mn, mx; minmax(v, n, &mn, &mx);
    double span = mx - mn; if (span < 1e-12) span = 1;
    int weeks = (n + 6) / 7;
    static const char *const dow[7] = { "Mon","Tue","Wed","Thu","Fri","Sat","Sun" };
    /* GitHub-style green ramp via 256-color */
    static const int ramp[] = { 235, 22, 28, 34, 40, 46 };
    for (int day = 0; day < 7; day++) {
        pb_col(&b, o.color, P_GREY); pb_putf(&b, "%s ", dow[day]); pb_reset(&b, o.color);
        for (int w = 0; w < weeks; w++) {
            int idx = w * 7 + day;
            if (idx >= n) { pb_puts(&b, "  "); continue; }
            double t = (v[idx] - mn) / span;
            int k = (int)(t * 5 + 0.5); if (k < 0) k = 0; if (k > 5) k = 5;
            if (o.color) pb_putf(&b, "\033[48;5;%dm  \033[0m", ramp[k]);
            else { const char *sh = k==0?"·":k<2?"░":k<4?"▒":"█"; pb_puts(&b, sh); pb_puts(&b, sh); }
        }
        pb_puts(&b, "\n");
    }
    return b.ovf ? -1 : pb_done(&b);
}

int plot_ridgeline(char *out, size_t cap, const char *const *names,
                   const double *const *series, const int *lens, int ns,
                   const plot_opts_t *opt) {
    if (!out || !series || ns <= 0) { if (out && cap) out[0] = '\0'; return -1; }
    plot_opts_t o = norm(opt);
    pbuf_t b; pb_init(&b, out, cap);
    pb_title(&b, &o);
    for (int s = 0; s < ns; s++) {
        if (!series[s] || lens[s] <= 0) continue;
        pb_col(&b, o.color, P_GREY);
        pb_putf(&b, "%-10.10s ", names && names[s] ? names[s] : "");
        pb_reset(&b, o.color);
        tui_braille_t bc;
        tui_braille_init(&bc, lens[s] * 2, 4);
        tui_braille_plot(&bc, series[s], lens[s]);
        pb_col(&b, o.color, PALETTE[s % 6]);
        char *mb = NULL; size_t ms = 0;
        FILE *f = open_memstream(&mb, &ms);
        if (f) { tui_braille_render(&bc, f, NULL); fclose(f); pb_puts(&b, mb); }
        free(mb);
        tui_braille_free(&bc);
        pb_reset(&b, o.color);
        pb_puts(&b, "\n");
    }
    return b.ovf ? -1 : pb_done(&b);
}

int plot_violin(char *out, size_t cap, const double *samples, int n,
                const plot_opts_t *opt) {
    if (!out || !samples || n <= 0) { if (out && cap) out[0] = '\0'; return -1; }
    plot_opts_t o = norm(opt);
    pbuf_t b; pb_init(&b, out, cap);
    pb_title(&b, &o);
    int bins = o.height > 2 ? o.height : 9;
    if (bins > 40) bins = 40;
    double mn, mx; minmax(samples, n, &mn, &mx);
    double span = mx - mn; if (span < 1e-12) span = 1;
    int counts[40] = {0}, cmax = 1;
    for (int i = 0; i < n; i++) {
        int bi = (int)((samples[i] - mn) / span * bins);
        if (bi >= bins) bi = bins - 1; if (bi < 0) bi = 0;
        if (++counts[bi] > cmax) cmax = counts[bi];
    }
    int half = o.width / 2; if (half < 4) half = 4;
    for (int r = bins - 1; r >= 0; r--) {       /* high values on top */
        int w = (int)((double)counts[r] / cmax * half + 0.5);
        pb_col(&b, o.color, P_GREY);
        pb_putf(&b, "%7.3g ", mn + span * (r + 0.5) / bins);
        pb_reset(&b, o.color);
        pb_col(&b, o.color, o.color_seq ? o.color_seq : P_MAG);
        for (int x = 0; x < half - w; x++) pb_puts(&b, " ");
        for (int x = 0; x < w; x++) pb_puts(&b, "█");
        pb_puts(&b, "▏");                          /* center spine */
        for (int x = 0; x < w; x++) pb_puts(&b, "█");
        pb_reset(&b, o.color);
        pb_puts(&b, "\n");
    }
    return b.ovf ? -1 : pb_done(&b);
}

/* 5-row block-digit font for 0-9, '.', '-', ':' '%' (columns separated by space) */
static const char *bignum_font(char ch, int row) {
    static const char *digits[13][5] = {
        {" ██ ","█  █","█  █","█  █"," ██ "},   /* 0 */
        {" █  ","██  "," █  "," █  ","███ "},   /* 1 */
        {"███ ","   █"," ██ ","█   ","████"},   /* 2 */
        {"███ ","   █"," ██ ","   █","███ "},   /* 3 */
        {"█  █","█  █","████","   █","   █"},   /* 4 */
        {"████","█   ","███ ","   █","███ "},   /* 5 */
        {" ██ ","█   ","███ ","█  █"," ██ "},   /* 6 */
        {"████","   █","  █ "," █  "," █  "},   /* 7 */
        {" ██ ","█  █"," ██ ","█  █"," ██ "},   /* 8 */
        {" ██ ","█  █"," ███","   █"," ██ "},   /* 9 */
        {"    ","    ","    ","    "," █  "},   /* . */
        {"    ","    ","███ ","    ","    "},   /* - */
        {"    "," █  ","    "," █  ","    "},   /* : */
    };
    int idx = -1;
    if (ch >= '0' && ch <= '9') idx = ch - '0';
    else if (ch == '.') idx = 10;
    else if (ch == '-') idx = 11;
    else if (ch == ':') idx = 12;
    if (idx < 0) return "    ";
    return digits[idx][row];
}

int plot_bignum(char *out, size_t cap, double value, const char *caption,
                const plot_opts_t *opt) {
    if (!out) return -1;
    plot_opts_t o = norm(opt);
    pbuf_t b; pb_init(&b, out, cap);
    char num[48];
    snprintf(num, sizeof num, "%.4g", value);
    for (int row = 0; row < 5; row++) {
        pb_col(&b, o.color, o.color_seq ? o.color_seq : P_BCYAN);
        for (const char *p = num; *p; p++) { pb_puts(&b, bignum_font(*p, row)); pb_puts(&b, " "); }
        pb_reset(&b, o.color);
        pb_puts(&b, "\n");
    }
    if (caption && caption[0]) {
        pb_col(&b, o.color, P_GREY);
        pb_putf(&b, "%s\n", caption);
        pb_reset(&b, o.color);
    }
    return b.ovf ? -1 : pb_done(&b);
}

/* ── JSON dispatch ────────────────────────────────────────────────────── */

typedef struct { double *v; int n; int cap; } darr_t;
static void darr_cb(const char *el, void *ctx) {
    darr_t *d = ctx;
    if (d->n >= d->cap) return;
    d->v[d->n++] = strtod(el, NULL);
}
/* Pull a numeric JSON array `key` into a fresh malloc'd buffer. */
static double *json_doubles(const char *json, const char *key, int *out_n, int cap) {
    double *buf = calloc((size_t)cap, sizeof(double));
    if (!buf) { *out_n = 0; return NULL; }
    darr_t d = { buf, 0, cap };
    json_array_foreach(json, key, darr_cb, &d);
    *out_n = d.n;
    if (d.n == 0) { free(buf); return NULL; }
    return buf;
}

typedef struct { char (*s)[24]; const char **p; int n; int cap; } sarr_t;
static void sarr_cb(const char *el, void *ctx) {
    sarr_t *a = ctx;
    if (a->n >= a->cap) return;
    /* element starts at first non-space; strip surrounding quotes */
    while (*el == ' ' || *el == '\t') el++;
    const char *s = el; size_t k = 0;
    if (*s == '"') { s++; while (*s && *s != '"' && k < 23) a->s[a->n][k++] = *s++; }
    else { while (*s && *s != ',' && *s != ']' && k < 23) a->s[a->n][k++] = *s++; }
    a->s[a->n][k] = '\0';
    a->p[a->n] = a->s[a->n];
    a->n++;
}

#define PLOT_MAX_PTS 512

/* ── strange attractor (phase-space, Braille dots + per-cell density color) ─
 * A 2-D chaotic map traced for many iterations. The orbit's fine filaments
 * come from the 2x4 Braille subpixel grid; each character cell is then tinted
 * on the viridis ramp by how often the orbit visited it (log-scaled), so the
 * dense folds of the attractor glow. No RNG — fully deterministic. */
int plot_attractor(char *out, size_t cap, const char *kind,
                   double a, double cb, double c, double d, long iters,
                   const plot_opts_t *opt) {
    if (!out || !cap) { if (out && cap) out[0] = '\0'; return -1; }
    plot_opts_t o = norm(opt);
    int clifford = (kind && (kind[0] == 'c' || kind[0] == 'C'));
    if (iters <= 0) iters = 300000;
    if (iters > 5000000) iters = 5000000;
    const long burn = 1000;

    pbuf_t b; pb_init(&b, out, cap);
    pb_title(&b, &o);

    tui_braille_t bc;
    tui_braille_init(&bc, o.width * 2, o.height * 4);
    int W = bc.w_cells, H = bc.h_cells;
    unsigned *cd = calloc((size_t)W * H, sizeof(unsigned));
    if (!cd) { tui_braille_free(&bc); if (cap) out[0] = '\0'; return -1; }

    #define ATTR_STEP(X,Y,NX,NY) do { \
        if (clifford) { NX = sin(a*(Y)) + c*cos(a*(X)); NY = sin(cb*(X)) + d*cos(cb*(Y)); } \
        else          { NX = sin(a*(Y)) - cos(cb*(X));  NY = sin(c*(X)) - cos(d*(Y));    } \
    } while (0)

    /* pass 1: discover the orbit's bounding box */
    double x = 0.1, y = 0.1, xmn = 1e30, xmx = -1e30, ymn = 1e30, ymx = -1e30;
    for (long i = 0; i < iters; i++) {
        double nx, ny; ATTR_STEP(x, y, nx, ny); x = nx; y = ny;
        if (i < burn) continue;
        if (x < xmn) xmn = x; if (x > xmx) xmx = x;
        if (y < ymn) ymn = y; if (y > ymx) ymx = y;
    }
    double xr = xmx - xmn, yr = ymx - ymn;
    if (xr < 1e-9) xr = 1; if (yr < 1e-9) yr = 1;
    xmn -= xr * 0.02; xmx += xr * 0.02; ymn -= yr * 0.02; ymx += yr * 0.02;
    xr = xmx - xmn; yr = ymx - ymn;

    /* pass 2: trace dots and accumulate per-cell visit density */
    x = 0.1; y = 0.1;
    unsigned maxd = 1;
    for (long i = 0; i < iters; i++) {
        double nx, ny; ATTR_STEP(x, y, nx, ny); x = nx; y = ny;
        if (i < burn) continue;
        int px = (int)((x - xmn) / xr * (bc.px_w - 1) + 0.5);
        int py = (int)((1.0 - (y - ymn) / yr) * (bc.px_h - 1) + 0.5);
        if (px < 0 || px >= bc.px_w || py < 0 || py >= bc.px_h) continue;
        tui_braille_set(&bc, px, py);
        unsigned v = ++cd[(py / 4) * W + (px / 2)];
        if (v > maxd) maxd = v;
    }
    #undef ATTR_STEP

    /* render: glyphs from the canvas, each cell tinted by its log-density */
    char *mb = NULL; size_t ms = 0;
    FILE *f = open_memstream(&mb, &ms);
    if (f) { tui_braille_render(&bc, f, NULL); fclose(f); }
    double lmax = log(1.0 + maxd);
    int row = 0;
    char *line = mb;
    while (line && *line && row < H) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        char *p = line; int col = 0;
        while (*p && col < W) {
            int glen = 1;                         /* a cell is one UTF-8 glyph */
            unsigned char lead = (unsigned char)*p;
            if (lead >= 0xF0) glen = 4; else if (lead >= 0xE0) glen = 3;
            else if (lead >= 0xC0) glen = 2;
            unsigned dv = cd[row * W + col];
            if (o.color) {
                if (dv == 0) pb_puts(&b, P_DIM);
                else pb_putf(&b, "\033[38;5;%dm", viridis256(log(1.0 + dv) / lmax));
            }
            char g[5]; int k = 0;
            for (; k < glen && *p; k++) g[k] = *p++;
            g[k] = '\0';
            pb_puts(&b, g);
            col++;
        }
        pb_reset(&b, o.color);
        pb_puts(&b, "\n");
        row++;
        if (!nl) break;
        line = nl + 1;
    }
    free(mb); free(cd); tui_braille_free(&bc);

    if (o.axes) {
        pb_col(&b, o.color, P_GREY);
        pb_putf(&b, "  %s  a=%.3g b=%.3g c=%.3g d=%.3g  %ldk pts\n",
                clifford ? "clifford" : "de jong", a, cb, c, d, iters / 1000);
        pb_reset(&b, o.color);
    }
    return b.ovf ? -1 : pb_done(&b);
}

int plot_dispatch(const char *input_json, char *out, size_t cap) {
    if (!input_json || !out) { if (out && cap) out[0] = '\0'; return -1; }
    char type[24] = "line";
    char *jtype = json_get_str(input_json, "type");
    if (jtype) { snprintf(type, sizeof type, "%s", jtype); free(jtype); }

    plot_opts_t o = plot_opts_default();
    char title[128] = "";
    char *jtitle = json_get_str(input_json, "title");
    if (jtitle) { snprintf(title, sizeof title, "%s", jtitle); free(jtitle);
                  if (title[0]) o.title = title; }
    o.width  = (int)json_get_double(input_json, "width",  o.width);
    o.height = (int)json_get_double(input_json, "height", o.height);
    o.color  = json_get_bool(input_json, "color", true);
    o.axes   = json_get_bool(input_json, "axes", true);

    int n = 0;
    double *data = json_doubles(input_json, "data", &n, PLOT_MAX_PTS);

    /* string labels */
    static char lbl_store[64][24];
    static const char *lbl_ptr[64];
    sarr_t sa = { lbl_store, lbl_ptr, 0, 64 };
    json_array_foreach(input_json, "labels", sarr_cb, &sa);
    const char *const *labels = sa.n > 0 ? lbl_ptr : NULL;

    int rc = -1;
    if (strcmp(type, "line") == 0 || strcmp(type, "area") == 0 ||
        strcmp(type, "scatter") == 0 || strcmp(type, "sparkline") == 0) {
        int xn = 0; double *x = json_doubles(input_json, "x", &xn, PLOT_MAX_PTS);
        if (strcmp(type, "area") == 0)         rc = plot_area(out, cap, data, n, &o);
        else if (strcmp(type, "scatter") == 0) rc = plot_scatter(out, cap, x ? x : data, data, n, &o);
        else if (strcmp(type, "sparkline") == 0) rc = plot_sparkline(out, cap, data, n, &o);
        else rc = plot_line_xy(out, cap, x, data, n, &o);
        free(x);
    } else if (strcmp(type, "bar") == 0) {
        rc = plot_bar(out, cap, labels, data, n, &o);
    } else if (strcmp(type, "column") == 0 || strcmp(type, "col") == 0) {
        rc = plot_column(out, cap, labels, data, n, &o);
    } else if (strcmp(type, "hist") == 0 || strcmp(type, "histogram") == 0) {
        rc = plot_hist(out, cap, data, n, (int)json_get_double(input_json, "bins", 0), &o);
    } else if (strcmp(type, "heatmap") == 0 || strcmp(type, "heat") == 0) {
        int rows = (int)json_get_double(input_json, "rows", 0);
        int cols = (int)json_get_double(input_json, "cols", 0);
        if (rows <= 0 || cols <= 0) {       /* infer a square-ish grid */
            cols = (int)(sqrt((double)n) + 0.5); if (cols < 1) cols = 1;
            rows = (n + cols - 1) / cols;
        }
        rc = plot_heatmap(out, cap, data, rows, cols, &o);
    } else if (strcmp(type, "box") == 0 || strcmp(type, "boxplot") == 0) {
        rc = plot_boxplot(out, cap, data, n, &o);
    } else if (strcmp(type, "candlestick") == 0 || strcmp(type, "candle") == 0 ||
               strcmp(type, "ohlc") == 0) {
        int on, hn, ln, cn;
        double *op = json_doubles(input_json, "open",  &on, PLOT_MAX_PTS);
        double *hp = json_doubles(input_json, "high",  &hn, PLOT_MAX_PTS);
        double *lp = json_doubles(input_json, "low",   &ln, PLOT_MAX_PTS);
        double *cp = json_doubles(input_json, "close", &cn, PLOT_MAX_PTS);
        int m = on; if (hn < m) m = hn; if (ln < m) m = ln; if (cn < m) m = cn;
        if (op && hp && lp && cp && m > 0)
            rc = plot_candlestick(out, cap, op, hp, lp, cp, m, &o);
        else { snprintf(out, cap, "candlestick needs open/high/low/close arrays"); rc = (int)strlen(out); }
        free(op); free(hp); free(lp); free(cp);
    } else if (strcmp(type, "gauge") == 0) {
        double val = json_get_double(input_json, "value", n > 0 ? data[0] : 0);
        double lo  = json_get_double(input_json, "min", 0);
        double hi  = json_get_double(input_json, "max", 100);
        rc = plot_gauge(out, cap, val, lo, hi, &o);
    } else if (strcmp(type, "pie") == 0 || strcmp(type, "donut") == 0) {
        rc = plot_pie(out, cap, labels, data, n, &o);
    } else if (strcmp(type, "waterfall") == 0) {
        rc = plot_waterfall(out, cap, labels, data, n, &o);
    } else if (strcmp(type, "bullet") == 0) {
        int rn = 0; double *rg = json_doubles(input_json, "ranges", &rn, 16);
        double val = json_get_double(input_json, "value", n > 0 ? data[0] : 0);
        double tgt = json_get_double(input_json, "target", 0);
        const char *lab = labels && sa.n > 0 ? labels[0] : (o.title ? o.title : "");
        rc = plot_bullet(out, cap, lab, val, tgt, rg, rn, &o);
        free(rg);
    } else if (strcmp(type, "lollipop") == 0 || strcmp(type, "dot") == 0) {
        rc = plot_lollipop(out, cap, labels, data, n, &o);
    } else if (strcmp(type, "slope") == 0) {
        int ln, rn2;
        double *lf = json_doubles(input_json, "left", &ln, PLOT_MAX_PTS);
        double *rt = json_doubles(input_json, "right", &rn2, PLOT_MAX_PTS);
        int m = ln < rn2 ? ln : rn2;
        if (lf && rt && m > 0) rc = plot_slope(out, cap, labels, lf, rt, m, &o);
        else { snprintf(out, cap, "slope needs left[] and right[] arrays"); rc = (int)strlen(out); }
        free(lf); free(rt);
    } else if (strcmp(type, "ecdf") == 0 || strcmp(type, "cdf") == 0) {
        rc = plot_ecdf(out, cap, data, n, &o);
    } else if (strcmp(type, "calendar") == 0 || strcmp(type, "cal") == 0) {
        rc = plot_calendar(out, cap, data, n, &o);
    } else if (strcmp(type, "violin") == 0) {
        rc = plot_violin(out, cap, data, n, &o);
    } else if (strcmp(type, "bignum") == 0 || strcmp(type, "kpi") == 0) {
        double val = json_get_double(input_json, "value", n > 0 ? data[0] : 0);
        rc = plot_bignum(out, cap, val, o.title, &o);
    } else if (strcmp(type, "attractor") == 0 || strcmp(type, "strange") == 0) {
        char kind[16] = "dejong";
        char *jk = json_get_str(input_json, "kind");
        if (jk) { snprintf(kind, sizeof kind, "%s", jk); free(jk); }
        int clifford = (kind[0] == 'c' || kind[0] == 'C');
        double aa = json_get_double(input_json, "a", clifford ? -1.4 : 1.4);
        double bb = json_get_double(input_json, "b", clifford ?  1.6 : -2.3);
        double cc = json_get_double(input_json, "c", clifford ?  1.0 :  2.4);
        double dd = json_get_double(input_json, "d", clifford ?  0.7 : -2.1);
        long   it = (long)json_get_double(input_json, "iters", 300000);
        if (o.width  < 50) o.width  = 64;   /* attractors want room to breathe */
        if (o.height < 14) o.height = 18;
        rc = plot_attractor(out, cap, kind, aa, bb, cc, dd, it, &o);
    } else if (strcmp(type, "ridgeline") == 0 || strcmp(type, "ridge") == 0) {
        /* parse "series":[[...],[...]] into per-series double arrays */
        const char *sp = json_get_raw(input_json, "series");
        double *sv[24] = {0}; int sl[24] = {0}; int nser = 0;
        if (sp) {
            const char *q = sp;
            while (*q && *q != '[') q++;
            if (*q == '[') q++;                 /* enter outer array */
            while (*q && nser < 24) {
                while (*q == ' ' || *q == ',' || *q == '\n' || *q == '\t') q++;
                if (*q == ']' || *q == '\0') break;
                if (*q == '[') {
                    double *buf = calloc(PLOT_MAX_PTS, sizeof(double)); int cnt = 0;
                    q++;
                    while (*q && *q != ']' && cnt < PLOT_MAX_PTS) {
                        while (*q == ' ' || *q == ',' || *q == '\n' || *q == '\t') q++;
                        if (*q == ']' || *q == '\0') break;
                        char *e; double dv = strtod(q, &e);
                        if (e == q) break;
                        buf[cnt++] = dv; q = e;
                    }
                    if (*q == ']') q++;
                    sv[nser] = buf; sl[nser] = cnt; nser++;
                } else q++;
            }
        }
        if (nser > 0)
            rc = plot_ridgeline(out, cap, labels, (const double *const *)sv, sl, nser, &o);
        else { snprintf(out, cap, "ridgeline needs series:[[...],[...]]"); rc = (int)strlen(out); }
        for (int i = 0; i < nser; i++) free(sv[i]);
        free((void *)sp);
    } else {
        snprintf(out, cap, "unknown plot type '%s'", type);
        rc = (int)strlen(out);
    }
    free(data);
    return rc;
}
