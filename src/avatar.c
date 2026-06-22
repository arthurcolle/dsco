/* ── avatar.c: procedural 3-D avatar — face & full body ─────────────────────
 * An SDF humanoid (detailed head: sockets+eyeballs+iris+pupil, eyelids that
 * blink, nostrils, upper/lower lips with a seam, cheekbones, brows, ears, hair
 * shell, optional glasses; plus neck, torso, arms, hands, legs for full-body
 * views) sphere-traced onto the Braille subpixel grid and shaded per material.
 *
 * Resolution scales three ways: canvas size (W×H cells → 2W×4H dots), an
 * anti-alias factor (aa² rays/subpixel), and zoom (narrows the FOV to magnify).
 * View modes frame the camera on the face, bust, or full body.
 *
 * All proportions/colors derive from a name via FNV-1a + mulberry32 (pets.c
 * scheme), so a name is a stable, recognizable character. */

#include "avatar.h"
#include "anim.h"
#include "tui.h"
#include "json_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

/* ── vec3 ────────────────────────────────────────────────────────────────── */
typedef struct { double x, y, z; } V3;
static inline V3 v3(double x,double y,double z){ V3 r={x,y,z}; return r; }
static inline V3 v3sub(V3 a,V3 b){ return v3(a.x-b.x,a.y-b.y,a.z-b.z); }
static inline V3 v3add(V3 a,V3 b){ return v3(a.x+b.x,a.y+b.y,a.z+b.z); }
static inline V3 v3scl(V3 a,double s){ return v3(a.x*s,a.y*s,a.z*s); }
static inline double v3dot(V3 a,V3 b){ return a.x*b.x+a.y*b.y+a.z*b.z; }
static inline V3 v3cross(V3 a,V3 b){ return v3(a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x); }
static inline double v3len(V3 a){ return sqrt(v3dot(a,a)); }
static inline V3 v3norm(V3 a){ double l=v3len(a); return l>1e-12?v3scl(a,1.0/l):a; }
static inline double clampd(double x,double lo,double hi){ return x<lo?lo:(x>hi?hi:x); }

/* ── materials + tone ramps (256-color, shadow→highlight) ────────────────── */
enum { M_BG=0, M_SKIN, M_EYE, M_IRIS, M_PUPIL, M_HAIR, M_BROW, M_LIP,
       M_GLASS, M_SHIRT, M_PANTS, M_NMAT };

static const int SKIN[5][6] = {
    { 95,131,167,174,180,224},   /* 0 pale   */
    { 94,130,173,179,216,223},   /* 1 light  */
    { 58, 94,130,137,173,180},   /* 2 tan    */
    { 52, 88, 94,130,136,173},   /* 3 brown  */
    {233, 52, 88, 94,130,137},   /* 4 deep   */
};
static const int EYEW[]  = {243,247,250,253,255,231};
static const int IRIS[4][5] = {
    { 58, 94,130,136,179},        /* brown */
    { 17, 19, 26, 39, 75},        /* blue  */
    { 22, 28, 34, 70,118},        /* green */
    { 94,100,136,142,180},        /* hazel */
};
static const int PUP[]   = {16,16,232,233};
static const int HAIRC[5][5] = {
    {232,233,235,238,240},        /* black  */
    { 52, 58, 94,130,137},        /* brown  */
    { 94,136,178,221,229},        /* blonde */
    { 52, 88,130,166,208},        /* ginger */
    {238,242,246,250,253},        /* grey   */
};
static const int LIPS[]  = {52,88,124,131,168,174};
static const int GLASSC[]= {16,16,238,240};
static const int SHIRTC[4][5] = {
    { 17, 18, 25, 32, 39},        /* blue  */
    { 22, 28, 34, 40, 78},        /* green */
    { 52, 88,124,160,196},        /* red   */
    {235,238,240,243,248},        /* grey  */
};
static const int PANTSC[2][4] = {
    { 17, 18, 24, 25},            /* denim */
    {234,236,238,240},            /* dark  */
};

static int ramp_pick(const int *r, int n, double t) {
    if (t < 0) t = 0; if (t > 1) t = 1;
    int i = (int)(t * (n - 1) + 0.5);
    if (i < 0) i = 0; if (i >= n) i = n - 1;
    return r[i];
}

/* ── face / body parameters ──────────────────────────────────────────────── */
typedef struct {
    double hw, hh, hd;                  /* head radii */
    double eye_sep, eye_y, eye_size;
    double nose_len, nose_w;
    double mouth_w, mouth_y;
    double brow_y, ear_size;
    int    skin, iris, hair, haircol, shirt, pants;
    bool   glasses;
    double blink;                       /* 1 open, 0 shut */
    double iris_dx, iris_dy;            /* gaze offset ±0.04 */
    double chin_d;                      /* chin cleft 0=none, 0.5=strong */
    double philtrum_d;                  /* philtrum groove depth */
    double stubble;                     /* beard stubble 0-1 */
    double lower_lid;                   /* lower eyelid puff 0-1 */
    double mouth_open;                  /* jaw/open-mouth expression 0-1 */
    double smile;                       /* smile expression 0-1 */
    double brow_raise;                  /* brow raise expression 0-1 */

    int    view;                        /* 0 face, 1 bust, 2 full */
    double zoom;
    int    aa;                          /* anti-alias samples per axis */
    double yaw, pitch;
} aface_t;

/* FNV-1a — consolidated into fnv1a_32() in crypto.h */
#include "crypto.h"
#define fnv1a(s) fnv1a_32(s)
static uint32_t mul32(uint32_t *st){ uint32_t z=(*st+=0x6D2B79F5u); z=(z^(z>>15))*(z|1u); z^=z+(z^(z>>7))*(z|61u); return z^(z>>14); }
static double mulf(uint32_t *st){ return mul32(st)/4294967296.0; }
static double mrange(uint32_t *st,double lo,double hi){ return lo+(hi-lo)*mulf(st); }

static aface_t aface_from_name(const char *name) {
    uint32_t st = fnv1a(name && name[0] ? name : "anon");
    aface_t f;
    f.hw = mrange(&st,0.58,0.66); f.hh = mrange(&st,0.72,0.82); f.hd = mrange(&st,0.56,0.64);
    f.eye_sep = mrange(&st,0.23,0.29); f.eye_y = mrange(&st,0.06,0.13); f.eye_size = mrange(&st,0.135,0.165);
    f.nose_len = mrange(&st,0.10,0.16); f.nose_w = mrange(&st,0.09,0.12);
    f.mouth_w = mrange(&st,0.18,0.25); f.mouth_y = mrange(&st,-0.34,-0.27);
    f.brow_y = f.eye_y + mrange(&st,0.15,0.20); f.ear_size = mrange(&st,0.14,0.18);
    f.iris_dx=0.0; f.iris_dy=0.0;
    f.chin_d   = mrange(&st,0.0,0.45);
    f.philtrum_d = mrange(&st,0.018,0.030);
    f.stubble  = 0.0;
    f.lower_lid = mrange(&st,0.15,0.35);
    f.mouth_open = 0.0;
    f.smile = 0.0;
    f.brow_raise = 0.0;

    f.skin    = (int)(mulf(&st)*5.0); if(f.skin>4)f.skin=4;
    f.iris    = (int)(mulf(&st)*4.0); if(f.iris>3)f.iris=3;
    f.hair    = (int)(mulf(&st)*5.0); if(f.hair>4)f.hair=4;
    f.haircol = (int)(mulf(&st)*5.0); if(f.haircol>4)f.haircol=4;
    f.shirt   = (int)(mulf(&st)*4.0); if(f.shirt>3)f.shirt=3;
    f.pants   = (int)(mulf(&st)*2.0); if(f.pants>1)f.pants=1;
    f.glasses = mulf(&st) < 0.28;
    f.blink = 1.0; f.view = 0; f.zoom = 1.0; f.aa = 2; f.yaw = 0.0; f.pitch = 0.0;
    return f;
}

/* ── SDF primitives ──────────────────────────────────────────────────────── */
static double sdEll(V3 p, V3 r){
    double k0=v3len(v3(p.x/r.x,p.y/r.y,p.z/r.z));
    double k1=v3len(v3(p.x/(r.x*r.x),p.y/(r.y*r.y),p.z/(r.z*r.z)));
    return k0*(k0-1.0)/(k1>1e-9?k1:1e-9);
}
static double sdSph(V3 p,double s){ return v3len(p)-s; }
static double sdRBox(V3 p,V3 b,double r){
    double qx=fabs(p.x)-b.x,qy=fabs(p.y)-b.y,qz=fabs(p.z)-b.z;
    double ax=fmax(qx,0),ay=fmax(qy,0),az=fmax(qz,0);
    return sqrt(ax*ax+ay*ay+az*az)+fmin(fmax(qx,fmax(qy,qz)),0.0)-r;
}
static double sdCap(V3 p,V3 a,V3 b,double r){
    V3 pa=v3sub(p,a), ba=v3sub(b,a);
    double h=clampd(v3dot(pa,ba)/v3dot(ba,ba),0,1);
    return v3len(v3sub(pa,v3scl(ba,h)))-r;
}
static double smin(double a,double b,double k){
    double h=fmax(k-fabs(a-b),0.0)/k; return fmin(a,b)-h*h*k*0.25;
}
static void opU(double *d,int *mat,double nd,int nm){ if(nd<*d){*d=nd;*mat=nm;} }

/* ── detailed head — v3 complete rewrite ────────────────────────────────── */
static double sdHead(V3 p, const aface_t *f, int *mat) {
    double open  = clampd(f->mouth_open, 0.0, 1.0);
    double smile = clampd(f->smile, 0.0, 1.0);
    double brow_lift = clampd(f->brow_raise, 0.0, 1.0) * 0.075;

    /* ── cranium: compound ellipsoid  ─────────────────────────────────── */
    double cran  = sdEll(v3sub(p, v3(0, 0.08, 0.0)),  v3(f->hw, f->hh*0.78, f->hd));
    double mid   = sdEll(v3sub(p, v3(0,-0.12, 0.02)), v3(f->hw*0.90, f->hh*0.60, f->hd*0.96));
    double jaw   = sdEll(v3sub(p, v3(0,-0.50, 0.06)), v3(f->hw*0.58, 0.38, f->hd*0.80));
    double skin  = smin(smin(cran, mid, 0.22), jaw, 0.20);

    /* cheekbones */
    for (int s=-1;s<=1;s+=2) {
        double ck = sdEll(v3sub(p, v3(s*f->hw*0.74, -0.08, f->hd*0.72)), v3(0.16,0.13,0.11));
        skin = smin(skin, ck, 0.14);
    }

    /* temples concavity */
    for (int s=-1;s<=1;s+=2) {
        double tmp = sdEll(v3sub(p, v3(s*f->hw*0.94, 0.20, f->hd*0.55)), v3(0.10,0.22,0.18));
        skin = fmax(skin, -tmp + 0.020);
    }

    /* frontal eminences — paired forehead fullness */
    for (int s=-1;s<=1;s+=2) {
        double fem = sdEll(v3sub(p, v3(s*f->hw*0.34, f->hh*0.42, f->hd*0.74)), v3(0.20,0.18,0.10));
        skin = smin(skin, fem, 0.12);
    }
    /* glabella — supraorbital bridge between the brows */
    double glab = sdEll(v3sub(p, v3(0, f->brow_y+brow_lift*0.35+0.010, f->hd*0.860)), v3(f->nose_w*0.55,0.055,0.045));
    skin = smin(skin, glab, 0.030);

    /* ears: helix shell + antihelix + concha + lobe */
    for (int s=-1;s<=1;s+=2) {
        V3 eo = v3(s*(f->hw + 0.005), -0.02, -0.03);
        double helix = fabs(sdEll(v3sub(p, eo), v3(0.058,f->ear_size,f->ear_size*0.88))) - 0.014;
        double antih = fabs(sdEll(v3sub(p, v3(eo.x-s*0.012,eo.y+0.04,eo.z+0.022)),
                                  v3(0.028,f->ear_size*0.60,f->ear_size*0.50))) - 0.008;
        double bowl  = sdEll(v3sub(p, v3(eo.x-s*0.008,eo.y,eo.z+0.018)),
                             v3(0.032,f->ear_size*0.52,f->ear_size*0.40));
        double ear   = fmax(smin(helix,antih,0.018), -bowl);
        double lobe  = sdEll(v3sub(p, v3(eo.x,eo.y-f->ear_size*0.82,eo.z+0.008)),
                             v3(0.040,0.032,0.028));
        double trag  = sdEll(v3sub(p, v3(eo.x-s*0.030,eo.y-0.006,eo.z+0.034)),
                             v3(0.014,0.024,0.018));   /* tragus over the canal */
        skin = smin(skin, smin(smin(ear,lobe,0.016),trag,0.012), 0.030);
    }

    /* nose: bridge ridge + lateral walls + tip + columella */
    double br_ridge = sdCap(p, v3(0,0.22,f->hd*0.890), v3(0,-0.040,f->hd*0.960), f->nose_w*0.32);
    for (int s=-1;s<=1;s+=2) {
        double lat = sdEll(v3sub(p, v3(s*f->nose_w*0.55,-0.04,f->hd*0.930)),
                           v3(f->nose_w*0.38,0.14,f->nose_len*0.55));
        skin = smin(skin, lat, 0.030);
    }
    double ntip  = sdEll(v3sub(p, v3(0,-0.145,f->hd*0.970)), v3(f->nose_w*0.80,f->nose_len*0.42,f->nose_len*0.60));
    double colba = sdEll(v3sub(p, v3(0,-0.180,f->hd*0.945)), v3(f->nose_w*0.35,0.030,0.030));
    double col   = sdCap(p, v3(0,-0.155,f->hd*0.920), v3(0,-0.250,f->hd*0.870), f->nose_w*0.18);
    double nose  = smin(smin(br_ridge,smin(ntip,colba,0.022),0.030), col, 0.018);
    skin = smin(skin, nose, 0.028);

    /* alar wings + alar crease */
    for (int s=-1;s<=1;s+=2) {
        double crease = sdCap(p, v3(s*f->nose_w*0.72,-0.105,f->hd*0.920),
                                 v3(s*f->nose_w*1.00,-0.210,f->hd*0.900), 0.012);
        skin = fmax(skin, -crease + 0.004);
        double aw = sdEll(v3sub(p, v3(s*f->nose_w*0.90,-0.175,f->hd*0.945)), v3(0.052,0.042,0.048));
        skin = smin(skin, aw, 0.024);
    }

    /* nostrils */
    for (int s=-1;s<=1;s+=2) {
        double nos = sdEll(v3sub(p, v3(s*f->nose_w*0.58,-0.222,f->hd*0.930)), v3(0.024,0.018,0.022));
        skin = fmax(skin, -nos);
    }

    /* eye sockets — precise oval, orbital brow bone */
    for (int s=-1;s<=1;s+=2) {
        double soc = sdEll(v3sub(p, v3(s*f->eye_sep,f->eye_y+0.010,f->hd*0.835)),
                           v3(f->eye_size*1.12,f->eye_size*0.86,0.080));
        skin = fmax(skin, -soc - 0.026);
        double bbone = sdEll(v3sub(p, v3(s*f->eye_sep,f->eye_y+f->eye_size*0.88,f->hd*0.820)),
                             v3(f->eye_size*1.18,0.040,0.055));
        skin = smin(skin, bbone, 0.028);
    }

    double d = skin; *mat = M_SKIN;

    /* eyeballs — placed at socket, sclera + iris + pupil + cornea */
    double ez = f->hd * 0.700;
    for (int s=-1;s<=1;s+=2) {
        V3 ec = v3(s*f->eye_sep, f->eye_y, ez);
        double sclera = sdEll(v3sub(p,ec), v3(f->eye_size*0.98,f->eye_size*0.92,f->eye_size*0.95));
        opU(&d, mat, sclera, M_EYE);
        double fz = ez + f->eye_size*0.82;
        V3 ic = v3(s*f->eye_sep+s*f->iris_dx, f->eye_y+f->iris_dy, fz);
        opU(&d, mat, sdEll(v3sub(p,ic), v3(f->eye_size*0.52,f->eye_size*0.52,f->eye_size*0.12)), M_IRIS);
        opU(&d, mat, sdEll(v3sub(p,v3(ic.x,ic.y,ic.z+0.008)), v3(f->eye_size*0.24,f->eye_size*0.24,f->eye_size*0.08)), M_PUPIL);
        double cornea = sdEll(v3sub(p, v3(ec.x,ec.y,fz+f->eye_size*0.06)),
                              v3(f->eye_size*0.62,f->eye_size*0.56,f->eye_size*0.20));
        opU(&d, mat, cornea, M_EYE);

        /* upper eyelid */
        double lidY = f->eye_y + f->eye_size*(0.62 - (1.0-f->blink)*1.55);
        double upLid = sdEll(v3sub(p, v3(s*f->eye_sep,lidY,ez+f->eye_size*0.28)),
                             v3(f->eye_size*1.22,f->eye_size*0.90,f->eye_size*1.18));
        opU(&d, mat, upLid, M_SKIN);

        /* eyelashes — dark line along the upper lid margin, follows the blink */
        double lashY = f->eye_y + f->eye_size*(0.46 - (1.0-f->blink)*1.50);
        V3 lA = v3(s*(f->eye_sep - f->eye_size*1.02), lashY,        ez+f->eye_size*0.96);
        V3 lB = v3(s*(f->eye_sep + f->eye_size*1.00), lashY+0.006,  ez+f->eye_size*0.80);
        opU(&d, mat, sdCap(p, lA, lB, 0.0075), M_BROW);

        /* lid crease */
        if (f->blink > 0.4) {
            V3 crA = v3(s*(f->eye_sep-f->eye_size*0.92), f->eye_y+f->eye_size*0.70, ez+f->eye_size*0.38);
            V3 crB = v3(s*(f->eye_sep+f->eye_size*0.88), f->eye_y+f->eye_size*0.60, ez+f->eye_size*0.35);
            double crvol = sdEll(v3sub(p, v3(s*f->eye_sep,f->eye_y+f->eye_size*0.65,ez+f->eye_size*0.35)),
                                 v3(f->eye_size*0.90,0.032,0.040));
            skin = fmax(skin, -sdCap(p,crA,crB,0.010) - 0.002);
            skin = fmax(skin, -crvol - 0.004);
        }

        /* lower eyelid */
        double lowY = f->eye_y - f->eye_size*(0.66+f->lower_lid*0.22);
        opU(&d, mat, sdEll(v3sub(p, v3(s*f->eye_sep,lowY,ez+f->eye_size*0.22)),
                           v3(f->eye_size*1.10,f->eye_size*0.28,f->eye_size*0.82)), M_SKIN);

        /* medial canthus */
        opU(&d, mat, sdSph(v3sub(p, v3(s*(f->eye_sep-f->eye_size*0.92),f->eye_y-0.008,ez+f->eye_size*0.35)), 0.018), M_EYE);

        /* brow: medial head + lateral tail */
        V3 bm0 = v3(s*(f->eye_sep-f->eye_size*1.0),  f->brow_y+brow_lift+0.004, f->hd*0.825);
        V3 bm1 = v3(s*(f->eye_sep+f->eye_size*0.15), f->brow_y+brow_lift+0.012, f->hd*0.822);
        V3 bl1 = v3(s*(f->eye_sep+f->eye_size*0.95), f->brow_y+brow_lift+0.025+smile*0.010, f->hd*0.815);
        double brow = smin(sdCap(p,bm0,bm1,0.030), sdCap(p,bm1,bl1,0.018), 0.012);
        opU(&d, mat, brow, M_BROW);
    }

    /* lips: Cupid's bow upper + full lower + commissures + seam */
    double mw = f->mouth_w * (1.0 + smile*0.16);
    double ulip_c = sdEll(v3sub(p,v3(0,f->mouth_y+0.062+smile*0.014,f->hd*0.887)), v3(mw*0.28,0.036,0.052));
    double ulip_l = sdEll(v3sub(p,v3(-mw*0.48,f->mouth_y+0.048+smile*0.026,f->hd*0.882)), v3(mw*0.30,0.033,0.048));
    double ulip_r = sdEll(v3sub(p,v3( mw*0.48,f->mouth_y+0.048+smile*0.026,f->hd*0.882)), v3(mw*0.30,0.033,0.048));
    double upperlip = smin(smin(ulip_l,ulip_r,0.022), ulip_c, 0.028);
    for (int s=-1;s<=1;s+=2) {
        double peak = sdSph(v3sub(p, v3(s*mw*0.22,f->mouth_y+0.088+smile*0.010,f->hd*0.888)), 0.024);
        upperlip = smin(upperlip, peak, 0.018);
    }
    double lowerlip = sdEll(v3sub(p,v3(0,f->mouth_y-0.052-open*0.050,f->hd*0.876)), v3(mw*0.78,0.052+open*0.014,0.058));
    double lips = smin(upperlip, lowerlip, 0.018);
    for (int s=-1;s<=1;s+=2) {
        double comm = sdSph(v3sub(p, v3(s*mw*0.90,f->mouth_y+0.006+smile*0.038,f->hd*0.878)), 0.020);
        lips = smin(lips, comm, 0.014);
    }
    lips = fmax(lips, -sdCap(p, v3(-mw*0.88,f->mouth_y+0.008-open*0.012,f->hd*0.895),
                                  v3( mw*0.88,f->mouth_y+0.008-open*0.012,f->hd*0.895), 0.007+open*0.020));
    opU(&d, mat, lips, M_LIP);
    if (open > 0.015) {
        double mouth_dark = sdEll(v3sub(p, v3(0,f->mouth_y-0.012-open*0.030,f->hd*0.912)),
                                  v3(mw*0.76,0.012+open*0.046,0.030));
        opU(&d, mat, mouth_dark, M_PUPIL);
    }

    /* nasolabial folds */
    for (int s=-1;s<=1;s+=2) {
        double fold = sdCap(p, v3(s*f->nose_w*0.96,-0.085,f->hd*0.895),
                               v3(s*mw*0.94,f->mouth_y+0.040+smile*0.018,f->hd*0.870), 0.010);
        skin = fmax(skin, -fold - 0.001);
    }

    /* philtrum ridges + centre groove */
    for (int s=-1;s<=1;s+=2) {
        double pr = sdCap(p, v3(s*f->nose_w*0.22,-0.175,f->hd*0.900),
                             v3(s*mw*0.20,f->mouth_y+0.085,f->hd*0.892), 0.009);
        skin = smin(skin, pr, 0.012);
    }
    skin = fmax(skin, -sdCap(p, v3(0,-0.168,f->hd*0.893),
                                  v3(0,f->mouth_y+0.082,f->hd*0.884), f->philtrum_d) + 0.003);

    /* mentolabial sulcus — groove between the lower lip and the chin */
    skin = fmax(skin, -sdCap(p, v3(-mw*0.62,f->mouth_y-0.105-open*0.020,f->hd*0.860),
                                  v3( mw*0.62,f->mouth_y-0.105-open*0.020,f->hd*0.860), 0.020) - 0.002);

    /* chin cleft */
    if (f->chin_d > 0.04) {
        skin = fmax(skin, -sdCap(p, v3(0,-0.530,f->hd*0.790),
                                      v3(0,-0.455,f->hd*0.820), f->chin_d*0.055));
    }
    opU(&d, mat, skin, M_SKIN);

    /* hair */
    if (f->hair > 0) {
        double lift  = (f->hair==2)?0.18:0.06;
        double grow  = (f->hair==2)?1.22:1.065;
        double thick = (f->hair==2)?0.035:0.022;
        double shell = fabs(sdEll(v3sub(p,v3(0,lift,-0.02)), v3(f->hw*grow,f->hh*grow,f->hd*grow))) - thick;
        double fopen = sdEll(v3sub(p,v3(0,-0.02,f->hd*0.52)), v3(f->hw*0.90,f->hh*0.72,f->hd*0.92));
        double hair_d = fmax(shell, -fopen);
        if (f->hair==1) hair_d = fmax(hair_d, -(p.y+0.02-f->hh*0.19));
        if (f->hair==4) hair_d = fmax(hair_d, -(p.y+0.12-f->hh*0.22));
        for (int s=-1;s<=1;s+=2) {
            double rec = sdEll(v3sub(p,v3(s*f->hw*0.72,f->hh*0.52,f->hd*0.70)), v3(0.18,0.14,0.12));
            hair_d = fmax(hair_d, -rec+0.010);
        }
        opU(&d, mat, hair_d, M_HAIR);
    }

    /* glasses */
    if (f->glasses) {
        double fzg = f->hd*0.845;
        for (int s=-1;s<=1;s+=2) {
            double frame = fabs(sdRBox(v3sub(p,v3(s*f->eye_sep,f->eye_y,fzg)),
                                      v3(f->eye_size*1.08,f->eye_size*0.88,0.018),0.035))-0.011;
            opU(&d, mat, frame, M_GLASS);
            opU(&d, mat, sdEll(v3sub(p,v3(s*f->nose_w*1.0,f->eye_y-f->eye_size*0.55,fzg-0.012)),
                               v3(0.014,0.022,0.012)), M_GLASS);
            V3 ta=v3(s*(f->eye_sep+f->eye_size*1.05),f->eye_y+0.008,fzg);
            V3 tb=v3(s*f->hw*0.98,f->eye_y-0.010,fzg-0.30);
            opU(&d, mat, sdCap(p,ta,tb,0.007), M_GLASS);
        }
        opU(&d, mat, sdCap(p, v3(-f->eye_sep*0.60,f->eye_y+0.010,fzg),
                               v3( f->eye_sep*0.60,f->eye_y+0.010,fzg), 0.009), M_GLASS);
    }
    return d;
}
/* ── body (neck, torso, arms, hands, legs) ───────────────────────────────── */
static double sdBody(V3 p, const aface_t *f, int *mat) {
    int m = M_SKIN;
    double neck = sdCap(p, v3(0,-f->hh*0.78,0.0), v3(0,-f->hh-0.28,-0.03), 0.20);
    double d = neck; *mat = m;

    double sh = -f->hh-0.34;                              /* shoulder line */
    double chest = sdEll(v3sub(p, v3(0, sh-0.55, 0)), v3(0.92,0.62,0.50));
    double abd   = sdEll(v3sub(p, v3(0, sh-1.35, 0)), v3(0.78,0.62,0.46));
    double torso = smin(chest, abd, 0.30);
    for (int s=-1;s<=1;s+=2)
        torso = smin(torso, sdSph(v3sub(p, v3(s*0.86, sh, 0)), 0.34), 0.25);   /* shoulders */
    opU(&d, mat, torso, M_SHIRT);

    for (int s=-1;s<=1;s+=2) {
        V3 shoulder = v3(s*0.92, sh-0.02, 0);
        V3 elbow    = v3(s*1.06, sh-0.95, 0.05);
        V3 wrist    = v3(s*0.96, sh-1.78, 0.12);
        opU(&d, mat, sdCap(p, shoulder, elbow, 0.18), M_SHIRT);      /* sleeve */
        opU(&d, mat, sdCap(p, elbow, wrist, 0.135), M_SKIN);         /* forearm */
        opU(&d, mat, sdSph(v3sub(p, wrist), 0.16), M_SKIN);          /* hand */
    }

    double pelvis = sdEll(v3sub(p, v3(0, sh-1.95, 0)), v3(0.74,0.42,0.46));
    opU(&d, mat, pelvis, M_PANTS);
    for (int s=-1;s<=1;s+=2) {
        V3 hip   = v3(s*0.36, sh-2.15, 0);
        V3 knee  = v3(s*0.40, sh-3.15, 0.05);
        V3 ankle = v3(s*0.38, sh-4.05, 0.0);
        opU(&d, mat, sdCap(p, hip, knee, 0.22), M_PANTS);
        opU(&d, mat, sdCap(p, knee, ankle, 0.165), M_PANTS);
        opU(&d, mat, sdRBox(v3sub(p, v3(s*0.38, sh-4.12, 0.12)), v3(0.13,0.06,0.22),0.04), M_SKIN); /* foot */
    }
    return d;
}

static double sdScene(V3 p, const aface_t *f, int *mat) {
    int mh, mb;
    double dh = sdHead(p, f, &mh);
    if (f->view == 0) { *mat = mh; return dh; }     /* face: head only (faster) */
    double db = sdBody(p, f, &mb);
    if (db < dh) { *mat = mb; return db; }
    *mat = mh; return dh;
}

/* ── shading per material ────────────────────────────────────────────────── */
static int shade_mat(int mat, double lum, const aface_t *f) {
    switch (mat) {
        case M_SKIN:  return ramp_pick(SKIN[f->skin], 6, lum);
        case M_EYE:   return ramp_pick(EYEW, 6, 0.4+0.6*lum);
        case M_IRIS:  return ramp_pick(IRIS[f->iris], 5, lum);
        case M_PUPIL: return ramp_pick(PUP, 4, lum);
        case M_HAIR:  return ramp_pick(HAIRC[f->haircol], 5, lum);
        case M_BROW:  return ramp_pick(HAIRC[f->haircol], 5, lum*0.6);
        case M_LIP:   return ramp_pick(LIPS, 6, lum);
        case M_GLASS: return ramp_pick(GLASSC, 4, lum);
        case M_SHIRT: return ramp_pick(SHIRTC[f->shirt], 5, lum);
        case M_PANTS: return ramp_pick(PANTSC[f->pants], 4, lum);
        default:      return -1;
    }
}

static int mat_priority(int mat) {
    switch (mat) {
        case M_PUPIL: return 9;
        case M_IRIS:  return 8;
        case M_EYE:   return 7;
        case M_LIP:   return 6;
        case M_BROW:  return 5;
        case M_GLASS: return 4;
        case M_HAIR:  return 3;
        case M_SKIN:  return 2;
        default:      return 1;
    }
}

static void braille_clear_dot(tui_braille_t *b, int x, int y) {
    static const unsigned char bit[4][2] = {
        {0x01,0x08},{0x02,0x10},{0x04,0x20},{0x40,0x80}
    };
    if (!b || !b->cells) return;
    if (x < 0 || y < 0 || x >= b->px_w || y >= b->px_h) return;
    int cx = x / 2, cy = y / 4;
    b->cells[cy * b->w_cells + cx] &= (unsigned char)~bit[y % 4][x % 2];
}

static void braille_clear_disk(tui_braille_t *b, int x, int y, int r) {
    for (int yy=-r; yy<=r; yy++) {
        for (int xx=-r; xx<=r; xx++) {
            if (xx*xx + yy*yy <= r*r) braille_clear_dot(b, x+xx, y+yy);
        }
    }
}

static void braille_clear_line(tui_braille_t *b, int x0, int y0, int x1, int y1, int r) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        braille_clear_disk(b, x0, y0, r);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static bool project_px(V3 p, V3 ro, V3 fwd, V3 rgt, V3 up,
                       double tanH, double aspect, int gw, int gh,
                       int *x, int *y) {
    V3 q = v3sub(p, ro);
    double z = v3dot(q, fwd);
    if (z <= 1e-6) return false;
    double u = v3dot(q, rgt) / z;
    double v = v3dot(q, up) / z;
    double sx = (u / (aspect * tanH) + 1.0) * 0.5;
    double sy = (1.0 - v / tanH) * 0.5;
    *x = (int)(sx * gw + 0.5);
    *y = (int)(sy * gh + 0.5);
    return *x >= -4 && *x < gw+4 && *y >= -4 && *y < gh+4;
}

static void clear_projected_line(tui_braille_t *bc, V3 a, V3 b,
                                 V3 ro, V3 fwd, V3 rgt, V3 up,
                                 double tanH, double aspect, int r) {
    int x0,y0,x1,y1;
    if (project_px(a,ro,fwd,rgt,up,tanH,aspect,bc->px_w,bc->px_h,&x0,&y0) &&
        project_px(b,ro,fwd,rgt,up,tanH,aspect,bc->px_w,bc->px_h,&x1,&y1)) {
        braille_clear_line(bc,x0,y0,x1,y1,r);
    }
}

static void clear_projected_dot(tui_braille_t *bc, V3 p,
                                V3 ro, V3 fwd, V3 rgt, V3 up,
                                double tanH, double aspect, int r) {
    int x,y;
    if (project_px(p,ro,fwd,rgt,up,tanH,aspect,bc->px_w,bc->px_h,&x,&y))
        braille_clear_disk(bc,x,y,r);
}

/* cheap hash → [0,1): drives skin grain (pores) and stubble speckle */
static double hash13(V3 p){
    double h = sin(p.x*157.31 + p.y*113.97 + p.z*271.13) * 43758.5453;
    return h - floor(h);
}

/* ── ambient occlusion: sample the field along the normal; crevices (eye
 *    sockets, alar creases, under the lip/chin) self-shadow and read as form. */
static double calc_ao(V3 p, V3 n, const aface_t *f) {
    double occ = 0.0, sca = 1.0;
    for (int i = 1; i <= 3; i++) {
        double h = 0.02 + 0.11 * (i / 3.0);
        int m; double d = sdScene(v3add(p, v3scl(n, h)), f, &m);
        occ += (h - d) * sca;
        sca *= 0.75;
    }
    return clampd(1.0 - 2.4 * occ, 0.0, 1.0);
}

/* ── soft shadow: march toward the key light; the nose/brow throw a real,
 *    penumbra-softened shadow across the cheek — the cue that sells depth.
 *    The head spans ~1 unit, so a short ray with a coarse step suffices. */
static double soft_shadow(V3 ro, V3 rd, const aface_t *f, double k) {
    double res = 1.0, t = 0.025;
    for (int i = 0; i < 11; i++) {
        int m; double h = sdScene(v3add(ro, v3scl(rd, t)), f, &m);
        if (h < 0.002) return 0.0;
        res = fmin(res, k * h / t);
        t += clampd(h, 0.03, 0.22);
        if (t > 1.7) break;
    }
    return clampd(res, 0.0, 1.0);
}

/* ── one ray → (material, luminance) ─────────────────────────────────────── */
static int march(V3 ro, V3 rd, const aface_t *f, double *out_lum) {
    const int MAXS = 220; double MAXD = 18.0;
    double t = 0.01; int s, mat = M_BG;
    /* Face view is head-only: cull against a bounding sphere so background rays
     * cost nothing and grazing rays don't crawl to MAXD — the big perf lever. */
    if (f->view == 0) {
        const double R = 1.45; V3 c = v3(0,-0.05,0);
        V3 oc = v3sub(ro, c);
        double b = v3dot(oc, rd), cc = v3dot(oc,oc) - R*R;
        double disc = b*b - cc;
        if (disc <= 0.0) { *out_lum = 0; return M_BG; }   /* misses head entirely */
        double sq = sqrt(disc);
        double t0 = -b - sq, t1 = -b + sq;
        if (t1 < 0.0) { *out_lum = 0; return M_BG; }
        if (t0 > t) t = t0;            /* start at sphere entry */
        MAXD = t1;                     /* stop at sphere exit */
    }
    for (s=0;s<MAXS;s++) {
        V3 p = v3add(ro, v3scl(rd,t));
        double d = sdScene(p, f, &mat);
        if (d < 0.00035) break;
        t += d * 0.82;   /* conservative step for smooth SDFs */
        if (t > MAXD) { *out_lum = 0; return M_BG; }
    }
    if (s == MAXS) { *out_lum = 0; return M_BG; }
    V3 p = v3add(ro, v3scl(rd,t));
    /* Tetrahedron normal — 4 samples, avoids axis-aligned artefacts */
    int mm;
    const double e = 0.00040;
    V3 k0 = v3( 1,-1,-1), k1 = v3(-1,-1, 1), k2 = v3(-1, 1,-1), k3 = v3( 1, 1, 1);
    #define SP(k) sdScene(v3add(p,v3scl(k,e)),f,&mm)
    V3 n = v3norm(v3add(v3add(v3scl(k0,SP(k0)),v3scl(k1,SP(k1))),
                        v3add(v3scl(k2,SP(k2)),v3scl(k3,SP(k3)))));
    #undef SP
    /* 3-light rig: key (warm upper-right), fill (cool left), rim (back) */
    V3 Lkey  = v3norm(v3( 0.50, 0.70, 0.85));
    V3 Lfill = v3norm(v3(-0.55, 0.15, 0.65));
    V3 Lrim  = v3norm(v3( 0.20,-0.25,-0.80));
    double Dkey  = fmax(v3dot(n, Lkey), 0.0);
    double Dfill = fmax(v3dot(n, Lfill), 0.0);
    /* GGX specular — key light only */
    V3 viewd = v3norm(v3scl(rd,-1.0));
    V3 H = v3norm(v3add(Lkey, viewd));
    double ndoth = fmax(v3dot(n, H), 0.0);
    double ndotv = fmax(v3dot(n, viewd), 0.0);
    bool wet = (mat==M_EYE||mat==M_IRIS||mat==M_GLASS||mat==M_LIP);
    double alpha2 = wet ? 0.008 : 0.18;
    double denom = ndoth*ndoth*(alpha2-1.0)+1.0;
    double GGX = alpha2 / (3.14159*denom*denom + 1e-9);
    double spec = GGX * Dkey * 0.012 * (wet ? 5.0 : 0.8);
    /* corneal retro-highlight (small sharp dot offset from centre) */
    if (mat==M_EYE) {
        V3 Hcorn = v3norm(v3add(Lkey, viewd));
        spec += pow(fmax(v3dot(n,Hcorn),0.0), 120.0) * 1.8;
    }
    /* SSS wrap for skin — light bleeds around the terminator into shadow */
    double sss = 0.0;
    if (mat==M_SKIN||mat==M_LIP||mat==M_EYE) {
        double wrap = (v3dot(n, Lkey) + 0.30) / 1.30;
        sss = fmax(wrap, 0.0) * (mat==M_SKIN ? 0.16 : 0.06);
    }
    /* Fresnel rim */
    double fresnel = pow(1.0 - ndotv, 3.0);
    double rim = fresnel * fmax(v3dot(n, Lrim), 0.0) * 0.34;

    /* contact shadow + crevice occlusion: this is what carves the features */
    double ao  = calc_ao(p, n, f);
    double sh  = (Dkey > 0.0) ? soft_shadow(v3add(p, v3scl(n, 0.012)), Lkey, f, 7.0) : 1.0;

    /* hemispheric ambient, gated by occlusion so sockets/creases go dark */
    double amb = (0.085 + 0.10*(n.y*0.5+0.5)) * ao;
    /* key carries the form (shadowed), fill lifts the dark side a touch */
    double lum = amb + Dkey*0.78*sh + Dfill*0.16*ao + rim + spec*sh + sss;

    if (mat==M_SKIN) {
        /* fine grain — pores/tone variation so flat planes aren't dead */
        lum += (hash13(v3scl(p,90.0)) - 0.5) * 0.045;
        /* stubble / beard: darken + speckle the beard mask when requested */
        if (f->stubble > 0.001) {
            double below = clampd((f->mouth_y + 0.12 - p.y) / 0.34, 0.0, 1.0);   /* jaw/chin */
            double front = clampd((p.z - f->hd*0.34) / (f->hd*0.5), 0.0, 1.0);
            double sides = clampd(1.0 - fabs(p.x)/(f->hw*0.98), 0.0, 1.0);
            double mous  = (p.y>f->mouth_y+0.02 && p.y<f->mouth_y+0.14 &&
                            fabs(p.x)<f->mouth_w) ? 0.75 : 0.0;
            double beard = fmax(below*sides, mous) * front;
            double speck = hash13(v3scl(p,52.0));
            lum -= f->stubble * beard * (0.28 + 0.42*speck);
        }
    }

    *out_lum = clampd(lum,0,1);
    return mat;
}

/* ── render one frame: Braille canvas + per-cell material color ───────────── */
static void aface_frame(tui_braille_t *bc, int *cell_color, const aface_t *f) {
    const int gw=bc->px_w, gh=bc->px_h, W=bc->w_cells, H=bc->h_cells;
    tui_braille_clear(bc);
    static const int BAYER[4][4] = {{0,8,2,10},{12,4,14,6},{3,11,1,9},{15,7,13,5}};

    /* view framing + zoom */
    double ty, dist, fov;
    switch (f->view) {
        case 2: ty=-2.05; dist=8.4; fov=0.80; break;   /* full body */
        case 1: ty=-0.80; dist=4.1; fov=0.82; break;   /* bust */
        default: ty= 0.00; dist=2.5; fov=0.80; break;  /* face */
    }
    double z = f->zoom>0.05 ? f->zoom : 1.0;
    fov = 2.0*atan(tan(fov*0.5)/z);

    V3 tgt = v3(0,ty,0);
    V3 ro  = v3(dist*cos(f->pitch)*sin(f->yaw), ty + dist*sin(f->pitch), dist*cos(f->pitch)*cos(f->yaw));
    V3 fwd = v3norm(v3sub(tgt,ro));
    V3 rgt = v3norm(v3cross(v3(0,1,0),fwd));
    V3 up  = v3cross(fwd,rgt);
    double tanH = tan(fov*0.5), aspect = (double)gw/(double)gh;

    int aa = f->aa<1?1:(f->aa>6?6:f->aa);
    double *acc = calloc((size_t)W*H*M_NMAT, sizeof(double));
    int    *cnt = calloc((size_t)W*H*M_NMAT, sizeof(int));
    if (!acc||!cnt) { free(acc); free(cnt); return; }

    for (int py=0; py<gh; py++) {
        for (int px=0; px<gw; px++) {
            double lumsum=0;
            int hitcnt=0, hitmat=M_BG;
            int matcnt[M_NMAT]={0};
            double matlum[M_NMAT]={0};
            for (int sy=0; sy<aa; sy++) for (int sx=0; sx<aa; sx++) {
                double ox=(sx+0.5)/aa-0.5, oy=(sy+0.5)/aa-0.5;
                double u=(((px+0.5+ox)/gw)*2.0-1.0)*aspect*tanH;
                double v=-(((py+0.5+oy)/gh)*2.0-1.0)*tanH;
                V3 rd=v3norm(v3add(fwd, v3add(v3scl(rgt,u), v3scl(up,v))));
                double lum; int mat=march(ro,rd,f,&lum);
                if (mat!=M_BG) {
                    lumsum += lum;
                    hitcnt++;
                    matcnt[mat]++;
                    matlum[mat] += lum;
                }
            }
            int ns=aa*aa;
            if (hitcnt) {
                int bestscore=-1;
                for (int mm=1; mm<M_NMAT; mm++) {
                    if (!matcnt[mm]) continue;
                    int score = matcnt[mm]*16 + mat_priority(mm);
                    if (score > bestscore) { bestscore=score; hitmat=mm; }
                }
            }
            double cover = hitcnt ? (double)hitcnt/(double)ns : 0.0;
            double lum = hitcnt ? lumsum/(double)hitcnt : 0.0;
            double thr = (BAYER[py&3][px&3]+0.5)/16.0;
            /* Ordered dither from real hit coverage and hit-only luminance.
             * This keeps silhouettes antialiased while allowing creases, eyes
             * and the mouth to cut visible dark structure through the face. */
            double dotlum = hitcnt ? cover * (0.18 + 0.92*pow(clampd(lum,0,1), 0.86)) : 0.0;
            if (dotlum > thr) tui_braille_set(bc, px, py);

            if (hitmat!=M_BG) {
                int cell=(py/4)*W+(px/2);
                double ml = matcnt[hitmat] ? matlum[hitmat]/matcnt[hitmat] : lum;
                acc[cell*M_NMAT+hitmat]+= ml;
                cnt[cell*M_NMAT+hitmat]+= 1;
            }
            (void)lum;
        }
    }
    if (f->view <= 1) {
        int stroke = gw >= 160 ? 2 : 1;
        double open = clampd(f->mouth_open,0.0,1.0);
        double smile = clampd(f->smile,0.0,1.0);
        double brow_lift = clampd(f->brow_raise,0.0,1.0) * 0.075;
        double front = f->hd * 0.94;
        for (int s=-1; s<=1; s+=2) {
            clear_projected_line(bc,
                v3(s*(f->eye_sep-f->eye_size*0.96), f->eye_y-0.004, front),
                v3(s*(f->eye_sep+f->eye_size*0.90), f->eye_y+0.002, front),
                ro,fwd,rgt,up,tanH,aspect,stroke);
            clear_projected_line(bc,
                v3(s*(f->eye_sep-f->eye_size*0.95), f->brow_y+brow_lift+0.012, f->hd*0.93),
                v3(s*(f->eye_sep+f->eye_size*0.92), f->brow_y+brow_lift+0.030+smile*0.010, f->hd*0.91),
                ro,fwd,rgt,up,tanH,aspect,stroke);
            clear_projected_dot(bc,
                v3(s*f->nose_w*0.58, -0.222, f->hd*0.985),
                ro,fwd,rgt,up,tanH,aspect,stroke);
        }
        clear_projected_line(bc,
            v3(0, f->eye_y+0.11, f->hd*0.955),
            v3(0, -0.145, f->hd*1.010),
            ro,fwd,rgt,up,tanH,aspect,stroke);

        double mw = f->mouth_w * (1.0 + smile*0.16);
        int mouth_r = stroke + (open > 0.18 ? 1 : 0);
        clear_projected_line(bc,
            v3(-mw*0.86, f->mouth_y+0.008-open*0.015+smile*0.030, f->hd*0.975),
            v3( mw*0.86, f->mouth_y+0.008-open*0.015+smile*0.030, f->hd*0.975),
            ro,fwd,rgt,up,tanH,aspect,mouth_r);
        if (open > 0.08) {
            clear_projected_line(bc,
                v3(-mw*0.58, f->mouth_y-0.012-open*0.055, f->hd*0.980),
                v3( mw*0.58, f->mouth_y-0.012-open*0.055, f->hd*0.980),
                ro,fwd,rgt,up,tanH,aspect,mouth_r);
        }
    }
    for (int c=0;c<W*H;c++) {
        int best=M_BG, bestscore=-1;
        for (int mm=1;mm<M_NMAT;mm++) {
            int n = cnt[c*M_NMAT+mm];
            if (!n) continue;
            int score = n*16 + mat_priority(mm);
            if (score > bestscore) { bestscore=score; best=mm; }
        }
        if (best==M_BG){ cell_color[c]=-1; continue; }
        double meanlum = acc[c*M_NMAT+best]/(cnt[c*M_NMAT+best]?cnt[c*M_NMAT+best]:1);
        cell_color[c]=shade_mat(best, meanlum, f);
    }
    free(acc); free(cnt);
}

/* ── spec parsing ────────────────────────────────────────────────────────── */
static int view_of(const char *s){
    if (!s) return 0;
    if (!strcmp(s,"full")||!strcmp(s,"body")||!strcmp(s,"fullbody")) return 2;
    if (!strcmp(s,"bust")||!strcmp(s,"half")||!strcmp(s,"portrait")) return 1;
    return 0;
}
static aface_t aface_parse(const char *json, char **owned_name, char **owned_title){
    char *name=json?json_get_str(json,"name"):NULL;
    char *title=json?json_get_str(json,"title"):NULL;
    char *view=json?json_get_str(json,"view"):NULL;
    *owned_name=name; *owned_title=title;
    aface_t f=aface_from_name(name);
    if (view){ f.view=view_of(view); free(view); }
    if (json){
        f.skin=json_get_int(json,"skin",f.skin);
        f.hair=json_get_int(json,"hair",f.hair);
        f.haircol=json_get_int(json,"haircol",f.haircol);
        f.iris=json_get_int(json,"iris",f.iris);
        f.shirt=json_get_int(json,"shirt",f.shirt);
        f.pants=json_get_int(json,"pants",f.pants);
        f.glasses=json_get_bool(json,"glasses",f.glasses);
        f.zoom=json_get_double(json,"zoom",f.zoom);
        f.aa=json_get_int(json,"aa",f.aa);
        f.yaw=json_get_double(json,"yaw",f.yaw);
        f.pitch=json_get_double(json,"pitch",f.pitch);
        if (json_get_int(json,"view",-99)!=-99) f.view=json_get_int(json,"view",f.view);
        /* geometry overrides from face-capture */
        f.hw        =json_get_double(json,"hw",        f.hw);
        f.hh        =json_get_double(json,"hh",        f.hh);
        f.hd        =json_get_double(json,"hd",        f.hd);
        f.eye_sep   =json_get_double(json,"eye_sep",   f.eye_sep);
        f.eye_y     =json_get_double(json,"eye_y",     f.eye_y);
        f.eye_size  =json_get_double(json,"eye_size",  f.eye_size);
        f.nose_len  =json_get_double(json,"nose_len",  f.nose_len);
        f.nose_w    =json_get_double(json,"nose_w",    f.nose_w);
        f.mouth_w   =json_get_double(json,"mouth_w",   f.mouth_w);
        f.mouth_y   =json_get_double(json,"mouth_y",   f.mouth_y);
        f.brow_y    =json_get_double(json,"brow_y",    f.brow_y);
        f.ear_size  =json_get_double(json,"ear_size",  f.ear_size);
        f.blink     =json_get_double(json,"blink",     f.blink);
        f.iris_dx   =json_get_double(json,"iris_dx",   f.iris_dx);
        f.iris_dy   =json_get_double(json,"iris_dy",   f.iris_dy);
        f.chin_d    =json_get_double(json,"chin_d",    f.chin_d);
        f.philtrum_d=json_get_double(json,"philtrum_d",f.philtrum_d);
        f.stubble   =json_get_double(json,"stubble",   f.stubble);
        f.lower_lid =json_get_double(json,"lower_lid", f.lower_lid);
        f.mouth_open=json_get_double(json,"mouth_open",f.mouth_open);
        f.smile     =json_get_double(json,"smile",     f.smile);
        f.brow_raise=json_get_double(json,"brow_raise",f.brow_raise);

    }
    if(f.skin<0)f.skin=0; if(f.skin>4)f.skin=4;
    if(f.hair<0)f.hair=0; if(f.hair>4)f.hair=4;
    if(f.haircol<0)f.haircol=0; if(f.haircol>4)f.haircol=4;
    if(f.iris<0)f.iris=0; if(f.iris>3)f.iris=3;
    if(f.shirt<0)f.shirt=0; if(f.shirt>3)f.shirt=3;
    if(f.pants<0)f.pants=0; if(f.pants>1)f.pants=1;
    f.blink=clampd(f.blink,0.0,1.0);
    f.mouth_open=clampd(f.mouth_open,0.0,1.0);
    f.smile=clampd(f.smile,0.0,1.0);
    f.brow_raise=clampd(f.brow_raise,0.0,1.0);
    f.lower_lid=clampd(f.lower_lid,0.0,1.0);
    return f;
}

bool avatar_is_kind(const char *kind){
    return kind && (!strcmp(kind,"face")||!strcmp(kind,"avatar")||!strcmp(kind,"body")||!strcmp(kind,"portrait"));
}

int avatar_plot(char *out, size_t cap, const char *json){
    if (!out||!cap){ if(out&&cap)out[0]='\0'; return -1; }
    char *name,*title; aface_t f=aface_parse(json,&name,&title);
    int rw=json?json_get_int(json,"width",0):0;
    int rh=json?json_get_int(json,"height",0):0;
    /* full-body wants a taller default canvas */
    int dw = f.view==2 ? 44 : 48, dh = f.view==2 ? 56 : 38;
    int W,H; anim_resolve_size(rw?rw:dw, rh?rh:dh, &W,&H);
    bool color = json?json_get_bool(json,"color",true):true;

    tui_braille_t bc; tui_braille_init(&bc,W*2,H*4);
    int *cc=malloc((size_t)W*H*sizeof(int));
    if(!cc){ tui_braille_free(&bc); free(name); free(title); if(cap)out[0]='\0'; return -1; }
    aface_frame(&bc, cc, &f);

    char *mb=NULL; size_t ms=0; FILE *mf=open_memstream(&mb,&ms);
    if (mf){
        const char *t = title?title:(name&&name[0]?name:"avatar");
        if(color) fprintf(mf,"\033[1m%s\033[0m\n",t); else fprintf(mf,"%s\n",t);
        anim_paint(mf,&bc,color,cc);
        static const char *HS[]={"bald","short","afro","long","cropped"};
        static const char *VW[]={"face","bust","full"};
        if(color) fprintf(mf,"\033[2m  %s view  zoom %.1fx  skin %d  hair %s  iris %d%s\033[0m\n",
                          VW[f.view], f.zoom, f.skin, HS[f.hair], f.iris, f.glasses?"  glasses":"");
        fclose(mf);
    }
    int n = mb?(int)snprintf(out,cap,"%s",mb):-1;
    free(mb); free(cc); tui_braille_free(&bc); free(name); free(title);
    return n;
}

int avatar_anim(const char *json){
    char *name,*title; aface_t f=aface_parse(json,&name,&title);
    int rw=json?json_get_int(json,"width",0):0;
    int rh=json?json_get_int(json,"height",0):0;
    int W,H; anim_resolve_size(rw,rh,&W,&H);
    bool color=json?json_get_bool(json,"color",true):true;
    long gens=json?json_get_int(json,"gens",0):0;
    int fps=json?json_get_int(json,"fps",24):24;
    if (f.aa<1) f.aa=1;   /* animation: keep AA modest for framerate unless asked */

    tui_braille_t bc; tui_braille_init(&bc,W*2,H*4);
    int *cc=malloc((size_t)W*H*sizeof(int));
    if(!cc){ tui_braille_free(&bc); free(name); free(title); return -1; }

    bool stdin_params = json ? json_get_bool(json,"stdin_params",false) : false;
    const long budget=anim_frame_budget(gens);
    anim_term_t term; anim_begin(&term);
    long frames=0;
    char stdinbuf[4096];
    for (; budget<0||frames<budget; frames++){
        if (anim_interrupted()) break;
        if (stdin_params){
            /* read one JSON line from stdin — face_capture.py drives us */
            if (!fgets(stdinbuf, sizeof(stdinbuf), stdin)) break;
            stdinbuf[strcspn(stdinbuf,"\n")] = 0;
            if (stdinbuf[0]){
                char *_n=NULL,*_t=NULL;
                aface_t nf = aface_parse(stdinbuf,&_n,&_t);
                /* copy geometry + pose — preserve color/view/aa/zoom from CLI */
                f.hw=nf.hw; f.hh=nf.hh; f.hd=nf.hd;
                f.eye_sep=nf.eye_sep; f.eye_y=nf.eye_y; f.eye_size=nf.eye_size;
                f.nose_len=nf.nose_len; f.nose_w=nf.nose_w;
                f.mouth_w=nf.mouth_w; f.mouth_y=nf.mouth_y;
                f.brow_y=nf.brow_y; f.ear_size=nf.ear_size;
                f.blink=nf.blink; f.yaw=nf.yaw; f.pitch=nf.pitch;
                f.lower_lid=nf.lower_lid;
                f.mouth_open=nf.mouth_open; f.smile=nf.smile; f.brow_raise=nf.brow_raise;
                free(_n); free(_t);
            }
        } else {
            double ph = frames*0.05;
            f.yaw   = 0.50*sin(ph) + 0.05*sin(ph*2.7);
            f.pitch = 0.04*sin(ph*0.6) + 0.02*sin(ph*1.9);
            long cyc = frames%80;
            f.blink = (cyc<3)?(double)cyc/3.0:(cyc>77?(80.0-cyc)/3.0:1.0);
            /* saccadic iris motion: slow target drift, fast settle */
            static double sx=0,sy=0,tx=0,ty=0;
            static long nextSacc=0;
            if (frames == nextSacc) {
                uint32_t rr = (uint32_t)(frames*6271+19937);
                rr ^= rr<<13; rr ^= rr>>17; rr ^= rr<<5;
                tx = ((rr&0xFFFF)/65535.0 - 0.5)*0.038;
                ty = (((rr>>16)&0xFFFF)/65535.0 - 0.5)*0.020;
                nextSacc = frames + 35 + (long)((rr&0xFF)%50);
            }
            sx += (tx-sx)*0.12; sy += (ty-sy)*0.12;
            f.iris_dx = sx; f.iris_dy = sy;
        }
        /* breathing: subtle vertical oscillation for bust/full view */
        if (f.view >= 1) f.pitch += 0.006*sin(frames*0.022);
        (void)0;

        aface_frame(&bc,cc,&f);
        fputs("\033[H",stdout);
        printf("\033[1m%s\033[0m\033[K\n", title?title:(name&&name[0]?name:"avatar"));
        if (stdin_params)
            printf("\033[2mlive face  yaw=%+.2f  blink=%.1f  frame %-5ld  ^C to stop\033[0m\033[K\n",
                   f.yaw, f.blink, frames);
        else
            printf("\033[2myaw=%+.2f  blink=%.1f  zoom %.1fx  frame %-5ld  ^C to stop\033[0m\033[K\n",
                   f.yaw, f.blink, f.zoom, frames);
        anim_paint(stdout,&bc,color,cc);
        fflush(stdout);
        if (!anim_tick(fps)) break;
    }
    anim_end(&term);
    free(cc); tui_braille_free(&bc); free(name); free(title);
    return (int)frames;
}
