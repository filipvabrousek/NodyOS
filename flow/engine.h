/* Flow - the 3D / node-graph engine behind the VFX application.
 *
 * Data flows through the graph as Geo values:
 *   - a baked triangle mesh, and/or baked sphere sprites, or
 *   - a clone set: an array of transforms that instance a template mesh (or a
 *     sphere sprite). MoGraph effectors edit the clone set; anything that needs
 *     real geometry "realizes" it first.
 * Everything a graph evaluation produces lives in a bump arena that is reset
 * at the start of the next evaluation.
 */
#ifndef FLOW_ENGINE_H
#define FLOW_ENGINE_H

#include "lib/types.h"
#include "lib/kmath.h"
#include "gui/gfx.h"

/* ---- vectors and transforms ---------------------------------------------- */
typedef struct V3 { float x, y, z; } V3;

static inline V3 v3(float x, float y, float z) { V3 v = { x, y, z }; return v; }
static inline V3 v3_add(V3 a, V3 b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
static inline V3 v3_sub(V3 a, V3 b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static inline V3 v3_mul(V3 a, float s) { return v3(a.x * s, a.y * s, a.z * s); }
static inline float v3_dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline V3 v3_cross(V3 a, V3 b) { return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
static inline float v3_len(V3 a) { return m_sqrt(v3_dot(a, a)); }
static inline V3 v3_norm(V3 a) { float l = v3_len(a); return l > 1e-12f ? v3_mul(a, 1.0f / l) : v3(0, 1, 0); }
static inline V3 v3_lerp(V3 a, V3 b, float t) { return v3(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t); }

/* affine transform: p' = M p + t, M stored row-major */
typedef struct Xf { float m[9]; V3 t; } Xf;

Xf xf_trs(V3 pos, V3 rot_radians, V3 scale);    /* rotation order: bank (Z), pitch (X), heading (Y) */
static inline V3 xf_point(const Xf *m, V3 p)
{
    return v3(m->m[0] * p.x + m->m[1] * p.y + m->m[2] * p.z + m->t.x,
              m->m[3] * p.x + m->m[4] * p.y + m->m[5] * p.z + m->t.y,
              m->m[6] * p.x + m->m[7] * p.y + m->m[8] * p.z + m->t.z);
}

/* ---- evaluation arena ----------------------------------------------------- */
bool  arena_init(usize bytes);
void *arena_alloc(usize n);             /* NULL when exhausted; callers treat that as "no geometry" */
void  arena_reset(void);
usize arena_mark(void);
void  arena_release(usize mark);
usize arena_used(void);

/* ---- geometry ------------------------------------------------------------- */
#define MAX_TRIS      120000
#define TRI_TWOSIDED  0x01000000u       /* flag in a triangle colour */
#define CLONE_COLOR   0xFF000000u       /* flag in a clone colour: overrides the template */

typedef struct Mesh {
    int  nv, nt;
    V3  *v;
    int *idx;                           /* 3 per triangle, counter-clockwise seen from outside */
    u32 *col;                           /* per triangle 0xRRGGBB plus flags */
} Mesh;

typedef struct Clone {
    V3    p, r, s;                      /* position, rotation (radians), scale */
    u32   col;
    float t;                            /* 0..1 position in the sequence */
} Clone;

typedef struct Sprite { V3 p; float r; u32 col; } Sprite;

typedef struct Geo {
    Mesh   *mesh;                       /* baked mesh, or the template when cl != NULL */
    Clone  *cl;
    int     ncl;
    bool    sprites;                    /* clones are sphere sprites of radius sprite_r */
    float   sprite_r;
    Sprite *spr;                        /* baked sprites */
    int     nspr;
} Geo;

Mesh *mesh_new(int nv, int nt);
Mesh *mesh_cube(V3 size, u32 col);
Mesh *mesh_sphere(float radius, int segments, u32 col);
Mesh *mesh_cylinder(float r_bottom, float r_top, float height, int segments, u32 col);
Mesh *mesh_torus(float radius, float tube, int segments, int sides, u32 col);
Mesh *mesh_plane(float sx, float sz, int div, u32 col);
Geo  *geo_new(void);
Geo  *geo_from_mesh(Mesh *m);
Geo  *geo_realize(Geo *g);              /* bake clones into mesh / sprites */
bool  geo_bounds(const Geo *g, V3 *lo, V3 *hi);         /* of a realized Geo */

/* ---- node graph ----------------------------------------------------------- */
#define MAX_NODES   40
#define MAX_PARAMS  18
#define MAX_INPUTS  4
#define FLOW_FPS    30

enum { PK_FLOAT, PK_INT, PK_ENUM, PK_BOOL, PK_COLOR };
enum { CAT_GEO, CAT_MOGRAPH, CAT_DEFORM, CAT_SIM, CAT_UTIL, CAT_COUNT };

typedef struct ParamDef {
    const char *name;
    u8    kind;
    float def, lo, hi, step;
    const char *opts;                   /* PK_ENUM: "A|B|C" */
    u8    vis_param;                    /* shown only when (vis >> p[vis_param]) & 1 ... */
    u16   vis;                          /* ... or always when vis == 0 */
} ParamDef;

struct Scene;
struct Node;

typedef struct NodeType {
    const char *name;                   /* shown in the editor */
    const char *id;                     /* stored in scene files, at most 11 characters */
    u8    cat, nin, np;
    const char *in[MAX_INPUTS];
    ParamDef p[MAX_PARAMS];
    Geo *(*eval)(struct Scene *, struct Node *);
} NodeType;

typedef struct Node {
    bool  used, bypass, busy;
    u8    type;
    int   x, y;                         /* position in the graph editor */
    float p[MAX_PARAMS];
    int   in[MAX_INPUTS];               /* source node index or -1 */
    Geo  *out;                          /* cached result of the current evaluation */
    u32   stamp;
    void *state;                        /* simulation state that survives between frames */
} Node;

typedef struct Scene {
    Node  n[MAX_NODES];
    int   display;                      /* node shown in the viewport, -1 for none */
    int   selected;
    int   frame, loop_len;
    bool  loop, grid, wire;
    float yaw, pitch, dist;             /* orbit camera */
    V3    target;
    int   pan_x, pan_y;                 /* graph editor scroll */
    u32   stamp;
} Scene;

extern const NodeType g_types[];
extern const int g_ntypes;
extern const char *const g_cat_names[CAT_COUNT];
extern const u32 g_cat_colors[CAT_COUNT];

int   type_find(const char *id);
int   param_enum_count(const ParamDef *d);
void  param_enum_name(const ParamDef *d, int i, char *out, int cap);
bool  param_visible(const NodeType *t, const Node *n, int i);

void  scene_init(Scene *sc);
void  scene_clear(Scene *sc);
int   scene_add(Scene *sc, int type, int x, int y);
void  scene_del(Scene *sc, int i);
bool  scene_connect(Scene *sc, int src, int dst, int slot);     /* false if it would form a cycle */
int   scene_count(const Scene *sc);
Geo  *scene_eval(Scene *sc);            /* evaluates the display node at sc->frame, realized */
Geo  *node_eval(Scene *sc, int i);
static inline float scene_time(const Scene *sc) { return (float)sc->frame / (float)FLOW_FPS; }

#define SCENE_FILE_MAX 8192
int   scene_save(const Scene *sc, u8 *buf, int cap);            /* bytes written, 0 on failure */
bool  scene_load(Scene *sc, const u8 *buf, int size);
void  scene_demo(Scene *sc, int which);
int   scene_demo_count(void);
const char *scene_demo_name(int which);

/* fluid.c */
Geo  *fluid_eval(Scene *sc, Node *n);
void  fluid_free(void *state);
int   fluid_count(const Node *n);

/* ---- viewport rendering --------------------------------------------------- */
typedef struct Guide { V3 a, b; u32 col; } Guide;
typedef struct RenderStats { int tris, drawn, sprites; } RenderStats;

int   scene_guides(const Scene *sc, Guide *out, int max);       /* helper lines for the viewport */
void  render_view(Surface *s, Rect vp, int *zbuf, const Scene *sc, const Geo *geo,
                  const Guide *guides, int nguides, RenderStats *stats);

#endif
