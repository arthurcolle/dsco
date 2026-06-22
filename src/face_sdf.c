/* ── face_sdf.c: a parametric signed-distance human face ──────────────────────
 * The head is composed from analytic SDF primitives (ellipsoids, capsules,
 * spheres, torus) blended with polynomial smooth-min so the whole reads as one
 * continuous, Lipschitz-ish field — clean enough for finite-difference normals.
 * Symmetric features use fabs(x) so they are computed once and mirrored.
 *
 * Object space: face looks down +Z, up is +Y, head radius ~1, centered near
 * the origin. All params are unitless ratios (see face_sdf.h). */

#include "face_sdf.h"

#include <math.h>
#include <string.h>
#include <stddef.h>

/* ── tiny vec3 ───────────────────────────────────────────────────────────── */
typedef struct { double x, y, z; } V3;
static inline V3 v3(double x, double y, double z) { V3 r = {x, y, z}; return r; }
static inline V3 vsub(V3 a, V3 b) { return v3(a.x-b.x, a.y-b.y, a.z-b.z); }
static inline double vlen(V3 a) { return sqrt(a.x*a.x + a.y*a.y + a.z*a.z); }

/* ── primitives ──────────────────────────────────────────────────────────── */
static double sdSphere(V3 p, double r) { return vlen(p) - r; }

/* scaled-gradient ellipsoid approximation (Quilez) */
static double sdEllipsoid(V3 p, V3 r) {
    double k0 = sqrt((p.x*p.x)/(r.x*r.x) + (p.y*p.y)/(r.y*r.y) + (p.z*p.z)/(r.z*r.z));
    double k1 = sqrt((p.x*p.x)/(r.x*r.x*r.x*r.x) + (p.y*p.y)/(r.y*r.y*r.y*r.y) + (p.z*p.z)/(r.z*r.z*r.z*r.z));
    if (k1 < 1e-9) return k0 - 1.0;
    return k0 * (k0 - 1.0) / k1;
}

static double sdBox(V3 p, V3 b) {
    double qx = fabs(p.x) - b.x, qy = fabs(p.y) - b.y, qz = fabs(p.z) - b.z;
    double ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0, oz = qz > 0 ? qz : 0;
    double mx = qx > qy ? (qx > qz ? qx : qz) : (qy > qz ? qy : qz);
    return sqrt(ox*ox + oy*oy + oz*oz) + (mx < 0 ? mx : 0);
}
static __attribute__((unused)) double sdRoundBox(V3 p, V3 b, double rad) { return sdBox(p, b) - rad; }

/* capsule from a to b, radius r */
static double sdCapsule(V3 p, V3 a, V3 b, double r) {
    V3 pa = vsub(p, a), ba = vsub(b, a);
    double h = (pa.x*ba.x + pa.y*ba.y + pa.z*ba.z) / (ba.x*ba.x + ba.y*ba.y + ba.z*ba.z + 1e-9);
    if (h < 0) h = 0; if (h > 1) h = 1;
    V3 d = v3(pa.x - ba.x*h, pa.y - ba.y*h, pa.z - ba.z*h);
    return vlen(d) - r;
}

static __attribute__((unused)) double sdTorus(V3 p, double R, double r) {
    double q = sqrt(p.x*p.x + p.z*p.z) - R;
    return sqrt(q*q + p.y*p.y) - r;
}

/* polynomial smooth-min / smooth-max */
static double smin(double a, double b, double k) {
    if (k < 1e-6) return a < b ? a : b;
    double h = 0.5 + 0.5 * (b - a) / k;
    if (h < 0) h = 0; if (h > 1) h = 1;
    return (b * (1.0 - h) + a * h) - k * h * (1.0 - h);
}
static double smax(double a, double b, double k) { return -smin(-a, -b, k); }

/* ── defaults / presets ──────────────────────────────────────────────────── */
void face_params_default(face_params_t *p) {
    if (!p) return;
    p->skull_r = 1.00; p->skull_flat = 0.10; p->face_long = 1.15;
    p->jaw_w = 0.62; p->jaw_len = 0.55; p->cheek = 0.20; p->temple = 0.90;
    p->brow = 0.10; p->brow_y = 0.18; p->forehead = 0.95;
    p->nose_len = 0.34; p->nose_w = 0.18; p->nose_bridge = 0.10; p->nose_y = 0.02;
    p->eye_sep = 0.30; p->eye_y = 0.06; p->eye_size = 0.14; p->eye_depth = 0.10; p->lid = 0.30;
    p->mouth_w = 0.26; p->mouth_y = -0.40; p->lip = 0.06; p->smile = 0.10;
    p->ear = 0.16; p->ear_y = 0.00;
    p->blend = 0.06;
}

int face_preset_count(void) { return 4; }

void face_preset(int idx, face_params_t *p, const char **name_out) {
    face_params_default(p);
    int n = face_preset_count();
    idx = ((idx % n) + n) % n;
    static const char *names[] = { "neutral", "round", "angular", "elder" };
    switch (idx) {
        case 1: /* round */
            p->face_long = 1.02; p->temple = 1.00; p->jaw_w = 0.72; p->jaw_len = 0.46;
            p->cheek = 0.30; p->forehead = 1.05; p->eye_size = 0.15; p->blend = 0.09;
            break;
        case 2: /* angular */
            p->face_long = 1.22; p->temple = 0.82; p->jaw_w = 0.56; p->jaw_len = 0.66;
            p->cheek = 0.13; p->brow = 0.15; p->nose_len = 0.40; p->nose_bridge = 0.15;
            p->blend = 0.04;
            break;
        case 3: /* elder */
            p->face_long = 1.18; p->jaw_w = 0.58; p->cheek = 0.10; p->brow = 0.16;
            p->brow_y = 0.16; p->nose_len = 0.38; p->nose_w = 0.20; p->lip = 0.045;
            p->eye_y = 0.05; p->lid = 0.42; p->ear = 0.18; p->blend = 0.055;
            break;
        default: break;     /* neutral */
    }
    if (name_out) *name_out = names[idx];
}

/* ── distance field ──────────────────────────────────────────────────────── */
double face_sdf_de(double X, double Y, double Z, const face_params_t *p) {
    face_params_t dp;
    if (!p) { face_params_default(&dp); p = &dp; }
    V3 q = v3(X, Y, Z);
    double R = p->skull_r;
    double k = p->blend;

    /* cranium: tall ellipsoid; flatten the back (-Z) a touch */
    V3 head_c = v3(0.0, 0.08, -0.04);
    V3 head_r = v3(R * p->temple, R * p->face_long, R * (1.0 - 0.18 * p->skull_flat));
    double d = sdEllipsoid(vsub(q, head_c), head_r);

    /* forehead fullness — small bulge up front */
    double fhead = sdEllipsoid(vsub(q, v3(0.0, R * 0.42, R * 0.30)),
                               v3(R * 0.62 * p->forehead, R * 0.40, R * 0.55));
    d = smin(d, fhead, k * 1.4);

    /* jaw + chin: lower, narrower mass tapering down */
    V3 jaw_c = v3(0.0, -R * (0.30 + 0.35 * p->jaw_len), R * 0.10);
    V3 jaw_r = v3(R * p->jaw_w, R * p->jaw_len, R * 0.80);
    double jaw = sdEllipsoid(vsub(q, jaw_c), jaw_r);
    d = smin(d, jaw, k * 1.6);

    /* chin point */
    double chin = sdSphere(vsub(q, v3(0.0, -R * (0.55 + 0.30 * p->jaw_len), R * 0.42)), R * 0.16);
    d = smin(d, chin, k * 1.4);

    /* mirrored x for symmetric features */
    double ax = fabs(q.x);
    V3 qm = v3(ax, q.y, q.z);

    /* cheekbones */
    double cheek = sdEllipsoid(vsub(qm, v3(R * 0.42, R * 0.02, R * 0.40)),
                               v3(R * 0.30, R * 0.26, R * 0.30));
    d = smin(d, cheek - R * 0.04 * p->cheek, k * 1.5);

    /* brow ridge: a capsule arching across, above the eyes */
    double brow = sdCapsule(qm, v3(0.02, R * p->brow_y, R * 0.62),
                            v3(R * 0.40, R * (p->brow_y - 0.02), R * 0.50), R * 0.07);
    d = smin(d, brow - R * 0.04 * p->brow, k * 0.9);

    /* nose: bridge → tip ridge, plus a tip bulb and nostril wings */
    double bridge = sdCapsule(q, v3(0.0, R * (p->nose_y + 0.20), R * 0.55),
                              v3(0.0, R * (p->nose_y - 0.04), R * (0.62 + p->nose_len * 0.4)),
                              R * (0.05 + 0.04 * p->nose_bridge));
    d = smin(d, bridge, k * 0.7);
    double tip = sdSphere(vsub(q, v3(0.0, R * (p->nose_y - 0.06), R * (0.66 + p->nose_len * 0.6))),
                          R * (0.08 + 0.02 * p->nose_w));
    d = smin(d, tip, k * 0.6);
    double nostril = sdSphere(vsub(qm, v3(R * (0.06 + p->nose_w * 0.5), R * (p->nose_y - 0.08),
                                          R * (0.60 + p->nose_len * 0.4))),
                              R * 0.06);
    d = smin(d, nostril, k * 0.6);

    /* eye sockets: carve a shallow recess, then set a proud eyeball into it */
    double eyey = R * (p->eye_y + 0.02);
    V3 eye_c = v3(R * p->eye_sep, eyey, R * (0.50 - p->eye_depth));
    double socket = sdSphere(vsub(qm, v3(eye_c.x, eye_c.y, eye_c.z + R * 0.10)),
                             R * (p->eye_size + 0.07));
    d = smax(d, -socket, k * 0.8);
    double eyeball = sdSphere(vsub(qm, eye_c), R * p->eye_size);
    d = smin(d, eyeball, k * 0.4);
    /* upper lid: a thin fold draped over the top of the eyeball */
    if (p->lid > 0.01) {
        double lidc = eyey + R * p->eye_size * (1.0 - p->lid);
        double lid = sdEllipsoid(vsub(qm, v3(eye_c.x, lidc, eye_c.z + R * 0.02)),
                                 v3(R * (p->eye_size + 0.03), R * p->eye_size * p->lid, R * 0.10));
        d = smin(d, lid, k * 0.5);
    }

    /* lips: upper + lower ellipsoid, corners lifted by smile */
    double smile_y = p->smile * (ax / (R * p->mouth_w + 1e-3));
    smile_y = smile_y * smile_y * (p->smile >= 0 ? 1.0 : -1.0) * R * 0.12;
    double upper = sdEllipsoid(vsub(qm, v3(0.0, R * p->mouth_y + R * 0.04 + smile_y, R * 0.52)),
                               v3(R * p->mouth_w, R * (0.035 + p->lip * 0.5), R * 0.10));
    double lower = sdEllipsoid(vsub(qm, v3(0.0, R * p->mouth_y - R * 0.04 + smile_y, R * 0.52)),
                               v3(R * p->mouth_w * 0.95, R * (0.045 + p->lip * 0.6), R * 0.11));
    d = smin(d, smin(upper, lower, k * 0.6), k * 0.7);

    /* ears */
    if (p->ear > 0.01) {
        double ear = sdEllipsoid(vsub(qm, v3(R * (p->temple * 0.96), R * p->ear_y, R * 0.00)),
                                 v3(R * 0.07, R * p->ear, R * p->ear * 0.7));
        d = smin(d, ear, k * 1.0);
    }
    return d;
}

/* ── material classification ─────────────────────────────────────────────── */
int face_sdf_material(double X, double Y, double Z, const face_params_t *p) {
    face_params_t dp;
    if (!p) { face_params_default(&dp); p = &dp; }
    double R = p->skull_r;
    double ax = fabs(X);
    V3 qm = v3(ax, Y, Z);

    /* eyes */
    double eyey = R * (p->eye_y + 0.02);
    V3 eye_c = v3(R * p->eye_sep, eyey, R * (0.50 - p->eye_depth));
    double de = vlen(vsub(qm, eye_c));
    if (de < R * (p->eye_size + 0.02)) {
        if (de < R * p->eye_size * 0.34) return FACE_MAT_PUPIL;
        if (de < R * p->eye_size * 0.62) return FACE_MAT_IRIS;
        return FACE_MAT_EYE_WHITE;
    }

    /* lips */
    double lipdx = ax / (R * p->mouth_w + 1e-3);
    if (lipdx < 1.05 && fabs(Y - R * p->mouth_y) < R * 0.11 && Z > R * 0.40)
        return FACE_MAT_LIP;

    /* nostrils */
    double nd = vlen(vsub(qm, v3(R * (0.06 + p->nose_w * 0.5), R * (p->nose_y - 0.08),
                                 R * (0.60 + p->nose_len * 0.4))));
    if (nd < R * 0.05) return FACE_MAT_NOSTRIL;

    /* brow */
    if (Y > R * (p->brow_y - 0.03) && Y < R * (p->brow_y + 0.10) && ax < R * 0.42 && Z > R * 0.45)
        return FACE_MAT_BROW;

    return FACE_MAT_SKIN;
}

void face_sdf_albedo(int material, double *r, double *g, double *b) {
    double cr = 0.86, cg = 0.66, cb = 0.54;     /* skin */
    switch (material) {
        case FACE_MAT_LIP:       cr = 0.72; cg = 0.34; cb = 0.34; break;
        case FACE_MAT_EYE_WHITE: cr = 0.93; cg = 0.93; cb = 0.92; break;
        case FACE_MAT_IRIS:      cr = 0.35; cg = 0.45; cb = 0.55; break;
        case FACE_MAT_PUPIL:     cr = 0.04; cg = 0.04; cb = 0.04; break;
        case FACE_MAT_BROW:      cr = 0.25; cg = 0.18; cb = 0.14; break;
        case FACE_MAT_NOSTRIL:   cr = 0.30; cg = 0.22; cb = 0.20; break;
        default: break;
    }
    if (r) *r = cr; if (g) *g = cg; if (b) *b = cb;
}
