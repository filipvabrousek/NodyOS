/* NodyOS - window manager and desktop shell. */
#ifndef NODY_WM_H
#define NODY_WM_H

#include "gui/gfx.h"
#include "lib/input.h"

#define WM_TITLE_H   24
#define WM_TASKBAR_H 32
#define WM_MAX_WIN   8
#define WM_MAX_APPS  6

typedef struct Window Window;

/* What a window sees each frame. Coordinates are relative to its content. */
typedef struct WinInput {
    int   mx, my;
    bool  inside;               /* pointer is over the content, or captured by it */
    bool  l_down, l_press, l_release, l_double;
    bool  r_down, r_press, r_release;
    bool  m_down, m_press, m_release;
    int   wheel;
    bool  focused;
    bool  activity;             /* something happened that may need a redraw */
    bool  resized;
    u8    mods;
    int   nkeys;
    KeyEv keys[16];
} WinInput;

typedef void (*WinUpdate)(Window *w, const WinInput *in);
typedef void (*WinClose)(Window *w);

struct Window {
    char      title[48];
    int       x, y, w, h;       /* content rectangle on screen */
    int       min_w, min_h;
    Surface   surf;             /* content pixels, drawn by the owner */
    WinUpdate update;           /* called once per frame */
    WinClose  on_close;
    void     *user;
    bool      resizable, maximized, closing;
    int       rx, ry, rw, rh;   /* geometry to restore after maximise */
    WinInput  in;
};

void     wm_init(void *framebuffer, int w, int h, int pitch_bytes, int bpp);
void     wm_frame(void);        /* input -> window updates -> composite -> present */

Window  *wm_create(const char *title, int x, int y, int w, int h, WinUpdate update, void *user);
void     wm_close(Window *w);
void     wm_focus(Window *w);
bool     wm_is_open(Window *w);
void     wm_set_title(Window *w, const char *title);
void     wm_invalidate(Window *w);
void     wm_invalidate_rect(Window *w, int x, int y, int ww, int hh);

/* dock / desktop launchers */
void     wm_register_app(const char *name, int icon, void (*launch)(void), Window *(*window)(void));
void     wm_draw_icon(Surface *s, int icon, int x, int y, int size);

int      wm_screen_w(void);
int      wm_screen_h(void);
u32      wm_composite_ms(void);         /* cost of the last composite + present */
u32      wm_frame_count(void);
Surface *wm_backbuffer(void);

#endif
