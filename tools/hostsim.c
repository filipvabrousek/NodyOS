/* hostsim - run the NodyOS GUI stack (window manager, apps, 3D engine, NodyFS)
 * as a normal process on the development machine. It is a test harness only:
 * it replaces the hardware layer (lib/plat.h) with libc and feeds scripted
 * input, so the code can be exercised under sanitizers. Nothing here is part
 * of the operating system image.
 *
 *   build/hostsim [--disk FILE] "<script>"
 *
 * Script: the same ';'-separated commands as tools/vm.py, plus
 *   frames N        advance N frames (16 ms of virtual time each)
 *   shot FILE.ppm   dump the back buffer
 *   monkey N SEED   N random input events (add "soft" to skip New / Delete)
 *   fuzz N SEED     N random graph edits, each evaluated, rendered and
 *                   round-tripped through the .flw format
 *   fstest          NodyFS self-test
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "lib/plat.h"
#include "lib/lib.h"
#include "lib/input.h"
#include "lib/fs.h"
#include "gui/wm.h"
#include "apps/apps.h"
#include "flow/engine.h"

#define W 1024
#define H 768

static u32 now_ms;
static FILE *disk;
static u32 disk_n = 65536;
static int mouse_x = W / 2, mouse_y = H / 2, mouse_btn;

u32   plat_ticks(void) { return now_ms; }
void  plat_puts(const char *s) { fputs(s, stdout); }
void  plat_meminfo(u32 *total_kb, u32 *used_kb) { *total_kb = 262144; *used_kb = 20000; }
const char *plat_cpu(void) { return "host simulator"; }
void  plat_reboot(void) { puts("[reboot requested]"); }
void  plat_poweroff(void) { puts("[poweroff requested]"); }
void *kmalloc(usize n) { return malloc(n); }
void  kfree(void *p) { free(p); }
u32   disk_sectors(void) { return disk ? disk_n : 0; }

void plat_now(DateTime *dt)
{
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    dt->year = (u16)(tm->tm_year + 1900); dt->month = (u8)(tm->tm_mon + 1); dt->day = (u8)tm->tm_mday;
    dt->hour = (u8)tm->tm_hour; dt->min = (u8)tm->tm_min; dt->sec = (u8)tm->tm_sec;
}

int disk_read(u32 lba, u32 count, void *buf)
{
    if (!disk || fseek(disk, (long)lba * 512, SEEK_SET)) return -1;
    return fread(buf, 512, count, disk) == count ? 0 : -1;
}

int disk_write(u32 lba, u32 count, const void *buf)
{
    if (!disk || fseek(disk, (long)lba * 512, SEEK_SET)) return -1;
    if (fwrite(buf, 512, count, disk) != count) return -1;
    fflush(disk);
    return 0;
}

static void frames(int n)
{
    for (int i = 0; i < n; i++) { wm_frame(); now_ms += 16; }
}

static void send_mouse(int wheel)
{
    MouseEv e;
    memset(&e, 0, sizeof e);
    e.absolute = 1;
    e.x = (u16)((mouse_x * 65536 + 32768) / W);
    e.y = (u16)((mouse_y * 65536 + 32768) / H);
    e.buttons = (u8)mouse_btn;
    e.wheel = (i16)wheel;
    input_push_mouse(&e);
    frames(1);
}

static int button_bit(const char *name)
{
    if (name && !strcmp(name, "right")) return MB_RIGHT;
    if (name && !strcmp(name, "middle")) return MB_MIDDLE;
    return MB_LEFT;
}

static void send_key(const char *name, u8 mods)
{
    KeyEv e;
    memset(&e, 0, sizeof e);
    e.mods = mods;
    if (!strcmp(name, "ret")) { e.code = K_ENTER; e.ch = 13; }
    else if (!strcmp(name, "esc")) e.code = K_ESC;
    else if (!strcmp(name, "delete")) e.code = K_DELETE;
    else if (!strcmp(name, "backspace")) e.code = K_BACKSPACE;
    else if (!strcmp(name, "tab")) e.code = K_TAB;
    else if (!strcmp(name, "spc")) { e.code = ' '; e.ch = ' '; }
    else if (!strcmp(name, "up")) e.code = K_UP;
    else if (!strcmp(name, "down")) e.code = K_DOWN;
    else if (!strcmp(name, "dot")) { e.code = '.'; e.ch = '.'; }
    else if (!strcmp(name, "minus")) { e.code = '-'; e.ch = (mods & MOD_SHIFT) ? '_' : '-'; }
    else { e.code = (u16)name[0]; e.ch = (u8)name[0]; }
    if (mods & (MOD_CTRL | MOD_ALT)) e.ch = 0;
    input_set_mods(mods);
    input_push_key(&e);
    frames(1);
    input_set_mods(0);
}

static void shot(const char *path)
{
    Surface *s = wm_backbuffer();
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return; }
    fprintf(f, "P6\n%d %d\n255\n", s->w, s->h);
    for (int i = 0; i < s->w * s->h; i++) {
        u32 p = s->px[i];
        fputc((int)(p >> 16) & 255, f); fputc((int)(p >> 8) & 255, f); fputc((int)p & 255, f);
    }
    fclose(f);
}

/* NodyFS self-test: block chains, overwrite, delete, directory limit, remount. */
static void fstest(void)
{
    static u8 in[70000], out[70000];
    static const u32 sizes[] = { 0, 1, 511, 4095, 4096, 4097, 8192, 20000, 65000 };
    u32 free0 = fs_free_bytes();
    static FsEntry listing[FS_MAX_FILES];
    int fails = 0, base = fs_list(listing, FS_MAX_FILES);
#define CHECK(c, msg) do { if (!(c)) { printf("fstest: FAILED %s\n", msg); fails++; } } while (0)
    for (int round = 0; round < 3; round++)
        for (unsigned i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
            char name[32];
            u32 n = sizes[(i + (unsigned)round * 4) % (sizeof sizes / sizeof sizes[0])];     /* overwrite with other sizes */
            snprintf(name, sizeof name, "t%u.bin", i);
            for (u32 k = 0; k < n; k++) in[k] = (u8)(k * 7 + i * 13 + (unsigned)round);
            CHECK(fs_write(name, in, n) == 0, "write");
            memset(out, 0xEE, sizeof out);
            CHECK(fs_size(name) == (int)n, "size");
            CHECK(fs_read(name, out, sizeof out) == (int)n && memcmp(in, out, n) == 0, "read back");
        }
    CHECK(fs_mount(), "remount");                       /* everything must come back from disk */
    for (unsigned i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        char name[32];
        u32 n = sizes[(i + 8) % (sizeof sizes / sizeof sizes[0])];
        snprintf(name, sizeof name, "t%u.bin", i);
        for (u32 k = 0; k < n; k++) in[k] = (u8)(k * 7 + i * 13 + 2);
        CHECK(fs_read(name, out, sizeof out) == (int)n && memcmp(in, out, n) == 0, "read after remount");
    }
    int made = 0;                                       /* fill the directory */
    for (int i = 0; i < FS_MAX_FILES + 4; i++) {
        char name[32];
        snprintf(name, sizeof name, "fill%d", i);
        if (fs_write(name, "x", 1) == 0) made++;
    }
    CHECK(base + 9 + made == FS_MAX_FILES, "directory limit");
    CHECK(fs_write("bad name", "x", 1) == -2 && fs_write("", "x", 1) == -2, "name validation");
    for (int i = 0; i < FS_MAX_FILES + 4; i++) { char name[32]; snprintf(name, sizeof name, "fill%d", i); fs_delete(name); }
    for (unsigned i = 0; i < 9; i++) { char name[32]; snprintf(name, sizeof name, "t%u.bin", i); CHECK(fs_delete(name) == 0, "delete"); }
    CHECK(fs_free_bytes() == free0, "all blocks returned");
    CHECK(fs_read("t3.bin", out, 10) == -1, "deleted file is gone");
    printf("fstest: %s\n", fails ? "FAILED" : "all checks passed");
    if (fails) exit(4);
#undef CHECK
}

static int rnd(unsigned *r, unsigned n)
{
    *r ^= *r << 13; *r ^= *r >> 17; *r ^= *r << 5;
    return (int)((*r >> 4) % n);
}

/* Engine fuzzer: random graphs, random wiring, random parameter values, with
 * an evaluation, a render and a save/load round trip after every change. */
static void fuzz(int steps, unsigned seed)
{
    static Scene sc, back;
    static u8 a[SCENE_FILE_MAX], b[SCENE_FILE_MAX];
    static u32 px[640 * 400];
    static int zb[640 * 400];
    Surface surf;
    surf_init(&surf, px, 640, 400, 640);
    scene_init(&sc);
    scene_init(&back);
    unsigned r = seed * 2654435761u + 99u;
    long tris = 0, evals = 0;
#define RND(n) rnd(&r, (unsigned)(n))
    for (int i = 0; i < steps; i++) {
        int act = RND(100), used[MAX_NODES], nu = 0;
        for (int k = 0; k < MAX_NODES; k++) if (sc.n[k].used) used[nu++] = k;
        if (act < 22 || nu == 0) {
            int id = scene_add(&sc, RND(g_ntypes), RND(600), RND(200));
            if (id >= 0 && RND(3) == 0) sc.display = id;
        } else if (act < 50) {
            int src = used[RND(nu)], dst = used[RND(nu)];
            if (g_types[sc.n[dst].type].nin) scene_connect(&sc, RND(8) ? src : -1, dst, RND(g_types[sc.n[dst].type].nin));
        } else if (act < 75) {
            Node *n = &sc.n[used[RND(nu)]];
            const NodeType *t = &g_types[n->type];
            if (t->np) {
                int k = RND(t->np);
                const ParamDef *d = &t->p[k];
                float hi = d->kind == PK_ENUM ? (float)(param_enum_count(d) - 1) : d->hi;
                float v = d->lo + (hi - d->lo) * (float)RND(1001) / 1000.0f;
                if (d->kind != PK_FLOAT) v = (float)(int)(v + 0.5f);
                if (v > hi) v = hi;                     /* rounding may step past the top of the range */
                n->p[k] = v;
            }
        } else if (act < 80) scene_del(&sc, used[RND(nu)]);
        else if (act < 88) sc.display = used[RND(nu)];
        else if (act < 92) sc.n[used[RND(nu)]].bypass = RND(2);
        else if (act < 95) { sc.yaw = (float)RND(628) / 100.0f; sc.pitch = (float)(RND(290) - 145) / 100.0f; sc.dist = 0.6f + (float)RND(300) / 10.0f; }
        else if (act < 97) sc.frame = 0;
        else if (act < 98) scene_clear(&sc);

        sc.frame++;
        sc.wire = RND(6) == 0;
        Geo *g = scene_eval(&sc);
        Guide guides[400];
        sc.selected = nu ? used[RND(nu)] : -1;
        if (sc.selected >= 0 && !sc.n[sc.selected].used) sc.selected = -1;
        int ng = scene_guides(&sc, guides, 400);
        RenderStats st;
        render_view(&surf, rect(0, 0, 640, 400), zb, &sc, g, guides, ng, &st);
        tris += st.tris;
        evals++;

        if (i % 7 == 0) {                               /* file format round trip must be lossless */
            int na = scene_save(&sc, a, sizeof a);
            if (na <= 0 || !scene_load(&back, a, na)) { printf("fuzz: save/load failed at step %d\n", i); exit(3); }
            int nb = scene_save(&back, b, sizeof b);    /* loading compacts node indices: compare re-saved bytes */
            if (na != nb || memcmp(a, b, (size_t)na)) {
                int at = 56;
                while (at < na && at < nb && a[at] == b[at]) at++;
                printf("fuzz: round trip differs at step %d (%d vs %d bytes), first at offset %d: %02x vs %02x\n", i, na, nb, at, a[at], b[at]);
                for (int q = 56; q < na; ) { printf("  node @%d %-10s np %d nin %d\n", q, (char *)a + q, a[q + 17], a[q + 18]); q += 20 + a[q + 17] * 4 + ((a[q + 18] + 3) & ~3); }
                exit(3);
            }
            a[56 + RND(na - 56 > 0 ? na - 56 : 1)] ^= (u8)(1 << RND(8));     /* corrupt one bit: must be rejected */
            if (na > 56 && scene_load(&back, a, na)) { printf("fuzz: corrupt file accepted at step %d\n", i); exit(3); }
        }
    }
    printf("fuzz: seed %u, %d steps, %ld evaluations, %ld triangles rendered\n", seed, steps, evals, tris);
    scene_clear(&sc);
    scene_clear(&back);
#undef RND
}

static void run(char *script)
{
    for (char *cmd = strtok(script, ";"); cmd; cmd = strtok(NULL, ";")) {
        char *a[8] = { 0 };
        int n = 0;
        char line[256];
        snprintf(line, sizeof line, "%s", cmd);
        char *save = NULL;
        for (char *t = strtok_r(line, " \t\n", &save); t && n < 8; t = strtok_r(NULL, " \t\n", &save)) a[n++] = t;
        if (!n) continue;
        if (!strcmp(a[0], "frames")) frames(atoi(a[1]));
        else if (!strcmp(a[0], "wait")) frames((int)(atof(a[1]) * 60));
        else if (!strcmp(a[0], "move")) { mouse_x = atoi(a[1]); mouse_y = atoi(a[2]); send_mouse(0); }
        else if (!strcmp(a[0], "down")) { mouse_btn |= button_bit(a[1]); send_mouse(0); }
        else if (!strcmp(a[0], "up")) { mouse_btn &= ~button_bit(a[1]); send_mouse(0); }
        else if (!strcmp(a[0], "click")) {
            mouse_x = atoi(a[1]); mouse_y = atoi(a[2]); send_mouse(0);
            mouse_btn |= button_bit(a[3]); send_mouse(0);
            mouse_btn &= ~button_bit(a[3]); send_mouse(0);
        } else if (!strcmp(a[0], "dclick")) {
            mouse_x = atoi(a[1]); mouse_y = atoi(a[2]); send_mouse(0);
            for (int k = 0; k < 2; k++) { mouse_btn |= MB_LEFT; send_mouse(0); mouse_btn &= ~MB_LEFT; send_mouse(0); }
        } else if (!strcmp(a[0], "drag")) {
            int x0 = atoi(a[1]), y0 = atoi(a[2]), x1 = atoi(a[3]), y1 = atoi(a[4]), b = button_bit(a[5]);
            mouse_x = x0; mouse_y = y0; send_mouse(0);
            mouse_btn |= b; send_mouse(0);
            for (int i = 1; i <= 12; i++) { mouse_x = x0 + (x1 - x0) * i / 12; mouse_y = y0 + (y1 - y0) * i / 12; send_mouse(0); }
            mouse_btn &= ~b; send_mouse(0);
        } else if (!strcmp(a[0], "wheel")) send_mouse(atoi(a[1]));
        else if (!strcmp(a[0], "key")) send_key(a[1], 0);
        else if (!strcmp(a[0], "combo")) {
            u8 m = 0;
            for (int i = 1; i < n - 1; i++) m |= !strcmp(a[i], "ctrl") ? MOD_CTRL : !strcmp(a[i], "shift") ? MOD_SHIFT : MOD_ALT;
            send_key(a[n - 1], m);
        } else if (!strcmp(a[0], "type")) {
            for (const char *p = a[1]; p && *p; p++) { char k[2] = { *p, 0 }; send_key(*p == '.' ? "dot" : *p == '-' ? "minus" : k, 0); }
        } else if (!strcmp(a[0], "shot")) shot(a[1]);
        else if (!strcmp(a[0], "fstest")) fstest();
        else if (!strcmp(a[0], "fuzz")) fuzz(atoi(a[1]), (unsigned)(n > 2 ? atoi(a[2]) : 1));
        else if (!strcmp(a[0], "monkey")) {
            /* random input: clicks, drags, wheel and keys all over the screen */
            static const char *const keys[] = { "delete", "tab", "esc", "spc", "ret", "backspace", "d", "b", "f", "g", "w",
                                                "a", "x", "1", "up", "down" };
            unsigned r = (unsigned)(n > 2 ? atoi(a[2]) : 1) * 2654435761u + 12345u;
            int soft = n > 3;                   /* "soft": no New / Delete, so graphs get to grow */
            for (int i = 0, steps = atoi(a[1]); i < steps; i++) {
                r ^= r << 13; r ^= r >> 17; r ^= r << 5;
                unsigned act = r % 100, v = r >> 8;
                if (act < 45) { mouse_x = (int)(v % W); mouse_y = (int)((v >> 10) % H); send_mouse(0); }
                else if (act < 60) { mouse_btn ^= MB_LEFT; send_mouse(0); }
                else if (act < 66) { mouse_btn ^= MB_RIGHT; send_mouse(0); }
                else if (act < 69) { mouse_btn ^= MB_MIDDLE; send_mouse(0); }
                else if (act < 75) send_mouse((int)(v % 7) - 3);
                else if (act < 90) send_key(keys[soft ? 1 + v % 4 : v % (sizeof keys / sizeof keys[0])], 0);
                else if (act < 92 && !soft) send_key(v & 1 ? "n" : "o", MOD_CTRL);
                else if (act < 93) send_key("s", MOD_CTRL);
                else frames((int)(v % 4));
            }
            mouse_btn = 0;
            send_mouse(0);
        }
        else { fprintf(stderr, "unknown command: %s\n", a[0]); exit(2); }
    }
}

int main(int argc, char **argv)
{
    const char *path = "build/host.img";
    char *script = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--disk") && i + 1 < argc) path = argv[++i];
        else script = argv[i];
    }
    disk = fopen(path, "r+b");
    if (!disk) {
        disk = fopen(path, "w+b");
        if (!disk) { perror(path); return 1; }
        fseek(disk, (long)disk_n * 512 - 1, SEEK_SET);
        fputc(0, disk);
        fflush(disk);
    } else {
        fseek(disk, 0, SEEK_END);
        disk_n = (u32)(ftell(disk) / 512);
    }
    static u32 fb[W * H];
    bool fs = fs_mount();
    printf("fs: %s\n", !fs ? "unavailable" : fs_was_formatted() ? "new volume" : "mounted");
    wm_init(fb, W, H, W * 4, 32);
    apps_register();
    if (fs && fs_was_formatted()) flow_install_examples();
    flow_launch();
    frames(2);
    if (script) run(script);
    fclose(disk);
    return 0;
}
