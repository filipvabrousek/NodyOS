/* NodyOS - 2D drawing on 32-bit XRGB surfaces. */
#ifndef NODY_GFX_H
#define NODY_GFX_H

#include "lib/types.h"

typedef struct Surface {
    u32 *px;
    int w, h, stride;           /* stride in pixels */
    int cx0, cy0, cx1, cy1;     /* clip rectangle, half-open */
} Surface;

typedef struct Rect { int x, y, w, h; } Rect;

static inline Rect rect(int x, int y, int w, int h) { Rect r = { x, y, w, h }; return r; }
static inline bool rect_has(Rect r, int x, int y) { return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h; }

#define RGB(r, g, b) ((u32)(((r) << 16) | ((g) << 8) | (b)))

#define FONT_W 6
#define FONT_H 12

u32  col_mix(u32 a, u32 b, int t);              /* t = 0..256 */
u32  col_scale(u32 c, int k);                   /* k = 256 keeps the colour */
u32  col_hsv(float h, float s, float v);        /* all 0..1 */

void surf_init(Surface *s, u32 *px, int w, int h, int stride);
void gfx_clip(Surface *s, int x, int y, int w, int h);
void gfx_clip_rect(Surface *s, Rect r);
void gfx_unclip(Surface *s);

void gfx_fill(Surface *s, int x, int y, int w, int h, u32 c);
void gfx_fill_a(Surface *s, int x, int y, int w, int h, u32 c, int alpha);     /* alpha 0..256 */
void gfx_frame(Surface *s, int x, int y, int w, int h, u32 c);
void gfx_vgrad(Surface *s, int x, int y, int w, int h, u32 top, u32 bottom);
void gfx_rrect(Surface *s, int x, int y, int w, int h, int r, u32 c);
void gfx_rrect_grad(Surface *s, int x, int y, int w, int h, int r, u32 top, u32 bottom);
void gfx_rframe(Surface *s, int x, int y, int w, int h, int r, u32 border, u32 fill);
void gfx_line(Surface *s, int x0, int y0, int x1, int y1, u32 c);
void gfx_line_aa(Surface *s, float x0, float y0, float x1, float y1, u32 c, bool thick);
void gfx_disc(Surface *s, float cx, float cy, float r, u32 c);
void gfx_tri(Surface *s, int x0, int y0, int x1, int y1, int x2, int y2, u32 c);
void gfx_blit(Surface *d, int dx, int dy, const Surface *src, int sx, int sy, int w, int h);

void font_init(void);
int  gfx_text(Surface *s, int x, int y, const char *str, u32 c);       /* returns end x */
int  gfx_text_bold(Surface *s, int x, int y, const char *str, u32 c);
int  gfx_text_big(Surface *s, int x, int y, const char *str, u32 c, int scale);
int  gfx_text_w(const char *str);
void gfx_text_fit(Surface *s, int x, int y, int maxw, const char *str, u32 c);
void gfx_text_center(Surface *s, Rect r, const char *str, u32 c);

#endif
