/* NodyOS - 2D drawing on 32-bit XRGB surfaces. Everything clips. */
#include "gui/gfx.h"
#include "lib/lib.h"
#include "lib/kmath.h"

/* ---- colour helpers ------------------------------------------------------- */
static inline u32 blend(u32 d, u32 s, int a)
{
    u32 rb = (((s & 0xFF00FF) * (u32)a + (d & 0xFF00FF) * (u32)(256 - a)) >> 8) & 0xFF00FF;
    u32 g  = (((s & 0x00FF00) * (u32)a + (d & 0x00FF00) * (u32)(256 - a)) >> 8) & 0x00FF00;
    return rb | g;
}

u32 col_mix(u32 a, u32 b, int t) { return blend(a, b, i_clamp(t, 0, 256)); }

u32 col_scale(u32 c, int k)
{
    int r = (int)((c >> 16) & 255) * k >> 8, g = (int)((c >> 8) & 255) * k >> 8, b = (int)(c & 255) * k >> 8;
    return RGB(i_min(r, 255), i_min(g, 255), i_min(b, 255));
}

u32 col_hsv(float h, float s, float v)
{
    h = (h - m_floor(h)) * 6.0f;
    int i = (int)h;
    float f = h - (float)i;
    float p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
    float r, g, b;
    switch (i) {
    case 0: r = v; g = t; b = p; break;
    case 1: r = q; g = v; b = p; break;
    case 2: r = p; g = v; b = t; break;
    case 3: r = p; g = q; b = v; break;
    case 4: r = t; g = p; b = v; break;
    default: r = v; g = p; b = q; break;
    }
    return RGB((int)(m_clamp(r, 0, 1) * 255), (int)(m_clamp(g, 0, 1) * 255), (int)(m_clamp(b, 0, 1) * 255));
}

/* ---- surfaces and clipping ------------------------------------------------ */
void surf_init(Surface *s, u32 *px, int w, int h, int stride)
{
    s->px = px; s->w = w; s->h = h; s->stride = stride;
    s->cx0 = 0; s->cy0 = 0; s->cx1 = w; s->cy1 = h;
}

void gfx_clip(Surface *s, int x, int y, int w, int h)
{
    s->cx0 = i_clamp(x, 0, s->w);
    s->cy0 = i_clamp(y, 0, s->h);
    s->cx1 = i_clamp(x + w, s->cx0, s->w);
    s->cy1 = i_clamp(y + h, s->cy0, s->h);
}

void gfx_clip_rect(Surface *s, Rect r) { gfx_clip(s, r.x, r.y, r.w, r.h); }
void gfx_unclip(Surface *s) { s->cx0 = 0; s->cy0 = 0; s->cx1 = s->w; s->cy1 = s->h; }

static inline void plot(Surface *s, int x, int y, u32 c)
{
    if (x >= s->cx0 && x < s->cx1 && y >= s->cy0 && y < s->cy1) s->px[y * s->stride + x] = c;
}

static inline void plot_a(Surface *s, int x, int y, u32 c, int a)
{
    if (a <= 0) return;
    if (x >= s->cx0 && x < s->cx1 && y >= s->cy0 && y < s->cy1) {
        u32 *p = &s->px[y * s->stride + x];
        *p = a >= 256 ? c : blend(*p, c, a);
    }
}

/* ---- rectangles ----------------------------------------------------------- */
void gfx_fill(Surface *s, int x, int y, int w, int h, u32 c)
{
    int x0 = i_max(x, s->cx0), y0 = i_max(y, s->cy0);
    int x1 = i_min(x + w, s->cx1), y1 = i_min(y + h, s->cy1);
    if (x0 >= x1 || y0 >= y1) return;
    for (int yy = y0; yy < y1; yy++) mem_set32(s->px + yy * s->stride + x0, c, (usize)(x1 - x0));
}

void gfx_fill_a(Surface *s, int x, int y, int w, int h, u32 c, int alpha)
{
    int x0 = i_max(x, s->cx0), y0 = i_max(y, s->cy0);
    int x1 = i_min(x + w, s->cx1), y1 = i_min(y + h, s->cy1);
    if (x0 >= x1 || y0 >= y1 || alpha <= 0) return;
    if (alpha >= 256) { gfx_fill(s, x, y, w, h, c); return; }
    for (int yy = y0; yy < y1; yy++) {
        u32 *p = s->px + yy * s->stride + x0;
        for (int n = x1 - x0; n > 0; n--, p++) *p = blend(*p, c, alpha);
    }
}

void gfx_frame(Surface *s, int x, int y, int w, int h, u32 c)
{
    gfx_fill(s, x, y, w, 1, c);
    gfx_fill(s, x, y + h - 1, w, 1, c);
    gfx_fill(s, x, y + 1, 1, h - 2, c);
    gfx_fill(s, x + w - 1, y + 1, 1, h - 2, c);
}

void gfx_vgrad(Surface *s, int x, int y, int w, int h, u32 top, u32 bottom)
{
    if (h <= 0) return;
    for (int j = 0; j < h; j++) {
        if (y + j < s->cy0 || y + j >= s->cy1) continue;
        gfx_fill(s, x, y + j, w, 1, blend(top, bottom, h > 1 ? j * 256 / (h - 1) : 0));
    }
}

/* filled rectangle with anti-aliased rounded corners and a vertical gradient */
void gfx_rrect_grad(Surface *s, int x, int y, int w, int h, int r, u32 top, u32 bottom)
{
    if (w <= 0 || h <= 0) return;
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    for (int j = 0; j < h; j++) {
        if (y + j < s->cy0 || y + j >= s->cy1) continue;
        u32 c = top == bottom ? top : blend(top, bottom, h > 1 ? j * 256 / (h - 1) : 0);
        int k = j < r ? j : (j >= h - r ? h - 1 - j : -1);
        int inset = 0;
        if (k >= 0) {
            float dy = (float)r - 0.5f - (float)k;
            float in = (float)r - m_sqrt(m_max((float)(r * r) - dy * dy, 0.0f));
            inset = (int)in;
            int cover = (int)((1.0f - (in - (float)inset)) * 256.0f);
            if (cover < 250) {
                plot_a(s, x + inset, y + j, c, cover);
                plot_a(s, x + w - 1 - inset, y + j, c, cover);
                inset++;
            }
        }
        gfx_fill(s, x + inset, y + j, w - 2 * inset, 1, c);
    }
}

void gfx_rrect(Surface *s, int x, int y, int w, int h, int r, u32 c)
{
    gfx_rrect_grad(s, x, y, w, h, r, c, c);
}

void gfx_rframe(Surface *s, int x, int y, int w, int h, int r, u32 border, u32 fill)
{
    gfx_rrect(s, x, y, w, h, r, border);
    gfx_rrect(s, x + 1, y + 1, w - 2, h - 2, r > 0 ? r - 1 : 0, fill);
}

/* ---- lines, discs, triangles ---------------------------------------------- */
void gfx_line(Surface *s, int x0, int y0, int x1, int y1, u32 c)
{
    if (y0 == y1) { if (x0 > x1) { int t = x0; x0 = x1; x1 = t; } gfx_fill(s, x0, y0, x1 - x0 + 1, 1, c); return; }
    if (x0 == x1) { if (y0 > y1) { int t = y0; y0 = y1; y1 = t; } gfx_fill(s, x0, y0, 1, y1 - y0 + 1, c); return; }
    int dx = i_abs(x1 - x0), dy = -i_abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx + dy;
    for (int guard = 0; guard < 8192; guard++) {
        plot(s, x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

/* anti-aliased line; "thick" makes it two pixels wide */
void gfx_line_aa(Surface *s, float x0, float y0, float x1, float y1, u32 c, bool thick)
{
    bool steep = m_abs(y1 - y0) > m_abs(x1 - x0);
    if (steep) { float t = x0; x0 = y0; y0 = t; t = x1; x1 = y1; y1 = t; }
    if (x0 > x1) { float t = x0; x0 = x1; x1 = t; t = y0; y0 = y1; y1 = t; }
    float dx = x1 - x0;
    float g = dx < 0.0001f ? 0.0f : (y1 - y0) / dx;
    int lo = steep ? s->cy0 : s->cx0, hi = steep ? s->cy1 : s->cx1;
    int xa = i_max(m_iround(x0), lo - 1), xb = i_min(m_iround(x1), hi);
    float y = y0 + g * ((float)xa - x0) - (thick ? 0.5f : 0.0f);
    for (int x = xa; x <= xb; x++, y += g) {
        int iy = m_ifloor(y);
        int f = (int)((y - (float)iy) * 256.0f);
        if (steep) {
            plot_a(s, iy, x, c, 256 - f);
            if (thick) { plot_a(s, iy + 1, x, c, 256); plot_a(s, iy + 2, x, c, f); }
            else plot_a(s, iy + 1, x, c, f);
        } else {
            plot_a(s, x, iy, c, 256 - f);
            if (thick) { plot_a(s, x, iy + 1, c, 256); plot_a(s, x, iy + 2, c, f); }
            else plot_a(s, x, iy + 1, c, f);
        }
    }
}

void gfx_disc(Surface *s, float cx, float cy, float r, u32 c)
{
    int x0 = i_max(m_ifloor(cx - r - 1), s->cx0), x1 = i_min(m_iceil(cx + r + 1), s->cx1);
    int y0 = i_max(m_ifloor(cy - r - 1), s->cy0), y1 = i_min(m_iceil(cy + r + 1), s->cy1);
    float rin = (r - 0.5f) * (r - 0.5f), rout = (r + 0.5f) * (r + 0.5f);
    if (r < 0.5f) rin = 0;
    for (int y = y0; y < y1; y++) {
        u32 *p = s->px + y * s->stride;
        float dy = (float)y + 0.5f - cy;
        for (int x = x0; x < x1; x++) {
            float dx = (float)x + 0.5f - cx;
            float d2 = dx * dx + dy * dy;
            if (d2 >= rout) continue;
            if (d2 <= rin) p[x] = c;
            else p[x] = blend(p[x], c, (int)((r + 0.5f - m_sqrt(d2)) * 256.0f));
        }
    }
}

void gfx_tri(Surface *s, int x0, int y0, int x1, int y1, int x2, int y2, u32 c)
{
    int minx = i_max(i_min(x0, i_min(x1, x2)), s->cx0), maxx = i_min(i_max(x0, i_max(x1, x2)), s->cx1 - 1);
    int miny = i_max(i_min(y0, i_min(y1, y2)), s->cy0), maxy = i_min(i_max(y0, i_max(y1, y2)), s->cy1 - 1);
    int area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
    if (area == 0) return;
    if (area < 0) { int t = x1; x1 = x2; x2 = t; t = y1; y1 = y2; y2 = t; }
    for (int y = miny; y <= maxy; y++)
        for (int x = minx; x <= maxx; x++) {
            int e0 = (x1 - x0) * (y - y0) - (y1 - y0) * (x - x0);
            int e1 = (x2 - x1) * (y - y1) - (y2 - y1) * (x - x1);
            int e2 = (x0 - x2) * (y - y2) - (y0 - y2) * (x - x2);
            if (e0 >= 0 && e1 >= 0 && e2 >= 0) s->px[y * s->stride + x] = c;
        }
}

void gfx_blit(Surface *d, int dx, int dy, const Surface *src, int sx, int sy, int w, int h)
{
    if (sx < 0) { w += sx; dx -= sx; sx = 0; }
    if (sy < 0) { h += sy; dy -= sy; sy = 0; }
    if (sx + w > src->w) w = src->w - sx;
    if (sy + h > src->h) h = src->h - sy;
    if (dx < d->cx0) { int k = d->cx0 - dx; w -= k; sx += k; dx = d->cx0; }
    if (dy < d->cy0) { int k = d->cy0 - dy; h -= k; sy += k; dy = d->cy0; }
    if (dx + w > d->cx1) w = d->cx1 - dx;
    if (dy + h > d->cy1) h = d->cy1 - dy;
    if (w <= 0 || h <= 0) return;
    for (int j = 0; j < h; j++)
        memcpy(d->px + (dy + j) * d->stride + dx, src->px + (sy + j) * src->stride + sx, (usize)w * 4);
}

/* ---- text ----------------------------------------------------------------- */
extern u8 font_rows[96][9];

static void glyph(Surface *s, int x, int y, unsigned ch, u32 c)
{
    if (ch < 32 || ch > 126) ch = '?';
    if (x + 5 <= s->cx0 || x >= s->cx1 || y + 10 <= s->cy0 || y + 1 >= s->cy1) return;
    const u8 *g = font_rows[ch - 32];
    bool inside = x >= s->cx0 && x + 5 <= s->cx1;
    for (int row = 0; row < 9; row++) {
        u8 bits = g[row];
        int yy = y + 1 + row;
        if (!bits || yy < s->cy0 || yy >= s->cy1) continue;
        u32 *p = s->px + yy * s->stride + x;
        if (inside) {
            if (bits & 0x10) p[0] = c;
            if (bits & 0x08) p[1] = c;
            if (bits & 0x04) p[2] = c;
            if (bits & 0x02) p[3] = c;
            if (bits & 0x01) p[4] = c;
        } else {
            for (int col = 0; col < 5; col++)
                if ((bits & (0x10 >> col)) && x + col >= s->cx0 && x + col < s->cx1) p[col] = c;
        }
    }
}

int gfx_text(Surface *s, int x, int y, const char *str, u32 c)
{
    for (; *str; str++, x += FONT_W) glyph(s, x, y, (u8)*str, c);
    return x;
}

int gfx_text_bold(Surface *s, int x, int y, const char *str, u32 c)
{
    for (; *str; str++, x += FONT_W + 1) {
        glyph(s, x, y, (u8)*str, c);
        glyph(s, x + 1, y, (u8)*str, c);
    }
    return x;
}

int gfx_text_big(Surface *s, int x, int y, const char *str, u32 c, int scale)
{
    for (; *str; str++, x += FONT_W * scale) {
        unsigned ch = (u8)*str;
        if (ch < 32 || ch > 126) ch = '?';
        const u8 *g = font_rows[ch - 32];
        for (int row = 0; row < 9; row++)
            for (int col = 0; col < 5; col++)
                if (g[row] & (0x10 >> col)) gfx_fill(s, x + col * scale, y + row * scale, scale, scale, c);
    }
    return x;
}

int gfx_text_w(const char *str) { return (int)str_len(str) * FONT_W; }

void gfx_text_fit(Surface *s, int x, int y, int maxw, const char *str, u32 c)
{
    int n = (int)str_len(str), fit = maxw / FONT_W;
    if (n <= fit) { gfx_text(s, x, y, str, c); return; }
    if (fit < 3) return;
    char buf[96];
    if (fit > (int)sizeof buf - 1) fit = (int)sizeof buf - 1;
    memcpy(buf, str, (usize)(fit - 2));
    buf[fit - 2] = '.'; buf[fit - 1] = '.'; buf[fit] = 0;
    gfx_text(s, x, y, buf, c);
}

void gfx_text_center(Surface *s, Rect r, const char *str, u32 c)
{
    gfx_text(s, r.x + (r.w - gfx_text_w(str)) / 2, r.y + (r.h - FONT_H) / 2 + 1, str, c);
}
