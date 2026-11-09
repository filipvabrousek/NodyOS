/* Flow - arena, transforms, primitive meshes, clone realization. */
#include "flow/engine.h"
#include "lib/lib.h"
#include "lib/plat.h"

/* ---- arena ---------------------------------------------------------------- */
static u8   *arena_base;
static usize arena_cap, arena_top;

bool arena_init(usize bytes)
{
    if (arena_base) return true;
    arena_base = kmalloc(bytes);
    arena_cap = arena_base ? bytes : 0;
    arena_top = 0;
    return arena_base != NULL;
}

void *arena_alloc(usize n)
{
    n = (n + 15) & ~(usize)15;
    if (arena_top + n > arena_cap) return NULL;
    void *p = arena_base + arena_top;
    arena_top += n;
    return p;
}

void  arena_reset(void) { arena_top = 0; }
usize arena_mark(void) { return arena_top; }
void  arena_release(usize mark) { arena_top = mark; }
usize arena_used(void) { return arena_top; }

/* ---- transforms ----------------------------------------------------------- */
Xf xf_trs(V3 pos, V3 r, V3 s)
{
    Xf x;
    x.t = pos;
    if (r.x == 0 && r.y == 0 && r.z == 0) {
        x.m[0] = s.x; x.m[1] = 0;   x.m[2] = 0;
        x.m[3] = 0;   x.m[4] = s.y; x.m[5] = 0;
        x.m[6] = 0;   x.m[7] = 0;   x.m[8] = s.z;
        return x;
    }
    float cx = m_cos(r.x), sx = m_sin(r.x);
    float cy = m_cos(r.y), sy = m_sin(r.y);
    float cz = m_cos(r.z), sz = m_sin(r.z);
    /* R = Ry * Rx * Rz, then scale the columns */
    x.m[0] = (cy * cz + sy * sx * sz) * s.x;  x.m[1] = (-cy * sz + sy * sx * cz) * s.y;  x.m[2] = (sy * cx) * s.z;
    x.m[3] = (cx * sz) * s.x;                 x.m[4] = (cx * cz) * s.y;                  x.m[5] = (-sx) * s.z;
    x.m[6] = (-sy * cz + cy * sx * sz) * s.x; x.m[7] = (sy * sz + cy * sx * cz) * s.y;   x.m[8] = (cy * cx) * s.z;
    return x;
}

/* ---- meshes --------------------------------------------------------------- */
Mesh *mesh_new(int nv, int nt)
{
    if (nv < 0 || nt < 0 || nt > MAX_TRIS || nv > MAX_TRIS * 3) return NULL;
    Mesh *m = arena_alloc(sizeof *m);
    if (!m) return NULL;
    m->nv = nv; m->nt = nt;
    m->v = arena_alloc(sizeof(V3) * (usize)(nv + 1));
    m->idx = arena_alloc(sizeof(int) * 3 * (usize)(nt + 1));
    m->col = arena_alloc(sizeof(u32) * (usize)(nt + 1));
    if (!m->v || !m->idx || !m->col) return NULL;
    return m;
}

static void tri(Mesh *m, int *t, int a, int b, int c, u32 col)
{
    m->idx[*t * 3] = a; m->idx[*t * 3 + 1] = b; m->idx[*t * 3 + 2] = c;
    m->col[*t] = col;
    (*t)++;
}

Mesh *mesh_cube(V3 size, u32 col)
{
    static const u8 faces[12][3] = {
        { 1, 3, 7 }, { 1, 7, 5 }, { 0, 6, 2 }, { 0, 4, 6 }, { 2, 6, 7 }, { 2, 7, 3 },
        { 0, 5, 4 }, { 0, 1, 5 }, { 4, 5, 7 }, { 4, 7, 6 }, { 0, 3, 1 }, { 0, 2, 3 }
    };
    Mesh *m = mesh_new(8, 12);
    if (!m) return NULL;
    for (int i = 0; i < 8; i++)
        m->v[i] = v3((i & 1 ? 0.5f : -0.5f) * size.x, (i & 2 ? 0.5f : -0.5f) * size.y, (i & 4 ? 0.5f : -0.5f) * size.z);
    int t = 0;
    for (int i = 0; i < 12; i++) tri(m, &t, faces[i][0], faces[i][1], faces[i][2], col);
    return m;
}

Mesh *mesh_sphere(float radius, int seg, u32 col)
{
    seg = i_clamp(seg, 4, 64);
    int rings = i_max(3, seg / 2);
    Mesh *m = mesh_new(2 + (rings - 1) * seg, 2 * seg + 2 * seg * (rings - 2));
    if (!m) return NULL;
    int v = 0, t = 0;
    m->v[v++] = v3(0, radius, 0);
    for (int i = 1; i < rings; i++) {
        float phi = M_PI * (float)i / (float)rings;
        float y = radius * m_cos(phi), rr = radius * m_sin(phi);
        for (int j = 0; j < seg; j++) {
            float th = M_TAU * (float)j / (float)seg;
            m->v[v++] = v3(rr * m_cos(th), y, rr * m_sin(th));
        }
    }
    int bottom = v;
    m->v[v++] = v3(0, -radius, 0);
    for (int j = 0; j < seg; j++) {
        int j1 = (j + 1) % seg;
        tri(m, &t, 0, 1 + j1, 1 + j, col);
        for (int i = 0; i < rings - 2; i++) {
            int a = 1 + i * seg + j, b = 1 + i * seg + j1, c = a + seg, d = b + seg;
            tri(m, &t, a, b, c, col);
            tri(m, &t, b, d, c, col);
        }
        int last = 1 + (rings - 2) * seg;
        tri(m, &t, bottom, last + j, last + j1, col);
    }
    return m;
}

/* cylinder along Y; r_top == 0 gives a cone */
Mesh *mesh_cylinder(float r0, float r1, float h, int seg, u32 col)
{
    seg = i_clamp(seg, 3, 64);
    bool cone = r1 <= 0.0001f;
    Mesh *m = cone ? mesh_new(seg + 2, seg * 2) : mesh_new(seg * 2 + 2, seg * 4);
    if (!m) return NULL;
    int t = 0;
    for (int j = 0; j < seg; j++) {
        float th = M_TAU * (float)j / (float)seg;
        float c = m_cos(th), s = m_sin(th);
        m->v[j] = v3(r0 * c, -h * 0.5f, r0 * s);
        if (!cone) m->v[seg + j] = v3(r1 * c, h * 0.5f, r1 * s);
    }
    if (cone) {
        int apex = seg, base = seg + 1;
        m->v[apex] = v3(0, h * 0.5f, 0);
        m->v[base] = v3(0, -h * 0.5f, 0);
        for (int j = 0; j < seg; j++) {
            int j1 = (j + 1) % seg;
            tri(m, &t, j, apex, j1, col);
            tri(m, &t, base, j, j1, col);
        }
    } else {
        int cb = seg * 2, ct = seg * 2 + 1;
        m->v[cb] = v3(0, -h * 0.5f, 0);
        m->v[ct] = v3(0, h * 0.5f, 0);
        for (int j = 0; j < seg; j++) {
            int j1 = (j + 1) % seg;
            tri(m, &t, j, seg + j, j1, col);
            tri(m, &t, j1, seg + j, seg + j1, col);
            tri(m, &t, cb, j, j1, col);
            tri(m, &t, ct, seg + j1, seg + j, col);
        }
    }
    return m;
}

Mesh *mesh_torus(float R, float r, int seg, int sides, u32 col)
{
    seg = i_clamp(seg, 3, 96);
    sides = i_clamp(sides, 3, 32);
    Mesh *m = mesh_new(seg * sides, seg * sides * 2);
    if (!m) return NULL;
    int t = 0;
    for (int i = 0; i < seg; i++) {
        float th = M_TAU * (float)i / (float)seg;
        float ct = m_cos(th), st = m_sin(th);
        for (int j = 0; j < sides; j++) {
            float ph = M_TAU * (float)j / (float)sides;
            float d = R + r * m_cos(ph);
            m->v[i * sides + j] = v3(d * ct, r * m_sin(ph), d * st);
        }
    }
    for (int i = 0; i < seg; i++) {
        int i1 = (i + 1) % seg;
        for (int j = 0; j < sides; j++) {
            int j1 = (j + 1) % sides;
            int a = i * sides + j, b = i1 * sides + j, c = i * sides + j1, d = i1 * sides + j1;
            tri(m, &t, a, c, b, col);
            tri(m, &t, b, c, d, col);
        }
    }
    return m;
}

Mesh *mesh_plane(float sx, float sz, int div, u32 col)
{
    div = i_clamp(div, 1, 96);
    int n = div + 1;
    Mesh *m = mesh_new(n * n, div * div * 2);
    if (!m) return NULL;
    int t = 0;
    for (int j = 0; j < n; j++)
        for (int i = 0; i < n; i++)
            m->v[j * n + i] = v3(-sx * 0.5f + sx * (float)i / (float)div, 0, -sz * 0.5f + sz * (float)j / (float)div);
    for (int j = 0; j < div; j++)
        for (int i = 0; i < div; i++) {
            int a = j * n + i, b = a + 1, c = a + n, d = c + 1;
            tri(m, &t, a, c, b, col | TRI_TWOSIDED);
            tri(m, &t, b, c, d, col | TRI_TWOSIDED);
        }
    return m;
}

/* ---- Geo ------------------------------------------------------------------ */
Geo *geo_new(void)
{
    Geo *g = arena_alloc(sizeof *g);
    if (g) memset(g, 0, sizeof *g);
    return g;
}

Geo *geo_from_mesh(Mesh *m)
{
    if (!m) return NULL;
    Geo *g = geo_new();
    if (g) g->mesh = m;
    return g;
}

Geo *geo_realize(Geo *g)
{
    if (!g || !g->cl) return g;
    Geo *o = geo_new();
    if (!o) return NULL;

    if (g->sprites) {
        o->spr = arena_alloc(sizeof(Sprite) * (usize)(g->ncl + 1));
        if (!o->spr) return NULL;
        for (int i = 0; i < g->ncl; i++) {
            const Clone *c = &g->cl[i];
            o->spr[i].p = c->p;
            o->spr[i].r = g->sprite_r * (c->s.x + c->s.y + c->s.z) * (1.0f / 3.0f);
            o->spr[i].col = c->col & 0xFFFFFF;
        }
        o->nspr = g->ncl;
        return o;
    }

    const Mesh *src = g->mesh;
    if (!src || src->nt == 0 || g->ncl == 0) return o;
    int n = g->ncl;
    if (n > MAX_TRIS / src->nt) n = MAX_TRIS / src->nt;         /* triangle budget */
    Mesh *m = mesh_new(n * src->nv, n * src->nt);
    if (!m) return o;
    for (int i = 0; i < n; i++) {
        const Clone *c = &g->cl[i];
        Xf x = xf_trs(c->p, c->r, c->s);
        V3 *dv = m->v + i * src->nv;
        for (int k = 0; k < src->nv; k++) dv[k] = xf_point(&x, src->v[k]);
        int *di = m->idx + i * src->nt * 3, base = i * src->nv;
        for (int k = 0; k < src->nt * 3; k++) di[k] = src->idx[k] + base;
        u32 *dc = m->col + i * src->nt;
        if (c->col & CLONE_COLOR) {
            u32 col = c->col & 0xFFFFFF;
            for (int k = 0; k < src->nt; k++) dc[k] = col | (src->col[k] & TRI_TWOSIDED);
        } else {
            memcpy(dc, src->col, sizeof(u32) * (usize)src->nt);
        }
    }
    o->mesh = m;
    return o;
}

bool geo_bounds(const Geo *g, V3 *lo, V3 *hi)
{
    bool any = false;
    V3 a = v3(0, 0, 0), b = v3(0, 0, 0);
    if (!g) return false;
    if (g->mesh && !g->cl) {
        for (int i = 0; i < g->mesh->nv; i++) {
            V3 p = g->mesh->v[i];
            if (!any) { a = b = p; any = true; continue; }
            a = v3(m_min(a.x, p.x), m_min(a.y, p.y), m_min(a.z, p.z));
            b = v3(m_max(b.x, p.x), m_max(b.y, p.y), m_max(b.z, p.z));
        }
    }
    for (int i = 0; i < g->nspr; i++) {
        V3 p = g->spr[i].p;
        float r = g->spr[i].r;
        if (!any) { a = v3(p.x - r, p.y - r, p.z - r); b = v3(p.x + r, p.y + r, p.z + r); any = true; continue; }
        a = v3(m_min(a.x, p.x - r), m_min(a.y, p.y - r), m_min(a.z, p.z - r));
        b = v3(m_max(b.x, p.x + r), m_max(b.y, p.y + r), m_max(b.z, p.z + r));
    }
    for (int i = 0; g->cl && i < g->ncl; i++) {
        V3 p = g->cl[i].p;
        if (!any) { a = b = p; any = true; continue; }
        a = v3(m_min(a.x, p.x), m_min(a.y, p.y), m_min(a.z, p.z));
        b = v3(m_max(b.x, p.x), m_max(b.y, p.y), m_max(b.z, p.z));
    }
    *lo = a; *hi = b;
    return any;
}
