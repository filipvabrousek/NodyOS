/* NodyOS - PS/2 keyboard and mouse, plus the absolute pointer that emulators
 * expose through the VMware backdoor port (so the host cursor and the guest
 * cursor stay in step without grabbing the mouse). */
#include "kernel/kernel.h"
#include "lib/input.h"

/* ---- 8042 controller ------------------------------------------------------ */
static void ctl_wait_in(void)  { for (int i = 0; i < 100000 && (inb(0x64) & 2); i++) { } }
static bool ctl_wait_out(void) { for (int i = 0; i < 100000; i++) if (inb(0x64) & 1) return true; return false; }
static void ctl_cmd(u8 c)      { ctl_wait_in(); outb(0x64, c); }
static void ctl_data(u8 d)     { ctl_wait_in(); outb(0x60, d); }
static int  ctl_read(void)     { return ctl_wait_out() ? inb(0x60) : -1; }
static void ctl_flush(void)    { for (int i = 0; i < 64 && (inb(0x64) & 1); i++) inb(0x60); }

static int mouse_cmd(u8 c)
{
    ctl_cmd(0xD4);                      /* next byte goes to the aux device */
    ctl_data(c);
    return ctl_read();                  /* 0xFA = ACK */
}

/* ---- keyboard: scancode set 1 (translated), US layout -------------------- */
static const char map_plain[128] = {
    0, 27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', 8, 9,
    'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', 13, 0, 'a', 's',
    'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0, '\\', 'z', 'x', 'c', 'v',
    'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' ', 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, '7', '8', '9', '-', '4', '5', '6', '+', '1',
    '2', '3', '0', '.', 0, 0, 0, 0, 0
};
static const char map_shift[128] = {
    0, 27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', 8, 9,
    'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', 13, 0, 'A', 'S',
    'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0, '|', 'Z', 'X', 'C', 'V',
    'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' ', 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, '7', '8', '9', '-', '4', '5', '6', '+', '1',
    '2', '3', '0', '.', 0, 0, 0, 0, 0
};

static u8 mods;
static bool caps, ext;

static void kbd_byte(u8 sc)
{
    if (sc == 0xE0) { ext = true; return; }
    if (sc == 0xE1) return;
    bool release = (sc & 0x80) != 0;
    u8 code = sc & 0x7F;
    bool e = ext;
    ext = false;

    u8 mod = 0;
    if (!e && (code == 0x2A || code == 0x36)) mod = MOD_SHIFT;
    else if (code == 0x1D) mod = MOD_CTRL;
    else if (code == 0x38) mod = MOD_ALT;
    if (mod) {
        if (release) mods &= (u8)~mod; else mods |= mod;
        input_set_mods(mods);
        return;
    }
    if (e && (code == 0x2A || code == 0x36)) return;    /* fake shifts around extended keys */
    if (release) return;
    if (code == 0x3A) { caps = !caps; return; }

    KeyEv ev = { 0, 0, mods };
    if (e) {
        switch (code) {
        case 0x48: ev.code = K_UP; break;
        case 0x50: ev.code = K_DOWN; break;
        case 0x4B: ev.code = K_LEFT; break;
        case 0x4D: ev.code = K_RIGHT; break;
        case 0x53: ev.code = K_DELETE; break;
        case 0x47: ev.code = K_HOME; break;
        case 0x4F: ev.code = K_END; break;
        case 0x49: ev.code = K_PGUP; break;
        case 0x51: ev.code = K_PGDN; break;
        case 0x1C: ev.code = K_ENTER; ev.ch = 13; break;
        case 0x35: ev.code = '/'; ev.ch = '/'; break;
        default: return;
        }
    } else if (code >= 0x3B && code <= 0x44) {
        ev.code = (u16)(K_F1 + (code - 0x3B));
    } else if (code == 0x57) {
        ev.code = K_F11;
    } else if (code == 0x58) {
        ev.code = K_F12;
    } else {
        char plain = map_plain[code];
        if (!plain) return;
        bool shift = (mods & MOD_SHIFT) != 0;
        if (plain >= 'a' && plain <= 'z' && caps) shift = !shift;
        ev.code = (u16)(u8)plain;
        ev.ch = (u8)(shift ? map_shift[code] : plain);
        if (mods & (MOD_CTRL | MOD_ALT)) ev.ch = 0;     /* shortcuts are not text */
    }
    input_push_key(&ev);
}

/* ---- absolute pointer through the VMware backdoor ------------------------- */
#define VM_MAGIC        0x564D5868u
#define VM_PORT         0x5658
#define VM_GETVERSION   10
#define VM_ABS_DATA     39
#define VM_ABS_STATUS   40
#define VM_ABS_COMMAND  41
#define VM_CMD_READ_ID  0x45414552u
#define VM_CMD_ABSOLUTE 0x53424152u
#define VM_VERSION_ID   0x3442554Au

static bool vm_abs;

static void vm_call(u32 cmd, u32 arg, u32 *a, u32 *b, u32 *c, u32 *d)
{
    u32 ra, rb, rc, rd;
    __asm__ volatile("inl %%dx, %%eax"
                     : "=a"(ra), "=b"(rb), "=c"(rc), "=d"(rd)
                     : "a"(VM_MAGIC), "b"(arg), "c"(cmd), "d"(VM_PORT)
                     : "esi", "edi", "memory");
    if (a) *a = ra;
    if (b) *b = rb;
    if (c) *c = rc;
    if (d) *d = rd;
}

static bool vm_init(void)
{
    u32 a, b;
    vm_call(VM_GETVERSION, ~VM_MAGIC, &a, &b, NULL, NULL);
    if (b != VM_MAGIC || a == 0xFFFFFFFFu) return false;       /* no backdoor */
    vm_call(VM_ABS_COMMAND, VM_CMD_READ_ID, NULL, NULL, NULL, NULL);
    vm_call(VM_ABS_STATUS, 0, &a, NULL, NULL, NULL);
    if ((a & 0xFFFF) == 0) return false;
    vm_call(VM_ABS_DATA, 1, &a, NULL, NULL, NULL);
    if (a != VM_VERSION_ID) return false;
    vm_call(VM_ABS_COMMAND, VM_CMD_ABSOLUTE, NULL, NULL, NULL, NULL);
    return true;
}

/* returns true if at least one absolute packet was waiting */
static bool vm_drain(void)
{
    bool any = false;
    for (int guard = 0; guard < 256; guard++) {
        u32 st, x, y, z;
        vm_call(VM_ABS_STATUS, 0, &st, NULL, NULL, NULL);
        if ((st & 0xFFFF0000u) == 0xFFFF0000u || (st & 0xFFFF) < 4) break;
        vm_call(VM_ABS_DATA, 4, &st, &x, &y, &z);
        MouseEv ev = { 0 };
        ev.absolute = 1;
        ev.x = (u16)x;
        ev.y = (u16)y;
        ev.wheel = (i16)(-(i8)(u8)z);
        if (st & 0x20) ev.buttons |= MB_LEFT;
        if (st & 0x10) ev.buttons |= MB_RIGHT;
        if (st & 0x08) ev.buttons |= MB_MIDDLE;
        input_push_mouse(&ev);
        any = true;
    }
    return any;
}

/* ---- mouse packets -------------------------------------------------------- */
static u8 pkt[4];
static int pkt_n, pkt_len = 3;

static void mouse_byte(u8 b)
{
    if (pkt_n == 0 && !(b & 0x08)) return;              /* resynchronise */
    pkt[pkt_n++] = b;
    if (pkt_n < pkt_len) return;
    pkt_n = 0;

    /* With the absolute pointer active the PS/2 packet is only a wake-up call;
     * the real data sits in the backdoor queue. */
    if (vm_abs && vm_drain()) return;

    MouseEv ev = { 0 };
    ev.buttons = pkt[0] & 7;
    ev.dx = (i16)((int)pkt[1] - ((pkt[0] & 0x10) ? 256 : 0));
    ev.dy = (i16)(-((int)pkt[2] - ((pkt[0] & 0x20) ? 256 : 0)));       /* screen y grows downward */
    if (pkt[0] & 0xC0) { ev.dx = 0; ev.dy = 0; }                        /* overflow */
    if (pkt_len == 4) ev.wheel = (i16)(-(i8)pkt[3]);
    input_push_mouse(&ev);
}

static void ps2_irq(void)
{
    for (int i = 0; i < 16; i++) {
        u8 st = inb(0x64);
        if (!(st & 1)) break;
        u8 b = inb(0x60);
        if (st & 0x20) mouse_byte(b); else kbd_byte(b);
    }
}

bool ps2_absolute(void) { return vm_abs; }

void ps2_init(void)
{
    ctl_cmd(0xAD);                      /* disable both ports while configuring */
    ctl_cmd(0xA7);
    ctl_flush();

    ctl_cmd(0x20);
    int cfg = ctl_read();
    if (cfg < 0) cfg = 0x45;
    cfg |= 0x43;                        /* IRQ1, IRQ12, scancode translation */
    cfg &= ~0x30;                       /* both clocks running */
    ctl_cmd(0x60);
    ctl_data((u8)cfg);

    ctl_cmd(0xAE);
    ctl_cmd(0xA8);

    mouse_cmd(0xF6);                    /* defaults */
    mouse_cmd(0xF3); mouse_cmd(200);    /* magic sample-rate sequence: enable the wheel */
    mouse_cmd(0xF3); mouse_cmd(100);
    mouse_cmd(0xF3); mouse_cmd(80);
    mouse_cmd(0xF2);
    int id = ctl_read();
    pkt_len = (id == 3) ? 4 : 3;
    mouse_cmd(0xF3); mouse_cmd(100);
    mouse_cmd(0xF4);                    /* start reporting */
    ctl_flush();

    vm_abs = vm_init();
    kprintf("ps2: mouse id %d, %d-byte packets, absolute pointer %s\n", id, pkt_len, vm_abs ? "on" : "off");

    irq_install(1, ps2_irq);
    irq_install(12, ps2_irq);
}
