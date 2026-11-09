/* Flow - software 3D viewport renderer.
 *
 * A z-buffered scanline rasterizer: flat-shaded triangles, sphere-impostor
 * sprites (used for fluid particles) and depth-tested lines. Per-pixel work is
 * integer only; floating point is confined to per-vertex and per-span setup.
 *
 * View space: x right, y up, z forward. Depth is stored as near/z in 2.30
 * fixed point, so larger means closer and it interpolates linearly on screen.
 */
#include "flow/engine.h"
#include "lib/lib.h"

#define NEARZ   0.08f
#define DSCALE  1073741824.0f

typedef struct RT {
    u32 *col;
    int *z;
    int  w, h, stride;
    float cx, cy, f;            /* projection */
} RT;

typedef struct Light {
    V3 key;                     /* direction towards the light, view space */
    V3 up;                      /* world up, view space */
} Light;

/* ---- shading -------------------------------------------------------------- */
static inline u32 shade(u32 base, float diff, float hemi)
{
    float fr = 0.16f + 0.18f * hemi + 0.80f * diff;
    float fg = 0.15f + 0.22f * hemi + 0.76f * diff;
    float fb = 0.17f + 0.27f * hemi + 0.69f * diff;
    int r = (int)((float)((base >> 16) & 255) * fr);
    int g = (int)((float)((base >> 8) & 255) * fg);
    int b = (int)((float)(base & 255) * fb);
    return RGB(i_min(r, 255), i_min(g, 255), i_min(b, 255));
}

/* ---- triangles ------------------------------------------------------------ */
static void fill_tri(const RT *rt, float x0, float y0, float d0, float x1, float y1, float d1,
                     float x2, float y2, float d2, u32 col)
{
    float t;
#define SWAP01 { t = x0; x0 = x1; x1 = t; t = y0; y0 = y1; y1 = t; t = d0; d0 = d1; d1 = t; }
#define SWAP12 { t = x1; x1 = x2; x2 = t; t = y1; y1 = y2; y2 = t; t = d1; d1 = d2; d2 = t; }
    if (y1 < y0) SWAP01
    if (y2 < y1) SWAP12
    if (y1 < y0) SWAP01
#undef SWAP01
#undef SWAP12

    int iy0 = m_iceil(y0 - 0.5f), iy1 = m_iceil(y2 - 0.5f);
    if (iy0 < 0) iy0 = 0;
    if (iy1 > rt->h) iy1 = rt->h;
    if (iy0 >= iy1) return;

    float area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
    if (area > -1e-5f && area < 1e-5f) return;
    float ia = 1.0f / area;
    float ddx = ((d1 - d0) * (y2 - y0) - (d2 - d0) * (y1 - y0)) * ia;
    float ddy = ((d2 - d0) * (x1 - x0) - (d1 - d0) * (x2 - x0)) * ia;
    ddx = m_clamp(ddx, -1.9f, 1.9f);

    float s02 = (x2 - x0) / (y2 - y0);
    float s01 = y1 > y0 ? (x1 - x0) / (y1 - y0) : 0.0f;
    float s12 = y2 > y1 ? (x2 - x1) / (y2 - y1) : 0.0f;
    float wmax = (float)rt->w;

    for (int y = iy0; y < iy1; y++) {
        float yc = (float)y + 0.5f;
        float xa = x0 + (yc - y0) * s02;
        float xb = yc < y1 ? x0 + (yc - y0) * s01 : x1 + (yc - y1) * s12;
        if (xa > xb) { t = xa; xa = xb; xb = t; }
        if (xa < 0.0f) xa = 0.0f;
        if (xb > wmax) xb = wmax;
        int ix0 = m_iceil(xa - 0.5f), ix1 = m_iceil(xb - 0.5f);
        if (ix0 < 0) ix0 = 0;
        if (ix1 > rt->w) ix1 = rt->w;
        if (ix0 >= ix1) continue;

        /* Depth at both ends of the span, clamped: sliver triangles can have
         * gradients so steep that stepping blindly would leave the valid range. */
        int n = ix1 - ix0;
        float da = m_clamp(d0 + ((float)ix0 + 0.5f - x0) * ddx + (yc - y0) * ddy, 0.0f, 1.99f);
        float db = m_clamp(da + ddx * (float)(n - 1), 0.0f, 1.99f);
        int di = (int)(da * DSCALE);
        int ddi = n > 1 ? (int)((db - da) * DSCALE / (float)(n - 1)) : 0;
        int *zp = rt->z + y * rt->w + ix0;
        u32 *cp = rt->col + y * rt->stride + ix0;
        do {
            if (di > *zp) { *zp = di; *cp = col; }
            di = (int)((u32)di + (u32)ddi);
            zp++; cp++;
        } while (--n);
    }
}

static inline void project(const RT *rt, V3 v, float *sx, float *sy, float *sd)
{
    float iz = 1.0f / v.z;
    *sx = rt->cx + rt->f * v.x * iz;
    *sy = rt->cy - rt->f * v.y * iz;
    *sd = NEARZ * iz;
}

/* clip a triangle against the near plane and draw what is left */
static void clip_tri(const RT *rt, V3 a, V3 b, V3 c, u32 col)
{
    V3 in[3] = { a, b, c }, out[4];
    int n = 0;
    for (int i = 0; i < 3; i++) {
        V3 p = in[i], q = in[(i + 1) % 3];
        bool pin = p.z > NEARZ, qin = q.z > NEARZ;
        if (pin) out[n++] = p;
        if (pin != qin) {
            V3 x = v3_lerp(p, q, (NEARZ - p.z) / (q.z - p.z));
            x.z = NEARZ;
            out[n++] = x;
        }
    }
    if (n < 3) return;
    float sx[4], sy[4], sd[4];
    for (int i = 0; i < n; i++) project(rt, out[i], &sx[i], &sy[i], &sd[i]);
    for (int i = 1; i + 1 < n; i++)
        fill_tri(rt, sx[0], sy[0], sd[0], sx[i], sy[i], sd[i], sx[i + 1], sy[i + 1], sd[i + 1], col);
}

/* ---- depth-tested lines --------------------------------------------------- */
static void line_screen(const RT *rt, float x0, float y0, float d0, float x1, float y1, float d1,
                        u32 col, int alpha, float bias)
{
    /* Liang-Barsky clip to the viewport */
    float t0 = 0.0f, t1 = 1.0f, dx = x1 - x0, dy = y1 - y0;
    float p[4] = { -dx, dx, -dy, dy };
    float q[4] = { x0, (float)rt->w - 1.01f - x0, y0, (float)rt->h - 1.01f - y0 };
    for (int i = 0; i < 4; i++) {
        if (p[i] > -1e-6f && p[i] < 1e-6f) { if (q[i] < 0) return; continue; }
        float r = q[i] / p[i];
        if (p[i] < 0) { if (r > t1) return; if (r > t0) t0 = r; }
        else { if (r < t0) return; if (r < t1) t1 = r; }
    }
    float ax = x0 + dx * t0, ay = y0 + dy * t0, ad = d0 + (d1 - d0) * t0;
    float bx = x0 + dx * t1, by = y0 + dy * t1, bd = d0 + (d1 - d0) * t1;
    float len = m_max(m_abs(bx - ax), m_abs(by - ay));
    int n = (int)len + 1;
    float inv = 1.0f / (float)n;
    float sx = (bx - ax) * inv, sy = (by - ay) * inv, sdz = (bd - ad) * inv;
    ad *= 1.0f + bias;
    sdz *= 1.0f + bias;
    for (int i = 0; i <= n; i++) {
        int xi = (int)ax, yi = (int)ay;
        if (xi >= 0 && xi < rt->w && yi >= 0 && yi < rt->h) {
            int di = (int)(m_clamp(ad, 0.0f, 1.99f) * DSCALE);
            if (di >= rt->z[yi * rt->w + xi]) {
                u32 *cp = &rt->col[yi * rt->stride + xi];
                *cp = alpha >= 256 ? col : col_mix(*cp, col, alpha);
            }
        }
        ax += sx; ay += sy; ad += sdz;
    }
}

static void line_view(const RT *rt, V3 a, V3 b, u32 col, int alpha, float bias)
{
    if (a.z <= NEARZ && b.z <= NEARZ) return;
    if (a.z <= NEARZ) { a = v3_lerp(a, b, (NEARZ - a.z) / (b.z - a.z)); a.z = NEARZ; }
    if (b.z <= NEARZ) { b = v3_lerp(b, a, (NEARZ - b.z) / (a.z - b.z)); b.z = NEARZ; }
    float x0, y0, d0, x1, y1, d1;
    project(rt, a, &x0, &y0, &d0);
    project(rt, b, &x1, &y1, &d1);
    line_screen(rt, x0, y0, d0, x1, y1, d1, col, alpha, bias);
}

/* ---- sphere impostors ----------------------------------------------------- */
#define SPH_N 32
static u8 sph_nz[SPH_N * SPH_N];        /* 0 outside, else 1..255 = height of the sphere surface */
static u8 sph_lv[SPH_N * SPH_N];        /* shading level 0..15 */

static void sphere_tables(const Light *L)
{
    for (int v = 0; v < SPH_N; v++)
        for (int u = 0; u < SPH_N; u++) {
            float fx = ((float)u + 0.5f) / (SPH_N / 2) - 1.0f, fy = ((float)v + 0.5f) / (SPH_N / 2) - 1.0f;
            float r2 = fx * fx + fy * fy;
            int i = v * SPH_N + u;
            if (r2 >= 1.0f) { sph_nz[i] = 0; sph_lv[i] = 0; continue; }
            float z = m_sqrt(1.0f - r2);
            V3 n = v3(fx, -fy, -z);                     /* faces the camera */
            float diff = m_max(0.0f, v3_dot(n, L->key));
            float hemi = v3_dot(n, L->up) * 0.5f + 0.5f;
            float spec = m_max(0.0f, v3_dot(n, v3_norm(v3(L->key.x, L->key.y, L->key.z - 1.0f))));
            spec = spec * spec; spec = spec * spec; spec = spec * spec; spec = spec * spec;
            float lum = 0.20f + 0.25f * hemi + 0.72f * diff + 0.55f * spec;
            sph_nz[i] = (u8)(1 + (int)(z * 254.0f));
            sph_lv[i] = (u8)i_clamp((int)(lum * 11.0f), 0, 15);
        }
}

static void draw_sprite(const RT *rt, float sx, float sy, float R, float dc, float dr, u32 base)
{
    int di = (int)(m_clamp(dc, 0.0f, 1.9f) * DSCALE);
    if (R < 0.9f) {                                     /* too small for the table: a single pixel */
        int x = (int)sx, y = (int)sy;
        if (x >= 0 && x < rt->w && y >= 0 && y < rt->h && di > rt->z[y * rt->w + x]) {
            rt->z[y * rt->w + x] = di;
            rt->col[y * rt->stride + x] = col_scale(base, 200);
        }
        return;
    }
    u32 ramp[16];
    for (int i = 0; i < 16; i++) {
        u32 c = col_scale(base, i * 256 / 11);
        ramp[i] = i > 12 ? col_mix(c, 0xFFFFFF, (i - 12) * 60) : c;
    }
    int dri = (int)(m_clamp(dr, 0.0f, 0.05f) * DSCALE);
    int x0 = i_max(m_ifloor(sx - R), 0), x1 = i_min(m_iceil(sx + R), rt->w);
    int y0 = i_max(m_ifloor(sy - R), 0), y1 = i_min(m_iceil(sy + R), rt->h);
    if (x0 >= x1 || y0 >= y1) return;
    float k = (float)(SPH_N / 2) / R;
    int step = (int)(k * 65536.0f);
    int tv = (int)((((float)y0 + 0.5f - sy) * k + (float)(SPH_N / 2)) * 65536.0f);
    int tu0 = (int)((((float)x0 + 0.5f - sx) * k + (float)(SPH_N / 2)) * 65536.0f);
    for (int y = y0; y < y1; y++, tv += step) {
        int iv = tv >> 16;
        if (iv < 0 || iv >= SPH_N) continue;
        const u8 *nz = sph_nz + iv * SPH_N, *lv = sph_lv + iv * SPH_N;
        int *zp = rt->z + y * rt->w;
        u32 *cp = rt->col + y * rt->stride;
        int tu = tu0;
        for (int x = x0; x < x1; x++, tu += step) {
            int iu = tu >> 16;
            if (iu < 0 || iu >= SPH_N || !nz[iu]) continue;
            int d = di + ((dri >> 8) * nz[iu]);
            if (d > zp[x]) { zp[x] = d; cp[x] = ramp[lv[iu]]; }
        }
    }
}

/* ---- frame ---------------------------------------------------------------- */
void render_view(Surface *s, Rect vp, int *zbuf, const Scene *sc, const Geo *geo,
                 const Guide *guides, int nguides, RenderStats *stats)
{
    RT rt;
    rt.col = s->px + vp.y * s->stride + vp.x;
    rt.z = zbuf;
    rt.w = vp.w; rt.h = vp.h; rt.stride = s->stride;
    rt.cx = (float)vp.w * 0.5f; rt.cy = (float)vp.h * 0.5f;
    rt.f = (float)vp.h * 1.07f;                         /* ~50 degree vertical field of view */
    if (stats) { stats->tris = 0; stats->drawn = 0; stats->sprites = 0; }
    if (vp.w <= 0 || vp.h <= 0) return;

    for (int y = 0; y < vp.h; y++)
        mem_set32(rt.col + y * rt.stride, col_mix(0x343A48, 0x191B21, y * 256 / vp.h), (usize)vp.w);
    mem_set32((u32 *)zbuf, 0, (usize)vp.w * (usize)vp.h);

    /* orbit camera */
    float cp = m_cos(sc->pitch), sp = m_sin(sc->pitch), cyw = m_cos(sc->yaw), syw = m_sin(sc->yaw);
    V3 eye = v3_add(sc->target, v3_mul(v3(cp * syw, sp, cp * cyw), sc->dist));
    V3 fwd = v3_norm(v3_sub(sc->target, eye));
    V3 right = v3_norm(v3_cross(fwd, v3(0, 1, 0)));
    V3 up = v3_cross(right, fwd);
#define TO_VIEW(p) v3(v3_dot(v3_sub(p, eye), right), v3_dot(v3_sub(p, eye), up), v3_dot(v3_sub(p, eye), fwd))

    V3 lw = v3_norm(v3(0.45f, 0.85f, 0.40f));
    Light L;
    L.key = v3(v3_dot(lw, right), v3_dot(lw, up), v3_dot(lw, fwd));
    L.up = v3(right.y, up.y, fwd.y);

    usize mark = arena_mark();
    const Mesh *m = geo ? geo->mesh : NULL;
    if (geo && geo->cl) m = NULL;                       /* unrealized clone sets are not drawable */

    V3 *vv = NULL;
    float *px = NULL, *py = NULL, *pd = NULL;
    if (m && m->nt > 0) {
        vv = arena_alloc(sizeof(V3) * (usize)m->nv);
        px = arena_alloc(sizeof(float) * (usize)m->nv);
        py = arena_alloc(sizeof(float) * (usize)m->nv);
        pd = arena_alloc(sizeof(float) * (usize)m->nv);
    }
    if (vv && px && py && pd) {
        for (int i = 0; i < m->nv; i++) {
            V3 v = TO_VIEW(m->v[i]);
            vv[i] = v;
            if (v.z > NEARZ) project(&rt, v, &px[i], &py[i], &pd[i]);
        }
        float wmax = (float)vp.w, hmax = (float)vp.h;
        int drawn = 0;
        for (int t = 0; t < m->nt; t++) {
            int ia = m->idx[t * 3], ib = m->idx[t * 3 + 1], ic = m->idx[t * 3 + 2];
            V3 a = vv[ia], b = vv[ib], c = vv[ic];
            int behind = (a.z <= NEARZ) + (b.z <= NEARZ) + (c.z <= NEARZ);
            if (behind == 3) continue;
            V3 n = v3_cross(v3_sub(b, a), v3_sub(c, a));
            float facing = v3_dot(n, a);                /* > 0: front face (view space is left-handed) */
            u32 col = m->col[t];
            if (facing <= 0 && !(col & TRI_TWOSIDED)) continue;
            float nl = v3_dot(n, n);
            if (nl < 1e-18f) continue;
            float inv = (facing > 0 ? -1.0f : 1.0f) / m_sqrt(nl);
            n = v3_mul(n, inv);
            u32 lit = shade(col, m_max(0.0f, v3_dot(n, L.key)), v3_dot(n, L.up) * 0.5f + 0.5f);
            if (behind) { clip_tri(&rt, a, b, c, lit); drawn++; continue; }
            float ax = px[ia], bx = px[ib], cx = px[ic], ay = py[ia], by = py[ib], cy = py[ic];
            if ((ax < 0 && bx < 0 && cx < 0) || (ax > wmax && bx > wmax && cx > wmax)) continue;
            if ((ay < 0 && by < 0 && cy < 0) || (ay > hmax && by > hmax && cy > hmax)) continue;
            fill_tri(&rt, ax, ay, pd[ia], bx, by, pd[ib], cx, cy, pd[ic], lit);
            drawn++;
        }
        if (sc->wire) {
            for (int t = 0; t < m->nt; t++) {
                int ia = m->idx[t * 3], ib = m->idx[t * 3 + 1], ic = m->idx[t * 3 + 2];
                if (vv[ia].z <= NEARZ || vv[ib].z <= NEARZ || vv[ic].z <= NEARZ) continue;
                V3 n = v3_cross(v3_sub(vv[ib], vv[ia]), v3_sub(vv[ic], vv[ia]));
                if (v3_dot(n, vv[ia]) <= 0 && !(m->col[t] & TRI_TWOSIDED)) continue;
                line_screen(&rt, px[ia], py[ia], pd[ia], px[ib], py[ib], pd[ib], 0x000000, 110, 0.004f);
                line_screen(&rt, px[ib], py[ib], pd[ib], px[ic], py[ic], pd[ic], 0x000000, 110, 0.004f);
                line_screen(&rt, px[ic], py[ic], pd[ic], px[ia], py[ia], pd[ia], 0x000000, 110, 0.004f);
            }
        }
        if (stats) { stats->tris = m->nt; stats->drawn = drawn; }
    }

    if (geo && geo->nspr > 0 && !geo->cl) {
        sphere_tables(&L);
        for (int i = 0; i < geo->nspr; i++) {
            const Sprite *sp_ = &geo->spr[i];
            V3 v = TO_VIEW(sp_->p);
            if (v.z - sp_->r <= NEARZ) continue;
            float iz = 1.0f / v.z;
            float R = rt.f * sp_->r * iz;
            float sx = rt.cx + rt.f * v.x * iz, sy = rt.cy - rt.f * v.y * iz;
            if (sx + R < 0 || sx - R > (float)vp.w || sy + R < 0 || sy - R > (float)vp.h) continue;
            float dc = NEARZ * iz;
            draw_sprite(&rt, sx, sy, R, dc, NEARZ / (v.z - sp_->r) - dc, sp_->col);
        }
        if (stats) stats->sprites = geo->nspr;
    }

    if (sc->grid) {                                     /* ground grid on y = 0 */
        const int G = 10;
        for (int i = -G; i <= G; i++) {
            u32 cx_ = i == 0 ? 0x5C7FBF : 0x6B7385, cz_ = i == 0 ? 0xBF5C5C : 0x6B7385;
            int a = i == 0 ? 150 : (i % 5 == 0 ? 95 : 48);
            line_view(&rt, TO_VIEW(v3((float)i, 0, (float)-G)), TO_VIEW(v3((float)i, 0, (float)G)), cx_, a, 0.0f);
            line_view(&rt, TO_VIEW(v3((float)-G, 0, (float)i)), TO_VIEW(v3((float)G, 0, (float)i)), cz_, a, 0.0f);
        }
    }
    for (int i = 0; i < nguides; i++)
        line_view(&rt, TO_VIEW(guides[i].a), TO_VIEW(guides[i].b), guides[i].col, 200, 0.002f);
#undef TO_VIEW

    /* orientation gizmo, bottom left */
    int ox0 = s->cx0, oy0 = s->cy0, ox1 = s->cx1, oy1 = s->cy1;
    gfx_clip_rect(s, vp);
    float gx = (float)vp.x + 34.0f, gy = (float)(vp.y + vp.h) - 34.0f;
    const V3 axes[3] = { v3(right.x, up.x, fwd.x), v3(right.y, up.y, fwd.y), v3(right.z, up.z, fwd.z) };
    const u32 acol[3] = { 0xE25555, 0x62C462, 0x5590E2 };
    const char *const aname[3] = { "X", "Y", "Z" };
    for (int pass = 0; pass < 2; pass++)                /* draw axes pointing away first */
        for (int i = 0; i < 3; i++) {
            if ((axes[i].z < 0) != (pass == 1)) continue;
            float ex = gx + axes[i].x * 22.0f, ey = gy - axes[i].y * 22.0f;
            gfx_line_aa(s, gx, gy, ex, ey, acol[i], true);
            gfx_text(s, (int)(gx + axes[i].x * 30.0f) - 2, (int)(gy - axes[i].y * 30.0f) - 6, aname[i], acol[i]);
        }
    s->cx0 = ox0; s->cy0 = oy0; s->cx1 = ox1; s->cy1 = oy1;

    arena_release(mark);
}
