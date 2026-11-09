/* NodyOS - a small immediate-mode widget kit shared by the applications. */
#ifndef NODY_UI_H
#define NODY_UI_H

#include "gui/wm.h"

#define UI_BG        0x1E2026
#define UI_PANEL     0x25282F
#define UI_PANEL2    0x2C3038
#define UI_DARK      0x17181D
#define UI_BORDER    0x3A3F4A
#define UI_TEXT      0xD9DCE3
#define UI_DIM       0x8B90A0
#define UI_ACCENT    0x4C8DFF
#define UI_ORANGE    0xFFA94D
#define UI_BTN       0x353A45
#define UI_BTN_HOT   0x444B5A
#define UI_BTN_DOWN  0x282C34

enum { UI_NORMAL, UI_PRIMARY, UI_ON, UI_FLAT, UI_DANGER };

typedef struct UI {
    Surface *s;
    const WinInput *in;
    int  mx, my;
    bool down, press, release, dbl;
    bool rpress;
    bool enabled;       /* false: widgets draw but ignore the pointer */
    u32  active;        /* widget that owns the pointer while the button is held */
    u32  kb_focus;      /* text field that receives the keyboard */
} UI;

void ui_begin(UI *u, Surface *s, const WinInput *in);
void ui_end(UI *u);
bool ui_hot(const UI *u, Rect r);
bool ui_button(UI *u, u32 id, Rect r, const char *label, int style);
bool ui_check(UI *u, u32 id, Rect r, bool *value, const char *label);
bool ui_slider(UI *u, u32 id, Rect r, float *value, float lo, float hi, float step, const char *text, u32 fill);
bool ui_textfield(UI *u, u32 id, Rect r, char *buf, int cap);   /* true when Enter is pressed */

#endif
