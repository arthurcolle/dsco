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

/* ── detailed head ───────────────────────────────────────────────────────── */
static double sdHead(V3 p, const aface_t *f, int *mat) {
    int m = M_SKIN;
    double head = sdEll(p, v3(f->hw, f->hh, f->hd));
    double jaw  = sdEll(v3sub(p, v3(0,-0.44,0.05)), v3(f->hw*0.68, 0.40, f->hd*0.80));
    double skin = smin(head, jaw, 0.26);
    /* cheekbones */
    for (int s=-1;s<=1;s+=2) {
        double cheek = sdEll(v3sub(p, v3(s*f->hw*0.55, -0.06, f->hd*0.66)), v3(0.20,0.16,0.14));
        skin = smin(skin, cheek, 0.16);
    }
    /* nose bridge + tip */
    double bridge = sdEll(v3sub(p, v3(0,0.04,f->hd*0.86)), v3(f->nose_w*0.7,0.20,f->nose_len*0.8));
    double tip    = sdEll(v3sub(p, v3(0,-0.13,f->hd*0.95)), v3(f->nose_w,0.10,f->nose_len));
    skin = smin(skin, smin(bridge, tip, 0.05), 0.05);
    /* alar wings — flared sides of nose base */
    for (int s=-1;s<=1;s+=2) {
        double alar = sdEll(v3sub(p, v3(s*f->nose_w*0.78,-0.15,f->hd*0.91)),
                            v3(0.055,0.038,0.040));
        skin = smin(skin, alar, 0.028);
    }

    /* ears with inner bowl */
    for (int s=-1;s<=1;s+=2) {
        double ear = sdEll(v3sub(p, v3(s*f->hw*0.99,-0.02,-0.05)), v3(0.07,f->ear_size,f->ear_size*0.9));
        double bowl= sdSph(v3sub(p, v3(s*f->hw*0.93,-0.02,0.02)), f->ear_size*0.55);
        ear = fmax(ear, -bowl);
        skin = smin(skin, ear, 0.04);
    }
    /* nostrils */
    for (int s=-1;s<=1;s+=2) {
        double nostril = sdSph(v3sub(p, v3(s*f->nose_w*0.55,-0.17,f->hd*0.93)), 0.028);
        skin = fmax(skin, -nostril);
    }
    /* eye sockets */
    for (int s=-1;s<=1;s+=2) {
        double soc = sdSph(v3sub(p, v3(s*f->eye_sep, f->eye_y, f->hd*0.88)), 0.155);
        skin = fmax(skin, -soc);
    }

    double d = skin; *mat = m;

    /* eyeballs + iris + pupil + eyelid + brow */
    double ez = f->hd*0.60;
    for (int s=-1;s<=1;s+=2) {
        V3 ec = v3(s*f->eye_sep, f->eye_y, ez);
        double white = sdEll(v3sub(p,ec), v3(f->eye_size, f->eye_size*0.85, f->eye_size));
        opU(&d, mat, white, M_EYE);
        double fz = ez + f->eye_size*0.72;
        /* iris + pupil with saccadic gaze offset */
        V3 ic = v3(s*f->eye_sep + s*f->iris_dx, f->eye_y + f->iris_dy, fz);
        opU(&d, mat, sdSph(v3sub(p,ic), f->eye_size*0.50), M_IRIS);
        opU(&d, mat, sdSph(v3sub(p,v3(ic.x,ic.y,ic.z+0.02)), f->eye_size*0.22), M_PUPIL);
        /* corneal dome — forward bubble for wet specular highlight */
        double cornea = sdEll(v3sub(p, v3(ec.x, ec.y, ez + f->eye_size*0.88)),
                              v3(f->eye_size*0.60, f->eye_size*0.54, f->eye_size*0.22));
        opU(&d, mat, cornea, M_EYE);
        /* upper eyelid: skin shell that descends as the eye blinks shut */

        double lidY = f->eye_y + f->eye_size*0.5 - (1.0-f->blink)*f->eye_size*1.7;
        double lid  = sdEll(v3sub(p, v3(s*f->eye_sep, lidY, ez+f->eye_size*0.25)),
                            v3(f->eye_size*1.18, f->eye_size*0.85, f->eye_size*1.12));
        opU(&d, mat, lid, M_SKIN);
        /* lower eyelid puff */
        double lowLidY = f->eye_y - f->eye_size*(0.55 + f->lower_lid*0.25);
        double lowLid  = sdEll(v3sub(p, v3(s*f->eye_sep, lowLidY, ez+f->eye_size*0.18)),
                               v3(f->eye_size*1.08, f->eye_size*0.22, f->eye_size*0.75));
        opU(&d, mat, lowLid, M_SKIN);
        /* arched brow — capsule for natural curve */
        V3 bA = v3(s*(f->eye_sep - f->eye_size*0.85), f->brow_y - 0.035, f->hd*0.82);
        V3 bB = v3(s*(f->eye_sep + f->eye_size*0.80), f->brow_y + 0.018, f->hd*0.815);
        double brow = sdCap(p, bA, bB, 0.026);
        opU(&d, mat, brow, M_BROW);

    }

    /* lips: upper + lower with a seam groove */
    /* philtrum groove above upper lip */
    double philcap = sdCap(p, v3(0,f->mouth_y+0.075,f->hd*0.90), v3(0,f->mouth_y+0.18,f->hd*0.87), f->philtrum_d);
    skin = fmax(skin, -philcap);
    /* chin cleft */
    if (f->chin_d > 0.05) {
        double cleft = sdCap(p, v3(0,-0.50,f->hd*0.78), v3(0,-0.42,f->hd*0.82), f->chin_d*0.06);
        skin = fmax(skin, -cleft);
    }
    double upper = sdRBox(v3sub(p, v3(0, f->mouth_y+0.035, f->hd*0.86)), v3(f->mouth_w, 0.035, 0.05), 0.02);

    double lower = sdRBox(v3sub(p, v3(0, f->mouth_y-0.045, f->hd*0.855)), v3(f->mouth_w*0.92, 0.045, 0.05), 0.025);
    double lips  = smin(upper, lower, 0.03);
    double seam  = sdRBox(v3sub(p, v3(0, f->mouth_y, f->hd*0.92)), v3(f->mouth_w*0.95, 0.006, 0.06), 0.0);
    lips = fmax(lips, -seam);
    opU(&d, mat, lips, M_LIP);

    /* hair shell with carved face opening */
    if (f->hair > 0) {
        double lift = f->hair==2 ? 0.16 : 0.07;
        double grow = f->hair==2 ? 1.18 : 1.07;
        double shell = fabs(sdEll(v3sub(p, v3(0,lift,-0.03)), v3(f->hw*grow,f->hh*grow,f->hd*grow))) - 0.025;
        double opening = sdEll(v3sub(p, v3(0,-0.04,f->hd*0.55)), v3(f->hw*0.92,f->hh*0.74,f->hd*0.9));
        double frontCut = (f->hair==3) ? 1e9 : -opening;        /* 3 = long: keep sides */
        double hair = fmax(shell, frontCut);
        if (f->hair==1) hair = fmax(hair, -(p.y + 0.05 - f->hh*0.2));   /* cropped back */
        opU(&d, mat, hair, M_HAIR);
    }

    /* glasses */
    if (f->glasses) {
        double fzg = f->hd*0.82;
        for (int s=-1;s<=1;s+=2) {
            double ring = fabs(sdRBox(v3sub(p, v3(s*f->eye_sep,f->eye_y,fzg)),
                              v3(f->eye_size*1.12,f->eye_size*0.98,0.02),0.04)) - 0.012;
            opU(&d, mat, ring, M_GLASS);
        }
        opU(&d, mat, sdRBox(v3sub(p, v3(0,f->eye_y,fzg)), v3(f->eye_sep*0.5,0.012,0.02),0.01), M_GLASS);
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

/* ── one ray → (material, luminance) ─────────────────────────────────────── */
static int march(V3 ro, V3 rd, const aface_t *f, double *out_lum) {
    const int MAXS = 160; const double MAXD = 18.0;
    double t = 0.02; int s, mat = M_BG;
    for (s=0;s<MAXS;s++) {
        V3 p = v3add(ro, v3scl(rd,t));
        double d = sdScene(p, f, &mat);
        if (d < 0.0007) break;
        t += d*0.9;
        if (t > MAXD) { *out_lum = 0; return M_BG; }
    }
    V3 p = v3add(ro, v3scl(rd,t));
    int mm; double e = 0.0009;
    V3 n = v3norm(v3(
        sdScene(v3(p.x+e,p.y,p.z),f,&mm) - sdScene(v3(p.x-e,p.y,p.z),f,&mm),
        sdScene(v3(p.x,p.y+e,p.z),f,&mm) - sdScene(v3(p.x,p.y-e,p.z),f,&mm),
        sdScene(v3(p.x,p.y,p.z+e),f,&mm) - sdScene(v3(p.x,p.y,p.z-e),f,&mm)));
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
    /* SSS wrap for skin — light bleeds around edges */
    double sss = 0.0;
    if (mat==M_SKIN||mat==M_LIP||mat==M_EYE) {
        double wrap = (v3dot(n, Lkey) + 0.35) / 1.35;
        sss = fmax(wrap, 0.0) * (mat==M_SKIN ? 0.20 : 0.08);
    }
    /* Fresnel rim */
    double fresnel = pow(1.0 - ndotv, 3.0);
    double rim = fresnel * fmax(v3dot(n, Lrim), 0.0) * 0.30;
    /* sky/ground ambient */
    double amb = 0.20 + 0.14*(n.y*0.5+0.5);
    double lum = clampd(amb + Dkey*0.62 + Dfill*0.18 + rim + spec + sss, 0.0, 1.0);

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
            double lumsum=0; int hitmat=M_BG; double bestlum=-1;
            for (int sy=0; sy<aa; sy++) for (int sx=0; sx<aa; sx++) {
                double ox=(sx+0.5)/aa-0.5, oy=(sy+0.5)/aa-0.5;
                double u=(((px+0.5+ox)/gw)*2.0-1.0)*aspect*tanH;
                double v=-(((py+0.5+oy)/gh)*2.0-1.0)*tanH;
                V3 rd=v3norm(v3add(fwd, v3add(v3scl(rgt,u), v3scl(up,v))));
                double lum; int mat=march(ro,rd,f,&lum);
                if (mat!=M_BG) { lumsum+=lum; if (lum>bestlum){bestlum=lum;hitmat=mat;} }
            }
            int ns=aa*aa;
            double cover = (bestlum>=0) ? 0.0 : 0.0;   /* placeholder */
            (void)cover;
            double lum = hitmat!=M_BG ? lumsum/ns : 0.0;
            double thr = (BAYER[py&3][px&3]+0.5)/16.0;
            /* ordered dither with gamma — maps linear lum to dot density */
            double dotlum = hitmat!=M_BG ? pow(lumsum/ns, 0.75)*1.15 : 0.0;
            if (dotlum > thr) tui_braille_set(bc, px, py);

            if (hitmat!=M_BG) {
                int cell=(py/4)*W+(px/2);
                acc[cell*M_NMAT+hitmat]+= bestlum;
                cnt[cell*M_NMAT+hitmat]+= 1;
            }
            (void)lum;
        }
    }
    for (int c=0;c<W*H;c++) {
        int best=M_BG; double bs=0;
        for (int mm=1;mm<M_NMAT;mm++) if (acc[c*M_NMAT+mm]>bs){bs=acc[c*M_NMAT+mm];best=mm;}
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

    }
    if(f.skin<0)f.skin=0; if(f.skin>4)f.skin=4;
    if(f.hair<0)f.hair=0; if(f.hair>4)f.hair=4;
    if(f.haircol<0)f.haircol=0; if(f.haircol>4)f.haircol=4;
    if(f.iris<0)f.iris=0; if(f.iris>3)f.iris=3;
    if(f.shirt<0)f.shirt=0; if(f.shirt>3)f.shirt=3;
    if(f.pants<0)f.pants=0; if(f.pants>1)f.pants=1;
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
