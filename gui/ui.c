/* NodyOS - immediate-mode widgets. */
#include "gui/ui.h"
#include "lib/lib.h"
#include "lib/kmath.h"
#include "lib/plat.h"

void ui_begin(UI *u, Surface *s, const WinInput *in)
{
    u->s = s;
    u->in = in;
    u->mx = in->mx; u->my = in->my;
    u->down = in->l_down; u->press = in->l_press; u->release = in->l_release; u->dbl = in->l_double;
    u->rpress = in->r_press;
    u->enabled = true;
}

void ui_end(UI *u)
{
    if (!u->in->l_down) u->active = 0;
}

bool ui_hot(const UI *u, Rect r)
{
    return u->enabled && u->in->inside && rect_has(r, u->mx, u->my);
}

bool ui_button(UI *u, u32 id, Rect r, const char *label, int style)
{
    bool hot = ui_hot(u, r);
    if (hot && u->press) u->active = id;
    bool held = u->active == id && hot;
    bool clicked = u->active == id && u->release && hot;

    u32 bg = UI_BTN, fg = UI_TEXT, border = 0x4A505E;
    if (style == UI_PRIMARY) { bg = 0x3A6FD8; border = 0x5C93FF; fg = 0xFFFFFF; }
    else if (style == UI_ON) { bg = 0x2F5FBF; border = 0x5C93FF; fg = 0xFFFFFF; }
    else if (style == UI_DANGER) { bg = 0x8E2F2F; border = 0xC65050; fg = 0xFFFFFF; }
    else if (style == UI_FLAT) { bg = UI_PANEL; border = UI_PANEL; }
    if (held) bg = col_scale(bg, 190);
    else if (hot) bg = style == UI_FLAT ? UI_BTN : col_scale(bg, 300);
    if (style == UI_FLAT && !hot) gfx_fill(u->s, r.x, r.y, r.w, r.h, bg);
    else gfx_rframe(u->s, r.x, r.y, r.w, r.h, 4, hot && style == UI_FLAT ? 0x4A505E : border, bg);
    gfx_text_center(u->s, r, label, fg);
    return clicked;
}

bool ui_check(UI *u, u32 id, Rect r, bool *value, const char *label)
{
    bool hot = ui_hot(u, r);
    if (hot && u->press) u->active = id;
    bool clicked = u->active == id && u->release && hot;
    if (clicked) *value = !*value;
    int by = r.y + (r.h - 12) / 2;
    gfx_rframe(u->s, r.x, by, 12, 12, 3, hot ? 0x6A7285 : 0x4A505E, *value ? UI_ACCENT : UI_DARK);
    if (*value) {
        gfx_line_aa(u->s, (float)r.x + 3, (float)by + 6.5f, (float)r.x + 5.5f, (float)by + 9, 0xFFFFFF, false);
        gfx_line_aa(u->s, (float)r.x + 5.5f, (float)by + 9, (float)r.x + 9.5f, (float)by + 3.5f, 0xFFFFFF, false);
    }
    if (label) gfx_text(u->s, r.x + 18, r.y + (r.h - FONT_H) / 2 + 1, label, UI_TEXT);
    return clicked;
}

/* horizontal slider: the bar fills in proportion to the value */
bool ui_slider(UI *u, u32 id, Rect r, float *value, float lo, float hi, float step, const char *text, u32 fill)
{
    bool hot = ui_hot(u, r);
    bool changed = false;
    if (hot && u->press) u->active = id;
    if (u->active == id && (u->down || u->press)) {
        float t = m_clamp((float)(u->mx - r.x - 1) / (float)(r.w - 2), 0.0f, 1.0f);
        float v = lo + t * (hi - lo);
        if (step > 0) v = lo + m_floor((v - lo) / step + 0.5f) * step;
        v = m_clamp(v, lo, hi);
        if (v != *value) { *value = v; changed = true; }
    }
    float t = hi > lo ? m_clamp((*value - lo) / (hi - lo), 0.0f, 1.0f) : 0.0f;
    bool live = u->active == id;
    gfx_rframe(u->s, r.x, r.y, r.w, r.h, 3, live ? UI_ACCENT : (hot ? 0x5A6172 : UI_BORDER), UI_DARK);
    int fw = (int)(t * (float)(r.w - 2) + 0.5f);
    if (fw > 0) gfx_rrect(u->s, r.x + 1, r.y + 1, fw, r.h - 2, 2, live ? col_scale(fill, 300) : fill);
    if (text) gfx_text(u->s, r.x + 6, r.y + (r.h - FONT_H) / 2 + 1, text, 0xF0F2F6);
    return changed;
}

bool ui_textfield(UI *u, u32 id, Rect r, char *buf, int cap)
{
    bool entered = false;
    if (u->enabled && u->press) {
        if (ui_hot(u, r)) u->kb_focus = id;
        else if (u->kb_focus == id) u->kb_focus = 0;
    }
    bool focus = u->kb_focus == id;
    if (focus && u->enabled) {
        int n = (int)str_len(buf);
        for (int i = 0; i < u->in->nkeys; i++) {
            const KeyEv *k = &u->in->keys[i];
            if (k->code == K_ENTER) entered = true;
            else if (k->code == K_BACKSPACE) { if (n > 0) buf[--n] = 0; }
            else if (k->ch >= 33 && k->ch < 127 && k->ch != '/' && n + 1 < cap) { buf[n++] = (char)k->ch; buf[n] = 0; }
        }
    }
    gfx_rframe(u->s, r.x, r.y, r.w, r.h, 3, focus ? UI_ACCENT : UI_BORDER, UI_DARK);
    int ty = r.y + (r.h - FONT_H) / 2 + 1;
    int ex = gfx_text(u->s, r.x + 6, ty, buf, UI_TEXT);
    if (focus && (plat_ticks() / 500) % 2 == 0) gfx_fill(u->s, ex + 1, ty + 1, 1, 10, UI_TEXT);
    return entered;
}
