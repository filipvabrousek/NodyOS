/* NodyOS - window manager and desktop shell.
 *
 * Each window owns a pixel buffer that its application draws into. Once per
 * frame the manager routes input, lets every window update itself, and then
 * recomposites only the damaged part of the screen:
 *
 *     wallpaper -> desktop icons -> windows (back to front) -> dock -> cursor
 *
 * into a back buffer, which is copied to the linear framebuffer.
 */
#include "gui/wm.h"
#include "lib/lib.h"
#include "lib/kmath.h"
#include "lib/plat.h"

enum { PART_NONE, PART_CONTENT, PART_TITLE, PART_CLOSE, PART_MAX, PART_RESIZE };
enum { DRAG_NONE, DRAG_MOVE, DRAG_RESIZE };

typedef struct App {
    const char *name;
    int icon;
    void (*launch)(void);
    Window *(*window)(void);
} App;

#define C_FRAME      0x101216
#define C_FRAME_ON   0x4C8DFF
#define C_TITLE_ON_T 0x3A3F4B
#define C_TITLE_ON_B 0x2B2F38
#define C_TITLE_OFF  0x25272D
#define C_TEXT       0xE6E8EE
#define C_TEXT_DIM   0x8A8F9C
#define C_DOCK       0x14161B
#define C_ACCENT     0x4C8DFF

static u8     *fb;
static int     fb_pitch, fb_bpp;
static int     SW, SH;
static Surface back, wall;
static Window *z[WM_MAX_WIN];           /* back to front; the last one has focus */
static int     nz;
static App     apps[WM_MAX_APPS];
static int     napps;

static int     cur_x, cur_y, cur_btn;
static Window *capture;                 /* window that owns the pointer while a button is held */
static int     drag_mode, drag_ox, drag_oy;
static Window *drag_win;
static Window *armed_win;
static int     armed_part;
static Window *hot_win;                 /* window / part under the pointer */
static int     hot_part;
static int     hot_dock = -1, sel_icon = -1;
static u32     click_ms;
static int     click_x, click_y;
static bool    click_valid;

static int     dx0, dy0, dx1, dy1;      /* damage rectangle */
static bool    dmg;
static char    clock_txt[40];
static u32     clock_sec = 0xFFFFFFFFu;
static u32     comp_ms, frames;

/* ---- damage tracking ------------------------------------------------------ */
static void damage(int x, int y, int w, int h)
{
    int x1 = x + w, y1 = y + h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x1 > SW) x1 = SW;
    if (y1 > SH) y1 = SH;
    if (x >= x1 || y >= y1) return;
    if (!dmg) { dx0 = x; dy0 = y; dx1 = x1; dy1 = y1; dmg = true; return; }
    dx0 = i_min(dx0, x); dy0 = i_min(dy0, y);
    dx1 = i_max(dx1, x1); dy1 = i_max(dy1, y1);
}

static void damage_frame(const Window *w) { damage(w->x - 2, w->y - WM_TITLE_H - 2, w->w + 12, w->h + WM_TITLE_H + 12); }
static void damage_title(const Window *w) { damage(w->x - 1, w->y - WM_TITLE_H - 1, w->w + 2, WM_TITLE_H + 1); }
static void damage_cursor(void) { damage(cur_x, cur_y, 13, 19); }
static void damage_dock(void) { damage(0, SH - WM_TASKBAR_H, SW, WM_TASKBAR_H); }

/* ---- geometry ------------------------------------------------------------- */
static Rect close_rect(const Window *w) { return rect(w->x + w->w - 24, w->y - WM_TITLE_H + 3, 18, 18); }
static Rect max_rect(const Window *w)   { return rect(w->x + w->w - 46, w->y - WM_TITLE_H + 3, 18, 18); }
static Rect icon_rect(int i)            { return rect(16, 22 + i * 86, 84, 78); }
static Rect dock_rect(int i)            { return rect(100 + i * 108, SH - WM_TASKBAR_H + 4, 102, WM_TASKBAR_H - 8); }

static Window *window_at(int x, int y, int *part)
{
    for (int i = nz - 1; i >= 0; i--) {
        Window *w = z[i];
        if (x < w->x - 1 || x >= w->x + w->w + 1 || y < w->y - WM_TITLE_H - 1 || y >= w->y + w->h + 1) continue;
        if (y < w->y) {
            if (rect_has(close_rect(w), x, y)) *part = PART_CLOSE;
            else if (w->resizable && rect_has(max_rect(w), x, y)) *part = PART_MAX;
            else *part = PART_TITLE;
        } else if (w->resizable && !w->maximized && x >= w->x + w->w - 13 && y >= w->y + w->h - 13) {
            *part = PART_RESIZE;
        } else {
            *part = PART_CONTENT;
        }
        return w;
    }
    *part = PART_NONE;
    return NULL;
}

/* ---- window list ---------------------------------------------------------- */
bool wm_is_open(Window *w)
{
    for (int i = 0; i < nz; i++) if (z[i] == w) return true;
    return false;
}

void wm_focus(Window *w)
{
    int i = 0;
    while (i < nz && z[i] != w) i++;
    if (i >= nz || i == nz - 1) return;
    damage_frame(z[nz - 1]);
    z[nz - 1]->in.activity = true;
    for (; i < nz - 1; i++) z[i] = z[i + 1];
    z[nz - 1] = w;
    w->in.activity = true;
    damage_frame(w);
    damage_dock();
}

Window *wm_create(const char *title, int x, int y, int w, int h, WinUpdate update, void *user)
{
    if (nz >= WM_MAX_WIN) return NULL;
    Window *win = kmalloc(sizeof *win);
    u32 *px = kmalloc((usize)SW * (usize)SH * 4);
    if (!win || !px) { kfree(win); kfree(px); return NULL; }
    memset(win, 0, sizeof *win);
    str_copy(win->title, title, sizeof win->title);
    w = i_min(w, SW - 2);
    h = i_min(h, SH - WM_TASKBAR_H - WM_TITLE_H - 2);
    win->x = i_clamp(x, 1, SW - w - 1);
    win->y = i_clamp(y, WM_TITLE_H + 1, SH - WM_TASKBAR_H - h - 1);
    win->w = w; win->h = h;
    win->min_w = 200; win->min_h = 120;
    win->update = update;
    win->user = user;
    win->resizable = true;
    surf_init(&win->surf, px, w, h, SW);
    mem_set32(px, 0x1E2026, (usize)SW * (usize)SH);
    win->in.activity = true;
    win->in.resized = true;
    if (nz) { damage_frame(z[nz - 1]); z[nz - 1]->in.activity = true; }
    z[nz++] = win;
    damage_frame(win);
    damage_dock();
    kprintf("wm: open \"%s\" %dx%d at %d,%d\n", win->title, w, h, win->x, win->y);
    return win;
}

void wm_close(Window *w) { if (w) w->closing = true; }

void wm_set_title(Window *w, const char *title)
{
    if (str_cmp(w->title, title) == 0) return;
    str_copy(w->title, title, sizeof w->title);
    damage_title(w);
}

void wm_invalidate(Window *w) { damage(w->x, w->y, w->w, w->h); }
void wm_invalidate_rect(Window *w, int x, int y, int ww, int hh)
{
    if (x < 0) { ww += x; x = 0; }
    if (y < 0) { hh += y; y = 0; }
    if (x + ww > w->w) ww = w->w - x;
    if (y + hh > w->h) hh = w->h - y;
    if (ww > 0 && hh > 0) damage(w->x + x, w->y + y, ww, hh);
}

static void toggle_max(Window *w)
{
    damage_frame(w);
    if (!w->maximized) {
        w->rx = w->x; w->ry = w->y; w->rw = w->w; w->rh = w->h;
        w->x = 0; w->y = WM_TITLE_H;
        w->w = SW; w->h = SH - WM_TASKBAR_H - WM_TITLE_H;
        w->maximized = true;
    } else {
        w->x = w->rx; w->y = w->ry; w->w = w->rw; w->h = w->rh;
        w->maximized = false;
    }
    w->surf.w = w->w; w->surf.h = w->h;
    w->in.resized = true;
    w->in.activity = true;
    damage_frame(w);
    kprintf("wm: \"%s\" %s (%dx%d)\n", w->title, w->maximized ? "maximised" : "restored", w->w, w->h);
}

static void reap_closed(void)
{
    for (int i = 0; i < nz;) {
        Window *w = z[i];
        if (!w->closing) { i++; continue; }
        kprintf("wm: close \"%s\"\n", w->title);
        damage_frame(w);
        if (w->on_close) w->on_close(w);
        if (capture == w) capture = NULL;
        if (drag_win == w) { drag_win = NULL; drag_mode = DRAG_NONE; }
        if (armed_win == w) armed_win = NULL;
        if (hot_win == w) hot_win = NULL;
        for (int k = i; k < nz - 1; k++) z[k] = z[k + 1];
        nz--;
        kfree(w->surf.px);
        kfree(w);
        if (nz) { damage_frame(z[nz - 1]); z[nz - 1]->in.activity = true; }
        damage_dock();
    }
}

/* ---- apps ----------------------------------------------------------------- */
void wm_register_app(const char *name, int icon, void (*launch)(void), Window *(*window)(void))
{
    if (napps >= WM_MAX_APPS) return;
    apps[napps].name = name;
    apps[napps].icon = icon;
    apps[napps].launch = launch;
    apps[napps].window = window;
    napps++;
}

static void app_activate(int i)
{
    Window *w = apps[i].window();
    if (w && wm_is_open(w)) wm_focus(w);
    else apps[i].launch();
}

#define SC(v) ((v) * size / 48)

void wm_draw_icon(Surface *s, int icon, int x, int y, int size)
{
    static const u32 top[] = { 0x8A6BFF, 0x4FA8FF, 0x3CCBB0 }, bot[] = { 0x5A3BD6, 0x2470DC, 0x1B8B78 };
    icon = i_clamp(icon, 0, 2);
    gfx_rrect_grad(s, x, y, size, size, size / 5 + 1, top[icon], bot[icon]);
    u32 ink = 0xFFFFFF;
    if (icon == 0) {                    /* node graph */
        gfx_line_aa(s, (float)(x + SC(21)), (float)(y + SC(16)), (float)(x + SC(29)), (float)(y + SC(24)), 0xD9D0FF, size > 24);
        gfx_line_aa(s, (float)(x + SC(21)), (float)(y + SC(33)), (float)(x + SC(29)), (float)(y + SC(25)), 0xD9D0FF, size > 24);
        gfx_rrect(s, x + SC(8), y + SC(11), SC(14), SC(10), SC(3), ink);
        gfx_rrect(s, x + SC(8), y + SC(28), SC(14), SC(10), SC(3), ink);
        gfx_rrect(s, x + SC(27), y + SC(19), SC(14), SC(11), SC(3), ink);
    } else if (icon == 1) {             /* folder */
        gfx_rrect(s, x + SC(8), y + SC(12), SC(15), SC(8), SC(2), 0xD6E9FF);
        gfx_rrect(s, x + SC(8), y + SC(17), SC(32), SC(20), SC(3), ink);
    } else {                            /* activity bars */
        gfx_rrect(s, x + SC(10), y + SC(26), SC(7), SC(12), SC(1), ink);
        gfx_rrect(s, x + SC(20), y + SC(18), SC(7), SC(20), SC(1), ink);
        gfx_rrect(s, x + SC(30), y + SC(10), SC(7), SC(28), SC(1), ink);
    }
}

/* ---- wallpaper ------------------------------------------------------------ */
static void make_wallpaper(void)
{
    const int r0 = 0x12, g0 = 0x16, b0 = 0x2A, r1 = 0x1A, g1 = 0x3D, b1 = 0x4B;
    for (int y = 0; y < SH; y++) {
        u32 *row = wall.px + y * SW;
        for (int x = 0; x < SW; x++) {
            u32 t = (u32)((x + 2 * y) * 65536 / (SW + 2 * SH));           /* 0..65535 diagonal */
            u32 n = m_hash((u32)(x * 7919 + y * 104729)) & 0xFFFF;         /* dither */
            int r = (int)(((u32)r0 * (65536 - t) + (u32)r1 * t + n) >> 16);
            int g = (int)(((u32)g0 * (65536 - t) + (u32)g1 * t + n) >> 16);
            int b = (int)(((u32)b0 * (65536 - t) + (u32)b1 * t + n) >> 16);
            row[x] = RGB(r, g, b);
        }
    }
    /* a double helix running across the lower half of the screen */
    float cy = (float)SH * 0.60f, amp = (float)SH * 0.11f, wl = (float)SW * 0.27f;
    for (int pass = 0; pass < 2; pass++) {                  /* far strand parts first */
        for (float x = -20; x < (float)SW + 20; x += 9.0f) {
            float ph = x / wl * M_TAU;
            for (int strand = 0; strand < 2; strand++) {
                float a = ph + (strand ? M_PI : 0.0f);
                float depth = m_cos(a);                     /* -1 far .. 1 near */
                if ((depth > 0) != (pass == 1)) continue;
                float y = cy + m_sin(a) * amp + (x - (float)SW * 0.5f) * 0.12f;
                int ix = i_clamp((int)x, 0, SW - 1), iy = i_clamp((int)y, 0, SH - 1);
                u32 base = wall.px[iy * SW + ix];
                u32 col = strand ? 0x9F7AEA : 0x4FD1C5;
                gfx_disc(&wall, x, y, 2.6f + depth * 1.5f, col_mix(base, col, 70 + (int)(depth * 45)));
            }
            if (((int)(x / 9.0f) & 3) == 0) {               /* rungs */
                float ya = cy + m_sin(ph) * amp + (x - (float)SW * 0.5f) * 0.12f;
                float yb = cy + m_sin(ph + M_PI) * amp + (x - (float)SW * 0.5f) * 0.12f;
                int iy = i_clamp((int)((ya + yb) * 0.5f), 0, SH - 1), ix = i_clamp((int)x, 0, SW - 1);
                if (pass == 0) gfx_line_aa(&wall, x, ya, x, yb, col_mix(wall.px[iy * SW + ix], 0x8FB4D8, 34), false);
            }
        }
    }
}

/* ---- drawing -------------------------------------------------------------- */
static const char *const cursor_art[19] = {
    "#...........",
    "##..........",
    "#o#.........",
    "#oo#........",
    "#ooo#.......",
    "#oooo#......",
    "#ooooo#.....",
    "#oooooo#....",
    "#ooooooo#...",
    "#oooooooo#..",
    "#ooooooooo#.",
    "#oooooo#####",
    "#ooo#oo#....",
    "#oo#.#oo#...",
    "#o#..#oo#...",
    "##....#oo#..",
    "#.....#oo#..",
    ".......##...",
    "............",
};

static void draw_cursor(void)
{
    for (int j = 0; j < 19; j++)
        for (int i = 0; i < 12; i++) {
            char c = cursor_art[j][i];
            int x = cur_x + i, y = cur_y + j;
            if (c == '.' || x < back.cx0 || x >= back.cx1 || y < back.cy0 || y >= back.cy1) continue;
            back.px[y * SW + x] = c == '#' ? 0x000000 : 0xFFFFFF;
        }
}

static void draw_window(Window *w, bool focused)
{
    int fx = w->x - 1, fy = w->y - WM_TITLE_H - 1, fw = w->w + 2, fh = w->h + WM_TITLE_H + 2;
    if (fx + fw + 8 <= back.cx0 || fx >= back.cx1 || fy + fh + 8 <= back.cy0 || fy >= back.cy1) return;

    /* soft drop shadow on the right and bottom edges */
    for (int i = 0; i < 6; i++) {
        int a = (focused ? 84 : 56) - i * 12;
        gfx_fill_a(&back, fx + fw + i, fy + 6 + i, 1, fh - 6, 0x000000, a);
        gfx_fill_a(&back, fx + 6 + i, fy + fh + i, fw - 6 + 1, 1, 0x000000, a);
    }

    gfx_frame(&back, fx, fy, fw, fh, focused ? C_FRAME_ON : C_FRAME);
    if (focused) gfx_vgrad(&back, w->x, w->y - WM_TITLE_H, w->w, WM_TITLE_H, C_TITLE_ON_T, C_TITLE_ON_B);
    else gfx_fill(&back, w->x, w->y - WM_TITLE_H, w->w, WM_TITLE_H, C_TITLE_OFF);
    gfx_fill(&back, w->x, w->y - 1, w->w, 1, 0x15171B);

    int ox0 = back.cx0, oy0 = back.cy0, ox1 = back.cx1, oy1 = back.cy1;
    int tx1 = i_min(ox1, w->x + w->w - (w->resizable ? 50 : 28));
    if (tx1 > ox0) {
        back.cx1 = tx1;
        gfx_text_bold(&back, w->x + 10, w->y - WM_TITLE_H + 6, w->title, focused ? C_TEXT : C_TEXT_DIM);
        back.cx1 = ox1;
    }

    /* title bar buttons */
    Rect c = close_rect(w);
    bool hot_c = hot_win == w && hot_part == PART_CLOSE;
    if (hot_c) gfx_rrect(&back, c.x, c.y, c.w, c.h, 4, armed_win == w && armed_part == PART_CLOSE ? 0xB02A2A : 0xE04545);
    u32 ink = hot_c ? 0xFFFFFF : (focused ? 0xC9CCD6 : 0x6F7480);
    gfx_line_aa(&back, (float)c.x + 5.5f, (float)c.y + 5.5f, (float)c.x + 12.5f, (float)c.y + 12.5f, ink, false);
    gfx_line_aa(&back, (float)c.x + 12.5f, (float)c.y + 5.5f, (float)c.x + 5.5f, (float)c.y + 12.5f, ink, false);
    if (w->resizable) {
        Rect m = max_rect(w);
        bool hot_m = hot_win == w && hot_part == PART_MAX;
        if (hot_m) gfx_rrect(&back, m.x, m.y, m.w, m.h, 4, 0x555B6A);
        u32 mink = hot_m ? 0xFFFFFF : (focused ? 0xC9CCD6 : 0x6F7480);
        gfx_frame(&back, m.x + 5, m.y + 5, 8, 8, mink);
        if (w->maximized) gfx_fill(&back, m.x + 5, m.y + 5, 8, 2, mink);
    }

    gfx_blit(&back, w->x, w->y, &w->surf, 0, 0, w->w, w->h);

    if (w->resizable && !w->maximized) {                    /* resize grip */
        int gx = w->x + w->w - 1, gy = w->y + w->h - 1;
        for (int i = 0; i < 3; i++)
            gfx_line(&back, gx - 3 - i * 4, gy - 1, gx - 1, gy - 3 - i * 4, 0x7C8394);
    }
    back.cx0 = ox0; back.cy0 = oy0; back.cx1 = ox1; back.cy1 = oy1;
}

static void draw_desktop(void)
{
    for (int i = 0; i < napps; i++) {
        Rect r = icon_rect(i);
        if (r.x + r.w <= back.cx0 || r.x >= back.cx1 || r.y + r.h <= back.cy0 || r.y >= back.cy1) continue;
        if (sel_icon == i) gfx_rrect(&back, r.x, r.y, r.w, r.h, 6, col_mix(0x1A2A40, C_ACCENT, 90));
        wm_draw_icon(&back, apps[i].icon, r.x + (r.w - 48) / 2, r.y + 6, 48);
        int tw = gfx_text_w(apps[i].name);
        gfx_text(&back, r.x + (r.w - tw) / 2 + 1, r.y + 59, apps[i].name, 0x000000);
        gfx_text(&back, r.x + (r.w - tw) / 2, r.y + 58, apps[i].name, 0xF2F4F8);
    }
}

static void draw_dock(void)
{
    int y = SH - WM_TASKBAR_H;
    if (y >= back.cy1) return;
    gfx_fill(&back, 0, y, SW, WM_TASKBAR_H, C_DOCK);
    gfx_fill(&back, 0, y, SW, 1, 0x2C303A);

    gfx_disc(&back, 17.0f, (float)y + 12.5f, 3.6f, 0x4FD1C5);      /* logo mark */
    gfx_disc(&back, 24.0f, (float)y + 19.5f, 3.6f, 0x9F7AEA);
    gfx_text_bold(&back, 34, y + 10, "NodyOS", C_TEXT);

    Window *focus = nz ? z[nz - 1] : NULL;
    for (int i = 0; i < napps; i++) {
        Rect r = dock_rect(i);
        Window *w = apps[i].window();
        bool open = w && wm_is_open(w);
        bool active = open && w == focus;
        if (active) gfx_rrect(&back, r.x, r.y, r.w, r.h, 5, 0x2E3340);
        else if (hot_dock == i) gfx_rrect(&back, r.x, r.y, r.w, r.h, 5, 0x22252D);
        wm_draw_icon(&back, apps[i].icon, r.x + 6, r.y + 3, 18);
        gfx_text(&back, r.x + 30, r.y + 6, apps[i].name, open ? C_TEXT : C_TEXT_DIM);
        if (open) gfx_fill(&back, r.x + 10, r.y + r.h - 2, r.w - 20, 2, active ? C_ACCENT : 0x596074);
    }
    gfx_text(&back, SW - gfx_text_w(clock_txt) - 12, y + 10, clock_txt, C_TEXT);
}

static void composite(void)
{
    gfx_clip(&back, dx0, dy0, dx1 - dx0, dy1 - dy0);
    /* If one window's content covers the whole damaged area, nothing beneath
     * it can show through: start drawing from that window. */
    int first = 0;
    bool covered = false;
    for (int i = nz - 1; i >= 0 && !covered; i--) {
        const Window *w = z[i];
        if (dx0 >= w->x && dy0 >= w->y && dx1 <= w->x + w->w && dy1 <= w->y + w->h) { first = i; covered = true; }
    }
    if (!covered) {
        gfx_blit(&back, dx0, dy0, &wall, dx0, dy0, dx1 - dx0, dy1 - dy0);
        draw_desktop();
    }
    for (int i = first; i < nz; i++) draw_window(z[i], i == nz - 1);
    draw_dock();
    draw_cursor();
    gfx_unclip(&back);
}

static void present(void)
{
    int w = dx1 - dx0;
    if (fb_bpp == 32) {
        for (int y = dy0; y < dy1; y++)
            memcpy(fb + (usize)y * (usize)fb_pitch + (usize)dx0 * 4, back.px + y * SW + dx0, (usize)w * 4);
    } else if (fb_bpp == 24) {
        for (int y = dy0; y < dy1; y++) {
            u8 *d = fb + (usize)y * (usize)fb_pitch + (usize)dx0 * 3;
            const u32 *s = back.px + y * SW + dx0;
            for (int x = 0; x < w; x++, d += 3) { d[0] = (u8)s[x]; d[1] = (u8)(s[x] >> 8); d[2] = (u8)(s[x] >> 16); }
        }
    }
}

/* ---- input ---------------------------------------------------------------- */
static bool double_click(void)
{
    u32 now = plat_ticks();
    bool d = click_valid && now - click_ms < 450 && i_abs(cur_x - click_x) < 5 && i_abs(cur_y - click_y) < 5;
    click_valid = !d;
    click_ms = now; click_x = cur_x; click_y = cur_y;
    return d;
}

static void set_hot(Window *w, int part)
{
    if (w == hot_win && part == hot_part) return;
    bool was = hot_part == PART_CLOSE || hot_part == PART_MAX;
    bool is = part == PART_CLOSE || part == PART_MAX;
    if (was && hot_win && wm_is_open(hot_win)) damage_title(hot_win);
    if (is && w) damage_title(w);
    /* tell both the old and the new window, so hover highlights can clear */
    if (hot_win && hot_win != w && wm_is_open(hot_win)) hot_win->in.activity = true;
    hot_win = w;
    hot_part = part;
}

static void pointer_moved(void)
{
    if (drag_mode == DRAG_MOVE && drag_win) {
        Window *w = drag_win;
        damage_frame(w);
        w->x = i_clamp(cur_x - drag_ox, 60 - w->w, SW - 60);
        w->y = i_clamp(cur_y - drag_oy, WM_TITLE_H, SH - WM_TASKBAR_H);
        damage_frame(w);
        return;
    }
    if (drag_mode == DRAG_RESIZE && drag_win) {
        Window *w = drag_win;
        damage_frame(w);
        w->w = i_clamp(cur_x - w->x + drag_ox, w->min_w, SW - w->x);
        w->h = i_clamp(cur_y - w->y + drag_oy, w->min_h, SH - WM_TASKBAR_H - w->y);
        w->surf.w = w->w; w->surf.h = w->h;
        w->in.resized = true;
        w->in.activity = true;
        damage_frame(w);
        return;
    }
    int part;
    Window *w = window_at(cur_x, cur_y, &part);
    set_hot(w, part);
    if (capture) capture->in.activity = true;
    else if (w && part == PART_CONTENT) w->in.activity = true;

    int hd = -1;
    if (!w && cur_y >= SH - WM_TASKBAR_H)
        for (int i = 0; i < napps; i++) if (rect_has(dock_rect(i), cur_x, cur_y)) hd = i;
    if (hd != hot_dock) { hot_dock = hd; damage_dock(); }
}

static void button_pressed(int btn)
{
    int part;
    Window *w = window_at(cur_x, cur_y, &part);

    if (!w) {
        if (!(btn & MB_LEFT)) return;
        if (cur_y >= SH - WM_TASKBAR_H) {
            for (int i = 0; i < napps; i++)
                if (rect_has(dock_rect(i), cur_x, cur_y)) { app_activate(i); return; }
            return;
        }
        int hit = -1;
        for (int i = 0; i < napps; i++) if (rect_has(icon_rect(i), cur_x, cur_y)) hit = i;
        bool dbl = double_click();
        if (hit != sel_icon) {
            if (sel_icon >= 0) { Rect r = icon_rect(sel_icon); damage(r.x, r.y, r.w, r.h); }
            if (hit >= 0) { Rect r = icon_rect(hit); damage(r.x, r.y, r.w, r.h); }
            sel_icon = hit;
        }
        if (hit >= 0 && dbl) app_activate(hit);
        return;
    }

    wm_focus(w);
    if (btn & MB_LEFT) {
        bool dbl = double_click();
        switch (part) {
        case PART_CLOSE:
        case PART_MAX:
            armed_win = w; armed_part = part;
            damage_title(w);
            break;
        case PART_TITLE:
            if (dbl && w->resizable) { toggle_max(w); break; }
            if (!w->maximized) {
                drag_mode = DRAG_MOVE; drag_win = w;
                drag_ox = cur_x - w->x; drag_oy = cur_y - w->y;
            }
            break;
        case PART_RESIZE:
            drag_mode = DRAG_RESIZE; drag_win = w;
            drag_ox = w->x + w->w - cur_x; drag_oy = w->y + w->h - cur_y;
            break;
        case PART_CONTENT:
            capture = w;
            w->in.l_press = true;
            w->in.l_double = dbl;
            w->in.activity = true;
            break;
        }
    }
    if ((btn & (MB_RIGHT | MB_MIDDLE)) && part == PART_CONTENT) {
        if (!capture) capture = w;
        if (capture == w) {
            if (btn & MB_RIGHT) w->in.r_press = true;
            if (btn & MB_MIDDLE) w->in.m_press = true;
            w->in.activity = true;
        }
    }
}

static void button_released(int btn)
{
    if (btn & MB_LEFT) {
        if (drag_mode != DRAG_NONE) {
            if (drag_win) kprintf("wm: \"%s\" now %dx%d at %d,%d\n", drag_win->title, drag_win->w, drag_win->h, drag_win->x, drag_win->y);
            drag_mode = DRAG_NONE;
            drag_win = NULL;
        }
        if (armed_win) {
            int part;
            Window *w = window_at(cur_x, cur_y, &part);
            Window *a = armed_win;
            damage_title(a);
            armed_win = NULL;
            if (w == a && part == armed_part) {
                if (part == PART_CLOSE) a->closing = true;
                else toggle_max(a);
            }
        }
    }
    if (capture) {
        if (btn & MB_LEFT) capture->in.l_release = true;
        if (btn & MB_RIGHT) capture->in.r_release = true;
        if (btn & MB_MIDDLE) capture->in.m_release = true;
        capture->in.activity = true;
    }
}

static void pump_input(void)
{
    MouseEv m;
    while (input_pop_mouse(&m)) {
        int nx, ny;
        if (m.absolute) { nx = (int)m.x * SW >> 16; ny = (int)m.y * SH >> 16; }
        else { nx = cur_x + m.dx; ny = cur_y + m.dy; }
        nx = i_clamp(nx, 0, SW - 1);
        ny = i_clamp(ny, 0, SH - 1);
        if (nx != cur_x || ny != cur_y) {
            damage_cursor();
            cur_x = nx; cur_y = ny;
            damage_cursor();
            pointer_moved();
        }
        int pressed = m.buttons & ~cur_btn, released = cur_btn & ~m.buttons;
        cur_btn = m.buttons;
        if (pressed) button_pressed(pressed);
        if (released) button_released(released);
        if (cur_btn == 0) capture = NULL;
        if (m.wheel) {
            int part;
            Window *w = window_at(cur_x, cur_y, &part);
            if (w && part == PART_CONTENT) { w->in.wheel += m.wheel; w->in.activity = true; }
        }
    }
    KeyEv k;
    while (input_pop_key(&k)) {
        Window *w = nz ? z[nz - 1] : NULL;
        if (w && w->in.nkeys < 16) { w->in.keys[w->in.nkeys++] = k; w->in.activity = true; }
    }
}

static void update_clock(void)
{
    static const char *const mon[13] = { "", "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    u32 sec = plat_ticks() / 1000;
    if (sec == clock_sec) return;
    clock_sec = sec;
    DateTime t;
    plat_now(&t);
    ksnprintf(clock_txt, sizeof clock_txt, "%d %s %d   %02d:%02d:%02d", t.day, mon[t.month <= 12 ? t.month : 0],
              t.year, t.hour, t.min, t.sec);
    damage(SW - 190, SH - WM_TASKBAR_H, 190, WM_TASKBAR_H);
}

/* ---- frame ---------------------------------------------------------------- */
void wm_frame(void)
{
    pump_input();

    Window *snap[WM_MAX_WIN];
    int n = nz;
    for (int i = 0; i < n; i++) snap[i] = z[i];
    for (int i = 0; i < n; i++) {
        Window *w = snap[i];
        if (!wm_is_open(w) || w->closing) continue;
        int part;
        Window *top = window_at(cur_x, cur_y, &part);
        WinInput *in = &w->in;
        in->mx = cur_x - w->x;
        in->my = cur_y - w->y;
        in->inside = capture == w || (!capture && drag_mode == DRAG_NONE && top == w && part == PART_CONTENT);
        in->l_down = capture == w && (cur_btn & MB_LEFT);
        in->r_down = capture == w && (cur_btn & MB_RIGHT);
        in->m_down = capture == w && (cur_btn & MB_MIDDLE);
        in->focused = nz && z[nz - 1] == w;
        in->mods = input_mods();
        w->surf.w = w->w; w->surf.h = w->h;
        gfx_unclip(&w->surf);
        WinInput frozen = *in;
        in->l_press = in->l_release = in->l_double = false;
        in->r_press = in->r_release = in->m_press = in->m_release = false;
        in->wheel = 0; in->nkeys = 0;
        in->activity = false; in->resized = false;
        if (w->update) w->update(w, &frozen);
    }
    reap_closed();
    update_clock();

    if (dmg) {
        u32 t0 = plat_ticks();
        composite();
        present();
        comp_ms = plat_ticks() - t0;
        dmg = false;
    }
    frames++;
}

void wm_init(void *framebuffer, int w, int h, int pitch_bytes, int bpp)
{
    fb = framebuffer; fb_pitch = pitch_bytes; fb_bpp = bpp;
    SW = w; SH = h;
    font_init();
    surf_init(&back, kmalloc((usize)w * (usize)h * 4), w, h, w);
    surf_init(&wall, kmalloc((usize)w * (usize)h * 4), w, h, w);
    make_wallpaper();
    cur_x = w / 2; cur_y = h / 2;
    damage(0, 0, w, h);
}

int      wm_screen_w(void) { return SW; }
int      wm_screen_h(void) { return SH; }
u32      wm_composite_ms(void) { return comp_ms; }
u32      wm_frame_count(void) { return frames; }
Surface *wm_backbuffer(void) { return &back; }
