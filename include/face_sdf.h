/* ── face_sdf.h: a parametric signed-distance human face ──────────────────────
 * A procedural head built from smooth-min-blended primitives (skull ellipsoid,
 * jaw, brow ridge, nose, lips, eye sockets + eyeballs, cheeks, ears). Designed
 * to be sphere-traced by the same raymarcher fractal.c uses, and to expose a
 * material id + albedo so the shader can tint skin / lips / eye-white / iris /
 * brow distinctly for a lifelike read.
 *
 * Object space convention: face looks down +Z, up is +Y, the head is centered
 * near the origin with overall radius ~1.0. All params are unitless ratios so
 * one camera framing works across faces.
 */
#ifndef DSCO_FACE_SDF_H
#define DSCO_FACE_SDF_H

typedef struct {
    /* skull / proportions */
    double skull_r;      /* base head radius                        ~1.00 */
    double skull_flat;   /* back-of-head flattening (0..1)          ~0.10 */
    double face_long;    /* vertical elongation of the face         ~1.15 */
    double jaw_w;        /* jaw width                                ~0.62 */
    double jaw_len;      /* chin drop                                ~0.55 */
    double cheek;        /* cheekbone prominence                    ~0.20 */
    double temple;       /* temple narrowing                        ~0.90 */

    /* brow + forehead */
    double brow;         /* brow-ridge prominence                   ~0.10 */
    double brow_y;       /* brow height                             ~0.18 */
    double forehead;     /* forehead roundness                      ~0.95 */

    /* nose */
    double nose_len;     /* tip projection along +Z                 ~0.34 */
    double nose_w;       /* nostril width                           ~0.18 */
    double nose_bridge;  /* bridge height                           ~0.10 */
    double nose_y;       /* vertical center of nose                 ~0.02 */

    /* eyes */
    double eye_sep;      /* half-distance between eyes              ~0.30 */
    double eye_y;        /* eye line height                        ~0.06 */
    double eye_size;     /* eyeball radius                          ~0.14 */
    double eye_depth;    /* socket recession along -Z               ~0.10 */
    double lid;          /* upper-lid coverage (0 open .. 1 shut)   ~0.30 */

    /* mouth / lips */
    double mouth_w;      /* mouth half-width                        ~0.26 */
    double mouth_y;      /* mouth height                           ~-0.40 */
    double lip;          /* lip fullness                            ~0.06 */
    double smile;        /* mouth-corner lift (-1..1)               ~0.10 */

    /* ears */
    double ear;          /* ear size (0 = hidden)                   ~0.16 */
    double ear_y;        /* ear height                              ~0.00 */

    /* surface */
    double blend;        /* global smooth-min radius (softness)     ~0.06 */
} face_params_t;

/* Material ids returned by face_sdf_material. */
enum {
    FACE_MAT_SKIN = 0,
    FACE_MAT_LIP,
    FACE_MAT_EYE_WHITE,
    FACE_MAT_IRIS,
    FACE_MAT_PUPIL,
    FACE_MAT_BROW,
    FACE_MAT_NOSTRIL,
    FACE_MAT_COUNT
};

/* Fill p with a neutral, balanced default face. */
void face_params_default(face_params_t *p);

/* A couple of preset variations so the controls can cycle "faces". idx wraps. */
int  face_preset_count(void);
void face_preset(int idx, face_params_t *p, const char **name_out);

/* Signed distance to the face surface at object-space (x,y,z). */
double face_sdf_de(double x, double y, double z, const face_params_t *p);

/* Material id at/near a surface point — used to pick albedo. Cheap to call from
 * the shade step (one classification at the hit point, not per march step). */
int face_sdf_material(double x, double y, double z, const face_params_t *p);

/* Base albedo (0..1 RGB) for a material id, before lighting. */
void face_sdf_albedo(int material, double *r, double *g, double *b);

#endif /* DSCO_FACE_SDF_H */
