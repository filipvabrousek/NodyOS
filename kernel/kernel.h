/* NodyOS - kernel internals. */
#ifndef NODY_KERNEL_H
#define NODY_KERNEL_H

#include "lib/types.h"
#include "lib/lib.h"
#include "lib/plat.h"

/* ---- port I/O ------------------------------------------------------------ */
static inline void outb(u16 port, u8 v)  { __asm__ volatile("outb %0, %1" : : "a"(v), "Nd"(port)); }
static inline void outw(u16 port, u16 v) { __asm__ volatile("outw %0, %1" : : "a"(v), "Nd"(port)); }
static inline u8   inb(u16 port)  { u8 r;  __asm__ volatile("inb %1, %0" : "=a"(r) : "Nd"(port)); return r; }
static inline u16  inw(u16 port)  { u16 r; __asm__ volatile("inw %1, %0" : "=a"(r) : "Nd"(port)); return r; }
static inline void insw(u16 port, void *buf, u32 words)
{
    __asm__ volatile("rep insw" : "+D"(buf), "+c"(words) : "d"(port) : "memory");
}
static inline void outsw(u16 port, const void *buf, u32 words)
{
    __asm__ volatile("rep outsw" : "+S"(buf), "+c"(words) : "d"(port) : "memory");
}
static inline void io_wait(void) { outb(0x80, 0); }
static inline void cpu_cli(void) { __asm__ volatile("cli"); }
static inline void cpu_sti(void) { __asm__ volatile("sti"); }
static inline void cpu_hlt(void) { __asm__ volatile("hlt"); }

/* ---- boot information handed over by the boot loader --------------------- */
typedef struct BootInfo {
    u32 magic;                  /* 'NODY' */
    u32 fb_addr, fb_width, fb_height, fb_pitch, fb_bpp;
    u32 mmap_addr, mmap_count;
    u32 boot_drive, kernel_sectors;
} BootInfo;

typedef struct E820 { u32 base_lo, base_hi, len_lo, len_hi, type, acpi; } E820;

/* register frame built by isr_common (entry.asm) */
typedef struct Regs {
    u32 edi, esi, ebp, esp, ebx, edx, ecx, eax;
    u32 int_no, err;
    u32 eip, cs, eflags;
} Regs;

/* cpu.c */
void cpu_init(void);
void irq_install(int irq, void (*fn)(void));
void panic(const char *msg);
extern volatile u32 g_ticks;

/* mem.c */
void mem_init(const BootInfo *bi);

/* drivers.c */
void serial_init(void);
void serial_putc(char c);
bool ata_init(void);
void rtc_read(DateTime *dt);

/* ps2.c */
void ps2_init(void);
bool ps2_absolute(void);

/* kernel.c */
void panic_screen(const char *text);

#endif
