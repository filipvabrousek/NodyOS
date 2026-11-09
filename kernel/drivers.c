/* NodyOS - small drivers: serial console, ATA disk, real-time clock, power. */
#include "kernel/kernel.h"

/* ---- COM1 debug console -------------------------------------------------- */
#define COM1 0x3F8

void serial_init(void)
{
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x80);               /* divisor latch */
    outb(COM1 + 0, 0x01);               /* 115200 baud */
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);               /* 8N1 */
    outb(COM1 + 2, 0xC7);               /* FIFO on */
    outb(COM1 + 4, 0x03);
}

void serial_putc(char c)
{
    for (int i = 0; i < 100000 && !(inb(COM1 + 5) & 0x20); i++) { }
    outb(COM1, (u8)c);
}

void plat_puts(const char *s)
{
    for (; *s; s++) {
        if (*s == '\n') serial_putc('\r');
        serial_putc(*s);
    }
}

/* ---- ATA PIO, primary bus, master drive, LBA28 ---------------------------- */
#define ATA_DATA   0x1F0
#define ATA_COUNT  0x1F2
#define ATA_LBA0   0x1F3
#define ATA_LBA1   0x1F4
#define ATA_LBA2   0x1F5
#define ATA_DRIVE  0x1F6
#define ATA_CMD    0x1F7
#define ATA_CTRL   0x3F6

static u32 ata_total;

static void ata_delay(void) { for (int i = 0; i < 4; i++) inb(ATA_CTRL); }

/* wait until the drive is no longer busy; optionally until it wants data */
static bool ata_wait(bool drq)
{
    ata_delay();
    for (int i = 0; i < 2000000; i++) {
        u8 s = inb(ATA_CMD);
        if (s & 0x80) continue;                 /* BSY */
        if (s & 0x21) return false;             /* ERR or DF */
        if (!drq || (s & 0x08)) return true;
    }
    return false;
}

static void ata_select(u32 lba, u32 count)
{
    outb(ATA_DRIVE, (u8)(0xE0 | ((lba >> 24) & 0x0F)));
    outb(ATA_COUNT, (u8)count);
    outb(ATA_LBA0, (u8)lba);
    outb(ATA_LBA1, (u8)(lba >> 8));
    outb(ATA_LBA2, (u8)(lba >> 16));
}

bool ata_init(void)
{
    u16 id[256];
    outb(ATA_CTRL, 0x02);                       /* polling only: no drive interrupts */
    outb(ATA_DRIVE, 0xA0);
    ata_delay();
    outb(ATA_COUNT, 0); outb(ATA_LBA0, 0); outb(ATA_LBA1, 0); outb(ATA_LBA2, 0);
    outb(ATA_CMD, 0xEC);                        /* IDENTIFY */
    if (inb(ATA_CMD) == 0 || !ata_wait(true)) {
        kprintf("ata: no disk\n");
        return false;
    }
    insw(ATA_DATA, id, 256);
    ata_total = (u32)id[60] | ((u32)id[61] << 16);
    kprintf("ata: %u sectors (%u MiB)\n", ata_total, ata_total >> 11);
    return ata_total != 0;
}

int disk_read(u32 lba, u32 count, void *buf)
{
    u16 *p = buf;
    while (count) {
        u32 n = count > 128 ? 128 : count;
        if (!ata_wait(false)) return -1;
        ata_select(lba, n);
        outb(ATA_CMD, 0x20);                    /* READ SECTORS */
        for (u32 i = 0; i < n; i++) {
            if (!ata_wait(true)) return -1;
            insw(ATA_DATA, p, 256);
            p += 256;
        }
        lba += n;
        count -= n;
    }
    return 0;
}

int disk_write(u32 lba, u32 count, const void *buf)
{
    const u16 *p = buf;
    while (count) {
        u32 n = count > 128 ? 128 : count;
        if (!ata_wait(false)) return -1;
        ata_select(lba, n);
        outb(ATA_CMD, 0x30);                    /* WRITE SECTORS */
        for (u32 i = 0; i < n; i++) {
            if (!ata_wait(true)) return -1;
            outsw(ATA_DATA, p, 256);
            p += 256;
        }
        lba += n;
        count -= n;
    }
    if (!ata_wait(false)) return -1;
    outb(ATA_CMD, 0xE7);                        /* FLUSH CACHE: data is on the platter */
    return ata_wait(false) ? 0 : -1;
}

u32 disk_sectors(void) { return ata_total; }

/* ---- CMOS real-time clock ------------------------------------------------ */
static u8 cmos(u8 reg)
{
    outb(0x70, reg);
    return inb(0x71);
}

static u8 bcd(u8 v) { return (u8)((v & 0x0F) + (v >> 4) * 10); }

void rtc_read(DateTime *dt)
{
    for (int i = 0; i < 100000 && (cmos(0x0A) & 0x80); i++) { }        /* update in progress */
    u8 s = cmos(0x00), m = cmos(0x02), h = cmos(0x04);
    u8 d = cmos(0x07), mo = cmos(0x08), y = cmos(0x09), b = cmos(0x0B);
    bool pm = (h & 0x80) != 0;
    h &= 0x7F;
    if (!(b & 0x04)) { s = bcd(s); m = bcd(m); h = bcd(h); d = bcd(d); mo = bcd(mo); y = bcd(y); }
    if (!(b & 0x02)) { h = (u8)(h % 12); if (pm) h = (u8)(h + 12); }    /* 12-hour clock */
    dt->sec = s; dt->min = m; dt->hour = h;
    dt->day = d; dt->month = mo; dt->year = (u16)(2000 + y);
}

void plat_now(DateTime *dt) { rtc_read(dt); }

/* ---- CPU identification and power ---------------------------------------- */
const char *plat_cpu(void)
{
    static char name[52];
    if (!name[0]) {
        u32 a, b, c, d;
        __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0x80000000u));
        if (a >= 0x80000004u) {
            u32 *out = (u32 *)name;
            for (u32 leaf = 0x80000002u; leaf <= 0x80000004u; leaf++) {
                __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(leaf));
                *out++ = a; *out++ = b; *out++ = c; *out++ = d;
            }
            name[48] = 0;
        } else {
            u32 *out = (u32 *)name;
            __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0));
            out[0] = b; out[1] = d; out[2] = c;
            name[12] = 0;
        }
        const char *s = name;                   /* trim leading spaces */
        while (*s == ' ') s++;
        if (s != name) memmove(name, s, str_len(s) + 1);
    }
    return name;
}

void plat_reboot(void)
{
    cpu_cli();
    for (int i = 0; i < 100000 && (inb(0x64) & 2); i++) { }
    outb(0x64, 0xFE);                           /* pulse the CPU reset line */
    for (;;) cpu_hlt();
}

void plat_poweroff(void)
{
    cpu_cli();
    outw(0x604, 0x2000);                        /* QEMU / ACPI PM1a: S5 */
    outw(0xB004, 0x2000);                       /* Bochs, old QEMU */
    for (;;) cpu_hlt();
}
