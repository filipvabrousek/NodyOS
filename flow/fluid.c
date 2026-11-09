/* Flow - particle fluid.
 *
 * A position-based viscoelastic solver in the spirit of smoothed-particle
 * hydrodynamics: every step predicts positions from velocities, measures the
 * local density around each particle with a short-range kernel, and pushes
 * neighbouring particles apart (or together) until the density approaches the
 * rest density ("double density relaxation"). A second, sharper kernel keeps
 * particles from clumping. Velocities are then recovered from the corrected
 * positions, which makes the scheme unconditionally stable.
 *
 * Neighbours are found with a uniform grid whose cell size equals the kernel
 * radius; each interacting pair is visited exactly once.
 */
#include "flow/engine.h"
#include "lib/lib.h"
#include "lib/plat.h"

#define FL_MAX      1400
#define FL_H        0.30f               /* kernel radius */
#define FL_PAIRS    40000
#define FL_GMAX     30                  /* grid cells along x and z */
#define FL_GY       40
#define FL_SUBSTEPS 2
#define FL_REST     3.4f                /* rest density */
#define FL_K        34.0f               /* stiffness */
#define FL_KNEAR    52.0f               /* near stiffness */
#define FL_TENSION  0.12f               /* how much of the negative pressure is kept */
#define FL_PR       0.065f              /* particle radius against walls */
#define FL_VMAX     14.0f

typedef struct Pair { u16 a, b; float q, nx, ny, nz; } Pair;

typedef struct Collider { int type; V3 c; float r; V3 lo, hi; } Collider;

typedef struct Fluid {
    int   n, cursor;
    int   frame;
    u32   sig, rng;
    float px[FL_MAX], py[FL_MAX], pz[FL_MAX];
    float vx[FL_MAX], vy[FL_MAX], vz[FL_MAX];
    float ox[FL_MAX], oy[FL_MAX], oz[FL_MAX];
    float rho[FL_MAX], rhon[FL_MAX];
    int   next[FL_MAX];
    int   head[FL_GMAX * FL_GY * FL_GMAX];
    int   npairs;
    Pair  pairs[FL_PAIRS];
} Fluid;

static inline float frand(Fluid *f)
{
    f->rng = f->rng * 1664525u + 1013904223u;
    return (float)(f->rng >> 8) * (1.0f / 16777216.0f);
}

static void spawn(Fluid *f, float x, float y, float z, float vx, float vy, float vz)
{
    int i;
    if (f->n < FL_MAX) i = f->n++;
    else return;
    f->px[i] = x; f->py[i] = y; f->pz[i] = z;
    f->vx[i] = vx; f->vy[i] = vy; f->vz[i] = vz;
}

static void respawn(Fluid *f, int i, float x, float y, float z, float vx, float vy, float vz)
{
    f->px[i] = x; f->py[i] = y; f->pz[i] = z;
    f->vx[i] = vx; f->vy[i] = vy; f->vz[i] = vz;
}

/* one new particle according to the emitter settings; index < 0 appends */
static void emit_one(Fluid *f, const Node *n, int index)
{
    int mode = m_iround(n->p[0]);
    float hx = n->p[10] * 0.5f - FL_PR, hz = n->p[11] * 0.5f - FL_PR;
    float x, y, z, vx = 0, vy = 0, vz = 0;
    if (mode == 2) {                                    /* rain over the whole tank */
        x = (frand(f) * 2 - 1) * hx;
        z = (frand(f) * 2 - 1) * hz;
        y = n->p[4] + frand(f) * 0.3f;
        vy = -1.0f;
    } else {                                            /* stream from a nozzle */
        /* Size the nozzle so the jet leaves at about rest density: the volume
         * swept per frame has to hold the particles emitted in that frame. */
        float jet = n->p[6];
        float swept = m_max(m_abs(jet), 1.2f) * (1.0f / FLOW_FPS);
        float radius = m_clamp(m_sqrt(n->p[2] * 0.0042f / (swept * M_PI)), 0.08f, 0.45f);
        float a = frand(f) * M_TAU, r = m_sqrt(frand(f)) * radius;
        x = n->p[3] + frand(f) * jet * (1.0f / FLOW_FPS);
        y = n->p[4] + m_cos(a) * r;
        z = n->p[5] + m_sin(a) * r;
        vx = jet + (frand(f) - 0.5f) * 0.2f;
        vy = (frand(f) - 0.5f) * 0.2f;
        vz = (frand(f) - 0.5f) * 0.2f;
    }
    x = m_clamp(x, -hx, hx);
    z = m_clamp(z, -hz, hz);
    if (index < 0) spawn(f, x, y, z, vx, vy, vz);
    else respawn(f, index, x, y, z, vx, vy, vz);
}

static void reset(Fluid *f, const Node *n)
{
    f->n = 0;
    f->cursor = 0;
    f->rng = 0x1234567u;
    f->frame = 0;
    if (m_iround(n->p[0]) == 1) {                       /* block: a slab of fluid dropped at once */
        int count = i_clamp(m_iround(n->p[1]), 1, FL_MAX);
        const float s = 0.165f;
        float hx = n->p[10] * 0.5f - FL_PR, hz = n->p[11] * 0.5f - FL_PR;
        int bz = i_max(1, (int)(hz * 2.0f / s));
        int bx = i_max(1, i_min((int)(hx * 2.0f / s), (int)(m_sqrt((float)count / (float)bz * 0.6f) + 1.0f)));
        int layers = (count + bx * bz - 1) / (bx * bz);
        float x0 = m_clamp(n->p[3] - (float)bx * s * 0.5f, -hx, hx - (float)bx * s);
        float y0 = m_max(n->p[4] - (float)layers * s * 0.5f, s * 0.5f);
        for (int i = 0; i < count; i++) {
            int ix = i % bx, iz = (i / bx) % bz, iy = i / (bx * bz);
            spawn(f, x0 + ((float)ix + 0.5f + (frand(f) - 0.5f) * 0.2f) * s,
                  y0 + (float)iy * s,
                  -hz + ((float)iz + 0.5f + (frand(f) - 0.5f) * 0.2f) * s, 0, 0, 0);
        }
    }
}

static inline int cell_of(float v, float origin, int count)
{
    return i_clamp((int)((v - origin) * (1.0f / FL_H)) + 1, 0, count - 1);
}

static inline void try_pair(Fluid *f, int i, int j)
{
    float dx = f->px[j] - f->px[i], dy = f->py[j] - f->py[i], dz = f->pz[j] - f->pz[i];
    float r2 = dx * dx + dy * dy + dz * dz;
    if (r2 >= FL_H * FL_H || f->npairs >= FL_PAIRS) return;
    Pair *p = &f->pairs[f->npairs++];
    p->a = (u16)i; p->b = (u16)j;
    if (r2 < 1e-10f) {                                  /* coincident: separate along an arbitrary axis */
        p->q = 1.0f; p->nx = 0; p->ny = 1; p->nz = 0;
        return;
    }
    float r = m_sqrt(r2), ir = 1.0f / r;
    p->q = 1.0f - r * (1.0f / FL_H);
    p->nx = dx * ir; p->ny = dy * ir; p->nz = dz * ir;
}

static void find_pairs(Fluid *f, float hx, float hz, int gx, int gz)
{
    /* the 13 "forward" neighbours of a cell: every pair of cells is visited once */
    static const i8 fwd[13][3] = {
        { 1, 0, 0 }, { -1, 1, 0 }, { 0, 1, 0 }, { 1, 1, 0 },
        { -1, -1, 1 }, { 0, -1, 1 }, { 1, -1, 1 }, { -1, 0, 1 }, { 0, 0, 1 }, { 1, 0, 1 },
        { -1, 1, 1 }, { 0, 1, 1 }, { 1, 1, 1 }
    };
    int cells = gx * FL_GY * gz;
    for (int i = 0; i < cells; i++) f->head[i] = -1;
    for (int i = 0; i < f->n; i++) {
        int c = cell_of(f->px[i], -hx, gx) + gx * (cell_of(f->py[i], 0, FL_GY) + FL_GY * cell_of(f->pz[i], -hz, gz));
        f->next[i] = f->head[c];
        f->head[c] = i;
    }
    f->npairs = 0;
    for (int i = 0; i < f->n; i++) {
        int cx = cell_of(f->px[i], -hx, gx), cy = cell_of(f->py[i], 0, FL_GY), cz = cell_of(f->pz[i], -hz, gz);
        for (int j = f->next[i]; j >= 0; j = f->next[j]) try_pair(f, i, j);
        for (int k = 0; k < 13; k++) {
            int nx = cx + fwd[k][0], ny = cy + fwd[k][1], nz = cz + fwd[k][2];
            if (nx < 0 || nx >= gx || ny < 0 || ny >= FL_GY || nz < 0 || nz >= gz) continue;
            for (int j = f->head[nx + gx * (ny + FL_GY * nz)]; j >= 0; j = f->next[j]) try_pair(f, i, j);
        }
    }
}

static inline void collide(Fluid *f, int i, float hx, float hz, const Collider *col)
{
    float x = f->px[i], y = f->py[i], z = f->pz[i];
    if (col->type == 1) {
        float dx = x - col->c.x, dy = y - col->c.y, dz = z - col->c.z;
        float d2 = dx * dx + dy * dy + dz * dz, R = col->r + FL_PR;
        if (d2 < R * R) {
            float d = m_sqrt(d2);
            if (d < 1e-5f) { dy = 1; d = 1; dx = dz = 0; }
            float k = R / d;
            x = col->c.x + dx * k; y = col->c.y + dy * k; z = col->c.z + dz * k;
        }
    } else if (col->type == 2) {
        float ax = x - (col->lo.x - FL_PR), bx = (col->hi.x + FL_PR) - x;
        float ay = y - (col->lo.y - FL_PR), by = (col->hi.y + FL_PR) - y;
        float az = z - (col->lo.z - FL_PR), bz = (col->hi.z + FL_PR) - z;
        if (ax > 0 && bx > 0 && ay > 0 && by > 0 && az > 0 && bz > 0) {
            float m = m_min(m_min(m_min(ax, bx), m_min(ay, by)), m_min(az, bz));   /* shallowest way out */
            if (m == by) y += by;
            else if (m == ax) x -= ax;
            else if (m == bx) x += bx;
            else if (m == az) z -= az;
            else if (m == bz) z += bz;
            else y -= ay;
        }
    }
    if (y < FL_PR) y = FL_PR;
    if (x < -hx) x = -hx; else if (x > hx) x = hx;
    if (z < -hz) z = -hz; else if (z > hz) z = hz;
    f->px[i] = x; f->py[i] = y; f->pz[i] = z;
}

static void step(Fluid *f, const Node *n, float dt, const Collider *col)
{
    float grav = n->p[7], visc = n->p[8], stiff = n->p[9];
    float hx = n->p[10] * 0.5f - FL_PR, hz = n->p[11] * 0.5f - FL_PR;
    int gx = i_clamp((int)(n->p[10] / FL_H) + 3, 3, FL_GMAX), gz = i_clamp((int)(n->p[11] / FL_H) + 3, 3, FL_GMAX);
    int np = f->n;

    for (int i = 0; i < np; i++) {                      /* predict */
        f->vy[i] -= grav * dt;
        f->ox[i] = f->px[i]; f->oy[i] = f->py[i]; f->oz[i] = f->pz[i];
        f->px[i] += f->vx[i] * dt; f->py[i] += f->vy[i] * dt; f->pz[i] += f->vz[i] * dt;
        f->rho[i] = 0; f->rhon[i] = 0;
    }

    find_pairs(f, hx, hz, gx, gz);

    for (int k = 0; k < f->npairs; k++) {               /* density and near density */
        const Pair *p = &f->pairs[k];
        float q2 = p->q * p->q, q3 = q2 * p->q;
        f->rho[p->a] += q2; f->rho[p->b] += q2;
        f->rhon[p->a] += q3; f->rhon[p->b] += q3;
    }
    float kp = FL_K * stiff * dt * dt * 0.5f, kn = FL_KNEAR * stiff * dt * dt * 0.5f;
    for (int k = 0; k < f->npairs; k++) {               /* relax: move pairs along their axis */
        const Pair *p = &f->pairs[k];
        int a = p->a, b = p->b;
        float press = (f->rho[a] - FL_REST) + (f->rho[b] - FL_REST);
        if (press < 0.0f) press *= FL_TENSION;          /* weak cohesion: enough for droplets, not for clumps */
        float near = f->rhon[a] + f->rhon[b];
        float d = m_clamp((kp * press + kn * near * p->q) * p->q, -0.012f, 0.024f);    /* never explosive */
        float dx = p->nx * d, dy = p->ny * d, dz = p->nz * d;
        f->px[a] -= dx; f->py[a] -= dy; f->pz[a] -= dz;
        f->px[b] += dx; f->py[b] += dy; f->pz[b] += dz;
    }

    float idt = 1.0f / dt;
    for (int i = 0; i < np; i++) {                      /* boundaries, then velocity from motion */
        collide(f, i, hx, hz, col);
        float vx = (f->px[i] - f->ox[i]) * idt, vy = (f->py[i] - f->oy[i]) * idt, vz = (f->pz[i] - f->oz[i]) * idt;
        float v2 = vx * vx + vy * vy + vz * vz;
        if (v2 > FL_VMAX * FL_VMAX) { float s = FL_VMAX / m_sqrt(v2); vx *= s; vy *= s; vz *= s; }
        f->vx[i] = vx; f->vy[i] = vy; f->vz[i] = vz;
    }

    if (visc > 0.0f) {                                  /* viscosity: damp approach speed of neighbours */
        float sigma = visc * 6.0f * dt, beta = visc * 1.5f * dt;
        for (int k = 0; k < f->npairs; k++) {
            const Pair *p = &f->pairs[k];
            int a = p->a, b = p->b;
            float u = (f->vx[a] - f->vx[b]) * p->nx + (f->vy[a] - f->vy[b]) * p->ny + (f->vz[a] - f->vz[b]) * p->nz;
            if (u <= 0.0f) continue;
            float imp = m_min(p->q * (sigma * u + beta * u * u), u) * 0.5f;
            f->vx[a] -= imp * p->nx; f->vy[a] -= imp * p->ny; f->vz[a] -= imp * p->nz;
            f->vx[b] += imp * p->nx; f->vy[b] += imp * p->ny; f->vz[b] += imp * p->nz;
        }
    }
}

int fluid_count(const Node *n)
{
    const Fluid *f = n->state;
    return f ? f->n : 0;
}

void fluid_free(void *state) { kfree(state); }

Geo *fluid_eval(Scene *sc, Node *n)
{
    Fluid *f = n->state;
    if (!f) {
        f = kmalloc(sizeof *f);
        if (!f) return NULL;
        memset(f, 0, sizeof *f);
        f->frame = -1;
        f->sig = 0xFFFFFFFFu;
        n->state = f;
    }
    int count = i_clamp(m_iround(n->p[1]), 1, FL_MAX);
    int mode = m_iround(n->p[0]);
    u32 sig = (u32)mode * 7919u + (u32)count * 31u + (u32)(n->p[10] * 100.0f) * 131u + (u32)(n->p[11] * 100.0f) * 1031u;

    /* optional collider: bounding sphere or box of whatever is plugged in */
    Collider col;
    col.type = 0;
    Geo *cg = n->in[0] >= 0 ? geo_realize(node_eval(sc, n->in[0])) : NULL;
    if (cg && geo_bounds(cg, &col.lo, &col.hi)) {
        col.type = m_iround(n->p[15]) == 1 ? 2 : 1;
        col.c = v3_mul(v3_add(col.lo, col.hi), 0.5f);
        V3 e = v3_sub(col.hi, col.lo);
        col.r = (e.x + e.y + e.z) * (1.0f / 6.0f);
    }

    if (sig != f->sig || sc->frame < f->frame || sc->frame == 0) {
        reset(f, n);
        f->sig = sig;
        f->frame = sc->frame;
    } else if (sc->frame > f->frame) {
        f->frame = sc->frame;
        if (mode != 1) {                                /* emit */
            int rate = i_clamp(m_iround(n->p[2]), 1, 60);
            for (int k = 0; k < rate; k++) {
                if (f->n < count) emit_one(f, n, -1);
                else if (n->p[16] > 0.5f && f->n > 0) { emit_one(f, n, f->cursor % f->n); f->cursor = (f->cursor + 1) % f->n; }
            }
        }
        if (f->n > count) f->n = count;
        for (int s = 0; s < FL_SUBSTEPS; s++) step(f, n, 1.0f / (FLOW_FPS * FL_SUBSTEPS), &col);
    }

    Geo *g = geo_new();
    Clone *cl = arena_alloc(sizeof(Clone) * (usize)(f->n + 1));
    if (!g || !cl) return NULL;
    u32 base = (u32)n->p[13] & 0xFFFFFF;
    float foam = n->p[14] * (1.0f / 5.0f);
    float inv = f->n > 1 ? 1.0f / (float)(f->n - 1) : 0.0f;
    for (int i = 0; i < f->n; i++) {
        float speed = m_sqrt(f->vx[i] * f->vx[i] + f->vy[i] * f->vy[i] + f->vz[i] * f->vz[i]);
        cl[i].p = v3(f->px[i], f->py[i], f->pz[i]);
        cl[i].r = v3(0, 0, 0);
        cl[i].s = v3(1, 1, 1);
        cl[i].t = (float)i * inv;
        cl[i].col = CLONE_COLOR | col_mix(base, 0xFFFFFF, (int)(m_clamp(speed * foam, 0.0f, 0.85f) * 256.0f));
    }
    g->cl = cl;
    g->ncl = f->n;
    g->sprites = true;
    g->sprite_r = n->p[12];
    return g;
}
