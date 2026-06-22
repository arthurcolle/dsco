/* ── fractal.c: ray-marched 3-D / 4-D fractals on the Braille grid ──────────
 * Distance-estimated sphere tracing of the Mandelbulb, quaternion Julia set
 * (the 4-D one), and Mandelbox, shaded onto a 2×4 Braille subpixel canvas.
 *
 * Recovering grayscale from a binary dot grid: per subpixel we ray-march a
 * luminance in [0,1], light the dot when it clears an ordered (Bayer) dither
 * threshold, and separately tint each character cell on a palette by the mean
 * luminance of its 8 subpixels. Dot-density and color both carry the shading,
 * so the surface reads as lit 3-D even in a monochrome terminal. */

#include "fractal.h"
#include "anim.h"
#include "tui.h"
#include "json_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ── small vec3 ──────────────────────────────────────────────────────────── */
typedef struct { double x, y, z; } V3;
static inline V3   v3(double x, double y, double z) { V3 r = {x, y, z}; return r; }
static inline V3   v3add(V3 a, V3 b) { return v3(a.x+b.x, a.y+b.y, a.z+b.z); }
static inline V3   v3sub(V3 a, V3 b) { return v3(a.x-b.x, a.y-b.y, a.z-b.z); }
static inline V3   v3scl(V3 a, double s) { return v3(a.x*s, a.y*s, a.z*s); }
static inline double v3dot(V3 a, V3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
static inline V3   v3cross(V3 a, V3 b) {
    return v3(a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x);
}
static inline double v3len(V3 a) { return sqrt(v3dot(a, a)); }
static inline V3   v3norm(V3 a) { double l = v3len(a); return l > 1e-12 ? v3scl(a, 1.0/l) : a; }

/* ── options ─────────────────────────────────────────────────────────────── */
typedef enum { F_BULB, F_QUAT, F_BOX } fkind_t;

typedef struct {
    fkind_t kind;
    const char *title;
    const char *palette;       /* fire | ice | viridis | grey */
    int    width, height;      /* character cells (0 = auto) */
    bool   color;
    int    iters;              /* fractal iteration cap */
    double power;              /* mandelbulb exponent */
    double cx, cy, cz, cw;     /* quaternion Julia constant */
    double wslice;             /* quaternion 4th-dim slice */
    double scale;              /* mandelbox scale */
    double yaw, pitch, dist, fov;
    long   gens;               /* anim: frames (<=0 = until ^C) */
    int    fps;
} fopts_t;

static fkind_t fkind_of(const char *k) {
    if (!k) return F_BULB;
    if (!strcmp(k, "quaternion") || !strcmp(k, "julia4d") || !strcmp(k, "qjulia")) return F_QUAT;
    if (!strcmp(k, "mandelbox")  || !strcmp(k, "box"))                              return F_BOX;
    return F_BULB;   /* mandelbulb / bulb / default */
}

bool fractal_is_kind(const char *kind) {
    if (!kind) return false;
    return !strcmp(kind, "mandelbulb") || !strcmp(kind, "bulb") ||
           !strcmp(kind, "quaternion") || !strcmp(kind, "julia4d") ||
           !strcmp(kind, "qjulia")     || !strcmp(kind, "mandelbox") ||
           !strcmp(kind, "box");
}

static fopts_t fractal_parse(const char *json, char **owned1, char **owned2, char **owned3) {
    fopts_t f; memset(&f, 0, sizeof f);
    /* anim.c keys the discriminator as "kind"; the plot tool keys it as "type" */
    char *kind = json ? json_get_str(json, "kind") : NULL;
    if (!kind && json) kind = json_get_str(json, "type");
    char *pal  = json ? json_get_str(json, "palette") : NULL;
    char *titl = json ? json_get_str(json, "title") : NULL;
    *owned1 = kind; *owned2 = pal; *owned3 = titl;

    f.kind    = fkind_of(kind);
    f.palette = pal;
    f.title   = titl;
    f.width   = json ? json_get_int(json, "width", 0) : 0;
    f.height  = json ? json_get_int(json, "height", 0) : 0;
    f.color   = json ? json_get_bool(json, "color", true) : true;
    f.iters   = json ? json_get_int(json, "iters", f.kind == F_QUAT ? 10 : (f.kind == F_BOX ? 12 : 8)) : 8;
    f.power   = json ? json_get_double(json, "power", 8.0) : 8.0;
    f.cx      = json ? json_get_double(json, "cx", -0.45) : -0.45;
    f.cy      = json ? json_get_double(json, "cy",  0.30) :  0.30;
    f.cz      = json ? json_get_double(json, "cz", -0.20) : -0.20;
    f.cw      = json ? json_get_double(json, "cw",  0.10) :  0.10;
    f.wslice  = json ? json_get_double(json, "wslice", 0.0) : 0.0;
    f.scale   = json ? json_get_double(json, "scale", -1.8) : -1.8;
    f.yaw     = json ? json_get_double(json, "yaw",   0.6) : 0.6;
    f.pitch   = json ? json_get_double(json, "pitch", 0.5) : 0.5;
    f.dist    = json ? json_get_double(json, "dist",  f.kind == F_BOX ? 7.0 : 2.7) : 2.7;
    f.fov     = json ? json_get_double(json, "fov",   0.85) : 0.85;
    f.gens    = json ? json_get_int(json, "gens", 0) : 0;
    f.fps     = json ? json_get_int(json, "fps", 20) : 20;
    if (f.iters < 2)  f.iters = 2;
    if (f.iters > 64) f.iters = 64;
    return f;
}

/* ── palettes (256-color ramps) ──────────────────────────────────────────── */
static const int PAL_FIRE[] = {16,52,88,124,160,166,202,208,214,220,226,228,229,230,231};
static const int PAL_ICE[]  = {16,17,18,19,20,21,27,33,39,45,51,87,123,159,195,231};
static const int PAL_VIR[]  = {16,17,18,20,26,32,38,44,49,48,46,82,118,154,190,226};
static const int PAL_GREY[] = {16,233,235,237,239,241,243,245,247,249,251,253,255};

static void pal_pick(const char *name, fkind_t k, const int **pal, int *n) {
    if (name && !strcmp(name, "fire"))    { *pal = PAL_FIRE; *n = (int)(sizeof PAL_FIRE/sizeof*PAL_FIRE); return; }
    if (name && !strcmp(name, "ice"))     { *pal = PAL_ICE;  *n = (int)(sizeof PAL_ICE /sizeof*PAL_ICE ); return; }
    if (name && !strcmp(name, "viridis")) { *pal = PAL_VIR;  *n = (int)(sizeof PAL_VIR /sizeof*PAL_VIR ); return; }
    if (name && !strcmp(name, "grey"))    { *pal = PAL_GREY; *n = (int)(sizeof PAL_GREY/sizeof*PAL_GREY); return; }
    /* per-kind default: bulb=fire, quaternion=ice, box=viridis */
    if (k == F_QUAT) { *pal = PAL_ICE; *n = (int)(sizeof PAL_ICE/sizeof*PAL_ICE); }
    else if (k == F_BOX) { *pal = PAL_VIR; *n = (int)(sizeof PAL_VIR/sizeof*PAL_VIR); }
    else { *pal = PAL_FIRE; *n = (int)(sizeof PAL_FIRE/sizeof*PAL_FIRE); }
}

/* 4×4 ordered-dither matrix, normalized at use to (0,1). */
static const int BAYER4[4][4] = {{0,8,2,10},{12,4,14,6},{3,11,1,9},{15,7,13,5}};

/* ── distance estimators ─────────────────────────────────────────────────── */
static double de_bulb(V3 p, const fopts_t *f) {
    double x = p.x, y = p.y, z = p.z, dr = 1.0, r = 0.0;
    const double power = f->power;
    for (int i = 0; i < f->iters; i++) {
        r = sqrt(x*x + y*y + z*z);
        if (r > 2.0) break;
        double rr = r > 1e-9 ? r : 1e-9;
        double theta = acos(z / rr);
        double phi   = atan2(y, x);
        double rp    = pow(rr, power - 1.0);
        dr = rp * power * dr + 1.0;
        double zr = rp * rr;                 /* r^power */
        double th = theta * power, ph = phi * power, st = sin(th);
        x = zr * st * cos(ph) + p.x;
        y = zr * st * sin(ph) + p.y;
        z = zr * cos(th)      + p.z;
    }
    double rr = r > 1e-9 ? r : 1e-9;
    return 0.5 * log(rr) * rr / (dr > 1e-9 ? dr : 1e-9);
}

/* quaternion Julia: z ← z² + c in H, slice at the 4th coord f->wslice */
static double de_quat(V3 p, const fopts_t *f) {
    double a = p.x, b = p.y, c = p.z, d = f->wslice;
    double dz = 1.0, r2 = 0.0;
    for (int i = 0; i < f->iters; i++) {
        r2 = a*a + b*b + c*c + d*d;
        if (r2 > 16.0) break;
        double r = sqrt(r2);
        dz = 2.0 * r * dz;                   /* |z'| ← 2|z||z'| */
        double na = a*a - b*b - c*c - d*d + f->cx;   /* quaternion square + c */
        double nb = 2.0*a*b + f->cy;
        double nc = 2.0*a*c + f->cz;
        double nd = 2.0*a*d + f->cw;
        a = na; b = nb; c = nc; d = nd;
    }
    double r = sqrt(r2 > 1e-18 ? r2 : 1e-18);
    return 0.5 * r * log(r) / (dz > 1e-9 ? dz : 1e-9);
}

static double de_box(V3 p, const fopts_t *f) {
    const double scale = f->scale;
    double x = p.x, y = p.y, z = p.z, dr = 1.0;
    for (int i = 0; i < f->iters; i++) {
        if (x > 1) x = 2 - x; else if (x < -1) x = -2 - x;   /* box fold */
        if (y > 1) y = 2 - y; else if (y < -1) y = -2 - y;
        if (z > 1) z = 2 - z; else if (z < -1) z = -2 - z;
        double r2 = x*x + y*y + z*z;                          /* sphere fold */
        if (r2 < 0.25)      { double t = 4.0;     x*=t; y*=t; z*=t; dr*=t; }
        else if (r2 < 1.0)  { double t = 1.0/r2;  x*=t; y*=t; z*=t; dr*=t; }
        x = scale*x + p.x; y = scale*y + p.y; z = scale*z + p.z;
        dr = dr * fabs(scale) + 1.0;
        if (x*x + y*y + z*z > 1e6) break;
    }
    return sqrt(x*x + y*y + z*z) / fabs(dr);
}

static double de(V3 p, const fopts_t *f) {
    switch (f->kind) {
        case F_QUAT: return de_quat(p, f);
        case F_BOX:  return de_box(p, f);
        default:     return de_bulb(p, f);
    }
}

/* ── ray march → luminance in [0,1] for one ray ──────────────────────────── */
static double march(V3 ro, V3 rd, const fopts_t *f) {
    const int   MAXS = 110;
    const double MAXD = 12.0;
    double t = 0.0, minh = 1e9;
    int s, hit = 0;
    for (s = 0; s < MAXS; s++) {
        V3 p = v3add(ro, v3scl(rd, t));
        double d = de(p, f);
        if (d < minh) minh = d;
        double eps = 0.0006 * (1.0 + t);
        if (d < eps) { hit = 1; break; }
        t += d * 0.92;
        if (t > MAXD) break;
    }
    if (!hit) {
        /* tight halo hugging the silhouette; the far field falls to true black
         * so the void doesn't dither into background stipple */
        double g = 0.16 * exp(-minh * 11.0) - 0.03;
        return g > 0 ? g : 0.0;
    }
    V3 p = v3add(ro, v3scl(rd, t));
    double e = 0.0009;
    V3 n = v3norm(v3(
        de(v3(p.x+e,p.y,p.z), f) - de(v3(p.x-e,p.y,p.z), f),
        de(v3(p.x,p.y+e,p.z), f) - de(v3(p.x,p.y-e,p.z), f),
        de(v3(p.x,p.y,p.z+e), f) - de(v3(p.x,p.y,p.z-e), f)));
    V3 L  = v3norm(v3(0.6, 0.75, -0.45));
    double diff = v3dot(n, L); if (diff < 0) diff = 0;
    double amb  = 0.18 + 0.12 * (n.y * 0.5 + 0.5);
    double ao   = 1.0 - (double)s / MAXS;        /* cheap occlusion from step count */
    double lum  = (amb + diff * 0.85) * (0.45 + 0.55 * ao);
    if (lum < 0) lum = 0; if (lum > 1) lum = 1;
    return lum;
}

/* ── camera + frame render into a Braille canvas + per-cell color ─────────── */
static void fractal_frame(tui_braille_t *bc, int *cell_color, const fopts_t *f) {
    const int gw = bc->px_w, gh = bc->px_h, W = bc->w_cells, H = bc->h_cells;
    tui_braille_clear(bc);

    /* orbit camera looking at the origin */
    V3 tgt = v3(0,0,0);
    V3 ro  = v3(f->dist * cos(f->pitch) * sin(f->yaw),
                f->dist * sin(f->pitch),
                f->dist * cos(f->pitch) * cos(f->yaw));
    V3 fwd = v3norm(v3sub(tgt, ro));
    V3 rgt = v3norm(v3cross(v3(0,1,0), fwd));
    if (v3len(rgt) < 1e-6) rgt = v3(1,0,0);
    V3 up  = v3cross(fwd, rgt);
    double tanH = tan(f->fov * 0.5);
    double aspect = (double)gw / (double)gh;

    const int *pal; int paln; pal_pick(f->palette, f->kind, &pal, &paln);
    double *cellsum = calloc((size_t)W * H, sizeof(double));
    if (!cellsum) return;

    for (int py = 0; py < gh; py++) {
        double v = -(((py + 0.5) / gh) * 2.0 - 1.0) * tanH;
        for (int px = 0; px < gw; px++) {
            double u = (((px + 0.5) / gw) * 2.0 - 1.0) * aspect * tanH;
            V3 rd = v3norm(v3add(fwd, v3add(v3scl(rgt, u), v3scl(up, v))));
            double lum = march(ro, rd, f);
            double thr = (BAYER4[py & 3][px & 3] + 0.5) / 16.0;
            if (lum > thr) tui_braille_set(bc, px, py);
            cellsum[(py / 4) * W + (px / 2)] += lum;
        }
    }
    for (int i = 0; i < W * H; i++) {
        double b = cellsum[i] / 8.0; if (b > 1) b = 1;
        cell_color[i] = (b < 0.02) ? -1 : pal[(int)(b * (paln - 1) + 0.5)];
    }
    free(cellsum);
}

/* ── still frame → string ─────────────────────────────────────────────────── */
static const char *fractal_label(fkind_t k) {
    return k == F_QUAT ? "Quaternion Julia (4-D slice)"
         : k == F_BOX  ? "Mandelbox"
                       : "Mandelbulb";
}

int fractal_plot(char *out, size_t cap, const char *json) {
    if (!out || !cap) { if (out && cap) out[0] = '\0'; return -1; }
    char *o1, *o2, *o3;
    fopts_t f = fractal_parse(json, &o1, &o2, &o3);

    int W, H; anim_resolve_size(f.width ? f.width : 64, f.height ? f.height : 30, &W, &H);
    tui_braille_t bc; tui_braille_init(&bc, W * 2, H * 4);
    int *cc = malloc((size_t)W * H * sizeof(int));
    if (!cc) { tui_braille_free(&bc); free(o1); free(o2); free(o3); if (cap) out[0] = '\0'; return -1; }

    fractal_frame(&bc, cc, &f);

    char *mb = NULL; size_t ms = 0;
    FILE *mf = open_memstream(&mb, &ms);
    if (mf) {
        const char *t = f.title ? f.title : fractal_label(f.kind);
        if (f.color) fprintf(mf, "\033[1m%s\033[0m\n", t); else fprintf(mf, "%s\n", t);
        anim_paint(mf, &bc, f.color, cc);
        if (f.kind == F_QUAT)
            fprintf(mf, "\033[2m  c=(%.2f,%.2f,%.2f,%.2f)  w=%.2f  iters=%d\033[0m\n",
                    f.cx, f.cy, f.cz, f.cw, f.wslice, f.iters);
        else if (f.kind == F_BOX)
            fprintf(mf, "\033[2m  scale=%.2f  iters=%d\033[0m\n", f.scale, f.iters);
        else
            fprintf(mf, "\033[2m  power=%.1f  iters=%d\033[0m\n", f.power, f.iters);
        fclose(mf);
    }
    int n = mb ? (int)snprintf(out, cap, "%s", mb) : -1;
    free(mb); free(cc); tui_braille_free(&bc);
    free(o1); free(o2); free(o3);
    return n;
}

/* ── animated loop ───────────────────────────────────────────────────────── */
int fractal_anim(const char *json) {
    char *o1, *o2, *o3;
    fopts_t f = fractal_parse(json, &o1, &o2, &o3);

    int W, H; anim_resolve_size(f.width, f.height, &W, &H);
    tui_braille_t bc; tui_braille_init(&bc, W * 2, H * 4);
    int *cc = malloc((size_t)W * H * sizeof(int));
    if (!cc) { tui_braille_free(&bc); free(o1); free(o2); free(o3); return -1; }

    const long budget = anim_frame_budget(f.gens);
    anim_term_t term; anim_begin(&term);
    long frames = 0;
    for (; budget < 0 || frames < budget; frames++) {
        if (anim_interrupted()) break;
        double ph = frames * 0.04;
        f.yaw   += 0.035;                         /* slow orbit */
        f.pitch  = 0.45 + 0.30 * sin(ph * 0.6);
        if (f.kind == F_QUAT) {
            /* the 4-D reveal: sweep the slice and rotate c through 4-space */
            f.wslice = 0.55 * sin(ph * 0.8);
            f.cx = -0.45 + 0.10 * sin(ph);
            f.cy =  0.30 + 0.10 * cos(ph * 1.1);
            f.cw =  0.10 + 0.20 * sin(ph * 0.5);
        } else if (f.kind == F_BULB) {
            f.power = 8.0 + 4.0 * sin(ph * 0.5);  /* breathe the exponent 4..12 */
        } else {
            f.scale = -1.8 + 0.3 * sin(ph * 0.7);
        }

        fractal_frame(&bc, cc, &f);

        char hdr[200];
        if (f.kind == F_QUAT)
            snprintf(hdr, sizeof hdr, "c=(%+.2f,%+.2f,%+.2f,%+.2f)  w=%+.2f  frame %-5ld  ^C to stop",
                     f.cx, f.cy, f.cz, f.cw, f.wslice, frames);
        else if (f.kind == F_BULB)
            snprintf(hdr, sizeof hdr, "power=%.2f  yaw=%.2f  frame %-5ld  ^C to stop",
                     f.power, f.yaw, frames);
        else
            snprintf(hdr, sizeof hdr, "scale=%.2f  yaw=%.2f  frame %-5ld  ^C to stop",
                     f.scale, f.yaw, frames);

        fputs("\033[H", stdout);
        printf("\033[1m%s\033[0m\033[K\n", f.title ? f.title : fractal_label(f.kind));
        printf("\033[2m%s\033[0m\033[K\n", hdr);
        anim_paint(stdout, &bc, f.color, cc);
        fflush(stdout);

        if (!anim_tick(f.fps)) break;
    }
    anim_end(&term);

    free(cc); tui_braille_free(&bc);
    free(o1); free(o2); free(o3);
    return (int)frames;
}
