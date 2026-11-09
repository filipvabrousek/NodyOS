/* NodyOS - System: what the machine looks like from inside. */
#include "apps/apps.h"
#include "gui/ui.h"
#include "lib/lib.h"
#include "lib/kmath.h"
#include "lib/fs.h"
#include "lib/plat.h"

static Window *win;
static UI ui;
static u32 last_draw, fps_t0, fps_f0, fps;

static void bar(Surface *s, int x, int y, int w, u32 used, u32 total, u32 col)
{
    gfx_rrect(s, x, y, w, 8, 3, UI_DARK);
    while (total > 0x100000) { total >>= 1; used >>= 1; }      /* keep the product in 32 bits */
    if (total) {
        int fw = (int)(used * (u32)w / total);
        if (fw > 0) gfx_rrect(s, x, y, i_clamp(fw, 4, w), 8, 3, col);
    }
}

static void row(Surface *s, int *y, const char *key, const char *val)
{
    gfx_text(s, 20, *y, key, UI_DIM);
    gfx_text_fit(s, 104, *y, s->w - 120, val, UI_TEXT);
    *y += 18;
}

static void update(Window *w, const WinInput *in)
{
    u32 now = plat_ticks();
    if (now - fps_t0 >= 1000) {
        fps = (wm_frame_count() - fps_f0) * 1000 / (now - fps_t0);
        fps_t0 = now; fps_f0 = wm_frame_count();
    }
    if (!in->activity && now - last_draw < 500) return;
    last_draw = now;

    Surface *s = &w->surf;
    ui_begin(&ui, s, in);
    gfx_fill(s, 0, 0, s->w, s->h, UI_BG);

    gfx_vgrad(s, 0, 0, s->w, 64, 0x2B3350, UI_BG);
    wm_draw_icon(s, 2, 20, 14, 36);
    gfx_text_big(s, 68, 14, "NodyOS", 0xFFFFFF, 2);
    gfx_text(s, 68, 38, "version 1.0  -  32-bit, written from scratch", UI_DIM);

    char buf[96];
    int y = 78;
    row(s, &y, "Processor", plat_cpu());

    u32 total, used;
    plat_meminfo(&total, &used);
    ksnprintf(buf, sizeof buf, "%u MB used of %u MB", used >> 10, total >> 10);
    row(s, &y, "Memory", buf);
    bar(s, 104, y - 3, s->w - 124, used, total, UI_ACCENT);
    y += 14;

    ksnprintf(buf, sizeof buf, "%d x %d, 32-bit colour", wm_screen_w(), wm_screen_h());
    row(s, &y, "Display", buf);

    ksnprintf(buf, sizeof buf, "%u MB ATA disk", disk_sectors() >> 11);
    row(s, &y, "Disk", buf);

    if (fs_mounted()) {
        u32 tot = fs_total_bytes(), fre = fs_free_bytes();
        FsEntry list[FS_MAX_FILES];
        int n = fs_list(list, FS_MAX_FILES);
        ksnprintf(buf, sizeof buf, "%d files, %u KB free of %u KB", n, fre >> 10, tot >> 10);
        row(s, &y, "NodyFS", buf);
        bar(s, 104, y - 3, s->w - 124, tot - fre, tot, 0x3CCBB0);
        y += 14;
    } else {
        row(s, &y, "NodyFS", "not mounted");
    }

    u32 sec = now / 1000;
    ksnprintf(buf, sizeof buf, "%u:%02u:%02u", sec / 3600, (sec / 60) % 60, sec % 60);
    row(s, &y, "Uptime", buf);
    ksnprintf(buf, sizeof buf, "%u fps, compositor %u ms", fps, wm_composite_ms());
    row(s, &y, "Desktop", buf);

    int by = s->h - 40;
    gfx_fill(s, 0, by - 10, s->w, 1, UI_BORDER);
    if (ui_button(&ui, 1, rect(s->w - 212, by, 94, 26), "Restart", UI_NORMAL)) {
        kprintf("sys: restart requested\n");
        plat_reboot();
    }
    if (ui_button(&ui, 2, rect(s->w - 110, by, 94, 26), "Shut Down", UI_DANGER)) {
        kprintf("sys: shutdown requested\n");
        plat_poweroff();
    }

    ui_end(&ui);
    wm_invalidate(w);
}

static void closed(Window *w) { (void)w; win = NULL; }

void sysinfo_launch(void)
{
    if (win && wm_is_open(win)) { wm_focus(win); return; }
    win = wm_create("System", wm_screen_w() - 420, 70, 380, 318, update, NULL);
    if (!win) return;
    win->on_close = closed;
    win->min_w = 340; win->min_h = 300;
}

Window *sysinfo_window(void) { return win; }
