/* Flow - node-based 3D VFX application.
 *
 *   +--------------------------------------------------------------+
 *   | toolbar: New  Open  Save  Save As  + Add Node        file    |
 *   +----------------------------------------+---------------------+
 *   |                                        |                     |
 *   |            3D viewport                 |     parameters      |
 *   |                                        |  of selected node   |
 *   +----------------------------------------+                     |
 *   | timeline: transport, frame, toggles    |                     |
 *   +----------------------------------------+---------------------+
 *   |                     node graph editor                        |
 *   +--------------------------------------------------------------+
 *
 * The interface is immediate-mode: each pane is redrawn (and handles its
 * input) only on frames where something may have changed for it.
 */
#include "apps/apps.h"
#include "gui/ui.h"
#include "flow/engine.h"
#include "lib/lib.h"
#include "lib/fs.h"
#include "lib/plat.h"

enum { P_TOOL = 1, P_VIEW = 2, P_TIME = 4, P_INSP = 8, P_GRAPH = 16, P_ALL = 31 };
enum { D_NONE, D_ORBIT, D_PAN, D_ZOOM, D_NODE, D_GPAN, D_WIRE_OUT, D_WIRE_IN };
enum { POP_NONE, POP_ADD, POP_CTX };
enum { DLG_NONE, DLG_OPEN, DLG_SAVE, DLG_DISCARD };
enum { ACT_NEW = 1, ACT_OPEN_DIALOG, ACT_OPEN_FILE };

#define TOOL_H      32
#define TIME_H      30
#define NODE_W      140
#define HEAD_H      18
#define ROW_H       14
#define MAX_GUIDES  400
#define ARENA_BYTES (32u << 20)

typedef struct Flow {
    Window *win;
    Scene   sc;
    UI      ui;
    Geo    *geo;
    int    *zbuf;
    Guide   guides[MAX_GUIDES];
    int     nguides;
    Rect    r_tool, r_view, r_time, r_insp, r_graph;
    int     dirty, next_dirty, last_pane, press_pane;
    bool    eval_dirty, view_dirty, playing;
    u32     last_tick;
    int     drag, drag_ox, drag_oy, last_mx, last_my;
    bool    drag_moved;
    int     wire_src, wire_dst, wire_slot;
    int     popup, popup_x, popup_y, popup_gx, popup_gy, ctx_node;
    bool    popup_at_point;
    int     dlg, dlg_scroll;
    char    dlg_name[FS_NAME_MAX];
    int     pending;                    /* action waiting for "discard changes?" */
    char    pending_name[FS_NAME_MAX];
    char    file[FS_NAME_MAX];
    bool    modified;
    char    status[96];
    bool    status_err;
    u32     status_until;
    int     insp_scroll, insp_height;
    RenderStats stats;
    u32     eval_ms, render_ms, fps_t0;
    int     fps_n, fps;
} Flow;

static Flow *F;

/* ---- small helpers -------------------------------------------------------- */
static void say(Flow *f, bool err, const char *fmt, ...)
{
    kva_list ap;
    __builtin_va_start(ap, fmt);
    kvsnprintf(f->status, sizeof f->status, fmt, ap);
    __builtin_va_end(ap);
    f->status_err = err;
    f->status_until = plat_ticks() + 6000;
    f->next_dirty |= P_TOOL;
    kprintf("flow: %s\n", f->status);
}

/* the scene changed: re-evaluate, redraw everything, mark the file as edited */
static void changed(Flow *f)
{
    f->modified = true;
    f->eval_dirty = true;
    f->next_dirty = P_ALL;
}

static void retitle(Flow *f)
{
    char t[64];
    ksnprintf(t, sizeof t, "Flow - %s%s", f->file[0] ? f->file : "untitled", f->modified ? " *" : "");
    if (f->win) wm_set_title(f->win, t);
}

static void layout(Flow *f, int w, int h)
{
    int gh = i_clamp(h * 36 / 100, 150, 300);
    int rw = i_clamp(w * 30 / 100, 250, 320);
    f->r_tool = rect(0, 0, w, TOOL_H);
    f->r_graph = rect(0, h - gh, w, gh);
    f->r_insp = rect(w - rw, TOOL_H, rw, h - gh - TOOL_H);
    f->r_view = rect(0, TOOL_H, w - rw, h - gh - TOOL_H - TIME_H);
    f->r_time = rect(0, h - gh - TIME_H, w - rw, TIME_H);
}

static int pane_at(const Flow *f, int x, int y)
{
    if (rect_has(f->r_tool, x, y)) return P_TOOL;
    if (rect_has(f->r_view, x, y)) return P_VIEW;
    if (rect_has(f->r_time, x, y)) return P_TIME;
    if (rect_has(f->r_insp, x, y)) return P_INSP;
    if (rect_has(f->r_graph, x, y)) return P_GRAPH;
    return 0;
}

/* ---- files ---------------------------------------------------------------- */
static u8 file_buf[SCENE_FILE_MAX];

static bool do_save(Flow *f, const char *name)
{
    int n = scene_save(&f->sc, file_buf, sizeof file_buf);
    if (n <= 0) { say(f, true, "Could not encode the scene"); return false; }
    int r = fs_write(name, file_buf, (u32)n);
    if (r != 0) {
        say(f, true, r == -4 ? "Disk is full" : r == -3 ? "Directory is full (64 files)"
                   : r == -2 ? "Invalid file name" : "Disk write failed");
        return false;
    }
    str_copy(f->file, name, sizeof f->file);
    f->modified = false;
    say(f, false, "Saved %s (%d bytes)", name, n);
    return true;
}

static bool do_open(Flow *f, const char *name)
{
    int n = fs_read(name, file_buf, sizeof file_buf);
    if (n <= 0) { say(f, true, "Cannot read %s", name); return false; }
    if (!scene_load(&f->sc, file_buf, n)) { say(f, true, "%s is not a valid Flow scene", name); return false; }
    str_copy(f->file, name, sizeof f->file);
    f->sc.frame = 0;
    f->playing = true;
    f->insp_scroll = 0;
    changed(f);
    f->modified = false;
    f->view_dirty = true;
    say(f, false, "Opened %s (%d nodes)", name, scene_count(&f->sc));
    return true;
}

static void cmd_new(Flow *f)
{
    scene_clear(&f->sc);
    int c = scene_add(&f->sc, type_find("cube"), 40, 40);
    f->sc.display = c;
    f->sc.selected = c;
    f->file[0] = 0;
    f->playing = false;
    changed(f);
    f->modified = false;
    say(f, false, "New scene");
}

static void open_dialog(Flow *f, int kind)
{
    f->dlg = kind;
    f->dlg_scroll = 0;
    f->popup = POP_NONE;
    f->ui.kb_focus = 301;
    if (kind == DLG_SAVE) str_copy(f->dlg_name, f->file[0] ? f->file : "scene.flw", sizeof f->dlg_name);
    else f->dlg_name[0] = 0;
}

static void close_overlay(Flow *f)
{
    f->dlg = DLG_NONE;
    f->popup = POP_NONE;
    f->ui.kb_focus = 0;
    f->view_dirty = true;
    f->next_dirty = P_ALL;
}

static void cmd_save(Flow *f)
{
    if (f->file[0]) do_save(f, f->file);
    else open_dialog(f, DLG_SAVE);
}

static void perform(Flow *f, int action, const char *name)
{
    if (action == ACT_NEW) cmd_new(f);
    else if (action == ACT_OPEN_DIALOG) open_dialog(f, DLG_OPEN);
    else if (action == ACT_OPEN_FILE) do_open(f, name);
}

/* New / Open replace the scene: ask first if there are unsaved edits */
static void request(Flow *f, int action, const char *name)
{
    if (!f->modified) { perform(f, action, name); return; }
    f->pending = action;
    str_copy(f->pending_name, name ? name : "", sizeof f->pending_name);
    f->dlg = DLG_DISCARD;
    f->popup = POP_NONE;
    f->next_dirty = P_ALL;
}

/* ---- graph editing -------------------------------------------------------- */
static int node_h(const NodeType *t) { return HEAD_H + (t->nin > 0 ? t->nin : 1) * ROW_H + 5; }
static int port_y(int y, int slot) { return y + HEAD_H + slot * ROW_H + ROW_H / 2 + 2; }

static void add_node(Flow *f, int type, bool at_point, int gx, int gy)
{
    Scene *sc = &f->sc;
    int sel = sc->selected, x, y;
    if (at_point) { x = gx; y = gy; }
    else if (sel >= 0) { x = sc->n[sel].x + NODE_W + 40; y = sc->n[sel].y; }
    else { x = -sc->pan_x + f->r_graph.w / 2 - NODE_W / 2; y = -sc->pan_y + f->r_graph.h / 2 - 30; }
    for (int tries = 0; tries < 8; tries++) {           /* do not stack nodes exactly on top of each other */
        bool clash = false;
        for (int i = 0; i < MAX_NODES; i++)
            if (sc->n[i].used && i_abs(sc->n[i].x - x) < 30 && i_abs(sc->n[i].y - y) < 30) clash = true;
        if (!clash) break;
        y += 64;
    }
    int i = scene_add(sc, type, x, y);
    if (i < 0) { say(f, true, "The scene is full (%d nodes)", MAX_NODES); return; }
    if (sel >= 0 && g_types[type].nin > 0) {            /* chain it after the selected node */
        scene_connect(sc, sel, i, 0);
        if (sc->display == sel || sc->display < 0) sc->display = i;
    } else if (sc->display < 0) {
        sc->display = i;
    }
    sc->selected = i;
    /* keep the new node in sight */
    int sx = x + sc->pan_x, sy = y + sc->pan_y;
    if (sx < 10) sc->pan_x += 10 - sx;
    if (sx + NODE_W > f->r_graph.w - 10) sc->pan_x -= sx + NODE_W - (f->r_graph.w - 10);
    if (sy < 10) sc->pan_y += 10 - sy;
    if (sy + 70 > f->r_graph.h - 10) sc->pan_y -= sy + 70 - (f->r_graph.h - 10);
    changed(f);
    kprintf("flow: added %s (node %d)\n", g_types[type].name, i);
}

static void delete_node(Flow *f, int i)
{
    if (i < 0 || !f->sc.n[i].used) return;
    kprintf("flow: deleted %s (node %d)\n", g_types[f->sc.n[i].type].name, i);
    scene_del(&f->sc, i);
    changed(f);
}

static void duplicate_node(Flow *f, int i)
{
    Scene *sc = &f->sc;
    if (i < 0 || !sc->n[i].used) return;
    int k = scene_add(sc, sc->n[i].type, sc->n[i].x + 24, sc->n[i].y + 24 + node_h(&g_types[sc->n[i].type]));
    if (k < 0) { say(f, true, "The scene is full (%d nodes)", MAX_NODES); return; }
    memcpy(sc->n[k].p, sc->n[i].p, sizeof sc->n[k].p);
    memcpy(sc->n[k].in, sc->n[i].in, sizeof sc->n[k].in);
    sc->selected = k;
    changed(f);
}

static void connect(Flow *f, int src, int dst, int slot)
{
    if (src == dst) return;
    if (!scene_connect(&f->sc, src, dst, slot)) { say(f, true, "That connection would create a loop"); return; }
    kprintf("flow: connected %s -> %s.%s\n", g_types[f->sc.n[src].type].name, g_types[f->sc.n[dst].type].name,
            g_types[f->sc.n[dst].type].in[slot]);
    changed(f);
}

/* ---- evaluation and viewport ---------------------------------------------- */
static void evaluate(Flow *f)
{
    u32 t0 = plat_ticks();
    f->geo = scene_eval(&f->sc);
    f->nguides = scene_guides(&f->sc, f->guides, MAX_GUIDES);
    f->eval_ms = plat_ticks() - t0;
    f->eval_dirty = false;
    f->view_dirty = true;
}

static void render(Flow *f)
{
    Surface *s = &f->win->surf;
    Rect r = f->r_view;
    u32 t0 = plat_ticks();
    render_view(s, r, f->zbuf, &f->sc, f->geo, f->guides, f->nguides, &f->stats);
    f->render_ms = plat_ticks() - t0;

    gfx_clip_rect(s, r);
    char buf[80];
    if (f->sc.display >= 0) {
        const NodeType *t = &g_types[f->sc.n[f->sc.display].type];
        gfx_rrect(s, r.x + 8, r.y + 8, 9, 9, 2, 0x3FB0FF);
        ksnprintf(buf, sizeof buf, "%s", t->name);
        gfx_text(s, r.x + 23, r.y + 7, buf, 0xE6E8EE);
    } else {
        gfx_text(s, r.x + 10, r.y + 7, "Nothing displayed - click the blue flag on a node", 0xB0B5C2);
    }
    const char *hint = "drag: orbit   shift-drag: pan   wheel: zoom";
    gfx_text(s, r.x + r.w - gfx_text_w(hint) - 10, r.y + r.h - 16, hint, 0x6F7686);

    int drops = 0;                      /* statistics, top right */
    const Scene *sc = &f->sc;
    for (int i = 0; i < MAX_NODES; i++)
        if (sc->n[i].used && sc->n[i].stamp == sc->stamp && g_types[sc->n[i].type].eval == fluid_eval) drops += fluid_count(&sc->n[i]);
    if (drops) ksnprintf(buf, sizeof buf, "%d tris  %d drops  %u+%u ms  %d fps", f->stats.tris, drops, f->eval_ms, f->render_ms, f->fps);
    else ksnprintf(buf, sizeof buf, "%d tris  %u+%u ms  %d fps", f->stats.tris, f->eval_ms, f->render_ms, f->fps);
    gfx_text(s, r.x + r.w - gfx_text_w(buf) - 10, r.y + 7, buf, 0x8B90A0);
    gfx_unclip(s);

    f->view_dirty = false;
    f->fps_n++;
    wm_invalidate_rect(f->win, r.x, r.y, r.w, r.h);
}

static void camera_basis(const Scene *sc, V3 *right, V3 *up)
{
    float cp = m_cos(sc->pitch), sp = m_sin(sc->pitch), cy = m_cos(sc->yaw), sy = m_sin(sc->yaw);
    V3 fwd = v3_mul(v3(cp * sy, sp, cp * cy), -1.0f);
    *right = v3_norm(v3_cross(fwd, v3(0, 1, 0)));
    *up = v3_cross(*right, fwd);
}

static void home_view(Flow *f)
{
    V3 lo, hi;
    Scene *sc = &f->sc;
    if (f->geo && geo_bounds(f->geo, &lo, &hi)) {
        V3 e = v3_sub(hi, lo);
        sc->target = v3_mul(v3_add(lo, hi), 0.5f);
        sc->dist = m_clamp(v3_len(e) * 1.15f + 1.0f, 2.0f, 60.0f);
    } else {
        sc->target = v3(0, 0.8f, 0);
        sc->dist = 11.0f;
    }
    sc->yaw = 0.62f; sc->pitch = 0.42f;
    f->view_dirty = true;
}

static void viewport_input(Flow *f)
{
    UI *u = &f->ui;
    const WinInput *in = u->in;
    Scene *sc = &f->sc;
    bool over = u->enabled && in->inside && rect_has(f->r_view, u->mx, u->my);
    if (f->drag == D_NONE && over) {
        if (in->l_press) f->drag = (in->mods & MOD_SHIFT) ? D_PAN : (in->mods & MOD_CTRL) ? D_ZOOM : D_ORBIT;
        else if (in->r_press) f->drag = D_ZOOM;
        else if (in->m_press) f->drag = D_PAN;
        f->last_mx = u->mx; f->last_my = u->my;
        if (in->wheel) {
            for (int i = 0; i < i_abs(in->wheel); i++) sc->dist *= in->wheel > 0 ? 0.9f : 1.111f;
            sc->dist = m_clamp(sc->dist, 1.0f, 80.0f);
            f->view_dirty = true;
        }
    }
    if (f->drag == D_ORBIT || f->drag == D_PAN || f->drag == D_ZOOM) {
        int dx = u->mx - f->last_mx, dy = u->my - f->last_my;
        f->last_mx = u->mx; f->last_my = u->my;
        if (!(in->l_down || in->r_down || in->m_down)) { f->drag = D_NONE; return; }
        if (!dx && !dy) return;
        if (f->drag == D_ORBIT) {
            sc->yaw -= (float)dx * 0.010f;
            sc->pitch = m_clamp(sc->pitch + (float)dy * 0.010f, -1.45f, 1.45f);
        } else if (f->drag == D_PAN) {
            V3 right, up;
            camera_basis(sc, &right, &up);
            float k = sc->dist * 0.0017f;
            sc->target = v3_add(sc->target, v3_add(v3_mul(right, -(float)dx * k), v3_mul(up, (float)dy * k)));
        } else {
            sc->dist = m_clamp(sc->dist * (1.0f + (float)dy * 0.01f), 1.0f, 80.0f);
        }
        f->view_dirty = true;
    }
}

/* ---- toolbar -------------------------------------------------------------- */
static void ui_toolbar(Flow *f)
{
    UI *u = &f->ui;
    Surface *s = u->s;
    Rect r = f->r_tool;
    gfx_clip_rect(s, r);
    gfx_fill(s, r.x, r.y, r.w, r.h, 0x2A2D35);
    gfx_fill(s, r.x, r.y + r.h - 1, r.w, 1, UI_DARK);
    int x = 8;
    if (ui_button(u, 101, rect(x, 5, 44, 22), "New", UI_NORMAL)) request(f, ACT_NEW, NULL);
    x += 48;
    if (ui_button(u, 102, rect(x, 5, 48, 22), "Open", UI_NORMAL)) request(f, ACT_OPEN_DIALOG, NULL);
    x += 52;
    if (ui_button(u, 103, rect(x, 5, 48, 22), "Save", UI_NORMAL)) cmd_save(f);
    x += 52;
    if (ui_button(u, 104, rect(x, 5, 64, 22), "Save As", UI_NORMAL)) open_dialog(f, DLG_SAVE);
    x += 74;
    gfx_fill(s, x - 5, 8, 1, 16, UI_BORDER);
    if (ui_button(u, 105, rect(x, 5, 86, 22), "+ Add Node", UI_PRIMARY)) {
        f->popup = POP_ADD;
        f->popup_x = x; f->popup_y = TOOL_H;
        f->popup_at_point = false;
    }
    x += 98;

    char name[48];
    ksnprintf(name, sizeof name, "%s%s", f->file[0] ? f->file : "untitled", f->modified ? " *" : "");
    int nw = gfx_text_w(name);
    gfx_text(s, r.w - nw - 12, 11, name, f->modified ? UI_ORANGE : UI_DIM);
    if (f->status[0])
        gfx_text_fit(s, x, 11, r.w - nw - 30 - x, f->status, f->status_err ? 0xFF7B7B : 0x9BE3B0);
    gfx_unclip(s);
}

/* ---- timeline ------------------------------------------------------------- */
static void ui_timeline(Flow *f)
{
    UI *u = &f->ui;
    Surface *s = u->s;
    Scene *sc = &f->sc;
    Rect r = f->r_time;
    gfx_clip_rect(s, r);
    gfx_fill(s, r.x, r.y, r.w, r.h, 0x23262D);
    gfx_fill(s, r.x, r.y, r.w, 1, UI_DARK);
    int y = r.y + 4, x = r.x + 8;

    if (ui_button(u, 201, rect(x, y, 26, 22), "", UI_NORMAL)) {             /* rewind */
        sc->frame = 0;
        f->eval_dirty = true;
    }
    gfx_fill(s, x + 8, y + 7, 2, 9, UI_TEXT);
    gfx_tri(s, x + 18, y + 6, x + 18, y + 16, x + 10, y + 11, UI_TEXT);
    x += 30;
    if (ui_button(u, 202, rect(x, y, 34, 22), "", f->playing ? UI_ON : UI_NORMAL)) {
        f->playing = !f->playing;
        f->last_tick = plat_ticks();
        kprintf("flow: %s at frame %d\n", f->playing ? "play" : "pause", sc->frame);
    }
    if (f->playing) { gfx_fill(s, x + 12, y + 6, 3, 10, 0xFFFFFF); gfx_fill(s, x + 19, y + 6, 3, 10, 0xFFFFFF); }
    else gfx_tri(s, x + 13, y + 5, x + 13, y + 17, x + 23, y + 11, UI_TEXT);
    x += 38;
    if (ui_button(u, 203, rect(x, y, 26, 22), "", UI_NORMAL)) {             /* single step */
        f->playing = false;
        sc->frame++;
        if (sc->loop && sc->frame >= sc->loop_len) sc->frame = 0;
        f->eval_dirty = true;
    }
    gfx_tri(s, x + 8, y + 6, x + 8, y + 16, x + 16, y + 11, UI_TEXT);
    gfx_fill(s, x + 17, y + 7, 2, 9, UI_TEXT);
    x += 34;

    char buf[96];
    ksnprintf(buf, sizeof buf, "Frame %4d", sc->frame);
    gfx_text(s, x, y + 6, buf, UI_TEXT);
    x += 66;
    float len = (float)sc->loop_len;                                        /* loop length doubles as progress bar */
    ksnprintf(buf, sizeof buf, "of %d", sc->loop_len);
    Rect bar = rect(x, y + 2, 96, 18);
    if (ui_slider(u, 204, bar, &len, 30, 900, 30, NULL, 0x39404D)) { sc->loop_len = (int)len; f->modified = true; }
    if (sc->loop_len > 0) {
        int pw = i_clamp(sc->frame * (bar.w - 2) / sc->loop_len, 0, bar.w - 2);
        gfx_fill(s, bar.x + 1, bar.y + bar.h - 4, pw, 3, UI_ACCENT);
    }
    gfx_text(s, bar.x + 6, y + 6, buf, 0xF0F2F6);
    x += 104;
    if (ui_check(u, 205, rect(x, y, 52, 22), &sc->loop, "Loop")) f->modified = true;
    x += 58;
    if (ui_check(u, 206, rect(x, y, 52, 22), &sc->grid, "Grid")) f->view_dirty = true;
    x += 58;
    if (ui_check(u, 207, rect(x, y, 52, 22), &sc->wire, "Wire")) f->view_dirty = true;
    x += 58;
    if (ui_button(u, 208, rect(x, y, 78, 22), "Frame All", UI_NORMAL)) home_view(f);
    gfx_unclip(s);
}

/* ---- parameter inspector -------------------------------------------------- */
static const u32 palette[12] = {
    0xFF5D73, 0xFF8A3D, 0xF2B84B, 0x63C788, 0x35C2A8, 0x4CC9F0,
    0x3A9BFF, 0x5B8DEF, 0x8A6BFF, 0xB877E0, 0xE8ECF2, 0x4A5060
};

static void ui_inspector(Flow *f)
{
    UI *u = &f->ui;
    Surface *s = u->s;
    Scene *sc = &f->sc;
    Rect r = f->r_insp;
    gfx_clip_rect(s, r);
    gfx_fill(s, r.x, r.y, r.w, r.h, 0x23262D);
    gfx_fill(s, r.x, r.y, 1, r.h, UI_DARK);

    if (sc->selected < 0 || !sc->n[sc->selected].used) {
        static const char *const help[] = {
            "No node selected.", "",
            "Graph", "  right-click      add a node", "  drag a port      connect nodes",
            "  drag a wire end  disconnect", "  blue flag        show in viewport",
            "  yellow flag      bypass node", "  Del              delete node", "",
            "Viewport", "  drag             orbit", "  shift-drag       pan", "  wheel, right-drag  zoom", "",
            "Space  play / pause", "Ctrl+S save   Ctrl+O open",
        };
        for (int i = 0; i < ARRAY_LEN(help); i++)
            gfx_text(s, r.x + 14, r.y + 14 + i * 15, help[i], i == 0 || help[i][0] != ' ' ? UI_TEXT : UI_DIM);
        gfx_unclip(s);
        return;
    }

    int ni = sc->selected;
    Node *n = &sc->n[ni];
    const NodeType *t = &g_types[n->type];
    u32 cat = g_cat_colors[t->cat];

    /* header */
    gfx_fill(s, r.x + 1, r.y, r.w - 1, 40, 0x2A2D35);
    gfx_fill(s, r.x + 1, r.y, 4, 40, cat);
    gfx_text_bold(s, r.x + 14, r.y + 6, t->name, 0xFFFFFF);
    gfx_text(s, r.x + 14, r.y + 22, g_cat_names[t->cat], col_mix(cat, 0xFFFFFF, 90));
    if (ui_button(u, 901, rect(r.x + r.w - 132, r.y + 9, 60, 22), "Display", sc->display == ni ? UI_ON : UI_NORMAL)) {
        sc->display = sc->display == ni ? -1 : ni;
        changed(f);
    }
    if (ui_button(u, 902, rect(r.x + r.w - 68, r.y + 9, 58, 22), "Bypass", n->bypass ? UI_ON : UI_NORMAL)) {
        n->bypass = !n->bypass;
        changed(f);
    }
    gfx_fill(s, r.x + 1, r.y + 40, r.w - 1, 1, UI_DARK);

    /* parameters, scrollable */
    Rect body = rect(r.x + 1, r.y + 41, r.w - 1, r.h - 41);
    if (ui_hot(u, body) && u->in->wheel) f->insp_scroll -= u->in->wheel * 24;
    f->insp_scroll = i_clamp(f->insp_scroll, 0, i_max(0, f->insp_height - body.h));
    gfx_clip_rect(s, body);
    bool body_live = u->enabled;
    if (!rect_has(body, u->mx, u->my) && !u->active) u->enabled = false;    /* widgets scrolled under the header stay inert */

    int y = body.y + 8 - f->insp_scroll;
    int lx = r.x + 14, cx = r.x + 104, cw = r.w - 104 - 12;
    char buf[48];
    if (t->np == 0) gfx_text(s, lx, y + 4, "This node has no parameters.", UI_DIM);
    for (int i = 0; i < t->np; i++) {
        const ParamDef *d = &t->p[i];
        if (!param_visible(t, n, i)) continue;
        u32 id = 1000 + (u32)i * 8;
        if (u->rpress && ui_hot(u, rect(r.x + 1, y, r.w - 1, 22)) && n->p[i] != d->def) {
            n->p[i] = d->def;                           /* right-click: back to the default */
            changed(f);
        }
        gfx_text(s, lx, y + 5, d->name, n->p[i] != d->def ? UI_TEXT : 0xAEB3C0);
        Rect c = rect(cx, y + 1, cw, 19);
        if (d->kind == PK_FLOAT || d->kind == PK_INT) {
            if (d->kind == PK_INT) ksnprintf(buf, sizeof buf, "%d", m_iround(n->p[i]));
            else ksnprintf(buf, sizeof buf, d->step >= 1.0f ? "%.0f" : d->step >= 0.01f ? "%.2f" : "%.3f", (double)n->p[i]);
            if (ui_slider(u, id, c, &n->p[i], d->lo, d->hi, d->step, buf, col_mix(0x39404D, cat, 110))) changed(f);
            y += 23;
        } else if (d->kind == PK_BOOL) {
            bool v = n->p[i] > 0.5f;
            if (ui_check(u, id, rect(cx, y, 60, 21), &v, v ? "On" : "Off")) { n->p[i] = v ? 1.0f : 0.0f; changed(f); }
            y += 23;
        } else if (d->kind == PK_ENUM) {
            int cnt = param_enum_count(d), cur = m_iround(n->p[i]);
            int bw = cw / (cnt > 0 ? cnt : 1);
            for (int k = 0; k < cnt; k++) {
                param_enum_name(d, k, buf, sizeof buf);
                if ((int)str_len(buf) * FONT_W > bw - 4) buf[i_max((bw - 4) / FONT_W, 1)] = 0;
                Rect b = rect(cx + k * bw, y + 1, k == cnt - 1 ? cw - k * bw : bw - 2, 19);
                if (ui_button(u, id + 1 + (u32)k, b, buf, cur == k ? UI_ON : UI_NORMAL) && cur != k) {
                    n->p[i] = (float)k;
                    f->insp_scroll = 0;
                    changed(f);
                }
            }
            y += 23;
        } else if (d->kind == PK_COLOR) {
            u32 col = (u32)n->p[i] & 0xFFFFFF;
            gfx_rframe(s, c.x, c.y, c.w, c.h, 3, 0x14161A, col);
            y += 23;
            int sw = cw / 12;
            for (int k = 0; k < 12; k++) {
                Rect b = rect(cx + k * sw, y, sw - 2, 14);
                bool hot = ui_hot(u, b);
                gfx_rframe(s, b.x, b.y, b.w, b.h, 2, hot ? 0xFFFFFF : 0x14161A, palette[k]);
                if (hot && u->press) { n->p[i] = (float)palette[k]; col = palette[k]; changed(f); }
            }
            y += 18;
            static const char *const ch_name[3] = { "R", "G", "B" };
            static const u32 ch_col[3] = { 0xA83C3C, 0x3C8A4A, 0x3C5FA8 };
            for (int k = 0; k < 3; k++) {
                int shift = 16 - k * 8;
                float v = (float)((col >> shift) & 255);
                ksnprintf(buf, sizeof buf, "%s %d", ch_name[k], (int)v);
                if (ui_slider(u, id + 1 + (u32)k, rect(cx, y, cw, 16), &v, 0, 255, 1, buf, ch_col[k])) {
                    col = (col & ~(0xFFu << shift)) | ((u32)v << shift);
                    n->p[i] = (float)col;
                    changed(f);
                }
                y += 19;
            }
            y += 4;
        }
    }
    f->insp_height = y + f->insp_scroll - body.y + 10;
    u->enabled = body_live;
    gfx_unclip(s);
}

/* ---- node graph ----------------------------------------------------------- */
typedef struct GHit { int node, in_node, in_slot, out_node, flag; } GHit;

static GHit graph_hit(const Flow *f, int mx, int my)
{
    GHit h = { -1, -1, -1, -1, 0 };
    const Scene *sc = &f->sc;
    int ox = f->r_graph.x + sc->pan_x, oy = f->r_graph.y + sc->pan_y;
    for (int i = MAX_NODES - 1; i >= 0; i--) {
        const Node *n = &sc->n[i];
        if (!n->used) continue;
        const NodeType *t = &g_types[n->type];
        int x = ox + n->x, y = oy + n->y;
        if (h.out_node < 0 && h.in_node < 0 && h.node < 0) {
            int dx = mx - (x + NODE_W), dy = my - (y + HEAD_H / 2);
            if (dx * dx + dy * dy <= 81) h.out_node = i;
            for (int k = 0; k < t->nin; k++) {
                dx = mx - x; dy = my - port_y(y, k);
                if (dx * dx + dy * dy <= 81) { h.in_node = i; h.in_slot = k; }
            }
        }
        if (h.node < 0 && mx >= x && mx < x + NODE_W && my >= y && my < y + node_h(t)) {
            h.node = i;
            if (my < y + HEAD_H) {
                if (mx >= x + NODE_W - 23 && mx < x + NODE_W - 9) h.flag = 2;
                else if (mx >= x + NODE_W - 38 && mx < x + NODE_W - 24) h.flag = 1;
            }
        }
    }
    return h;
}

static void wire(Surface *s, int x0, int y0, int x1, int y1, u32 col)
{
    float dx = m_max(34.0f, m_abs((float)(x1 - x0)) * 0.5f);
    float px = (float)x0, py = (float)y0;
    for (int i = 1; i <= 24; i++) {
        float t = (float)i / 24.0f, mt = 1.0f - t;
        float a = mt * mt * mt, b = 3 * mt * mt * t, c = 3 * mt * t * t, d = t * t * t;
        float x = a * (float)x0 + b * ((float)x0 + dx) + c * ((float)x1 - dx) + d * (float)x1;
        float y = (a + b) * (float)y0 + (c + d) * (float)y1;
        gfx_line_aa(s, px, py, x, y, col, true);
        px = x; py = y;
    }
}

static void port(Surface *s, int x, int y, u32 fill, bool hot)
{
    gfx_disc(s, (float)x + 0.5f, (float)y + 0.5f, hot ? 6.5f : 5.5f, hot ? 0xFFFFFF : 0x14161A);
    gfx_disc(s, (float)x + 0.5f, (float)y + 0.5f, 4.0f, fill);
}

static void ui_graph(Flow *f)
{
    UI *u = &f->ui;
    Surface *s = u->s;
    Scene *sc = &f->sc;
    Rect r = f->r_graph;
    bool over = ui_hot(u, r);
    GHit h = graph_hit(f, u->mx, u->my);
    if (!over) { h.node = h.in_node = h.out_node = -1; h.flag = 0; }

    /* --- input --- */
    if (over && f->drag == D_NONE) {
        if (u->press) {
            if (h.out_node >= 0) {
                f->drag = D_WIRE_OUT; f->wire_src = h.out_node;
            } else if (h.in_node >= 0) {
                int src = sc->n[h.in_node].in[h.in_slot];
                if (src >= 0) {                         /* grab the existing wire by its end */
                    sc->n[h.in_node].in[h.in_slot] = -1;
                    f->drag = D_WIRE_OUT; f->wire_src = src;
                    changed(f);
                } else {
                    f->drag = D_WIRE_IN; f->wire_dst = h.in_node; f->wire_slot = h.in_slot;
                }
            } else if (h.node >= 0) {
                if (h.flag == 2) { sc->display = sc->display == h.node ? -1 : h.node; changed(f); }
                else if (h.flag == 1) { sc->n[h.node].bypass = !sc->n[h.node].bypass; changed(f); }
                else {
                    if (sc->selected != h.node) { sc->selected = h.node; f->insp_scroll = 0; f->eval_dirty = true; }
                    f->drag = D_NODE;
                    f->drag_ox = u->mx - sc->n[h.node].x;
                    f->drag_oy = u->my - sc->n[h.node].y;
                }
            } else {
                f->drag = D_GPAN; f->drag_moved = false;
                f->last_mx = u->mx; f->last_my = u->my;
            }
        } else if (u->rpress) {
            f->popup_x = u->mx; f->popup_y = u->my;
            if (h.node >= 0) {
                sc->selected = h.node;
                f->popup = POP_CTX; f->ctx_node = h.node;
            } else {
                f->popup = POP_ADD; f->popup_at_point = true;
                f->popup_gx = u->mx - r.x - sc->pan_x - NODE_W / 2;
                f->popup_gy = u->my - r.y - sc->pan_y - 10;
            }
        }
    }
    if (f->drag == D_NODE) {
        if (u->down && sc->selected >= 0) {
            int nx = u->mx - f->drag_ox, ny = u->my - f->drag_oy;
            if (nx != sc->n[sc->selected].x || ny != sc->n[sc->selected].y) {
                sc->n[sc->selected].x = nx; sc->n[sc->selected].y = ny;
                f->modified = true;
            }
        } else f->drag = D_NONE;
    } else if (f->drag == D_GPAN) {
        if (u->down) {
            int dx = u->mx - f->last_mx, dy = u->my - f->last_my;
            if (dx || dy) { sc->pan_x += dx; sc->pan_y += dy; f->drag_moved = true; }
            f->last_mx = u->mx; f->last_my = u->my;
        } else {
            if (!f->drag_moved && sc->selected >= 0) { sc->selected = -1; f->eval_dirty = true; }
            f->drag = D_NONE;
        }
    } else if (f->drag == D_WIRE_OUT && !u->down) {
        int dst = h.in_node >= 0 ? h.in_node : h.node, slot = h.in_node >= 0 ? h.in_slot : 0;
        if (h.in_node < 0 && dst >= 0) {                /* dropped on a node body: first free input */
            const NodeType *t = &g_types[sc->n[dst].type];
            if (t->nin == 0) dst = -1;
            for (int k = 0; dst >= 0 && k < t->nin; k++) if (sc->n[dst].in[k] < 0) { slot = k; break; }
        }
        if (dst >= 0) connect(f, f->wire_src, dst, slot);
        f->drag = D_NONE;
        f->next_dirty = P_ALL;
    } else if (f->drag == D_WIRE_IN && !u->down) {
        int src = h.out_node >= 0 ? h.out_node : h.node;
        if (src >= 0) connect(f, src, f->wire_dst, f->wire_slot);
        f->drag = D_NONE;
        f->next_dirty = P_ALL;
    }

    /* --- drawing --- */
    gfx_clip_rect(s, r);
    gfx_fill(s, r.x, r.y, r.w, r.h, 0x191B20);
    gfx_fill(s, r.x, r.y, r.w, 1, UI_DARK);
    int ox = r.x + sc->pan_x, oy = r.y + sc->pan_y;
    for (int gy = ((sc->pan_y % 24) + 24) % 24; gy < r.h; gy += 24)
        for (int gx = ((sc->pan_x % 24) + 24) % 24; gx < r.w; gx += 24)
            gfx_fill(s, r.x + gx, r.y + gy, 1, 1, 0x30343D);

    for (int i = 0; i < MAX_NODES; i++) {               /* wires under the nodes */
        const Node *n = &sc->n[i];
        if (!n->used) continue;
        for (int k = 0; k < g_types[n->type].nin; k++) {
            int src = n->in[k];
            if (src < 0) continue;
            bool lit = i == sc->selected || src == sc->selected;
            wire(s, ox + sc->n[src].x + NODE_W, oy + sc->n[src].y + HEAD_H / 2, ox + n->x, port_y(oy + n->y, k),
                 lit ? UI_ORANGE : 0x8F97A8);
        }
    }

    for (int i = 0; i < MAX_NODES; i++) {
        const Node *n = &sc->n[i];
        if (!n->used) continue;
        const NodeType *t = &g_types[n->type];
        int x = ox + n->x, y = oy + n->y, hh = node_h(t);
        if (x > r.x + r.w || x + NODE_W + 8 < r.x || y > r.y + r.h || y + hh + 8 < r.y) continue;
        bool sel = i == sc->selected, shown = i == sc->display;
        u32 cat = n->bypass ? 0x555A66 : g_cat_colors[t->cat];
        gfx_fill_a(s, x + 3, y + 4, NODE_W, hh, 0x000000, 70);
        gfx_rframe(s, x - 1, y - 1, NODE_W + 2, hh + 2, 6, sel ? UI_ORANGE : (shown ? 0x3FB0FF : 0x0E0F12), 0x2B2F38);
        gfx_rrect(s, x, y, NODE_W, HEAD_H, 5, cat);
        gfx_fill(s, x, y + HEAD_H - 5, NODE_W, 5, cat);
        gfx_text(s, x + 8, y + 3, t->name, n->bypass ? 0xB9BDC8 : 0xFFFFFF);
        /* flags: bypass (yellow) and display (blue) */
        gfx_rrect(s, x + NODE_W - 37, y + 4, 11, 10, 2, n->bypass ? 0xF5C542 : col_scale(cat, 150));
        gfx_rrect(s, x + NODE_W - 22, y + 4, 11, 10, 2, shown ? 0x3FB0FF : col_scale(cat, 150));
        if (shown) gfx_fill(s, x + NODE_W - 19, y + 7, 5, 4, 0xFFFFFF);

        if (t->nin == 0) gfx_text(s, x + 10, y + HEAD_H + 3, g_cat_names[t->cat], 0x6F7686);
        for (int k = 0; k < t->nin; k++) {
            int py = port_y(y, k);
            bool hot = (f->drag == D_WIRE_OUT || f->drag == D_NONE) && h.in_node == i && h.in_slot == k;
            gfx_text(s, x + 11, py - 5, t->in[k], n->in[k] >= 0 ? UI_TEXT : UI_DIM);
            port(s, x, py, n->in[k] >= 0 ? 0xD5DAE5 : 0x596072, hot);
        }
        port(s, x + NODE_W, y + HEAD_H / 2, col_mix(cat, 0xFFFFFF, 130), (f->drag == D_WIRE_IN || f->drag == D_NONE) && h.out_node == i);
    }

    if (f->drag == D_WIRE_OUT && sc->n[f->wire_src].used)
        wire(s, ox + sc->n[f->wire_src].x + NODE_W, oy + sc->n[f->wire_src].y + HEAD_H / 2, u->mx, u->my, 0xFFFFFF);
    if (f->drag == D_WIRE_IN && sc->n[f->wire_dst].used)
        wire(s, u->mx, u->my, ox + sc->n[f->wire_dst].x, port_y(oy + sc->n[f->wire_dst].y, f->wire_slot), 0xFFFFFF);

    gfx_text(s, r.x + 10, r.y + r.h - 16,
             "right-click: add node    drag port to port: connect    blue flag: display    Del: delete    Tab: add",
             0x565C6B);
    if (scene_count(sc) == 0)
        gfx_text(s, r.x + r.w / 2 - 120, r.y + r.h / 2 - 6, "Empty graph - right-click to add a node", UI_DIM);
    gfx_unclip(s);
}

/* ---- popups --------------------------------------------------------------- */
static void ui_popup(Flow *f)
{
    UI *u = &f->ui;
    Surface *s = u->s;
    Scene *sc = &f->sc;
    for (int i = 0; i < u->in->nkeys; i++)
        if (u->in->keys[i].code == K_ESC) { close_overlay(f); return; }
    if (f->popup == POP_ADD) {
        const int colw = 118, rows = 6;
        Rect p = rect(f->popup_x, f->popup_y, CAT_COUNT * colw + 12, 30 + rows * 21 + 8);
        p.x = i_clamp(p.x, 4, s->w - p.w - 4);
        p.y = i_clamp(p.y, 4, s->h - p.h - 4);
        gfx_rframe(s, p.x, p.y, p.w, p.h, 6, 0x565D6E, 0x20232A);
        for (int c = 0; c < CAT_COUNT; c++) {
            int x = p.x + 6 + c * colw, y = p.y + 8;
            gfx_fill(s, x + 4, y + 3, 3, 9, g_cat_colors[c]);
            gfx_text_bold(s, x + 12, y + 1, g_cat_names[c], col_mix(g_cat_colors[c], 0xFFFFFF, 120));
            y += 20;
            for (int i = 0; i < g_ntypes; i++) {
                if (g_types[i].cat != c) continue;
                if (ui_button(u, 400 + (u32)i, rect(x, y, colw - 4, 20), g_types[i].name, UI_FLAT)) {
                    add_node(f, i, f->popup_at_point, f->popup_gx, f->popup_gy);
                    close_overlay(f);
                    return;
                }
                y += 21;
            }
        }
        if ((u->press || u->rpress) && !rect_has(p, u->mx, u->my)) close_overlay(f);
    } else if (f->popup == POP_CTX) {
        static const char *const items[5] = { "Display in viewport", "Bypass", "Disconnect inputs", "Duplicate", "Delete" };
        Rect p = rect(f->popup_x, f->popup_y, 150, 5 * 21 + 10);
        p.x = i_clamp(p.x, 4, s->w - p.w - 4);
        p.y = i_clamp(p.y, 4, s->h - p.h - 4);
        gfx_rframe(s, p.x, p.y, p.w, p.h, 6, 0x565D6E, 0x20232A);
        int ni = f->ctx_node;
        for (int i = 0; i < 5; i++) {
            if (!ui_button(u, 450 + (u32)i, rect(p.x + 5, p.y + 5 + i * 21, p.w - 10, 20), items[i], UI_FLAT)) continue;
            if (ni >= 0 && sc->n[ni].used) {
                if (i == 0) { sc->display = ni; changed(f); }
                else if (i == 1) { sc->n[ni].bypass = !sc->n[ni].bypass; changed(f); }
                else if (i == 2) { for (int k = 0; k < MAX_INPUTS; k++) sc->n[ni].in[k] = -1; changed(f); }
                else if (i == 3) duplicate_node(f, ni);
                else delete_node(f, ni);
            }
            close_overlay(f);
            return;
        }
        if ((u->press || u->rpress) && !rect_has(p, u->mx, u->my)) close_overlay(f);
    }
}

/* ---- open / save dialog --------------------------------------------------- */
static void dialog_confirm(Flow *f)
{
    char name[FS_NAME_MAX + 8];
    str_copy(name, f->dlg_name, sizeof name);
    if (!name[0]) return;
    if (!str_ends(name, ".flw")) str_cat(name, ".flw", sizeof name);
    if (str_len(name) >= FS_NAME_MAX) { say(f, true, "That name is too long"); return; }
    int kind = f->dlg;
    close_overlay(f);
    if (kind == DLG_SAVE) do_save(f, name);
    else do_open(f, name);
}

static void ui_dialog(Flow *f)
{
    UI *u = &f->ui;
    Surface *s = u->s;
    if (f->dlg == DLG_DISCARD) {
        Rect c = rect((s->w - 360) / 2, (s->h - 132) / 2, 360, 132);
        char msg[96];
        gfx_rframe(s, c.x - 2, c.y - 2, c.w + 4, c.h + 4, 8, 0x0C0D10, 0x0C0D10);
        gfx_rframe(s, c.x, c.y, c.w, c.h, 6, 0x5A6274, 0x23262D);
        gfx_text_bold(s, c.x + 16, c.y + 16, "Discard unsaved changes?", 0xFFFFFF);
        ksnprintf(msg, sizeof msg, "\"%s\" has edits that are not saved.", f->file[0] ? f->file : "untitled");
        gfx_text_fit(s, c.x + 16, c.y + 42, c.w - 32, msg, UI_DIM);
        bool cancel = ui_button(u, 311, rect(c.x + c.w - 228, c.y + c.h - 42, 90, 26), "Cancel", UI_NORMAL);
        bool discard = ui_button(u, 312, rect(c.x + c.w - 132, c.y + c.h - 42, 116, 26), "Discard", UI_DANGER);
        for (int i = 0; i < u->in->nkeys; i++) {
            if (u->in->keys[i].code == K_ESC) cancel = true;
            if (u->in->keys[i].code == K_ENTER) discard = true;
        }
        if (discard) {
            int action = f->pending;
            close_overlay(f);
            f->modified = false;
            perform(f, action, f->pending_name);
        } else if (cancel) {
            close_overlay(f);
        }
        return;
    }
    bool save = f->dlg == DLG_SAVE;
    Rect d = rect((s->w - 400) / 2, (s->h - 336) / 2, 400, 336);
    gfx_rframe(s, d.x - 2, d.y - 2, d.w + 4, d.h + 4, 8, 0x0C0D10, 0x0C0D10);
    gfx_rframe(s, d.x, d.y, d.w, d.h, 6, 0x5A6274, 0x23262D);
    gfx_rrect(s, d.x + 1, d.y + 1, d.w - 2, 28, 5, 0x2F3440);
    gfx_fill(s, d.x + 1, d.y + 20, d.w - 2, 9, 0x2F3440);
    gfx_text_bold(s, d.x + 12, d.y + 9, save ? "Save Scene As" : "Open Scene", 0xFFFFFF);
    gfx_text(s, d.x + d.w - 150, d.y + 9, "NodyFS volume  /", UI_DIM);

    for (int i = 0; i < u->in->nkeys; i++)
        if (u->in->keys[i].code == K_ESC) { close_overlay(f); return; }

    /* file list */
    static FsEntry list[FS_MAX_FILES];
    int n = fs_list(list, FS_MAX_FILES), shown = 0;
    Rect lr = rect(d.x + 12, d.y + 38, d.w - 24, 9 * 21 + 4);
    gfx_rframe(s, lr.x, lr.y, lr.w, lr.h, 4, UI_BORDER, UI_DARK);
    if (ui_hot(u, lr) && u->in->wheel) f->dlg_scroll -= u->in->wheel;
    int total = 0;
    for (int i = 0; i < n; i++) total += str_ends(list[i].name, ".flw");
    f->dlg_scroll = i_clamp(f->dlg_scroll, 0, i_max(0, total - 9));
    bool confirm = false;
    int idx = 0;
    for (int i = 0; i < n; i++) {
        if (!str_ends(list[i].name, ".flw")) continue;
        if (idx++ < f->dlg_scroll || shown >= 9) continue;
        Rect row = rect(lr.x + 2, lr.y + 2 + shown * 21, lr.w - 4, 21);
        bool hot = ui_hot(u, row), sel = str_cmp(list[i].name, f->dlg_name) == 0;
        if (sel) gfx_rrect(s, row.x, row.y, row.w, row.h, 3, 0x2B4C86);
        else if (hot) gfx_rrect(s, row.x, row.y, row.w, row.h, 3, 0x2A2D35);
        gfx_rrect(s, row.x + 7, row.y + 5, 12, 11, 2, 0x8A6BFF);
        gfx_fill(s, row.x + 10, row.y + 9, 6, 2, 0xFFFFFF);
        gfx_text(s, row.x + 26, row.y + 5, list[i].name, UI_TEXT);
        char buf[32];
        ksnprintf(buf, sizeof buf, "%u bytes", list[i].size);
        gfx_text(s, row.x + row.w - gfx_text_w(buf) - 8, row.y + 5, buf, UI_DIM);
        if (hot && u->press) {
            str_copy(f->dlg_name, list[i].name, sizeof f->dlg_name);
            if (u->dbl) confirm = true;
        }
        shown++;
    }
    if (total == 0) gfx_text(s, lr.x + 14, lr.y + 14, "No scenes saved yet.", UI_DIM);

    int y = lr.y + lr.h + 12;
    gfx_text(s, d.x + 14, y + 6, "Name", UI_DIM);
    if (ui_textfield(u, 301, rect(d.x + 52, y, d.w - 64, 24), f->dlg_name, FS_NAME_MAX - 4)) confirm = true;
    y += 38;
    if (ui_button(u, 302, rect(d.x + d.w - 198, y, 90, 26), "Cancel", UI_NORMAL)) { close_overlay(f); return; }
    if (ui_button(u, 303, rect(d.x + d.w - 102, y, 90, 26), save ? "Save" : "Open", UI_PRIMARY)) confirm = true;
    if (save && fs_size(f->dlg_name) >= 0) gfx_text(s, d.x + 14, y + 8, "Replaces the existing file", UI_ORANGE);
    if (confirm) dialog_confirm(f);
}

/* ---- keyboard ------------------------------------------------------------- */
static void shortcuts(Flow *f, const WinInput *in)
{
    Scene *sc = &f->sc;
    for (int i = 0; i < in->nkeys; i++) {
        const KeyEv *k = &in->keys[i];
        if (k->mods & MOD_CTRL) {
            if (k->code == 's') cmd_save(f);
            else if (k->code == 'o') request(f, ACT_OPEN_DIALOG, NULL);
            else if (k->code == 'n') request(f, ACT_NEW, NULL);
            continue;
        }
        switch (k->code) {
        case ' ':
            f->playing = !f->playing;
            f->last_tick = plat_ticks();
            break;
        case K_DELETE: case K_BACKSPACE:
            delete_node(f, sc->selected);
            break;
        case K_TAB:
            f->popup = POP_ADD;
            f->popup_at_point = pane_at(f, in->mx, in->my) == P_GRAPH;
            f->popup_x = in->mx; f->popup_y = in->my;
            f->popup_gx = in->mx - f->r_graph.x - sc->pan_x - NODE_W / 2;
            f->popup_gy = in->my - f->r_graph.y - sc->pan_y - 10;
            break;
        case 'd':
            if (sc->selected >= 0) { sc->display = sc->selected; changed(f); }
            break;
        case 'b':
            if (sc->selected >= 0) { sc->n[sc->selected].bypass = !sc->n[sc->selected].bypass; changed(f); }
            break;
        case 'f':
            home_view(f);
            break;
        case 'g':
            sc->grid = !sc->grid; f->view_dirty = true;
            break;
        case 'w':
            sc->wire = !sc->wire; f->view_dirty = true;
            break;
        }
    }
}

/* ---- per-frame update ----------------------------------------------------- */
static void update(Window *w, const WinInput *in)
{
    Flow *f = F;
    Surface *s = &w->surf;
    Scene *sc = &f->sc;
    u32 now = plat_ticks();
    layout(f, s->w, s->h);
    f->dirty |= f->next_dirty;
    f->next_dirty = 0;
    if (in->resized) { f->dirty = P_ALL; f->view_dirty = true; }

    if (f->playing && now - f->last_tick >= 1000 / FLOW_FPS) {
        f->last_tick = now - f->last_tick > 200 ? now : f->last_tick + 1000 / FLOW_FPS;
        sc->frame++;
        if (sc->loop && sc->frame >= sc->loop_len) sc->frame = 0;
        f->eval_dirty = true;
        f->dirty |= P_TIME;
    }
    if (now - f->fps_t0 >= 1000) {
        f->fps = (int)((u32)f->fps_n * 1000u / (now - f->fps_t0));
        f->fps_n = 0;
        f->fps_t0 = now;
    }
    if (f->status[0] && (i32)(now - f->status_until) > 0) { f->status[0] = 0; f->dirty |= P_TOOL; }

    bool modal = f->dlg != DLG_NONE || f->popup != POP_NONE;
    if (in->activity) {
        int mp = in->inside ? pane_at(f, in->mx, in->my) : 0;
        if (in->l_press || in->r_press || in->m_press) f->press_pane = mp;
        if (in->l_press || in->l_release || in->r_press || in->r_release || in->m_press || in->m_release || in->nkeys || in->wheel) {
            f->dirty = P_ALL;
            f->next_dirty = P_ALL;
        } else {
            f->dirty |= mp | f->last_pane;
            if (in->l_down || in->r_down || in->m_down) f->dirty |= f->press_pane;
        }
        f->last_pane = mp;
    }
    if (modal) f->dirty = P_ALL;

    if (f->eval_dirty) evaluate(f);
    if (f->view_dirty) render(f);

    if (f->dirty) {
        int drawn = f->dirty;
        f->dirty = 0;                   /* anything changed during this pass is redrawn next frame */
        ui_begin(&f->ui, s, in);
        f->ui.enabled = !modal;
        if (!modal) shortcuts(f, in);
        if (drawn & P_TOOL) ui_toolbar(f);
        if (drawn & P_VIEW) viewport_input(f);
        if (drawn & P_TIME) ui_timeline(f);
        if (drawn & P_INSP) ui_inspector(f);
        if (drawn & P_GRAPH) ui_graph(f);
        f->ui.enabled = true;
        if (modal) {                    /* overlays opened this frame are drawn from the next one */
            if (f->popup != POP_NONE) ui_popup(f);
            else if (f->dlg != DLG_NONE) ui_dialog(f);
        }
        ui_end(&f->ui);
        retitle(f);
        if (modal || drawn == P_ALL) wm_invalidate(w);
        else {
            if (drawn & P_TOOL) wm_invalidate_rect(w, f->r_tool.x, f->r_tool.y, f->r_tool.w, f->r_tool.h);
            if (drawn & P_TIME) wm_invalidate_rect(w, f->r_time.x, f->r_time.y, f->r_time.w, f->r_time.h);
            if (drawn & P_INSP) wm_invalidate_rect(w, f->r_insp.x, f->r_insp.y, f->r_insp.w, f->r_insp.h);
            if (drawn & P_GRAPH) wm_invalidate_rect(w, f->r_graph.x, f->r_graph.y, f->r_graph.w, f->r_graph.h);
        }
    }
}

static void closed(Window *w)
{
    (void)w;
    F->win = NULL;                      /* the scene stays in memory and comes back on relaunch */
}

/* ---- entry points --------------------------------------------------------- */
static bool flow_init(void)
{
    if (F) return true;
    Flow *f = kmalloc(sizeof *f);
    if (!f) return false;
    memset(f, 0, sizeof *f);
    f->zbuf = kmalloc((usize)wm_screen_w() * (usize)wm_screen_h() * sizeof(int));
    if (!f->zbuf || !arena_init(ARENA_BYTES)) { kfree(f->zbuf); kfree(f); return false; }
    scene_init(&f->sc);
    scene_demo(&f->sc, 0);
    f->playing = true;
    F = f;
    return true;
}

void flow_launch(void)
{
    if (!flow_init()) return;
    Flow *f = F;
    if (f->win && wm_is_open(f->win)) { wm_focus(f->win); return; }
    int w = i_min(1004, wm_screen_w() - 20), h = i_min(700, wm_screen_h() - WM_TASKBAR_H - WM_TITLE_H - 12);
    f->win = wm_create("Flow", (wm_screen_w() - w) / 2, WM_TITLE_H + 6, w, h, update, f);
    if (!f->win) return;
    f->win->on_close = closed;
    f->win->min_w = 660; f->win->min_h = 470;
    f->dirty = P_ALL;
    f->next_dirty = P_ALL;
    f->eval_dirty = true;
    f->drag = D_NONE;
    f->popup = POP_NONE;
    f->dlg = DLG_NONE;
    f->last_tick = plat_ticks();
    retitle(f);
}

Window *flow_window(void) { return F ? F->win : NULL; }

void flow_open_file(const char *name)
{
    flow_launch();
    if (!F || !F->win) return;
    wm_focus(F->win);
    request(F, ACT_OPEN_FILE, name);
}

/* first boot on a blank disk: leave a few scenes to open */
void flow_install_examples(void)
{
    static Scene sc;
    scene_init(&sc);
    for (int i = 0; i < scene_demo_count(); i++) {
        char name[FS_NAME_MAX];
        scene_demo(&sc, i);
        int n = scene_save(&sc, file_buf, sizeof file_buf);
        ksnprintf(name, sizeof name, "%s.flw", scene_demo_name(i));
        if (n > 0 && fs_write(name, file_buf, (u32)n) == 0) kprintf("flow: wrote example %s (%d bytes)\n", name, n);
    }
    scene_clear(&sc);
}
