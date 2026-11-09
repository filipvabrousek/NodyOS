/* Flow - node types: generators, MoGraph (cloner + effectors), deformers,
 * utilities. Each node type is one table entry plus one evaluation function. */
#include "flow/engine.h"
#include "lib/lib.h"

const char *const g_cat_names[CAT_COUNT] = { "Geometry", "MoGraph", "Deform", "Simulate", "Utility" };
const u32 g_cat_colors[CAT_COUNT] = { 0x2FA08D, 0x8E5BD6, 0xD6715B, 0x3F8ED6, 0x6F7686 };

/* ---- parameter access ----------------------------------------------------- */
static inline int  pi(const Node *n, int i) { return m_iround(n->p[i]); }
static inline bool pb(const Node *n, int i) { return n->p[i] > 0.5f; }
static inline u32  pc(const Node *n, int i) { return (u32)n->p[i] & 0xFFFFFF; }
static inline Geo *in_geo(Scene *sc, Node *n, int slot) { return node_eval(sc, n->in[slot]); }

static u32 lerp_col(u32 a, u32 b, float t) { return col_mix(a, b, (int)(m_clamp(t, 0, 1) * 256.0f)); }

/* ---- generators ----------------------------------------------------------- */
static Geo *ev_cube(Scene *sc, Node *n)     { (void)sc; return geo_from_mesh(mesh_cube(v3(n->p[0], n->p[1], n->p[2]), pc(n, 3))); }
static Geo *ev_sphere(Scene *sc, Node *n)   { (void)sc; return geo_from_mesh(mesh_sphere(n->p[0], pi(n, 1), pc(n, 2))); }
static Geo *ev_cylinder(Scene *sc, Node *n) { (void)sc; return geo_from_mesh(mesh_cylinder(n->p[0], n->p[0], n->p[1], pi(n, 2), pc(n, 3))); }
static Geo *ev_cone(Scene *sc, Node *n)     { (void)sc; return geo_from_mesh(mesh_cylinder(n->p[0], 0, n->p[1], pi(n, 2), pc(n, 3))); }
static Geo *ev_torus(Scene *sc, Node *n)    { (void)sc; return geo_from_mesh(mesh_torus(n->p[0], n->p[1], pi(n, 2), pi(n, 3), pc(n, 4))); }
static Geo *ev_plane(Scene *sc, Node *n)    { (void)sc; return geo_from_mesh(mesh_plane(n->p[0], n->p[1], pi(n, 2), pc(n, 3))); }

/* ---- cloner --------------------------------------------------------------- */
#define MAX_CLONES 6000

static Geo *ev_cloner(Scene *sc, Node *n)
{
    Geo *src = geo_realize(in_geo(sc, n, 0));
    if (!src) return NULL;
    int mode = pi(n, 0);
    float us = n->p[15];
    Geo *pts = mode == 3 ? in_geo(sc, n, 1) : NULL;
    if (mode == 3 && !pts) return src;                  /* nothing to clone onto yet */
    if (pts && !pts->cl) pts = geo_realize(pts);
    if (mode == 3 && !pts) return src;

    int count = 0, nx = 1, ny = 1, nz = 1;
    if (mode == 0 || mode == 1) count = pi(n, 1);
    else if (mode == 2) { nx = i_clamp(pi(n, 9), 1, 64); ny = i_clamp(pi(n, 10), 1, 64); nz = i_clamp(pi(n, 11), 1, 64); count = nx * ny * nz; }
    else if (pts->cl) count = pts->ncl;
    else if (pts->mesh) count = pts->mesh->nv;
    else count = pts->nspr;
    count = i_clamp(count, 0, MAX_CLONES);

    Clone *cl = arena_alloc(sizeof(Clone) * (usize)(count + 1));
    Geo *g = geo_new();
    if (!cl || !g) return NULL;
    float inv = count > 1 ? 1.0f / (float)(count - 1) : 0.0f;

    for (int i = 0; i < count; i++) {
        Clone *c = &cl[i];
        c->p = v3(0, 0, 0); c->r = v3(0, 0, 0); c->s = v3(us, us, us);
        c->col = 0;
        c->t = (float)i * inv;
        if (mode == 0) {                                /* linear */
            c->p = v3_mul(v3(n->p[2], n->p[3], n->p[4]), (float)i);
            c->r.y = n->p[5] * M_DEG * (float)i;
            float k = us;
            for (int j = 0; j < i; j++) k *= n->p[6];
            c->s = v3(k, k, k);
        } else if (mode == 1) {                         /* radial, in the XZ plane */
            float a = M_TAU * (float)i / (float)count;
            c->p = v3(n->p[7] * m_cos(a), 0, n->p[7] * m_sin(a));
            if (pb(n, 8)) c->r.y = -a;                  /* local +X points outward */
        } else if (mode == 2) {                         /* grid, centred */
            int ix = i % nx, iy = (i / nx) % ny, iz = i / (nx * ny);
            c->p = v3(((float)ix - (float)(nx - 1) * 0.5f) * n->p[12],
                      ((float)iy - (float)(ny - 1) * 0.5f) * n->p[13],
                      ((float)iz - (float)(nz - 1) * 0.5f) * n->p[14]);
        } else if (pts->cl) {                           /* onto the points of another clone set */
            c->p = pts->cl[i].p;
            c->r = pts->cl[i].r;
            c->s = v3_mul(pts->cl[i].s, us);
            c->col = pts->cl[i].col;
        } else if (pts->mesh) {
            c->p = pts->mesh->v[i];
        } else {
            c->p = pts->spr[i].p;
        }
    }
    g->mesh = src->mesh;
    g->cl = cl;
    g->ncl = count;
    if (!src->mesh && src->nspr > 0) {                  /* cloning a sprite: stay a sprite */
        g->sprites = true;
        g->sprite_r = src->spr[0].r;
        for (int i = 0; i < count; i++) if (!(cl[i].col & CLONE_COLOR)) cl[i].col = CLONE_COLOR | src->spr[0].col;
    }
    return g;
}

/* ---- effectors ------------------------------------------------------------ */
typedef struct Falloff { int type; V3 c; float radius, strength; bool invert; } Falloff;

static Falloff falloff_get(Scene *sc, Node *n, int base, int field_slot)
{
    Falloff f;
    f.type = pi(n, base);
    f.c = v3(n->p[base + 1], n->p[base + 2], n->p[base + 3]);
    f.radius = m_max(n->p[base + 4], 0.01f);
    f.strength = n->p[base + 5];
    f.invert = pb(n, base + 6);
    if (n->in[field_slot] >= 0) {                       /* a field object drives the falloff centre */
        V3 lo, hi;
        Geo *fg = geo_realize(in_geo(sc, n, field_slot));
        if (geo_bounds(fg, &lo, &hi)) {
            f.c = v3_add(f.c, v3_mul(v3_add(lo, hi), 0.5f));
            if (f.type == 0) f.type = 1;
        }
    }
    return f;
}

static inline float falloff_w(const Falloff *f, V3 p)
{
    float w = 1.0f;
    if (f->type == 1) {
        w = m_clamp(1.0f - v3_len(v3_sub(p, f->c)) / f->radius, 0.0f, 1.0f);
        w = m_smooth(w);
    } else if (f->type == 2) {
        w = m_smooth(m_clamp((p.x - f->c.x) / f->radius * 0.5f + 0.5f, 0.0f, 1.0f));
    }
    if (f->invert) w = 1.0f - w;
    return w * f->strength;
}

/* index of the first falloff parameter for effector types, -1 otherwise */
static int falloff_base(int type)
{
    const char *id = g_types[type].id;
    if (str_cmp(id, "plain") == 0) return 9;
    if (str_cmp(id, "random") == 0 || str_cmp(id, "wave") == 0 || str_cmp(id, "step") == 0) return 8;
    return -1;
}

/* private copy of the incoming clone set; a plain mesh becomes a single clone */
static Geo *clones_in(Scene *sc, Node *n)
{
    Geo *g = in_geo(sc, n, 0);
    if (!g) return NULL;
    if (!g->cl && !g->mesh) return g;
    Geo *o = geo_new();
    if (!o) return NULL;
    *o = *g;
    if (g->cl) {
        o->cl = arena_alloc(sizeof(Clone) * (usize)(g->ncl + 1));
        if (!o->cl) return NULL;
        memcpy(o->cl, g->cl, sizeof(Clone) * (usize)g->ncl);
    } else {
        o->cl = arena_alloc(sizeof(Clone));
        if (!o->cl) return NULL;
        o->cl[0].p = v3(0, 0, 0); o->cl[0].r = v3(0, 0, 0); o->cl[0].s = v3(1, 1, 1);
        o->cl[0].col = 0; o->cl[0].t = 0;
        o->ncl = 1;
        o->spr = NULL; o->nspr = 0;
    }
    return o;
}

static u32 clone_base_col(const Geo *g, const Clone *c)
{
    if (c->col & CLONE_COLOR) return c->col & 0xFFFFFF;
    if (g->mesh && g->mesh->nt > 0) return g->mesh->col[0] & 0xFFFFFF;
    return 0xCCCCCC;
}

static inline void fx_apply(Clone *c, float w, V3 move, float scale, V3 rot_deg)
{
    c->p = v3_add(c->p, v3_mul(move, w));
    c->s = v3_mul(c->s, m_max(0.0f, 1.0f + scale * w));
    c->r = v3_add(c->r, v3_mul(rot_deg, w * M_DEG));
}

static Geo *ev_plain(Scene *sc, Node *n)
{
    Geo *g = clones_in(sc, n);
    if (!g || !g->cl) return g;
    Falloff f = falloff_get(sc, n, 9, 1);
    V3 move = v3(n->p[0], n->p[1], n->p[2]), rot = v3(n->p[4], n->p[5], n->p[6]);
    bool use_col = pb(n, 7);
    u32 col = pc(n, 8);
    for (int i = 0; i < g->ncl; i++) {
        Clone *c = &g->cl[i];
        float w = falloff_w(&f, c->p);
        if (use_col && w > 0.004f) c->col = CLONE_COLOR | lerp_col(clone_base_col(g, c), col, w);
        fx_apply(c, w, move, n->p[3], rot);
    }
    return g;
}

static Geo *ev_random(Scene *sc, Node *n)
{
    Geo *g = clones_in(sc, n);
    if (!g || !g->cl) return g;
    Falloff f = falloff_get(sc, n, 8, 1);
    u32 seed = (u32)pi(n, 5);
    float speed = n->p[6], time = scene_time(sc);
    bool rcol = pb(n, 7);
    for (int i = 0; i < g->ncl; i++) {
        Clone *c = &g->cl[i];
        float w = falloff_w(&f, c->p);
        float r[7];
        for (int k = 0; k < 7; k++) {
            if (speed > 0.0f)
                r[k] = m_clamp(m_noise3((float)i * 0.731f + (float)k * 17.3f, time * speed, (float)seed * 3.17f) * 1.7f, -1.0f, 1.0f);
            else
                r[k] = m_rand11(m_hash2(seed * 977u + (u32)k, (u32)i));
        }
        if (rcol && w > 0.004f) {
            u32 rc = col_hsv(m_rand01(m_hash2(seed + 77u, (u32)i)), 0.62f, 0.96f);
            c->col = CLONE_COLOR | lerp_col(clone_base_col(g, c), rc, w);
        }
        fx_apply(c, w, v3(r[0] * n->p[0], r[1] * n->p[1], r[2] * n->p[2]), r[3] * n->p[3],
                 v3(r[4] * n->p[4], r[5] * n->p[4], r[6] * n->p[4]));
    }
    return g;
}

static Geo *ev_wave(Scene *sc, Node *n)
{
    Geo *g = clones_in(sc, n);
    if (!g || !g->cl) return g;
    Falloff f = falloff_get(sc, n, 8, 1);
    int pattern = pi(n, 0);
    float freq = n->p[6], phase0 = scene_time(sc) * n->p[7];
    V3 move = v3(n->p[1], n->p[2], n->p[3]);
    for (int i = 0; i < g->ncl; i++) {
        Clone *c = &g->cl[i];
        float w = falloff_w(&f, c->p);
        float ph = pattern == 0 ? (float)i * freq : pattern == 1 ? c->p.x * freq : pattern == 2 ? c->p.z * freq
                 : m_sqrt(c->p.x * c->p.x + c->p.z * c->p.z) * freq;
        float v = m_sin((ph - phase0) * M_TAU);
        fx_apply(c, w * v, move, n->p[4], v3(0, n->p[5], 0));
    }
    return g;
}

static Geo *ev_step(Scene *sc, Node *n)
{
    Geo *g = clones_in(sc, n);
    if (!g || !g->cl) return g;
    Falloff f = falloff_get(sc, n, 8, 1);
    V3 move = v3(n->p[0], n->p[1], n->p[2]), rot = v3(n->p[4], n->p[5], n->p[6]);
    bool smooth = pi(n, 7) == 1;
    for (int i = 0; i < g->ncl; i++) {
        Clone *c = &g->cl[i];
        float t = smooth ? m_smooth(c->t) : c->t;
        fx_apply(c, falloff_w(&f, c->p) * t, move, n->p[3], rot);
    }
    return g;
}

/* ---- colour --------------------------------------------------------------- */
static Geo *ev_color(Scene *sc, Node *n)
{
    Geo *g = in_geo(sc, n, 0);
    if (!g) return NULL;
    int mode = pi(n, 0);
    u32 ca = pc(n, 1), cb = pc(n, 2), seed = (u32)pi(n, 3);
    Geo *o = geo_new();
    if (!o) return NULL;
    *o = *g;
    V3 lo, hi;
    float y0 = 0, yr = 1;
    if (mode == 2 && geo_bounds(g, &lo, &hi)) { y0 = lo.y; yr = hi.y - lo.y > 1e-5f ? 1.0f / (hi.y - lo.y) : 0.0f; }

    if (g->cl) {
        o->cl = arena_alloc(sizeof(Clone) * (usize)(g->ncl + 1));
        if (!o->cl) return NULL;
        for (int i = 0; i < g->ncl; i++) {
            Clone c = g->cl[i];
            float t = mode == 1 ? c.t : mode == 2 ? (c.p.y - y0) * yr : mode == 3 ? m_rand01(m_hash2(seed, (u32)i)) : 0.0f;
            c.col = CLONE_COLOR | lerp_col(ca, cb, t);
            o->cl[i] = c;
        }
        return o;
    }
    if (g->mesh) {
        const Mesh *s = g->mesh;
        Mesh *m = arena_alloc(sizeof *m);
        u32 *col = arena_alloc(sizeof(u32) * (usize)(s->nt + 1));
        if (!m || !col) return NULL;
        *m = *s;
        m->col = col;
        float inv = s->nt > 1 ? 1.0f / (float)(s->nt - 1) : 0.0f;
        for (int t = 0; t < s->nt; t++) {
            float k = 0.0f;
            if (mode == 1) k = (float)t * inv;
            else if (mode == 2) k = ((s->v[s->idx[t * 3]].y + s->v[s->idx[t * 3 + 1]].y + s->v[s->idx[t * 3 + 2]].y) * (1.0f / 3.0f) - y0) * yr;
            else if (mode == 3) k = m_rand01(m_hash2(seed, (u32)t));
            col[t] = lerp_col(ca, cb, k) | (s->col[t] & TRI_TWOSIDED);
        }
        o->mesh = m;
    }
    if (g->nspr > 0) {
        o->spr = arena_alloc(sizeof(Sprite) * (usize)g->nspr);
        if (!o->spr) return NULL;
        float inv = g->nspr > 1 ? 1.0f / (float)(g->nspr - 1) : 0.0f;
        for (int i = 0; i < g->nspr; i++) {
            Sprite s = g->spr[i];
            float t = mode == 1 ? (float)i * inv : mode == 2 ? (s.p.y - y0) * yr : mode == 3 ? m_rand01(m_hash2(seed, (u32)i)) : 0.0f;
            s.col = lerp_col(ca, cb, t);
            o->spr[i] = s;
        }
    }
    return o;
}

/* ---- deformers ------------------------------------------------------------ */
/* copy of a realized Geo with fresh, writable vertex and sprite arrays */
static Geo *writable(Geo *g)
{
    g = geo_realize(g);
    if (!g) return NULL;
    Geo *o = geo_new();
    if (!o) return NULL;
    if (g->mesh) {
        Mesh *m = arena_alloc(sizeof *m);
        if (!m) return NULL;
        *m = *g->mesh;
        m->v = arena_alloc(sizeof(V3) * (usize)(m->nv + 1));
        if (!m->v) return NULL;
        memcpy(m->v, g->mesh->v, sizeof(V3) * (usize)m->nv);
        o->mesh = m;
    }
    if (g->nspr > 0) {
        o->spr = arena_alloc(sizeof(Sprite) * (usize)g->nspr);
        if (!o->spr) return NULL;
        memcpy(o->spr, g->spr, sizeof(Sprite) * (usize)g->nspr);
        o->nspr = g->nspr;
    }
    return o;
}

static Geo *apply_xf(Geo *g, const Xf *x, float sprite_scale)
{
    Geo *o = writable(g);
    if (!o) return NULL;
    if (o->mesh) for (int i = 0; i < o->mesh->nv; i++) o->mesh->v[i] = xf_point(x, o->mesh->v[i]);
    for (int i = 0; i < o->nspr; i++) { o->spr[i].p = xf_point(x, o->spr[i].p); o->spr[i].r *= sprite_scale; }
    return o;
}

static Geo *ev_transform(Scene *sc, Node *n)
{
    V3 s = v3(n->p[6] * n->p[7], n->p[6] * n->p[8], n->p[6] * n->p[9]);
    Xf x = xf_trs(v3(n->p[0], n->p[1], n->p[2]), v3(n->p[3] * M_DEG, n->p[4] * M_DEG, n->p[5] * M_DEG), s);
    return apply_xf(in_geo(sc, n, 0), &x, (s.x + s.y + s.z) * (1.0f / 3.0f));
}

static Geo *ev_animate(Scene *sc, Node *n)
{
    float t = scene_time(sc);
    float orbit = t * n->p[6] * M_TAU;
    V3 pos = v3(n->p[5] * m_cos(orbit), n->p[3] * m_sin(t * n->p[4] * M_TAU), n->p[5] * m_sin(orbit));
    V3 rot = v3(n->p[0] * t * M_DEG, n->p[1] * t * M_DEG, n->p[2] * t * M_DEG);
    float k = 1.0f + n->p[7] * m_sin(t * n->p[8] * M_TAU);
    Xf x = xf_trs(pos, rot, v3(k, k, k));
    return apply_xf(in_geo(sc, n, 0), &x, k);
}

static inline V3 twist_point(V3 p, int axis, float rate)
{
    if (axis == 0) { float a = p.x * rate, c = m_cos(a), s = m_sin(a); return v3(p.x, p.y * c - p.z * s, p.y * s + p.z * c); }
    if (axis == 1) { float a = p.y * rate, c = m_cos(a), s = m_sin(a); return v3(p.x * c + p.z * s, p.y, -p.x * s + p.z * c); }
    float a = p.z * rate, c = m_cos(a), s = m_sin(a);
    return v3(p.x * c - p.y * s, p.x * s + p.y * c, p.z);
}

static Geo *ev_twist(Scene *sc, Node *n)
{
    Geo *o = writable(in_geo(sc, n, 0));
    if (!o) return NULL;
    int axis = pi(n, 1);
    float rate = n->p[0] * M_DEG;
    if (o->mesh) for (int i = 0; i < o->mesh->nv; i++) o->mesh->v[i] = twist_point(o->mesh->v[i], axis, rate);
    for (int i = 0; i < o->nspr; i++) o->spr[i].p = twist_point(o->spr[i].p, axis, rate);
    return o;
}

static inline V3 noise_point(V3 p, float amt, float scale, float tz, int dir, float seed)
{
    float a = m_noise3(p.x * scale + seed, p.y * scale + tz, p.z * scale - seed);
    if (dir == 0) return v3(p.x, p.y + a * amt, p.z);
    if (dir == 2) return v3_add(p, v3_mul(v3_norm(p), a * amt));
    float b = m_noise3(p.x * scale + 31.7f + seed, p.y * scale - tz, p.z * scale + 11.3f);
    float c = m_noise3(p.x * scale - 17.1f, p.y * scale + 5.9f + seed, p.z * scale + tz);
    return v3(p.x + a * amt, p.y + b * amt, p.z + c * amt);
}

static Geo *ev_noise(Scene *sc, Node *n)
{
    Geo *o = writable(in_geo(sc, n, 0));
    if (!o) return NULL;
    float amt = n->p[0], scale = n->p[1], tz = scene_time(sc) * n->p[2], seed = (float)pi(n, 4) * 7.3f;
    int dir = pi(n, 3);
    if (o->mesh) for (int i = 0; i < o->mesh->nv; i++) o->mesh->v[i] = noise_point(o->mesh->v[i], amt, scale, tz, dir, seed);
    for (int i = 0; i < o->nspr; i++) o->spr[i].p = noise_point(o->spr[i].p, amt, scale, tz, dir, seed);
    return o;
}

/* ---- merge ---------------------------------------------------------------- */
static Geo *ev_merge(Scene *sc, Node *n)
{
    Geo *in[MAX_INPUTS];
    int nv = 0, nt = 0, ns = 0;
    for (int i = 0; i < MAX_INPUTS; i++) {
        in[i] = geo_realize(in_geo(sc, n, i));
        if (!in[i]) continue;
        if (in[i]->mesh) { nv += in[i]->mesh->nv; nt += in[i]->mesh->nt; }
        ns += in[i]->nspr;
    }
    Geo *o = geo_new();
    if (!o) return NULL;
    if (nt > MAX_TRIS) return in[0];
    if (nt > 0) {
        Mesh *m = mesh_new(nv, nt);
        if (!m) return in[0];
        int v0 = 0, t0 = 0;
        for (int i = 0; i < MAX_INPUTS; i++) {
            const Mesh *s = in[i] ? in[i]->mesh : NULL;
            if (!s) continue;
            memcpy(m->v + v0, s->v, sizeof(V3) * (usize)s->nv);
            memcpy(m->col + t0, s->col, sizeof(u32) * (usize)s->nt);
            for (int k = 0; k < s->nt * 3; k++) m->idx[t0 * 3 + k] = s->idx[k] + v0;
            v0 += s->nv;
            t0 += s->nt;
        }
        o->mesh = m;
    }
    if (ns > 0) {
        o->spr = arena_alloc(sizeof(Sprite) * (usize)ns);
        if (!o->spr) return o;
        for (int i = 0; i < MAX_INPUTS; i++) {
            if (!in[i] || in[i]->nspr == 0) continue;
            memcpy(o->spr + o->nspr, in[i]->spr, sizeof(Sprite) * (usize)in[i]->nspr);
            o->nspr += in[i]->nspr;
        }
    }
    return o;
}

/* ---- the node type table -------------------------------------------------- */
#define PF(name, def, lo, hi, step)           { name, PK_FLOAT, def, lo, hi, step, 0, 0, 0 }
#define PFV(name, def, lo, hi, step, vp, vis) { name, PK_FLOAT, def, lo, hi, step, 0, vp, vis }
#define PI_(name, def, lo, hi)                { name, PK_INT, def, lo, hi, 1, 0, 0, 0 }
#define PIV(name, def, lo, hi, vp, vis)       { name, PK_INT, def, lo, hi, 1, 0, vp, vis }
#define PE(name, def, opts)                   { name, PK_ENUM, def, 0, 0, 1, opts, 0, 0 }
#define PB(name, def)                         { name, PK_BOOL, def, 0, 1, 1, 0, 0, 0 }
#define PBV(name, def, vp, vis)               { name, PK_BOOL, def, 0, 1, 1, 0, vp, vis }
#define PC(name, rgb)                         { name, PK_COLOR, (float)(rgb), 0, 16777215.0f, 1, 0, 0, 0 }
#define PCV(name, rgb, vp, vis)               { name, PK_COLOR, (float)(rgb), 0, 16777215.0f, 1, 0, vp, vis }
#define FALLOFF(b) \
    PE("Falloff", 0, "None|Sphere|Linear"), \
    PFV("Center X", 0, -10, 10, 0.05f, b, 6), PFV("Center Y", 0, -10, 10, 0.05f, b, 6), \
    PFV("Center Z", 0, -10, 10, 0.05f, b, 6), PFV("Radius", 3, 0.1f, 12, 0.05f, b, 6), \
    PF("Strength", 1, 0, 2, 0.01f), PBV("Invert", 0, b, 6)

const NodeType g_types[] = {
    { "Cube", "cube", CAT_GEO, 0, 4, { 0 },
      { PF("Size X", 1, 0.05f, 6, 0.05f), PF("Size Y", 1, 0.05f, 6, 0.05f), PF("Size Z", 1, 0.05f, 6, 0.05f),
        PC("Color", 0x5B8DEF) }, ev_cube },
    { "Sphere", "sphere", CAT_GEO, 0, 3, { 0 },
      { PF("Radius", 0.7f, 0.05f, 4, 0.05f), PI_("Segments", 18, 4, 48), PC("Color", 0xF2B84B) }, ev_sphere },
    { "Cylinder", "cylinder", CAT_GEO, 0, 4, { 0 },
      { PF("Radius", 0.5f, 0.05f, 4, 0.05f), PF("Height", 1.4f, 0.05f, 8, 0.05f), PI_("Segments", 20, 3, 48),
        PC("Color", 0x63C788) }, ev_cylinder },
    { "Cone", "cone", CAT_GEO, 0, 4, { 0 },
      { PF("Radius", 0.6f, 0.05f, 4, 0.05f), PF("Height", 1.4f, 0.05f, 8, 0.05f), PI_("Segments", 20, 3, 48),
        PC("Color", 0xEB6A5A) }, ev_cone },
    { "Torus", "torus", CAT_GEO, 0, 5, { 0 },
      { PF("Radius", 0.9f, 0.1f, 5, 0.05f), PF("Tube", 0.28f, 0.02f, 2, 0.01f), PI_("Segments", 28, 3, 64),
        PI_("Sides", 10, 3, 24), PC("Color", 0xB877E0) }, ev_torus },
    { "Plane", "plane", CAT_GEO, 0, 4, { 0 },
      { PF("Size X", 6, 0.1f, 20, 0.1f), PF("Size Z", 6, 0.1f, 20, 0.1f), PI_("Divisions", 16, 1, 64),
        PC("Color", 0x8A93A6) }, ev_plane },

    { "Cloner", "cloner", CAT_MOGRAPH, 2, 16, { "Object", "Points" },
      { PE("Mode", 2, "Linear|Radial|Grid|Points"),
        PIV("Count", 8, 1, 200, 0, 3),
        PFV("Step X", 1.4f, -5, 5, 0.05f, 0, 1), PFV("Step Y", 0, -5, 5, 0.05f, 0, 1), PFV("Step Z", 0, -5, 5, 0.05f, 0, 1),
        PFV("Step Rotate", 0, -90, 90, 1, 0, 1), PFV("Step Scale", 1, 0.5f, 1.5f, 0.01f, 0, 1),
        PFV("Radius", 3, 0, 10, 0.05f, 0, 2), PBV("Align", 1, 0, 2),
        PIV("Count X", 5, 1, 30, 0, 4), PIV("Count Y", 1, 1, 30, 0, 4), PIV("Count Z", 5, 1, 30, 0, 4),
        PFV("Space X", 1.4f, 0, 5, 0.05f, 0, 4), PFV("Space Y", 1.4f, 0, 5, 0.05f, 0, 4), PFV("Space Z", 1.4f, 0, 5, 0.05f, 0, 4),
        PF("Clone Scale", 1, 0.05f, 3, 0.01f) }, ev_cloner },
    { "Plain Effector", "plain", CAT_MOGRAPH, 2, 16, { "Clones", "Field" },
      { PF("Move X", 0, -6, 6, 0.05f), PF("Move Y", 1.5f, -6, 6, 0.05f), PF("Move Z", 0, -6, 6, 0.05f),
        PF("Scale", 0, -1, 3, 0.01f),
        PF("Rotate X", 0, -360, 360, 1), PF("Rotate Y", 0, -360, 360, 1), PF("Rotate Z", 0, -360, 360, 1),
        PB("Use Color", 0), PCV("Color", 0xFF8A3D, 7, 2),
        FALLOFF(9) }, ev_plain },
    { "Random Effector", "random", CAT_MOGRAPH, 2, 15, { "Clones", "Field" },
      { PF("Move X", 0.5f, 0, 5, 0.05f), PF("Move Y", 0.5f, 0, 5, 0.05f), PF("Move Z", 0.5f, 0, 5, 0.05f),
        PF("Scale", 0.3f, 0, 1, 0.01f), PF("Rotate", 30, 0, 360, 1), PI_("Seed", 1, 0, 99),
        PF("Speed", 0, 0, 4, 0.05f), PB("Random Color", 0),
        FALLOFF(8) }, ev_random },
    { "Wave Effector", "wave", CAT_MOGRAPH, 2, 15, { "Clones", "Field" },
      { PE("Pattern", 3, "Index|Along X|Along Z|Radial"),
        PF("Move X", 0, -6, 6, 0.05f), PF("Move Y", 0.8f, -6, 6, 0.05f), PF("Move Z", 0, -6, 6, 0.05f),
        PF("Scale", 0, -1, 2, 0.01f), PF("Rotate Y", 0, -360, 360, 1),
        PF("Frequency", 0.18f, 0, 2, 0.01f), PF("Speed", 0.6f, -4, 4, 0.05f),
        FALLOFF(8) }, ev_wave },
    { "Step Effector", "step", CAT_MOGRAPH, 2, 15, { "Clones", "Field" },
      { PF("Move X", 0, -8, 8, 0.05f), PF("Move Y", 2, -8, 8, 0.05f), PF("Move Z", 0, -8, 8, 0.05f),
        PF("Scale", 0, -1, 3, 0.01f),
        PF("Rotate X", 0, -720, 720, 1), PF("Rotate Y", 0, -720, 720, 1), PF("Rotate Z", 0, -720, 720, 1),
        PE("Ease", 0, "Linear|Smooth"),
        FALLOFF(8) }, ev_step },

    { "Transform", "transform", CAT_DEFORM, 1, 10, { "Geometry" },
      { PF("Move X", 0, -10, 10, 0.05f), PF("Move Y", 0, -10, 10, 0.05f), PF("Move Z", 0, -10, 10, 0.05f),
        PF("Rotate X", 0, -360, 360, 1), PF("Rotate Y", 0, -360, 360, 1), PF("Rotate Z", 0, -360, 360, 1),
        PF("Scale", 1, 0.05f, 5, 0.01f),
        PF("Scale X", 1, 0.05f, 5, 0.01f), PF("Scale Y", 1, 0.05f, 5, 0.01f), PF("Scale Z", 1, 0.05f, 5, 0.01f) }, ev_transform },
    { "Animate", "animate", CAT_DEFORM, 1, 9, { "Geometry" },
      { PF("Spin X", 0, -360, 360, 1), PF("Spin Y", 45, -360, 360, 1), PF("Spin Z", 0, -360, 360, 1),
        PF("Bob Height", 0, 0, 4, 0.05f), PF("Bob Speed", 0.5f, 0, 4, 0.05f),
        PF("Orbit Radius", 0, 0, 8, 0.05f), PF("Orbit Speed", 0.25f, -2, 2, 0.01f),
        PF("Pulse", 0, 0, 1, 0.01f), PF("Pulse Speed", 1, 0, 4, 0.05f) }, ev_animate },
    { "Twist", "twist", CAT_DEFORM, 1, 2, { "Geometry" },
      { PF("Angle", 45, -360, 360, 1), PE("Axis", 1, "X|Y|Z") }, ev_twist },
    { "Noise", "noise", CAT_DEFORM, 1, 5, { "Geometry" },
      { PF("Amount", 0.4f, 0, 3, 0.01f), PF("Scale", 0.8f, 0.05f, 4, 0.01f), PF("Speed", 0.5f, 0, 4, 0.05f),
        PE("Direction", 0, "Up|Any|Outward"), PI_("Seed", 0, 0, 99) }, ev_noise },

    { "Fluid", "fluid", CAT_SIM, 1, 17, { "Collider" },
      { PE("Emitter", 0, "Stream|Block|Rain"),
        PI_("Particles", 700, 50, 1400),
        PIV("Rate", 4, 1, 30, 0, 5),
        PF("Emit X", -1.4f, -5, 5, 0.05f), PF("Emit Y", 2.6f, 0.2f, 8, 0.05f), PF("Emit Z", 0, -5, 5, 0.05f),
        PFV("Jet Speed", 3.0f, -8, 8, 0.05f, 0, 1),
        PF("Gravity", 9.8f, 0, 25, 0.1f), PF("Viscosity", 0.15f, 0, 1, 0.01f), PF("Stiffness", 1, 0.2f, 3, 0.01f),
        PF("Tank X", 3.0f, 0.6f, 8, 0.05f), PF("Tank Z", 1.3f, 0.6f, 8, 0.05f),
        PF("Drop Size", 0.105f, 0.03f, 0.3f, 0.005f),
        PC("Color", 0x3A9BFF), PF("Foam", 0.6f, 0, 1, 0.01f),
        PE("Collider", 0, "Sphere|Box"), PB("Recycle", 0) }, fluid_eval },

    { "Color", "color", CAT_UTIL, 1, 4, { "Geometry" },
      { PE("Mode", 0, "Solid|Index|Height|Random"), PC("Color A", 0xFF5D73),
        PCV("Color B", 0x4CC9F0, 0, 14), PIV("Seed", 1, 0, 99, 0, 8) }, ev_color },
    { "Merge", "merge", CAT_UTIL, 4, 0, { "A", "B", "C", "D" }, { { 0 } }, ev_merge },
};

const int g_ntypes = ARRAY_LEN(g_types);

int type_find(const char *id)
{
    for (int i = 0; i < g_ntypes; i++)
        if (str_cmp(g_types[i].id, id) == 0) return i;
    return -1;
}

int param_enum_count(const ParamDef *d)
{
    int n = 1;
    if (!d->opts) return 0;
    for (const char *p = d->opts; *p; p++) if (*p == '|') n++;
    return n;
}

void param_enum_name(const ParamDef *d, int i, char *out, int cap)
{
    const char *p = d->opts ? d->opts : "";
    while (i > 0 && *p) { if (*p == '|') i--; p++; }
    int n = 0;
    while (*p && *p != '|' && n + 1 < cap) out[n++] = *p++;
    out[n] = 0;
}

bool param_visible(const NodeType *t, const Node *n, int i)
{
    const ParamDef *d = &t->p[i];
    if (!d->vis) return true;
    return (d->vis >> m_iround(n->p[d->vis_param])) & 1;
}

/* ---- viewport helper lines for the selected node -------------------------- */
static int ring(Guide *out, int n, int max, V3 c, float r, int plane, u32 col)
{
    const int seg = 32;
    for (int i = 0; i < seg && n < max; i++) {
        float a0 = M_TAU * (float)i / seg, a1 = M_TAU * (float)(i + 1) / seg;
        float c0 = m_cos(a0) * r, s0 = m_sin(a0) * r, c1 = m_cos(a1) * r, s1 = m_sin(a1) * r;
        if (plane == 0) { out[n].a = v3(c.x + c0, c.y, c.z + s0); out[n].b = v3(c.x + c1, c.y, c.z + s1); }
        else if (plane == 1) { out[n].a = v3(c.x + c0, c.y + s0, c.z); out[n].b = v3(c.x + c1, c.y + s1, c.z); }
        else { out[n].a = v3(c.x, c.y + c0, c.z + s0); out[n].b = v3(c.x, c.y + c1, c.z + s1); }
        out[n++].col = col;
    }
    return n;
}

static int box(Guide *out, int n, int max, V3 lo, V3 hi, u32 col)
{
    static const u8 e[12][2] = { {0,1},{2,3},{4,5},{6,7},{0,2},{1,3},{4,6},{5,7},{0,4},{1,5},{2,6},{3,7} };
    for (int i = 0; i < 12 && n < max; i++) {
        int a = e[i][0], b = e[i][1];
        out[n].a = v3(a & 1 ? hi.x : lo.x, a & 2 ? hi.y : lo.y, a & 4 ? hi.z : lo.z);
        out[n].b = v3(b & 1 ? hi.x : lo.x, b & 2 ? hi.y : lo.y, b & 4 ? hi.z : lo.z);
        out[n++].col = col;
    }
    return n;
}

int scene_guides(const Scene *sc, Guide *out, int max)
{
    int n = 0;
    Scene *s = (Scene *)sc;
    for (int i = 0; i < MAX_NODES; i++) {
        Node *nd = &s->n[i];
        if (!nd->used || nd->stamp != sc->stamp || nd->bypass) continue;     /* only nodes in the displayed chain */
        const NodeType *t = &g_types[nd->type];
        if (t->eval == fluid_eval) {                    /* fluid tank and emitter */
            float hx = nd->p[10] * 0.5f, hz = nd->p[11] * 0.5f;
            n = box(out, n, max, v3(-hx, 0, -hz), v3(hx, m_max(nd->p[4], 1.0f), hz), 0x4A7FB8);
            if (pi(nd, 0) != 1) n = ring(out, n, max, v3(nd->p[3], nd->p[4], nd->p[5]), 0.12f, 2, 0x9CC8FF);
        }
        int fb = falloff_base(nd->type);
        if (fb >= 0 && i == sc->selected) {
            Falloff f = falloff_get(s, nd, fb, 1);
            if (f.type == 1) {
                for (int p = 0; p < 3; p++) n = ring(out, n, max, f.c, f.radius, p, 0xFFA94D);
            } else if (f.type == 2) {
                n = box(out, n, max, v3(f.c.x - f.radius, 0, -3), v3(f.c.x + f.radius, 2, 3), 0xFFA94D);
            }
        }
    }
    return n;
}
