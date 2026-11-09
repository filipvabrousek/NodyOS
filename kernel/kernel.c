/* NodyOS - kernel entry point and main loop. */
#include "kernel/kernel.h"
#include "gui/wm.h"
#include "lib/fs.h"
#include "apps/apps.h"

static const BootInfo *boot;

/* draw straight to the framebuffer: used before the desktop is up and by panics */
static bool raw_surface(Surface *s)
{
    if (!boot || boot->fb_bpp != 32) return false;
    surf_init(s, (u32 *)(uptr)boot->fb_addr, (int)boot->fb_width, (int)boot->fb_height, (int)(boot->fb_pitch / 4));
    return true;
}

void panic_screen(const char *text)
{
    Surface s;
    if (!raw_surface(&s)) return;
    gfx_fill(&s, 0, 0, s.w, 46, 0xA31515);
    gfx_text_bold(&s, 14, 8, "NodyOS kernel panic", 0xFFFFFF);
    gfx_text(&s, 14, 26, text, 0xFFFFFF);
}

static void splash(void)
{
    Surface s;
    if (!raw_surface(&s)) return;
    gfx_fill(&s, 0, 0, s.w, s.h, 0x12162A);
    gfx_disc(&s, (float)s.w / 2 - 79, (float)s.h / 2 - 9, 7, 0x4FD1C5);
    gfx_disc(&s, (float)s.w / 2 - 65, (float)s.h / 2 + 5, 7, 0x9F7AEA);
    gfx_text_big(&s, s.w / 2 - 47, s.h / 2 - 16, "NodyOS", 0xFFFFFF, 3);
    gfx_text(&s, s.w / 2 - 47, s.h / 2 + 22, "starting the desktop...", 0x8A8F9C);
}

void kmain(const BootInfo *bi)
{
    serial_init();
    kprintf("\nNodyOS kernel\n");
    if (bi->magic != 0x59444F4E) {
        kprintf("bad boot info\n");
        for (;;) cpu_hlt();
    }
    boot = bi;
    kprintf("boot: framebuffer %x, %ux%ux%u, pitch %u; kernel %u sectors; %u memory ranges\n",
            bi->fb_addr, bi->fb_width, bi->fb_height, bi->fb_bpp, bi->fb_pitch, bi->kernel_sectors, bi->mmap_count);

    cpu_init();
    mem_init(bi);
    font_init();
    splash();
    ps2_init();
    cpu_sti();

    bool disk = ata_init();
    bool fs = disk && fs_mount();
    kprintf("fs: %s\n", !fs ? "unavailable" : fs_was_formatted() ? "new NodyFS volume created" : "NodyFS volume mounted");
    if (fs) {
        FsEntry list[FS_MAX_FILES];
        int n = fs_list(list, FS_MAX_FILES);
        for (int i = 0; i < n; i++) kprintf("fs:   %-24s %6u bytes\n", list[i].name, list[i].size);
        kprintf("fs: %d files, %u KB free\n", n, fs_free_bytes() >> 10);
    }

    wm_init((void *)(uptr)bi->fb_addr, (int)bi->fb_width, (int)bi->fb_height, (int)bi->fb_pitch, (int)bi->fb_bpp);
    apps_register();
    if (fs && fs_was_formatted()) flow_install_examples();
    flow_launch();
    kprintf("desktop ready after %u ms\n", g_ticks);

    for (;;) {
        u32 t0 = g_ticks;
        wm_frame();
        while (g_ticks - t0 < 16) cpu_hlt();    /* cap at ~60 frames per second */
    }
}
