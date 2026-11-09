/* NodyOS - Files: browse the NodyFS volume, open scenes in Flow, delete. */
#include "apps/apps.h"
#include "gui/ui.h"
#include "lib/lib.h"
#include "lib/kmath.h"
#include "lib/fs.h"
#include "lib/plat.h"

static Window *win;
static UI ui;
static FsEntry list[FS_MAX_FILES];
static int count;
static char selected[FS_NAME_MAX];
static u32 last_draw, confirm_until;
static char confirm[FS_NAME_MAX];
static int scroll;

static int find_selected(void)
{
    for (int i = 0; i < count; i++) if (str_cmp(list[i].name, selected) == 0) return i;
    return -1;
}

static void update(Window *w, const WinInput *in)
{
    u32 now = plat_ticks();
    if (!in->activity && now - last_draw < 1000) return;
    last_draw = now;

    Surface *s = &w->surf;
    ui_begin(&ui, s, in);
    count = fs_list(list, FS_MAX_FILES);
    int sel = find_selected();
    bool open = false, del = false;

    gfx_fill(s, 0, 0, s->w, s->h, UI_BG);
    gfx_fill(s, 0, 0, s->w, 36, UI_PANEL);
    gfx_fill(s, 0, 36, s->w, 1, UI_DARK);
    if (ui_button(&ui, 1, rect(8, 6, 64, 24), "Open", sel >= 0 ? UI_PRIMARY : UI_NORMAL)) open = true;
    /* deleting takes two clicks: the first one arms the button for a few seconds */
    bool armed = confirm[0] && str_cmp(confirm, selected) == 0 && (i32)(confirm_until - now) > 0;
    if (ui_button(&ui, 2, rect(78, 6, armed ? 104 : 64, 24), armed ? "Really delete?" : "Delete", armed ? UI_DANGER : UI_NORMAL)) del = true;
    gfx_text(s, armed ? 196 : 156, 13, "NodyFS volume  /", UI_DIM);

    for (int i = 0; i < in->nkeys; i++) {
        if (in->keys[i].code == K_ENTER) open = true;
        if (in->keys[i].code == K_DELETE) del = true;
        if (in->keys[i].code == K_DOWN && sel + 1 < count) str_copy(selected, list[sel + 1].name, sizeof selected);
        if (in->keys[i].code == K_UP && sel > 0) str_copy(selected, list[sel - 1].name, sizeof selected);
    }

    /* column header */
    int y = 37;
    gfx_fill(s, 0, y, s->w, 20, UI_PANEL2);
    gfx_text(s, 34, y + 5, "Name", UI_DIM);
    gfx_text(s, s->w - 196, y + 5, "Size", UI_DIM);
    gfx_text(s, s->w - 124, y + 5, "Modified", UI_DIM);
    y += 20;

    int rows = (s->h - y - 24) / 20;
    if (in->wheel) scroll -= in->wheel * 2;
    scroll = i_clamp(scroll, 0, i_max(0, count - rows));
    gfx_clip(s, 0, y, s->w, rows * 20);
    for (int i = scroll; i < count && i < scroll + rows; i++) {
        Rect r = rect(0, y + (i - scroll) * 20, s->w, 20);
        bool hot = ui_hot(&ui, r);
        bool is_sel = str_cmp(list[i].name, selected) == 0;
        if (is_sel) gfx_fill(s, r.x, r.y, r.w, r.h, 0x2B4C86);
        else if (hot) gfx_fill(s, r.x, r.y, r.w, r.h, 0x2A2D35);
        if (hot && ui.press) {
            str_copy(selected, list[i].name, sizeof selected);
            if (ui.dbl) open = true;
        }
        bool scene = str_ends(list[i].name, ".flw");
        gfx_rrect(s, 12, r.y + 4, 13, 12, 2, scene ? 0x8A6BFF : 0x6C7384);
        if (scene) gfx_fill(s, 15, r.y + 9, 7, 2, 0xFFFFFF);
        gfx_text_fit(s, 34, r.y + 5, s->w - 240, list[i].name, UI_TEXT);
        char buf[48];
        if (list[i].size < 10240) ksnprintf(buf, sizeof buf, "%u B", list[i].size);
        else ksnprintf(buf, sizeof buf, "%u KB", list[i].size >> 10);
        gfx_text(s, s->w - 196, r.y + 5, buf, UI_DIM);
        int yr, mo, d, h, mi;
        fs_unpack_time(list[i].mtime, &yr, &mo, &d, &h, &mi);
        ksnprintf(buf, sizeof buf, "%04d-%02d-%02d %02d:%02d", yr, mo, d, h, mi);
        gfx_text(s, s->w - 124, r.y + 5, buf, UI_DIM);
    }
    gfx_unclip(s);
    if (count == 0) gfx_text(s, 34, y + 14, "No files yet. Save a scene from Flow.", UI_DIM);

    /* status bar */
    char buf[96];
    gfx_fill(s, 0, s->h - 22, s->w, 22, UI_PANEL);
    gfx_fill(s, 0, s->h - 23, s->w, 1, UI_DARK);
    ksnprintf(buf, sizeof buf, "%d file%s   %u KB free of %u KB", count, count == 1 ? "" : "s",
              fs_free_bytes() >> 10, fs_total_bytes() >> 10);
    gfx_text(s, 10, s->h - 16, buf, UI_DIM);

    ui_end(&ui);
    wm_invalidate(w);

    if (open && selected[0] && find_selected() >= 0) {
        if (str_ends(selected, ".flw")) flow_open_file(selected);
    } else if (del && selected[0] && find_selected() >= 0) {
        if (armed) {
            kprintf("files: delete %s\n", selected);
            fs_delete(selected);
            selected[0] = 0;
            confirm[0] = 0;
        } else {
            str_copy(confirm, selected, sizeof confirm);
            confirm_until = now + 4000;
        }
        last_draw = 0;
        w->in.activity = true;
    }
}

static void closed(Window *w) { (void)w; win = NULL; }

void files_launch(void)
{
    if (win && wm_is_open(win)) { wm_focus(win); return; }
    win = wm_create("Files", 150, 120, 470, 300, update, NULL);
    if (!win) return;
    win->on_close = closed;
    win->min_w = 380; win->min_h = 180;
}

Window *files_window(void) { return win; }

void apps_register(void)
{
    wm_register_app("Flow", 0, flow_launch, flow_window);
    wm_register_app("Files", 1, files_launch, files_window);
    wm_register_app("System", 2, sysinfo_launch, sysinfo_window);
}
