/* NodyOS - input event queues, filled by the PS/2 drivers at interrupt time
 * and drained by the window manager once per frame. */
#ifndef NODY_INPUT_H
#define NODY_INPUT_H

#include "lib/types.h"

#define MB_LEFT   1
#define MB_RIGHT  2
#define MB_MIDDLE 4

#define MOD_SHIFT 1
#define MOD_CTRL  2
#define MOD_ALT   4

/* key codes: printable keys use their unshifted ASCII value */
enum {
    K_ESC = 27, K_BACKSPACE = 8, K_TAB = 9, K_ENTER = 13,
    K_UP = 0x100, K_DOWN, K_LEFT, K_RIGHT, K_DELETE, K_HOME, K_END, K_PGUP, K_PGDN,
    K_F1, K_F2, K_F3, K_F4, K_F5, K_F6, K_F7, K_F8, K_F9, K_F10, K_F11, K_F12
};

typedef struct MouseEv {
    u8  absolute;       /* 0: dx/dy are relative motion, 1: x/y are 0..65535 */
    u8  buttons;
    i16 dx, dy, wheel;  /* wheel > 0 scrolls away from the user */
    u16 x, y;
} MouseEv;

typedef struct KeyEv {
    u16 code;
    u8  ch;             /* character produced, 0 if none */
    u8  mods;
} KeyEv;

void input_set_mods(u8 mods);           /* live modifier state (shift held while dragging) */
u8   input_mods(void);

void input_push_mouse(const MouseEv *e);
bool input_pop_mouse(MouseEv *e);
void input_push_key(const KeyEv *e);
bool input_pop_key(KeyEv *e);

#endif
